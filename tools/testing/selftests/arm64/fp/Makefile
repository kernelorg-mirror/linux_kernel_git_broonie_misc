# SPDX-License-Identifier: GPL-2.0

# A proper top_srcdir is needed by KSFT(lib.mk)
top_srcdir = $(realpath ../../../../../)

CFLAGS += $(KHDR_INCLUDES)

TEST_GEN_PROGS := \
	fp-ptrace \
	fp-stress \
	sve-ptrace sve-probe-vls \
	vec-syscfg \
	za-fork za-ptrace
TEST_GEN_PROGS_EXTENDED := fp-pidbench fp-stress-vmm fpsimd-test \
	kernel-test \
	rdvl-sme rdvl-sve \
	sve-test \
	ssve-test \
	za-test \
	zt-ptrace \
	zt-test \
	vlset
TEST_PROGS_EXTENDED := fpsimd-stress sve-stress ssve-stress za-stress
TEST_GEN_FILES := \
	fpsimd-test-kvm.bin \
	sve-test-kvm.bin \
	ssve-test-kvm.bin \
	za-test-kvm.bin \
	zt-test-kvm.bin

EXTRA_CLEAN += $(OUTPUT)/asm-utils.o $(OUTPUT)/asm-utils-linux.o \
	$(OUTPUT)/rdvl.o $(OUTPUT)/za-fork-asm.o \
	$(patsubst %.bin,%.elf,$(TEST_GEN_FILES)) \
	$(KVM_LDS)

ASM_UTILS_LINUX := $(OUTPUT)/asm-utils.o $(OUTPUT)/asm-utils-linux.o
ASM_UTILS_KVM := asm-utils.S asm-utils-kvm.S

OBJCOPY ?= $(CROSS_COMPILE)objcopy

# For sysreg definitons
ARCH_INCLUDES := $(KHDR_INCLUDES) -I$(top_srcdir)/tools/arch/arm64/include \
	-I$(top_srcdir)/tools/include \
	-I$(top_srcdir)/tools/include/generated

# The guests run with no OS under them at all, link them as flat binaries
# with the link script placing them where fp-stress-vmm puts their RAM.
KVM_LDS := $(OUTPUT)/fp-stress-kvm.lds
KVM_LDFLAGS := -nostdlib -static $(ARCH_INCLUDES) -D__ASSEMBLY__ \
	-Wl,-T,$(KVM_LDS)

# Supported/required from GNU ld 2.39, not confirmed LLD but after 22.1.2
COMMA=,
KVM_LDFLAGS += $(call try-run,$(LD) --warn-rwx-segments -v,-Wl$(COMMA)--warn-rwx-segments)

$(KVM_LDS): fp-stress-kvm.lds.S fp-stress-vmm.h
	$(CC) -E -P -x c $< -o $@

$(OUTPUT)/fp-stress-vmm: CFLAGS += $(ARCH_INCLUDES)

# Build with nolibc to avoid effects due to libc's clone() support
$(OUTPUT)/fp-pidbench: fp-pidbench.S $(ASM_UTILS_LINUX)
	$(CC) -nostdlib $^ -o $@
$(OUTPUT)/fp-ptrace: fp-ptrace.c fp-ptrace-asm.S
$(OUTPUT)/fpsimd-test: fpsimd-test.S $(ASM_UTILS_LINUX)
	$(CC) -nostdlib $^ -o $@
$(OUTPUT)/rdvl-sve: rdvl-sve.c $(OUTPUT)/rdvl.o
$(OUTPUT)/rdvl-sme: rdvl-sme.c $(OUTPUT)/rdvl.o
$(OUTPUT)/sve-ptrace: sve-ptrace.c
$(OUTPUT)/sve-probe-vls: sve-probe-vls.c $(OUTPUT)/rdvl.o
$(OUTPUT)/sve-test: sve-test.S $(ASM_UTILS_LINUX)
	$(CC) -nostdlib $^ -o $@
$(OUTPUT)/ssve-test: sve-test.S $(ASM_UTILS_LINUX)
	$(CC) -DSSVE -nostdlib $^ -o $@
$(OUTPUT)/vec-syscfg: vec-syscfg.c $(OUTPUT)/rdvl.o
$(OUTPUT)/vlset: vlset.c
$(OUTPUT)/za-fork: za-fork.c $(OUTPUT)/za-fork-asm.o
	$(CC) -fno-asynchronous-unwind-tables -fno-ident -s -Os -nostdlib \
		-include ../../../../include/nolibc/nolibc.h -I../..\
		-static -ffreestanding -Wall $^ -o $@
$(OUTPUT)/za-ptrace: za-ptrace.c
$(OUTPUT)/za-test: za-test.S $(ASM_UTILS_LINUX)
	$(CC) -nostdlib $^ -o $@
$(OUTPUT)/zt-ptrace: zt-ptrace.c
$(OUTPUT)/zt-test: zt-test.S $(ASM_UTILS_LINUX)
	$(CC) -nostdlib $^ -o $@

$(OUTPUT)/fpsimd-test-kvm.elf: fpsimd-test.S $(ASM_UTILS_KVM) $(KVM_LDS)
	$(CC) $(KVM_LDFLAGS) $(filter %.S,$^) -o $@
$(OUTPUT)/sve-test-kvm.elf: sve-test.S $(ASM_UTILS_KVM) $(KVM_LDS)
	$(CC) $(KVM_LDFLAGS) $(filter %.S,$^) -o $@
$(OUTPUT)/ssve-test-kvm.elf: sve-test.S $(ASM_UTILS_KVM) $(KVM_LDS)
	$(CC) -DSSVE $(KVM_LDFLAGS) $(filter %.S,$^) -o $@
$(OUTPUT)/za-test-kvm.elf: za-test.S $(ASM_UTILS_KVM) $(KVM_LDS)
	$(CC) $(KVM_LDFLAGS) $(filter %.S,$^) -o $@
$(OUTPUT)/zt-test-kvm.elf: zt-test.S $(ASM_UTILS_KVM) $(KVM_LDS)
	$(CC) $(KVM_LDFLAGS) $(filter %.S,$^) -o $@

$(OUTPUT)/%-kvm.bin: $(OUTPUT)/%-kvm.elf
	$(OBJCOPY) -O binary $< $@

include ../../lib.mk
