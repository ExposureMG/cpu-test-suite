# Xenon CPU hardware findings for implementers

Scope: one console (Jasper, PVR 0x00710500), runs of 2026-10-19, XTS bare-metal tests under libxenon (XeLL-loaded
ELF32, hypervisor state). Default conditions unless a row says otherwise: SF=64, sibling thread in libxenon's idle
loop, other cores idle, code_align 0. Units: CPU cycles at 3.2 GHz; timebase 49.876 MHz = 64 cycles per tick (calib,
6 runs, TB/RTC ratio 1.0000). Semantics that differ from the architecture are also in `src/model/KNOWN.md`; the
per-instruction table is `docs/findings_instructions.csv` (regenerate: `tools/findings_table.py <schedmodel.json>`).

Evidence shorthands:
- **SM**: scheduling model (`tools/xts_schedmodel.py`) over the 7 newest runs: sched `0025ae045b06f1d1`,
  `0025ae042766b054`, `0025ae04591bd080`; timing `0025ae0426d0144c`; mem `0025ae045da71568`; smt `0025ae0458d1030b`;
  semantic `0025ae0459488217`. "n/r" = records / runs that agree. `RN` = repeatable_noisy (each run's fit was noisy,
  but >= 2 runs reproduce the value within 2 %).
- **results4**: the 10,776 OpenXenos hardware cases replayed by the XTS model (0 mismatches outside estimate tables).
- **sem**: semantic runs of 2026-10-19; `0025ae0459488217` has 0 failures (67,907 pass, 12,052 info, 346 skip).

Confidence: **hardware-confirmed** (decisive hardware cases), **repeatable** (timing that several runs agree on),
**single run**, **unverified** (model assumption / not yet measured).

---------------------------------------------------------------------------------------------------------------------

## 1. Instruction semantics that differ from PowerPC 2.02 / AltiVec / VMX128

### 1.1 FPSCR, VSCR, scalar FP

| Fact / value | Evidence | Confidence | Implementation note |
|---|---|---|---|
| FPSCR[NI] (0x4) cannot be set | results4: 196 cases with input FPSCR=4 read back NI=0 (184 executed, 12 trapped); `mtfsb1 29` does not set it (1 case); `mtfsf` FLM 5+7 with FRB all-ones reads back `...070B` | hardware-confirmed | Emulator: mask NI on every FPSCR write, no non-IEEE mode. Compiler: no NI-based denormal flushing |
| FPSCR bit 20 (reserved, 0x800) cannot be set | sem: 593 FP cases with the bit set in the input read it back as 0, over 35 opcode-59/63 mnemonics; results4 mtfs* | hardware-confirmed (whether only FP instructions clear it: unverified, no non-FP case had it set) | Emulator: clear 0x800 after every FPSCR write |
| `mtfsf 0xFF` does not write FEX/VX (summary bits); `mtvscr` keeps only NJ and SAT and zeroes the other VSCR words | results4 (KNOWN.md "facts the framework applies") | hardware-confirmed | Emulator: recompute FEX/VX; mask VSCR to NJ\|SAT |
| `mtfsb1` sets FX only for an exception bit going 0->1; `mtfsfi` to a field other than 0 never sets FX | results4 (OX UX XX VXSNAN VXIMZ VXVC VXSQRT VXCVI seen) | hardware-confirmed | Emulator FX logic |
| `mffs` high word = 0xFFF80000 | results4 24/24 (all with FPSCR=0) | hardware-confirmed | Emulator |
| Single-precision ops (`fadds fsubs fmuls fdivs fsqrts fmadds fmsubs fnmadds fnmsubs`) take operands of single-denormal magnitude (2^-149 <= \|x\| < 2^-126, incl. low mantissa bits a single cannot hold) as ordinary values at full double precision, rounded once to single | sem: 84 records (exponents 874, 886, 896), all four RN modes, UX/XX/FI/FR, FPRF | hardware-confirmed | Emulator: do not flush/round the operand first; compute exactly, round once |
| A finite nonzero operand with \|x\| < 2^-149 or \|x\| >= 2^128 (no NaN, no invalid, no infinity involved) gives 0x7FF8000000000000, FR=FI=0, FPRF=QNaN, no exception bit; an infinite operand still wins; `fdivs`/`fsqrts` with such an operand compute in double | results4 (QNaN at exponents <= 26 and 2001-2040), sem (ordinary at 874 and 1150) | hardware-confirmed; exact cut-offs (exponents 27-873, 1151-2000) unverified | Emulator: operand range check on single ops; compiler: keep single-op inputs in single range |
| Enabled underflow (UE=1): the exponent-adjusted tiny result (+192 single/frsp) is classed **normal** in FPRF; UE=1 with an exact tiny result still sets UX | sem: 8 frsp records + fadds/fmuls/fnmadds UE cases | hardware-confirmed | Emulator FPRF |
| Disabled overflow sets FR (frsp/fdivs, RN=0 and 1; architecturally undefined) | results4 | hardware-confirmed | Emulator |
| fmadd family: inf*0 sets VXIMZ even when the addend is a NaN (SNaN addend: VXSNAN+VXIMZ) | results4, 1 case | single run (1 case) | Emulator |
| Single-precision results keep the double NaN payload (`fadds 0.1, 0x7FF0000000000001` = `0x7FF8000000000001`) | results4 | hardware-confirmed | Emulator: no payload truncation on single ops |
| `fctiw[z]` result sign-extended to 64 bits; NaN -> 0xFFFFFFFF80000000 | results4 | hardware-confirmed | Emulator |
| `frsp` of a NaN: low 29 bits cleared (Book I) or kept | results4 has only the default QNaN input | unverified | Follow Book I until measured |
| Tininess detected before rounding | no discriminating case (results4 has 18 UX cases) | unverified | Decisive case in KNOWN.md (`fmul 0x3FEFFFFFFFFFFFFE, 0x0010000000000001`) |

### 1.2 Estimates

| Fact / value | Evidence | Confidence | Implementation note |
|---|---|---|---|
| `fres` keeps 8 significant bits (`fres(1.0)` = 255/256), `frsqrte` 9; vrefp/vrsqrtefp look piecewise linear with full-width significands | results4: 224/224 vrefp/vrsqrtefp lanes within 2^-12, fres within 2^-8, frsqrte within 2^-5 (29/29) | hardware-confirmed (bounds); tables unknown | Emulator needs the exact tables (189 results4 cases are table values); a bounded approximation only passes the architected checks |
| Estimates never set XX/FI/FR and do not depend on RN; fres overflow at the single range sets OX only (value by RN); `fres` of \|x\| >= 2^128 gives a signed zero without UX/XX | results4 | hardware-confirmed | Emulator flag logic |
| `fres` with FPSCR[OE]=1 and \|x\| < 2^-129 returns the 8-bit table value x 2^-192 with x's sign (enabled-overflow adjust), not inf/max | sem: 4 records (\|x\| 2^-1074 .. 2^-238) | hardware-confirmed; boundary exponents -129/-128 and the FPSCR flags unverified | Emulator |
| `vexptefp`/`vexptefp128` flush every denormal result to +0 even with VSCR[NJ]=0 (x <= -127 -> +0; 2^-126, 2^-125 exact) | sem: 24 records | hardware-confirmed; NJ=1, x in (-127,-126), and the same flush for vrefp/vlogefp unverified | Emulator: x <= -127 -> +0 |

### 1.3 VMX / VMX128

| Fact / value | Evidence | Confidence | Implementation note |
|---|---|---|---|
| `vsl`/`vsr` shift **per byte**: result byte i uses count `vB.byte[i] & 7`, shifted-in bits from the next (vsl; 0 past byte 15) / previous (vsr) byte of vA | results4: 41 of 48 cases (unequal counts) differ from the whole-register rule; all 48 match per-byte | hardware-confirmed | Emulator: per-byte rule (the AltiVec "undefined" case is defined on Xenon) |
| `vslo`/`vsro` (and 128) use vB bits 121:124; `vsplt*` mask the index (UIMM & 15/7/3); `vspltisw128` ignores VB128; `vsel128` takes the mask from the old VD128 | results4 (131 vector-integer mnemonics, 3,144 cases, 0 mismatches) | hardware-confirmed | Emulator |
| VSCR[SAT] set only when a lane actually saturates; arithmetic never changes VSCR[NJ] | results4 | hardware-confirmed | Emulator |
| VMX128 FMA (`vmaddfp128 vmaddcfp128 vnmsubfp128`): fused, one rounding, always RNE, FPSCR untouched; orders `VA*VB+VD`, `VA*VD+VB`, `-(VA*VB-VD)` negated after rounding (exact cancellation -> -0); NaN = first of (1st multiplicand, addend, 2nd multiplicand) quieted, never negated; invalid -> 0x7FC00000; NJ=1 flushes inputs and results | results4 216/216 VMX128 specials; fmastat results17 192/192 FMA | hardware-confirmed (two-different-NaN order: 0-2 lanes, unverified) | Emulator: true fused FMA with the per-opcode operand order |
| `vmsum3fp128`/`vmsum4fp128` are not IEEE: exact products truncated to an accumulator LSB 2^(emax-28) (emax = largest ea+eb of the nonzero products, denormals at their normalized exponent); one's-complement sum relative to the **majority product sign** (2:2 tie -> +), each minority product costs one LSB; result truncated to 24 bits; exponent overflow -> QNaN with the result's sign (never inf); \|r\| in [2^-127, 2^-126) keeps its fraction with exponent field 0 (also NJ=1); zero magnitude -> +0, all products -0 -> -0; any NaN, inf*0, inf-inf -> 0x7FC00000; vmsum3 ignores lane w; VSCR unchanged | results4 48/48 + sem 151/151 (199 cases; the only matching parameter combination; first-product sign fails 151/151); zeros: 70 (+0), 8 (-0), 22 (+0 mixed) | hardware-confirmed; product rounding before truncation, overflowing sums of in-range products, more 2:2 ties, NJ=1 in [2^-127,2^-126) beyond 1 case unverified | Emulator: implement the accumulator exactly (an IEEE dot product is off by an LSB or the sign); `src/model/vmx128.c` is a reference |
| `vpkd3d128`/`vupkd3d128`: integer D3D formats are floats biased by 3.0 (code = bits - 0x40400000); pack clamps and sets SAT when it clamps; NaN packs to the most negative code (0x8000 / 0x200), unpack turns that back into 0x7FC00000; FLOAT16 = Xenos half (saturates to 0x7FFF\|sign, no inf/NaN, never sets SAT); **FLOAT16_4 truncates** like FLOAT16_2 | results4; SAT 0->1 in 10 cases; sem: 48 records, 66 lanes where round-to-nearest-even is one too high, 0 rounding up | hardware-confirmed; type 6/7, pack size 0, unpack UIMM 8-31 and several (type, pack) pairs unverified (list in KNOWN.md) | Emulator: truncate FLOAT16_4 (xenia rounds to nearest) |
| `vpermwi128`: `VD.i = VB[PERM >> (6-2i) & 3]`; `vrlimi128`: `VD.i = rotl_words(VB, z)[i]` where IMM bit (3-i) is set, else VD.i | results4 (vpermwi: 23 of 256 PERM values; vrlimi: IMM 2, 4 and IMM bit 4 never ran) | hardware-confirmed for the values run | Emulator |
| Vector numeric details: vctuxs/vctsxs NaN -> 0 without SAT; vcmpbfp NaN -> 0xC0000000; vmaddfp NaN order A, B, C; vnmsubfp negates after rounding; vmulfp128 = rounded product | results4 (OpenXenos SHORTCOMINGS 8.6 table reproduces) | hardware-confirmed | Emulator |
| AltiVec integer multiply family (`vmul*`, integer `vmsum*`, `vmhadd*`, `vmladd*`) is illegal on Xenon | exec.illegal 552 records pass in sem (29 hardware-illegal words + invalid forms raise cause 0x08) | hardware-confirmed | Compiler: never emit; emulator: program interrupt |

### 1.4 Integer, CR, loads and stores

| Fact / value | Evidence | Confidence | Implementation note |
|---|---|---|---|
| `lswx` with XER byte count 0 behaves as count 1: the byte at EA lands in the top byte of RT's low word, the rest of RT is zero (EA = scratch+0x85: 0x00000000C8000000); RT=RA or RT=RB still raises illegal (cause 08) | sem `lswx.count0#0-7` at SF=64 and SF=32: 14/14 meaningful cases, plus `lswx#18` | hardware-confirmed | Emulator: n = 0 -> 1 for lswx (architecture: RT undefined) |
| `stswx` with count 0 stores nothing | sem 16/16 | hardware-confirmed (architected) | - |
| `mfocrf` ignores FXM and returns the whole CR (RD bits 0-31 = 0) | results4, ~20 multi-bit-FXM cases | hardware-confirmed (single-bit FXM with nonzero CR unverified) | Emulator: mfocrf == mfcr |
| `mtocrf` with several FXM bits writes only the most significant selected field; FXM=0 writes nothing | results4, ~15 cases | hardware-confirmed | Emulator; compiler: multi-field writes need `mtcrf` (20 cycles, 3.6) |
| `mulhw`/`mulhwu`: architecturally undefined upper word = sign / zero extension of the high word | results4 (e.g. -2*0x7FFFFFFF -> 0xFFFFFFFFFFFFFFFF) | hardware-confirmed | Emulator |
| `mulh*` with OE=1 and Rc=1 is illegal | OpenXenos sweep `reserved_forms_illegal` | hardware-confirmed | Emulator decoder (OE=1, Rc=0 = plain mulh: unverified) |
| Word divides use the low words, quotient zero-extended (`divw 0x80000011/7 -> 0x00000000EDB6DB71`); OV=0 for ordinary operands | results4 | hardware-confirmed | Emulator |
| Divide by zero, 0x80000000/-1, INT64_MIN/-1 | no case in results4 or the timing runs; the model returns 0 and sets OV | unverified | Measure before relying on any value |
| `stwcx.`/`stdcx.` with Rc=0, X-form update forms with RA=0, load-update with RA=RT are illegal (0x700, cause 0x08) | sweep: 28 X-form words; D/DS-form update forms extrapolated (not swept) | hardware-confirmed (X-form) / unverified (D/DS-form) | Emulator decoder |
| `lvebx`/`lvehx`/`lvewx`/`lvewx128` load the **whole aligned quadword** (architecturally undefined lanes); `stvebx/stvehx/stvewx` store only the selected element; `lvx`-family ignore EA[28:31]; `lvsl/lvsr` do no access | results4 (114 ld/st mnemonics, 2,736 cases, 0 mismatches) | hardware-confirmed | Emulator: fill all 16 bytes |
| Cell `lvlx/lvrx/stvlx/stvrx` (+ `l` and 128 forms, identical): lvlx loads EA..end of quadword into the high end, zero below; lvrx loads quadword start..EA-1 into the low end (EA aligned: all zero, no access); stores are the inverse | results4 | hardware-confirmed | Emulator |
| `lfs`: an SNaN stays an SNaN, single denormals are normalised; `stfs`: exponent > 896 is a bit shuffle with **no rounding** (2^128 stores 0x7F800000, NaN payload kept), 874..896 denormalised by **truncation**, smaller nonzero -> signed zero; neither touches FPSCR; `stfiwx` stores FPR[32:63] | results4 (every stfs-family case) | hardware-confirmed | Emulator: no host float conversion for stfs |
| Alignment: integer and vector loads/stores never trap on a misaligned EA; FP loads/stores (all forms, also 8-byte) take an alignment interrupt unless EA % 4 == 0 (EA % 8 == 4 executes); lwarx/stwcx. need EA % 4, ldarx/stdcx. EA % 8; alignment is checked before translation (unmapped misaligned EA -> alignment, not DSI) | results4 | hardware-confirmed | Emulator exception order; compiler: at least 4-byte-align doubles |
| `dcbz` and `dcbz128` both zero the 128-byte line containing EA | results4 | hardware-confirmed | Emulator: dcbz zeroes 128 bytes, not 32 |
| One reservation per thread: set by every lwarx/ldarx that completes, cleared by every stwcx./stdcx. that completes (success or not); success iff set and in the same 128-byte granule; traps neither set nor clear it; it **persisted ~300 jobs** (an ldarx at line+64 made a later stwcx. at line+12 succeed) | results4: one success, every later stcx failed | single run (one decisive case; granule >= 128 B inferred) | Emulator: keep the reservation across unrelated code; 128-byte granule |
| mtspr XER keeps 0xE000007F only | no hardware case | unverified | - |
| SF=0 (32-bit mode) CR0/CA/OV, EA truncation | all results4 cases ran SF=1 | unverified (per KNOWN.md) | - |

---------------------------------------------------------------------------------------------------------------------

## 2. Pipelines and issue

### 2.1 Issue and pairing (issue.pair; SM 4 records / 4 runs per ordered pair)

| Fact / value | Evidence | Confidence | Implementation note |
|---|---|---|---|
| One thread sustains ~1 simple instruction per cycle: 1-cycle filler (`addi`/`li` stream) 1.020 cycles per instruction | SM filler 8/4; frontend method run `0025ae04268cf9e8` (li 1.020) | repeatable | The 0.02 is the layout bias (section 9) |
| Two 1-cycle instructions from **different** issue groups cost 1.35 cycles per pair (overlap 0.66-0.68), never 1.0; two from the **same** group cost 2.0 | e.g. int+fp 1.345, int+lwz 1.346, vfp+vperm 1.345; int+int 2.04, fp+fp 2.01 | repeatable | Scheduler: 2 slots, but a different-group pair costs ~1.35 cycles; interleave integer ops with load/FP/VMX ops |
| Order matters: lwz then FP/VMX 1.35, FP then lwz 2.04, VMX (vfp, vlogic, vperm) then lwz 2.56; int/lwz/FP then stw 2.04, stw then int/lwz 3.00, stw then FP/VMX 2.93; vfp then vlogic 3.12 | pair matrix below | repeatable | Put loads and integer ops before FP/VMX ops and before stores in a pair |
| Stores and VMX loads are expensive in pairs: stfd with anything 3.8-4.0, stfd+stfd 7.2; stvx with int/FP 3.3, with lwz 2.04; lfd with int/FP 3.3, lfd+lfd 5.84; lvx128 with anything 10-12, lvx+lvx 23.0 | pair matrix | repeatable | - |
| 402 ordered class pairs have verdict "co-issue", mostly a long busy instruction (div, mul, mtcrf, recimm, varshift, b) hiding a cheap partner (verdicts: yes 402, partial 582, no 418, interfere 301, undetermined 697, anomaly_faster 172) | SM pairing | repeatable | Use the cycle matrix, not the verdict |

Ordered pair cost, cycles per (first; second) pair of independent instructions (each alone 1.02 for the simple classes):

| first \ second | int | cmp | crlogic | lwz | fp | vfp | vlogic | vperm | stw | stfd | stvx | lfd | lvx |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **int** | 2.04 | 2.03 | 1.35 | 1.35 | 1.35 | 1.35 | 1.35 | 1.35 | 2.04 | 3.83 | 3.34 | 3.33 | 11.29 |
| **cmp** | 2.04 | 2.03 | 2.03 | 1.35 | 1.35 | 1.35 | 1.35 | 1.35 | 2.04 | 3.83 | 3.34 | 3.33 | 11.29 |
| **crlogic** | 1.35 | 2.03 | 2.03 | 1.35 | 1.35 | 1.35 | 1.35 | 1.35 | 2.98 | 3.83 | 3.34 | 3.33 | 11.29 |
| **lwz** | 1.35 | 1.35 | 1.35 | 2.04 | 1.35 | 1.35 | 1.35 | 1.35 | 2.04 | 3.96 | 2.04 | 2.04 | 12.04 |
| **fp** | 1.35 | 1.35 | 1.35 | 2.04 | 2.01 | 2.01 | 2.01 | 1.35 | 2.04 | - | 2.63 | 2.04 | 10.27 |
| **vfp** | 1.35 | 1.35 | 1.35 | 2.56 | 2.01 | 2.01 | 3.12 | 1.35 | 2.04 | 3.96 | 2.04 | 2.04 | 10.37 |
| **vlogic** | 1.34 | 1.35 | 1.35 | 2.56 | 2.01 | 2.15 | 2.01 | 1.35 | 2.04 | 3.96 | 2.04 | 2.04 | 10.34 |
| **vperm** | 1.35 | 1.35 | 1.35 | 2.56 | 1.35 | 1.35 | 1.35 | 2.01 | 2.04 | - | 2.04 | 2.04 | 10.25 |
| **stw** | 3.00 | 2.03 | 2.98 | 3.00 | 2.93 | 2.93 | 2.93 | 2.93 | 5.81 | - | - | 2.04 | 10.26 |
| **stfd** | 3.76 | 3.84 | 3.81 | 4.00 | 3.76 | 3.76 | 3.76 | 4.03 | 7.05 | 7.20 | 7.46 | 3.98 | 10.37 |
| **stvx** | 3.34 | 3.31 | 3.31 | 2.04 | 3.34 | 3.34 | 3.34 | - | - | 7.18 | 5.83 | 2.04 | 10.38 |
| **lfd** | 3.33 | 3.30 | 3.33 | 2.17 | 3.33 | 3.33 | 3.34 | 3.47 | 2.04 | 4.02 | 2.04 | 5.84 | - |
| **lvx** | 11.16 | 11.54 | 11.54 | 2.63 | 11.14 | 11.29 | 11.22 | 11.23 | 6.92 | 10.55 | 10.55 | 12.79 | 22.98 |

(`-` = no accepted value. Classes: int = add, lwz = lwz, fp = fadd, vfp = vaddfp128, vlogic = vand128, vperm = vperm128,
stw = stw, stfd = stfd, stvx = stvx128, lfd = lfd, lvx = lvx128. logic/rotimm/mtlr behave like int; lbz/ld like lwz;
fma/fconv/vint like fp; vsplt like vperm.)

### 2.2 Pipe groups and unit conflicts (unit.conflict)

| Fact / value | Evidence | Confidence | Implementation note |
|---|---|---|---|
| 13 pipe groups: `int_cmp_crlogic` {int, cmp, crlogic}, `fp_vfp_n6` {fp, lwz, vest, vfp, vlogic, vperm}, `lfd`, `lvx`, `stw`, `stfd_stvx` {stfd, stvx}, `br_t`, and six issue-blocking singletons `div`, `mul`, `varshift`, `recimm`, `fdiv`, `fsqrt` | SM pipes.groups (edges: 51 shared, 99 partial, 23 separate; 20 edges single run) | repeatable values; the grouping is the tool's vote (model assumption) | LLVM: one ProcResource per group; classes without their own measurement use a fallback (CSV note "pipe_group inferred") |
| Shared (overlap ~0) mixes: lwz with fp/vfp/vlogic/vperm (-0.01..-0.014) and vest (0.15); int with cmp, mul (0.02), varshift (0.07), div (0.03); fp with vfp, vlogic, vest, fdiv, fsqrt, recimm; most other mixes partial (overlap 0.6-0.7) | SM edges, 4 runs each | repeatable | Integer loads compete with FP/VMX arithmetic in mixed streams (but see the ordered pairs in 2.1) |
| Issue-blocking units, busy cycles imposed on a following stream: div blocks int 31.5, lwz 30.7, mul 29.3; mul blocks int 10.8, div 10.0; varshift blocks int 17.1, mul 15.9; recimm (`andi.`) blocks int 15.7; fdiv blocks fp 81.9 and vfp 81.9; fsqrt blocks fp 91.0, fdiv 84.0 | SM pipes.busy 4/4 (fsqrt/fp 3/3) | repeatable | LLVM: ReleaseAtCycles = busy cycles on the blocked groups; emulator timing: stall the thread's integer issue during div/mul/varshift |
| Undetermined busy: div/fp 22.4, fdiv/int 13.8, fdiv/lfd 60.3, vest/vfp 3.0 | SM pipes.busy (RN) | repeatable value, verdict undetermined | Treat fdiv as not blocking integer (partial) |

### 2.3 Microcoded instructions (ucode family: isolated cost + cost to neighbours)

105 variants are microcoded (SM coverage.microcoded, 4-run votes); nearly all also block a neighbour's dual issue.

| Group | Variants | Issue cost (cycles) | Confidence | Implementation note |
|---|---|---|---|---|
| Integer record forms of logical/rotate/shift-imm/cntlz/exts + `andi.` `andis.` | `and. andc. or. orc. nor. nand. eqv. xor. cntlzw. rlwinm. rlwimi. rldicl. rldicr. rldic. rldimi. srawi. sradi. extsb. extsh. extsw.`, `andi.`, `andis.` (22) | 14 (exts. 16) | repeatable | Compiler: never use Rc=1 logical ops or `andi.`; emit the op + `cmpwi`. add/subf/neg/addc with Rc=1 stay 2.02 and are not microcoded |
| Variable shifts/rotates | `slw srw sraw sld srd srad rlwnm rldcl rldcr` and their Rc forms (18) | 16 (Rc 18) | repeatable | Compiler: prefer immediate shifts/rotates (1 cycle) |
| Multiply | `mulli`(7.1), `mulhw mulhwu`(11), `mulld`(13), `mulhd mulhdu`(14), `mullwo`(11), `mulldo`, `mulld.`(26), `mulhw.`(24), `mulhd. mulhdu.`(27) (12) | see column | repeatable | `mullw` base verdict undetermined (11 cycles, unpipelined) |
| Divide | `divw divwu divd divdu` (+ o, .) (12) | 28 issue / 18 latency (small operands); Rc 40 | repeatable | Data-dependent (3.2) |
| Algebraic loads | `lha lhau lhaux lhax lwax lwaux` (6); `lwa` undetermined (24) | 21-24 | repeatable | Compiler: `lhz` + `extsh` (5 + 2 cycles, not microcoded) |
| CR/SPR/FPSCR/VSCR moves | `mfcr`(26) `mtcrf`(20) `mfocrf`(26) `mfspr`(3; XER 25) `mftb`(44) `mcrfs`(13) `mffs`(30) `mfvscr`(40) `mtvscr`(40) (9) | see column | repeatable | `mtocrf` (one field) and `mtspr` are not microcoded (1.02) |
| FP compares | `fcmpu fcmpo` (2) | 16 | repeatable | Compiler: avoid FP compare + branch in hot loops (`fsel` issues in 1) |
| Branches | `b`, `bc` (2) | 8.18 (ucode stream); `bc` 4.99 in issue.pair | repeatable | Section 4 |
| Cache/sync | `dcbz dcbz128` (55.0), `isync` (33.6), `sync` (162.3) (4) | see column | repeatable | - |
| Vector compares with Rc=1 | all 18 `vcmp*.` / `vcmp*128.` | 32 (latency 32) | repeatable | Compiler: avoid `vcmp.` + CR6 branch in loops |
| FP record forms (no ucode verdict, but as slow) | all FP `.` forms | latency and throughput 31.97 | repeatable | Compiler: never use FP Rc=1 |
| Undetermined (38 base mnemonics) | `lvx lvxl lvebx lvewx lvlx lvlxl lfsx lfdx lfsu lfdu lfdux lwa lwarx ldarx stwcx stdcx dcbf dcbst icbi eieio fdiv fdivs fsqrt fsqrts mullw vrefp128 vrsqrtefp lmw stmw lswi lswx stswi stswx sc bclr bcctr eciwx ecowx` | CSV | unverified | Use the CSV issue cost |

### 2.4 Issue cost by class (SM classes, representative instruction; 4 records / 4 runs unless noted)

| Class (representative) | Issue cost | Class (representative) | Issue cost |
|---|---|---|---|
| int, logic, rotimm, filler (`add or rlwinm addi`) | 1.02 | carry (`adde`) | 2.02 (CA chain) |
| cmp, crlogic, mtlr (`cmpi creqv mtspr`) | 1.015 | mflr (`mfspr`) | 3.01 |
| fp, fma, fconv (`fadd fmadd fctiwz`) | 1.006 | fcmp (`fcmpu`) | 16.02 |
| vfp, vfma, vint, vlogic, vperm, vsplt, vcmp, vconv, vdot | 1.005-1.006 | vest (`vrefp128`) | 2.00 |
| lwz, lbz, ld, lwzx | 1.02 | lwzu (update form) | 1.66 |
| lfd, lfs | 2.92 (RN) | lvx (`lvx128`) / lvlx | 11.6 / 6.30 (throughput with rotating destinations 2.93) |
| stw, std | 2.90 | stwu (update form) | 2.49 |
| stfd | 3.58 (RN) | stvx (`stvx128`) | 2.92 (RN) |
| mul (`mullw`) / mul64 (`mulld`) / mulli | 11.0 / 13.0 / 7.09 | div / div64 (`divw` / `divd`) | 28.0 |
| varshift (`slw`) | 16.0 | recimm (`andi.`) | 14.0 |
| fdiv (`fdiv`) | 74.0 | fsqrt (`fsqrt`, rthroughput) | 84.0 |
| b (`b`) | 8.18 | br_t / br_nt (`bc`) | 4.99 (4.45-5.17) |
| mfcr / mtcrf | 26.0 / 20.0 | | |

---------------------------------------------------------------------------------------------------------------------

## 3. Latency and bypass

### 3.1 Dependent latency per class (lat.chain, every source slot equal unless noted; SM 4 records / 2 runs each; throughput tput.indep 3/2)

| Instructions | Latency | Rthroughput | Confidence | Implementation note |
|---|---|---|---|---|
| Integer add/subf/neg/logical/rotate-imm/shift-imm/cntlz/exts (incl. add-type Rc=1) | 2.02 | 1.02 | repeatable | Every simple ALU op has latency 2 |
| Carry ops (`adde addze addme subfe subfze subfme`) | 2.02 (through RA/RB and through CA alone) | 2.02 (CA serialises) | repeatable | addc+adde pair 1.67 cycles; CA read extra ~1.0; `mfxer` after addc 26 |
| Logical/rotate Rc=1, `andi.`/`andis.` | 14.0 | 14.0 | repeatable | Unpipelined; result zero or not and a dependent branch change nothing (andi.->bc 16.1 dependent and independent) |
| exts Rc=1 | 16.0 | 16.0 | repeatable | - |
| Variable shifts/rotates | 16.0 (Rc 18.0) | 16.0 | repeatable | Unpipelined |
| `mullw`, `mulhw(u)` | 11.0 (Rc 24.0) | 11.0 | repeatable | Unpipelined (depth 1.0); no data dependence (all bins within 5 %) |
| `mulld` / `mulhd(u)` / `mulli` | 14.0 / 15.0 / 8.0 | 13.0 / 14.0 / 7.0 | repeatable | - |
| `divw divwu divd divdu` | 18.0 small operands (Rc 30, OE+Rc 32) | 18.0 | repeatable | Issue cost 28 in the ucode stream; data-dependent (3.2) |
| CR logical, `mcrf` | 1.02 | 1.00 | repeatable | The only 1-cycle-latency ops found |
| `mtocrf`->`mfocrf` / `mtcrf`->`mfcr` round trip | 28.0 / 46.0 | - | repeatable | sysops 3/3 |
| Scalar FP arithmetic, FMA, fsel, fmr/fabs/fneg, frsp, fres, frsqrte, fcti*, fcfid (double and single) | 10.0 | 1.00-1.01 (pipelined, depth ~9.9) | repeatable | FP Rc=1 forms: 31.97 latency and throughput |
| `fdiv(s)` / `fsqrt(s)` | 74.0 / 84.0 (Rc 92.2 / 104.0) | 74.0 / 84.0 (unpipelined) | repeatable | Single and double identical |
| VMX integer, logical, permute, merge, pack/unpack, splat, `vpermwi128 vrlimi128 vpkd3d128 vupkd3d128`, `vcmp*` (integer and FP), `vmaxfp vminfp` | 4.0 | 1.00 | repeatable | - |
| VMX FP add/sub/mul/madd/nmsub (AltiVec and 128), conversions (`vcfsx vcfux vctsxs vctuxs v*xwfp128 vcfp*xws128`), `vrfi*`, `vexptefp vlogefp` | 12.0 | 1.00 | repeatable | - |
| `vmsum3fp128 vmsum4fp128` | 14.0 | 1.01 | repeatable | - |
| `vrefp vrsqrtefp` (+128) | 14.0 | 2.00 (half-pipelined, depth ~7) | repeatable | - |
| `vcmp*.` (Rc=1) | 32.0 | 32.0 | repeatable | Unpipelined |
| Under a busy sibling thread | integer add chain 2.02 -> 2.42 (x1.20); FP/VMX x1.00 | - | single run (smt) | Section 7 |

### 3.2 Data-dependent timing (lat.data; SM 4 records / 2 runs per bin)

| Fact / value | Evidence | Confidence | Implementation note |
|---|---|---|---|
| Scalar FP with **denormal inputs**: ~47.3 cycles per independent instruction (x45.8), 74 dependent (x7.4); denormal *results* from normal inputs: no penalty (x1.00) | fadd/fsub/fmul/fmadd/fmsub/fnmadd/fnmsub/fres/frsp/frsqrte/fcti* (single and double) | repeatable | Emulator timing: a denormal operand costs ~47 cycles of issue; compiler: avoid denormal inputs (no NI mode, 1.1) |
| Scalar FP with **NaN inputs**: 36.4 cycles independent (x35), 66 dependent (x6.6); inf/zero inputs x1.00 | same set | repeatable | Same |
| `fdiv(s)` with a zero or inf operand 17.0, NaN 24.0, denormal 94.0 (vs 74); `fsqrt(s)` zero/inf/negative 17.0, NaN 24.0, denormal 104.0 (vs 84) | lat.data bins | repeatable | Emulator timing |
| VMX FP with denormal inputs and VSCR[NJ]=0: ~57.7 independent (x57), 90 dependent (x7.5); vmsum3/4fp128 61.8; vrefp/vrsqrtefp 66.7; with NJ=1 the same bins are x1.00-1.02 | vaddfp vmaddfp vnmsubfp vsubfp vmulfp128 vexptefp vlogefp (+128), vrefp, vrsqrtefp | repeatable | Run VMX code with VSCR[NJ]=1 |
| Integer divide by operand size: small or quotient 0: 18.0; wide/wide 32.0; negative wide 44.0; wide or by-one 46.0 (divw: x2.55); 64-bit wide 78.0 (divd: x4.33) | divw/divwu/divd/divdu bins | repeatable | Latency grows with operand/quotient width; compiler: divide by constants via multiply |
| Integer add/logical and multiply: no data dependence (all bins within 5 %) | int and mul bins | repeatable | - |

### 3.3 Bypass / forwarding (bypass.matrix; "extra" = stall beyond the producer's ordinary forwarding to a reference consumer, "stall" = adjacent-pair stall in a stream of independent pairs; SM 3 runs unless noted)

| Fact / value | Evidence | Confidence | Implementation note |
|---|---|---|---|
| Integer result -> load/store **address** (RA of `lwz`/`stw`): +2.5..2.7 extra (stall 3.8-4.0); stall 3.80, 3.83, 1.80, 0.00 at distance 0, 1, 2, 4 | int>lwz.RA0, int>stw.RA0, sweep int>lwz | repeatable | Address generation reads its base ~2.5 cycles before ALU consumers: keep >= 4 instructions between |
| Integer result -> **indexed** address (`lwzx` RA or RB): +6.46 extra (stall 7.7) | int>lwzx.RA0 (3 runs), .RB (2 runs) | repeatable | Prefer D-form addressing after a just-computed index |
| Load result -> load address (pointer chasing): +2.93 extra; stall 3.96, 3.95, 1.92, 0.00 at distance 0, 1, 2, 4 | ld>lwz.RA0 sweep | repeatable | - |
| GPR result -> **store data** (`stw/std` RS): int +2.67, load +2.94, mul +3.12, mflr +2.63; int>stw.RS stall 3.96, 2.93, 0.00 at distance 0, 1, 2 (2 fillers hide it) | int/ld/lwz/mul/mflr > stw.RS | repeatable | Keep 2 instructions between a GPR producer and its store |
| FP result -> `stfd` data: **no stall** at distance 0..16 | fp>stfd.FS sweep (3 runs; distance 0 single run) | repeatable | FP stores may follow their producer immediately |
| VMX FP result -> `stvx` data: full latency, stall 10.95, 10.43, 8.91, 7.04, 3.50, 0.00 at distance 0, 1, 2, 4, 8, 16 | vfp>stvx.VS128 sweep | repeatable | Unlike FP stores, VMX stores wait for the data |
| FP -> FP stall 8.98, 8.73, 7.11, 4.25, 1.29, 0.00 at distance 0, 1, 2, 4, 8, 16; VMX FP -> VMX FP 10.98, 10.72, 9.09, 6.14, 3.10, 0.00 | fp>fp.FA, vfp>vfp.VA128 sweeps | repeatable | Matches latency 10 / 12 at ~1 instruction per cycle |
| VMX FP producers -> vperm/vsplt/stvx/vdot/vest consumers: +0.73..+1.10 extra over a vlogic consumer; vfp -> vlogic none | vfp/vfma/vconv/vdot > vperm, stvx, vfp, vdot | repeatable | Cross-pipe VMX forwarding costs ~1 cycle |
| `lvx` -> `vperm` **control** operand (VC): +8.03 extra | lvx>vperm.VC128 (extra 3 runs; the penalty record itself not accepted) | repeatable (extra only) | Load permute masks early (or build them with lvsl/lvsr) |
| vest (vrefp/vrsqrtefp) -> stvx data: stall 11.8 | vest>stvx.VS128 | repeatable | - |
| Compare -> branch: `cmp*` -> bc 0.00, `add.` -> bc 0.00, `vcmp*.` -> bc 1.69, `fcmpu` -> bc 20.8 (2 runs); `fcmpu` -> CR logical 12.0-12.4 | cmp/int_rc/vcmp_rc/fcmp > br_t.BI | repeatable | GPR compare-and-branch is free; an FP compare feeding a branch costs ~21 |
| Other extras: int -> div RA +0.97; int -> stvx RB +0.52; int -> stfiwx RB +0.03; lfd -> fdiv FA +0.77 | bypass.matrix | repeatable | - |
| `mtctr`->`bctr`, `mtlr`->`blr` | section 4 | - | - |

### 3.4 Load-to-use (ldst.lat; L1 values SM 10 records / 4 runs)

| Fact / value | Evidence | Confidence | Implementation note |
|---|---|---|---|
| GPR loads (`lwz lbz lhz ld`, x/u/brx forms): L1 load-to-use 5.03-5.04 | lat.<form> (index-chain method) | repeatable | Load latency 5 |
| Algebraic loads: `lha` 20.1, `lhau/lhaux/lwaux` 21.3, `lhax/lwax` 22.1, `lwa` 23.3 | same | repeatable | Microcoded (2.3) |
| `lwarx`/`ldarx`: 73.8-73.9 | same | repeatable (RN) | - |
| FP/VR loads (`lfd lfs lvx lvx128 lvlx ...`) 32.9-33.0; `lvebx/lvehx/lvewx` 35.7 | measured through `fcmpu`/`vcmpequw` -> CR -> GPR, assuming FP/VR compares reach CR as fast as cmpd (fcmpu is itself microcoded, 16) | unverified (method upper bound) | Not an FP load latency; the adjacent-pair stall lfd/lfs -> fadd is only 0.12-0.15 (bypass.matrix). Needs a direct FP chain |
| L2 load-to-use per form: GPR 44.3-45.4, algebraic 81.5-88.6, FP/VR (CR method) 98.6-99.7, lwarx 134 | lat.* at cache=l2 | single run | GPR L2 matches the chase (section 5) |
| Cold (flushed) load-to-use: GPR 638-642, algebraic 669-672, FP/VR 670-673, larx 728-729 | lat.* at cache=cold | single run | - |
| Load reciprocal throughput with 8 rotating destinations: lwz 1.03, update forms 1.04, lfd/lfs 2.93, lvx/lvx128 2.93 | ltp.<form>, 5-13 records / 3-4 runs | repeatable | The 6.3/11.5 lvx "issue cost" is a same-register stream; use 2.93 as throughput |
| Store reciprocal throughput: stw/stfd into one line 2.93; into 8 different lines 14.4 | stp.<form> 9-17 / 4 | repeatable | Stores that each touch a different line cost ~14 |

### 3.5 Load-hit-store and cross-file transfers (ldst.lat; 8-9 records / 4 runs, mostly RN)

| Fact / value | Evidence | Confidence | Implementation note |
|---|---|---|---|
| Load-hit-store penalty: GPR store -> GPR load 38.6, FPR -> FPR 39.8, VR -> VR 40.05; the same for exact, inside, covers and partial overlap; another word of the same line (+32 B) or the bytes just before: 0 | lhs.<st>.<ld>.<rel> | repeatable | Emulator timing ~38-40 cycles when a load reads bytes of an in-flight store; compiler: avoid store -> reload (spills, unions, punning) |
| Byte/halfword stores also penalise a load starting in the **same aligned word** (stbx -> loads at EA+1: 38.6; sthx -> loads at EA+2: 38.6), word/doubleword stores do not (stwx/stdx -> next address 0.00); VR: stvebx/stvehx -> element at EA+1/EA+2 40.05, stvewx -> lvewx at EA+4 0.00 | lhs adj_after relations | repeatable values; "aligned 4-byte word granularity" is an inference | Treat byte/half stores as covering their aligned word for LHS |
| Cross-register-file moves through memory (store -> load): GPR->FPR 40.13-40.14, FPR->GPR 38.45-38.46, GPR->VR 40.13-40.15, VR->GPR 38.62, FPR->VR 39.79, VR->FPR 40.05-40.06 | xfer.<st>.<ld> | repeatable | There is no direct GPR<->FPR/VR path: every int<->float move costs ~40 cycles; keep values in one register file |
| Loop-carried round trips through memory (4-byte hops): g-g 49.4, v-v 64.0, f-f 80.0, g-v-g 112.8, g-f-g 121.6, f-v-f 128.0, g-v-f-g 176.0, g-f-v-g 192.2 | rt.<route> 8-9 / 4 | repeatable | Cost of e.g. `(int)float` conversions inside a dependency chain |
| Explicit misalignment, see section 5 | - | - | - |

### 3.6 SPR, CR, FPSCR, VSCR moves, barriers and atomics (sysops; 3 records / 3 runs)

| Operation | Cost (cycles) | Confidence | Implementation note |
|---|---|---|---|
| `mfctr` / `mflr` throughput; `mtctr` / `mtlr` | 3.0 / 1.02 | repeatable | - |
| `mfxer` / `mtxer`; mtxer -> mfxer round trip | 25.0 / 1.02; 27.4 | repeatable | Avoid reading XER |
| mtctr -> mfctr / mtlr -> mflr round trip | 5.28 / 5.34 | repeatable | - |
| `mfcr`, `mfocrf` (any FXM) | 26.0 | repeatable | - |
| `mtcrf` (FXM 01, 80, FF) / `mtocrf` | 20.0 / 1.02 | repeatable | Restore CR fields with one `mtocrf` per field |
| `mffs` / `mffs.` | 30.0 / 46.9 | repeatable | - |
| `mtfsf`, `mtfsfi`, `mtfsb0/1` / their Rc=1 forms | 1.006 / 32.0 | repeatable | Writing FPSCR is cheap, reading it is not |
| `mtfsf` -> `mffs` round trip; `mcrfs` | 61.0; 13.0 | repeatable | - |
| `mfvscr`, `mtvscr`; round trip | 40.0; 80.0 | repeatable | - |
| `mftb` (TBR 268 and 269) | 44.0 | repeatable | - |
| `sync` (= `ptesync`): alone / after a load / after a store | 162.3 / 180.0 / 190.3 | repeatable | - |
| `lwsync`: alone / after a load / after a store | 32.1 / 52.0 / 62.2 | repeatable | Prefer lwsync (5x cheaper than sync) |
| `eieio`: alone / after a load / after a store | 92.0 / 92.0 / 122.0 | repeatable | - |
| `isync`: alone / after a load / after a store | 33.65 / 53.2 / 60.9 | repeatable | - |
| `lwarx`/`ldarx` alone | 64.8-66.0 | repeatable | - |
| `stwcx.`/`stdcx.` without a reservation | 77.7-80.1 | repeatable | - |
| larx + stcx (success) / atomic read-modify-write / reservation lost to a store | 129.4-130.2 / 140.1-140.2 / 155.1-155.3 | repeatable | An uncontended atomic increment costs ~140 |
| Compare issue (`cmp cmpi cmpl cmpli`, L=0 and L=1), CR logical (chain, same field) | 1.015 | repeatable | No same-field penalty |

---------------------------------------------------------------------------------------------------------------------

## 4. Branches and front end

| Fact / value | Evidence | Confidence | Implementation note |
|---|---|---|---|
| Mispredict penalty 25.6-25.8 (2*random - never - always on a 50 % random branch) | SM 25.65 (2 runs); the 5 line-layout sched runs 25.62-25.84; the 5 earlier (old layout) sched runs 26.60-26.68 | repeatable | Use ~26 |
| Conditional branch not taken, or taken to the next word: 4.90 each in a back-to-back branch stream (range 4.83-5.68; bc taken over 0 words 5.51); unconditional `b` to the next word 8.18 | branch pred / tk_bc.d0, SM 3/3 | repeatable | - |
| Taken branch skipping >= 1 word: 8.14-8.21 for `b` and `bc`, independent of distance (1..63 words) | tk_b / tk_bc .d<n>, SM 3/3 | repeatable | A taken redirect costs ~8 in a branch-dense stream; lay out code so the hot path falls through |
| Register branch after `mtctr`: penalty 22.7 (distance 0), 21.1-24.6 with 2..24 independent instructions between, ~0 at 32 | bctr.d<n> (3 runs), bypass.matrix | repeatable | Hoist `mtctr` >= 32 instructions before `bctr` |
| Register branch after `mtlr`: 18.7-19.6 (distance 0), 20.9-24.4 at 2..32 - **not hidden by 32 instructions** | bclr.d<n> (3 runs), bypass.matrix | repeatable | `blr` after a late `mtlr` costs ~20; leave LR to the link stack |
| Link stack: 4 entries | linkstack/depth = 4 in all 10 sched runs; SM RN 3/3 | repeatable | Calls nested deeper than 4 mispredict their returns |
| Call tree cost (bl, mflr/mtlr, blr) by depth: 16.4, 35.5, 54.2, 73.4 (+19.4 per level up to 4), 111.7 (d5), 161.4 (d6), 181.3-181.9 (d7), 223.3 (d8), 299.4-322.0 (d10), 382.7 (d12), 537.4 (d16) | linkstack/tree_d<n>, 10 sched runs | repeatable | - |
| BHT aliasing: no effect for branch pairs 256 B .. 128 KB apart (\|extra\| <= 0.005) | bht.k8..k17, SM 3/3 | repeatable | No need to model BHT index conflicts |
| Repeating direction patterns of period 2..16 are predicted (extra -0.95..-1.19 against the random reference, rate estimate <= 0); period 32: extra +0.18, rate 0.7 %; period 64: extra +0.19 (-0.40..0.78) | pattern.p<n> (p2-p16 2-3 runs; p32 single run; p64 2 runs) | repeatable (p <= 16) / single run (p32, p64) | Predictor learns at least period-16 histories |
| Loop overhead (bdnz loop, bodies of 1-16 `li`): intercept 6.88, 0.84 per body instruction | branch loop, SM 3/3 | repeatable | - |
| **Loop-top placement** (frontend.loop, `bdnz` loop of 128 `li`): per-iteration overhead by loop-top byte offset mod 16: 0 -> 9.0-9.3, 4 -> 3.7-4.2, 8 -> 11.0-11.6, 12 -> 3.7-4.2; only the offset mod 16 matters (all 32 offsets of a 128-byte line) | map run `0025ae0458043b3a` and method run `0025ae04268cf9e8` (agree) | repeatable | Compiler: align hot loop tops to byte 4 or 12 of a 16-byte fetch group (5-7 cycles per iteration better than 0 or 8); emulator: fetch in 16-byte groups |
| Top at byte 0 with 8-word depth steps: per-iteration cost jumps 4-11 cycles from depth to depth (li fit residual 8.9 %, 1.032 per instruction); top at byte 12 and whole 128-byte lines: li 1.020, add 2.018, residual <= 0.06 % | frontend method.old/new (`0025ae04268cf9e8`) | repeatable (map 2 runs; method once) | Fetch-group phase of the loop branch matters |
| Taken `b` over 128 B with 3 nops before the target: target at byte 8 mod 16 costs 10.3-10.8, other phases 7.9-8.2; without the nops (b and bc) every phase 8.07-8.48 | align.b.m3/m0, align.bc.m0, SM 3/3 | repeatable | Avoid targets at byte 8 of a fetch group when the target block starts with filler |
| code_align (kernel offset 4/8/16/32/64 in the line): median x1.00 for every offset; lat.data issue cost x0.97-1.13 at offset 4; branch records x0.75-1.63 at every offset | SM condition_effects code_align=* | repeatable | Only the fetch-group phase matters, not the 128-byte line offset |

---------------------------------------------------------------------------------------------------------------------

## 5. Memory hierarchy

| Fact / value | Evidence | Confidence | Implementation note |
|---|---|---|---|
| L1 D: 32 KB, 4-way (set stride 8 KB); random-chase load-to-use 5.02-5.08 for 4-32 KB | assoc.l1 "4-way, set stride 8K" (single run); chase 30-32 KB 2 runs (RN), knee 32 KB -> 34 KB (5.08 -> 18.1, single run) | repeatable (plateau) / single run (ways, knee point) | - |
| L2: 1 MB, 8-way (set stride 128 KB); load-to-use 44.5-45.3 for 40-256 KB chases, 54-66 for 384-960 KB, 71 at 1 MB | assoc.l2 "8-way, set stride 128K (=> 1024K)" (single run); chase curve 2 runs; knee 1 MB -> 1088 KB (71 -> 367) | repeatable (curve) / single run (ways) | L2 latency ~45; the rise from 384 KB on is not explained |
| The automatic level summary says L2 capacity 256 KB (range 128 KB-1 MB) and L2 latency 49.7 | SM memory.levels / capacity_bytes | single run | Contradicts the 1 MB knee and the associativity probe: it ends the plateau at the 384 KB rise |
| Memory: 629-645 cycles (1.15-32 MB chases), 677 at 64 MB; level summary 640.4 | chase curve 2 runs | repeatable | ~640 |
| Line size 128 bytes | design value (not measured); consistent with 128/256 B strides both hitting L1 (5.02/5.03) and dcbz zeroing 128 B | unverified (design value) | - |
| Stride chase, 64 lines: 128 B 5.02, 256 B 5.03, 1 KB 49.3, 2 KB 54.5, 4 KB 66.2, 16 KB 69.3, >= 64 KB 652-654 | stride.<s> | single run | Large power-of-two strides miss L2 (set conflicts) |
| Address-order dependent chase: 512 KB 0.77x, 1 MB 0.68x of random; from memory 613 (0.95-0.96x) | chase.seq, 2 runs | repeatable | No useful hardware prefetch for dependent loads from memory |
| Streams, bytes per cycle (cycles per 128-B line), cold / L2-resident: 1 ld per line 1.49 (86) / 16.3 (7.9, RN); 16 ld per line 0.18 (712) / 0.97 (132, single); 8 lvx128 per line 0.20 (630) / -; 1 std per line 1.21 (106) / 8.75 (14.6, RN); 16 std per line 0.50 (258) / 2.98 (43); 8 stvx128 per line 0.61 (210) / 4.15 (31, single); dcbz or dcbz128 + 8 stvx128 0.86 (150) / 1.81 (71); dcbz only 3.61 (35) / 5.31 (24, RN) | stream.<kind>.<cold\|l2>, 2 runs unless noted (each unit advances 128 B) | repeatable | Inference: a second access to a line that is still missing waits for the fill (16 loads per line ~ one memory latency per line; 1 load per line overlaps ~7 misses). Compiler: touch each new line once early, then use it |
| Software prefetch 8 lines ahead in a 1-load-per-line stream: dcbt 1.42, dcbtst 1.42, xdcbt 1.42 vs 1.48 B/cycle without (0.96x); placement test (prefetch.xdcbt2) verdict undetermined | pfstream.*, prefetch.xdcbt2 | single run | No benefit seen for this stream |
| Cache-op cost (op minus `ori` in its place): dcbt resident 1.09, dcbtst resident 1.02, dcbtst cold 98.9, xdcbt resident 0.30, dcbz/dcbz128 after a store 35.1, dcbz128 cold stream 23.4, dcbst resident 6.6, dcbf resident 603, dcbf after a store 604 | cop.<op>.<state> | single run | - |
| Cache-op issue cost in an isolated same-op stream: dcbf 92.2, dcbst 92.2, icbi 72.0, dcbz/dcbz128 55.0, dcbt/dcbtst/xdcbt 1.02, dst/dstst/dss 0.67 | ucode, SM 4/4 | repeatable | dcbf 92 vs 603: different line states; use ~600 for flushing a resident line |
| Translation (64 KB pages; one line per page vs the same lines packed): penalty 26-30 per load for 8-768 pages, 204 at 1024, 526 at 1280, 635 at 1536-2032 (one software TLB refill per load); knee 768 pages | tlb.<pages> in mem runs `0025ae045b2fbc7d` and `0025ae045da71568` (agree); probe.tlb 1600-2032 in `0025ae045805e86e` and `0025ae0450a9ddcf` (634.5-636.3) | repeatable | TLB reach ~768-1024 pages (48-64 MB); a TLB miss costs ~635 under libxenon (software refill). The ~27-cycle penalty already at 8 pages is not ERAT capacity (cause unverified) |
| Misaligned loads inside a line: no penalty (mis1, mis4: 0.00 +- 0.02) | mis.<form>.mis1/mis4, 9 / 4 | repeatable | No cost for misaligned GPR/vector accesses inside a line |
| Line-crossing loads, and 4 KB / 64 KB page-crossing (same cost): latency +52.0 (ld, lhz), +54.0 (lwz), +54.0-55.1 (lfd); throughput +51.0 (ld, lhz), +57.1 (lwz), +53.1 (lfd); lwz L1 load-to-use 59.0 at line/page crossing | mis.<form>.line/page4k/page64k 9 / 4; lat.lwz@line_cross 4 / 4 | repeatable | ~55 cycles per crossing access; align data to its size |
| Line-crossing stores: throughput +33.6 (stfd) .. +37.4 (stw); 4 KB / 64 KB page-crossing stores +44.5 (stfd) .. +49.0 (stw) | mis.* stores 8-9 / 4 | repeatable | - |

---------------------------------------------------------------------------------------------------------------------

## 6. String and multiple loads/stores (sysops str.<op>, sched run `0025ae045b06f1d1`, line layout, all fits pass)

| Instruction | Issue cost by bytes (registers) | Confidence | Implementation note |
|---|---|---|---|
| `lswi`, `stswi` | 4 B (1) 15.0, 8 B (2) 16.0, 16 B (4) 18.0, 32 B (8) 22.0 = 14 + 1 per register | single run | Compiler: never use string ops for copies (4 `ld` move 32 B in ~4 issue cycles vs 22) |
| `lswx`, `stswx` (count from XER) | 4 B 38.0, 8 B 39.0, 16 B 41.0, 32 B 45.0 = 37 + 1 per register (+23 over the immediate forms) | single run | - |
| `stmw` | 1 reg 17.0, 4 regs 20.0, 8 regs 24.0, 18 regs (r14-r31) 49.0 | single run | 1-8 registers fit 16 + 1 per register; 18 registers cost 15 more than that line (cause unverified) |
| `lmw` | 1 reg (r31 only) 17.0 | single run | More registers not measurable in the frame (r27-r30 hold it) |
| Semantics: `lswx` count 0 = 1 byte; `stswx` count 0 stores nothing | section 1.4 | hardware-confirmed | - |
| lmw/stmw alignment interrupt for EA % 4 != 0, `lmw` with RA in range illegal, string ops partly outside mapped memory = DSI with nothing written | model choices (PPC970/750 behaviour) | unverified | - |

---------------------------------------------------------------------------------------------------------------------

## 7. SMT and multi-core (smt run `0025ae0458d1030b`; ratios against the default condition; all single run)

| Fact / value | Evidence | Confidence | Implementation note |
|---|---|---|---|
| Busy sibling thread (int, fp or vmx load kernel): independent throughput x1.91-1.92 (median, ~400 tput.indep records), issue cost x1.88-1.89 (lat.data, ucode); dependent latency median x1.00 (711-731 lat.chain records) | condition_effects sibling=busy_* | single run | The threads share issue: a throughput-bound thread halves, latency-bound code is mostly unaffected |
| Exception: integer ALU latency rises with a busy sibling (add chain x1.20, 2.02 -> 2.42; mullw x1.04-1.08; divw x1.05 with busy_int); FP/VMX latency stays x1.00 | instruction ratios under sibling=busy_* | single run | Integer dependent chains slow down under SMT |
| Sibling running the **same** instruction stream: throughput x2.28, latency x1.27 (median; up to x2.5) | sibling=same_insn | single run | Worst case of two identical threads |
| Sibling busy with memory (pointer chase): x1.02-1.05 | sibling=busy_mem | single run | A memory-stalled sibling barely costs issue slots |
| Parked sibling (`xenon_sleep_thread`) vs the default (sibling in libxenon's `or 1,1,1` wait loop): median x0.994, 10 % of records x0.89 or faster | sibling=parked | single run | The default idle sibling still costs up to ~11 % on some kernels; park it for best single-thread timing |
| Thread priority low (`or 1,1,1` at kernel start): throughput x31.8, issue cost x31.2, latency median x8.0 (add 2 -> 32, fadd 10 -> 32, vaddfp 12 -> 32, vperm 4 -> 32, mullw 11 -> 32, slw 16 -> 32, divw 18 -> 32); fdiv and fsqrt unchanged (x1.00) | thread_prio=low, ~2,054 records | single run | Low priority issues about one instruction per 32 cycles (sibling in its own low-priority wait loop) |
| Thread priority high (`or 3,3,3`) / medium (`or 2,2,2`): x0.998 / x1.000 | thread_prio=high/medium | single run | No gain over medium with an idle sibling |
| Other cores loaded (l2_pressure, mem_stream): core-local kernels unchanged (lat.chain x0.994-1.001, lat.data issue x0.98-1.02) | other_cores=*, 1,496-1,663 records | single run | - |
| Other cores pressing the L2: this core's L2 plateau 47.3-48.1 (vs 44.5-45.3); 640 KB-1 MB chases already go to memory (609-639 vs 60-71) | other_cores=l2_pressure chase curve | single run | Shared L2: the effective L2 per core shrinks under load |
| Other cores streaming memory: chases of 192 KB and more cost 709-750 (vs 45 from L2 / 640 from memory), memory latency about +100 (~15 %); load streams 0.16-0.17 B/cycle | other_cores=mem_stream | single run | Memory-bound neighbours evict this core's L2 lines |
| Cross-core mailbox ping-pong (harness, uncached flags): median 134 ticks (~8,600 cycles, 2.69 us), p99 176-177 ticks | self.cores pingpong, 6 cores runs | repeatable | Harness figure, not a CPU property |

---------------------------------------------------------------------------------------------------------------------

## 8. Cross-core coherency

| Fact / value | Evidence | Confidence | Implementation note |
|---|---|---|---|
| A line that `xdcbt` fetches into a core's L1 is **not invalidated** by another core's stores or `dcbf`: 999 of 1,000 rounds read stale data (127,840 of 128,000 words) | self.cores data_sync_c2t_xdcbt in cores runs `0025ae04585f610d`, `0025ae045933cff1`, `0025ae047e605ab7` (identical counts) | hardware-confirmed | Emulator: an xdcbt-fetched line stays stale until the reading core drops it; software must `dcbf` on the reading core before re-reading a buffer it touched with xdcbt |
| An xdcbt on a line already in L1 changes nothing (the variant without the prior L1 drop: 0 stale words, run `0025ae047fe02121`) | data_sync_c2t_xdcbt before commit b69604b | hardware-confirmed | - |
| The reader's own `dcbf` removes the stale line (c2t_xdcbt_staged: 0 of 1,000) | 3 cores runs | hardware-confirmed | - |
| Plain cross-core hand-over is coherent: every other data_sync variant 0 stale words, code_sync 0 stale | 5 cores runs | hardware-confirmed | No extra flushing needed without xdcbt |
| Consequence in the first semantic run: about half of every load family read stale scratch data after the first xdcbt case (lbz 25/48, ld 26/48, lvx 24/72, lvx128 72/72); after flushing on the test core too, every load family has 0 failures | KNOWN.md "Open"; second semantic run | hardware-confirmed | Harnesses that run xdcbt must flush shared buffers on both cores |

---------------------------------------------------------------------------------------------------------------------

## 9. Platform and harness caveats

| Fact / value | Evidence | Confidence | Implementation note |
|---|---|---|---|
| More than ~2040 live 64 KB pages livelock libxenon's software TLB refill: probe.tlb 2040 (~2044 pages plus the kernel's own) HANG on the test core; `tlb.2048` once froze the whole console (green static, no CRASH/HANG report); 1600-2032 pages run | tlb_bisect `0025ae045805e86e` (hang at 2040), `0025ae0450a9ddcf` (2040-2048 quarantined, below pass); mem run `0025ae042607416b` (freeze) | hardware-confirmed | Likely libxenon `drivers/ppc/except.S` (11 page-number bits into PpeTlbIndex = 2048 slots): a libxenon limit, not a CPU property. Keep working sets below ~2000 pages (~125 MB) |
| Timebase 49,875,841 Hz (49.871-49.877 MHz), 64 CPU cycles per tick (3.192 GHz); dependent add 2.018, independent add 1.020, ratio 1.978; noise floor 0.19 ticks | calib, 6 runs | repeatable | One tick (64 cycles) is the raw resolution; fits over depth go below it |
| `mftb` costs 44 cycles; a back-to-back pair 40-53 | calib mftb_cost, 6 runs | repeatable | Framed fits cancel it |
| Line layout bias: about +0.02 cycles per instruction word (1-cycle ops read 1.02, add 2.018); FP/VMX ops read 1.004-1.006 | timing.h; frontend method run; SM | repeatable | Round measured costs (1.02 -> 1, 2.02 -> 2, 14.00 -> 14) |
| Families not on the line layout (branch loop/align/bht/call trees, mem.hier streams/cache ops/prefetch, ldst.lat rt/resv) keep their own depths and stay sensitive to fetch-group phase | commits 9d2565d, 5ce8464, 6057055 | repeatable | Compare those values only within the same family |
| One-off ~83-tick (~5,300-cycle) event on the test core about every 1.4 ms (seen in a sched run); the EE=1 / EE=0 probe (200 runs each) saw 0 outliers in 2 cores runs | commit e89d791; self.cores irq_ee1/irq_ee0 | single run (cause unidentified) | XTS compares minima for A/B/A; an emulator need not model it |
| Single-run values: everything under sibling / thread priority / other cores (one smt run), string ops, cache-op costs, prefetch, stride, associativity, L2 and cold load-to-use per form, the level summary; 748 of 12,263 SM values are single_source | SM coverage | - | Re-run before treating them as final |
| Repeatable-noisy values: 2,355 records accepted because >= 2 runs agree within 2 % (e.g. stb, stwx, stvx, stfs, lfdu/lfsu issue costs, the LHS values) | SM coverage.noisy_promoted | repeatable | Flagged `repeatable_noisy` in the CSV |
| No accepted issue cost: `sc`, `eciwx`, `ecowx`, `bclr`, `bcctr` (given as branch penalties instead), `lfdx` (runs disagree by 4.6 %) | SM / README | - | - |
| FP/VR load-to-use (~33) is a method upper bound | section 3.4 | unverified | Needs a direct FP chain measurement |
| Default sibling = libxenon wait loop at low priority, not parked | section 7 | single run | Default timings include a polling sibling |
| Measurements run in hypervisor state under libxenon with 64 KB pages and a software TLB refill; user-mode titles under the Xbox kernel may see other translation costs | section 5, mem.hier header | unverified for other environments | Do not transfer the 635-cycle TLB miss to retail software without checking |
| Dual-issue pair costs never reach 1.0 per pair (best 1.35) | section 2.1 | repeatable | Cause (front end, SMT slot sharing with the idle sibling) unverified |
| Console identity (serials, keys, console tag) is deliberately absent from this document and the CSV | - | - | - |
