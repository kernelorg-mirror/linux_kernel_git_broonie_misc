// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2015-2021 ARM Limited.
// Original author: Dave Martin <Dave.Martin@arm.com>
//
// Utility functions for assembly code which use Linux syscalls.

#include <asm/unistd.h>
#include "assembler.h"

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
