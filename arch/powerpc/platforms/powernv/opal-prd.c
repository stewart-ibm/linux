/*
 * OPAL Runtime Diagnostics interface driver
 * Supported on POWERNV platform
 *
 * (C) Copyright IBM 2015
 *
 * Author: Vishal Kulkarni <kvishal at in.ibm.com>
 * Author: Vaidyanathan Srinivasan <svaidy at linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "opal-prd: " fmt
#define DEBUG

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/opal-prd.h>
#include <asm/opal.h>
#include <asm/io.h>
#include <asm/uaccess.h>

static struct opal_prd_range ranges[OPAL_PRD_MAX_RANGES];

struct opal_prd_msg_queue_item {
	struct opal_prd_msg	msg;
	struct list_head	list;
};

static LIST_HEAD(opal_prd_msg_queue);
static DEFINE_SPINLOCK(opal_prd_msg_queue_lock);
static DECLARE_WAIT_QUEUE_HEAD(opal_prd_msg_wait);
static atomic_t usage;

static struct opal_prd_range *find_range_by_addr(uint64_t addr)
{
	struct opal_prd_range *range;
	unsigned int i;

	for (i = 0; i < OPAL_PRD_MAX_RANGES; i++) {
		range = &ranges[i];
		if (addr >= range->physaddr &&
				addr < range->physaddr + range->size)
			return range;
	}

	return NULL;
}

static int opal_prd_open(struct inode *inode, struct file *file)
{
	if (atomic_xchg(&usage, 1) == 1)
		return -EBUSY;

	return 0;
}

/*
 * opal_prd_mmap - maps the hbrt binary into userspace
 * @file: file structure for the device
 * @vma: VMA to map the registers into
 */

static int opal_prd_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct opal_prd_range *range;
	size_t addr, size;
	int rc;

	pr_debug("opal_prd_mmap(0x%016lx, 0x%016lx, 0x%lx, 0x%lx)\n",
			vma->vm_start, vma->vm_end, vma->vm_pgoff,
			vma->vm_flags);

	addr = vma->vm_pgoff << PAGE_SHIFT;
	size = vma->vm_end - vma->vm_start;

	/* ensure we're mapping within one of the allowable ranges */
	range = find_range_by_addr(addr);
	if (!range)
		return -EINVAL;

	if (addr + size > range->physaddr + range->size)
		return -EINVAL;

	vma->vm_page_prot = phys_mem_access_prot(file, vma->vm_pgoff,
						 size, vma->vm_page_prot)
				| _PAGE_SPECIAL;

	rc = remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, size,
			vma->vm_page_prot);

	return rc;
}

static bool opal_msg_queue_empty(void)
{
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(&opal_prd_msg_queue_lock, flags);
	ret = list_empty(&opal_prd_msg_queue);
	spin_unlock_irqrestore(&opal_prd_msg_queue_lock, flags);

	return ret;
}

static unsigned int opal_prd_poll(struct file *file,
		struct poll_table_struct *wait)
{
	poll_wait(file, &opal_prd_msg_wait, wait);

	if (!opal_msg_queue_empty())
		return POLLIN | POLLRDNORM;

	return 0;
}

static ssize_t opal_prd_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	struct opal_prd_msg_queue_item *item;
	unsigned long flags;
	ssize_t size;
	int rc;

	size = sizeof(item->msg);

	if (count < size)
		return -EINVAL;

	if (*ppos)
		return -ESPIPE;

	item = NULL;

	for (;;) {

		spin_lock_irqsave(&opal_prd_msg_queue_lock, flags);
		if (!list_empty(&opal_prd_msg_queue)) {
			item = list_first_entry(&opal_prd_msg_queue,
					struct opal_prd_msg_queue_item, list);
			list_del(&item->list);
		}
		spin_unlock_irqrestore(&opal_prd_msg_queue_lock, flags);

		if (item)
			break;

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		rc = wait_event_interruptible(opal_prd_msg_wait,
				!opal_msg_queue_empty());
		if (rc)
			return -EINTR;
	}

	rc = copy_to_user(buf, &item->msg, size);
	if (rc) {
		/* eep! re-queue at the head of the list */
		spin_lock_irqsave(&opal_prd_msg_queue_lock, flags);
		list_add(&item->list, &opal_prd_msg_queue);
		spin_unlock_irqrestore(&opal_prd_msg_queue_lock, flags);
		return -EFAULT;
	}

	kfree(item);

	return size;
}

static ssize_t opal_prd_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	struct opal_prd_msg msg;
	ssize_t size;
	int rc;

	size = sizeof(msg);

	if (count < size)
		return -EINVAL;

	rc = copy_from_user(&msg, buf, sizeof(msg));
	if (rc)
		return -EFAULT;

	rc = opal_prd_msg(&msg);
	if (rc) {
		pr_warn("write: opal_prd_msg returned %d\n", rc);
		return -EIO;
	}

	return size;
}

static int opal_prd_release(struct inode *inode, struct file *file)
{
	struct opal_prd_msg msg;

	msg.type = OPAL_PRD_MSG_TYPE_FINI;
	msg.token = 0;

	opal_prd_msg(&msg);
	atomic_xchg(&usage, 0);

	return 0;
}


static long opal_prd_ioctl(struct file *file, unsigned int cmd,
		unsigned long param)
{
	struct opal_prd_info info;
	struct opal_prd_scom scom;
	int rc = 0;

	switch(cmd) {
	case OPAL_PRD_GET_INFO:
		info.version = OPAL_PRD_VERSION;
		memcpy(&info.ranges, ranges, sizeof(info.ranges));
		rc = copy_to_user((void __user *)param, &info, sizeof(info));
		if (rc)
			return -EFAULT;
		break;

	case OPAL_PRD_SCOM_READ:
		rc = copy_from_user(&scom, (void __user *)param, sizeof(scom));
		if (rc)
			return -EFAULT;

		rc = opal_xscom_read(scom.chip, scom.addr,
				(__be64 *)&scom.data);
		scom.data = be64_to_cpu(scom.data);
		pr_debug("ioctl SCOM_READ: chip %llx addr %016llx "
				"data %016llx rc %d\n",
				scom.chip, scom.addr, scom.data, rc);
		if (rc)
			return -EIO;

		rc = copy_to_user((void __user *)param, &scom, sizeof(scom));
		if (rc)
			return -EFAULT;
		break;

	case OPAL_PRD_SCOM_WRITE:
		rc = copy_from_user(&scom, (void __user *)param, sizeof(scom));
		if (rc)
			return -EFAULT;

		rc = opal_xscom_write(scom.chip, scom.addr, scom.data);
		pr_debug("ioctl SCOM_WRITE: chip %llx addr %016llx "
				"data %016llx rc %d\n",
				scom.chip, scom.addr, scom.data, rc);
		if (rc)
			return -EIO;

		break;

	default:
		rc = -EINVAL;
	}

	return rc;
}

struct file_operations opal_prd_fops = {
	.open		= opal_prd_open,
	.mmap		= opal_prd_mmap,
	.poll		= opal_prd_poll,
	.read		= opal_prd_read,
	.write		= opal_prd_write,
	.unlocked_ioctl	= opal_prd_ioctl,
	.release	= opal_prd_release,
	.owner		= THIS_MODULE,
};

static struct miscdevice opal_prd_dev = {
        .minor		= MISC_DYNAMIC_MINOR,
        .name		= "opal-prd",
        .fops		= &opal_prd_fops,
};

/* opal interface */
static int opal_prd_msg_notifier(struct notifier_block *nb,
		unsigned long msg_type, void *_msg)
{
	struct opal_prd_msg_queue_item *item;
	struct opal_msg *msg = _msg;
	unsigned long flags;

	if (msg_type != OPAL_MSG_PRD)
		return 0;

	item = kzalloc(sizeof(*item), GFP_ATOMIC);
	if (!item)
		return -ENOMEM;

	memcpy(&item->msg, msg->params, sizeof(item->msg));

	spin_lock_irqsave(&opal_prd_msg_queue_lock, flags);
	list_add_tail(&item->list, &opal_prd_msg_queue);
	spin_unlock_irqrestore(&opal_prd_msg_queue_lock, flags);

	wake_up_interruptible(&opal_prd_msg_wait);

	return 0;
}

static struct notifier_block opal_prd_event_nb = {
	.notifier_call	= opal_prd_msg_notifier,
	.next		= NULL,
	.priority	= 0,
};

static bool is_prd_range(const char *name)
{
	return true;
}

#ifdef DEBUG
static void create_test_range(int idx)
{
	struct opal_prd_range *range;
	struct page *page;

	if (idx >= OPAL_PRD_MAX_RANGES) {
		pr_debug("Not adding debug page: no ranges left\n");
		return;
	}

	range = &ranges[idx];

	page = alloc_page(GFP_KERNEL);
	if (!page) {
		pr_debug("Not adding debug page: page allocation failed\n");
		return;
	}

	strcpy(range->name, "test");
	range->physaddr = page_to_phys(page);
	range->size = PAGE_SIZE;
	memcpy(phys_to_virt(range->physaddr), "test", 5);

}
#else
static void create_test_range(int idx) { }
#endif


/**
 * Find the HBRT code region in reserved-ranges and set code_region_physaddr
 * and code_region_size accordingly.
 */
static int parse_regions(void)
{
	const __be32 *ranges_prop;
	int i, n, rc, nr_ranges;
	struct device_node *np;
	const char *name;

	np = of_find_node_by_path("/");
	if (!np)
		return -ENODEV;

	nr_ranges = of_property_count_strings(np, "reserved-names");
	ranges_prop = of_get_property(np, "reserved-ranges", NULL);
	if (!ranges_prop) {
		of_node_put(np);
		return -ENODEV;
	}

	for (i = 0, n = 0; i < nr_ranges; i++) {
		uint64_t addr, size;

		rc = of_property_read_string_index(np, "reserved-names", i,
				&name);
		if (rc)
			continue;

		if (strlen(name) >= OPAL_PRD_RANGE_NAME_LEN)
			continue;

		if (!is_prd_range(name))
			continue;

		addr = of_read_number(ranges_prop + (i * 4) + 0, 2);
		size = PAGE_ALIGN(of_read_number(ranges_prop + (i * 4) + 2, 2));

		if (addr & (PAGE_SIZE - 1)) {
			pr_warn("skipping range %s: not page-aligned\n",
					name);
			continue;
		}

		if (n == OPAL_PRD_MAX_RANGES) {
			pr_warn("Too many PRD ranges! Skipping %s\n", name);
		} else {
			strncpy(ranges[n].name, name,
					OPAL_PRD_RANGE_NAME_LEN - 1);
			ranges[n].physaddr = addr;
			ranges[n].size = size;
			n++;
		}
	}

	of_node_put(np);

	create_test_range(n);

	return 0;
}

static int __init opal_prd_init(void)
{
	int rc;

	/* parse the code region information from the device tree */
	rc = parse_regions();
	if (rc) {
		pr_err("Couldn't parse region information from DT\n");
		return rc;
	}

	rc = opal_message_notifier_register(OPAL_MSG_PRD, &opal_prd_event_nb);
	if (rc) {
		pr_err("Couldn't register event notifier\n");
		return rc;
	}

	rc = misc_register(&opal_prd_dev);
	if (rc) {
		pr_err("failed to register miscdev\n");
		return rc;
	}

	return 0;
}

static void __exit opal_prd_exit(void)
{
	misc_deregister(&opal_prd_dev);
	opal_message_notifier_unregister(OPAL_MSG_PRD, &opal_prd_event_nb);
}

module_init(opal_prd_init);
module_exit(opal_prd_exit);

MODULE_DESCRIPTION("PowerNV OPAL runtime diagnostic driver");
MODULE_LICENSE("GPL");

