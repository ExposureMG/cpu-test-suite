# XTS - Xenon CPU test suite.
#
#   make [PROFILE=smoke] [BUILD=build]     console ELF: $(BUILD)/xts-$(PROFILE).elf32 (+ check_illegal on it)
#   make host-test [BUILD=...]             host unit tests (registry, scheduler, sink, transport end to end through
#                                          tools/xts_recv.py, fit, encoder, model) + xts_recv.py --selftest
#   make host-dryrun PROFILE=<p>           run profile <p> on the PC: every block/case, every kernel encoded + decoded back;
#                                          the results travel as UDP to tools/xts_recv.py -> $(BUILD)/dryrun-<p>/run/
#   make check [BUILD=...]                 check_draws (RNG draw order), check_encoder (vs xenon-as), check_model (vs results4),
#                                          check_illegal, check_records
#   make recv [OUT=xts-runs]               receive a console run (tools/xts_recv.py: listens on UDP 36360, writes
#                                          OUT/<console_tag>/<run_nonce>/); RECV_ARGS="--keep-listening" for several runs
#   make usb USB=/run/media/<you>/<stick>  copy the ELF to <stick>/xenon.elf (XeLL autoloads it) and unmount; nothing else
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
SRC_HASH := $(shell cat $(sort $(ALL_SRCS) $(CON_ASM) $(wildcard include/xts/*.h src/*/*.h profiles/*.c tools/*.py quarantine.txt)) isa/xenon_isa.json 2>/dev/null | sha256sum | cut -c1-12)
BUILD_ID := $(GIT_HASH)-$(SRC_HASH)

# ---------------------------------------------------------------------------------------------------- options
# CFLAGS_EXTRA: extra compiler flags for every object, e.g. -DXTS_CRUMB_EVERY_TIMING=4 (breadcrumb cadence of the
#   non-self/semantic families, src/core/main.c).
OPT_DEFS :=
# Network destination of the results (console builds):
#   NET_DEST=            (default) UDP broadcast to every host on the LAN - fine when the receiver is wired.
#   NET_DEST=auto        unicast to THIS PC: its IPv4 address and MAC are read now from the default-route interface.
#   NET_DEST=<a.b.c.d> NET_DEST_MAC=<aa:bb:cc:dd:ee:ff>   unicast to another receiver.
#   Unicast is strongly recommended when the receiver is on Wi-Fi: unicast frames get link-layer retries, broadcast
#   frames do not. The console resolves the MAC by ARP (the compiled-in MAC is used only by NET_DRIVER=own).
# NET_FPS: frame copies per second (default 2000 broadcast, 1000 unicast). NET_COPIES: copies per frame (default 2).
NET_DEST ?=
NET_DEST_MAC ?=
ifeq ($(NET_DEST),auto)
  NET_IFACE := $(shell ip -4 route show default 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($$i=="dev"){print $$(i+1); exit}}')
  NET_DEST_RESOLVED := $(shell ip -4 -o addr show dev $(NET_IFACE) 2>/dev/null | awk '{split($$4,a,"/"); print a[1]; exit}')
  NET_DEST_MAC := $(shell cat /sys/class/net/$(NET_IFACE)/address 2>/dev/null)
else
  NET_DEST_RESOLVED := $(NET_DEST)
endif
ifneq ($(NET_DEST_RESOLVED),)
  ifeq ($(NET_DEST_MAC),)
    $(error NET_DEST=$(NET_DEST) needs NET_DEST_MAC=aa:bb:cc:dd:ee:ff (or use NET_DEST=auto on the receiving PC))
  endif
  NET_IP_HEX := $(shell echo $(NET_DEST_RESOLVED) | awk -F. 'NF==4{printf "0x%02X%02X%02X%02Xu", $$1,$$2,$$3,$$4}')
  NET_MAC_C := $(shell echo $(NET_DEST_MAC) | awk -F: 'NF==6{printf "0x%s,0x%s,0x%s,0x%s,0x%s,0x%s", $$1,$$2,$$3,$$4,$$5,$$6}')
  ifeq ($(NET_IP_HEX),)
    $(error NET_DEST=$(NET_DEST_RESOLVED) is not an IPv4 address)
  endif
  ifeq ($(NET_MAC_C),)
    $(error NET_DEST_MAC=$(NET_DEST_MAC) is not a MAC address)
  endif
  OPT_DEFS += -DXTS_NET_DEST_IP=$(NET_IP_HEX) '-DXTS_NET_DEST_MAC=$(NET_MAC_C)'
  NET_FPS ?= 1000
else
  NET_FPS ?= 2000
endif
NET_COPIES ?= 2
# NET_SETTLE_S: seconds to wait after the Ethernet link comes up before sending (default 10): the NIC bring-up resets
#   the PHY and many switches/routers do not forward for several seconds after a port's link returns.
NET_SETTLE_S ?= 10
OPT_DEFS += -DXTS_NET_SETTLE_S=$(NET_SETTLE_S)
# NET_DRIVER=own: XTS's own TX-only descriptor driver (default: libxenon's enet.c driver, proven on hardware).
NET_DRIVER ?=
ifeq ($(NET_DRIVER),own)
  OPT_DEFS += -DXTS_NET_OWN_DRIVER=1
endif
OPT_DEFS += -DXTS_NET_FPS=$(NET_FPS) -DXTS_NET_COPIES=$(NET_COPIES)
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
           -fno-strict-aliasing -DXTS_HOST $(COMMON_INC) -I$(SF)/platform/host $(HOST_SAN) $(OPT_DEFS) \
           $(CFLAGS_EXTRA)
SF_DEFS := -DSOFTFLOAT_FAST_INT64 -DSOFTFLOAT_ROUND_ODD -DINLINE_LEVEL=5 -DSOFTFLOAT_FAST_DIV32TO16 -DSOFTFLOAT_FAST_DIV64TO32
SF_INC := -I$(SF)/source/PowerPC -I$(SF)/source/include

# ---------------------------------------------------------------------------------------------------- generated code
GEN_HDRS := $(GEN)/isa_table.h $(GEN)/build_id.h $(GEN)/quarantine.h

$(GEN)/isa_table.c $(GEN)/isa_table.h &: tools/gen_isa.py isa/xenon_isa.json include/xts/isa.h
	@mkdir -p $(GEN)
	$(PYTHON) -B tools/gen_isa.py --json isa/xenon_isa.json --header include/xts/isa.h --out $(GEN)
	@touch $(GEN)/isa_table.c $(GEN)/isa_table.h

$(GEN)/slot.S: tools/gen_slot.py
	@mkdir -p $(GEN)
	$(PYTHON) -B tools/gen_slot.py > $@.tmp && mv $@.tmp $@

# the compiled-in quarantine list (src/core/main.c skips these cases)
$(GEN)/quarantine.h: quarantine.txt tools/gen_quarantine.py
	@mkdir -p $(GEN)
	$(PYTHON) -B tools/gen_quarantine.py quarantine.txt > $@.tmp && mv $@.tmp $@

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

# Objects before the archives: the patched except.o comes first, so libxenon.a(except.o) is not pulled in (a member that
# still got pulled in would collide and fail the link). The map/symbol checks fail the build if the console ELF contains
# any storage code: libfat, libxenon's USB stack (the console never touches USB; XeLL loads xenon.elf), or a file API.
CON_FORBIDDEN_OBJS := libfat|\((tinyehci|usbmass|usbmain|usbctrl|usbd|usbhub|usbhid|usbdevs|usb_os|ohci|ata|iso9660)\.o\)
CON_FORBIDDEN_SYMS := fatMount|fatInitDefault|fatUnmount|usb_init|usb_do_poll|USBStorage_[A-Za-z_]*|fopen|_fopen_r|xenon_ata_init
$(ELF): $(CON_OBJS) $(CON)/obj/profiles/$(PROFILE).o $(CON)/libsoftfloat.a tools/xts.lds
	$(XLD) -melf32ppc $(CON_OBJS) $(CON)/obj/profiles/$(PROFILE).o $(CON)/libsoftfloat.a \
	  --gc-sections -Map $(CON)/xts-$(PROFILE).map -L$(DEVKITXENON)/usr/lib -L$(DEVKITXENON)/xenon/lib/32 \
	  --start-group -lxenon -lm -lc $(XLIBGCC) --end-group -n -T tools/xts.lds -o $@
	@map=$(CON)/xts-$(PROFILE).map; \
	 if grep -q 'libxenon\.a(except\.o)' $$map; then echo "link map: libxenon.a(except.o) was pulled in" >&2; rm -f $@; exit 1; fi; \
	 bad=`grep -oE '$(CON_FORBIDDEN_OBJS)' $$map | sort -u | tr '\n' ' '`; \
	 if [ -n "$$bad" ]; then echo "link map: storage/USB objects linked: $$bad" >&2; rm -f $@; exit 1; fi; \
	 bad=`$(XPREFIX)nm $@ | grep -E ' [TtWw] ($(CON_FORBIDDEN_SYMS))$$' | awk '{print $$3}' | tr '\n' ' '`; \
	 if [ -n "$$bad" ]; then echo "console ELF defines storage/file symbols: $$bad" >&2; rm -f $@; exit 1; fi; \
	 echo "link map: no libfat, no USB stack, no file API in the console ELF"

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
HOST_PLAT_LINK = $(HOSTCC) $(HOST_SAN) -o $@ $(filter %.o,$^) $(filter %.a,$^) -lm

ENC_OBJS := $(HST)/obj/src/enc/enc.o $(HST)/obj/src/enc/isa.o $(HST)/gen/isa_table.o
MODEL_OBJS := $(patsubst %.c,$(HST)/obj/%.o,$(wildcard src/model/*.c)) $(HST)/obj/src/exec/state.o $(ENC_OBJS)

$(HST)/xts-enc-cli: $(HST)/obj/host/enc_cli.o $(ENC_OBJS)
	$(HOST_LINK)
$(HST)/xts-model-replay: $(HST)/obj/host/model_replay.o $(MODEL_OBJS) $(HST)/libsoftfloat.a
	$(HOST_LINK)
$(HST)/xts-host-test: $(HST)/obj/host/test_main.o $(patsubst %.c,$(HST)/obj/%.o,$(HOST_TEST_SRCS)) $(HST_OBJS) $(HST)/libsoftfloat.a
	$(HOST_PLAT_LINK)
$(HST)/xts-dryrun-$(PROFILE): $(HST)/obj/host/dryrun_main.o $(HST_OBJS) $(HST)/obj/profiles/$(PROFILE).o $(HST)/libsoftfloat.a
	$(HOST_PLAT_LINK)

host-tools: $(HST)/xts-enc-cli $(HST)/xts-model-replay

# host tests start tools/xts_recv.py themselves (XTS_PYTHON / XTS_RECV) for the end-to-end transport tests
host-test: $(HST)/xts-host-test
	rm -rf $(BUILD)/host-test
	$(PYTHON) -B tools/xts_recv.py --selftest
	ASAN_OPTIONS=detect_leaks=0 XTS_PYTHON=$(PYTHON) XTS_RECV=tools/xts_recv.py $(HST)/xts-host-test --tmp $(BUILD)/host-test

# the dry run sends its results as UDP to a receiver on an ephemeral localhost port (xts_recv.py --exec sets
# XTS_NET_HOST for the child); the received run directory is $(BUILD)/dryrun-$(PROFILE)/run
host-dryrun: $(HST)/xts-dryrun-$(PROFILE)
	rm -rf $(BUILD)/dryrun-$(PROFILE)
	ASAN_OPTIONS=detect_leaks=0 XTS_QUIET=1 $(PYTHON) -B tools/xts_recv.py --once --quiet --port 0 \
	  --run-dir $(BUILD)/dryrun-$(PROFILE)/run --exec $(HST)/xts-dryrun-$(PROFILE)

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
	$(PYTHON) -B tools/check_records.py $(BUILD)/dryrun-$(PROFILE)/run $(if $(KEYS),--keys-baseline $(KEYS)) \
	  $(if $(filter smoke,$(PROFILE)),--max-records $(SMOKE_BUDGET))

# every RNG draw in its own full expression: C leaves the order of call arguments / operands open, and host gcc evaluates
# call arguments right to left where xenon-gcc goes left to right (the dry run would generate other case inputs)
check-draws: $(GEN_HDRS)
	$(PYTHON) -B tools/check_draws.py --selftest
	$(PYTHON) -B tools/check_draws.py --gen $(GEN) $(HST_SRCS) $(wildcard profiles/*.c)

check: check-draws check-encoder check-model check-illegal check-records

usb: $(ELF32)
	@test -n "$(USB)" || (echo "usage: make usb USB=/path/to/mounted/stick [PROFILE=...]" >&2; exit 1)
	tools/install_elf.sh $(ELF32) $(USB)

# TFTP boot: XeLL fetches xenon.elf over TFTP (point it at this PC with tftp_server=<ip> in kboot.conf). The server
# (tools/xts_tftp.py, library tftpy in a venv under $(BUILD)) answers ONLY "xenon.elf" with this profile's ELF, refuses
# every other name (XeLL also asks for NAND/XeLL update images) and every upload. Port 69 needs root: it runs via sudo.
VENV := $(BUILD)/venv
$(VENV)/.ok: tools/requirements.txt
	$(PYTHON) -m venv $(VENV)
	$(VENV)/bin/pip install -q -r tools/requirements.txt
	touch $@

serve: $(ELF32) $(VENV)/.ok
	@echo "serving $(ELF32) as xenon.elf on UDP port 69 (sudo)"
	sudo $(VENV)/bin/python -B tools/xts_tftp.py --elf $(ELF32)

# kboot.conf for the USB stick that makes XeLL fetch xenon.elf from THIS PC over TFTP. XeLL needs "#KBOOTCONFIG" as the
# first line and at least one kernel entry; an entry whose file is not on the stick falls back to TFTP from tftp_server.
# TFTP_SERVER=<ip> overrides the address (default: this PC's address on the default-route interface).
TFTP_SERVER ?= $(shell ip -4 -o addr show dev $$(ip -4 route show default | awk '{for(i=1;i<=NF;i++) if($$i=="dev"){print $$(i+1); exit}}') 2>/dev/null | awk '{split($$4,a,"/"); print a[1]; exit}')
kbootconf:
	@test -n "$(TFTP_SERVER)" || (echo "kbootconf: could not find this PC's address; use TFTP_SERVER=<ip>" >&2; exit 1)
	@mkdir -p $(BUILD)
	@printf '#KBOOTCONFIG\n; generated by make kbootconf - copy to the root of the USB stick (no xenon.elf on the stick)\ntftp_server=%s\ntimeout=0\nxts="xenon.elf"\n' $(TFTP_SERVER) > $(BUILD)/kboot.conf
	@echo "wrote $(BUILD)/kboot.conf (tftp_server=$(TFTP_SERVER)):"; cat $(BUILD)/kboot.conf

check-tftp: $(VENV)/.ok
	$(VENV)/bin/python -B tools/xts_tftp.py --selftest

OUT ?= xts-runs
recv:
	$(PYTHON) -B tools/xts_recv.py --out $(OUT) $(RECV_ARGS)

check-recv:
	$(PYTHON) -B tools/xts_recv.py --selftest

bootstrap:
	XTS_DEVKITXENON=$(DEVKITXENON) LIBXENON_SRC=$(LIBXENON_SRC) tools/bootstrap.sh

clean:
	rm -rf $(BUILD)

FORCE:
.PHONY: all host-tools host-test host-dryrun serve kbootconf check-tftp recv check-recv check check-draws check-encoder check-model check-illegal check-records usb bootstrap clean FORCE

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
