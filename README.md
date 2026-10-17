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
| `BUILD=<dir>` | isolated output directory |

Changing an option rebuilds everything (flag stamp in `$(BUILD)/.options`).

## First console run
1. `make PROFILE=smoke && make usb USB=...` and boot. The startup screen shows the USB device, the volume label, free
   space and watchdog ON/OFF; the run ends with "run finished". On the stick, check that:
   - `summary.json` has `"write_errors":0`;
   - every `self.*` record passed;
   - `run.log` has flush times.
2. `smoke_crash`: the screen shows "XTS CRASH vector 0700", and `journal.txt` ends with a `CRASH ... case=3` line. Reboot
   and boot the same ELF: it resumes, quarantines case 3 and finishes. `sysinfo_boot2.json` `crashpage_survived` says
   whether the crash page survives a reboot.
3. `semantic`: every `fail` is either a model bug or a new hardware fact. Divide by zero, 32-bit-mode carry/overflow,
   lmw/string ops and the d3d/vmsum gaps are model guesses waiting for this run; `failures.jsonl` has the state diffs.
4. `timing`: check the anchors (dependent add 2, fadd 10, vaddfp 12) and the `calib` cycles-per-tick record.
5. `sched`, `mem`, then `smt` (the sibling/park/other-core code has never run on hardware; watch for `lost` threads).

## Robustness
- **USB:** at start a probe file is written, read back and removed. If that fails the run halts with "FAT32 (MBR) USB
  stick required". The sinks and the journal stay open; each block ends with fflush + fsync. Every I/O error is counted
  (`write_errors` in summary.json, first errors on screen), and run.log gets the flush time per block.
- **Journal:** `journal.txt` gets `BEGIN`/`END` per block. A boot that finds a `BEGIN` without an `END` names the case
  from (in order):
  1. the crash page;
  2. a `CRASH` line the crash path wrote;
  3. the last `CASE` line.
  It then quarantines only that case. A block with no named case is re-run once (`RETRY`) with a `CASE` line before every
  case, so a second hang names it.
- **Crash path:** an interrupt outside the case being run goes through libxenon's handler to `crashdump`. There it gets a
  per-thread emergency stack and its r2/r13. It writes the crash page (EA 0x9D000000, clear of XeLL's load area) and,
  when no file operation was in progress, a journal `CRASH` line. Crashes on other hardware threads get their own slots.
  `sysinfo.json` records whether the page survived the reboot (`crashpage_survived`).
- **Inside a case:** a program or alignment interrupt is recorded and execution resumes after it. Words the model
  predicts would take a data-storage interrupt are never executed (a DSI is fatal). Memory words the model cannot
  simulate run only if their address is inside the scratch area or the big buffer.
- **Screen:** a status line at most once a second (block, family, counts, write errors, watchdog), plus errors.
  `run.log` holds the full log.
- **Unattended runs:** build with `WATCHDOG=1`. The watchdog is kicked after every case and every execution, with a
  timeout of 120 s (600 s for mem/smt families). On a stall it reboots through the SMC, XeLL reloads `xenon.elf`, and
  the journal resumes.

## Host side
```sh
make host-test                     # unit tests incl. journal resume with simulated hang/crash (forked "boots")
make host-dryrun PROFILE=semantic  # run a profile on the PC: every case, every kernel encoded and decoded back
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
src/core/         run driver, registry, scheduler, sinks, journal, sysinfo, crash page        (framework)
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
third_party/      SoftFloat 3e with a PowerPC specialization (XTS-PowerPC.md)
```
