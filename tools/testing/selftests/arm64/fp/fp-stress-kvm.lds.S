/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Link script for the flat binary builds of the fp-stress loads which run
 * as guests under fp-stress-vmm.
 *
 * The VMM loads the image at the start of the guest RAM and starts the
 * guest there so the startup code has to be first in the image, it is in
 * .text.boot for that.
 */

#include "fp-stress-vmm.h"

OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
	. = GUEST_RAM_BASE;

	.text : {
		*(.text.boot)
		*(.text)
		*(.text.*)
	}

	.rodata : {
		*(.rodata)
		*(.rodata.*)
	}

	.data : {
		*(.data)
		*(.data.*)
	}

	.bss : {
		*(.bss)
		*(.bss.*)
		*(COMMON)
	}

	. = ALIGN(16);
	_end = .;

	/DISCARD/ : {
		*(.note*)
		*(.comment)
		*(.eh_frame*)
	}
}

/*
 * The VMM sizes the guest RAM from the size of the image file and the
 * guest puts its stack immediately above _end, anything allocated but not
 * present in the file would leave the two disagreeing about where the RAM
 * ends.
 */
ASSERT(SIZEOF(.bss) == 0, "fp-stress guests must not have a .bss")
