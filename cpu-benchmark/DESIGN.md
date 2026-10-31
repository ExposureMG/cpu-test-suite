# LibXenon CPU Benchmark: design

Goal: measure how fast code built by two Xbox 360 (Xenon) toolchains runs on
the same console, and print the comparison on the framebuffer when the run
finishes.

| Variant | id (symbol namespace) | label | toolchain root | default CPU |
|---|---|---|---|---|
| A | `xenon3` | `testxenon3` | `/home/e3xp0/Projects/libxenon/libxenon/toolchain/testxenon3` | `-mcpu=xenon` (+VMX128, Xenon cost model) |
| B | `old16`  | `test16old`  | `/home/e3xp0/Projects/libxenon/libxenon/toolchain/test16old`  | `-mcpu=cell` (generic PPC64 + Xenon tweaks) |

Both are GCC 16.2.0, newlib 4.6.0, binutils 2.47, 32-bit (ILP32) big-endian,
built with the libxenon flags
`MACHDEP = -DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float`.
testxenon3 predefines `_ARCH_XENON __XENON__ __VMX128__`; test16old predefines
`__PPU__`.  Neither is given an explicit `-mcpu`: each uses its configured default,
because the defaults are what users get.

## 1. Method

**One image, both compilers (the main deliverable, `bench-ab.elf32`).**  The
kernel sources are compiled twice, once by each toolchain, into two symbol
namespaces (`xenon3_*`, `old16_*`).  The harness (compiled by testxenon3,
linked with testxenon3's libgcc, newlib and libxenon) runs kernel *i* of A and
kernel *i* of B back to back on the same boot, the same arena addresses and the
same input data, then prints a side-by-side table.  This removes run-to-run,
boot-to-boot and thermal variation from the comparison.

* testxenon3 objects may call `_savevr128_N`/`_restvr128_N` (VMX128 callee-saved
  registers v64-v127), which only testxenon3's libgcc has, so the combined image
  is linked by testxenon3.  test16old code never touches v32-v127, so the ABI
  mix is safe.  The kernel interface uses only `void *` and `double`.
* **Library routines (category LIB).**  A combined image links one libc, so
  `tools/libprefix.py` pulls the needed members (and their closure inside
  libc/libm) out of *each* toolchain's `xenon/lib/32/libc.a`/`libm.a`, links them
  with `ld -m elf32ppc -r`, and renames every symbol to `<id>_lib_<name>` with
  `objcopy --prefix-symbols`.  Undefined externals of the closure (`__errno`,
  libgcc helpers) are renamed back to the real names.  LIB kernels call
  `BLIB(memcpy)` and so on (`include/bench_lib.h`), which binds each variant to its own
  newlib build.  newlib in testxenon3 was compiled with `-O2 -mcpu=xenon`, and
  in test16old with the cell default.
* **Single-toolchain images (`bench-xenon3.elf32`, `bench-old16.elf32`)** are
  fallbacks, built from the same sources with one toolchain end to end
  (harness, libxenon, newlib).  They print a one-column table.
* **Host build (`build/host/bench-host`)** compiles everything with the host
  gcc (both namespaces), using a host platform layer whose "framebuffer" is a
  virtual cols x rows character grid.  It is used to test kernels (ASan/UBSan),
  the harness logic and whether the report fits on screen.  There is no Xbox 360
  or Xenon emulator here, so **nothing can be run on Xenon during development**.
  Cross builds are checked statically: link, symbols, data sections,
  disassembly.
* **Big-endian runner (`make qemu-check`, `tools/qemu/`)** compiles the same
  kernel sources with *both* Xenon compilers using 32-bit-only flags
  (`-mcpu=7400 -mno-powerpc64 -maltivec`: qemu-ppc has no 64-bit CPU) and runs
  them under `qemu-ppc` (G4, big-endian, AltiVec) with a freestanding runtime.
  It checks byte-order-dependent logic, AltiVec intrinsics semantics, and that
  the two compilers agree on every result.  It is not the real code (no
  `-mpowerpc64`, no VMX128), but it is the closest thing to hardware available.
  `make refs` records its results in `src/harness/refs.h`, and every image
  checks each variant's result against them on the console (step 6).
* **Code placement.**  Where the linker puts a kernel moves its timing by
  several percent (on the host, identical A and B code showed 0.95x-1.06x
  from link order alone).  So every kernel object's and newlib member's
  non-empty `.text`/`.rodata` starts on a `PLACE_ALIGN` boundary (16 KB: the
  way size of Xenon's 2-way 32 KB L1 I-cache, which also covers its
  4K-entry branch history table; `tools/place.py`).  Identical code then gets
  identical cache sets, branch-history entries and fetch alignment in A and
  B; only each compiler's own layout inside its object differs.
* **Layout shifts (`make SHIFTS="0 4 8 12"`, default `0`).**  Identical
  placement is still one placement, which may favour one compiler's code by
  accident (the first console runs showed 3-5 % in branchy kernels that
  loop alignment did not explain).  With several shifts the A/B image (and
  the host build) links every kernel object and newlib member once per
  shift, its code starting `shift` bytes past its boundary for A and B
  alike (`tools/place.py --shift`: no-op padding in front of each `.text*`
  section, symbols and relocations moved with it; `.rodata*` stays on the
  boundary).  The copy at shift S gets the symbol namespace `<id>_sS_`
  (`place.py --rename`, `libprefix.py --prefix <id>_sS_lib_ --shift S`);
  the shift-0 copy keeps today's names, so the default image is unchanged.
  `src/harness/variants.c` exports one row of variant tables per shift
  (`-DBENCH_SHIFT_LIST`), and `check_syms.py place` verifies the placement
  in the link map.  Shifts are multiples of 4 below `PLACE_ALIGN`, at most
  8 (`BENCH_MAX_SHIFTS`); each one adds about 3.9 MB to the image, which
  must stay below XeLL's 32 MiB ELF limit (`check-ab` checks it).

**Timing.**  The timebase (`mftb`, 49.875 MHz = 3.2 GHz / 64) is read with the
`mftbu`/`mftbl`/`mftbu` retry loop.  libxenon runs with MSR[EE]=0 (no
interrupts).  After `xenon_make_it_faster`, threads 1-5 sit in libxenon's
low-priority secondary poll loop (thread 1 is the SMT sibling on core 0),
the same for A and B.  The harness is compiled with `-fno-tree-vectorize`,
so the referee (guards, timing) is plain scalar code on both toolchains.
Per kernel:

1. Check that A and B kernel *i* have the same `name`.  Write a guard
   pattern (4 KB) right after `arena_bytes`.  Call `A.init(arena)` **once**:
   both variants then run on identical bytes, even for FP-generated inputs.
2. Warm-up: `A.run` and `B.run` once each.  Their results are the reference values.
3. Calibrate `reps` with A so one sample (a block of `reps` back-to-back
   `run()` calls, timed as a whole) takes about `BENCH_SAMPLE_MS` (default 60 ms).
   B uses the same `reps`.
4. `BENCH_SAMPLES` samples per variant (default 7), alternating the order (AB,
   BA, AB, ...).  Every call's return value is compared bit-exactly with that
   variant's reference.  A difference marks the kernel **UNSTABLE**.
5. Per variant: min, median and max of ticks-per-run.  **time = min**,
   **noise = (median-min)/min**.  **speedup = time_B / time_A** (>1 means
   testxenon3 is faster).
6. Cross-check A vs B results: EXACT kernels must be bit-identical, APPROX ones
   within `tolerance` (relative).  Otherwise the kernel is **MISMATCH**.  The guard
   pattern must be intact (else **OVERRUN**).  Each variant's result must also
   match the kernel's reference result in `src/harness/refs.h` by the same rule
   (else **WRONG A** / **WRONG B**): only A's init runs, so a miscompiled init
   would feed both variants the same wrong input, which only the reference can
   catch.  A reference recorded for a different descriptor is ignored, and the
   byte-order-dependent one (bswap) is not checked on a little-endian host.
   Failed kernels are excluded from the geomeans and shown in red.
7. Geometric mean of speedups per category and overall.

With layout shifts (`r->nshift` > 1) steps 1 and 3 run once per kernel
(init by A at the first shift, `reps` calibrated with A at the first
shift).  Step 2 runs every copy; each must return the first shift's result
of its variant bit for bit (same code, same input), else **UNSTABLE**.  In
step 4 every sample goes round the shifts, A and B of one shift back to
back in the sample's order (AB or BA), so each shift's samples cover the
same stretch of time.  Step 5 is done per shift and variant; the speedup at
shift s is `time_B[s] / time_A[s]`, and the kernel's **speedup is the
median over the shifts** (the mean of the two middle values for an even
count); the times shown are the medians over the shifts of each shift's
`time`.  **spread** = max / min - 1 of the per-shift speedups; **noise** is
the largest noise of any copy; the noise band is
max(`BENCH_LAYOUT_PCT` (1 %), 2 x noise), because a speedup is the ratio of
two times that each scatter by up to the noise.  A kernel is
**layout-sensitive** when its spread exceeds the band or when it **flips**
(a per-shift speedup above 1 + band/2 and another below 1 - band/2).
Layout sensitivity is information, not a failure: the kernel stays in the
geomeans with its median.  Step 7 uses the medians and also gives the
geomean at each shift over the same kernels.  The step 6 checks use
the first shift's results (all shifts' results are equal or UNSTABLE).

## 2. Layout and ownership

```
DESIGN.md                    this file (owner: lead)
README.md                    usage: build, run, read the screen      (infra)
Makefile                     all targets                             (infra)
include/bench.h              shared types                            (lead, frozen)
include/bench_kernel.h       kernel-side helpers, namespacing        (lead, frozen)
include/bench_lib.h          BLIB() routines                         (lead, frozen)
src/kernels/table.c          per-variant table + build info          (lead, frozen)
src/kernels/list_<cat>.h     kernel ids per category, display order  (lead, frozen)
src/kernels/<cat>_<id>.c     one kernel per file                     (kernel authors)
src/harness/*.c *.h          runner, stats, report, platform layers  (infra)
tools/ktest.c, kcheck.sh     per-kernel host test + cross checks     (lead)
tools/libprefix.py           newlib member extraction + prefixing    (infra)
tools/check_syms.py          build-time kernel object checks         (infra)
tools/qemu/                  big-endian qemu-ppc runner + qemu.mk    (BE validation)
build/                       all outputs
```

"Frozen" files change only through the lead.  If a frozen file blocks you, say
so in your report and do not edit it.

## 3. Kernel contract (every kernel author reads this)

A kernel file `src/kernels/<cat>_<id>.c` includes `bench_kernel.h` (and
`bench_lib.h` for LIB kernels, `<altivec.h>` for intrinsics kernels).  It defines
`static void <id>_init(void *arena)`, `static double <id>_run(void *arena)`,
and one descriptor:

```c
BENCH_KDEF(crc32) = {
	.name = "crc32", .desc = "CRC-32 slice-by-4, 64 KB", .unit = "B",
	.category = BENCH_CAT_INT, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = 65536, .arena_bytes = ARENA_BYTES,
	.init = crc32_init, .run = crc32_run,
};
```

Rules:

1. **ids are fixed** by section 5 and by `src/kernels/list_*.h`.  `name` equals
   the id.  `desc` is 32 chars or fewer, `unit` 4 chars or fewer.  `units` is a compile-time
   constant equal to the exact work done by one `run()`, so design the workload
   so its size is known exactly (for example, deterministic length cycles
   instead of random lengths).
2. **All mutable data lives in the arena** (32 MB, 64 KB-aligned, shared by
   both variants).  No mutable `static`/global variables: the B variant's
   statics would never be initialised, because only A's `init` runs.
   `static const` tables are fine.  Place sub-buffers at offsets that are multiples
   of 128 (`BENCH_LINE`).  Report `arena_bytes` honestly: the harness puts a
   guard right after it.
3. `init` is deterministic.  Use `bench_rng` with a kernel-specific seed.
   Generate integer data with integer arithmetic.  FP data may be computed
   freely, since only one variant's `init` runs, but keep it simple.  `init` may
   store absolute pointers in the arena, because both variants run on the same arena.
4. `run` is one pass of the workload.  It **must not modify its inputs**, must
   return the **same value on every call**, and must derive every pointer from
   `arena`.  If it needs a scratch copy (sorting, simulation state), it resets
   that copy itself at the start, using `bench_copy`/`bench_fill`.  The reset is part
   of the measured work, so keep it small relative to the rest.
5. **Result.**  EXACT kernels fold their outputs into a `uint32_t` (`bench_mix`,
   sums, xors) and `return bench_result_u32(h)`.  APPROX kernels (FP results
   may differ in the last bits between compilers, for example through FMA
   contraction) return a `double` aggregate (sum, energy, total iterations)
   and set `.flags = BENCH_F_APPROX, .tolerance = <rel>`.  Pick the
   tolerance from the analysis in section 5, never "big enough to hide bugs".  Fold
   enough of the output that a wrong result changes the value, but keep the
   folding cheap relative to the work (sample every N-th element if the kernel
   is store-heavy).
6. **No libc/libm calls.**  Use `BLIB(fn)` for the LIB routines, and only in LIB
   kernels.  The only other allowed undefined symbols are `sqrt`/`sqrtf` (GCC's
   errno fallback after an inline `fsqrt`) and libgcc's save/restore helpers.
   GCC turns plain zero-fill loops, strlen-style loops and provably
   non-overlapping copy loops into `memset`/`strlen`/`memcpy` calls; use
   `bench_copy`/`bench_fill` or fold a transform into the loop.
   `tools/kcheck.sh` reports any such symbol.
7. **Write idiomatic, portable C** the way a homebrew developer would (C11 +
   GNU extensions; `restrict` where real code would use it).  No inline asm.  No
   `#ifdef __XENON__` code paths: both compilers must compile *the same source*.
   The one exception: AltiVec-intrinsics kernels wrap the intrinsics in
   `#ifdef __ALTIVEC__` and carry a scalar fallback with the same data layout
   for the x86 host build.  Both Xenon compilers (and the qemu runner) define
   `__ALTIVEC__` and compile the AltiVec path.
   No undefined behaviour: no signed overflow (kernels are built without
   `-fwrapv`), no strict-aliasing violations (use `memcpy`-free unions or
   `__builtin_memcpy` of 4/8 bytes for type punning), no out-of-bounds accesses.
8. **Alignment.**  Keep multi-byte scalar loads naturally aligned (Xenon handles
   misaligned integer loads in microcode at best).  Vector loads and stores
   (`vec_ld`/`vec_st`) need 16-byte alignment.  Vector types come from `<altivec.h>`
   (`vector float`, `vector unsigned char`, ...).
9. **Size.**  One `run()` should take roughly 0.1-2 ms on Xenon (3.2 GHz,
   in-order, dual-issue, IPC about 0.5-1 on typical code), or about 0.01-0.3 ms on a
   modern host at -O2.  The harness repeats `run()` to fill a sample.  Keep the
   working set at or below 512 KB (L2 is 1 MB, shared) unless the kernel is about memory.
   Stack use in `run` must stay at or below 16 KB.
10. Check every kernel with
    `tools/kcheck.sh src/kernels/<cat>_<id>.c <id>` from the project root.  It
    runs the host test with ASan/UBSan and at -O2, and cross-compiles with both
    toolchains at -O2 with `-Wall -Wextra -Werror`.  It checks undefined symbols
    and writable data and writes the assembly to `build/kcheck/<file>.<variant>.s`.
    Read the assembly of your hot loops for both variants.  Make sure the work is not
    optimised away and that the loop does what you intended.

Xenon facts worth knowing (from the testxenon3 cost model, `xenon-costs.def`):
the integer latency is 2 cycles.  Record-form ops (`andi.`, `and.`, `rlwinm.`) are
microcoded at about 14 cycles, and so are variable shifts (`slw`/`srw`/`sraw`, 16
cycles) and algebraic loads (`lha`/`lwa`, 20-23 cycles).  `mullw` takes 11 cycles,
`mulld` 14, `divw` 18-46 and `divd` up to 78.  The FP latency is 10 cycles, `fdiv` 74 and `fsqrt` 84.  `fcmpu`
issues once per 16 cycles.  Every GPR/FPR/VR transfer goes through memory and suffers a
load-hit-store of about 40 cycles.  The core is in-order with two issue slots.  The VMX128 extension
has 128 vector registers.  **The kernels should contain the constructs real
code has (bit tests, int16 data, int/float conversions, variable shifts,
divides by constants, FP compares, vectorisable loops).  Do not avoid them:
they are where the toolchains differ.**

## 4. Kernel authors' workflow

1. Read sections 3 and 5, `include/bench.h`, `include/bench_kernel.h` and
   `include/bench_lib.h` (`tools/kcheck.sh` shows how kernels are built).
2. Write `src/kernels/<cat>_<id>.c` for each assigned id.  Do not edit any
   other file.  If you think a frozen file needs a change, explain it in your report.
3. `tools/kcheck.sh ...` until it prints `PASS`.  Check the host timing
   against rule 9.  Read the hot loops in both `.s` files.
4. Report per kernel: id, units, arena bytes, flags/tolerance and why, host
   µs/run, `.text` size for both variants, and anything notable in the
   assembly (vectorised or not, microcoded ops present, and so on).

## 5. Kernel catalogue

Sizes are targets.  Adjust them within rule 9 if needed and report the change.
Data is little-endian only where stated.  The rows below are as built; where
an author changed a target, the kernel's source comment says why.

### INT: integer ALU, bit manipulation, multiply/divide  (file prefix `int_`)
| id | workload | unit / units | result |
|---|---|---|---|
| `crc32` | CRC-32 (zlib polynomial 0xEDB88320, reflected) with slicing-by-4 tables (4x256 u32 built in init) over a 64 KB random buffer. Assemble the 32-bit chunks from bytes (the buffer is a byte stream; the machine is big-endian). | B / 65536 | EXACT: final CRC |
| `murmur3` | MurmurHash3_x86_32 over 4096 keys from a byte pool.  Keys start at 4-byte-aligned offsets; lengths cycle deterministically through 1..64 (each length 64 times, shuffled order allowed) so the total is 133120 B.  Body: 4-byte blocks with rotl/multiply, tail switch, fmix. | B / 133120 | EXACT: fold of all hashes |
| `expgolmb` | Exp-Golomb `ue(v)` decoder (H.264 style): init encodes 65536 values with a skewed distribution (v < 4096) MSB-first into a bitstream (pad the end), run decodes them with a 32- or 64-bit bit-reservoir reader (leading-zero count, variable shifts, refill). | sym / 65536 | EXACT: fold of decoded values |
| `divmod` | Integer division mix: 8192 u32 → decimal ASCII (div/mod by 10 and/or 100 via digit-pair table), 2048 GCDs (Euclid with `%` on variable divisors), and for 8192 values `v % 7`, `v % 1000`, `v / 3` and a signed `/ 16` on negated values. | op / 18432 | EXACT |
| `bitops` | Over 8192 u64 words: popcount and parity (SWAR: `__builtin_popcountll`/`__builtin_parityll` become libgcc calls on Xenon, which rule 6 forbids), clz/ctz (`__builtin_clzll`/`ctzll` on non-zero inputs), bit reversal (mask-and-shift swap ladder), rotate by a data-dependent amount, and branchy single-bit tests `if (w & (1ull << k))` with data-dependent k. | word / 8192 | EXACT |

### BR: branch-heavy control flow  (file prefix `br_`)
| id | workload | unit / units | result |
|---|---|---|---|
| `quicksrt` | Copy 8192 random int32 into a work buffer (`bench_copy`), sort with your own quicksort (median-of-3, insertion sort for n ≤ 16, recurse on the smaller side). | elem / 8192 | EXACT: fold of sorted array (also proves order) |
| `bsearch` | Sorted array of 65536 strictly increasing u32 (256 KB), 4096 queries (about half present), branchy `lower_bound` (`if (a[mid] < key) lo = mid + 1; else hi = mid;`). | find / 4096 | EXACT: fold of indices |
| `interp` | Stack/register bytecode VM, 24 opcodes (push/load/store/add/sub/mul/and/or/xor/shl/shr/cmp/jz/jnz/jmp/dup/swap/halt...), `switch` dispatch, running a fixed program (a sieve over 2048 words, a prime-gap hash, nested LCG loops).  147,486 VM ops per run, the exact count (`units`); a taken jump past 4x that halts the VM, so corrupt bytecode cannot hang the console. | vmop / 147486 | EXACT: fold of VM state |
| `numfsm` | CoreMark-style state machine: 32 KB of comma/space-separated tokens (ints, signed ints, decimals, scientific `1.5e-3`, hex `0x1F`, invalid tokens), scanned char by char through a `switch(state)` machine.  Count final states per class and transitions. | B / 32768 | EXACT |

### MEM: memory hierarchy  (file prefix `mem_`)
| id | workload | unit / units | result |
|---|---|---|---|
| `bswap` | 8192 32-byte little-endian records `{u32 id; u16 a, b; u64 ts; f32 x, y; u32 flags; u16 c, d;}` decoded into native big-endian structs (`__builtin_bswap16/32/64`; floats via their u32 bits, no FP math), as a game reading a PC data file does. | B / 262144 | EXACT |
| `transpos` | 256x256 u32 matrix transpose in 16x16 tiles (256 KB in, 256 KB out). | elem / 65536 | EXACT: fold of sampled output |
| `ptrchase` | 4096 nodes, one per 128-byte line (512 KB), linked in one random cycle (Sattolo), each with a payload; follow 16384 hops summing payloads.  Latency-bound control: the expected speedup is about 1.0. | hop / 16384 | EXACT |
| `stream` | STREAM triad `a[i] = b[i] + s * c[i]`, double, 3 arrays of 262144 (6 MB, beyond L2). | B / 6291456 | APPROX (1e-12): sum of sampled `a` |

### FP: scalar floating point  (file prefix `fp_`)
| id | workload | unit / units | result |
|---|---|---|---|
| `matmul` | 64x64 double C = A·B, i-k-j order (initialise each C row from the k=0 term instead of zeroing, so no memset). | flop / 524288 | APPROX (1e-12): sum of C |
| `nbody` | 64 bodies, double, 4 steps of pairwise gravity with softening (sqrt and divide), positions/velocities reset from the input at the start of `run`. | pair / 8064 | APPROX: total energy or position sum |
| `mandel` | float Mandelbrot 128x96 over [-2.0, 0.6) x [-1.0, 1.0) at pixel centres, max 64 iterations, main cardioid and period-2 bulb skipped in closed form (as real renderers do), iteration counts written to a u8 image. | px / 12288 | APPROX (1e-3): total iterations |
| `mathapx` | Game-math-library style `sinf`/`cosf`/`expf` approximations (Cephes-like: range reduction through a float→int conversion, quadrant selection by bit tests, minimax polynomials, exponent assembly by bit manipulation) over 8192 inputs each. | eval / 24576 | APPROX: sum |

### VEC: SIMD  (file prefix `vec_`)
| id | workload | unit / units | result |
|---|---|---|---|
| `vxform` | AltiVec intrinsics: 8192 `vector float` vertices × 4x4 matrix (splat + `vec_madd`), perspective divide with `vec_re` + one Newton-Raphson step, 4 vertices per iteration (enough live vectors to stress the register file). | vert / 8192 | APPROX: sum of outputs |
| `vblend` | AltiVec intrinsics: 256x256 RGBA8 src-over-dst alpha blend with per-pixel alpha into a separate output.  Xenon's VMX has no integer multiply or multiply-sum (testxenon3 rejects `vec_mule`/`vec_mulo`/`vec_mladd`/`vec_msum`/`vec_sum4s`; test16old's patterns for them are broken), so the products are exact float math: unpack to words, `vec_ctf`, alpha splat with `vec_perm`, `vec_madd`, exact rounding divide by 255, pack back with `vec_packsu`. | px / 65536 | EXACT |
| `saxpy` | Plain C (for the auto-vectoriser): `z[i] = a*x[i] + y[i]` and a 4-lane dot product (`s0..s3` partial sums) over 16384 floats, `restrict` pointers. | elem / 16384 | APPROX |
| `pcmconv` | Plain C: 32768 interleaved int16 stereo frames → float → 2x2 gain/pan matrix → clamp to the int16 range → int16 interleaved output (audio mixer path: int16 loads, int↔float conversions). | frm / 32768 | APPROX: sum of outputs |
| `sad16` | Plain C: motion search: 32 16x16 macroblocks of a 128x64 u8 frame against a reference frame, ±3 px window (49 positions), SAD with `abs()`, keep the best. | px / 401408 | EXACT |

### MIX: application-style  (file prefix `mix_`)
| id | workload | unit / units | result |
|---|---|---|---|
| `sha256` | SHA-256 compression over 256 64-byte blocks (16 KB) of random data (message words are big-endian, native). | B / 16384 | EXACT: fold of digest |
| `aes128` | AES-128 in CTR mode with 4 T-tables (built in init from the S-box), 16 KB. | B / 16384 | EXACT |
| `lz4dec` | LZ4 block-format decoder (tokens, 255-extended lengths, 16-bit LE offsets, overlapping matches).  init builds 128 KB of compressible text-like data and compresses it with a simple greedy LZ4 encoder; run decodes it. | B / 131072 | EXACT |
| `idct8` | JPEG `jidctint`-style 8x8 integer IDCT (CONST_BITS 13, PASS1_BITS 2) over 1024 blocks of int16 coefficients (sparse, realistic) × u16 dequant table, range-limited to u8. | blk / 1024 | EXACT |
| `raytrace` | Small float ray tracer, 64x48: 6 spheres + ground plane, one directional light, shadow ray, Lambert + ambient, one reflection bounce, RGB8 output. | ray / 3072 | APPROX (1e-3): sum of pixel bytes |

### LIB: the toolchain's newlib routines through `BLIB()`  (file prefix `lib_`)
| id | owner | workload | unit / units | result |
|---|---|---|---|---|
| `memcpy` | MEM | 4096 copies with lengths cycling through 1..256 (each 16 times) at arbitrary byte offsets inside 128 KB, then 4 aligned 32 KB copies. | B / 657408 | EXACT: fold of sampled dst |
| `memset` | MEM | 4096 fills with lengths cycling through 1..512 (each 8 times) at arbitrary offsets, varying values, then 4 aligned 64 KB fills. | B / 1312768 | EXACT |
| `strops` | BR | 2048 NUL-terminated strings (4-60 chars, shared prefixes): `strlen`, `strcmp` against a neighbour, `strchr`, plus 512 `memcmp`s of 64-byte blocks (some equal). | call / 6656 | EXACT |
| `qsortlib` | BR | `BLIB(qsort)` of 4096 u32 with a comparator in the kernel file (copy-in with `bench_copy`). | elem / 4096 | EXACT |
| `mathf` | FP | `sinf cosf expf logf sqrtf` over 2048 suitable inputs each. | call / 10240 | APPROX |
| `mathd` | FP | `sin cos exp log sqrt` over 2048 inputs each. | call / 10240 | APPROX |

The two newlibs share the fdlibm cores but not the libm wrappers:
test16old's is configured with the SVID/X-Open ones (`__fdlib_version`,
`matherr`), testxenon3's with the C99 `__math_*` ones.  So `mathf`/`mathd`
measure each libm as shipped, wrappers included.

## 6. Harness, report and platform  (infra)

**Variant wiring.**  `src/harness/variants.c` is compiled with `-DBENCH_A=xenon3
[-DBENCH_B=old16]` and exports the 1 or 2 `bench_variant_info` pointers
(`xenon3_info`, `old16_info` come from `src/kernels/table.c`).  The runner checks
that the tables have equal length and names.

**Runner** (`runner.c`, portable): implements section 1.  Compile-time knobs:
`BENCH_SAMPLES` (7), `BENCH_SAMPLE_MS` (60), `BENCH_LAYOUT_PCT` (1.0, the
floor of the layout-sensitivity band).  `bench_run_all` takes the variant
tables as rows of shifts (`vars[s][j]`) and their offsets; results are kept
per shift and variant (`bench_kres.v[s][j]`, `sspeed[s]`) plus the
per-kernel median, spread and verdict.  The host binary also accepts `--quick`,
`--only <id>`, `--cols N --rows N`.  Progress goes to the scrolling console, one
line per kernel, for example
`[ 7/33] bsearch  BR   A  812.3 us  B  905.1 us  1.114x  ok`.
The total run should take about a minute or less; each extra layout shift
adds about 28 s (33 kernels x 7 samples x 60 ms x 2 variants).  With
shifts the progress line shows the median speedup, the spread and the
layout marker.

**Report** (`report.c`, portable): the final screen is drawn after a clear,
with the platform's `screen_put(col, row, color, text)` (clipped, no wrapping, no
newlines on the framebuffer: the libxenon console scrolls on '\n' past the
last row and wraps at the last column, so keep every line at or below cols-1).  It must
fit `cols x rows` as reported by the console (720p HDMI is about 160x45,
or 148x41 with overscan; 480p is about 80x30, or 74x27).  Pick the most
informative layout that fits:

* **wide** (one kernel per line): name, category, description (if room),
  A µs/run, B µs/run, A and B cycles/unit (Xenon: ticks × 64 / units; omit on
  the host), speedup (colored green if > 1.01, red if < 0.99), noise %, check
  status;
* **compact** (two kernels per line): name, A µs, B µs, speedup, status flag;
* header: title + version; A and B lines (label, compiler, cpu, kernel cflags);
  environment (CPU PVR, speed, TB frequency, samples × sample length, kernel
  count, total run time);
* footer: per-category geomeans (one or two lines), overall geomean with
  wording like "testxenon3 is 1.143x faster than test16old (geomean of 33
  kernels)", failure count, legend "speedup = B time / A time" and what "ok"
  vouches for ("ok = A, B and reference agree").  Check words, most severe
  first: MISSING, BADDESC, NOTIMER, OVERRUN, UNSTABLE, WRONG A/B/AB (WRONG in
  single images), MISMATCH.
* Single-variant images: µs/run, cycles/unit, Munits/s, result (hex for EXACT).
* Layout shifts: the speedup is the median over the shifts, A/B µs and
  cyc/u are the medians of each shift's time; a `*` after the speedup marks
  a layout-sensitive kernel (`!` a flip).  wide adds one speedup column per
  shift (or one `min-max` column when they do not fit) and a `spread`
  column (yellow when layout-sensitive), ahead of the description and the
  bar; compact and minimal add the marker; the header names the shifts and
  the footer adds the geomean at each shift and the layout-sensitive count.

After drawing the screen, dump everything to serial only, one CSV line per
kernel per variant with all statistics and results (`plat_log`), so users with
a serial cable get machine-readable data.  With layout shifts (and only
then) the log adds a `shifts` line, a trailing `shift` field to the `kernel`
lines (one line per kernel, shift and variant; every old field keeps its
meaning for that copy, `speedup` being that shift's), one `layout` line per
kernel (median, min, max, spread, noise, band, flip, sensitive, the times
shown and the per-shift speedups) and `geomean_shift` lines; the `geomean`
lines keep their format and hold the geomeans of the medians (README,
"Serial log").

**Platform API** (`platform.h`; `platform_xenon.c`, `platform_host.c`):
`plat_init` (Xenon: `xenos_init(VIDEO_MODE_AUTO)`, `console_init()`,
`xenon_make_it_faster(XENON_SPEED_FULL)`, allocate the 64 KB-aligned arena),
`plat_ticks`, `plat_ticks_per_sec` (Xenon: `PPC_TIMEBASE_FREQ`),
`plat_cycles_per_tick` (Xenon 64.0, host 0 = unknown), `plat_cpu_desc` (Xenon:
PVR via `mfspr(287)` and speed via `xenon_get_speed()`: declare it extern, since it
is not in the header), `plat_status` (scrolling FB + serial), `plat_log` (serial
only: libxenon `putch`), `plat_screen_size`, `plat_screen_clear`,
`plat_screen_put`, `plat_finish` (Xenon: spin forever with a low-priority hint so
the results stay on screen.  Returning from `main` would make libxenon
reload XeLL).  On Xenon, framebuffer-only text goes through
`console_set_cursor`/`console_set_colors`/`console_putch` (colors are the
`CONSOLE_COLOR_*` values); `printf` writes to both FB and UART.  The host
platform keeps a virtual cols x rows grid, prints it (with ANSI colors when
stdout is a TTY) and counts clipped characters.  `make check` fails if any
were clipped or any kernel failed.

## 7. Build (infra)

`Makefile` variables `X3`, `O16` (toolchain roots, defaults in the table above),
`OPT` (default `-O2`, applied to kernels of both variants), `MACHDEP`,
`PLACE_ALIGN`, `SHIFTS` (layout shifts, section 1).
Kernel/table objects are built per variant with that variant's `xenon-gcc`,
`-std=gnu11 $(MACHDEP) $(OPT) -Wall -Wextra -DBENCH_VARIANT=<id>
-DBENCH_LABEL='"<label>"' -DBENCH_CFLAGS='"$(OPT)"' -Iinclude` into
`build/<id>/`.  Harness objects are built by the image's linking toolchain with
`-O2 -fno-tree-vectorize -I$(TC)/usr/include` (libxenon headers).  The images
link copies of the kernel objects whose non-empty `.text`/`.rodata` sections
are aligned to `PLACE_ALIGN` (default 16384; `tools/place.py` patches
`sh_addralign`, `build/placed/`), and `libprefix.py --align` does the same for
every newlib member (section 1, code placement).  With `SHIFTS`, the copy of
every kernel object at shift S is `build/placed-sS/...` (`place.py --align
--shift S --rename <id>_=<id>_sS_`) and of every newlib object
`build/<id>/lib_<id>_sS.o`; the A/B image, `build/host/bench-host` and the
ASan build link all of them (the single-toolchain images only shift 0).
Link like devkitxenon's `rules`:
`$(CC) $^ $(MACHDEP) -Wl,--gc-sections -Wl,-Map,<map> -L$(TC)/xenon/lib/32
-L$(TC)/usr/lib -lxenon -lm -n -T $(TC)/app.lds -o x.elf`, then
`objcopy -O elf32-powerpc --adjust-vma 0x80000000 x.elf x.elf32 && strip x.elf32`.

Targets: `all` (= `ab single`), `ab` → `build/bench-ab.elf32`, `single` →
`build/bench-xenon3.elf32 build/bench-old16.elf32`, `host`, `check` (host ASan
run + host quick runs at 148x41 and 74x27 + `check_syms.py` over every
cross-compiled kernel object + the A/B link map and code placement
(`check_syms.py place`: every kernel's and newlib member's code at its
boundary + shift, its tables on the boundary) + a Xenon instruction-set scan
of the images + `refs.h` current and every host result matching it), `refs`
(`qemu-check`, then `tools/mkrefs.py` records the big-endian results in
`src/harness/refs.h`), `asm` (per-kernel `.s` for both variants in
`build/asm/<id>/`), `sizes` (per-kernel `.text` bytes, A vs B), `clean`.
The Makefile ends with `-include tools/qemu/qemu.mk`.  That fragment, owned
by the BE-validation author, defines only `qemu-check`/`qemu-clean` and
`QEMU_*` variables, builds into `build/qemu/`, and also works standalone as
`make -f tools/qemu/qemu.mk qemu-check`.
`check_syms.py` repeats the kcheck symbol and writable-data rules for all
kernel objects and fails the build on a violation.  `libprefix.py` must check
that every requested `<id>_lib_<sym>` is defined in its output and print the
externals it left unresolved.

The shell for tool calls is **zsh**: unquoted `$VAR` is not word-split.  Use
bash scripts (`#!/usr/bin/env bash`), arrays, or `${=VAR}`.
