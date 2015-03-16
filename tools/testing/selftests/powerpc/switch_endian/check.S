#include "common.h"

/*
 * Checks that registers contain what we expect, ie. they were not clobbered by
 * the syscall. If that all works we then print a message.
 *
 * r14: address of message to print (size is hardcoded at 25 bytes)
 * r15: pattern to check registers against.
 *
 * At the end r3 == 0 if everything's OK.
 */
	nop	# guaranteed to be illegal in reverse-endian
	cmpd    r15,r3
	bne     1f
	addi    r3,r15,4
	cmpd    r3,r4
	bne     1f
	lis     r3,0x00FF
	ori     r3,r3,0xF000
	mfcr    r4
	and     r4,r4,r3
	cmpw    r3,r4
	addi    r3,r15,34
	bne     1f
	addi    r3,r15,32
	mflr    r4
	cmpd    r3,r4
	bne     1f
	addi    r3,r15,5
	cmpd    r3,r5
	bne     1f
	addi    r3,r15,6
	cmpd    r3,r6
	bne     1f
	addi    r3,r15,7
	cmpd    r3,r7
	bne     1f
	addi    r3,r15,8
	cmpd    r3,r8
	bne     1f
	addi    r3,r15,13
	cmpd    r3,r13
	bne     1f
	addi    r3,r15,16
	cmpd    r3,r16
	bne     1f
	addi    r3,r15,17
	cmpd    r3,r17
	bne     1f
	addi    r3,r15,18
	cmpd    r3,r18
	bne     1f
	addi    r3,r15,19
	cmpd    r3,r19
	bne     1f
	addi    r3,r15,20
	cmpd    r3,r20
	bne     1f
	addi    r3,r15,21
	cmpd    r3,r21
	bne     1f
	addi    r3,r15,22
	cmpd    r3,r22
	bne     1f
	addi    r3,r15,23
	cmpd    r3,r23
	bne     1f
	addi    r3,r15,24
	cmpd    r3,r24
	bne     1f
	addi    r3,r15,25
	cmpd    r3,r25
	bne     1f
	addi    r3,r15,26
	cmpd    r3,r26
	bne     1f
	addi    r3,r15,27
	cmpd    r3,r27
	bne     1f
	addi    r3,r15,28
	cmpd    r3,r28
	bne     1f
	addi    r3,r15,29
	cmpd    r3,r29
	bne     1f
	addi    r3,r15,30
	cmpd    r3,r30
	bne     1f
	addi    r3,r15,31
	cmpd    r3,r31
	bne     1f
	li      r0,4
	li      r3,1	/* stdout */
	mr      r4,r14
	li      r5,25	/* strlen(message) */
	sc
	b	2f
1:	li	r0, __NR_exit
	sc
2:	li	r0, __NR_switch_endian
	nop
