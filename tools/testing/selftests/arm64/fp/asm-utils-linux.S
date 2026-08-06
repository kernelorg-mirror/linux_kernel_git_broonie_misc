// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2015-2021 ARM Limited.
// Original author: Dave Martin <Dave.Martin@arm.com>
//
// Utility functions for assembly code which use Linux syscalls.

#include <asm/unistd.h>
#include "assembler.h"
#include "asm-offsets.h"

// Print a single character x0 to stdout
// Clobbers x0-x2,x8
function putc
	str	x0, [sp, #-16]!

	mov	x0, #1			// STDOUT_FILENO
	mov	x1, sp
	mov	x2, #1
	mov	x8, #__NR_write
	svc	#0

	add	sp, sp, #16
	ret
endfunction
.globl	putc

// Print a NUL-terminated string starting at address x0 to stdout
// Clobbers x0-x3,x8
function puts
	mov	x1, x0

	mov	x2, #0
0:	ldrb	w3, [x0], #1
	cbz	w3, 1f
	add	x2, x2, #1
	b	0b

1:	mov	w0, #1			// STDOUT_FILENO
	mov	x8, #__NR_write
	svc	#0

	ret
endfunction
.globl	puts

// Signal handler which reports a clean exit and terminates the program.
// The tests keep their iteration count in x22 and their signal count in
// x23, report those from the interrupted context.
//
// w0: signal number
// x2: ucontext
function terminate_handler
	mov	w21, w0
	mov	x20, x2

	puts	"Terminated by signal "
	mov	w0, w21
	bl	putdec
	puts	", no error, iterations="
	ldr	x0, [x20, #ucontext_regs + 8 * 22]
	bl	putdec
	puts	", signals="
	ldr	x0, [x20, #ucontext_regs + 8 * 23]
	bl	putdecn

	mov	x0, #0
	mov	x8, #__NR_exit
	svc	#0
endfunction
.globl	terminate_handler

// Signal handler which only records that the program was interrupted.
// The tests keep their signal count in x23, update that in the
// interrupted context.
//
// w0: signal number
// x2: ucontext
function tickle_handler
	// Increment the signal count (x23):
	ldr	x0, [x2, #ucontext_regs + 8 * 23]
	add	x0, x0, #1
	str	x0, [x2, #ucontext_regs + 8 * 23]

	ret
endfunction
.globl	tickle_handler
