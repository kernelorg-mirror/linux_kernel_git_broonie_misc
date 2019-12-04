/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2017 Linaro, Ltd. <ard.biesheuvel@linaro.org>
 */

#include <linux/linkage.h>

SYM_CODE_START(absolute_data64)
	ldr	x0, 0f
	ret
0:	.quad	sym64_abs
SYM_CODE_END(absolute_data64)

SYM_CODE_START(absolute_data32)
	ldr	w0, 0f
	ret
0:	.long	sym32_abs
SYM_CODE_END(absolute_data32)

SYM_CODE_START(absolute_data16)
	adr	x0, 0f
	ldrh	w0, [x0]
	ret
0:	.short	sym16_abs, 0
SYM_CODE_END(absolute_data16)

SYM_CODE_START(signed_movw)
	movz	x0, #:abs_g2_s:sym64_abs
	movk	x0, #:abs_g1_nc:sym64_abs
	movk	x0, #:abs_g0_nc:sym64_abs
	ret
SYM_CODE_END(signed_movw)

SYM_CODE_START(unsigned_movw)
	movz	x0, #:abs_g3:sym64_abs
	movk	x0, #:abs_g2_nc:sym64_abs
	movk	x0, #:abs_g1_nc:sym64_abs
	movk	x0, #:abs_g0_nc:sym64_abs
	ret
SYM_CODE_END(unsigned_movw)

	.align	12
	.space	0xff8
SYM_CODE_START(relative_adrp)
	adrp	x0, sym64_rel
	add	x0, x0, #:lo12:sym64_rel
	ret
SYM_CODE_END(relative_adrp)

	.align	12
	.space	0xffc
SYM_CODE_START(relative_adrp_far)
	adrp	x0, memstart_addr
	add	x0, x0, #:lo12:memstart_addr
	ret
SYM_CODE_END(relative_adrp_far)

SYM_CODE_START(relative_adr)
	adr	x0, sym64_rel
	ret
SYM_CODE_END(relative_adr)

SYM_CODE_START(relative_data64)
	adr	x1, 0f
	ldr	x0, [x1]
	add	x0, x0, x1
	ret
0:	.quad	sym64_rel - .
SYM_CODE_END(relative_data64)

SYM_CODE_START(relative_data32)
	adr	x1, 0f
	ldr	w0, [x1]
	add	x0, x0, x1
	ret
0:	.long	sym64_rel - .
SYM_CODE_END(relative_data32)

SYM_CODE_START(relative_data16)
	adr	x1, 0f
	ldrsh	w0, [x1]
	add	x0, x0, x1
	ret
0:	.short	sym64_rel - ., 0
SYM_CODE_END(relative_data16)
