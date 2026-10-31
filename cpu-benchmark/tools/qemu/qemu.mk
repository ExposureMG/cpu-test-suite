# tools/qemu/qemu.mk - big-endian validation of the kernels under qemu-ppc.
#
#   make qemu-check                        (the main Makefile ends with
#                                           "-include tools/qemu/qemu.mk")
#   make -f tools/qemu/qemu.mk qemu-check  (standalone, from the project root)
#   make ... qemu-clean                    (removes build/qemu/ only)
#
# Compiles src/kernels/*_*.c and src/kernels/table.c with BOTH Xenon
# compilers using 32-bit-only flags (qemu-ppc emulates a G4: no 64-bit
# instructions, no VMX128), links them with a freestanding runtime into
# build/qemu/bench-qemu.elf and runs it under qemu-ppc.  See
# tools/qemu/README.md.
#
# This fragment defines only the targets qemu-check and qemu-clean (plus file
# targets under build/qemu/) and variables prefixed QEMU_.  X3, O16 and OPT
# are reused when the including Makefile (or the command line) defines them.

QEMU_MK := $(lastword $(MAKEFILE_LIST))
QEMU_DIR := $(patsubst %/,%,$(dir $(QEMU_MK)))
QEMU_ROOT := $(patsubst %/,%,$(dir $(patsubst %/,%,$(dir $(QEMU_DIR)))))

# ---- configuration ------------------------------------------------------------
QEMU_X3 = $(if $(X3),$(X3),/home/e3xp0/Projects/libxenon/libxenon/toolchain/testxenon3)
QEMU_O16 = $(if $(O16),$(O16),/home/e3xp0/Projects/libxenon/libxenon/toolchain/test16old)
QEMU_OPT = $(if $(OPT),$(OPT),-O2)
QEMU_BIN ?= qemu-ppc
QEMU_CPU ?= 7400
QEMU_GCC_CPU ?= 7400
QEMU_TIMEOUT ?= 1200
QEMU_HOSTCC ?= gcc
QEMU_ARGS ?=
QEMU_XCHECK ?= 1

QEMU_OUT = $(QEMU_ROOT)/build/qemu
QEMU_HOUT = $(QEMU_OUT)/host
QEMU_KDIR = $(QEMU_ROOT)/src/kernels
QEMU_INC = $(QEMU_ROOT)/include

# 32-bit-only code: G4 (7400) with AltiVec, no -mpowerpc64.  -mno-vmx128 is
# implied by -mcpu=7400 on testxenon3 and passed explicitly anyway;
# test16old does not know the option.
QEMU_ARCH = -m32 -mcpu=$(QEMU_GCC_CPU) -mno-powerpc64 -maltivec -mhard-float -fno-pic
QEMU_KCFLAGS = -std=gnu11 -DXENON $(QEMU_ARCH) $(QEMU_OPT) -Wall -Wextra -I$(QEMU_INC)
QEMU_RTCFLAGS = -std=gnu11 $(QEMU_ARCH) -mno-vmx128 -O2 -ffreestanding -fno-builtin \
	-fno-tree-loop-distribute-patterns -ffp-contract=off -Wall -Wextra \
	-I$(QEMU_INC) -I$(QEMU_DIR)
QEMU_HOSTCFLAGS = -std=gnu11 -O2 -Wall -Wextra -ffp-contract=off
QEMU_CC_xenon3 = $(QEMU_X3)/bin/xenon-gcc
QEMU_CC_old16 = $(QEMU_O16)/bin/xenon-gcc
QEMU_LD = $(QEMU_X3)/bin/xenon-ld
QEMU_VFLAGS_xenon3 = -mno-vmx128 -DBENCH_VARIANT=xenon3 -DBENCH_LABEL='"testxenon3"'
QEMU_VFLAGS_old16 = -DBENCH_VARIANT=old16 -DBENCH_LABEL='"test16old"'
QEMU_CFLAGS_STR = -DBENCH_CFLAGS='"$(QEMU_OPT) (qemu: -m32 -mcpu=$(QEMU_GCC_CPU) -mno-powerpc64)"'

# ---- files ----------------------------------------------------------------------
QEMU_KSRC = $(sort $(wildcard $(QEMU_KDIR)/*_*.c))
QEMU_KNAMES = $(notdir $(basename $(QEMU_KSRC))) table
QEMU_KOBJ_xenon3 = $(addprefix $(QEMU_OUT)/xenon3/,$(addsuffix .o,$(QEMU_KNAMES)))
QEMU_KOBJ_old16 = $(addprefix $(QEMU_OUT)/old16/,$(addsuffix .o,$(QEMU_KNAMES)))
QEMU_RT_OBJ = $(addprefix $(QEMU_OUT)/rt/,start.o savres.o qrt.o qrt_fmt.o qlibgcc.o qmath.o qblib.o)
QEMU_RT_COBJ = $(addprefix $(QEMU_OUT)/rt/,qrt.o qrt_fmt.o qlibgcc.o qmath.o qblib.o qrunner.o qst_main.o)
QEMU_RT_SOBJ = $(addprefix $(QEMU_OUT)/rt/,start.o savres.o)
QEMU_ST_OBJ = $(QEMU_OUT)/rt/qselftest.xenon3.o $(QEMU_OUT)/rt/qselftest.old16.o
QEMU_ELF = $(QEMU_OUT)/bench-qemu.elf
QEMU_ST_ELF = $(QEMU_OUT)/qselftest.elf
QEMU_HOST_KOBJ = $(addprefix $(QEMU_HOUT)/k/,$(addsuffix .o,$(QEMU_KNAMES)))
QEMU_HOST_RUNNER = $(QEMU_HOUT)/qrunner-host
QEMU_RT_HDRS = $(QEMU_DIR)/qrt.h $(QEMU_DIR)/qmath.h $(QEMU_INC)/bench.h
QEMU_FLAGS = $(QEMU_OUT)/flags.txt
QEMU_KLIST = $(QEMU_OUT)/kernels.txt

# Kernel ids without a source file yet (evaluated only when linking).
QEMU_KIDS = $(shell sed -n 's/^[[:space:]]*BENCH_LIST(\([a-z0-9_]*\)).*/\1/p' $(QEMU_KDIR)/list_*.h)
QEMU_KDEFD = $(patsubst BENCH_KDEF(%),%,$(shell grep -ho 'BENCH_KDEF([a-z0-9_]*)' $(QEMU_KSRC) /dev/null))
QEMU_MISSING = $(filter-out $(QEMU_KDEFD),$(QEMU_KIDS))

# ---- targets ----------------------------------------------------------------------
.PHONY: qemu-check qemu-clean

qemu-check: $(QEMU_ELF) $(QEMU_ST_ELF) $(QEMU_HOUT)/qselftest $(QEMU_HOUT)/qhosttest.ok \
		$(if $(filter 1,$(QEMU_XCHECK)),$(QEMU_HOST_RUNNER))
	@QEMU_BIN='$(QEMU_BIN)' QEMU_CPU='$(QEMU_CPU)' QEMU_TIMEOUT='$(QEMU_TIMEOUT)' \
	  QEMU_ARGS='$(QEMU_ARGS)' QEMU_OUT='$(QEMU_OUT)' QEMU_X3='$(QEMU_X3)' \
	  QEMU_ELF='$(QEMU_ELF)' QEMU_ST_ELF='$(QEMU_ST_ELF)' \
	  QEMU_HOST_ST='$(QEMU_HOUT)/qselftest' \
	  QEMU_HOST_RUNNER='$(if $(filter 1,$(QEMU_XCHECK)),$(QEMU_HOST_RUNNER))' \
	  QEMU_OBJS='$(QEMU_KOBJ_xenon3) $(QEMU_KOBJ_old16) $(QEMU_RT_OBJ) $(QEMU_OUT)/rt/qrunner.o $(QEMU_ST_OBJ) $(QEMU_OUT)/rt/qst_main.o' \
	  bash $(QEMU_DIR)/qcheck.sh

qemu-clean:
	rm -rf $(QEMU_OUT)

$(QEMU_OUT)/xenon3 $(QEMU_OUT)/old16 $(QEMU_OUT)/rt $(QEMU_HOUT)/k:
	mkdir -p $@

# Everything depends on the flags it was built with: flags.txt is rewritten
# (and so becomes newer) only when they change.  .force is never created.
$(QEMU_OUT)/.force:
$(QEMU_FLAGS): $(QEMU_OUT)/.force | $(QEMU_OUT)/rt
	@printf '%s\n' "x3=$(QEMU_X3)" "o16=$(QEMU_O16)" "k=$(QEMU_KCFLAGS)" "rt=$(QEMU_RTCFLAGS)" \
	  "host=$(QEMU_HOSTCC) $(QEMU_HOSTCFLAGS)" > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

# The set of kernel files: a removed kernel must cause a relink too.
$(QEMU_KLIST): $(QEMU_OUT)/.force | $(QEMU_OUT)/rt
	@printf '%s\n' $(QEMU_KNAMES) > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

# Kernels and table, once per toolchain (the real sources, 32-bit flags).
$(QEMU_KOBJ_xenon3): $(QEMU_OUT)/xenon3/%.o: $(QEMU_KDIR)/%.c $(QEMU_FLAGS) | $(QEMU_OUT)/xenon3
	$(QEMU_CC_xenon3) $(QEMU_KCFLAGS) $(QEMU_VFLAGS_xenon3) $(QEMU_CFLAGS_STR) -MMD -MP -c $< -o $@

$(QEMU_KOBJ_old16): $(QEMU_OUT)/old16/%.o: $(QEMU_KDIR)/%.c $(QEMU_FLAGS) | $(QEMU_OUT)/old16
	$(QEMU_CC_old16) $(QEMU_KCFLAGS) $(QEMU_VFLAGS_old16) $(QEMU_CFLAGS_STR) -MMD -MP -c $< -o $@

# Runtime (testxenon3, like the real harness).
$(QEMU_RT_COBJ): $(QEMU_OUT)/rt/%.o: $(QEMU_DIR)/%.c $(QEMU_RT_HDRS) $(QEMU_FLAGS) | $(QEMU_OUT)/rt
	$(QEMU_CC_xenon3) $(QEMU_RTCFLAGS) -c $< -o $@

$(QEMU_RT_SOBJ): $(QEMU_OUT)/rt/%.o: $(QEMU_DIR)/%.S $(QEMU_FLAGS) | $(QEMU_OUT)/rt
	$(QEMU_CC_xenon3) $(QEMU_ARCH) -mno-vmx128 -c $< -o $@

# Helper self-test, compiled like a kernel by each toolchain.
$(QEMU_OUT)/rt/qselftest.xenon3.o: $(QEMU_DIR)/qselftest.c $(QEMU_DIR)/qrt.h $(QEMU_FLAGS) | $(QEMU_OUT)/rt
	$(QEMU_CC_xenon3) $(QEMU_KCFLAGS) -mno-vmx128 -ffp-contract=off -I$(QEMU_DIR) -DQST_NS=xenon3 -c $< -o $@

$(QEMU_OUT)/rt/qselftest.old16.o: $(QEMU_DIR)/qselftest.c $(QEMU_DIR)/qrt.h $(QEMU_FLAGS) | $(QEMU_OUT)/rt
	$(QEMU_CC_old16) $(QEMU_KCFLAGS) -ffp-contract=off -I$(QEMU_DIR) -DQST_NS=old16 -c $< -o $@

# Static link, no toolchain libraries (no newlib, no libgcc.a).
$(QEMU_ELF): $(QEMU_RT_OBJ) $(QEMU_OUT)/rt/qrunner.o $(QEMU_KOBJ_xenon3) $(QEMU_KOBJ_old16) $(QEMU_KLIST)
	@if [ -n "$(QEMU_MISSING)" ]; then \
	  echo "qemu-check: no kernel source yet for: $(QEMU_MISSING)"; \
	  echo "  table.c references every kernel; tools/qemu/qscratch.sh runs the check on a"; \
	  echo "  scratch copy with stubs for the missing ones."; exit 1; fi
	$(QEMU_LD) -m elf32ppc -static -e _start -Map $(QEMU_OUT)/bench-qemu.map -o $@ $(filter %.o,$^)

$(QEMU_ST_ELF): $(QEMU_RT_OBJ) $(QEMU_OUT)/rt/qst_main.o $(QEMU_ST_OBJ)
	$(QEMU_LD) -m elf32ppc -static -e _start -o $@ $^

# ---- host side ----------------------------------------------------------------------
# Unit test of the runtime's C code against native arithmetic and glibc.
$(QEMU_HOUT)/qhosttest: $(addprefix $(QEMU_DIR)/,qhosttest.c qlibgcc.c qmath.c qmath.h qblib.c) \
		$(QEMU_FLAGS) | $(QEMU_HOUT)/k
	$(QEMU_HOSTCC) $(QEMU_HOSTCFLAGS) -fno-builtin -fno-tree-loop-distribute-patterns -DQLG_HOST \
	  -I$(QEMU_DIR) -c $(QEMU_DIR)/qlibgcc.c -o $(QEMU_HOUT)/qlibgcc.o
	$(QEMU_HOSTCC) $(QEMU_HOSTCFLAGS) -fno-builtin -fno-tree-loop-distribute-patterns -DQLG_HOST \
	  -I$(QEMU_DIR) -c $(QEMU_DIR)/qmath.c -o $(QEMU_HOUT)/qmath.o
	$(QEMU_HOSTCC) $(QEMU_HOSTCFLAGS) -fno-builtin -fno-tree-loop-distribute-patterns -DQLG_HOST \
	  -I$(QEMU_DIR) -c $(QEMU_DIR)/qblib.c -o $(QEMU_HOUT)/qblib.o
	$(QEMU_HOSTCC) $(QEMU_HOSTCFLAGS) -I$(QEMU_DIR) $(QEMU_DIR)/qhosttest.c \
	  $(QEMU_HOUT)/qlibgcc.o $(QEMU_HOUT)/qmath.o $(QEMU_HOUT)/qblib.o -lm -o $@

$(QEMU_HOUT)/qhosttest.ok: $(QEMU_HOUT)/qhosttest
	@echo "qemu: host unit test of the runtime helpers ($<)"
	@$< -q > $(QEMU_HOUT)/qhosttest.log 2>&1 || { cat $(QEMU_HOUT)/qhosttest.log; exit 1; }
	@cat $(QEMU_HOUT)/qhosttest.log
	@touch $@

# Reference output of the helper self-test: native host arithmetic.
$(QEMU_HOUT)/qselftest: $(addprefix $(QEMU_DIR)/,qselftest.c qst_main.c qrt_fmt.c qrt_host.c qrt.h) \
		$(QEMU_FLAGS) | $(QEMU_HOUT)/k
	$(QEMU_HOSTCC) $(QEMU_HOSTCFLAGS) -I$(QEMU_DIR) -I$(QEMU_INC) -DQST_NS=host -DQST_HOST \
	  $(QEMU_DIR)/qselftest.c $(QEMU_DIR)/qst_main.c $(QEMU_DIR)/qrt_fmt.c $(QEMU_DIR)/qrt_host.c -lm -o $@

# BE/LE cross-check: the same runner over host-compiled kernels (scalar
# fallbacks, host libc).  Informational: a host build failure only skips it.
$(QEMU_HOST_KOBJ): $(QEMU_HOUT)/k/%.o: $(QEMU_KDIR)/%.c $(QEMU_FLAGS) | $(QEMU_HOUT)/k
	-$(QEMU_HOSTCC) $(QEMU_HOSTCFLAGS) -DBENCH_HOST -DBENCH_VARIANT=xenon3 -DBENCH_LABEL='"host"' \
	  -DBENCH_CFLAGS='"-O2"' -I$(QEMU_INC) -MMD -MP -c $< -o $@

$(QEMU_HOST_RUNNER): $(QEMU_HOST_KOBJ) $(addprefix $(QEMU_DIR)/,qrunner.c qrt_fmt.c qrt_host.c qrt.h) $(QEMU_KLIST)
	@rm -f $@
	-$(QEMU_HOSTCC) $(QEMU_HOSTCFLAGS) -DQR_SINGLE_VARIANT -I$(QEMU_DIR) -I$(QEMU_INC) \
	  $(QEMU_DIR)/qrunner.c $(QEMU_DIR)/qrt_fmt.c $(QEMU_DIR)/qrt_host.c $(QEMU_HOST_KOBJ) -lm -o $@

-include $(wildcard $(QEMU_OUT)/xenon3/*.d $(QEMU_OUT)/old16/*.d $(QEMU_HOUT)/k/*.d)
