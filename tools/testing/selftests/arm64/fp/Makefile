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

EXTRA_CLEAN += $(OUTPUT)/asm-utils.o $(OUTPUT)/rdvl.o

$(OUTPUT)/fp-pidbench: fp-pidbench.S asm-utils.o
	$(CC) -nostdlib $^ -o $@
$(OUTPUT)/fpsimd-test: fpsimd-test.S asm-utils.o
	$(CC) -nostdlib $^ -o $@
$(OUTPUT)/rdvl-sve: rdvl-sve.c rdvl.o
$(OUTPUT)/sve-ptrace: sve-ptrace.c
$(OUTPUT)/sve-probe-vls: sve-probe-vls.c rdvl.o
$(OUTPUT)/sve-test: sve-test.S asm-utils.o
	$(CC) -nostdlib $^ -o $@
$(OUTPUT)/vec-syscfg: vec-syscfg.c rdvl.o
$(OUTPUT)/vlset: vlset.c

include ../../lib.mk
