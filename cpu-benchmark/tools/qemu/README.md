# tools/qemu: big-endian validation under qemu-ppc

There is no Xbox 360 and no Xenon emulator here, and the host is
little-endian x86-64.  This directory is the closest thing to hardware we
have. It compiles the **real** kernel sources (`src/kernels/*_*.c` and
`src/kernels/table.c`) with **both** Xenon compilers into their two
namespaces (`xenon3_*`, `old16_*`). It uses 32-bit-only flags, links the
result with a small freestanding runtime, and runs it under `qemu-ppc`
(user mode, 32-bit big-endian PowerPC G4 with AltiVec).

It checks:

* logic that depends on byte order;
* AltiVec intrinsics semantics (the AltiVec path under qemu against the
  kernel's scalar fallback on the host);
* that both compilers agree on every result, run after run.

It does **not** run the real code, and it measures **no** timing. See
[Limitations](#limitations).

## Usage

```sh
make qemu-check                          # from the main Makefile (it -includes tools/qemu/qemu.mk)
make -f tools/qemu/qemu.mk qemu-check    # standalone, from the project root
make qemu-clean                          # removes build/qemu/ only
```

`qemu-check` needs every kernel, because `table.c` references all 33 ids.
While kernels are still missing, the link step stops with a list of the
missing ids. In that case use the scratch runner. It copies the tree to
`$TMPDIR`, keeps every real kernel, stubs only the missing ones, and runs
the same check there. Nothing in the project is modified:

```sh
tools/qemu/qscratch.sh            # stub missing kernels
tools/qemu/qscratch.sh -k         # also run tools/kcheck.sh on each real kernel; stub failures
tools/qemu/qscratch.sh -S         # stubs only (tests the runner itself)
tools/qemu/qscratch.sh -o DIR -j8 # other scratch dir; extra args go to make
```

The stubs come from `tools/qemu/stubs/<cat>_<id>.c` when one exists. They
cover AltiVec float and integer intrinsics, every BLIB routine, `sqrtf`, the
64-bit libgcc helpers, and a deliberately native-endian `sha256` that the
cross-check should flag. Other ids get a generated generic EXACT stub.
`qemu.mk` itself never uses stubs.

To run the runner by hand, after a build:

```sh
qemu-ppc -cpu 7400 build/qemu/bench-qemu.elf [-v] [-m] [-l] [-s] [-r N] [-k id]...
  -k id   only this kernel (repeatable; also --only id)
  -v      verbose: descriptor fields, bits of every run's result, pass-2 results
  -m      machine-readable result lines (@R id flags tol A B status)
  -l      list kernel ids
  -s      skip the second, 0x5A-poisoned init-determinism pass
  -r N    run() calls per variant, 1-8 (default 3)
```

The runner exits with 0 when every kernel is ok, 1 when a kernel failed, 2
on a usage error, and 3 on a crash. A crash prints the signal, the kernel,
the phase (for example `old16 run`), and pc/lr/fault address. Map them with
`build/qemu/bench-qemu.map` or
`$(X3)/bin/xenon-addr2line -f -e build/qemu/bench-qemu.elf <pc>`.

Make variables (all optional): `QEMU_BIN` (default `qemu-ppc`), `QEMU_CPU`
(qemu model, `7400`), `QEMU_GCC_CPU` (`-mcpu=`, `7400`), `QEMU_TIMEOUT`
(seconds, `1200`), `QEMU_ARGS` (extra runner options, for example
`QEMU_ARGS=-v` or `QEMU_ARGS="-k crc32"`), `QEMU_XCHECK=0` (skip the
host cross-check), and `QEMU_HOSTCC`. The kernel optimisation level follows
`OPT` (default `-O2`). The toolchain roots follow `X3`/`O16` when they are
defined, and default to the paths in DESIGN.md. A flag change rebuilds what
it affects (`build/qemu/flags.txt`). Adding or removing a kernel relinks
(`build/qemu/kernels.txt`).

## What `qemu-check` does

1. **Instruction scan.** `xenon-objdump -d -M 7400` prints every encoding
   the G4 does not have as `.long`: 64-bit loads/stores/rotates/divides,
   `fcfid`/`fctid*`, VMX128, `isel`, `popcntb`, `fre`, `fri*`, `lvlx`/`stvrx`,
   and so on. `fsqrt[s]`, `mfocrf`/`mtocrf` and L=1 (64-bit) compares are
   matched by name. `-M xenon` names VMX128 instructions `*128`, and those
   are matched too. All kernel, table and runtime objects and both images
   are scanned. Any hit fails the check. Verified on Xenon-flag objects:
   the scan catches `ld`/`std`/`rldicl`/`cmpdi`/`lvx128`/`vmaddcfp128`.
2. **Helper calls** of the 32-bit kernel objects (informational). Plain
   libc names that appear only under these flags are listed. Example:
   `memcpy` for a large block copy, which GCC expands inline with
   `-mpowerpc64`.
3. **Helper self-test.** `qselftest.c` exercises every libgcc helper,
   `sqrt` and `sqrtf` through ordinary C operators (`a / b` on `uint64_t`,
   `(double)int64`, `__builtin_popcountll`, `__builtin_powi`, ...). It is
   compiled by each Xenon compiler and run under qemu, and compiled
   natively as the reference. The 17 result hashes must be identical for
   xenon3, old16 and the host.
4. **The runner**, per kernel index:
   * check that both tables hold the same id (NAME);
   * check that the descriptor is valid by the `ktest.c` rules, and that
     A's and B's descriptors are identical field by field (DESC);
   * poison `arena_bytes` with 0xA5 and write a 4 KB guard pattern after it;
   * run `xenon3.init` once;
   * call `run()` 3 times per variant in the real harness' alternating
     order (AB, BA, AB);
   * each variant must return bit-identical values every time
     (UNSTABLE-A/-B);
   * EXACT kernels must agree bit for bit and APPROX kernels within
     `tolerance * max(|a|,|b|)` (MISMATCH);
   * EXACT results must be uint32 values and APPROX results finite (RESULT);
   * the guard must be intact (OVERRUN);
   * then poison with 0x5A, run init again, and call each variant's
     `run()` once. A different result means init or run read bytes that
     init never wrote (UNINIT);
   * measure the deepest stack use of the `run()` calls. The stack below
     the runner's frame is painted first. Use over DESIGN.md's 16 KB is a
     WARN, not a failure: 32-bit code can need more stack than the real
     build. `-v` prints the depth per kernel, and the summary names the
     deepest one.

   It prints one line per kernel (id, category, both results, status) and
   a summary.

   If the run crashes or times out, every kernel is run again on its own,
   so one crash does not hide the others' results.

   **Known toolchain defects.** `known-failures.txt` lists root-caused
   failures that are the compilers' fault, not the kernels' (today one:
   test16old's `sad16`, see below). A listed kernel is reported as `XFAIL`
   and does not fail the step, but only if its `@R` line matches the entry
   exactly (status flags, A result bits, B result bits). Any other failure,
   or the same kernel failing with a different result, still fails the
   check; a listed kernel that passes is reported as `XPASS`.
5. **BE/LE cross-check** (informational). The same runner is built for the
   host over host-compiled kernels (`-DBENCH_HOST`: scalar fallbacks, host
   libc/libm). It compares its results with the qemu run and reports each
   kernel as same, within tolerance, or DIFFERENT.
   * An EXACT kernel whose result differs is endian-dependent. That is
     legitimate only where DESIGN.md says data is native. For example,
     `bswap` decodes little-endian records into native big-endian structs.
   * An AltiVec kernel that differs means its intrinsics path and scalar
     fallback disagree.
   * APPROX kernels differ by FMA contraction (PPC has fused multiply-add,
     x86-64 baseline does not). LIB math kernels also differ by the libm
     used.

The negative tests were run on a scratch copy with broken kernels. Every
status was produced as intended:

* a non-idempotent run;
* a 1-byte overrun;
* an uninitialised read;
* `#ifdef __XENON__` results, both EXACT and APPROX;
* a 1e-9 difference under a 1e-12 tolerance (MISMATCH) and under 1e-6 (ok);
* an EXACT result of 0.5;
* a NaN;
* differing `units`;
* a NULL dereference, which gave CRASH followed by the isolation runs.

## How it is built

| | |
|---|---|
| Kernel flags | `-std=gnu11 -DXENON -m32 -mcpu=7400 -mno-powerpc64 -maltivec -mhard-float -fno-pic $(OPT) -Wall -Wextra`, plus `-mno-vmx128` for testxenon3. That flag is already implied by `-mcpu=7400` (`-Q --help=target` shows it disabled) and is passed anyway; test16old does not know it. Same `BENCH_VARIANT`/`BENCH_LABEL` as the real build; `BENCH_CFLAGS` records the qemu flags. |
| Runtime flags | the same arch flags, `-O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -ffp-contract=off`, compiled by testxenon3. |
| Link | `xenon-ld -m elf32ppc -static -e _start`, with **no** toolchain library: no newlib (testxenon3's 32-bit newlib contains 64-bit instructions), no libgcc.a. |
| Run | `qemu-ppc -cpu 7400` (G4, AltiVec, no fsqrt, no 64-bit). |

Runtime files:

* `start.S`
  * `_start`: Linux PPC32 entry, with argc/argv read from the initial r1.
    It builds the first frame, sets VSCR to non-Java mode (NJ=1, so vector
    denormals flush to zero: the Linux default, and what the Xenon VMX
    does), and calls `qrt_main`.
  * `qrt_syscall` (`sc`, r0 = number, CR0.SO error convention) and
    `qrt_exit` (exit_group 234, falling back to exit 1).
  * the VSCR accessors. qemu honours NJ: a denormal `vec_madd` gives 0 with
    NJ=1 and keeps its value with NJ=0.
* `savres.S`: `_savegpr_N`, `_restgpr_N[_x]`, `_savefpr_N`, `_restfpr_N[_x]`,
  `_savevr_N`, `_restvr_N`. GCC calls these at `-Os`. They are
  instruction-for-instruction identical to libgcc's crtsavres members (132
  symbols, checked against testxenon3's objects).
* `qrt.c`: buffered `write(1)` output, a static 32 MB + 64 KB arena aligned
  to 64 KB (in .bss), and handlers for SIGSEGV/SIGILL/SIGBUS/SIGFPE/SIGTRAP/
  SIGABRT on an alternate stack. The handlers print the context and the
  PPC32 ucontext pc/lr. `qrt_host.c` is the hosted equivalent for the host
  builds.
* `qrt_fmt.c`: strings, decimal, hex, and doubles as their bit pattern plus
  a rough 12-digit decimal. Portable, so qemu and host print the same text.
* `qlibgcc.c`: every libgcc helper that 32-bit code needs at
  -O0/-O1/-O2/-O3/-Os for both compilers (found with `xenon-nm -u`):
  * `__udivdi3 __umoddi3 __divdi3 __moddi3 __udivmoddi4 __divmoddi4`
  * `__ashldi3 __lshrdi3 __ashrdi3 __muldi3 __negdi2 __cmpdi2 __ucmpdi2`
  * `__popcount{si,di}2 __parity{si,di}2 __clz{si,di}2 __ctz{si,di}2
    __ffs{si,di}2 __clrsb{si,di}2 __bswap{si,di}2`
  * `__float{,un}di{df,sf} __fix{,uns}{df,sf}di __fixuns{df,sf}si`
  * `__powidf2 __powisf2`

  They are built from 32-bit word operations only, so they can never call
  themselves, and the object has no undefined symbols. powi uses libgcc's
  exact algorithm. int64-to-float conversions use libgcc's sticky-bit
  method (correctly rounded).
* `qmath.c`:
  * `sqrt`/`sqrtf`: GCC calls these for every square root on the G4. They
    are exact digit-by-digit integer square roots, so they are correctly
    rounded and bit-identical to a hardware `fsqrt`, with IEEE special
    cases.
  * the sin/cos/exp/log stand-ins: Cody-Waite reduction plus Taylor/atanh
    series, at most 2 ulp from glibc.
* `qblib.c`: the BLIB stand-ins `memcpy memmove memset memcmp strlen strcmp
  strchr qsort` (qsort is a heapsort) and `sinf cosf expf logf sqrtf sin cos
  exp log sqrt`. There is one implementation, exported as both
  `xenon3_lib_*` and `old16_lib_*`. Plain `memcpy/memmove/memset/memcmp`
  are exported too, for calls GCC generates.
* `qrunner.c`: the runner (above). `qselftest.c` + `qst_main.c`: the helper
  self-test.
* `qhosttest.c`: the host unit test of `qlibgcc.c`/`qmath.c`/`qblib.c`,
  built with `-DQLG_HOST` (names prefixed `qlg_`). It runs once per change
  of those files, and the run takes about 10 s:
  * 4M divisions and 12.8M shifts;
  * 3M bit-op cases and 12M conversion cases;
  * 2M powi cases;
  * 20M `sqrt` and 46M `sqrtf` cases. For `sqrtf`, that is every mantissa
    of an even and an odd exponent, every subnormal and the top exponent;
  * ulp error of the math stand-ins over several ranges;
  * mem/str/qsort against glibc.

  Everything is bit-exact against native x86-64 arithmetic.

Outputs, all in `build/qemu/`:

* `bench-qemu.elf` and `bench-qemu.map`;
* `qselftest.elf`;
* `xenon3/`, `old16/` and `rt/` (objects);
* `host/` (host builds and `qhosttest.log`);
* `qemu-run.txt` (the last runner output, including the `@R` lines);
* `host-run.txt` (the cross-check).

## Limitations

* **Not the real code.**
  * There is no `-mpowerpc64`. 64-bit arithmetic becomes 32-bit sequences
    and libgcc calls. Divisions by 64-bit values, int64/FP conversions and
    popcounts take very different paths from the real build, which uses
    `divd`, `fcfid` and so on inline.
  * There is no VMX128 (v32-v127, `vmaddcfp128`, ...). Register allocation,
    spills and auto-vectorisation differ.
  * Tuning is `-mcpu=7400` for both compilers instead of each one's
    default (xenon and cell).
  * There is no `fsqrt`: every square root is a call to the runtime's
    `sqrt`.

  So this check validates the *sources* and the compilers' 32-bit code
  generation. A miscompile that only shows with the real flags cannot be
  seen here. That is why the instruction scan exists: it proves that what
  ran is pure G4 code.
* **No timing.** qemu's speed says nothing about Xenon, so nothing is timed.
  The whole run takes about 1.5 s.
* **Not newlib.** LIB kernels call the stand-ins in `qblib.c`, the same code
  for both variants. That tests the kernel code and the BLIB calling
  convention, but not either toolchain's newlib.
* **qemu is not a G4.**
  * qemu-user models no caches and no memory ordering.
  * qemu's `vrefp`/`vrsqrtefp`/`fres`/`frsqrte` are near-exact, where the
    hardware returns 12-bit estimates.
  * VSCR[NJ] is honoured.
  * FP exception flags are not checked.
* The BE/LE cross-check is informational. Some kernels are legitimately
  endian-dependent.

## Toolchain findings from this check (for kernel authors)

* **AltiVec integer multiplies do not build with the real flags.**
  * testxenon3 (default `-mcpu=xenon`) rejects `vec_mule`, `vec_mulo`
    (bytes and halfwords), `vec_mladd`, `vec_msum`, `vec_madds`,
    `vec_mradds` and `vec_sum4s` with "not supported on Xenon: the '...'
    instruction is not implemented by the Xenon PPE".
  * test16old (default cell) hits an internal compiler error in
    `expand_insn, optabs.cc:8518` on `vec_mule`/`vec_mulo` of bytes and on
    `vec_mladd`, `vec_msum`, `vec_madds`, `vec_mradds` and `vec_sum4s`, at
    `-mcpu=7400` too.
  * `vec_perm`, `vec_packsu`, `vec_madd`, `vec_re`, `vec_rsqrte`,
    `vec_ctf`, `vec_cts`, merges, shifts, `vec_avg` and min/max work on both.

  DESIGN.md's `vblend` recipe (`vec_mule`/`vec_mulo`) therefore cannot be
  built as written. The real `src/kernels/vec_vblend.c` already works
  around it, as does `stubs/vec_vblend.c`: widen the bytes to 32-bit lanes,
  blend with `vec_madd` in float (exact for 8-bit products), convert back,
  shift and pack. Any new VEC kernel should avoid the intrinsics listed
  above.
* **test16old miscompiles a plain-C SAD reduction at -O2.** The loop is
  `sad += abs(a[x] - b[x])` over `uint8_t`, with AltiVec on.
  * The vectorised loop loses its loads. `vec_sad16`'s `run()` returns
    `0x00000000` on test16old, against `0x95dd045c` on testxenon3 and the
    host.
  * The same degenerate code is generated with the real flags (cell,
    `-mpowerpc64`). Its `sad16x16` sets `r3 = 0` and sums zero vectors, so
    in the real A/B image the sad16 kernel of B does no work.
  * The result is correct at `-O1`, with `-fno-tree-loop-vectorize`, and
    with `-mno-altivec`.
  * The ternary abs, an `int` accumulator and a `uint16_t` row sum all
    fail the same way. `sad += a[x] > b[x] ? a[x] - b[x] : b[x] - a[x];` is
    correct on both compilers and vectorised by both at `-O2`.
  * Root cause (integration): test16old's `gcc-16.2.0.diff` changes the
    conditions of the AltiVec `vmsum*`, `vmul[eo]*`, `vsum4*`/`vsum2sws`/
    `vsumsws`, `vmladduhm` and `vmh[r]addshs` define_insns to
    `(TARGET_ALTIVEC && 0)`, but the standard-name expanders that emit them
    (`widen_ssum`/`widen_usum`, `sdot_prod`/`udot_prod`, the widening
    multiplies, ...) stay enabled. The vectoriser picks `WIDEN_SUM_EXPR`
    (the vect dump says "widen_sum pattern recognized"), the `vmsumshm` it
    expands to is dropped, and the accumulator is never set. Through the
    builtin path the same patterns hit the `expand_insn` ICE instead.
  * The kernel keeps the textbook `abs()` loop (DESIGN.md asks for it), so
    the console run shows sad16 as WRONG B (B differs from A and from the
    reference result in `src/harness/refs.h`): the benchmark detecting a
    real test16old bug. `known-failures.txt` pins this exact failure.
* **-O3 is not usable with these toolchains.**
  * test16old ICEs on `mix_idct8.c` (`expand_insn, optabs.cc:8518`, the
    same disabled patterns: `idct8_init`'s quantisation loop is vectorised
    with a vector multiply), with the qemu and the real flags.
  * With the qemu flags both compilers build wrong code for the sad16 loop
    nest (outer-loop vectorisation): A gives `0x520b1fde`, B `0x67d971c3`.
    testxenon3 chains its "optimised realignment" (`lvsr` of `-addr`, then
    `vperm` with the previous row's second block) across rows that are 128
    bytes apart, so row k is assembled from row k-1's high block. With the
    real flags testxenon3 uses `lvlx`/`lvrx` instead of realignment there,
    and that code could not be run.
  * qemu's `vmsumshm` was checked against the ISA definition (400,000
    lanes). The default `-O2` build of testxenon3 is scalar and correct.
* With the 32-bit flags, GCC emits a `memcpy` call in `sha256_run` and a
  `memset` call in `vxform_init`. This does not happen with the real flags,
  where kcheck passes.
