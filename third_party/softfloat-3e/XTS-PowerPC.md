# SoftFloat 3e in XTS

Source: Berkeley SoftFloat Release 3e, `http://www.jhauser.us/arithmetic/SoftFloat-3e.zip`
(sha256 `21130ce885d35c1fe73fc1e1bf2244178167e05c6747cad5f450cc991714c746`). License: `COPYING.txt` (BSD 3-clause).

Vendored: `source/include/`, the `.c` files of the `SOFTFLOAT_FAST_INT64` build (list in `sources.mk`, taken from
`build/Linux-x86_64-GCC/Makefile`), and a specialization directory `source/PowerPC/` derived from `source/8086-SSE/`.
Nothing else in the release was changed. `platform/{host,console}/platform.h` replace the release's per-build
`platform.h` (host: little-endian with `__int128`; console: big-endian, no `__int128`). Built as
`$(BUILD)/{host,console}/libsoftfloat.a` with `-DSOFTFLOAT_FAST_INT64 -DSOFTFLOAT_ROUND_ODD -DINLINE_LEVEL=5`.

## PowerPC specialization (`source/PowerPC/`)
| Item | 8086-SSE | PowerPC (XTS) | Why |
|---|---|---|---|
| default NaN f32 | `0xFFC00000` | `0x7FC00000` | PowerPC/AltiVec default QNaN is positive |
| default NaN f64 | `0xFFF8000000000000` | `0x7FF8000000000000` | same |
| default NaN f16 / extF80 / f128 | negative | positive | consistency only (PowerPC has no such formats) |
| NaN propagation f16/f32/f64 | signaling NaN wins, then larger significand | **first NaN operand (A, then B)**, quieted | PowerPC Book I 4.3.6 / AltiVec PEM: frA, then frB, then frC |
| NaN propagation extF80/f128 | x86 rule | unchanged | not used by the model |
| int conversion, positive overflow | `0x80000000` / `0x8000000000000000` | `0x7FFFFFFF` / `0x7FFFFFFFFFFFFFFF` | `fctiw[z]`/`fctid[z]` saturate |
| int conversion, negative overflow / NaN | `0x80000000` | unchanged (`0x80000000`, `0x8000...0`) | `fctiw` NaN result |
| unsigned conversion, negative / NaN | all ones | `0` | AltiVec `vctuxs` saturates to 0 |
| tininess | after rounding | **before rounding** (initial value of the runtime variable `softfloat_detectTininess`) | PowerPC detects underflow before rounding; the FP model owner must confirm against results4 and may change the runtime variable without editing this tree |

Things SoftFloat cannot express and the model must handle itself (`src/model/fp.c`, `vfp.c`):
- **FMA operand order.** `softfloat_mulAddF64(A, B, C)` computes A*B+C and propagates NaNs in the order A, B, C.
  PowerPC `fmadd frD,frA,frC,frB` computes frA*frC+frB but selects NaNs in the order frA, frB, frC. Check the operands for
  NaN *before* calling SoftFloat and return the PowerPC choice.
- **Signed vector conversions.** `vctsxs`/`vcfpsxws128` of a NaN give 0, unlike `fctiw` (`0x80000000`).
- **FPSCR.** SoftFloat only raises invalid/overflow/underflow/inexact/divide-by-zero; VX subtypes (VXSNAN, VXISI, VXIDI,
  VXZDZ, VXIMZ, VXVC, VXSQRT, VXCVI), FR/FI, FPRF and the non-IEEE mode (NI) are computed by the model.
- **Denormal flushing** (VSCR[NJ], FPSCR[NI]) happens outside SoftFloat.
