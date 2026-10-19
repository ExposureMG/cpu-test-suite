/* Load/store model: integer, byte-reverse, update forms, FP, load-reserve/store-conditional, load/store multiple and
 * string, cache-management and ordering no-ops. (Vector memory: ldst_vec.c, Cell: cellvmx.c.)
 *
 * Hardware rules applied (OpenXenos results4, PVR 00710500; details and evidence in KNOWN.md "ldst (mldst)"):
 *  - integer loads/stores never take an alignment interrupt, whatever the EA;
 *  - FP loads/stores (lfs/lfd/stfs/stfd/stfiwx, all forms) take one unless EA % 4 == 0;
 *  - lwarx/stwcx. need EA % 4 == 0, ldarx/stdcx. EA % 8 == 0; stwcx./stdcx. with Rc=0 is illegal;
 *  - update forms with RA=0 (all) or RA=RT (loads) are illegal;
 *  - dcbz and dcbz128 both clear the 128 aligned bytes around the EA.
 * MSR[SF]=0 (mode bit): the effective address is the low 32 bits of the sum; update forms write that to RA.
 */
#include <string.h>
#include <xts/enc.h>
#include <xts/model.h>
#include "ldst_priv.h"

enum { K_D, K_DS, K_X };

typedef struct acc {
  uint8_t kind;   /* K_D / K_DS / K_X */
  uint8_t size;   /* bytes */
  uint8_t sext;   /* load: sign-extend */
  uint8_t rev;    /* byte-reversed */
  uint8_t upd;    /* update form */
} acc_t;

static uint64_t ea_of(const acc_t *a, uint32_t w, const xts_state_t *in)
{
  unsigned ra = XM_RA(w);
  uint64_t b = (ra || a->upd) ? in->gpr[ra] : 0, off;
  switch (a->kind) {
  case K_D: off = (uint64_t)XM_SIMM16(w); break;
  case K_DS: off = (uint64_t)(int64_t)(int16_t)(w & 0xFFFC); break;
  default: off = in->gpr[XM_RB(w)]; break;
  }
  return ml_trunc(in, b + off);
}

static uint64_t bswap_n(uint64_t v, int n)
{
  switch (n) {
  case 2: return __builtin_bswap16((uint16_t)v);
  case 4: return __builtin_bswap32((uint32_t)v);
  case 8: return __builtin_bswap64(v);
  }
  return v;
}

static uint64_t sext_n(uint64_t v, int n)
{
  switch (n) {
  case 1: return (uint64_t)(int64_t)(int8_t)v;
  case 2: return (uint64_t)(int64_t)(int16_t)v;
  case 4: return (uint64_t)(int64_t)(int32_t)v;
  }
  return v;
}

static xts_mres_t int_load(const acc_t *a, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  unsigned rt = XM_RD(w), ra = XM_RA(w);
  if (a->upd && (ra == 0 || ra == rt)) return XTS_M_ILLEGAL;
  uint64_t ea = ea_of(a, w, in), v;
  if (xm_load(m, ea, a->size, &v)) return XTS_M_DSI;
  if (a->rev) v = bswap_n(v, a->size);
  if (a->sext) v = sext_n(v, a->size);
  out->gpr[rt] = v;
  if (a->upd) out->gpr[ra] = ea;
  return XTS_M_OK;
}

static xts_mres_t int_store(const acc_t *a, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  unsigned rs = XM_RS(w), ra = XM_RA(w);
  if (a->upd && ra == 0) return XTS_M_ILLEGAL;
  uint64_t ea = ea_of(a, w, in), v = in->gpr[rs];
  if (a->size < 8) v &= (1ull << (8 * a->size)) - 1;
  if (a->rev) v = bswap_n(v, a->size);
  if (xm_store(m, ea, a->size, v)) return XTS_M_DSI;
  if (a->upd) out->gpr[ra] = ea;
  return XTS_M_OK;
}

#define IFUN(nm, fn, ...)                                                                                          \
  static xts_mres_t m_##nm(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out) \
  {                                                                                                                \
    static const acc_t a = { __VA_ARGS__ };                                                                        \
    (void)d;                                                                                                       \
    return fn(&a, w, in, m, out);                                                                                  \
  }
/*          name   fn        kind  size sext rev upd */
IFUN(lbz,   int_load,  K_D,  1, 0, 0, 0)
IFUN(lbzu,  int_load,  K_D,  1, 0, 0, 1)
IFUN(lbzx,  int_load,  K_X,  1, 0, 0, 0)
IFUN(lbzux, int_load,  K_X,  1, 0, 0, 1)
IFUN(lhz,   int_load,  K_D,  2, 0, 0, 0)
IFUN(lhzu,  int_load,  K_D,  2, 0, 0, 1)
IFUN(lhzx,  int_load,  K_X,  2, 0, 0, 0)
IFUN(lhzux, int_load,  K_X,  2, 0, 0, 1)
IFUN(lha,   int_load,  K_D,  2, 1, 0, 0)
IFUN(lhau,  int_load,  K_D,  2, 1, 0, 1)
IFUN(lhax,  int_load,  K_X,  2, 1, 0, 0)
IFUN(lhaux, int_load,  K_X,  2, 1, 0, 1)
IFUN(lwz,   int_load,  K_D,  4, 0, 0, 0)
IFUN(lwzu,  int_load,  K_D,  4, 0, 0, 1)
IFUN(lwzx,  int_load,  K_X,  4, 0, 0, 0)
IFUN(lwzux, int_load,  K_X,  4, 0, 0, 1)
IFUN(lwa,   int_load,  K_DS, 4, 1, 0, 0)
IFUN(lwax,  int_load,  K_X,  4, 1, 0, 0)
IFUN(lwaux, int_load,  K_X,  4, 1, 0, 1)
IFUN(ld,    int_load,  K_DS, 8, 0, 0, 0)
IFUN(ldu,   int_load,  K_DS, 8, 0, 0, 1)
IFUN(ldx,   int_load,  K_X,  8, 0, 0, 0)
IFUN(ldux,  int_load,  K_X,  8, 0, 0, 1)
IFUN(lhbrx, int_load,  K_X,  2, 0, 1, 0)
IFUN(lwbrx, int_load,  K_X,  4, 0, 1, 0)
IFUN(ldbrx, int_load,  K_X,  8, 0, 1, 0)
IFUN(stb,   int_store, K_D,  1, 0, 0, 0)
IFUN(stbu,  int_store, K_D,  1, 0, 0, 1)
IFUN(stbx,  int_store, K_X,  1, 0, 0, 0)
IFUN(stbux, int_store, K_X,  1, 0, 0, 1)
IFUN(sth,   int_store, K_D,  2, 0, 0, 0)
IFUN(sthu,  int_store, K_D,  2, 0, 0, 1)
IFUN(sthx,  int_store, K_X,  2, 0, 0, 0)
IFUN(sthux, int_store, K_X,  2, 0, 0, 1)
IFUN(stw,   int_store, K_D,  4, 0, 0, 0)
IFUN(stwu,  int_store, K_D,  4, 0, 0, 1)
IFUN(stwx,  int_store, K_X,  4, 0, 0, 0)
IFUN(stwux, int_store, K_X,  4, 0, 0, 1)
IFUN(std,   int_store, K_DS, 8, 0, 0, 0)
IFUN(stdu,  int_store, K_DS, 8, 0, 0, 1)
IFUN(stdx,  int_store, K_X,  8, 0, 0, 0)
IFUN(stdux, int_store, K_X,  8, 0, 0, 1)
IFUN(sthbrx, int_store, K_X, 2, 0, 1, 0)
IFUN(stwbrx, int_store, K_X, 4, 0, 1, 0)
IFUN(stdbrx, int_store, K_X, 8, 0, 1, 0)

XTS_MODEL("lbz", m_lbz);
XTS_MODEL("lbzu", m_lbzu);
XTS_MODEL("lbzx", m_lbzx);
XTS_MODEL("lbzux", m_lbzux);
XTS_MODEL("lhz", m_lhz);
XTS_MODEL("lhzu", m_lhzu);
XTS_MODEL("lhzx", m_lhzx);
XTS_MODEL("lhzux", m_lhzux);
XTS_MODEL("lha", m_lha);
XTS_MODEL("lhau", m_lhau);
XTS_MODEL("lhax", m_lhax);
XTS_MODEL("lhaux", m_lhaux);
XTS_MODEL("lwz", m_lwz);
XTS_MODEL("lwzu", m_lwzu);
XTS_MODEL("lwzx", m_lwzx);
XTS_MODEL("lwzux", m_lwzux);
XTS_MODEL("lwa", m_lwa);
XTS_MODEL("lwax", m_lwax);
XTS_MODEL("lwaux", m_lwaux);
XTS_MODEL("ld", m_ld);
XTS_MODEL("ldu", m_ldu);
XTS_MODEL("ldx", m_ldx);
XTS_MODEL("ldux", m_ldux);
XTS_MODEL("lhbrx", m_lhbrx);
XTS_MODEL("lwbrx", m_lwbrx);
XTS_MODEL("ldbrx", m_ldbrx);
XTS_MODEL("stb", m_stb);
XTS_MODEL("stbu", m_stbu);
XTS_MODEL("stbx", m_stbx);
XTS_MODEL("stbux", m_stbux);
XTS_MODEL("sth", m_sth);
XTS_MODEL("sthu", m_sthu);
XTS_MODEL("sthx", m_sthx);
XTS_MODEL("sthux", m_sthux);
XTS_MODEL("stw", m_stw);
XTS_MODEL("stwu", m_stwu);
XTS_MODEL("stwx", m_stwx);
XTS_MODEL("stwux", m_stwux);
XTS_MODEL("std", m_std);
XTS_MODEL("stdu", m_stdu);
XTS_MODEL("stdx", m_stdx);
XTS_MODEL("stdux", m_stdux);
XTS_MODEL("sthbrx", m_sthbrx);
XTS_MODEL("stwbrx", m_stwbrx);
XTS_MODEL("stdbrx", m_stdbrx);

/* ------------------------------------------------------------------------------------------------------------------
 * Floating point loads/stores. No FPSCR effect, no exceptions.
 * ------------------------------------------------------------------------------------------------------------------ */

/* Book I 4.6.2 / 4.3.5.1: single in storage -> double in an FPR. A bit shuffle for normal/zero/infinity/NaN (a NaN
 * keeps its payload and is NOT quieted); a denormal is normalised. Exact, no rounding. */
static uint64_t ml_single_to_double(uint32_t s)
{
  uint64_t sign = (uint64_t)(s >> 31) << 63;
  uint32_t e = (s >> 23) & 0xFF, f = s & 0x7FFFFF;
  if (e == 0xFF) return sign | 0x7FF0000000000000ull | ((uint64_t)f << 29);
  if (e == 0) {
    if (!f) return sign;
    int p = 31 - __builtin_clz(f);                        /* leading one, 0..22; value = 2^(p-149) * 1.x */
    return sign | ((uint64_t)(p + 874) << 52) | (((uint64_t)f << (52 - p)) & 0xFFFFFFFFFFFFFull);
  }
  return sign | ((uint64_t)(e + 896) << 52) | ((uint64_t)f << 29);
}

/* Book I 4.6.3 SINGLE(): double in an FPR -> single in storage. For exponent > 896 (and zero) it is a bit shuffle
 * (out-of-range magnitudes are NOT rounded to infinity: the result is "well defined but not numerically equal"),
 * 874..896 is denormalised by truncation, smaller non-zero values are architecturally undefined (the model stores
 * the sign with a zero magnitude, which is what results4 shows). No rounding, no FPSCR effect. */
static uint32_t ml_double_to_single(uint64_t d)
{
  uint32_t e = (uint32_t)(d >> 52) & 0x7FF;
  if (e > 896 || (d & 0x7FFFFFFFFFFFFFFFull) == 0)
    return (uint32_t)((d >> 32) & 0xC0000000u) | (uint32_t)((d >> 29) & 0x3FFFFFFFu);
  uint32_t sign = (uint32_t)(d >> 63) << 31;
  if (e >= 874) {
    uint64_t frac = (1ull << 52) | (d & 0xFFFFFFFFFFFFFull);
    frac >>= (897 - e);
    return sign | (uint32_t)((frac >> 29) & 0x7FFFFF);
  }
  return sign;
}

static xts_mres_t fp_access(const acc_t *a, int is_store, int kind_single, int integer_word, uint32_t w,
                            const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  unsigned ra = XM_RA(w), fr = XM_RD(w);
  if (a->upd && ra == 0) return XTS_M_ILLEGAL;
  uint64_t ea = ea_of(a, w, in);
  if (ea & 3) {                                   /* FP accesses need word alignment on this machine (even lfd/stfd) */
    m->fault_ea = ea;
    return XTS_M_ALIGNMENT;
  }
  if (is_store) {
    uint64_t v = in->fpr[fr];
    int n = 8;
    if (kind_single) { v = ml_double_to_single(v); n = 4; }
    else if (integer_word) { v &= 0xFFFFFFFFull; n = 4; }       /* stfiwx: low word of the FPR, no conversion */
    if (xm_store(m, ea, n, v)) return XTS_M_DSI;
  } else {
    uint64_t v;
    if (xm_load(m, ea, a->size, &v)) return XTS_M_DSI;
    out->fpr[fr] = kind_single ? ml_single_to_double((uint32_t)v) : v;
  }
  if (a->upd) out->gpr[ra] = ea;
  return XTS_M_OK;
}

#define FFUN(nm, store, single, iw, ...)                                                                           \
  static xts_mres_t m_##nm(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out) \
  {                                                                                                                \
    static const acc_t a = { __VA_ARGS__ };                                                                        \
    (void)d;                                                                                                       \
    return fp_access(&a, store, single, iw, w, in, m, out);                                                        \
  }
/*          name    st sg iw   kind size sext rev upd */
FFUN(lfs,    0, 1, 0,  K_D, 4, 0, 0, 0)
FFUN(lfsu,   0, 1, 0,  K_D, 4, 0, 0, 1)
FFUN(lfsx,   0, 1, 0,  K_X, 4, 0, 0, 0)
FFUN(lfsux,  0, 1, 0,  K_X, 4, 0, 0, 1)
FFUN(lfd,    0, 0, 0,  K_D, 8, 0, 0, 0)
FFUN(lfdu,   0, 0, 0,  K_D, 8, 0, 0, 1)
FFUN(lfdx,   0, 0, 0,  K_X, 8, 0, 0, 0)
FFUN(lfdux,  0, 0, 0,  K_X, 8, 0, 0, 1)
FFUN(stfs,   1, 1, 0,  K_D, 4, 0, 0, 0)
FFUN(stfsu,  1, 1, 0,  K_D, 4, 0, 0, 1)
FFUN(stfsx,  1, 1, 0,  K_X, 4, 0, 0, 0)
FFUN(stfsux, 1, 1, 0,  K_X, 4, 0, 0, 1)
FFUN(stfd,   1, 0, 0,  K_D, 8, 0, 0, 0)
FFUN(stfdu,  1, 0, 0,  K_D, 8, 0, 0, 1)
FFUN(stfdx,  1, 0, 0,  K_X, 8, 0, 0, 0)
FFUN(stfdux, 1, 0, 0,  K_X, 8, 0, 0, 1)
FFUN(stfiwx, 1, 0, 1,  K_X, 4, 0, 0, 0)

XTS_MODEL("lfs", m_lfs);
XTS_MODEL("lfsu", m_lfsu);
XTS_MODEL("lfsx", m_lfsx);
XTS_MODEL("lfsux", m_lfsux);
XTS_MODEL("lfd", m_lfd);
XTS_MODEL("lfdu", m_lfdu);
XTS_MODEL("lfdx", m_lfdx);
XTS_MODEL("lfdux", m_lfdux);
XTS_MODEL("stfs", m_stfs);
XTS_MODEL("stfsu", m_stfsu);
XTS_MODEL("stfsx", m_stfsx);
XTS_MODEL("stfsux", m_stfsux);
XTS_MODEL("stfd", m_stfd);
XTS_MODEL("stfdu", m_stfdu);
XTS_MODEL("stfdx", m_stfdx);
XTS_MODEL("stfdux", m_stfdux);
XTS_MODEL("stfiwx", m_stfiwx);

/* ------------------------------------------------------------------------------------------------------------------
 * Load-reserve / store-conditional.
 *
 * The reservation is the only hidden state of this family: neither xts_state_t nor xts_mem_t has a slot for it, so the
 * model keeps ONE reservation (single thread) in a file static. It is set by every lwarx/ldarx that completes and
 * cleared by every stwcx./stdcx. that completes (success or not, as architected) - the same sequence the hardware sees
 * when each case runs through xts_semantic_check in order. results4 shows exactly that carry-over: its first
 * non-trapping stwcx. succeeded because an ldarx some jobs earlier had left a reservation in the same 128-byte line,
 * and every later stwcx./stdcx. failed. The granule is the 128-byte cache line (stwcx. at line+12 matched an ldarx at
 * line+64). Callers that need a defined starting point call xts_model_resv_clear() (see KNOWN.md).
 * ------------------------------------------------------------------------------------------------------------------ */
static struct { int valid; uint64_t line; } resv;

void xts_model_resv_clear(void) { resv.valid = 0; }
/* save / restore for callers that run the model twice for one case (host simulation + oracle) */
void xts_model_resv_get(int *valid, uint64_t *line) { *valid = resv.valid; *line = resv.line; }
void xts_model_resv_set(int valid, uint64_t line) { resv.valid = valid; resv.line = line; }

static xts_mres_t larx(uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out, int size)
{
  uint64_t ea = ml_trunc(in, ml_ra0(in, XM_RA(w)) + in->gpr[XM_RB(w)]), v;
  if (ea & (uint64_t)(size - 1)) {
    m->fault_ea = ea;
    return XTS_M_ALIGNMENT;
  }
  if (xm_load(m, ea, size, &v)) return XTS_M_DSI;
  out->gpr[XM_RD(w)] = v;
  resv.valid = 1;
  resv.line = ea >> 7;
  return XTS_M_OK;
}

static xts_mres_t stcx(uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out, int size)
{
  if (!(w & 1)) return XTS_M_ILLEGAL;             /* Rc must be 1 */
  uint64_t ea = ml_trunc(in, ml_ra0(in, XM_RA(w)) + in->gpr[XM_RB(w)]);
  if (ea & (uint64_t)(size - 1)) {
    m->fault_ea = ea;
    return XTS_M_ALIGNMENT;
  }
  int ok = resv.valid && resv.line == (ea >> 7);
  if (ok) {
    uint64_t v = in->gpr[XM_RS(w)];
    if (size < 8) v &= (1ull << (8 * size)) - 1;
    if (xm_store(m, ea, size, v)) return XTS_M_DSI;
  }
  resv.valid = 0;
  xm_set_cr_field(out, 0, (ok ? 2u : 0u) | (xm_xer_so(in) ? 1u : 0u));
  return XTS_M_OK;
}

static xts_mres_t m_lwarx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d;
  return larx(w, in, m, out, 4);
}
static xts_mres_t m_ldarx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d;
  return larx(w, in, m, out, 8);
}
static xts_mres_t m_stwcx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d;
  return stcx(w, in, m, out, 4);
}
static xts_mres_t m_stdcx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d;
  return stcx(w, in, m, out, 8);
}
XTS_MODEL("lwarx", m_lwarx);
XTS_MODEL("ldarx", m_ldarx);
XTS_MODEL("stwcx", m_stwcx);
XTS_MODEL("stdcx", m_stdcx);

/* ------------------------------------------------------------------------------------------------------------------
 * Load/store multiple and string. Registers wrap r31 -> r0 for the string forms. Hardware status "untested" (not in
 * results4): invalid forms (RA/RB in the loaded range ...) are modelled as illegal, lmw/stmw as needing EA % 4 == 0
 * (alignment interrupt otherwise, as on PPC970/Cell PPE); string ops never take an alignment interrupt.
 * ------------------------------------------------------------------------------------------------------------------ */
static xts_mres_t m_lmw(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d;
  unsigned rt = XM_RD(w), ra = XM_RA(w);
  if (ra >= rt) return XTS_M_ILLEGAL;             /* RA in the range to be loaded, incl. RA=0 with RT=0 */
  uint64_t ea = ml_trunc(in, ml_ra0(in, ra) + (uint64_t)XM_SIMM16(w));
  if (ea & 3) {
    m->fault_ea = ea;
    return XTS_M_ALIGNMENT;
  }
  uint32_t n = 32 - rt;
  uint8_t *p = xts_mem_ptr(m, ea, 4 * n);
  if (!p) return XTS_M_DSI;
  for (unsigned i = 0; i < n; i++) out->gpr[rt + i] = xts_be_load(p + 4 * i, 4);
  return XTS_M_OK;
}

static xts_mres_t m_stmw(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)out;
  unsigned rs = XM_RS(w), ra = XM_RA(w);
  uint64_t ea = ml_trunc(in, ml_ra0(in, ra) + (uint64_t)XM_SIMM16(w));
  if (ea & 3) {
    m->fault_ea = ea;
    return XTS_M_ALIGNMENT;
  }
  uint32_t n = 32 - rs;
  uint8_t *p = xts_mem_ptr(m, ea, 4 * n);
  if (!p) return XTS_M_DSI;
  for (unsigned i = 0; i < n; i++) xts_be_store(p + 4 * i, 4, in->gpr[rs + i]);
  return XTS_M_OK;
}
XTS_MODEL("lmw", m_lmw);
XTS_MODEL("stmw", m_stmw);

static xts_mres_t str_load(uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out, uint64_t ea, uint32_t n,
                           int indexed)
{
  unsigned rt = XM_RD(w), ra = XM_RA(w), rb = XM_RB(w);
  (void)in;
  if (indexed) {
    /* Xenon: an XER byte count of 0 (RT architecturally undefined) behaves exactly like a count of 1 - one byte into
     * the top of RT's low word, RT = RA or RB is illegal (semantic run 2026-10-19, lswx.count0: 14 of 14 cases) */
    if (!n) n = 1;
    if (rt == ra || rt == rb) return XTS_M_ILLEGAL;
  }
  uint32_t nr = (n + 3) / 4;
  for (uint32_t i = 0; i < nr; i++) {             /* RA (and RB for lswx) must not be one of the registers loaded */
    unsigned r = (rt + i) & 31;
    if (r == ra || (indexed && r == rb)) return XTS_M_ILLEGAL;
  }
  if (!n) return XTS_M_OK;
  uint8_t *p = xts_mem_ptr(m, ea, n);
  if (!p) return XTS_M_DSI;
  for (uint32_t i = 0; i < nr; i++) {
    uint64_t v = 0;
    for (int j = 0; j < 4; j++) v = (v << 8) | ((i * 4 + (uint32_t)j) < n ? p[i * 4 + (uint32_t)j] : 0);
    out->gpr[(rt + i) & 31] = v;
  }
  return XTS_M_OK;
}

static xts_mres_t str_store(uint32_t w, const xts_state_t *in, xts_mem_t *m, uint64_t ea, uint32_t n)
{
  unsigned rs = XM_RS(w);
  if (!n) return XTS_M_OK;
  uint8_t *p = xts_mem_ptr(m, ea, n);
  if (!p) return XTS_M_DSI;
  for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(in->gpr[(rs + i / 4) & 31] >> (24 - 8 * (i & 3)));
  return XTS_M_OK;
}

static xts_mres_t m_lswi(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d;
  uint32_t nb = XM_RB(w), n = nb ? nb : 32;
  return str_load(w, in, m, out, ml_trunc(in, ml_ra0(in, XM_RA(w))), n, 0);
}
static xts_mres_t m_lswx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d;
  uint64_t ea = ml_trunc(in, ml_ra0(in, XM_RA(w)) + in->gpr[XM_RB(w)]);
  return str_load(w, in, m, out, ea, (uint32_t)(in->xer & 0x7F), 1);
}
static xts_mres_t m_stswi(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)out;
  uint32_t nb = XM_RB(w), n = nb ? nb : 32;
  return str_store(w, in, m, ml_trunc(in, ml_ra0(in, XM_RA(w))), n);
}
static xts_mres_t m_stswx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)out;
  uint64_t ea = ml_trunc(in, ml_ra0(in, XM_RA(w)) + in->gpr[XM_RB(w)]);
  return str_store(w, in, m, ea, (uint32_t)(in->xer & 0x7F));
}
XTS_MODEL("lswi", m_lswi);
XTS_MODEL("lswx", m_lswx);
XTS_MODEL("stswi", m_stswi);
XTS_MODEL("stswx", m_stswx);

/* ------------------------------------------------------------------------------------------------------------------
 * Cache management, ordering, prefetch: no architected state change in this model (a single thread, memory is
 * coherent). dcbz / dcbz128 zero the 128-byte aligned block containing the EA (results4: both clear 128 bytes).
 * ------------------------------------------------------------------------------------------------------------------ */
static xts_mres_t m_nop(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)w; (void)in; (void)m; (void)out;
  return XTS_M_OK;
}
XTS_MODEL("sync", m_nop);
XTS_MODEL("eieio", m_nop);
XTS_MODEL("isync", m_nop);
XTS_MODEL("dst", m_nop);
XTS_MODEL("dstst", m_nop);
XTS_MODEL("dss", m_nop);
XTS_MODEL("dcbst", m_nop);
XTS_MODEL("dcbf", m_nop);
XTS_MODEL("dcbt", m_nop);
XTS_MODEL("dcbtst", m_nop);
XTS_MODEL("icbi", m_nop);
XTS_MODEL("xdcbt", m_nop);

static xts_mres_t m_dcbz(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)out;
  uint64_t ea = ml_trunc(in, ml_ra0(in, XM_RA(w)) + in->gpr[XM_RB(w)]) & ~127ull;
  uint8_t *p = xts_mem_ptr(m, ea, 128);
  if (!p) return XTS_M_DSI;
  memset(p, 0, 128);
  return XTS_M_OK;
}
XTS_MODEL("dcbz", m_dcbz);
XTS_MODEL("dcbz128", m_dcbz);
