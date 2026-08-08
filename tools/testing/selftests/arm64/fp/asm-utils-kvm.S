// SPDX-License-Identifier: GPL-2.0-only
//
// Utility functions for assembly code programs run under fp-stress-vmm.
//
// Copyright 2026, Arm Ltd

#include <asm/sysreg.h>

#include "assembler.h"
#include "asm-offsets.h"
#include "fp-stress-vmm.h"

#define PSCI_0_2_FN_SYSTEM_OFF		0x84000008

// Print a single character x0 to the console
// Clobbers x1
function putc
	movz	x1, #(CONSOLE_BASE >> 16), lsl #16
	strb	w0, [x1]
	ret
endfunction
.globl	putc

// Print a \0 terminated string starting at address x0 to the console.
// Doesn't use putc as a slight optimisation.
// Clobbers x0-x3
function puts
	mov	x2, x0
	movz	x3, #(CONSOLE_BASE >> 16), lsl #16

0:	ldrb	w0, [x2], #1
	cbz	w0, 1f
	strb	w0, [x3]
	b	0b

1:	ret
endfunction
.globl	puts

// Report errors to the host by shutting down, there is no other reason one
// of these test programs should exit.
function exit_error
	movz	x0, #(PSCI_0_2_FN_SYSTEM_OFF >> 16), lsl #16
	movk	x0, #(PSCI_0_2_FN_SYSTEM_OFF & 0xffff)
	hvc	#0

0:	b	0b		// PSCI should not have returned
endfunction
.globl	exit_error

// Exception vectors.  We have no handlers for anything so each entry
// just BRKs with its own number.
	.balign	2048
vectors:
	.irp	vector, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
	.balign	0x80
	brk	#\vector
	.endr

// Main program entry point.
//
// The VMM starts us at the very start of the image so the link script
// places .text.boot first.
	.section .text.boot, "ax"
.globl _start
function _start
	// Vectors that report any exception we take to the VMM.
	adr	x0, vectors
	msr	VBAR_EL1, x0

	// The VMM sizes our RAM as the image plus GUEST_EXTRA_RAM, put the
	// stack at the top of that so it grows down away from the image.
	ldr	x0, =_end
	ldr	x1, =GUEST_EXTRA_RAM
	add	x0, x0, x1
	and	x0, x0, #~15
	mov	sp, x0

	// Log the current EL for diagnostic purposes.
	puts	"EL:\t\tEL"
	mrs	x0, CurrentEL
	ubfx	x0, x0, #2, #2
	bl	putdecn

	b	main
endfunction
