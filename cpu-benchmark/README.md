# LibXenon CPU benchmark

Measures how fast code built by two Xbox 360 (Xenon) GCC toolchains runs on
the same console, and prints a side-by-side comparison on the framebuffer
when the run finishes.

| slot | variant id | label | toolchain | default CPU |
|---|---|---|---|---|
| A | `xenon3` | testxenon3 | `$(X3)` | `-mcpu=xenon` (VMX128, Xenon cost model) |
| B | `old16` | test16old | `$(O16)` | `-mcpu=cell` (generic PPC64 + Xenon tweaks) |

The main deliverable, `build/bench-ab.elf32`, contains every kernel twice
(compiled by A into the `xenon3_*` namespace and by B into `old16_*`) plus
each toolchain's own newlib routines for the LIB kernels.  It runs kernel
*i* of A and kernel *i* of B back to back on the same boot, the same memory
and the same input bytes, so boot-to-boot and thermal variation cancel out.
DESIGN.md is the full specification.

## Build

Requirements: both toolchains (paths in the Makefile, override with
`make X3=... O16=...`), GNU make, python3, a host gcc for the host build.

    make            # = make ab single
    make ab         # build/bench-ab.elf32        A and B in one image (use this)
    make single     # build/bench-xenon3.elf32, build/bench-old16.elf32
    make host       # build/host/bench-host       everything for the host
    make check      # host ASan run + host runs at 148x41 and 74x27 + object checks
                    #   + link-map and code-placement checks of the A/B image
                    #   + Xenon instruction-set scan of the three images
    make asm        # build/asm/<variant>/<kernel>.s for both variants
    make sizes      # .text bytes per kernel, A vs B
    make qemu-check # big-endian run of the kernels under qemu-ppc (tools/qemu/)
    make refs       # qemu-check, then record its results in src/harness/refs.h
    make clean

Variables:

* `OPT` (default `-O2`): optimisation flags for the kernels of both
  variants.  The harness is always `-O2 -fno-tree-vectorize`: it is the
  referee, so its guard and timing loops stay plain scalar code.
* `PLACE_ALIGN` (default 16384): every kernel object's and every newlib
  routine's code and tables start on this boundary (see "Code placement"
  below); 0 links them packed.
* `SHIFTS` (default `0`): code layout shifts in bytes, for example
  `make SHIFTS="0 4 8 12"`.  `bench-ab.elf32` (and the host build) then
  contain every kernel and newlib routine once per shift, with its code
  starting that many bytes past its `PLACE_ALIGN` boundary, and the run
  measures A and B at every shift (see "Layout shifts" below).  Multiples
  of 4 below `PLACE_ALIGN`, at most 8 values.  The default builds and runs
  what it did before this option existed.
* `BENCH_DEFS`: extra harness defines, for example
  `make BENCH_DEFS="-DBENCH_SAMPLES=9 -DBENCH_SAMPLE_MS=100"`
  (defaults: 7 samples of about 60 ms per kernel and variant).
* `MACHDEP`: the libxenon machine flags.

Changing a variable rebuilds whatever depends on it (flag stamps in
`build/*/flags`).  Kernels listed in `src/kernels/list_*.h` whose source
does not exist yet are linked as placeholders and shown as MISSING, so the
images always link; `make check` fails until every kernel exists and passes.

The single-toolchain images are fallbacks built with one toolchain end to
end (harness, libxenon, newlib).  They print one column per kernel.

## Run on the console

Load `build/bench-ab.elf32` from XeLL like any other libxenon program.  The
program initialises video (`VIDEO_MODE_AUTO`), sets the CPU to full speed
(`xenon_make_it_faster`), prints one progress line per kernel, then clears
the screen and draws the report.  With 33 kernels the whole run takes about
30 to 60 seconds; drawing the report takes a few more seconds (the libxenon
console flushes the framebuffer after every character).

The program never exits: returning from `main` would make libxenon reload
XeLL and wipe the screen.  Read the screen, then power the console off.

Progress lines look like

    [ 7/33] bsearch  BR   A   812.3 us  B   905.1 us   1.114x  ok

and are also sent to the serial port.  With layout shifts the times and
the speedup are medians over the shifts, followed by the spread and the
layout marker (see "Layout shifts"):

    [ 7/33] bsearch  BR   A   812.3 us  B   905.1 us  1.114x  1.7%*  ok
  A crash or hang leaves the name of
the kernel that was running on the screen.  A failed check adds a line with
the results, for example

    [22/33] sad16    VEC  A   ...   us  B   ...   us   ...     WRONG B
            A 0x95dd045c  B 0x00000000  reference 0x95dd045c

Measurement conditions: thread 0 runs everything with interrupts off.
`xenon_make_it_faster` leaves the other five hardware threads in libxenon's
low-priority polling loop, as in every libxenon program; thread 1 shares
core 0 and takes a few dispatch slots.  That is the same for A and B (and is
the "idle sibling" state testxenon3's `-mxenon-smt` cost model assumes), so
it lowers absolute numbers slightly but not the speedups.

### Serial log

After the report is drawn, everything is written to the UART only
(115200 baud, the libxenon `putch` port), as CSV:

    #bench,version,mode,platform,cpu,tb_hz,cycles_per_tick,...
    bench,1.0,ab,Xenon,PVR 00710800; speed 1 (full),49875000,64,7,60,0,33,0,41.234,
    #screen,layout,cols,rows
    screen,wide,148,41
    variant,A,xenon3,testxenon3,gcc 16.2.0,xenon+vmx128,-O2
    #kernel,name,category,slot,variant,status,check,tolerance,units,unit,...
    kernel,crc32,INT,A,xenon3,ok,EXACT,0,65536,B,...,sample_ticks
    geomean,INT,1.123456,5
    geomean,ALL,1.143000,33
    #end

One `kernel` line per kernel and variant, with min/median/max ticks per
`run()`, us/run, cycles/unit, Munits/s, noise, the result value (and hex for
EXACT kernels), the number of calls whose result differed, the speedup,
every sample, and the reference check: `ref` is `ok` or `WRONG` for this
variant (or `none`, `stale`, `byteorder` when there was nothing to compare
with) and `ref_value` the reference result.  Lines starting with `#` are
headers.

With layout shifts (`SHIFTS` with more than one value) the log has, in
addition, and only then:

    #shifts,count,bytes,layout_sensitive,flips,layout_pct
    shifts,4,0;4;8;12,3,1,1
    #kernel,name,...,ref,ref_value,shift
    kernel,crc32,INT,A,xenon3,ok,EXACT,...,ok,779898862,0
    ...
    kernel,crc32,INT,B,old16,ok,EXACT,...,ok,779898862,12
    #layout,name,category,status,shifts,speedup_median,speedup_min,speedup_max,spread_pct,noise_pct,band_pct,flip,sensitive,a_ticks,b_ticks,speedups
    layout,bsearch,BR,ok,4,1.114000,1.102000,1.121000,1.724,0.120,1.000,0,1,38963.10,43404.90,1.102000;1.113000;1.115000;1.121000
    #geomean,category,speedup,kernels
    geomean,ALL,1.143000,33
    #geomean_shift,shift,speedup,kernels
    geomean_shift,0,1.141000,33

* `shifts`: the number of shifts, their offsets in bytes (`;`-separated, in
  measurement order), how many passed kernels are layout-sensitive and how
  many of those flip, and `BENCH_LAYOUT_PCT` (see "Layout shifts").
* `kernel` lines: one per kernel, shift and variant (shift by shift, A then
  B), with one more field at the end, `shift` (bytes).  Every other field
  keeps its meaning for the copy measured at that shift: `speedup` is that
  shift's B/A, `samples`/`sample_ticks` that copy's samples, `calls` and
  `bad_calls` that copy's calls (A at the first shift also counts the
  calibration calls).  `status`, `reps`, `kernel_ms` (all shifts) and the
  reference fields are per kernel and repeat on each line; `result` is
  the result every copy must return (the first shift's; a copy that
  returned anything else has `bad_calls` > 0 and makes the kernel
  UNSTABLE).
* `layout` lines: one per kernel: `speedup_median` (the kernel's speedup),
  `speedup_min`/`speedup_max` and `spread_pct` = (max / min - 1) x 100 over
  the shifts, `noise_pct` (the largest noise of any copy), `band_pct`,
  `flip` and `sensitive` (0/1, see "Layout shifts"), `a_ticks`/`b_ticks`
  (the times shown on screen: the median over the shifts of each shift's
  minimum, ticks per run) and `speedups`, the speedup at each shift in the
  order of the `shifts` line.
* `geomean` lines keep their format; their speedup is the geomean of the
  per-kernel medians.  `geomean_shift` lines give the geomean at each
  shift over the same kernels.

## Reading the screen

The layout adapts to the console size (`console_get_dimensions`, after
overscan): 720p is about 160x45 (148x41 with overscan), 480p about 80x30
(74x27).

* **wide** (720p and larger): one kernel per line with category,
  description, A and B us/run, A and B cycles per unit (timebase ticks x 64
  / units), speedup with a bar centred on 1.0, noise and check status;
* **compact** (480p): two kernels per line with A us, B us and the speedup
  (or the failure word);
* **minimal**: smaller screens, overall result and speedups only.

With layout shifts the speedup column is the median over the shifts, the
us and cyc/u columns are the medians over the shifts of each shift's time
(so B us / A us need not equal the speedup exactly), and a `*` follows the
speedup of a layout-sensitive kernel (`!` when the faster toolchain
depends on the shift).  wide adds one speedup column per shift (`+0`,
`+4`, ...; a `min-max` column instead when they do not fit) and the
`spread` column, yellow for layout-sensitive kernels; the description and
the bar get what room is left.  The footer adds a "by shift" line with the
geomean at each shift and the number of layout-sensitive kernels.

Header: title, one line per toolchain (label, compiler, default CPU, kernel
flags), run information (kernels, samples, run time), CPU PVR and speed,
timebase.  Footer: geometric mean of the speedups per category and over all
kernels ("testxenon3 is 1.143x faster than test16old (geomean of 33
kernels)"), the failure count, and a legend.

* **speedup = B time / A time**: above 1 means testxenon3's code is faster.
  Green above 1.01, red below 0.99.
* **time** is the fastest of the samples (each sample is many back-to-back
  `run()` calls timed as a whole); **noise** is (median - min) / min, yellow
  above 3 %.
* **cyc/u**: CPU cycles per work unit (byte, element, pixel ... the `unit`
  column), at 3.2 GHz = 64 cycles per timebase tick.  The microseconds and
  cycles assume the CPU runs at full speed; the header shows the speed
  `xenon_get_speed()` reports, in yellow when it is not full.  The speedups
  are ratios measured on the same clock either way.

Kernels that fail a check are shown in red and are left out of the
geomeans.  "ok" means A and B agree and both match the kernel's reference
result (the legend says so, or how many kernels had one):

| status | meaning |
|---|---|
| WRONG A, WRONG B, WRONG AB | that variant's result differs from the reference result in `src/harness/refs.h` (same rule as MISMATCH); single images say WRONG |
| MISMATCH | A and B results differ (EXACT: not bit-identical; APPROX: beyond the kernel's relative tolerance) |
| UNSTABLE | a `run()` call returned a different value than the first call of the same variant |
| OVERRUN | the 4 KB guard pattern before the arena or after the kernel's `arena_bytes` was damaged |
| BADDESC | invalid descriptor, or the A and B descriptors differ (the kernel is not run) |
| MISSING | the kernel's source did not exist when the image was built |
| NOTIMER | the timebase did not advance |

When several apply, the word shown is the first of MISSING, BADDESC,
NOTIMER, OVERRUN, UNSTABLE, WRONG, MISMATCH; the CSV `status` field lists
them all.

### Reference results

Every image checks each variant's result against a reference: the result
both toolchains' kernels compute big-endian under qemu-ppc
(`make qemu-check`), recorded in `src/harness/refs.h` by `make refs`
(`tools/mkrefs.py`).  EXACT kernels compute the same integers on any
PowerPC, so they must match bit for bit; APPROX kernels must match within
their tolerance.  This matters because only testxenon3's `init` runs and
eight of them are VMX128-vectorised code that no emulator has executed: a
miscompiled init would give A and B the same wrong input, and the A/B
comparison alone could not see it.

* WRONG B (or WRONG A): that variant computed a wrong result.
* WRONG AB: both are wrong in the same way; suspect the shared init data,
  i.e. testxenon3's init code on the hardware.
* The reference records the kernel's descriptor; after a kernel changes,
  `make check` fails (`tools/mkrefs.py --verify` hashes the kernel sources
  and the shared headers) until `make refs` records new results, and the
  CSV shows `stale` for an outdated entry.  bswap decodes little-endian
  records into native structs, so its result depends on the byte order:
  the host build skips it (`byteorder`), the console checks it.

### Code placement

Code placement alone moves single-kernel timings by several percent, which
is as large as the effects being measured: on the host, A and B are the same
code from the same compiler, yet the packed A/B image showed mandel at
1.064x and divmod at 0.946x purely from where the linker put each copy.  So
every kernel object's and newlib routine's `.text` and `.rodata` start on a
16 KB boundary (`PLACE_ALIGN`, `tools/place.py`), the way size of Xenon's
2-way 32 KB L1 instruction cache, which also covers its 4K-entry branch
history table.  Identical code then gets identical cache sets, branch
history entries and fetch alignment in A and B whatever was linked before
it; what remains is each compiler's own code layout.  On the host the
identical-code speedups went from 1.9 % rms (4 kernels beyond 2 %) to about
1 % rms, which is the host's run-to-run noise (the same binary run twice
differs that much; mathapx alternates between about 0.96x and 1.05x
whichever copy runs as A).  The console has a fixed clock and nothing else
running, so its noise column should be much lower: read speedups against
it.  The padding makes `bench-ab.elf32` about 4 MB.

### Layout shifts

Identical placement makes A and B comparable, but it is still *one*
placement: where a loop or a branch lands relative to the fetch blocks,
the 128-byte cache lines and the branch history table indices can favour
one compiler's code by a few percent by accident (the first console runs
showed 3-5 % in branchy kernels that the compilers' loop alignment did not
explain).  `make SHIFTS="0 4 8 12"` measures every kernel at several
placements and separates the two effects:

* Each kernel object and newlib routine is linked once per shift, its code
  starting `shift` bytes past its `PLACE_ALIGN` boundary, the same for A
  and B (`tools/place.py --shift` puts that many no-ops in front of the
  code; tables stay on the boundary).  The copy at shift S has its own
  symbol namespace (`xenon3_s4_k_crc32`, `old16_s4_lib_memcpy`); the copy
  at shift 0 is the one the default image has.  `make check` verifies in
  the link map that every copy starts at boundary + shift
  (`check_syms.py place`).
* Each kernel is initialised once; every sample then goes round the shifts,
  A and B of one shift back to back (alternating AB/BA as before), with
  the same `reps`.  Every copy must return the same result as the first
  shift's (else UNSTABLE).
* The kernel's **speedup is the median of its per-shift speedups**, and the
  geomeans use these medians.  **spread** = max / min - 1 of the per-shift
  speedups.  A kernel is **layout-sensitive** (`*`) when the spread exceeds
  its noise band, max(`BENCH_LAYOUT_PCT` = 1 %, 2 x its noise), or when its
  speedup is above 1 at one shift and below 1 at another by more than half
  the band (`!`, the verdict flips).  The band is twice the noise because
  a speedup is a ratio of two times that each scatter by up to the noise.
  Change the floor with `BENCH_DEFS=-DBENCH_LAYOUT_PCT=0.5`.  Failed
  kernels are never marked.

Read it like this: a kernel whose speedup holds at every shift (small
spread, no marker) measures code generation; a marked kernel's speedup at
any single placement is partly placement luck, and its median is the
better estimate.  Shifts of 4-28 bytes move code within the fetch blocks
and undo the compilers' own 16/32-byte function and loop alignment (for
both); multiples of 32 keep that alignment and move the code across cache
lines; 1024 and more move it to other cache sets and branch history
entries.

Cost: every extra shift adds about 3.9 MB to `bench-ab.elf32` (4 MB with
one shift, 15.6 MB with 4, 31 MB with 8; `make check` fails above XeLL's
32 MiB ELF limit) and about 28 s to the run (33 kernels x 7 samples x 60 ms
x 2 variants), so 4 shifts take about 1.5 minutes longer.  Fewer or
shorter samples (`BENCH_DEFS`) shorten it.  The single-toolchain images
are not affected (one copy, no shifts).

### Known toolchain defects

* **sad16 shows WRONG B: test16old miscompiles it.**  At `-O2` test16old
  vectorises the textbook `sad += abs(a[x] - b[x])` reduction through a
  widen-sum expander whose `vmsumshm` instruction its gcc patch disabled
  (`toolchain/gcc-16.2.0.diff` sets the AltiVec `vmsum*`, `vmul[eo]*`,
  `vsum*`, `vmladduhm` and `vmh[r]addshs` patterns to
  `(TARGET_ALTIVEC && 0)` but leaves the expanders that emit them
  enabled).  The instruction is silently dropped, the frames are never
  loaded, and B's `run()` returns 0 (testxenon3 and the host return
  0x95dd045c).  The kernel is kept as written on purpose: this is the
  benchmark catching a real test16old bug, which also hits other vectorised
  narrow-integer sums (u8 -> u32 and int16 -> int32 sum loops return 0
  under qemu as well).  B's sad16 time is meaningless and is not in the
  geomeans.  `make qemu-check` reports it as XFAIL
  (`tools/qemu/known-failures.txt`); `bench-old16.elf32`, which has no A
  to compare with, shows sad16 as WRONG (result 0x00000000).
* **`OPT=-O3` does not build:** test16old stops with an internal compiler
  error on `mix_idct8.c` (`expand_insn`, the same disabled patterns).  Under
  the qemu flags, both compilers also compute wrong sad16 results at `-O3`.

* **LIB math measures two different libm wrapper layers.**  Both newlibs
  share the fdlibm cores, but test16old's is configured with the
  SVID/X-Open wrappers (`__fdlib_version` load and `matherr` path in every
  `expf`/`logf`/`exp`/`log`/`sqrt` call) and testxenon3's with the C99
  `__math_*` ones.  LIB mathf/mathd therefore compare each toolchain's libm
  as shipped, wrappers included, not code generation alone.

## Host build

`build/host/bench-host` compiles the harness and both namespaces with the
host gcc (labels host-A and host-B; both are the same compiler, so speedups
are about 1.0).  It is for testing kernels, the runner and the report
layout.  The "framebuffer" is a virtual character grid with the console's
rules (no wrapping, never the last column), printed in a frame:

    build/host/bench-host --quick --cols 74 --rows 27
    build/host/bench-host --only crc32,bsearch
    build/host/bench-host --quick --xenon-preview --cols 148 --rows 41

Options: `--quick` (3 samples x 5 ms), `--only ID[,ID...]`, `--samples N`,
`--sample-ms N`, `--cols N`, `--rows N`, `--color`/`--no-color`,
`--no-frame`, `--no-csv`, `--ghz F` (show cycles per unit for an F GHz host),
`--xenon-preview` (Xenon header texts, 49.875 MHz timebase, 64 cycles per
tick: a layout preview with the host's numbers).  The exit status is a bit
mask: 1 a kernel failed, 2 the report did not fit (clipped characters or
kernels left out), 4 setup error.  `make check` runs it under ASan/UBSan
and at 148x41 and 74x27 and fails on any non-zero status, on any EXACT
result that differs between the ASan (-O1) and the -O2 build (a sign of
unspecified or undefined behaviour, such as two `bench_rng` calls in one
expression), and on any kernel the host could not check against
`src/harness/refs.h` (bswap excepted, see "Reference results").

## Tools

* `tools/libprefix.py --tc <root> --prefix <id>_lib_ --header include/bench_lib.h -o <out.o>`
  pulls the members defining `BENCH_LIB_SYMBOLS` (and their closure inside
  libc.a/libm.a) out of a toolchain's `xenon/lib/32`, links them with
  `ld -m elf32ppc -r`, prefixes every symbol, renames the remaining
  undefined ones (`__errno`) back, and verifies that every
  `<id>_lib_<routine>` is defined and that no unprefixed definition leaks.
  It prints the members pulled and the externals left.
* `tools/check_syms.py kernels|sizes|map|place|isa ...`: the kcheck rules (allowed
  undefined symbols, no writable data, namespaced globals) over every
  cross-compiled kernel object, the size table, the A/B link-map check
  (each variant's `*_lib_*` routines come from its own prefixed object, the
  harness's `memcpy` from testxenon3's libc), and the instruction-set scan:
  every code word of the images must decode under testxenon3's
  `objdump -M xenon`, whose allow-list holds only what the Xenon PPE
  implements (test16old's assembler does not check this).  `place` checks
  in the A/B link map that every kernel's and newlib member's code and
  tables start on their `PLACE_ALIGN` boundary, the code `shift` bytes
  past it.
* `tools/place.py --align N [--shift S --rename OLD=NEW] in.o out.o`:
  copies an object with its non-empty `.text*`/`.rodata*` sections aligned
  to N (it patches `sh_addralign`; test16old's binutils 2.32 objcopy has no
  option for it).  The images link these copies; `libprefix.py --align N`
  does the same for each newlib member.  For layout shifts `--shift S` also
  puts S bytes of no-ops in front of the code (symbols, relocations and
  section-relative addends move with it) and `--rename` gives the copy its
  own symbol namespace; `libprefix.py --shift S` shifts every member.
* `tools/mkrefs.py`: writes `src/harness/refs.h` from a qemu-check run and a
  host run (`make refs`); `--verify` checks it is current (`make check`).
* `tools/kcheck.sh src/kernels/<cat>_<id>.c <id>`: per-kernel checks for
  kernel authors (DESIGN.md section 4).

## Layout

    include/          bench.h, bench_kernel.h, bench_lib.h (shared, frozen)
    src/kernels/      one kernel per file, table.c, list_<cat>.h
    src/harness/      main.c, runner.c (method), report.c (screen, CSV),
                      variants.c, platform.h + platform_xenon.c / platform_host.c,
                      refs.h (generated reference results)
    tools/            libprefix.py, place.py, mkrefs.py, check_syms.py,
                      kcheck.sh, ktest.c, qemu/
    build/            all outputs
