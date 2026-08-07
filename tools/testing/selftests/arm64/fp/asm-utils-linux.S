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

// Terminate the program, reporting failure.  Does not return.
function exit_error
	mov	x0, #1
	mov	x8, #__NR_exit
	svc	#0
endfunction
.globl	exit_error

// Install a signal handler
//
// w0: signal number
// x1: sa_action
// w2: sa_flags
// Clobbers x0-x6,x8
function setsignal
	str	x30, [sp, #-((sa_sz + 15) / 16 * 16 + 16)]!

	mov	w4, w0
	mov	x5, x1
	mov	w6, w2

	add	x0, sp, #16
	mov	x1, #sa_sz
	bl	memclr

	mov	w0, w4
	add	x1, sp, #16
	str	w6, [x1, #sa_flags]
	str	x5, [x1, #sa_handler]
	mov	x2, #0
	mov	x3, #sa_mask_sz
	mov	x8, #__NR_rt_sigaction
	svc	#0

	cbz	w0, 1f

	puts	"sigaction failure\n"
	b	exit_error

1:	ldr	x30, [sp], #((sa_sz + 15) / 16 * 16 + 16)
	ret
endfunction
.globl	setsignal

// Programs which want to be interrupted by SIGUSR1 provide an
// irritator_handler(), for those that don't the signal is left alone.
.weak	irritator_handler

// Main program entry point.  Install the signal handlers shared by the
// programs, zeroing the signal count in x23 that the handlers maintain,
// read and report the PID the programs use for uniqueness in x20, then
// run the program specific main().
.globl _start
function _start
	enable_gcs

	mov	x23, #0		// signal count

	mov	w0, #SIGINT
	adr	x1, terminate_handler
	mov	w2, #SA_SIGINFO
	bl	setsignal

	mov	w0, #SIGTERM
	adr	x1, terminate_handler
	mov	w2, #SA_SIGINFO
	bl	setsignal

	// Load as a literal, adr can't reference the symbol when the
	// program provides no handler and it resolves to 0
	ldr	x1, =irritator_handler
	cbz	x1, 1f
	mov	w0, #SIGUSR1
	mov	w2, #SA_SIGINFO
	orr	w2, w2, #SA_NODEFER
	bl	setsignal

1:	mov	w0, #SIGUSR2
	adr	x1, tickle_handler
	mov	w2, #SA_SIGINFO
	orr	w2, w2, #SA_NODEFER
	bl	setsignal

	// Obtain our PID, to ensure test pattern uniqueness between processes
	mov	x8, #__NR_getpid
	svc	#0
	mov	x20, x0

	puts	"PID:\t"
	mov	x0, x20
	bl	putdecn

	b	main
endfunction
