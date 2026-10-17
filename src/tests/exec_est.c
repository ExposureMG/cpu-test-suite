/* exec.est - estimate instructions (fres, frsqrte, vrefp, vrsqrtefp, vexptefp, vlogefp and the VMX128 forms) and the
 * tininess probe.
 *
 * WHY: the model cannot reproduce the hardware tables (KNOWN.md "fp (mfp)": 5-12 distinct inputs per op), so these results are
 * raw data to reconstruct them (emulator / compiler constant folder) plus an accuracy-bound check. Every run emits ONE
 * record, metric "estimate":
 *   status  the framework's verdict (xts_estimate_verdict, the same one xts_semantic_check uses for XTS_M_ESTIMATE):
 *           pass  every lane the model knows exactly matches bit for bit, every other lane with a defined bound is within it
 *                 (fres 2^-8, frsqrte 2^-5, vrefp/vrsqrtefp 2^-12), and nothing but the target (+ FPSCR / CR1) changed
 *           fail  some lane is outside its bound / exact value, or other state changed
 *           info  nothing could be checked (specials the model cannot give, denormals, exp2/log2 lanes without an exact answer)
 *           trap  the instruction took an interrupt      skip  host dry run (the model has no table: out == in)
 *   detail  {"i": input bits, "o": EXACT output bits}; scalar: hex strings, vector: 4 lanes (lane 0 first). Edge sweeps add
 *           "fpscr" (scalar); records that are not "pass" add the verdict's members ("lanes":[{x,cpu,verdict,ok}], ...).
 * The case_id of a record is "<op>.<sweep>#<run index>", e.g. "vrefp.main#1234"; the input is a pure function of it
 * (no random numbers), so a run is reproducible and the sweep can be re-derived from the id alone.
 *
 * Sweeps (inputs per op; vector ops pack 4 inputs per instruction, lane i = input 4*run+i; scalar ops run one):
 *   main  every top-mantissa bin x 3 low-bit patterns (all zero, all one, half = 1000..0) x exponent parities:
 *         recip/log2 E=0; rsqrt E=0 and E=1 (the exponent parity selects the half of the table).
 *         exp2: x = m/2^23, m = bin<<11 | pattern, i.e. every 12-bit fraction bin of x in [0,1).
 *         VECTOR: 12 top fraction bits = 4096 bins. vrefp/vrsqrtefp outputs look piecewise linear with full-width significands
 *         and the architected bound is 2^-12, so an index finer than 2^-12 relative cannot hide structure; 12 bits is also the
 *         widest the single-precision low bits (11 left) still let us check "depends on the top bits only" with 3 patterns.
 *         SCALAR: 9 top bits = 512 bins (outputs carry 8 (fres) / 9 (frsqrte) significant bits, and PPC estimate tables are
 *         indexed by <= 6 bits, so 9 leaves a margin of 8x; the 3 low patterns (43 low bits of the double) expose any
 *         dependence on more bits). The ROADMAP plan asked for about 1,500 inputs per scalar op.
 *   exp   a few exponents (E list per kind, both signs where legal) x 64 bins x pattern cycling: the output must be the
 *         E=0 result scaled (checks the exponent path, overflow/denormal boundaries stay away from the bound check).
 *   neg   exp2 only: x = -m/2^23 on every 4th bin (the table is indexed by the fraction of x - floor(x)).
 *   int   exp2 only: every integer -160..132 (exact results, saturation, flush, overflow).
 *   e1    log2 only: E = +-1 on every 16th bin x 3 patterns (result = E + log2(mantissa)).
 *   edge  specials: +-0, +-inf, NaN (quiet / signalling / payload), denormals, min/max normals, powers of two, the model's
 *         boundary values, 2^-129 / 2^126..2^128 for recip, +-126..-151 and 127..128 for exp2.
 * VMX128 forms run the same inputs on every 16th bin (bin = 16*k + 9) of the legacy sweep, so their outputs can be compared with
 * the legacy forms bin by bin (results4 showed the forms agree on every shared lane), with the 64-bin sweeps thinned to match.
 *
 * Conditions: .axes "sf,vscr_nj". The dense sweeps only need to prove the table does not depend on them: under sf=32 or
 * VSCR[NJ]=1 (any non-default value) only every 8th run of a sweep executes (the edge sweep and the tininess probe always run
 * in full; NJ matters for denormal inputs / results, which the edge sweep covers).
 *
 * Tininess probe (cases "tiny.*"): see tiny_run. */
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>

enum { SW_MAIN, SW_EXP, SW_NEG, SW_INT, SW_E1, SW_EDGE, SW_TINY, SW_COUNT };
static const char *const sw_name[SW_COUNT] = { "main", "exp", "neg", "int", "e1", "edge", "tiny" };

typedef struct {
  const char *name;
  int insn;
  int scalar, kind;       /* kind 0 recip, 1 rsqrt, 2 exp2, 3 log2 */
  int step;               /* 1: every bin; 16: bin = 16*k + 9 (VMX128 forms) */
  int bits;               /* top fraction bits used as the bin index */
} est_op_t;

static const est_op_t OPS[] = {
  { "fres", XI_fresx, 1, 0, 1, 9 },           { "frsqrte", XI_frsqrtex, 1, 1, 1, 9 },
  { "vrefp", XI_vrefp, 0, 0, 1, 12 },         { "vrsqrtefp", XI_vrsqrtefp, 0, 1, 1, 12 },
  { "vexptefp", XI_vexptefp, 0, 2, 1, 12 },   { "vlogefp", XI_vlogefp, 0, 3, 1, 12 },
  { "vrefp128", XI_vrefp128, 0, 0, 16, 12 },  { "vrsqrtefp128", XI_vrsqrtefp128, 0, 1, 16, 12 },
  { "vexptefp128", XI_vexptefp128, 0, 2, 16, 12 }, { "vlogefp128", XI_vlogefp128, 0, 3, 16, 12 },
};
#define NOPS ((int)(sizeof OPS / sizeof OPS[0]))
#define CHUNK 32               /* runs per case */

/* ---- bit helpers (integer only) ---- */
static int msb(uint64_t v) { return 63 - __builtin_clzll(v); }

static uint64_t w2d(uint32_t f)             /* single bits -> double bits (exact, denormals normalised) */
{
  uint64_t s = (uint64_t)(f >> 31) << 63;
  uint32_t e = (f >> 23) & 0xFF, m = f & 0x7FFFFF;
  if (e == 0xFF) return s | (0x7FFull << 52) | ((uint64_t)m << 29);
  if (e == 0) {
    if (!m) return s;
    int n = msb(m);
    return s | ((uint64_t)(1023 - 149 + n) << 52) | (((uint64_t)m << (52 - n)) & 0xFFFFFFFFFFFFFull);
  }
  return s | ((uint64_t)(e - 127 + 1023) << 52) | ((uint64_t)m << 29);
}

static uint32_t f32_ints(int64_t num, int sh)   /* num * 2^-sh as single bits, normal range, |num| < 2^31 */
{
  if (!num) return 0;
  uint32_t sg = num < 0;
  uint64_t u = num < 0 ? 0 - (uint64_t)num : (uint64_t)num;
  int n = msb(u);
  uint32_t m = n <= 23 ? (uint32_t)(u << (23 - n)) : (uint32_t)(u >> (n - 23));
  return (sg << 31) | ((uint32_t)(n - sh + 127) << 23) | (m & 0x7FFFFF);
}

static uint32_t nbins(const est_op_t *o) { return (1u << o->bits) / (uint32_t)o->step; }
static uint32_t sample_bins(const est_op_t *o, uint32_t want) { uint32_t n = nbins(o); return want < n ? want : n; }
static uint32_t real_bin(const est_op_t *o, uint32_t e) { return e * (uint32_t)o->step + (o->step > 1 ? 9u : 0u); }

static uint64_t low_pat(int low, int pat)
{
  return pat == 0 ? 0 : pat == 1 ? ((1ull << low) - 1) : (1ull << (low - 1));
}

/* sign, unbiased exponent E, real bin, low pattern -> input bits (32 or 64) */
static uint64_t mk_fp(const est_op_t *o, int sign, int E, uint32_t bin, int pat)
{
  int fb = o->scalar ? 52 : 23, low = fb - o->bits;
  uint64_t frac = ((uint64_t)bin << low) | low_pat(low, pat);
  if (o->scalar) return ((uint64_t)sign << 63) | ((uint64_t)(E + 1023) << 52) | frac;
  return ((uint64_t)sign << 31) | ((uint64_t)(E + 127) << 23) | frac;
}

/* ---- sweep definitions ---- */
static const int8_t exps_rec[] = { -100, -60, -20, -3, 3, 20, 60, 100 };
static const int8_t exps_rsq[] = { -100, -61, -20, -3, 3, 20, 61, 100 };
static const int8_t exps_exp[] = { -30, -12, 0, 1, 2, 3, 4, 5, 6, 7 };
static const int8_t exps_log[] = { 2, 3, 5, 10, 30, 60, 100, 126, -2, -5, -30, -60, -100, -126 };

static const int8_t *exp_list(const est_op_t *o, int *n)
{
  switch (o->kind) {
  case 0: *n = (int)sizeof exps_rec; return exps_rec;
  case 1: *n = (int)sizeof exps_rsq; return exps_rsq;
  case 2: *n = (int)sizeof exps_exp; return exps_exp;
  default: *n = (int)sizeof exps_log; return exps_log;
  }
}
static int nsigns(const est_op_t *o) { return (o->kind == 0 || o->kind == 2) ? 2 : 1; }

/* single-precision edge inputs per kind */
static const uint32_t edge_common[] = {
  0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001, 0xFF800001, 0x7FC00123, 0x7FFFFFFF,
  0x00000001, 0x80000001, 0x00000003, 0x007FFFFF, 0x807FFFFF, 0x00400000, 0x00800000, 0x80800000, 0x00800001, 0x7F7FFFFF,
  0xFF7FFFFF, 0x3F800000, 0xBF800000, 0x3F000000, 0x40000000, 0x3F800001, 0x3F7FFFFF, 0x34000000, 0x4B000000, 0x40400000,
};
static const uint32_t edge_recip[] = {
  0x00100000, 0x000FFFFF, 0x00080000, 0x00200000, 0x01000000, 0x7E7FFFFF, 0x7E800000, 0x7EFFFFFF, 0x7F000000, 0x7F000001,
  0x7F400000, 0xFE800000, 0xFF000000, 0x3FC00000, 0x40200000, 0x00FFFFFF,
};
static const uint32_t edge_rsqrt[] = { 0x3E800000, 0x40800000, 0x7F000000, 0x00000002, 0x3FC00000, 0x40100000, 0xC0000000, 0xFF7FFFFF };
static const uint32_t edge_exp2[] = {
  0x43000000, 0x42FFFFFF, 0x42FE0000, 0x43008000, 0x43010000, 0xC2FC0000, 0xC2FE0000, 0xC3000000, 0xC3150000, 0xC3158000,
  0xC3160000, 0xC3170000, 0xC3480000, 0xC2FDFFFF, 0xC3150001, 0x33800000, 0x0D800000, 0x2B8CBCCC, 0xBF7FFFFF, 0xBF800001,
  0x3F000000, 0xBF000000, 0x42FC0000, 0xC2FBFFFF, 0x43800000, 0xC3800000, 0x4F000000, 0xCF000000,
};
static const uint32_t edge_log2[] = {
  0x00000002, 0x00000004, 0x00200000, 0x3FB504F3, 0x3F3504F3, 0x7F000000, 0x00FFFFFF, 0x01000000, 0x3F7FFFFE, 0x3F800002,
  0x42800000, 0x3E800000, 0x7F7FFFFE, 0x40800000, 0x00000400, 0x80400000,
};
/* extra scalar inputs (double bits): double denormals / min / max, near-power-of-two doubles, the 2^-129 / 2^128 boundaries */
static const uint64_t edge_dbl[] = {
  0x0000000000000001ull, 0x000FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x8010000000000000ull, 0x7FEFFFFFFFFFFFFFull,
  0xFFEFFFFFFFFFFFFFull, 0x3FF0000000000001ull, 0x3FEFFFFFFFFFFFFFull, 0x37E0000000000000ull, 0x37D0000000000000ull,
  0x37F0000000000000ull, 0x47E0000000000000ull, 0x47F0000000000000ull, 0x4800000000000000ull, 0x4810000000000000ull,
  0x7FF0000000000001ull, 0x7FF8000000000001ull, 0xFFF8000000000000ull, 0x3FD5555555555555ull, 0x4000000000000001ull,
  0x3810000000000000ull, 0x380FFFFFFFFFFFFFull, 0x47EFFFFFE0000000ull, 0x47F0000020000000ull,
};
#define NA(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

static uint32_t edge_count(const est_op_t *o)
{
  uint32_t n = NA(edge_common);
  switch (o->kind) {
  case 0: n += NA(edge_recip); break;
  case 1: n += NA(edge_rsqrt); break;
  case 2: n += NA(edge_exp2); break;
  default: n += NA(edge_log2); break;
  }
  return n + (o->scalar ? NA(edge_dbl) : 0);
}

static uint64_t edge_input(const est_op_t *o, uint32_t i)
{
  const uint32_t *k = NULL;
  uint32_t nk = 0;
  switch (o->kind) {
  case 0: k = edge_recip; nk = NA(edge_recip); break;
  case 1: k = edge_rsqrt; nk = NA(edge_rsqrt); break;
  case 2: k = edge_exp2; nk = NA(edge_exp2); break;
  default: k = edge_log2; nk = NA(edge_log2); break;
  }
  uint32_t f;
  if (i < NA(edge_common)) f = edge_common[i];
  else if (i < NA(edge_common) + nk) f = k[i - NA(edge_common)];
  else return edge_dbl[i - NA(edge_common) - nk];
  return o->scalar ? w2d(f) : f;
}

static uint32_t sweep_count(const est_op_t *o, int sw)
{
  const uint32_t nb = nbins(o);
  int ne;
  exp_list(o, &ne);
  switch (sw) {
  case SW_MAIN: return (o->kind == 1 ? 2u : 1u) * nb * 3;
  case SW_EXP: return (uint32_t)(ne * nsigns(o)) * sample_bins(o, 64);
  case SW_NEG: return o->kind == 2 ? sample_bins(o, nb / 4) * 3 : 0;
  case SW_INT: return o->kind == 2 ? 293 : 0;
  case SW_E1: return o->kind == 3 ? 2 * sample_bins(o, nb / 16) * 3 : 0;
  case SW_EDGE: return edge_count(o);
  default: return 0;
  }
}

static uint64_t sweep_input(const est_op_t *o, int sw, uint32_t i)
{
  const uint32_t nb = nbins(o);
  switch (sw) {
  case SW_MAIN: {
    int p = (int)(i % 3);
    uint32_t e = (i / 3) % nb, g = i / (3 * nb), bin = real_bin(o, e);
    if (o->kind == 2) return f32_ints((int64_t)(((uint64_t)bin << 11) | low_pat(11, p)), 23);
    return mk_fp(o, 0, (int)g, bin, p);
  }
  case SW_EXP: {
    int ne;
    const int8_t *el = exp_list(o, &ne);
    uint32_t B = sample_bins(o, 64), k = i % B, rest = i / B;
    int s = (int)(rest % (uint32_t)nsigns(o)), ei = (int)(rest / (uint32_t)nsigns(o));
    uint32_t stride = nb / B, e = k * stride + stride / 2;
    return mk_fp(o, s, el[ei], real_bin(o, e), (int)((k + (uint32_t)ei) % 3));
  }
  case SW_NEG: {
    int p = (int)(i % 3);
    uint32_t e = (i / 3) * 4 + 1;
    uint64_t m = ((uint64_t)real_bin(o, e) << 11) | low_pat(11, p);
    return f32_ints(-(int64_t)m, 23);
  }
  case SW_INT: return f32_ints((int64_t)i - 160, 0);
  case SW_E1: {
    int p = (int)(i % 3);
    uint32_t B = sample_bins(o, nb / 16), k = (i / 3) % B, ei = i / (3 * B), stride = nb / B;
    return mk_fp(o, 0, ei ? 1 : -1, real_bin(o, k * stride + stride / 2), p);
  }
  default: return edge_input(o, i);
  }
}

/* ---- enumeration ---- */
static int est_enumerate(xts_caselist_t *cl)
{
  for (int oi = 0; oi < NOPS; oi++) {
    const est_op_t *o = &OPS[oi];
    for (int sw = 0; sw < SW_TINY; sw++) {
      uint32_t cnt = sweep_count(o, sw);
      if (!cnt) continue;
      uint32_t runs = o->scalar ? cnt : (cnt + 3) / 4;
      for (uint32_t r0 = 0; r0 < runs; r0 += CHUNK) {
        xts_case_t *c = xts_case_addf(cl, "%s.%s#%u", o->name, sw_name[sw], r0);
        if (!c) return -1;
        c->insn = o->insn;
        c->arg[0] = oi;
        c->arg[1] = sw;
        c->arg[2] = r0;
        c->arg[3] = runs - r0 < CHUNK ? runs - r0 : CHUNK;
      }
    }
  }
  for (int v = 0; v < 4; v++) {
    static const char *const tn[4] = { "fmul", "fmuls", "fmadd", "fmadds" };
    static const int ids[4] = { XI_fmulx, XI_fmulsx, XI_fmaddx, XI_fmaddsx };
    xts_case_t *c = xts_case_addf(cl, "tiny.%s#0", tn[v]);
    if (!c) return -1;
    c->insn = ids[v];
    c->arg[0] = -1;
    c->arg[1] = SW_TINY;
    c->arg[2] = v;
  }
  return 0;
}

/* ---- one estimate run ---- */
static void run_one(xts_ctx_t *ctx, const est_op_t *o, int sw, uint32_t run, uint32_t cnt)
{
  static xts_exec_t x;
  char det[1280], id[48];
  uint64_t in[4], out[4];
  int nl = o->scalar ? 1 : 4;
  for (int l = 0; l < nl; l++) {
    uint32_t i = o->scalar ? run : run * 4 + (uint32_t)l;
    in[l] = sweep_input(o, sw, i < cnt ? i : cnt - 1);
  }
  xts_case_init(&x);
  uint32_t w;
  int vd = o->step > 1 ? 40 : 3, vb = o->step > 1 ? 77 : 4;
  if (o->scalar) {
    xts_set_fpr(&x, 4, in[0]);
    w = xts_enc(o->insn, 0, 3, 4);
  } else {
    xts_set_vr(&x, vb, xts_vr4((uint32_t)in[0], (uint32_t)in[1], (uint32_t)in[2], (uint32_t)in[3]));
    w = xts_enc(o->insn, 0, vd, vb);
  }
  snprintf(id, sizeof id, "%s.%s#%u", o->name, sw_name[sw], run);
  xts_record_t r;
  xts_record_init(&r);
  r.case_id = id;
  r.metric = "estimate";
  r.word = w;
  r.has_word = 1;
  xts_json_t j;
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  if (!w) {
    r.status = XTS_ST_FAIL;
    xj_kv_str(&j, "why", "encoder rejected the operands");
  } else {
    int st = xts_exec_word(&x, w);
    xj_key(&j, "i");
    if (o->scalar) xj_hex(&j, in[0], 16);
    else { xj_arr(&j); for (int l = 0; l < 4; l++) xj_hex(&j, in[l], 8); xj_end(&j); }
    if (st == XTS_EXEC_NOTRUN || st == XTS_EXEC_BADKERNEL) {
      r.status = XTS_ST_FAIL;
      xj_kv_str(&j, "exec_error", x.err);
    } else if (st == XTS_EXEC_DRYRUN) {
      r.status = XTS_ST_SKIP;
      xj_kv_str(&j, "why", "host dry run: not executed");
    } else if (x.trap.trapped) {
      r.status = XTS_ST_TRAP;
      xj_kv_hex(&j, "vector", x.trap.vector, 4);
      xj_kv_hex(&j, "cause", x.trap.cause, 2);
    } else {
      for (int l = 0; l < nl; l++) out[l] = o->scalar ? x.out.fpr[3] : x.out.vr[vd].w[l];
      r.status = xts_estimate_verdict(o->insn, w, &x.in, &x.out, NULL);
      xj_key(&j, "o");
      if (o->scalar) xj_hex(&j, out[0], 16);
      else { xj_arr(&j); for (int l = 0; l < 4; l++) xj_hex(&j, out[l], 8); xj_end(&j); }
      if (o->scalar && sw == SW_EDGE) xj_kv_hex(&j, "fpscr", (uint32_t)x.out.fpscr, 8);
      if (r.status != XTS_ST_PASS) xts_estimate_verdict(o->insn, w, &x.in, &x.out, &j);   /* the per-lane detail */
    }
  }
  xj_finish(&j);
  r.detail = j.buf;
  r.detail_truncated = j.truncated;
  xts_emit(ctx, &r);
}

/* ---- tininess probe ----
 * Does Xenon detect tininess before or after rounding? The product below has an exact value just under the minimum normal
 * (2^-126 or 2^-1022 times (1 - tiny)) that rounds UP to the minimum normal with RN=0:
 *   double: 0x3FEFFFFFFFFFFFFE * 0x0010000000000001 = 2^-1022 (1 - 2^-104)
 *   single: 0x21FFFFFE * 0x1E000001 (as doubles) = 2^-126 (1 - 2^-46)   [(2^24-2)/2^23 * (1 + 2^-23) * 2^-127]
 * Before-rounding tininess raises UX (FPSCR 8A064000-like: FX|UX|XX|FI|FPRF=+normal), after-rounding does not (82064000-like).
 * fmadd / fmadds add +0 (same exact value). Two records per case: the semantic check against the model (which assumes
 * before-rounding; pass = same), then metric "tininess" (status info) with FPSCR and the verdict. */
static int tiny_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  static xts_exec_t x;
  int v = (int)c->arg[2], single = v & 1, fma = v >> 1;
  uint64_t a = single ? w2d(0x21FFFFFEu) : 0x3FEFFFFFFFFFFFFEull;
  uint64_t b = single ? w2d(0x1E000001u) : 0x0010000000000001ull;
  uint64_t minnorm = single ? 0x3810000000000000ull : 0x0010000000000000ull;
  xts_case_init(&x);
  xts_set_fpr(&x, 1, a);
  xts_set_fpr(&x, 2, b);
  xts_set_fpr(&x, 3, 0);
  uint32_t w = fma ? xts_enc(single ? XI_fmaddsx : XI_fmaddx, 0, 5, 1, 2, 3) : xts_enc(single ? XI_fmulsx : XI_fmulx, 0, 5, 1, 2);
  if (!w) return -1;
  int st = xts_semantic_check(ctx, &x, w, "tininess probe: model assumes before rounding (UX set)");
  char det[320];
  xts_json_t j;
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  xj_kv_hex(&j, "a", a, 16);
  xj_kv_hex(&j, "c", b, 16);
  xj_kv_hex(&j, "res", x.out.fpr[5], 16);
  xj_kv_hex(&j, "fpscr", (uint32_t)x.out.fpscr, 8);
  xj_kv_int(&j, "ux", (x.out.fpscr & XTS_FPSCR_UX) != 0);
  const char *verdict = "indeterminate";
  if (x.out.fpr[5] == minnorm) verdict = (x.out.fpscr & XTS_FPSCR_UX) ? "before-rounding" : "after-rounding";
  xj_kv_str(&j, "tininess", verdict);
  xj_kv_str(&j, "model", "before-rounding");
  xj_kv_int(&j, "semantic_status", st);
  xj_finish(&j);
  xts_record_t r;
  xts_record_init(&r);
  r.metric = "tininess";
  r.word = w;
  r.has_word = 1;
  r.status = XTS_ST_INFO;
  r.detail = j.buf;
  r.detail_truncated = j.truncated;
  xts_emit(ctx, &r);
  return 0;
}

static int est_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  int sw = (int)c->arg[1];
  if (sw == SW_TINY) return tiny_run(c, ctx);
  const est_op_t *o = &OPS[c->arg[0]];
  uint32_t cnt = sweep_count(o, sw), r0 = (uint32_t)c->arg[2], n = (uint32_t)c->arg[3];
  int thin = sw != SW_EDGE && (!xts_env.sf || (xts_env.vscr_set & XTS_VSCR_NJ));
  for (uint32_t k = 0; k < n; k++) {
    if (thin && ((r0 + k) & 7)) continue;
    run_one(ctx, o, sw, r0 + k, cnt);
  }
  if (!ctx->nrecords) xts_emit_status(ctx, XTS_ST_SKIP, "{\"why\":\"thinned: non-default condition\"}");
  return 0;
}

XTS_FAMILY(exec_est, .kind = "semantic", .axes = "sf,vscr_nj", .enumerate = est_enumerate, .run = est_run, .block_cases = 16,
           .desc = "estimate tables (fres frsqrte vrefp vrsqrtefp vexptefp vlogefp + 128 forms): structured sweeps, exact output bits, "
                   "accuracy bound; tininess probe");
