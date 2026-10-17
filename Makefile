# XTS - Xenon CPU test suite.
#
#   make [PROFILE=smoke] [BUILD=build]     console ELF: $(BUILD)/xts-$(PROFILE).elf32 (+ check_illegal on it)
#   make host-test [BUILD=...]             host unit tests (registry, scheduler, journal resume, sinks, fit, encoder, model)
#   make host-dryrun PROFILE=<p>           run profile <p> on the PC: every block/case, every kernel encoded + decoded back
#   make check [BUILD=...]                 check_encoder (vs xenon-as), check_model (vs results4), check_illegal
#   make usb USB=/run/media/<you>/<stick>  copy the ELF to <stick>/xenon.elf (XeLL autoloads it)
#   make clean [BUILD=...]
#
# Everything a build writes goes below $(BUILD) (generated code in $(BUILD)/gen), so parallel builds with different
# BUILD directories never collide. New files in src/tests, src/conds, src/model, src/smt are picked up automatically.
# Files named *_console.c are built only for the console, *_host.c only for the host.

.DEFAULT_GOAL := all
.SUFFIXES:

# The environment's DEVKITXENON is not trusted (a stale /sdk/xenon is common): only a command-line value overrides.
ifneq ($(origin DEVKITXENON),command line)
DEVKITXENON := /usr/local/xenon
endif
LIBXENON_SRC ?= $(HOME)/Projects/libxenon/libxenon
OPENXENOS ?= $(HOME)/Projects/OpenXenos
PROFILE ?= smoke
BUILD ?= build
PYTHON ?= python3
HOSTCC ?= gcc
HOST_SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer

GEN := $(BUILD)/gen
CON := $(BUILD)/console
HST := $(BUILD)/host
SF := third_party/softfloat-3e

XPREFIX := $(DEVKITXENON)/bin/xenon-
XCC := $(XPREFIX)gcc
XLD := $(XPREFIX)ld
XOBJCOPY := $(XPREFIX)objcopy
XSTRIP := $(XPREFIX)strip

# ---------------------------------------------------------------------------------------------------- sources
FRAMEWORK_SRCS := $(wildcard src/core/*.c src/enc/*.c src/exec/*.c src/timing/*.c src/plat/*.c)
EXT_SRCS := $(wildcard src/model/*.c src/tests/*.c src/conds/*.c src/smt/*.c)
ALL_SRCS := $(FRAMEWORK_SRCS) $(EXT_SRCS)
CON_SRCS := $(filter-out %_host.c,$(ALL_SRCS))
HST_SRCS := $(filter-out %_console.c,$(ALL_SRCS))
CON_ASM := $(wildcard src/exec/*.S src/smt/*.S)
PROFILE_SRC := profiles/$(PROFILE).c
include $(SF)/sources.mk

# ---------------------------------------------------------------------------------------------------- build id
GIT_HASH := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo nogit)$(shell git diff --quiet HEAD 2>/dev/null || echo -dirty)
SRC_HASH := $(shell cat $(sort $(ALL_SRCS) $(CON_ASM) $(wildcard include/xts/*.h profiles/*.c tools/*.py)) isa/xenon_isa.json 2>/dev/null | sha256sum | cut -c1-12)
BUILD_ID := $(GIT_HASH)-$(SRC_HASH)

# ---------------------------------------------------------------------------------------------------- options
# WATCHDOG=1: hang watchdog thread on PIR 4 (SMC reboot -> XeLL reloads xenon.elf -> journal resumes); off by default.
# SPEED_RISKY=1: allow the speed axis to change the CPU speed code (unverified on hardware); off by default.
WATCHDOG ?= 0
SPEED_RISKY ?= 0
OPT_DEFS := $(if $(filter 1,$(WATCHDOG)),-DXTS_WATCHDOG) $(if $(filter 1,$(SPEED_RISKY)),-DXTS_SPEED_RISKY)
# every object depends on this stamp, so changing an option (or CFLAGS_EXTRA) rebuilds everything
OPT_STAMP := $(BUILD)/.options
$(shell mkdir -p $(BUILD); echo '$(OPT_DEFS) $(CFLAGS_EXTRA)' | cmp -s - $(OPT_STAMP) || echo '$(OPT_DEFS) $(CFLAGS_EXTRA)' > $(OPT_STAMP))

# ---------------------------------------------------------------------------------------------------- flags
COMMON_INC := -Iinclude -Isrc -I$(GEN) -I$(SF)/source/include   # -Isrc: framework-private <core/..> <exec/..> <model/..> <plat/..>
XMACHDEP := -mcpu=cell -mtune=cell -maltivec -m32 -mpowerpc64 -mhard-float -fno-pic -DBYTE_ORDER=BIG_ENDIAN
XCFLAGS := -std=gnu11 -g -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format -Wno-missing-field-initializers -Wno-format-truncation \
           -ffunction-sections -fdata-sections -fwrapv -fno-strict-aliasing $(XMACHDEP) -DXTS_CONSOLE \
           $(COMMON_INC) -I$(SF)/platform/console -I$(DEVKITXENON)/usr/include $(OPT_DEFS) $(CFLAGS_EXTRA)
XASFLAGS := -x assembler-with-cpp $(XMACHDEP) -Wa,-mvmx128 -DXTS_CONSOLE $(OPT_DEFS) -Iinclude -I$(DEVKITXENON)/usr/include
HCFLAGS := -std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation -fwrapv \
           -fno-strict-aliasing -DXTS_HOST $(COMMON_INC) -I$(SF)/platform/host $(HOST_SAN) $(OPT_DEFS) $(CFLAGS_EXTRA)
SF_DEFS := -DSOFTFLOAT_FAST_INT64 -DSOFTFLOAT_ROUND_ODD -DINLINE_LEVEL=5 -DSOFTFLOAT_FAST_DIV32TO16 -DSOFTFLOAT_FAST_DIV64TO32
SF_INC := -I$(SF)/source/PowerPC -I$(SF)/source/include

# ---------------------------------------------------------------------------------------------------- generated code
GEN_HDRS := $(GEN)/isa_table.h $(GEN)/build_id.h

$(GEN)/isa_table.c $(GEN)/isa_table.h &: tools/gen_isa.py isa/xenon_isa.json include/xts/isa.h
	@mkdir -p $(GEN)
	$(PYTHON) -B tools/gen_isa.py --json isa/xenon_isa.json --header include/xts/isa.h --out $(GEN)
	@touch $(GEN)/isa_table.c $(GEN)/isa_table.h

$(GEN)/slot.S: tools/gen_slot.py
	@mkdir -p $(GEN)
	$(PYTHON) -B tools/gen_slot.py > $@.tmp && mv $@.tmp $@

# build_id.h changes only when the id does (no needless rebuilds)
$(GEN)/build_id.h: FORCE
	@mkdir -p $(GEN)
	@printf '#define XTS_GIT_HASH "%s"\n#define XTS_BUILD_ID "%s"\n' '$(GIT_HASH)' '$(BUILD_ID)' > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@

# libxenon's fault path builds crashdump's address with lis/ori, which sign-extends to 0xFFFFFFFF8000xxxx when the
# interrupted code runs with MSR[SF]=1, so the jump faults again and nothing is reported: insert a zero-extend.
# Also export ex_program (== ex_alignment) so the XTS vector stub can hand out-of-range interrupts back to libxenon.
$(GEN)/except.S: $(LIBXENON_SRC)/drivers/ppc/except.S
	@mkdir -p $(GEN)
	sed -e 's/^\([[:space:]]*\)ori %r4,%r4,crashdump@l[[:space:]]*$$/&\n\1rldicl %r4,%r4,0,32/' \
	    -e 's/^ex_program:/.globl ex_program\nex_program:/' $< > $@.tmp
	@test `grep -c 'rldicl %r4,%r4,0,32' $@.tmp` -eq 1 || (echo "except.S crashdump patch did not apply" >&2; rm -f $@.tmp; exit 1)
	@test `grep -c '^.globl ex_program' $@.tmp` -eq 1 || (echo "except.S ex_program patch did not apply" >&2; rm -f $@.tmp; exit 1)
	@mv $@.tmp $@

# ---------------------------------------------------------------------------------------------------- console build
CON_OBJS := $(patsubst %.c,$(CON)/obj/%.o,$(CON_SRCS)) $(patsubst %.S,$(CON)/obj/%.o,$(CON_ASM)) \
            $(CON)/gen/isa_table.o $(CON)/gen/slot.o $(CON)/gen/except.o
CON_SF_OBJS := $(patsubst %.c,$(CON)/softfloat/%.o,$(SOFTFLOAT_SRCS) $(SOFTFLOAT_SPEC_SRCS))
ELF := $(CON)/xts-$(PROFILE).elf
ELF32 := $(BUILD)/xts-$(PROFILE).elf32

$(CON)/obj/%.o: %.c $(GEN_HDRS) $(OPT_STAMP)
	@mkdir -p $(@D)
	$(XCC) $(XCFLAGS) -MMD -MP -c $< -o $@
$(CON)/obj/%.o: %.S $(OPT_STAMP)
	@mkdir -p $(@D)
	$(XCC) $(XASFLAGS) -MMD -MP -c $< -o $@
$(CON)/gen/isa_table.o: $(GEN)/isa_table.c $(GEN_HDRS)
	@mkdir -p $(@D)
	$(XCC) $(XCFLAGS) -MMD -MP -c $< -o $@
$(CON)/gen/slot.o: $(GEN)/slot.S
	@mkdir -p $(@D)
	$(XCC) $(XASFLAGS) -c $< -o $@
$(CON)/gen/except.o: $(GEN)/except.S
	@mkdir -p $(@D)
	$(XCC) $(XASFLAGS) -c $< -o $@
$(CON)/softfloat/%.o: $(SF)/source/%.c
	@mkdir -p $(@D)
	$(XCC) -std=gnu11 -O2 $(XMACHDEP) -ffunction-sections -fdata-sections $(SF_DEFS) $(SF_INC) -I$(SF)/platform/console -c $< -o $@
$(CON)/libsoftfloat.a: $(CON_SF_OBJS)
	@rm -f $@
	$(XPREFIX)ar crs $@ $^

XLIBGCC = $(shell $(XCC) $(XMACHDEP) -print-libgcc-file-name)

# patched except.o comes before -lxenon, so the archive's own except.o is not pulled in
$(ELF): $(CON_OBJS) $(CON)/obj/profiles/$(PROFILE).o $(CON)/libsoftfloat.a tools/xts.lds
	$(XLD) -melf32ppc $(CON_OBJS) $(CON)/obj/profiles/$(PROFILE).o $(CON)/libsoftfloat.a --gc-sections \
	  -Map $(CON)/xts-$(PROFILE).map -L$(DEVKITXENON)/usr/lib -L$(DEVKITXENON)/xenon/lib/32 \
	  --start-group -lxenon -lfat -lm -lc $(XLIBGCC) --end-group -n -T tools/xts.lds -o $@

$(ELF32): $(ELF) tools/check_illegal.py
	$(XOBJCOPY) -O elf32-powerpc --adjust-vma 0x80000000 $< $@
	$(XSTRIP) $@
	$(PYTHON) -B tools/check_illegal.py --elf $< --json isa/xenon_isa.json --nm $(XPREFIX)nm
	@echo "built $@ (profile $(PROFILE), build id $(BUILD_ID))"

all: $(ELF32)

# ---------------------------------------------------------------------------------------------------- host build
HST_OBJS := $(patsubst %.c,$(HST)/obj/%.o,$(HST_SRCS)) $(HST)/gen/isa_table.o
HST_SF_OBJS := $(patsubst %.c,$(HST)/softfloat/%.o,$(SOFTFLOAT_SRCS) $(SOFTFLOAT_SPEC_SRCS))
HOST_TEST_SRCS := $(wildcard host/tests/*.c)

$(HST)/obj/%.o: %.c $(GEN_HDRS) $(OPT_STAMP)
	@mkdir -p $(@D)
	$(HOSTCC) $(HCFLAGS) -MMD -MP -c $< -o $@
$(HST)/gen/isa_table.o: $(GEN)/isa_table.c $(GEN_HDRS)
	@mkdir -p $(@D)
	$(HOSTCC) $(HCFLAGS) -MMD -MP -c $< -o $@
$(HST)/softfloat/%.o: $(SF)/source/%.c
	@mkdir -p $(@D)
	$(HOSTCC) -std=gnu11 -O2 $(SF_DEFS) $(SF_INC) -I$(SF)/platform/host -c $< -o $@
$(HST)/libsoftfloat.a: $(HST_SF_OBJS)
	@rm -f $@
	ar crs $@ $^

HOST_LINK = $(HOSTCC) $(HOST_SAN) -o $@ $^ -lm

ENC_OBJS := $(HST)/obj/src/enc/enc.o $(HST)/obj/src/enc/isa.o $(HST)/gen/isa_table.o
MODEL_OBJS := $(patsubst %.c,$(HST)/obj/%.o,$(wildcard src/model/*.c)) $(HST)/obj/src/exec/state.o $(ENC_OBJS)

$(HST)/xts-enc-cli: $(HST)/obj/host/enc_cli.o $(ENC_OBJS)
	$(HOST_LINK)
$(HST)/xts-model-replay: $(HST)/obj/host/model_replay.o $(MODEL_OBJS) $(HST)/libsoftfloat.a
	$(HOST_LINK)
$(HST)/xts-host-test: $(HST)/obj/host/test_main.o $(patsubst %.c,$(HST)/obj/%.o,$(HOST_TEST_SRCS)) $(HST_OBJS) $(HST)/libsoftfloat.a
	$(HOST_LINK)
$(HST)/xts-dryrun-$(PROFILE): $(HST)/obj/host/dryrun_main.o $(HST_OBJS) $(HST)/obj/profiles/$(PROFILE).o $(HST)/libsoftfloat.a
	$(HOST_LINK)

host-tools: $(HST)/xts-enc-cli $(HST)/xts-model-replay

host-test: $(HST)/xts-host-test
	rm -rf $(BUILD)/host-test
	ASAN_OPTIONS=detect_leaks=0 $(HST)/xts-host-test --tmp $(BUILD)/host-test

host-dryrun: $(HST)/xts-dryrun-$(PROFILE)
	rm -rf $(BUILD)/dryrun-$(PROFILE)
	ASAN_OPTIONS=detect_leaks=0 XTS_QUIET=1 $(HST)/xts-dryrun-$(PROFILE) --root $(BUILD)/dryrun-$(PROFILE)

# ---------------------------------------------------------------------------------------------------- checks
check-encoder: $(HST)/xts-enc-cli
	$(PYTHON) -B tools/check_encoder.py --cli $(HST)/xts-enc-cli --as $(XPREFIX)as --work $(BUILD)/check-encoder

check-model: $(HST)/xts-model-replay
	ASAN_OPTIONS=detect_leaks=0 $(PYTHON) -B tools/check_model.py --replay $(HST)/xts-model-replay --work $(BUILD)/check-model \
	  --openxenos $(OPENXENOS)

check-illegal: $(ELF32)

# record schema / vocabulary / key-set check on the profile's host dry run. KEYS=<file> compares (family, case_id, metric)
# against a snapshot (write one with: tools/check_records.py <run> --write-keys FILE). smoke has a record budget.
SMOKE_BUDGET ?= 500
check-records: host-dryrun
	$(PYTHON) -B tools/check_records.py $(BUILD)/dryrun-$(PROFILE)/xts/host/0001 $(if $(KEYS),--keys-baseline $(KEYS)) \
	  $(if $(filter smoke,$(PROFILE)),--max-records $(SMOKE_BUDGET))

check: check-encoder check-model check-illegal check-records

usb: $(ELF32)
	@test -n "$(USB)" || (echo "usage: make usb USB=/path/to/mounted/stick [PROFILE=...]" >&2; exit 1)
	tools/usb_install.sh $(ELF32) $(USB)

bootstrap:
	XTS_DEVKITXENON=$(DEVKITXENON) LIBXENON_SRC=$(LIBXENON_SRC) tools/bootstrap.sh

clean:
	rm -rf $(BUILD)

FORCE:
.PHONY: all host-tools host-test host-dryrun check check-encoder check-model check-illegal check-records usb bootstrap clean FORCE

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
