// SPDX-License-Identifier: GPL-2.0-only

#ifndef FP_STRESS_H
#define FP_STRESS_H

#define GUEST_RAM_BASE  0x80000000

/* For stack */
#define GUEST_EXTRA_RAM (1024 * 1024)

/*
 * Console: byte writes to CONSOLE_BASE are emitted on stdout.  Not
 * backed by a memslot, so guest accesses trap with KVM_EXIT_MMIO.
 */
#define CONSOLE_BASE    0x09000000

#endif
