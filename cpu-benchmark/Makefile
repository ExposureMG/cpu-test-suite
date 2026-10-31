# LibXenon CPU benchmark - build (DESIGN.md section 7).
#
#   make / make all  build/bench-ab.elf32 + the single-toolchain images
#   make ab          build/bench-ab.elf32: both compilers in one image
#   make single      build/bench-xenon3.elf32 build/bench-old16.elf32
#   make host        build/host/bench-host (host gcc, both namespaces)
#   make check       host ASan run, host quick runs at 148x41 and 74x27,
#                    check_syms.py over every cross-compiled kernel object,
#                    link-map check of the A/B image, Xenon instruction-set
#                    scan of the three images (check-isa)
#   make asm         per-kernel .s for both variants in build/asm/<variant>/
#   make sizes       per-kernel .text bytes, A vs B
#   make refs        regenerate src/harness/refs.h (the big-endian reference
#                    results the images check every kernel against) from a
#                    qemu-check run; needs qemu-ppc
#   make clean       remove build/
#
# Variables: X3, O16 (toolchain roots), OPT (kernel optimisation flags, both
# variants), MACHDEP, HOST_OPT, BENCH_DEFS (extra harness defines, e.g.
# -DBENCH_SAMPLES=9 -DBENCH_SAMPLE_MS=100), PLACE_ALIGN (boundary every
# kernel's and newlib routine's code and tables start on, default 16384; 0
# links them packed).  Changing any of them rebuilds what depends on it.
#
# Kernel sources are src/kernels/<cat>_<id>.c.  Kernels listed in
# src/kernels/list_*.h whose source does not exist yet are linked as
# placeholders (build/gen/missing.c) that the runner reports as MISSING, so
# the images always link; `make check` fails while any are missing.

X3       ?= /home/e3xp0/Projects/libxenon/libxenon/toolchain/testxenon3
O16      ?= /home/e3xp0/Projects/libxenon/libxenon/toolchain/test16old
OPT      ?= -O2
MACHDEP  ?= -DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float
HOSTCC   ?= gcc
HOST_OPT ?= -O2
PYTHON   ?= python3
BENCH_DEFS ?=
# Each kernel object's and newlib member's .text/.rodata starts on a
# PLACE_ALIGN boundary (tools/place.py) in every image, so identical code gets
# the same cache sets and branch-history entries in A and B whatever was
# linked before it.  16 KB is the way size of Xenon's 2-way 32 KB L1
# instruction cache and covers its 4K-entry branch history table.
PLACE_ALIGN ?= 16384
HOST_A_LABEL ?= host-A
HOST_B_LABEL ?= host-B

BUILD := build

# variant A / B (DESIGN.md table)
A    := xenon3
B    := old16
A_LABEL := testxenon3
B_LABEL := test16old
A_CC := $(X3)/bin/xenon-gcc
B_CC := $(O16)/bin/xenon-gcc

.SUFFIXES:
.DELETE_ON_ERROR:
.DEFAULT_GOAL := all

# ---- sources ---------------------------------------------------------------

CATS  := int br mem fp vec mix lib
LISTS := $(foreach c,$(CATS),src/kernels/list_$(c).h)
KSRC  := $(sort $(wildcard $(foreach c,$(CATS),src/kernels/$(c)_*.c)))
KIDS  := $(shell sed -n 's/^[[:space:]]*BENCH_LIST(\([a-z0-9_]*\)).*/\1/p' $(LISTS) /dev/null)
KDEFD := $(patsubst BENCH_KDEF(%),%,$(shell grep -ho 'BENCH_KDEF([a-z0-9_]*)' $(KSRC) /dev/null))
MISSING := $(filter-out $(KDEFD),$(KIDS))
ifneq ($(MISSING),)
$(info note: no source yet for $(words $(MISSING)) kernel(s), linked as MISSING placeholders: $(MISSING))
endif

HSRC_X := main.c runner.c report.c variants.c platform_xenon.c
HSRC_H := main.c runner.c report.c variants.c platform_host.c

KCFLAGS = -std=gnu11 $(MACHDEP) $(OPT) -Wall -Wextra -g -Iinclude -MMD -MP
# The harness is the referee, not the subject: no auto-vectorisation, so its
# guard and timing code stays plain scalar code on both toolchains.
HCFLAGS = -std=gnu11 $(MACHDEP) -O2 -fno-tree-vectorize -g -Wall -Wextra -Iinclude \
	  -Isrc/harness $(BENCH_DEFS) -MMD -MP
HOSTCF  = -std=gnu11 -g -Wall -Wextra -DBENCH_HOST -Iinclude -Isrc/harness -MMD -MP
ASANCF  = -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

kobjs = $(patsubst src/kernels/%.c,$(BUILD)/$(1)/kernels/%.o,$(KSRC) src/kernels/table.c) \
	$(if $(MISSING),$(BUILD)/$(1)/gen/missing.o)
# the same objects as linked: copies with their sections on PLACE_ALIGN
pobjs = $(patsubst $(BUILD)/%,$(BUILD)/placed/%,$(call kobjs,$(1)))

A_KOBJS := $(call kobjs,$(A))
B_KOBJS := $(call kobjs,$(B))
A_POBJS := $(call pobjs,$(A))
B_POBJS := $(call pobjs,$(B))
A_LIB   := $(BUILD)/$(A)/lib_$(A).o
B_LIB   := $(BUILD)/$(B)/lib_$(B).o

AB_HOBJS  := $(patsubst %.c,$(BUILD)/ab/harness/%.o,$(HSRC_X))
SA_HOBJS  := $(patsubst %.c,$(BUILD)/single-$(A)/harness/%.o,$(HSRC_X))
SB_HOBJS  := $(patsubst %.c,$(BUILD)/single-$(B)/harness/%.o,$(HSRC_X))

H_KOBJS   := $(call kobjs,host/$(A)) $(call kobjs,host/$(B))
H_POBJS   := $(call pobjs,host/$(A)) $(call pobjs,host/$(B))
H_HOBJS   := $(patsubst %.c,$(BUILD)/host/harness/%.o,$(HSRC_H))
HS_HOBJS  := $(filter-out %/variants.o,$(H_HOBJS)) $(BUILD)/host/single/variants.o
HA_KOBJS  := $(call kobjs,host-asan/$(A)) $(call kobjs,host-asan/$(B))
HA_HOBJS  := $(patsubst %.c,$(BUILD)/host-asan/harness/%.o,$(HSRC_H))

ASM := $(patsubst src/kernels/%.c,$(BUILD)/asm/$(A)/%.s,$(KSRC)) \
       $(patsubst src/kernels/%.c,$(BUILD)/asm/$(B)/%.s,$(KSRC))

ALL_OBJS := $(A_KOBJS) $(B_KOBJS) $(AB_HOBJS) $(SA_HOBJS) $(SB_HOBJS) \
	    $(H_KOBJS) $(H_HOBJS) $(HS_HOBJS) $(HA_KOBJS) $(HA_HOBJS)

# reached only through the placed copies: keep them (incremental builds)
.SECONDARY: $(A_KOBJS) $(B_KOBJS) $(H_KOBJS)

# ---- top-level targets -------------------------------------------------------

.PHONY: all ab single host host-asan check check-host check-syms check-ab \
	check-isa check-refs asm sizes libprefix refs clean help FORCE

all: ab single

ab: $(BUILD)/bench-ab.elf32

single: $(BUILD)/bench-$(A).elf32 $(BUILD)/bench-$(B).elf32

host: $(BUILD)/host/bench-host $(BUILD)/host/bench-host-single

host-asan: $(BUILD)/host-asan/bench-host

libprefix: $(A_LIB) $(B_LIB)

help:
	@sed -n '2,29s/^# \{0,1\}//p' Makefile

# ---- flag stamps: rebuild when a variable changes ---------------------------

# $(call stamp,<content>) writes $@ only when <content> differs
define stamp
	@mkdir -p $(@D)
	@printf '%s\n' "$(1)" > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi
endef

$(BUILD)/$(A)/flags: FORCE
	$(call stamp,$(A_CC) $(MACHDEP) $(OPT))
$(BUILD)/$(B)/flags: FORCE
	$(call stamp,$(B_CC) $(MACHDEP) $(OPT))
$(BUILD)/ab/flags $(BUILD)/single-$(A)/flags: FORCE
	$(call stamp,$(A_CC) $(MACHDEP) $(BENCH_DEFS))
$(BUILD)/single-$(B)/flags: FORCE
	$(call stamp,$(B_CC) $(MACHDEP) $(BENCH_DEFS))
$(BUILD)/host/flags: FORCE
	$(call stamp,$(HOSTCC) $(HOST_OPT) $(BENCH_DEFS) $(HOST_A_LABEL) $(HOST_B_LABEL))
$(BUILD)/host-asan/flags: FORCE
	$(call stamp,$(HOSTCC) $(ASANCF) $(BENCH_DEFS) $(HOST_A_LABEL) $(HOST_B_LABEL))
$(BUILD)/placed/flags: FORCE
	$(call stamp,$(PLACE_ALIGN))

# ---- placeholders for kernels without a source --------------------------------

$(BUILD)/gen/missing.c: FORCE
	@mkdir -p $(@D)
	@{ echo '/* Generated by the Makefile: kernels listed in src/kernels/list_*.h'; \
	   echo ' * whose source does not exist yet.  The runner reports them as MISSING. */'; \
	   echo '#include "bench_kernel.h"'; \
	   for id in $(MISSING); do \
	     cat=; for c in $(CATS); do \
	       if grep -q "^[[:space:]]*BENCH_LIST($$id)" src/kernels/list_$$c.h; then cat=$$c; fi; \
	     done; \
	     C=$$(echo "$$cat" | tr a-z A-Z); \
	     printf 'BENCH_KDEF(%s) = {\n\t.name = "%s", .desc = "(kernel source missing)", .unit = "-",\n\t.category = BENCH_CAT_%s, .flags = BENCH_F_EXACT, .units = 1,\n};\n' \
	       "$$id" "$$id" "$$C"; \
	   done; } > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

# ---- cross-compiled kernels (per variant) ------------------------------------

$(BUILD)/$(A)/kernels/%.o: src/kernels/%.c $(BUILD)/$(A)/flags
	@mkdir -p $(@D)
	$(A_CC) $(KCFLAGS) -DBENCH_VARIANT=$(A) -DBENCH_LABEL='"$(A_LABEL)"' \
		-DBENCH_CFLAGS='"$(OPT)"' -c $< -o $@
$(BUILD)/$(B)/kernels/%.o: src/kernels/%.c $(BUILD)/$(B)/flags
	@mkdir -p $(@D)
	$(B_CC) $(KCFLAGS) -DBENCH_VARIANT=$(B) -DBENCH_LABEL='"$(B_LABEL)"' \
		-DBENCH_CFLAGS='"$(OPT)"' -c $< -o $@
$(BUILD)/$(A)/gen/missing.o: $(BUILD)/gen/missing.c $(BUILD)/$(A)/flags
	@mkdir -p $(@D)
	$(A_CC) $(KCFLAGS) -DBENCH_VARIANT=$(A) -c $< -o $@
$(BUILD)/$(B)/gen/missing.o: $(BUILD)/gen/missing.c $(BUILD)/$(B)/flags
	@mkdir -p $(@D)
	$(B_CC) $(KCFLAGS) -DBENCH_VARIANT=$(B) -c $< -o $@

# ---- placement: link copies with code and tables on PLACE_ALIGN -------------

PLACE = $(PYTHON) tools/place.py --align $(PLACE_ALIGN)

$(BUILD)/placed/%.o: $(BUILD)/%.o tools/place.py $(BUILD)/placed/flags
	@mkdir -p $(@D)
	@$(PLACE) $< $@

# ---- each toolchain's newlib routines, prefixed (tools/libprefix.py) ----------

LIBDEPS = include/bench_lib.h tools/libprefix.py tools/place.py $(BUILD)/placed/flags

$(A_LIB): $(LIBDEPS) $(X3)/xenon/lib/32/libc.a $(X3)/xenon/lib/32/libm.a
	@mkdir -p $(@D)
	$(PYTHON) tools/libprefix.py --tc $(X3) --prefix $(A)_lib_ --header include/bench_lib.h \
		--align $(PLACE_ALIGN) -o $@
$(B_LIB): $(LIBDEPS) $(O16)/xenon/lib/32/libc.a $(O16)/xenon/lib/32/libm.a
	@mkdir -p $(@D)
	$(PYTHON) tools/libprefix.py --tc $(O16) --prefix $(B)_lib_ --header include/bench_lib.h \
		--align $(PLACE_ALIGN) -o $@

# ---- harness, compiled by each image's linking toolchain -----------------------

$(BUILD)/ab/harness/%.o: src/harness/%.c $(BUILD)/ab/flags
	@mkdir -p $(@D)
	$(A_CC) $(HCFLAGS) -I$(X3)/usr/include -DBENCH_A=$(A) -DBENCH_B=$(B) -c $< -o $@
$(BUILD)/single-$(A)/harness/%.o: src/harness/%.c $(BUILD)/single-$(A)/flags
	@mkdir -p $(@D)
	$(A_CC) $(HCFLAGS) -I$(X3)/usr/include -DBENCH_A=$(A) -c $< -o $@
$(BUILD)/single-$(B)/harness/%.o: src/harness/%.c $(BUILD)/single-$(B)/flags
	@mkdir -p $(@D)
	$(B_CC) $(HCFLAGS) -I$(O16)/usr/include -DBENCH_A=$(B) -c $< -o $@

# ---- link (devkitxenon rules) ---------------------------------------------------

# $(call link,<toolchain root>,<map file>)
define link
	$(1)/bin/xenon-gcc $(filter %.o,$^) -g $(MACHDEP) -Wl,--gc-sections \
		-Wl,-Map,$(2) -L$(1)/xenon/lib/32 -L$(1)/usr/lib -lxenon -lm -n \
		-T $(1)/app.lds -o $@
endef

# $(call elf32,<toolchain root>)
define elf32
	$(1)/bin/xenon-objcopy -O elf32-powerpc --adjust-vma 0x80000000 $< $@
	$(1)/bin/xenon-strip $@
endef

$(BUILD)/bench-ab.elf: $(AB_HOBJS) $(A_POBJS) $(B_POBJS) $(A_LIB) $(B_LIB)
	$(call link,$(X3),$(BUILD)/bench-ab.map)
$(BUILD)/bench-$(A).elf: $(SA_HOBJS) $(A_POBJS) $(A_LIB)
	$(call link,$(X3),$(BUILD)/bench-$(A).map)
$(BUILD)/bench-$(B).elf: $(SB_HOBJS) $(B_POBJS) $(B_LIB)
	$(call link,$(O16),$(BUILD)/bench-$(B).map)

$(BUILD)/bench-ab.elf32 $(BUILD)/bench-$(A).elf32: $(BUILD)/%.elf32: $(BUILD)/%.elf
	$(call elf32,$(X3))
$(BUILD)/bench-$(B).elf32: $(BUILD)/bench-$(B).elf
	$(call elf32,$(O16))

# ---- host build (both namespaces, host gcc) --------------------------------------

$(BUILD)/host/$(A)/kernels/%.o: src/kernels/%.c $(BUILD)/host/flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTCF) $(HOST_OPT) -DBENCH_VARIANT=$(A) -DBENCH_LABEL='"$(HOST_A_LABEL)"' \
		-DBENCH_CFLAGS='"$(HOST_OPT)"' -c $< -o $@
$(BUILD)/host/$(B)/kernels/%.o: src/kernels/%.c $(BUILD)/host/flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTCF) $(HOST_OPT) -DBENCH_VARIANT=$(B) -DBENCH_LABEL='"$(HOST_B_LABEL)"' \
		-DBENCH_CFLAGS='"$(HOST_OPT)"' -c $< -o $@
$(BUILD)/host/%/gen/missing.o: $(BUILD)/gen/missing.c $(BUILD)/host/flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTCF) $(HOST_OPT) -DBENCH_VARIANT=$* -c $< -o $@
$(BUILD)/host/harness/%.o: src/harness/%.c $(BUILD)/host/flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTCF) -O2 $(BENCH_DEFS) -DBENCH_A=$(A) -DBENCH_B=$(B) -c $< -o $@
$(BUILD)/host/bench-host: $(H_HOBJS) $(H_POBJS)
	$(HOSTCC) $(filter %.o,$^) -lm -o $@
# single-variant report (like bench-xenon3.elf32), A's kernels only
$(BUILD)/host/single/variants.o: src/harness/variants.c $(BUILD)/host/flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTCF) -O2 $(BENCH_DEFS) -DBENCH_A=$(A) -c $< -o $@
$(BUILD)/host/bench-host-single: $(HS_HOBJS) $(call pobjs,host/$(A))
	$(HOSTCC) $(filter %.o,$^) -lm -o $@

$(BUILD)/host-asan/$(A)/kernels/%.o: src/kernels/%.c $(BUILD)/host-asan/flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTCF) $(ASANCF) -DBENCH_VARIANT=$(A) -DBENCH_LABEL='"$(HOST_A_LABEL)"' \
		-DBENCH_CFLAGS='"$(ASANCF)"' -c $< -o $@
$(BUILD)/host-asan/$(B)/kernels/%.o: src/kernels/%.c $(BUILD)/host-asan/flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTCF) $(ASANCF) -DBENCH_VARIANT=$(B) -DBENCH_LABEL='"$(HOST_B_LABEL)"' \
		-DBENCH_CFLAGS='"$(ASANCF)"' -c $< -o $@
$(BUILD)/host-asan/%/gen/missing.o: $(BUILD)/gen/missing.c $(BUILD)/host-asan/flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTCF) $(ASANCF) -DBENCH_VARIANT=$* -c $< -o $@
$(BUILD)/host-asan/harness/%.o: src/harness/%.c $(BUILD)/host-asan/flags
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTCF) $(ASANCF) $(BENCH_DEFS) -DBENCH_A=$(A) -DBENCH_B=$(B) -c $< -o $@
$(BUILD)/host-asan/bench-host: $(HA_HOBJS) $(HA_KOBJS)
	$(HOSTCC) $(ASANCF) $(filter %.o,$^) -lm -o $@

# ---- checks ---------------------------------------------------------------------

# $(call hostrun,<log>,<command...>): run, keep the output in <log>, print
# the screen summary; on failure print the whole log.
define hostrun
	@mkdir -p $(BUILD)/check
	@if $(2) > $(1) 2>&1; then \
		echo "ok    $(2)  [$$(grep '^host screen' $(1))]"; \
	else rc=$$?; cat $(1); echo "FAIL  (exit $$rc) $(2)  (log: $(1))"; exit 1; fi
endef

check: check-refs check-syms check-ab check-isa check-host
	@echo "make check: all checks passed"

# src/harness/refs.h was generated from the current kernel sources and headers
check-refs:
	$(PYTHON) tools/mkrefs.py --verify src/harness/refs.h

# Only instructions the Xenon PPE implements (testxenon3's objdump -M xenon
# allow-list) in the code of every image: catches Cell-only or AltiVec
# integer-multiply/sum encodings that would trap on the console.
check-isa: $(BUILD)/bench-ab.elf $(BUILD)/bench-$(A).elf $(BUILD)/bench-$(B).elf
	$(PYTHON) tools/check_syms.py isa --objdump $(X3)/bin/xenon-objdump $^

check-host: $(BUILD)/host/bench-host $(BUILD)/host/bench-host-single $(BUILD)/host-asan/bench-host
	$(call hostrun,$(BUILD)/check/asan.log,$(BUILD)/host-asan/bench-host --quick --no-color --cols 160 --rows 45)
	$(call hostrun,$(BUILD)/check/148x41.log,$(BUILD)/host/bench-host --quick --no-color --cols 148 --rows 41)
	$(call hostrun,$(BUILD)/check/74x27.log,$(BUILD)/host/bench-host --quick --no-color --cols 74 --rows 27)
	$(call hostrun,$(BUILD)/check/single-74x27.log,$(BUILD)/host/bench-host-single --quick --no-color --cols 74 --rows 27)
	@awk -F, 'FNR == 1 { f++ } $$1 == "kernel" && $$7 == "EXACT" { k = $$2 " " $$4; \
		if (f == 1) r[k] = $$22; \
		else if (!(k in r) || r[k] != $$22) { bad = 1; \
			print "FAIL  " k ": result " r[k] " (ASan, -O1) != " $$22 " (-O2): unspecified or undefined behaviour?" } } \
		END { exit bad }' $(BUILD)/check/asan.log $(BUILD)/check/148x41.log
	@echo "ok    EXACT results identical in the ASan (-O1) and -O2 host builds"
	@awk -F, '$$1 == "kernel" && $$29 != "ok" && $$29 != "byteorder" { bad = 1; \
		print "FAIL  " $$2 " " $$4 ": reference check \"" $$29 "\" (make refs records new ones)" } \
		END { exit bad }' $(BUILD)/check/148x41.log
	@echo "ok    every result matches its reference in src/harness/refs.h"

check-syms: $(A_KOBJS) $(B_KOBJS)
	$(PYTHON) tools/check_syms.py kernels --tc $(X3) --variant $(A) --header include/bench_lib.h $(A_KOBJS)
	$(PYTHON) tools/check_syms.py kernels --tc $(O16) --variant $(B) --header include/bench_lib.h $(B_KOBJS)

check-ab: $(BUILD)/bench-ab.elf32
	$(PYTHON) tools/check_syms.py map --map $(BUILD)/bench-ab.map \
		--libc $(X3)/xenon/lib/32/libc.a $(A):$(A_LIB) $(B):$(B_LIB)
	@$(X3)/bin/xenon-readelf -h -l $< | grep -E 'Class|Data:|Machine|Entry|LOAD' | sed 's/^ */  elf32: /'
	@$(X3)/bin/xenon-readelf -h $< | grep -q 'ELF32' && \
	 $(X3)/bin/xenon-readelf -h $< | grep -q 'big endian' && \
	 $(X3)/bin/xenon-readelf -l $< | grep -Eq 'LOAD +0x[0-9a-f]+ 0x00000000 ' || \
	 { echo "check-ab: $< is not the expected ELF32 BE image loaded at 0"; exit 1; }

# ---- reference results (src/harness/refs.h) --------------------------------------

# Records the big-endian results of a fresh qemu-check run as the references
# the images check every kernel against (tools/mkrefs.py).  A host run gives
# the descriptors and the little-endian results; its exit status is ignored,
# since stale references are what it may be reporting.
refs: qemu-check $(BUILD)/host/bench-host
	@mkdir -p $(BUILD)/refs
	-@$(BUILD)/host/bench-host --quick --no-color --no-frame > $(BUILD)/refs/host.log 2>&1
	$(PYTHON) tools/mkrefs.py --be $(BUILD)/qemu/qemu-run.txt --host $(BUILD)/refs/host.log \
		--known tools/qemu/known-failures.txt -o src/harness/refs.h

# ---- assembly and sizes ------------------------------------------------------------

asm: $(ASM)
	@echo "assembly in $(BUILD)/asm/$(A)/ and $(BUILD)/asm/$(B)/"

$(BUILD)/asm/$(A)/%.s: src/kernels/%.c $(BUILD)/$(A)/flags
	@mkdir -p $(@D)
	$(A_CC) $(filter-out -g,$(KCFLAGS)) -MF $(@:.s=.d) -MT $@ -DBENCH_VARIANT=$(A) \
		-S -fverbose-asm $< -o $@
$(BUILD)/asm/$(B)/%.s: src/kernels/%.c $(BUILD)/$(B)/flags
	@mkdir -p $(@D)
	$(B_CC) $(filter-out -g,$(KCFLAGS)) -MF $(@:.s=.d) -MT $@ -DBENCH_VARIANT=$(B) \
		-S -fverbose-asm $< -o $@

sizes: $(A_KOBJS) $(B_KOBJS)
	@$(PYTHON) tools/check_syms.py sizes --size-a $(X3)/bin/xenon-size \
		--size-b $(O16)/bin/xenon-size --label-a $(A_LABEL) --label-b $(B_LABEL) \
		$(BUILD)/$(A)/kernels $(BUILD)/$(B)/kernels

clean:
	rm -rf $(BUILD)

FORCE:

-include $(ALL_OBJS:.o=.d) $(ASM:.s=.d)

-include tools/qemu/qemu.mk
