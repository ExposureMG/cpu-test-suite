/* FP model core shared by src/model/fp*.c and vfp*.c (owner: mfp).
 *
 * Everything here is integer code on top of SoftFloat: no host or console FPU arithmetic is used for any result, so the
 * model is an independent oracle on both builds.
 *
 * Scalar results are computed in two steps:
 *   1. xf_exact(): the operation on finite operands in binary128 with SoftFloat's round-to-odd. 113 >= 53 + 2 bits, so
 *      rounding that value once more to 53 or 24 bits gives the correctly rounded result in every mode, the inexact
 *      information survives (sticky), and |value| < 2^emin exactly when the infinitely precise value is ("tiny before
 *      rounding", PowerPC Book I 4.4.4).
 *   2. xf_round(): PowerPC rounding with an unbounded exponent: FR/FI, OX/UX/XX, denormalisation when UE=0, the
 *      exponent adjust (+-1536 / +-192) when OE/UE are enabled, and the overflow result by RN.
 */
#ifndef XTS_MODEL_FP_CORE_H
#define XTS_MODEL_FP_CORE_H
#include <stdint.h>
#include <xts/enc.h>
#include <xts/model.h>
/* the library is built with SOFTFLOAT_FAST_INT64 (Makefile SF_DEFS); the f128 value interface needs it declared */
#ifndef SOFTFLOAT_FAST_INT64
#define SOFTFLOAT_FAST_INT64
#endif
#include "softfloat.h"

/* float128_t word order = SoftFloat's struct uint128 order (LITTLEENDIAN in platform/{host,console}/platform.h) */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define XF_HI 1
#define XF_LO 0
#else
#define XF_HI 0
#define XF_LO 1
#endif

/* ---- double / single bit helpers ---- */
#define XD_SIGN 0x8000000000000000ull
#define XD_EXPM 0x7FF0000000000000ull
#define XD_FRAC 0x000FFFFFFFFFFFFFull
#define XD_QBIT 0x0008000000000000ull
#define XD_INF  0x7FF0000000000000ull
#define XD_MAXS 0x47EFFFFFE0000000ull   /* largest single, as a double */
#define XD_MAXD 0x7FEFFFFFFFFFFFFFull
static inline int xd_sign(uint64_t b) { return (int)(b >> 63); }
static inline int xd_exp(uint64_t b) { return (int)((b >> 52) & 0x7FF); }
static inline int xd_isnan(uint64_t b) { return (b & ~XD_SIGN) > XD_INF; }
static inline int xd_issnan(uint64_t b) { return xd_isnan(b) && !(b & XD_QBIT); }
static inline int xd_isinf(uint64_t b) { return (b & ~XD_SIGN) == XD_INF; }
static inline int xd_iszero(uint64_t b) { return (b & ~XD_SIGN) == 0; }
static inline uint64_t xd_quiet(uint64_t b) { return b | XD_QBIT; }

#define XS_SIGN 0x80000000u
#define XS_INF  0x7F800000u
#define XS_QBIT 0x00400000u
static inline int xs_isnan(uint32_t b) { return (b & ~XS_SIGN) > XS_INF; }
static inline int xs_isinf(uint32_t b) { return (b & ~XS_SIGN) == XS_INF; }
static inline int xs_iszero(uint32_t b) { return (b & ~XS_SIGN) == 0; }
static inline int xs_isdenorm(uint32_t b) { return (b & 0x7F800000u) == 0 && (b & 0x007FFFFFu); }

/* ---- exact intermediate ---- */
typedef struct xf {
  int zero;           /* exact zero (sign valid) */
  int sign;
  int e;              /* unbiased exponent of the leading bit */
  uint64_t mh, ml;    /* 113-bit significand M = mh:ml, 2^112 <= M < 2^113; value = M * 2^(e-112) */
} xf_t;

enum { XF_ADD, XF_MUL, XF_DIV, XF_SQRT, XF_FMA };
/* a op b (FMA: a*b + c); finite double operands only. rn = FPSCR[RN], used for the sign of an exact zero sum. */
xf_t xf_exact(int op, uint64_t a, uint64_t b, uint64_t c, int rn);
xf_t xf_from_f128(float128_t f);
xf_t xf_from_i64(int64_t v);

typedef struct xf_res {
  uint64_t bits;      /* double-format result */
  uint32_t exc;       /* OX UX XX raised */
  int fr, fi;
} xf_res_t;
/* Round to `prec` bits (53: double, 24: single result kept in double format) with FPSCR[RN], honouring UE/OE. */
xf_res_t xf_round(const xf_t *x, int prec, uint32_t fpscr);

/* ---- FPSCR / CR glue ---- */
/* FPSCR after an arithmetic instruction: FR, FI, FPRF replaced (fprf < 0: keep), new exception bits merged. */
uint32_t xf_fpscr_set(uint32_t fpscr, int fr, int fi, int64_t fprf, uint32_t exc);
static inline void xf_cr1(xts_state_t *s) { xm_set_cr_field(s, 1, ((uint32_t)s->fpscr >> 28) & 0xF); }

/* ---- operand access by JSON operand name (any form, including the scattered VMX128 fields) ---- */
int xf_op(const xts_insn_t *d, uint32_t w, const char *name);
static inline int xf_rc(const xts_insn_t *d, uint32_t w) { return (xts_flags_of(d->index, w) & XTS_RC) != 0; }

static inline int xf_bitlen(uint64_t v) { return v ? 64 - __builtin_clzll(v) : 0; }

/* ---- verdict channel for estimate instructions (fres frsqrte vrefp vrsqrtefp vexptefp vlogefp and the 128 forms) --
 * The hardware tables are not derivable from results4 (24 cases each), so these handlers model the architected special
 * values exactly and return XTS_M_UNIMPLEMENTED when a result needs the table (see KNOWN.md "## fp (mfp)").
 * xf_estimate_ok() is the accuracy check a tolerance-aware semantic check could use instead (interface request):
 * kind 0 recip, 1 rsqrt; vector 0 = fres/frsqrte bounds (2^-8 / 2^-5), 1 = vrefp/vrsqrtefp (2^-12); x and est as
 * doubles (vector lanes widened exactly). Returns 1 within the bound, 0 outside, -1 when not applicable (special
 * operand or result, or kind 2/3 = exp2/log2, which have no exact reference here). SoftFloat binary128 only. */
int xf_estimate_ok(int kind, int vector, uint64_t x, uint64_t est);
#endif
