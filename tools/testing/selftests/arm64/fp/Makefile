# SPDX-License-Identifier: GPL-2.0

# A proper top_srcdir is needed by KSFT(lib.mk)
top_srcdir = $(realpath ../../../../../)

CFLAGS += -I$(top_srcdir)/usr/include/

TEST_GEN_PROGS := sve-ptrace sve-probe-vls vec-syscfg
TEST_GEN_PROGS_EXTENDED := fp-pidbench fpsimd-test \
	rdvl-sve \
	sve-test \
	vlset
TEST_PROGS_EXTENDED := fpsimd-stress sve-stress

fp-pidbench: fp-pidbench.S asm-utils.o
	$(CC) -nostdlib $^ -o $@
fpsimd-test: fpsimd-test.o asm-utils.o
	$(CC) -nostdlib $^ -o $@
rdvl-sve: rdvl-sve.o rdvl.o
sve-ptrace: sve-ptrace.o
sve-probe-vls: sve-probe-vls.o rdvl.o
sve-test: sve-test.o asm-utils.o
	$(CC) -nostdlib $^ -o $@
vec-syscfg: vec-syscfg.o rdvl.o
vlset: vlset.o

include ../../lib.mk
