// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2015-2019 ARM Limited.
// Original author: Dave Martin <Dave.Martin@arm.com>
//
// Simple FPSIMD context switch test
// Repeatedly writes unique test patterns into each FPSIMD register
// and reads them back to verify integrity.
//
// for x in `seq 1 NR_CPUS`; do fpsimd-test & pids=$pids\ $! ; done
// (leave it running for as long as you want...)
// kill $pids

#include "assembler.h"
#include "asm-offsets.h"

#define NVR	32
#define MAXVL_B	(128 / 8)

.macro _vldr Vn:req, Xt:req
	ld1	{v\Vn\().2d}, [x\Xt]
.endm

.macro _vstr Vn:req, Xt:req
	st1	{v\Vn\().2d}, [x\Xt]
.endm

// Generate accessor functions to read/write programmatically selected
// FPSIMD registers.
// x0 is the register index to access
// x1 is the memory address to read from (getv,setp) or store to (setv,setp)
// All clobber x0-x2
define_accessor setv, NVR, _vldr
define_accessor getv, NVR, _vstr

// Declare some storate space to shadow the SVE register contents:
.pushsection .text
.data
.align 4
vref:
	.space	MAXVL_B * NVR
scratch:
	.space	MAXVL_B
.popsection

// Generate a test pattern for storage in SVE registers
// x0: pid	(16 bits)
// x1: register number (6 bits)
// x2: generation (4 bits)
function pattern
	orr	w1, w0, w1, lsl #16
	orr	w2, w1, w2, lsl #28

	ldr	x0, =scratch
	mov	w1, #MAXVL_B / 4

0:	str	w2, [x0], #4
	add	w2, w2, #(1 << 22)
	subs	w1, w1, #1
	bne	0b

	ret
endfunction

// Get the address of shadow data for FPSIMD V-register V<xn>
.macro _adrv xd, xn, nrtmp
	ldr	\xd, =vref
	mov	x\nrtmp, #16
	madd	\xd, x\nrtmp, \xn, \xd
.endm

// Set up test pattern in a FPSIMD V-register
// x0: pid
// x1: register number
// x2: generation
function setup_vreg
	mov	x4, x30

	mov	x6, x1
	bl	pattern
	_adrv	x0, x6, 2
	mov	x5, x0
	ldr	x1, =scratch
	bl	memcpy

	mov	x0, x6
	mov	x1, x5
	bl	setv

	ret	x4
endfunction

// Trivial memory compare: compare x2 bytes starting at address x0 with
// bytes starting at address x1.
// Returns only if all bytes match; otherwise, the program is aborted.
// Clobbers x0-x5.
function memcmp
	cbz	x2, 1f

	mov	x5, #0
0:	ldrb	w3, [x0, x5]
	ldrb	w4, [x1, x5]
	add	x5, x5, #1
	cmp	w3, w4
	b.ne	barf
	subs	x2, x2, #1
	b.ne	0b

1:	ret
endfunction

// Verify that a FPSIMD V-register matches its shadow in memory, else abort
// x0: reg number
// Clobbers x0-x5.
function check_vreg
	mov	x3, x30

	_adrv	x5, x0, 6
	mov	x4, x0
	ldr	x7, =scratch

	mov	x0, x7
	mov	x1, x6
	bl	memfill_ae

	mov	x0, x4
	mov	x1, x7
	bl	getv

	mov	x0, x5
	mov	x1, x7
	mov	x2, x6
	mov	x30, x3
	b	memcmp
endfunction

// Modify live register state, the signal return will undo our changes
function irritator_handler
	// Increment the irritation signal count (x23):
	ldr	x0, [x2, #ucontext_regs + 8 * 23]
	add	x0, x0, #1
	str	x0, [x2, #ucontext_regs + 8 * 23]

	// Corrupt some random V-regs
	movi	v0.8b, #7
	movi	v9.16b, #9
	movi	v31.8b, #31

	ret
endfunction
.globl	irritator_handler

// Program specific main function
.globl main
function main
	// Sanity-check and report the vector length

	mov	x19, #128
	cmp	x19, #128
	b.lo	1f
	cmp	x19, #2048
	b.hi	1f
	tst	x19, #(8 - 1)
	b.eq	2f

1:	puts	"Bad vector length: "
	mov	x0, x19
	bl	putdecn
	b	exit_error

2:	puts	"Vector length:\t"
	mov	x0, x19
	bl	putdec
	puts	" bits\n"

	mov	x22, #0		// generation number, increments per iteration
.Ltest_loop:

	mov	x21, #0		// Set up V-regs & shadow with test pattern
0:	mov	x0, x20
	mov	x1, x21
	and	x2, x22, #0xf
	bl	setup_vreg
	add	x21, x21, #1
	cmp	x21, #NVR
	b.lo	0b

	mov	x21, #0
0:	mov	x0, x21
	bl	check_vreg
	add	x21, x21, #1
	cmp	x21, #NVR
	b.lo	0b

	add	x22, x22, #1
	b	.Ltest_loop
endfunction

function barf
	mov	x10, x0	// expected data
	mov	x11, x1	// actual data
	mov	x12, x2	// data size

	puts	"Mismatch: PID="
	mov	x0, x20
	bl	putdec
	puts	", iteration="
	mov	x0, x22
	bl	putdec
	puts	", reg="
	mov	x0, x21
	bl	putdecn
	puts	"\tExpected ["
	mov	x0, x10
	mov	x1, x12
	bl	dumphex
	puts	"]\n\tGot      ["
	mov	x0, x11
	mov	x1, x12
	bl	dumphex
	puts	"]\n"

	b	exit_error
endfunction
