# XTS - Xenon CPU test suite

A self-contained libxenon program for the Xbox 360 CPU (Xenon / Waternoose and successors). One ELF per profile is
loaded by XeLL from a USB stick; it generates every test on the console, checks results against a C reference model,
measures timing, and writes everything back to the same USB stick. No network, no PC in the loop.

The method and the first data come from OpenXenos `isa/hwtest` (PC-driven, 10,776 semantic cases in `results4`). XTS
moves the generation onto the console, adds an oracle (the reference model, SoftFloat for FP), a better timing method
(multi-depth least-squares fits with a quality gate), condition axes (32/64-bit mode, FP/vector modes, cache state,
alignment, SMT and multi-core load) and a resumable run journal.

## Quick start
```sh
make bootstrap                          # verify /usr/local/xenon (gcc 16, VMX128 gas, libxenon.a, libfat.a) and inputs
make PROFILE=smoke                      # -> build/xts-smoke.elf32 (and check_illegal on it)
make usb USB=/run/media/$USER/STICK     # copies it to STICK/xenon.elf; XeLL autoloads it
```
Boot the console into XeLL with the stick plugged in. The screen shows progress; when it says the run finished, power
off and read the stick:

```
xts/runs.txt                            run counter
xts/label.txt                           optional: your name for this console (otherwise <type>-<pvr>-<xenosid>)
xts/<console_tag>/current.txt           present while a run is unfinished (the next boot of the same ELF resumes it)
xts/<console_tag>/<run_id>/
    sysinfo.json        console identity: type, PVR + CPU name, PIR, HID0/1/4/6 (64-bit), PCI bridge rev, Xenos id,
                        DVE, RAM size, SMC version, NAND kind, speed code, TB Hz, temperatures, build id, git hash,
                        profile, ISA table sha256, model version. Never the CPU key, keyvault or fuse lines 3-6.
    run.log             human-readable log (also on screen)
    results.jsonl       one record per line (schema in CONTRIBUTING.md)
    results.csv         the same records as CSV (without detail)
    failures.jsonl      fail / hang / crash records with their full detail (state diffs)
    temps.csv           SMC temperatures after every block
    journal.txt         BEGIN/END per block (resume and hang detection)
    summary.json        status counts per family and per axis value
    sysinfo_boot<N>.json  identity at each resumed boot
```

## Profiles
| Profile | Contents |
|---|---|
| `smoke` | identity, harness self-test (`self.*`) and a capped sample of every `exec.*` family, in 64- and 32-bit mode (about 400 records). **Run this first.** |
| `smoke_crash` | a few blocks, then a deliberate crash outside the case (`self.crash`): checks the crash path, the journal `CRASH` line, crash-page survival and resume. Boot it twice. |
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
| `calib` | calib | TB Hz vs SMC RTC, mftb cost, noise floor, cycles per tick (sets the scale, journaled across reboots) |
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
| `speed` | default full code2 code3 code4 (changing the speed needs `SPEED_RISKY=1`) |
| `sibling` | idle parked busy_int busy_fp busy_vmx busy_mem same_insn |
| `other_cores` | idle l2_pressure mem_stream |

There is deliberately no `fpscr_ni` axis: FPSCR[NI] cannot be set on Xenon.

## Build options
| Option | Effect |
|---|---|
| `PROFILE=<p>` | which profile is linked (one ELF per profile) |
| `WATCHDOG=1` | hang watchdog on PIR 4: no progress for 120 s -> SMC reboot -> XeLL reloads `xenon.elf` -> the journal resumes. Unattended full runs. |
| `SPEED_RISKY=1` | lets the `speed` axis change the CPU speed code |
| `USB_FIX=0` | link libxenon's own USB mass-storage driver instead of the patched copy (`third_party/libxenon-usb`, default `USB_FIX=1`) |
| `BUILD=<dir>` | isolated output directory |

Changing an option rebuilds everything (flag stamp in `$(BUILD)/.options`).

## First console run
1. `make PROFILE=smoke && make usb USB=...` and boot. The startup screen shows the USB device, the volume label, free
   space and watchdog ON/OFF; the run ends with "run finished ... stick unmounted: safe to power off" (shown only after
   the final flush and unmount). On the stick, check that:
   - `summary.json` has `"write_errors":0` and `"guard_violations":0`;
   - every `self.*` record passed;
   - `run.log` has a `flush:` line per block (flush time + breadcrumb count/time) and, at the end, the
     `breadcrumbs (progress.bin, ...)` line with the per-kind cost and `run end: ... unmount`;
   - `crash.json` and `progress.bin` exist in the run directory (`crash.json` says `"state":"armed"`);
   - on a PC: `python3 tools/fat_check.py <image of the stick>` (or `fsck.fat -n /dev/sdX`) reports no errors.
2. `smoke_crash`: rows 0-5 of the screen show "XTS CRASH vector 0700 ...", the case (`self.crash` case 3 "crash"),
   "report saved to uda:/xts/<tag>/<run>/crash.json" and "stick is safe to reset". Copy the stick (or look at it on a
   PC): `crash.json` is valid JSON with `"state":"crashed"`, the registers, the case and the last 50 log lines, and
   `fat_check.py` is clean. Reset and boot the same ELF: it resumes, records the crash (`failures.jsonl`, detail
   `crash_report`), quarantines case 3, finishes and unmounts.
3. `semantic`: every `fail` is either a model bug or a new hardware fact. Divide by zero, 32-bit-mode carry/overflow,
   lmw/string ops and the d3d/vmsum gaps are model guesses waiting for this run; `failures.jsonl` has the state diffs.
4. `timing`: check the anchors (dependent add 2, fadd 10, vaddfp 12) and the `calib` cycles-per-tick record.
5. `sched`, `mem`, then `smt` (the sibling/park/other-core code has never run on hardware; watch for `lost` threads).

## Robustness
- **The stick is consistent whenever test code runs.** Records and log lines of the current block stay in RAM. At
  each block boundary XTS writes them, fsyncs every file and calls `fatFlush` (both FAT copies, FSInfo, every dirty
  cache page) - only then do the next block's cases start. While cases run no file is written at all: a sink, journal
  or file call attempted then is refused, counted (`summary.json` `guard_violations`) and logged as `BUG:`. If a
  block's buffered output grows past 1.5 MB, the chunk is written at the next case boundary (journal `PART`).
- **Report files**, created per run at a boundary while storage is healthy and then located on the device:
  - `crash.json` (64 KB): valid JSON at all times. Every 512-byte sector starts with `"slot_NNN":"<16-char magic>"`
    and is padded with spaces; the crash path only rewrites sectors that still hold the magic. `"state":"armed"` =
    nothing happened; `"state":"crashed"` = a report: `kind` (`main` = PIR 0, `thread` = another hardware thread),
    `block`, `case_index`/`case_rel`/`case_id`, `phase`, `outcome`, `vector`, `srr0`, `srr1`, `dar`, `dsisr` (0x300 /
    0x600), `lr`, `ctr`, `cr`, `xer`, `pir`, `r0`..`r31`, family / mnemonic / seed, the last executed `word` with its
    input registers `in_r0`.. (self/semantic families), other threads `thr<N>_*`, storage state, `build_id`, and the
    last 50 run.log lines `log_0`... Ignore the `slot_NNN` members. Read it with any JSON parser.
  - `progress.bin` (one sector, also JSON): the breadcrumb, rewritten raw before every case of self/semantic families
    (every 16th case of timing/sched/mem/smt/calib families - `CFLAGS_EXTRA=-DXTS_CRUMB_EVERY_TIMING=<n>`, 0 = block
    start only; one verified sector write each, cost per family kind in `run.log` and `summary.json`): run, boot,
    block, case, family, mnemonic, `every`, seq, timebase. After a hang it names the case that was running (with
    `"every":1`; sparse breadcrumbs make the next boot re-run the block case by case first).
- **Crash path** (unhandled exception: libxenon's handler -> `crashdump` -> emergency stack): no libfat, no malloc, no
  stdio, never a flush or unmount. If the USB driver / storage code was in the middle of an operation, the stick is
  not touched ("report NOT saved ... check the stick"); otherwise the report goes into `crash.json` through the disc
  guard's verified raw sector writes, sector 0 last. The screen (rows 0-5) says where the report is and whether the
  stick is safe to reset. Crashes on other hardware threads are kept in RAM; PIR 0 writes them into `crash.json` at its
  next case boundary and records them at the block end.
- **Resume** (`journal.txt` has `BEGIN`/`END` per block, written at boundaries only): a `BEGIN` without `END` becomes a
  `crash` record (case named by `crash.json`) or a `hang` record (case named by `progress.bin`), with the report /
  breadcrumb in `detail`, and only that case is quarantined. A block with no named case is re-run once (`RETRY`) with a
  breadcrumb before every case, so a second hang names it. The next boot copies `crash.json` to
  `crash_boot<N>.json`, journals `REPORT tb=...` (never ingested twice) and re-arms both files.
- **Storage (see "Storage" below):** every sector libfat reads is read twice and compared, every sector it writes is
  read back; the first storage error stops all writing and the run, with "STORAGE ERROR ... stop using this stick until
  it has been checked" on screen. At start a probe file is written, read back and removed. If that fails the run halts
  with "FAT32 (MBR) USB stick required". Every I/O error is counted (`write_errors` in summary.json, first errors on
  screen). The run ends with `fatFlush` + `fatUnmount`; halts after storage set-up flush and unmount too when storage
  is healthy.
- **Inside a case:** a program or alignment interrupt is recorded and execution resumes after it. Words the model
  predicts would take a data-storage interrupt are never executed (a DSI is fatal). Memory words the model cannot
  simulate run only if their address is inside the scratch area or the big buffer. SMT load kernels (other hardware
  threads) only touch their own buffers: their addresses are range-checked before every launch (`src/smt/smt.c`).
- **Screen:** a status line at most once a second (block, family, counts, write errors, watchdog), plus errors.
  `run.log` holds the full log; everything on screen is also in `run.log` or the records, except what the crash path
  shows when it could not write safely.
- **Unattended runs:** build with `WATCHDOG=1`. The watchdog is kicked after every case and every execution, with a
  timeout of 120 s (600 s for mem/smt families). On a stall it reboots through the SMC, XeLL reloads `xenon.elf`, and
  the journal resumes.

## Storage
A smoke run once crashed and corrupted the FAT32 stick it wrote to (a directory cluster partly overwritten by another
cluster's data and a USB command block, FAT #1 != FAT #2, the new `xts` entry pointing to a free cluster). The storage
path is now:

```
core sinks / journal --stdio--> libfat (third_party/libfat, patched) --> disc guard (src/plat/disc_guard.c) --> USB driver (third_party/libxenon-usb, patched)
```
- **USB driver** (`USB_FIX=1`): transfer errors and short transfers trigger bulk-only reset recovery and a fresh command
  instead of continuing with stale state; a halted transfer no longer executes `trap` (a crash inside the driver);
  retries are bounded. Details and the bug analysis: `third_party/libxenon-usb/XTS-PATCHES.md`.
- **libfat**: a failed cache-page load drops the page; FAT #2 is kept in sync; FSInfo is written on every sync; a failed
  FAT read is no longer taken for a free cluster; deleted directory slots are reused correctly; FAT pages are written
  before directory/data pages. Details: `third_party/libfat/XTS-PATCHES.md`.
- **Disc guard**: double read + compare, write + read-back; the first failure latches "poisoned": no further write
  reaches the stick, XTS stops, the screen says STORAGE ERROR. `run.log` ends with the guard's numbers and
  `summary.json` has a `"storage"` object (sectors read/written, retries, time spent).
- The stick is mounted once, as before named `uda:` (`udb:`/`udc:` for other ports); `sysinfo.json` records
  `usb_driver`, `disc_guard` and `usb_sector_size`.

After a STORAGE ERROR, or a crash whose screen says the stick may need a check, check it on a PC before using it again:
Windows `chkdsk X: /f`, Linux `fsck.fat -n /dev/sdX` (read-only), or make an image (`dd if=/dev/sdX of=stick.img`,
read-only on the stick) and run `python3 tools/fat_check.py stick.img`. Doing that after the first runs of a new build
is a good habit anyway.

PC-side tools: `tools/fat_mkimage.py` (FAT32 images like the stick, superfloppy or MBR, optionally pre-populated),
`tools/fat_check.py` (read-only checker: FAT copies, chains, cross-links, lost clusters, FSInfo, pre-existing files
byte-identical), `make host-fatreplay` (the storage stack on images with injected USB faults, upstream vs patched).

## Host side
```sh
make host-test                     # unit tests incl. resume after simulated hangs/crashes (forked "boots"; FAT images)
make host-dryrun PROFILE=semantic  # run a profile on the PC: every case, every kernel encoded and decoded back
make host-fatreplay                # the console's storage stack on FAT32 images with injected USB faults (a few minutes)
XTS_HOST_STORAGE=image:stick.img build/host/xts-dryrun-smoke   # a dry run writing through libfat into an image
make check                         # check_encoder (all 484 insns vs xenon-as), check_model (results4), check_illegal
python3 tools/xts_merge.py <run dirs...> -o merged/        # merge runs from one or more consoles
python3 tools/xts_schedmodel.py merged/merged.jsonl -o model/ --td   # scheduling model (JSON/CSV + LLVM .td sketch)
```
`BUILD=build/<name>` isolates all outputs (parallel builds never collide). `DEVKITXENON` from the environment is
ignored (a stale `/sdk/xenon` is common); pass it on the make command line to override `/usr/local/xenon`.
`LIBXENON_SRC` (default `~/Projects/libxenon/libxenon`) provides `drivers/ppc/except.S`, which is copied into
`$(BUILD)/gen` with two patches (SF=1-safe crashdump address; `ex_program` exported). `OPENXENOS` (default
`~/Projects/OpenXenos`) provides results4 and its plan generator for `check_model` (read only).

## Layout
```
include/xts/      public interface (frozen for Phase B; see CONTRIBUTING.md)
src/core/         run driver, registry, scheduler, sinks, journal, sysinfo, crash reports     (framework)
src/exec/         state image, slot/vectors, kernel JIT, semantic oracle, big buffer           (framework)
src/enc/          encoder/decoder over the generated table                                     (framework)
src/timing/       measurement engine                                                           (framework)
src/plat/         console (libxenon) and host platform layers                                  (framework)
src/model/        reference model, one file per instruction family
src/tests/        test families, one file each
src/conds/        condition axes, one file each
src/smt/          sibling-thread / other-core control, watchdog
profiles/         one file per profile
tools/            gen_isa.py, gen_slot.py, check_*.py, xts.lds, bootstrap.sh, usb_install.sh
host/             host-only programs (enc CLI, model replay, dry run, unit tests)
isa/              vendored xenon_isa.json + SOURCE
third_party/      SoftFloat 3e with a PowerPC specialization (XTS-PowerPC.md); libfat and the libxenon USB storage
                  driver, vendored with patches (XTS-PATCHES.md in each)
```
