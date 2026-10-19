# Reference model: known hardware facts and open disagreements

Every disagreement between the model and a console (tools/check_model.py on results4, or a `fail` record of a semantic
family) is either fixed in the model or recorded here with its evidence.

## Facts the framework already applies (src/model/model.c `xm_state_loaded`)
- **FPSCR[NI] cannot be set on Xenon.** Every results4 case whose input image had FPSCR = 4 (NI=1) read back NI=0
  (196 cases: 184 that executed, 12 that trapped; FPSCR inputs 0-3 read back unchanged). The dispatcher clears NI in
  the model's input, so the model sees what the CPU holds. Consequence for the `fpscr_ni` axis: setting NI through the
  input image (mtfsf) has no effect; whether `mtfsb1 29` can set it is untested.
- mtfsf 0xFF does not write FEX/VX (summary bits); mtvscr keeps only NJ and SAT.
- **FPSCR bit 20 (reserved, 0x800) cannot be set either** (semantic run 2026-10-19, Jasper): all 593 FP cases whose
  input image had it set read it back as 0, across the 35 opcode-59/63 mnemonics that keep or read the FPSCR (fmr,
  fabs, fcmpu, mffs, mcrfs field 5, the arithmetic ops ...); results4's mtfs* cases already showed that mtfs* cannot set
  it. Cleared in the model's input like NI. Unverified: no non-FP case was loaded with the bit, so "the slot's mtfsf
  cannot set it" and "every FP instruction clears it first" are not yet told apart (a non-FP case with bit 20 set in its
  input FPSCR decides it: kept = the second rule).

## Open
- **Not a model disagreement: the semantic run's load failures (2026-10-19) were an XTS bug.** About half of every
  load family (lbz 25/48, ld 26/48, lvx 24/72, lvx128 72/72, ...) read data that an EARLIER store case had left in
  scratch, while the expected baseline (byte i = i*13+7) was what the suite core had written and flushed. All 144 load
  cases before the first xdcbt case passed; the first stale load came 24 records after it. Plain cross-core hand-over is
  coherent (cores run: data_sync_c2t_read/c2t_store/t2c_plain all 0), so the likely cause is that a line xdcbt brought
  into the test core's L1 is not invalidated by the suite core's stores and dcbf (data_sync_c2t_xdcbt tests it). Fixed in
  src/exec/exec_console.c: the test core drops its copies of the shared buffers before every case, and xts_dcbf_range /
  xts_prep flush on the test core too. Second semantic run: every load family 0 failures.
- **lswx with XER byte count 0** (second semantic run, word 7F055C2A = lswx r24,r5,r11, SF=0, EA = scratch+0x84): the
  model leaves RT unchanged (architecturally undefined), the CPU returned 0x00000000BB000000 - the byte at EA in the top
  byte of the low word, as if one byte were loaded. One case; the run's other count-0 cases (if any) passed but carry no
  detail. Not changed in the model yet: needs targeted count-0 cases (several EAs, RT values, SF=0/1).
- **vmsum4fp128: hardware is 1 ULP lower than the model on a sign-mixed case** (first console smoke run, 2026-10-19,
  Jasper, PVR 0x00710500; exec.vmx128 case vmsum4fp128#12, word 173945D8, sf=64, VSCR 0). Inputs (VA = VD = v89,
  VB = v8): A = AF3D0257 96C316AE 37EFC7C9 C5106AAB, B = 80000001 007FFFFF BF000000 4F000000. Model D4906AAB in every
  lane, hardware D4906AAA. The dominant product (lane 3, about -4.96e12) is negative while the first product (lane 0,
  a tiny negative times a negative denormal) is positive; the model sums in one's complement relative to the FIRST
  product's sign, so the reference sign is the likely error (alternatives: the largest product's sign, or denormal
  B lanes handled differently with NJ=0). Not fixed yet: wait for the semantic profile's targeted vmsum cases (sign
  mixes, denormals, aliasing) and pick the rule that matches all of them.

## vint (mvint)
Files: `src/model/vint.c` (element arithmetic, compares, shifts, logicals, splats, mfvscr/mtvscr), `vperm.c` (merge, pack,
unpack, perm, sldoi, sel, octet/bit shifts), `vint_priv.h`. 131 mnemonics (98 AltiVec VX/VA/VC + 33 VMX128 non-FP);
results4: 3,144 cases (131 x 24), 0 mismatches.
- **vsl / vsr shift per byte, not per register (hardware fact).** The architecture shifts the 128-bit register by
  vB[125:127] and leaves the result undefined unless all 16 bytes of vB have equal low 3 bits. Xenon computes result byte
  i with count `vB.byte[i] & 7`, taking the shifted-in bits from the next byte of vA (vsl; 0 past byte 15) or the
  previous byte (vsr; 0 before byte 0). Evidence: 41 of the 48 results4 vsl/vsr cases (all with unequal counts) differed
  from the whole-register model and all 48 match the per-byte rule. With equal counts both rules agree. The model uses the
  per-byte rule, so the semantic family may feed arbitrary count vectors.
- vslo/vsro (and the 128 forms) use vB bits 121:124 (byte 15, `>> 3 & 0xF`) and match on all inputs seen.
- vspltb/h/w[128] mask the element index (UIMM & 15/7/3); vspltisw128 ignores the (reserved) VB128 field. vsel128 takes
  the mask from the old value of the tied VD128 operand.
- Saturating add/sub/pack set VSCR[SAT] only when a lane actually saturates; the model never touches VSCR[NJ]. mtvscr
  keeps only NJ and SAT of vB word 3 and zeroes the other VSCR words (matches the loaded-state normalisation).
- Rc compares: CR6 = {LT = all lanes true, GT = 0, EQ = all lanes false, SO = 0}; Rc bit is bit 10 (VC) or bit 6 (VX128_R),
  read through `xts_flags_of`.
- Not in this file (expected-illegal on Xenon): vmul*/vmsum*/vmhadd*/vmladd* and friends.

## int (mint)
Files: src/model/int_ext.c (add/sub/carry, mul, div, logical, extends, counts, compares), rot.c (rotates, shifts),
cmp_cr.c (CR logic/moves, mfspr/mtspr), trap.c (tw td twi tdi). results4 replay: all variants present match
(every mnemonic below 24/24).
- **Coverage limits of results4.** All 10,776 cases ran with MSR[SF]=1: the SF=0 behaviour (CR0 and compare on the low
  word, CA/OV from bit 32/31, 64-bit result still written in full) is implemented from the architecture, NOT verified.
  mulh* and divw with Rc=1 compute CR0 from the full 64-bit value written (so a negative `divw` quotient, which is
  zero-extended, gives GT in 64-bit mode): consistent with the data but no negative-quotient Rc case exists in results4.
- **divw/divwu/divd/divdu, undefined cases (divisor 0, 0x80000000/-1, INT64_MIN/-1): results4 has NO such case**
  (divide-by-zero never occurs in the 24 cases per mnemonic; the divwide timing runs only use ordinary operands). The
  model returns 0 in RD and sets OV (OE=1) for these; this is a guess, to be settled by the first hardware run.
  Defined behaviour confirmed by hardware: word divides use the low words, the quotient is zero-extended to 64 bits
  (e.g. divw 0x80000011/7 -> 0x00000000EDB6DB71); OV=0 for ordinary operands.
- **mulhw/mulhwu**: the architecturally undefined upper word is the sign extension (mulhw) / zero (mulhwu) of the high
  word, i.e. mulhw writes the sign-extended 64-bit value of the high 32 bits (results4, e.g. -2*0x7FFFFFFF ->
  0xFFFFFFFFFFFFFFFF). **mulh* with OE=1 and Rc=1 is illegal** (OpenXenos sweep.json `reserved_forms_illegal`; the
  decoder mask leaves OE open so the dispatcher does not reject it): the handlers return XTS_M_ILLEGAL. OE=1 with
  Rc=0 is treated as plain mulh (OE ignored, XER untouched): unverified.
- **mfocrf ignores FXM and returns the whole CR** (results4: every multi-bit-FXM case with a non-zero CR; ~20 cases).
  A single-bit FXM with a non-zero CR does not occur in results4, so that case is assumed identical (the architecture
  allows the other fields to be undefined). RD bits 0-31 are 0.
- **mtocrf with several FXM bits writes only the most significant selected field** (field 0 first), not all selected
  fields as mtcrf does (results4, ~15 cases). FXM=0 changes nothing.
- **mtspr XER** keeps only SO/OV/CA and the 7-bit byte count (0xE000007F); unverified (no mfspr/mtspr in results4,
  hardware status "untested"). mfspr/mtspr of any SPR other than XER(1), LR(8), CTR(9) returns UNIMPLEMENTED.
  **mftb is not registered** (nondeterministic): the dispatcher answers UNIMPLEMENTED.
- cmp/cmpl/cmpi/cmpli: L=0 compares the low words (sign- or zero-extended) in either mode; L=1 the full 64 bits.
- Rc on cntlzd (not listed as an Rc form in the JSON) is honoured when the bit is set; not in results4.
- `__int128` is not used (the console compiler is 32-bit): 64x64->128 multiplies go through mulu64/muls64 helpers.

## ldst (mldst)
Files: `src/model/ldst.c` (integer, FP, larx/stcx, multiple/string, cache ops), `ldst_vec.c` (AltiVec + VMX128 memory),
`cellvmx.c` (Cell lvlx/lvrx/stvlx/stvrx and their `l`/VMX128 forms). results4 replay: 114 mnemonics, 2,736 cases, 0
mismatches; only lmw, stmw, lswi, lswx, stswi, stswx (hardware status "untested") have no hardware cases.

Facts from results4 that the model encodes:
- **Alignment.** Integer loads/stores (incl. byte-reverse, `lha/lwa`, `ld/std`) and all vector loads/stores never trap on a
  misaligned EA. FP loads/stores (`lfs lfd stfs stfd stfiwx`, every update/indexed form, even the 8-byte ones) take an
  alignment interrupt (0x600) unless EA % 4 == 0; EA % 8 == 4 executes. `lwarx/stwcx.` need EA % 4, `ldarx/stdcx.` EA % 8.
  Alignment is checked before the access, so an unmapped misaligned EA is ALIGNMENT, not DSI.
- **Vector quadword rules.** `lvx/stvx/lvxl/stvxl` (and 128 forms) ignore EA[28:31]. `lvebx/lvehx/lvewx` (+`lvewx128`) load
  the WHOLE aligned quadword on Xenon (architecturally the other lanes are undefined; hardware replaces all 16 bytes).
  `stvebx/stvehx/stvewx` store only the element selected by the EA's low bits. `lvsl/lvsr` use EA[28:31], no memory access.
- **Cell.** `lvlx` loads bytes EA..end-of-quadword into the high end of vD, zero below; `lvrx` loads quadword-start..EA-1
  into the low end, zero above (EA aligned: all zero, no access); `stvlx/stvrx` are the inverse and write only those bytes.
  The `l` (LRU) forms and the VMX128 forms are identical in effect.
- **FP single conversion** is bit-exact without the host FPU. `lfs`: normal/zero/inf/NaN are a bit shuffle (an SNaN stays an
  SNaN, no quieting), denormal singles are normalised. `stfs`: exponent > 896 (and zero) is a bit shuffle with NO rounding
  (2^128 stores as 0x7F800000, a NaN keeps its payload bits), 874..896 is denormalised by truncation, smaller non-zero values
  (architecturally undefined, and double denormals) store as sign|0 - matches every stfs-family case in results4. Neither
  touches FPSCR. `stfiwx` stores FPR[32:63].
- **dcbz and dcbz128 both zero the 128 bytes** containing the EA. dcbf/dcbst/dcbt/dcbtst/icbi/xdcbt/sync/eieio/isync/
  dst/dstst/dss are state-free no-ops (a wild EA does not fault in the model).
- **Reservation (hidden state).** Neither `xts_state_t` nor `xts_mem_t` has a slot, so the model keeps ONE reservation in a
  file static of ldst.c: set by every lwarx/ldarx that completes, cleared by every stwcx./stdcx. that completes (success or
  not). `stcx` succeeds iff the reservation is set and in the same 128-byte line; success stores and sets CR0 = 0b0010|SO,
  failure CR0 = 0b000|SO. This is exactly how results4 behaves: its first non-trapping `stwcx.` (EA = line+12) succeeded
  because an `ldarx` ~300 jobs earlier (EA = line+64) had left a reservation, and every later `stwcx.`/`stdcx.` failed;
  traps (alignment) neither set nor clear it. Consequences: the model result for `stcx` depends on the calls before it,
  so (a) run `lwarx` through `xts_semantic_check` before `stwcx.` to get a defined success case, (b) the model must be called
  once per executed case in order. The host dry run calls it twice per case (simulated CPU + oracle), which spuriously
  fails a *succeeding* stcx there: wrap the simulation in `xts_model_resv_get/set` (see interface notes) or avoid stcx
  in dry runs. `xts_model_resv_clear()` resets it (host tests). The 128-byte granule is inferred from that one success
  (any granule >= 128 B would also fit; a smaller one would not).
- `stwcx./stdcx.` with Rc=0, update forms with RA=0 (all update forms), and load-update with RA=RT are ILLEGAL (0x700/0x08).
  For the X-form update ops this is measured (sweep.json `reserved_forms_illegal`: 28 words); the D/DS-form
  (`lwzu lbzu lhzu lhau lfsu lfdu stwu ... ldu stdu`) invalid forms were NOT swept (primaries 32-47/58/62 are not in
  results5-8), so they are extrapolated from the X forms. A store-update with RA=RS (e.g. `stwu r1,-16(r1)`) is architecturally
  valid and modelled as valid: the X-form sweep used all-zero register fields, so it cannot tell RA=RS from RA=0.
- EA under MSR[SF]=0 is the low 32 bits of the sum, and update forms write that truncated value to RA. All results4 cases
  ran with SF=1, so this is UNVERIFIED on hardware (the `sf` axis in a semantic family will check it).

Unverified model choices (no hardware data; first thing to check when a console run disagrees):
- `lmw/stmw`: EA % 4 != 0 -> alignment interrupt (PPC970/750 behaviour); `lmw` with RA in [RT..31] (incl. RA=0 with RT=0) is
  ILLEGAL. Registers r30/r31 are ordinary members of the range (the exec agent must keep r30 out of the range or wrap it).
- `lswi/lswx`: no alignment interrupt; RA (or RB for lswx) among the registers loaded -> ILLEGAL; `lswx` with RT=RA or RT=RB ->
  ILLEGAL; `lswx` with XER[25:31]=0 leaves RT unchanged (architecturally undefined). The byte count is XER[25:31] (7 bits).
  Unused low bytes of the last register are zeroed, upper halves of every loaded register are zeroed. `lswi` NB=0 = 32 bytes.
- A multi-register/multi-byte access that is partly outside the mapped regions is a DSI with nothing written.

## vmx128 (mvmx)
`src/model/vmx128.c`: vmaddfp128 vmaddcfp128 vnmsubfp128 vmsum3fp128 vmsum4fp128 vpermwi128 vrlimi128 vpkd3d128
vupkd3d128. results4: 216/216 match (24 each). fmastat results17: 192/192 VMX128 FMA cases match (replayed through
xts-model-replay). Open disagreements: none.

**FMA.** Fused, one rounding, always round-to-nearest-even, FPSCR untouched. Orders: vmaddfp128 `VD = VA*VB + VD`,
vmaddcfp128 `VD = VA*VD + VB`, vnmsubfp128 `VD = -(VA*VB - VD)`, negated after rounding (exact cancellation -> -0).
NaN: first NaN of (first multiplicand, addend, second multiplicand), quieted with 0x00400000, never negated; invalid
-> 0x7FC00000. NJ=1 flushes denormal inputs and results to signed zero.

**vmsum3fp128 / vmsum4fp128 are not IEEE.** We searched two families against all 48 results4 cases (24 each):
- IEEE-style ordered sums (NaN, overflow and flush rules applied as below): 24 orders × product rounding
  {exact, RN24, RZ24} × intermediate rounding {exact, RN, RZ at 24/25/26/32/48 bits} × final {RN, RZ} = 1,584
  candidates. The best mismatches 3/48. These are the one-LSB-low results `-1.5 + -0.75 (+0) = 0xC00FFFFF` and
  `Y + 2.5 + (-0) = Y - 1 ulp`, which no sign-symmetric sum can give.
- Aligned-accumulator family: product {exact, RN24, RZ24} × accumulator width 20-199 bits × reference sign {p0,
  largest product} × final {RN, RZ} × sign handling {signed sum, one's complement without end-around carry} ×
  zero-product exponent {real, ignored}. Signed sum: best 3/48. Reference = largest product: best 2/48. Final RN: best
  3/48. **0/48** for one's complement relative to sign(p0) + final truncation, for any product rounding and any
  width from 25 to 88 bits below the largest product's MSB.

The model implements that winner: the exact 48-bit products are truncated to the LSB of the largest exponent sum.
Products with p0's sign add their magnitude, and the others add `~magnitude` (= -m-1). A negative total flips the sign
and is complemented back. The result is truncated to 24 bits. Special rules, all from results4:
- Any NaN gives 0x7FC00000; an SNaN payload is lost.
- inf*0 and inf-inf give 0x7FC00000.
- Exponent overflow gives a QNaN carrying the result's sign (0x7FC00000 or 0xFFC00000), never infinity (3 cases).
- A result below 2^-126 becomes a signed zero even with NJ=0.
- NJ=0 keeps denormal inputs (inf*denormal = inf).
- vmsum3 ignores lane w.
- VSCR is never changed.

**vpermwi128.** `VD.i = VB[PERM >> (6-2i) & 3]`.

**vrlimi128.** `VD.i = rotl_words(VB, z)[i]` where IMM bit (3-i) is set, else VD.i.

**vpkd3d128 / vupkd3d128.** Integer D3D formats are floats biased by 3.0, so the code is `bits - 0x40400000`:
- Pack clamps the code to the format range and sets VSCR[SAT] when it clamps (10 cases show the 0->1 transition).
- A NaN packs to the most negative code: 0x8000 or 0x200 (4 lanes). Unpack turns that code back into 0x7FC00000.
- FLOAT16 is the Xenos half (saturates to 0x7FFF|sign, no inf or NaN) and never sets SAT.
- Layouts, placements per (pack size, z), and the UIMM>>2 unpack type follow xenia. Every executed combination
  agrees with it.
- Not modelled (`XTS_M_UNIMPLEMENTED`): pack size 0 and type 7 for both instructions.

### Not covered by results4: targets for exec.vmx128
- **FMA NaN order:** 0 lanes with two different NaNs for vmaddfp128 and vmaddcfp128, 2 for vnmsubfp128. Also open:
  NJ=1 with a denormal result that is not an exact zero, and vnmsubfp128 exact cancellation (-0).
- **vmsum, accumulator:** the width is undecided between 25 and 88 bits below the largest product's MSB (the model
  uses 47-48). Product pre-rounding is undecided. Target: sums where small products of p0's sign or the opposite sign
  land 25-90 bits below the largest one.
- **vmsum, signs and zeros:** the sign of an exactly zero sum and of all-zero products (the model gives -0 when an
  opposite-signed product brings the total to -1), and `x + (-x)`.
- **vmsum, specials:** a product that overflows while the sum is back in range; an infinity plus an overflowing finite
  sum; and NJ=1 with denormal inputs (no case discriminates).
- **vpermwi128:** 23 of 256 PERM values ran. Every (position, selector) pair was covered.
- **vrlimi128:** IMM 2 and 4, and IMM bit 4 (values 16-31), never ran. The model ignores bit 4.
- **vpkd3d128, (type, pack) pairs never run:**
  - 14 of the 18 pairs for types 0-5 and pack sizes 1-3 ran. Never run: (0,1) D3DCOLOR 32-bit, (3,2) FLOAT16_2 with
    pack 2, (5,2) and (5,3);
  - type 6 (4_20_20_20) and type 7 entirely;
  - pack size 0 (all types).
- **vpkd3d128, codes and values:**
  - No integer lane inside the clamp window: the linear part is xenia's.
  - No NaN in an unsigned field (D3DCOLOR, the W of 2_10_10_10): the model packs 0, without SAT.
  - Whether a NaN sets SAT is undecided (every NaN case already saturated another lane).
  - Whether SAT counts unpacked lanes (NORMSHORT2/FLOAT16_2 z,w) is undecided.
  - No FLOAT16 lane where truncation and round-to-nearest differ, and no half denormal (2^-24..2^-14). The model
    truncates for FLOAT16_2 and uses RNE for FLOAT16_4, as xenia does.
- **vupkd3d128:** only UIMM 0-7 ran (types 0 and 1). Never run:
  - UIMM 8-31 (types 2-7);
  - the 0x8000 NaN code;
  - FLOAT16 exponent 0 and 31.

## fp (mfp)
Files `fp_core.{h,c}` (binary128 round-to-odd intermediate + PowerPC rounding, FPSCR glue), `fp.c` (scalar + FPSCR
ops), `fp_est.c` (fres, frsqrte), `vfp.{h,c}` (vector FP), `vfp_est.c` (vector estimates); host tests `t_mfp_*.c`
(incl. 112,000 random add/mul/div/sqrt/fmadd/fadds/frsp cases x 4 RN against SoftFloat's own f64/f32 ops). No host FPU
arithmetic anywhere. **check-model: 82 mnemonics, 1,968 cases: 1,779 match, 0 mismatch, 189 unimplemented**, all of
them estimate ops (table below).

Hardware facts (results4) the model applies:
- **Single-precision operand range.** `fadds fsubs fmuls fmadds fmsubs fnmadds fnmsubs`: if no operand is a NaN, no
  invalid operation occurs and no operand is infinite, a finite nonzero operand outside the normal single range
  (double exponent < 897 or > 1150) gives `0x7FF8000000000000`, FR=FI=0, FPRF=QNaN and no exception bit (all such
  cases, e.g. `1.0 + 2^-1022`, `2^-1022 * 0`). An infinite operand still wins (`inf * 2^-997 + x` = inf).
  `fdivs`/`fsqrts` with such an operand compute in **double** precision (double rounding, denormals and overflow:
  `0.1/DBL_MAX` = `0x0000666666666666` with UX).
- **Disabled overflow sets FR** (frsp/fdivs, RN=0 and RN=1; architecturally undefined).
- **fmadd family: inf*0 sets VXIMZ even when the addend is a NaN** (SNaN addend: VXSNAN + VXIMZ, 1 case).
- Single results keep the double NaN payload (`fadds 0.1, 0x7FF0000000000001` = `0x7FF8000000000001`).
- `fctiw[z]` results are sign-extended to 64 bits, NaN -> `0xFFFFFFFF80000000`; fcti* leave FPRF as it was (results4
  only loads FPRF=0, so "unchanged" and "cleared" are indistinguishable).
- `mffs` high word is `0xFFF80000` (24/24, all with FPSCR=0).
- `mtfsb1 29` does **not** set NI (1 case), and `mtfsf` cannot set NI or the reserved bit 20 (FLM fields 5+7 with
  FRB=all ones read back `...070B`). The model clears both after every FPSCR write. mtfsb1 sets FX only for an
  exception bit going 0->1 (OX UX XX VXSNAN VXIMZ VXVC VXSQRT VXCVI seen; not FPRF/enable bits); mtfsfi to a field
  other than 0 never sets FX.
- fres: overflow at the single range sets OX only (no XX, FR/FI clear, value by RN: `fres(2^-1022)` = +inf,
  `fres(2^-997)` RN=1 = max single); `|x| >= 2^128` gives a signed zero with no UX/XX (x ~ 2^995, RN 0/2/3).
  Estimates never set XX/FI/FR and do not depend on RN.
- Vector: everything in the "Numeric behaviour" table of OpenXenos `SHORTCOMINGS.md` 8.6 reproduces (NaN order A, B, C
  for vmaddfp; vnmsubfp negates after rounding; vctuxs/vctsxs NaN -> 0 without SAT; vcmpbfp NaN -> `0xC0000000`;
  max/min of zeros; vmulfp128 = rounded product).

Not decided by results4 (the model follows Book I / the PEM):
- **Tininess: before rounding (Book I 4.4.4), unconfirmed.** results4 has 18 UX cases, none whose rounded result is
  the minimum normal, so before/after rounding cannot be told apart. The scalar model computes tininess from its exact
  intermediate and does not use `softfloat_detectTininess`; vector ops raise no flags, so the SoftFloat setting is
  irrelevant to this model. Decisive case: `fmul 0x3FEFFFFFFFFFFFFE, 0x0010000000000001` (RN=0, exact value
  2^-1022 * (1 - 2^-104)) gives 2^-1022 with FPSCR `8A064000` before rounding (model) or `82064000` after rounding.
- Enabled exceptions (`fpscr_en` axis): VE/ZE suppress the target (FPRF kept, FR/FI cleared), OE/UE deliver the
  exponent-adjusted result (+-1536 double, +-192 single and frsp), XX only if inexact; results4 never enables one.
- frsp of a NaN clears the low 29 bits (Book I A.1); results4 only has `0x7FF8000000000000` inputs and the arithmetic
  single ops keep the payload, so Xenon may not truncate.
- Single denormal operands (as doubles, exponents 874-896) count as outside the single range (no sample).
- NJ=1 flush of denormal *results* (PEM; 0 discriminating lanes); `min(+0,-0)` = -0; vctuxs of (-1, 0) gives 0
  without SAT; FR for fcti*/fcfid = magnitude incremented.

Estimates (no tolerance channel, tables not derivable from 5-12 distinct inputs per op): the handlers return
`XTS_M_UNIMPLEMENTED` (semantic check: skip) whenever a result needs the hardware table, and model exactly the
architected specials: NaN (SNaN quieted, payload kept), +-0, +-inf, negatives for rsqrt/log, fres/vrefp overflow
(|x| < 2^-129), NJ flush of results certain to be denormal, vexptefp of integral x (exact; 0 / +inf beyond -150 / 128),
0 < x < 2^-100 -> 1.0 (results4: denormals and 2^-126), vlogefp of 2^y (exact, y in [-149, 127]).
| op | unimplemented cases (of 24) | modelled lanes | op | unimplemented | modelled lanes |
|---|---|---|---|---|---|
| fresx | 13 | - | frsqrtex | 16 | - |
| vrefp / vrefp128 | 23 / 24 | 67 of 192 (both) | vrsqrtefp / 128 | 23 / 23 | 93 of 192 |
| vexptefp / 128 | 15 / 17 | 143 of 192 | vlogefp / 128 | 19 / 16 | 140 of 192 |
All 443 modelled lanes match. The table results satisfy the architected bounds (checked with `xf_estimate_ok`):
224/224 vrefp/vrsqrtefp lanes within 2^-12, fres within 2^-8 (`fres(2.5)` is exactly at it) and frsqrte within 2^-5
(29/29 non-overflow scalar results). Observed shape: fres keeps 8 significant bits (`fres(1.0)` = 255/256), frsqrte 9,
vrefp/vrsqrtefp look piecewise linear (full-width significands). Exact tables need the OpenXenos ROADMAP `est` plan.
