# XTS - Xenon CPU test suite

A self-contained libxenon program for the Xbox 360 CPU (Xenon / Waternoose and successors). One ELF per profile is
loaded by XeLL from a USB stick; it generates every test on the console, checks results against a C reference model,
measures timing, and sends everything to a PC as **UDP messages** over Ethernet (libxenon's network stack with DHCP,
on a dedicated core that never sends while a measurement runs).
**Nothing is ever written on the console:** the stick is only XeLL's boot medium, so nothing on the console can corrupt
it (two USB sticks were destroyed by the old on-console storage path; it has been removed completely).

The method and the first data come from OpenXenos `isa/hwtest` (PC-driven, 10,776 semantic cases in `results4`). XTS
moves the generation onto the console, adds an oracle (the reference model, SoftFloat for FP), a better timing method
(multi-depth least-squares fits with a quality gate), condition axes (32/64-bit mode, FP/vector modes, cache state,
alignment, SMT and multi-core load) and dedicated cores: core 0 runs only the tests, core 1 the suite, core 2 the
network transmitter (`include/xts/xcore.h`).

## Quick start
```sh
make bootstrap                          # verify /usr/local/xenon (gcc 16, VMX128 gas, libxenon.a) and inputs
make PROFILE=net_hello                  # -> build/xts-net_hello.elf32 (and check_illegal on it)
make usb USB=/run/media/$USER/STICK     # copies it to STICK/xenon.elf (nothing else) and unmounts the stick
make recv                               # on the PC, BEFORE booting: tools/xts_recv.py --out xts-runs
```
Connect the console's Ethernet port to the same network as the PC (the console gets its address by DHCP, like XeLL;
without DHCP it falls back to libxenon's static 192.168.1.99). Boot it into XeLL with the stick plugged in, or over
TFTP (below). The screen
shows the build id, the progress and finally that the run ended; the receiver prints a live progress line and, after
the run's END message, a one-line verdict.

### Booting over TFTP (no USB stick in the loop)
XeLL can fetch `xenon.elf` over TFTP. The PC serves it with `tools/xts_tftp.py`, built on the `tftpy` library, which
`make serve` installs into a virtual environment under `build/`.
```sh
make serve PROFILE=smoke NET_DEST=auto   # builds the ELF, then serves it as xenon.elf on UDP port 69 (asks for sudo)
make recv                                # in a second terminal: the result receiver
```
- **Point XeLL at this PC:** without configuration XeLL asks the DHCP server's address, which on most networks is the
  router. `make kbootconf` writes `build/kboot.conf`; copy it to the root of a USB stick. XeLL only reads it, and the
  stick must not hold a `xenon.elf`. The file looks like this:
  ```
  #KBOOTCONFIG
  tftp_server=192.168.4.101
  timeout=0
  xts="xenon.elf"
  ```
  XeLL requires `#KBOOTCONFIG` as the first line and at least one kernel entry. A file with only `tftp_server` stops
  at "No kernels found". The entry `xts="xenon.elf"` is not found on the stick, so XeLL fetches it over TFTP from
  `tftp_server`. Use `TFTP_SERVER=<ip>` to override the address.
- **Keep the address stable:** give the PC a DHCP reservation so the address in `kboot.conf` stays valid.
- **The server is read-only and answers one name:** it serves only `xenon.elf` (the selected profile's ELF, re-read
  for every request, so a rebuild needs no restart). Every other name gets "file not found": XeLL's TFTP loop also
  asks for NAND and XeLL update images. Every upload is refused. `make check-tftp` tests all of this.

### Receiver on Wi-Fi: build with `NET_DEST=auto`
Broadcast frames reach Wi-Fi clients at the lowest basic rate with no link-layer retries, so bursts get lost. The run
is then incomplete (the END counts show it). With `NET_DEST=auto` the console sends **unicast** UDP to this PC: its
IPv4 address is read at build time from the default-route interface and compiled in (with its MAC, used by
`NET_DRIVER=own`); the console resolves the MAC by ARP before sending. Wi-Fi then retries lost frames at the link layer.

| Option | Meaning |
|---|---|
| `NET_DEST=auto` | unicast to this PC |
| `NET_DEST=<ip> NET_DEST_MAC=<mac>` | unicast to another receiver |
| `NET_FPS=<n>` | frame copies per second (default 2000 broadcast, 1000 unicast) |
| `NET_COPIES=<n>` | copies of each frame (default 2) |
| `NET_DRIVER=own` | XTS's own raw TX-only driver instead of libxenon's stack (not seen to deliver frames on the bring-up network) |
| `NET_SETTLE_S=<n>` | `NET_DRIVER=own` / no-DHCP only: seconds to wait after link-up before sending (default 10) |

The startup screen shows which destination the ELF was built for. Sending happens only between measurements, so none
of these options affect the results.

## Output
`tools/xts_recv.py` (Python 3 standard library only) listens on UDP port 36360 on every interface, de-duplicates the
frames (each is sent twice), checks every CRC, reassembles the messages and writes one directory per run:
```
xts-runs/<console_tag>/<run_id>/        console_tag = <type>-<pvr>-<xenosid>, run_id = the run nonce (16 hex digits)
    sysinfo.json        console identity: type, PVR + CPU name, PIR, HID0/1/4/6 (64-bit), PCI bridge rev, Xenos id,
                        DVE, RAM size, SMC version, NAND kind, speed code, TB Hz, temperatures, build id, git hash,
                        profile, ISA table sha256, model version. Never the CPU key, keyvault or fuse lines 3-6.
    run.log             human-readable log
    results.jsonl       one record per line (schema in CONTRIBUTING.md)
    results.csv         the same records as CSV (without detail)
    failures.jsonl      fail / hang / crash records with their full detail (state diffs)
    temps.csv           SMC temperatures after every block
    summary.json        status counts per family and per axis value, plus the transport verdict
    crash.json          only if the test core crashed (CRASH report) or hung (HANG report)
    receive.json        loss accounting: complete, missing_seq ranges, crc_errors, duplicates, incomplete_messages,
                        per-type message counts vs END, rec_hash check, the last breadcrumb
    capture.bin         every frame as received (python3 tools/xts_recv.py --replay capture.bin rebuilds the directory)
```
This is the layout the old USB sink wrote, so `xts_merge.py`, `xts_schedmodel.py` and `check_records.py` work
unchanged. A run is **complete** only if its HELLO and END arrived, no frame is missing, the message counts and the
hash over every record match END. The verdict line says COMPLETE or INCOMPLETE (with the reasons) and END's status
(`complete`, `aborted`, `crashed`, `hang`); the exit status is 0 only for a complete run with status `complete`.

```sh
python3 tools/xts_recv.py --out xts-runs                    # one run, then exit (make recv)
python3 tools/xts_recv.py --out xts-runs --keep-listening   # several runs / consoles until Ctrl-C
python3 tools/xts_recv.py --quarantine-out quarantine.txt   # append a quarantine line for every CRASH/HANG
python3 tools/xts_recv.py --timeout 60                      # finish a run after 60 s of silence (no END)
```
Ctrl-C writes whatever has arrived (a run without END is INCOMPLETE). If the console hangs hard (the transmitter core
too), the last breadcrumb in `receive.json` names the case that was about to run.

## Profiles
| Profile | Contents |
|---|---|
| `net_hello` | bring-up 1: only the transmitter: HELLO, 100 LOG lines, END. Proves the NIC TX path and the receiver. |
| `cores` | bring-up 2: every core role; 10,000 remote no-op calls (latency), heartbeats, code sync across cores, SMT exit self-check, transport fragmentation (`self.cores`, `self.net`) |
| `smoke` | identity, harness self-test (`self.*`) and a capped sample of every `exec.*` family, in 64- and 32-bit mode (about 400 records) |
| `smoke_crash` | a few blocks, then a deliberate crash on the test core (`self.crash`): a crash record, the CRASH report (`crash.json`) and END `crashed` must arrive |
| `semantic` | every `exec.*` family x sf, then one-axis sweeps of `fpscr_rn`, `fpscr_en`, `vscr_nj`; `vmx128.regmap` x sf |
| `timing` | `calib`, `lat.*` (+ `code_align` sweep), `tput.*` |
| `sched` | `bypass.matrix`, `issue.pair`, `unit.conflict`, `ucode`, `ldst.lat` (+ `data_align`), `branch` (+ `code_align`), `sysops` |
| `mem` | `mem.hier`, `ldst.lat` (+ `cache`, `data_align`) |
| `smt` | `lat.*`, `tput.*`, `issue.*`, `unit.*`, `ucode` under each `sibling` value; `lat.*`/`tput.*` x `thread_prio`; `lat.*`, `mem.*`, `ldst.*` x `other_cores` |
| `full` | all of the above, in that order |

Rows with `.sweep = 1` vary one axis at a time and skip families that are not sensitive to it, on top of a
default-condition pass, so a full run stays tractable. Families and axes added later join automatically through the
globs. Agent estimates (unverified): `timing` about 7 minutes, `sched` a few minutes per condition vector; `full` is
dominated by the SMT sweeps.

## Test families
| Family | Kind | What it produces |
|---|---|---|
| `self.exec`, `self.kernel` | self | harness self-test: nop / `tw` / illegal word, JIT kernels, traps in kernels, code pointers, a timed add chain |
| `exec.ppc` | semantic | every user-mode ppc instruction the console executes (24 cases each, Rc/OE variants), traps by TO, XER/LR/CTR moves, branches via landing pads, lmw/stmw/string ops around r30, larx/stcx with a defined reservation |
| `exec.fpu` | semantic | scalar FP + FPSCR ops (axes `fpscr_rn`, `fpscr_en`) |
| `exec.altivec` | semantic | 137 AltiVec + 8 Cell ops (axis `vscr_nj`) |
| `exec.vmx128` | semantic | 75 VMX128 ops, v0-v127, full immediate ranges, targeted cases for every gap results4 left (FMA NaN pairs, vmsum structure, d3d pack/unpack) |
| `exec.illegal` | semantic | the 29 hardware-illegal words and the invalid forms (RA=0 and RA=RT separated), must raise cause 0x08 |
| `exec.est` | semantic | estimate ops (fres, frsqrte, vrefp, vrsqrtefp, vexptefp, vlogefp and 128 forms): structured sweeps recording exact output bits (table data) + architected bound check; tininess probes |
| `vmx128.regmap` | semantic | VD/VA/VB 0-127, VC128, VPERM128 field maps |
| `calib` | calib | TB Hz vs SMC RTC, mftb cost, noise floor, cycles per tick (sets the scale for the rest of the run) |
| `lat.chain` | timing | dependent latency for every timable op, **per source operand slot** |
| `tput.indep` | timing | reciprocal throughput curve for N = 1..16 chains, pipelining depth |
| `lat.data` | timing | data-dependent latency/issue by operand bin (int/mul/div/FP/vector, denormals, NJ) |
| `bypass.matrix` | sched | producer x consumer x operand-slot extra cycles, distance sweeps, mtctr/mtlr hide distances |
| `issue.pair` | sched | ordered dual-issue pairs against a verified 1-cycle filler, overlap +- error |
| `unit.conflict` | sched | shared-pipe detection (mixes at several ratios), unpipelined busy time, pipe-class graph |
| `ucode` | sched | microcoded/cracked detection per instruction variant: isolated cost and cost to neighbours |
| `ldst.lat` | sched | load-to-use per form and cache level, throughput, load-hit-store matrix, cross-file transfers, misalignment, reservations |
| `mem.hier` | mem | pointer chase 4K-64M, strides, L1/L2 associativity, streams, prefetch (dcbt/xdcbt), cache-op costs, ERAT/TLB reach |
| `branch` | sched | taken/not-taken, taken distance, mispredict, bclr/bcctr vs distance, link stack, loop overhead, target alignment, BHT aliasing |
| `sysops` | sched | compares and CR ops, SPR moves, FPSCR/VSCR ops, barriers, larx/stcx, Rc=1 cost, carry chains |

## Condition axes
| Axis | Values (first = default) |
|---|---|
| `sf` | 64, 32 |
| `fpscr_rn` | rn rz rp rm |
| `fpscr_en` | none ve oe ue ze xe all (enabled exceptions suppress/adjust results with MSR[FE]=0) |
| `vscr_nj` | 0 1 |
| `cache` | default cold l1 l2 mem |
| `code_align` | 0 4 8 16 32 64 (kernel offset within the 128-byte line) |
| `data_align` | aligned mis1 mis4 line_cross page_cross (4 KB) page64k_cross (libxenon page) |
| `thread_prio` | default low medium high |
| `unroll`, `chains` | depth/chain overrides for timing families |
| `speed` | default (read only: the speed code XeLL left is recorded, never changed) |
| `sibling` | idle parked busy_int busy_fp busy_vmx busy_mem same_insn |
| `other_cores` | idle l2_pressure mem_stream |

There is deliberately no `fpscr_ni` axis: FPSCR[NI] cannot be set on Xenon.

## Build options
| Option | Effect |
|---|---|
| `PROFILE=<p>` | which profile is linked (one ELF per profile) |
| `BUILD=<dir>` | isolated output directory |
| `CFLAGS_EXTRA=-DXTS_CRUMB_EVERY_TIMING=<n>` | breadcrumb cadence of timing/sched/mem/smt/calib families (default 16; 0 = block start only) |

Changing `CFLAGS_EXTRA` rebuilds everything (flag stamp in `$(BUILD)/.options`). `quarantine.txt` is compiled in
(`$(BUILD)/gen/quarantine.h`). The console link fails if libfat, libxenon's USB stack or a file API (`fopen`, ...)
ends up in the ELF.

## First console run
Start `make recv` (or `python3 tools/xts_recv.py --out xts-runs`) on the PC before each boot; one profile per boot,
power-cycle in between.
1. `net_hello`: the screen shows the DHCP address and `PC MAC resolved`; the receiver shows HELLO and END with
   **0 missing frames**, `LOG 100`, verdict COMPLETE. If nothing arrives: the console screen line (`PC MAC NOT
   resolved` = the PC did not answer ARP: wrong `NET_DEST` or a different subnet), the PC firewall (UDP 36360
   inbound), and `tcpdump -ni <if> udp port 36360` on the PC.
2. `cores`: COMPLETE, every `self.cores` / `self.net` record `pass`; the ping-pong latency and the heartbeats are in
   the records' detail.
3. `smoke`: COMPLETE, every `self.*` record passed, and the key set matches the host dry run
   (`make host-dryrun PROFILE=smoke`, `tools/check_records.py build/dryrun-smoke/run --write-keys smoke.keys`, then
   `tools/check_records.py <run dir> --keys-baseline smoke.keys`).
4. `smoke_crash`: the transfer is COMPLETE with END status **crashed**: a `crash` record for `self.crash` case `crash`,
   and `crash.json` with `"kind": "crash"`, `"vector": "0700"`, the registers, the case and the last 50 log lines. The
   receiver suggests the line `self.crash crash` for quarantine.txt - do not add that one (the profile exists to
   crash).
5. `semantic`: every `fail` is either a model bug or a new hardware fact. Divide by zero, 32-bit-mode carry/overflow,
   lmw/string ops and the d3d/vmsum gaps are model guesses waiting for this run; `failures.jsonl` has the state diffs.
6. `timing`: check the anchors (dependent add 2, fadd 10, vaddfp 12) and the `calib` cycles-per-tick record.
7. `sched`, `mem`, then `smt` (the sibling/park/other-core code has never run on hardware; watch for `lost` threads).

## Robustness
- **No storage on the console.** No file is opened, written or mounted; libfat and libxenon's USB stack are not even
  linked (the link map is checked). USB is touched only by XeLL, before XTS starts.
- **Transport** (`include/xts/net.h`, wire format `docs/PROTOCOL.md`): every record is one REC message (the exact
  results.jsonl line), every log line a LOG message, each block end a BLOCK message; HELLO x3 starts the run (identity,
  plan, core map, quarantine list), END x3 ends it (status, per-type counts, last sequence number, FNV-1a hash over the
  records). Frames are numbered, CRC-checked, sent twice and paced; only the transmitter core touches the NIC, it never
  starts a frame while the test core measures. It uses libxenon's DHCP + lwIP UDP; lwIP's receive processing (ARP, DHCP
  lease) runs only on that core and only between measurements, though the NIC may store incoming frames meanwhile.
- **Breadcrumbs:** before every case of self/semantic families (every 16th case of the timing-like families) the suite
  sends a CRUMB message and waits until it is on the wire before the case runs, so the PC always knows the case in
  flight.
- **Crash or hang of the test core:** the crash handler fills the test core's crash slot; a case that does not
  finish within its timeout (2 s self/semantic, 10 s timing/sched/calib, 60 s mem/smt) counts as a hang. The suite
  then sends a crash/hang record for the exact case, the CRASH/HANG report (registers, DAR/DSISR, the case and its input
  registers, the last 50 log lines; `crash.json` on the PC) and END with status `crashed`/`hang`, waits until all of it
  is sent and halts. Crashes of the other hardware threads (sibling / other-core loads) become `pir<N>` crash records
  at the block end and the run goes on.
- **No resume:** a known-bad case is skipped through the compiled-in **`quarantine.txt`** (`<family> <case_id>` per
  line; `xts_recv.py --quarantine-out quarantine.txt` appends the suggestion, then rebuild). Skipped cases are `skip`
  records saying "quarantined"; runs stay reproducible.
- **Inside a case:** a program or alignment interrupt is recorded and execution resumes after it. Words the model
  predicts would take a data-storage interrupt are never executed (a DSI is fatal). Memory words the model cannot
  simulate run only if their address is inside the scratch area or the big buffer. SMT load kernels (other hardware
  threads) only touch their own buffers: their addresses are range-checked before every launch (`src/smt/smt.c`).
- **Screen:** the build id at boot, a status line at most once a second (block, family, counts, messages sent), errors
  and the final state. Everything on screen is also in `run.log` or the records.

## Host side
```sh
make host-test                     # unit tests + end-to-end transport tests (real UDP to tools/xts_recv.py, loss /
                                   # duplicate / reorder / corruption injection, crash and hang reports) + receiver selftest
make host-dryrun PROFILE=semantic  # run a profile on the PC through the receiver: build/dryrun-semantic/run/
make check                         # check_encoder (all 484 insns vs xenon-as), check_model (results4), check_illegal,
                                   # check_records (on the received dry-run directory)
python3 tools/xts_recv.py --selftest
python3 tools/xts_merge.py <run dirs...> -o merged/        # merge runs from one or more consoles
python3 tools/xts_schedmodel.py merged/merged.jsonl -o model/ --td   # scheduling model (JSON/CSV + LLVM .td sketch)
```
Host transport knobs (environment of a host dry run): `XTS_NET_HOST=<ip>:<port>` (default `127.0.0.1:36360`, `off`),
`XTS_NET_COPIES`, `XTS_NET_FPS`, `XTS_NET_DROP=<rate>,<seed>` (+ `XTS_NET_DROP_LOG=<file>`), `XTS_NET_CORRUPT`,
`XTS_NET_REORDER=<window>,<seed>` (`src/plat/net_host.c`).
`BUILD=build/<name>` isolates all outputs (parallel builds never collide). `DEVKITXENON` from the environment is
ignored (a stale `/sdk/xenon` is common); pass it on the make command line to override `/usr/local/xenon`.
`LIBXENON_SRC` (default `~/Projects/libxenon/libxenon`) provides `drivers/ppc/except.S`, which is copied into
`$(BUILD)/gen` with two patches (SF=1-safe crashdump address; `ex_program` exported). `OPENXENOS` (default
`~/Projects/OpenXenos`) provides results4 and its plan generator for `check_model` (read only).

## Layout
```
include/xts/      public interface (see CONTRIBUTING.md); net.h + xcore.h: transport and core roles
src/core/         run driver, registry, scheduler, sink, in-RAM results store, wire format (netfmt), sysinfo,
                  breadcrumbs and crash/hang reports                                           (framework)
src/exec/         state image, slot/vectors, kernel JIT, semantic oracle, big buffer           (framework)
src/enc/          encoder/decoder over the generated table                                     (framework)
src/timing/       measurement engine                                                           (framework)
src/plat/         console (libxenon, NIC transmitter, core roles) and host platform layers     (framework)
src/model/        reference model, one file per instruction family
src/tests/        test families, one file each
src/conds/        condition axes, one file each
src/smt/          sibling-thread / other-core control
profiles/         one file per profile
quarantine.txt    cases skipped by every build (compiled in)
tools/            xts_recv.py (receiver), gen_isa.py, gen_slot.py, gen_quarantine.py, check_*.py, xts_merge.py,
                  xts_schedmodel.py, xts.lds, bootstrap.sh, install_elf.sh
host/             host-only programs (enc CLI, model replay, dry run, unit tests)
docs/             PROTOCOL.md (wire format)
isa/              vendored xenon_isa.json + SOURCE
third_party/      SoftFloat 3e with a PowerPC specialization (XTS-PowerPC.md)
```
