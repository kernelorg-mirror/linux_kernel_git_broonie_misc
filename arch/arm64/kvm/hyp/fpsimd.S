/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2015 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 */

#include <linux/linkage.h>

#include <asm/fpsimdmacros.h>

	.text

SYM_FUNC_START(__fpsimd_save_state)
	fpsimd_save	x0, 1
	ret
SYM_FUNC_END(__fpsimd_save_state)

SYM_FUNC_START(__fpsimd_restore_state)
	fpsimd_restore	x0, 1
	ret
SYM_FUNC_END(__fpsimd_restore_state)

SYM_FUNC_START(__sve_restore_state)
	sve_load 0, x1, x2, 3
	ret
SYM_FUNC_END(__sve_restore_state)

SYM_FUNC_START(__sve_save_state)
	sve_save 0, x1, x2, 3
	ret
SYM_FUNC_END(__sve_save_state)

SYM_FUNC_START(__sve_get_vl)
	_sve_rdvl	0, 1
	ret
SYM_FUNC_END(__sve_get_vl)

SYM_FUNC_START(__sme_save_state)
	_sme_rdsvl	2, 1		// x2 = VL/8
	sme_save_za 0, x2, 12		// Leaves x0 pointing to the end of ZA

	cbz	x1, 1f
	_str_zt 0
1:
	ret
SYM_FUNC_END(__sme_save_state)

SYM_FUNC_START(__sme_restore_state)
	_sme_rdsvl	2, 1		// x2 = VL/8
	sme_load_za	0, x2, 12	// Leaves x0 pointing to end of ZA

	cbz	x1, 1f
	_ldr_zt 0

1:
	ret
SYM_FUNC_END(__sme_restore_state)
