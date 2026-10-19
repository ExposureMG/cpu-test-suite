/* ldst.lat - load/store timing per form (port + redesign of OpenXenos plans.ldst_jobs / forwarding / analysis_timing.an_ldst).
 *
 * Cases (case id prefix):
 *   lat.<form>      load-to-use latency of every load form at one cache level: the level is the cache axis value
 *                   (default -> l1; cold = dcbf'd lines visited once; l1/l2/mem = random cycle over 16K / 256K / 3M,
 *                   steady state). Under the data_align axis (cache default) the data is misaligned / line-crossing /
 *                   4 KB- or 64 KB-page-crossing by the axis offset for the form's width (xts_cond_data_offset).
 *                     GPR forms: "index chain" - the loaded value IS the offset of the next pointer:
 *                       F r4,<anchor+di> ; ldx r3,r3,r4      latency = step - (ref_idx - ref_pure)
 *                     where ref_idx is the same chain with `ld` and ref_pure the plain `ld r3,8(r3)` chase at the same
 *                     level (so the second load, whatever it costs, cancels exactly).
 *                     FPR/VR forms: no FPR/VR -> GPR path except memory or CR, so the chain goes through CR:
 *                       F f1 ; fcmpu cr1,f1,f2 ; mfocrf r7 ; rlwinm r7 (EQ -> V) ; ori r7,r7,V ; ldx r3,r3,r7
 *                     (the ori keeps the step V even if the compare were to fail, so the chain can never go wild)
 *                     latency = step - ref_cr + ref_pure, ref_cr = same chain with ld + cmpd. ASSUMES fcmpu/vcmpequw.
 *                     reach CR as fast as cmpd (detail.method says so); the level-to-level differences are exact.
 *   use.<form>      FPR/VR forms, L1: in-order use stall, F + dependent consumer vs F + independent consumer (extra)
 *   ltp.<form>      load reciprocal throughput: 8 rotating independent destinations and 8 fixed addresses (identical
 *                   address footprint at every depth, depths 8..64, method v2); data_align axis honoured
 *   stp.<form>      store reciprocal throughput, 8 addresses in one line and in 8 lines; data_align axis honoured
 *   lhs.<st>.<ld>.<rel>   store -> load penalty (load-hit-store): independent st;ld pairs with the load at relation
 *                   rel (exact, inside, covers, partial, adj_after, adj_before, sameline) minus the same pairs with the
 *                   load moved to another line (same misalignment). GPR, FPR and VR widths.
 *   xfer.<st>.<ld>  the same for register-file changes through memory (GPR->FPR, FPR->GPR, GPR->VR, VR->GPR, FPR<->VR)
 *   rt.<route>      loop-carried round trips through memory (g->f->g ...): latency of the whole trip
 *   mis.<form>.<kind>.<lat|tput>   explicit misalignment penalty (mis1, mis4, line, page4k, page64k) vs aligned
 *   resv.<stcx>.<pair|alone|lost>  lwarx/ldarx + stwcx./stdcx. costs (reservation held, none, lost to a store)
 * Axes: cache (lat only), data_align (lat at l1, ltp, stp). Combinations a case does not depend on emit nothing.
 * Every chain is written and walked back before it is used; FP/larx forms are never run at offsets that trap. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <xts/cond_util.h>
#include <xts/xts.h>
#include "mem_util.h"

enum { K_LAT = 1, K_USE, K_LTP, K_STP, K_LHS, K_RT, K_MIS, K_RESV };
enum { R_EXACT = 0, R_INSIDE, R_COVERS, R_PARTIAL, R_ADJ_AFTER, R_ADJ_BEFORE, R_SAMELINE, R_N };
static const char *const rel_name[R_N] = { "exact", "inside", "covers", "partial", "adj_after", "adj_before", "sameline" };
enum { MK_MIS1 = 1, MK_MIS4, MK_LINE, MK_PG4K, MK_PG64K, MK_N };
static const char *const mk_name[MK_N] = { "aligned", "mis1", "mis4", "line", "page4k", "page64k" };

#define LAT_ITERS 256
#define STEPS_MAX (32 * LAT_ITERS)     /* deepest default depth x iterations */
#define LHS_S 64                       /* store offset inside the test line */
#define LHS_OTHER 1024                 /* baseline: same geometry, 8 lines (8 L1 sets) further */

/* ------------------------------------------------------------------------------------------------- axis helpers */
static int cache_level(void)
{
  switch (xts_env.cache_state) {
  case 1: return MU_LV_COLD;
  case 3: return MU_LV_L2;
  case 4: return MU_LV_MEM;
  default: return MU_LV_L1;
  }
}
/* offset the data_align axis asks for, for an access of `w` bytes: the axis helper (line_cross 128-w/2, page_cross
 * 4096-w/2, page64k_cross 65536-w/2; w 1 cannot straddle -> 0) */
static uint32_t axis_offset(int w) { return xts_cond_data_offset((unsigned)w); }
/* element geometry the data_align kind needs: 0 none, 4 (12 KB elements, 4 KB crossing), 64 (192 KB elements, 64 KB crossing) */
static int axis_page(int dk) { return dk == 3 ? 4 : dk == 4 ? 64 : 0; }
static uint32_t mk_offset(int kind, int w)
{
  switch (kind) {
  case MK_MIS1: return 1;
  case MK_MIS4: return 4;
  case MK_LINE: return 128u - (uint32_t)w / 2;
  case MK_PG4K: return 4096u - (uint32_t)w / 2;
  case MK_PG64K: return 65536u - (uint32_t)w / 2;
  default: return 0;
  }
}
/* "would this offset take an alignment interrupt" (results4 rules) */
static int traps_at(const mu_form_t *f, uint32_t off) { return f->align_trap && (off % (uint32_t)f->align_trap); }

static int log2u(uint32_t v)
{
  int b = 0;
  while ((1u << b) < v) b++;
  return b;
}

/* level -> chain size and how runs start */
typedef struct lvp { uint32_t n; int warm, flush; uint32_t rotate; uint32_t ws; } lvp_t;
static void level_params(int lv, uint32_t stride, int page, lvp_t *p)
{
  memset(p, 0, sizeof *p);
  switch (lv) {
  case MU_LV_L2: p->ws = MU_WS_L2; p->warm = 1; break;
  case MU_LV_MEM: p->ws = MU_WS_MEM; p->rotate = STEPS_MAX; break;
  case MU_LV_COLD: p->ws = (STEPS_MAX + 64) * stride; p->flush = 1; break;
  default: p->ws = MU_WS_L1; p->warm = 1; break;
  }
  p->n = p->ws / stride;
  if (page) p->n = 4;
  if (p->n < 4) p->n = 4;
  p->ws = p->n * stride;
}

/* ------------------------------------------------------------------------------------------------ latency kernels */
enum { LM_GPR = 0, LM_CR, LM_REF_IDX, LM_REF_CR };
typedef struct lat_arg {
  const mu_form_t *f;
  int mode;
  mu_geom_t g;
  mu_chain_t *c;
  lvp_t lv;
  uint32_t runs;
} lat_arg_t;

static void emit_cr_tail(xts_kbuf_t *k, int vec, int V)
{
  int b = log2u((uint32_t)V);
  int src = vec ? 7 : 25;                               /* CR6[0] (all true) / CR1[EQ] as LSB bit numbers */
  xk_emit(k, XI_mfocrf, 0, 7, vec ? 0x02 : 0x40);
  xk_emit(k, XI_rlwinmx, 0, 7, 7, (b - src) & 31, 31 - b, 31 - b);
  xk_emit(k, XI_ori, 0, 7, 7, V);                      /* V whatever the compare said: data-dependent, never wild */
  xk_emit(k, XI_ldx, 0, 3, 3, 7);
}

static int lat_body(xts_kbuf_t *k, int depth, void *arg)
{
  lat_arg_t *a = arg;
  const mu_form_t *f = a->f;
  for (int i = 0; i < depth; i++) {
    switch (a->mode) {
    case LM_GPR:
      mu_emit_mem(k, f->insn, 4, 3, f->xform ? 5 : a->g.di);
      xk_emit(k, XI_ldx, 0, 3, 3, 4);
      break;
    case LM_REF_IDX:
      xk_emit(k, XI_ld, 0, 4, 0, 3);
      xk_emit(k, XI_ldx, 0, 3, 3, 4);
      break;
    case LM_REF_CR:
      xk_emit(k, XI_ld, 0, 4, 0, 3);
      xk_emit(k, XI_cmp, 0, 1, 1, 4, 6);
      emit_cr_tail(k, 0, 8);
      break;
    default:                                            /* LM_CR */
      if (f->file == 'f') {
        mu_emit_mem(k, f->insn, 1, 3, f->xform ? 5 : a->g.di);
        xk_emit(k, XI_fcmpu, 0, 1, 1, 2);
        emit_cr_tail(k, 0, a->g.V);
      } else {
        int v = f->vr128 ? 101 : 1;
        mu_emit_mem(k, f->insn, v, 3, 5);
        if (f->elem) {
          if (f->vr128) xk_emit(k, XI_vspltw128, 0, v, v, 0);
          else xk_emit(k, f->width == 1 ? XI_vspltb : f->width == 2 ? XI_vsplth : XI_vspltw, 0, v, v, 0);
        }
        if (f->vr128) xk_emit(k, XI_vcmpequw128, XTS_RC, 109, v, 102);
        else xk_emit(k, XI_vcmpequw, XTS_RC, 9, v, 2);
        emit_cr_tail(k, 1, a->g.V);
      }
      break;
    }
  }
  return 0;
}

static int lat_pre(xts_kbuf_t *k, int depth, void *arg)
{
  lat_arg_t *a = arg;
  (void)depth;
  if (!a->lv.warm) return 0;
  xk_emit(k, XI_orx, 0, 20, 3, 3);
  xk_li32(k, 21, (uint32_t)a->c->ptr_rel);
  xk_li32(k, 23, (uint32_t)a->c->data_rel);
  xk_li32(k, 24, a->c->n);
  int top = xk_loop_begin(k, 24);
  xk_emit(k, XI_lbzx, 0, 22, 20, 23);                  /* the data's (first) line */
  xk_emit(k, XI_ldx, 0, 20, 20, 21);                   /* the pointer's line, and on to the next anchor */
  xk_loop_end(k, top);
  return 0;
}

static int lat_setup(xts_exec_t *x, void *arg)
{
  lat_arg_t *a = arg;
  if (a->lv.flush) mu_chain_flush(a->c);
  xts_set_gpr(x, 3, mu_chain_addr_at(a->c, a->lv.rotate * a->runs));
  a->runs++;
  xts_set_gpr(x, 4, (uint64_t)a->g.V);                 /* a load that did not happen still steps correctly */
  xts_set_gpr(x, 5, (uint64_t)a->g.di);
  xts_set_gpr(x, 6, 8);
  uint64_t one = 0x3FF0000000000000ull;
  xts_set_fpr(x, 1, one);
  xts_set_fpr(x, 2, one);
  xts_vr_t z = xts_vr4(0, 0, 0, 0);
  xts_set_vr(x, 1, z); xts_set_vr(x, 2, z); xts_set_vr(x, 101, z); xts_set_vr(x, 102, z);
  return 0;
}

/* build the chain for (form, offset, page kind, level); 0 or <0 */
static mu_chain_t lchain;
static int lat_chain(const mu_form_t *f, uint32_t doff, int page, int lv, mu_geom_t *g, lvp_t *p, uint32_t seed)
{
  if (mu_geom_for(f, doff, page, g) < 0) return -1;
  level_params(lv, g->stride, page, p);
  if (mu_big(MU_BIG_BYTES, &lchain.buf) < 0) return -2;
  lchain.base = 0;
  lchain.n = p->n;
  lchain.stride = g->stride;
  lchain.jmod = 0;
  lchain.bdiv = 0;
  lchain.anchor = g->a;
  lchain.ptr_rel = g->ptr_rel;
  lchain.data_rel = g->data_rel;
  lchain.data_w = g->data_w;
  lchain.df = *f;
  lchain.data = f->file == 'g' ? (uint64_t)g->V : f->fpsingle ? 0x3F800000ull : 0x3FF0000000000000ull;
  lchain.random = 1;
  lchain.seed = seed;
  return mu_chain_build(&lchain) < 0 ? -3 : 0;
}

static double lat_run(xts_ctx_t *ctx, lat_arg_t *a, xts_tspec_t *s, xts_tresult_t *r, const char *unit)
{
  *s = (xts_tspec_t)XTS_TSPEC_DEFAULT;
  s->iters = LAT_ITERS;
  s->unit = unit;
  s->body = lat_body;
  s->pre = lat_pre;
  s->setup = lat_setup;
  s->arg = a;
  a->runs = 0;
  xts_time_line_layout(s, 1);                         /* top at byte 12 of a fetch group, whole-line depths */
  return mu_measure(ctx, s, r);
}

/* per-level references (ld chain), measured once per boot */
typedef struct lref { int valid; double pure, idx, cr; int st, attempts; int pst[3]; char pwhy[3][96]; } lref_t;
static lref_t lrefs[MU_LV_N];
static const lref_t *lat_refs(xts_ctx_t *ctx, int lv)
{
  lref_t *R = &lrefs[lv];
  if (R->valid) return R;
  static mu_form_t fld;
  mu_form_of(XI_ld, &fld);
  mu_geom_t g;
  lvp_t p;
  R->pure = R->idx = R->cr = NAN;
  R->st = XTS_ST_FAIL;
  if (lat_chain(&fld, 0, 0, lv, &g, &p, 0xAEF1u + (uint32_t)lv) < 0) return R;
  static lat_arg_t a;
  memset(&a, 0, sizeof a);
  a.f = &fld; a.g = g; a.c = &lchain; a.lv = p;
  xts_tspec_t s;
  xts_tresult_t r1, r2, r3;
  /* every lat.* record of this level carries the references' status: one noisy reference series would mark the whole
   * level noisy for the rest of the boot, so measure them again (up to 3 times) until all three are usable */
  for (int attempt = 0; attempt < 3; attempt++) {
    a.mode = LM_REF_IDX;
    R->idx = lat_run(ctx, &a, &s, &r1, "ld r4,0(r3) ; ldx r3,r3,r4");
    a.mode = LM_REF_CR;
    R->cr = lat_run(ctx, &a, &s, &r2, "ld ; cmpd cr1 ; mfocrf ; rlwinm ; ori ; ldx");
    /* pure chase over the same chain: the pointer is at +8 */
    xts_tspec_t sp = XTS_TSPEC_DEFAULT;
    sp.iters = LAT_ITERS;
    sp.line_layout = 1;                               /* mu_chase_measure applies the line layout */
    R->pure = mu_chase_measure(ctx, &lchain, p.warm, p.flush, p.rotate, &sp, &r3);
    R->st = xts_status_worst(xts_status_worst(r1.status, r2.status), r3.status);
    const xts_tresult_t *pr[3] = { &r1, &r2, &r3 };   /* idx, cr, pure: which part keeps a level noisy */
    for (int k = 0; k < 3; k++) {
      R->pst[k] = pr[k]->failed ? XTS_ST_FAIL : pr[k]->status;
      snprintf(R->pwhy[k], sizeof R->pwhy[k], "%s", pr[k]->why);
    }
    R->attempts = attempt + 1;
    if (xts_status_measured(R->st) || R->st == XTS_ST_SKIP) break;
  }
  R->valid = 1;
  return R;
}

/* ------------------------------------------------------------------------------------ throughput / pattern kernels */
enum { P_LINE = 0, P_LINES, P_LINE2, P_PG4K, P_PG64K };
typedef struct tp_arg {
  const mu_form_t *f;
  int pattern, use;        /* use: 0 none, 1 dependent consumer, 2 independent consumer */
  uint32_t off;
  int idx;                 /* RB value for X-forms */
  uint64_t base;
} tp_arg_t;

static uint32_t pat_pos(int pattern, int k, uint32_t off)
{
  switch (pattern) {
  case P_LINES: return 128u * (uint32_t)k + off;
  case P_LINE2: return 256u * (uint32_t)k + off;
  case P_PG4K: return 8192u * (uint32_t)(k & 1) + off;
  case P_PG64K: return 131072u * (uint32_t)(k & 1) + off;
  default: return 16u * (uint32_t)k + off;
  }
}
static int pattern_for(int dk_or_mk, int is_mk)
{
  if (is_mk) return dk_or_mk == MK_LINE ? P_LINE2 : dk_or_mk == MK_PG4K ? P_PG4K : dk_or_mk == MK_PG64K ? P_PG64K : P_LINE;
  return dk_or_mk == 2 ? P_LINE2 : dk_or_mk == 3 ? P_PG4K : dk_or_mk == 4 ? P_PG64K : P_LINE;   /* data_kind */
}

static int tp_body(xts_kbuf_t *k, int depth, void *arg)
{
  tp_arg_t *a = arg;
  const mu_form_t *f = a->f;
  int v128 = f->vr128;
  for (int j = 0; j < depth; j++) {
    int base = 16 + (j & 7);
    if (f->load) {
      int dst = f->file == 'g' ? 8 + (j & 7) : f->file == 'f' ? 8 + (j & 7) : (v128 ? 72 : 8) + (j & 7);
      if (a->use) dst = (f->file == 'v' && v128 ? 110 : 10) + (j & 3);
      mu_emit_mem(k, f->insn, dst, base, f->xform ? 5 : 0);
      if (a->use) {
        int res = (f->file == 'v' && v128 ? 120 : 20) + (j & 3);
        int src = a->use == 1 ? dst : (f->file == 'v' && v128 ? 103 : 3);
        if (f->file == 'f') xk_emit(k, XI_faddx, 0, res, src, 2);
        else if (v128) xk_emit(k, XI_vand128, 0, res, src, 102);
        else xk_emit(k, XI_vand, 0, res, src, 2);
      }
    } else {
      int src = f->file == 'v' && v128 ? 72 : 8;
      mu_emit_mem(k, f->insn, src, base, f->xform ? 5 : 0);
    }
  }
  return 0;
}

static int tp_setup(xts_exec_t *x, void *arg)
{
  tp_arg_t *a = arg;
  for (int kk = 0; kk < 8; kk++) xts_set_gpr(x, 16 + kk, a->base + pat_pos(a->pattern, kk, a->off));
  xts_set_gpr(x, 5, (uint64_t)a->idx);
  xts_set_gpr(x, 8, 0);
  xts_vr_t z = xts_vr4(0, 0, 0, 0);
  for (int r = 0; r < 32; r++) xts_set_fpr(x, r, 0);
  xts_set_fpr(x, 2, 0x3FF0000000000000ull);
  for (int r = 0; r < 128; r++) xts_set_vr(x, r, z);
  return 0;
}

static mu_buf_t region;
/* zero and warm the pattern region (first 256 KB + 128 of the big buffer) */
static int region_prep(void)
{
  if (mu_big(MU_BIG_BYTES, &region) < 0) return -1;
  memset(region.p, 0, 2u * 131072u + 4096u);
  xts_touch_range(region.p, 2u * 131072u + 4096u);
  return 0;
}

static double tp_run(xts_ctx_t *ctx, tp_arg_t *a, xts_tspec_t *s, xts_tresult_t *r, const char *unit)
{
  static const int dep[6] = { 8, 16, 24, 32, 48, 64 };
  *s = (xts_tspec_t)XTS_TSPEC_DEFAULT;
  for (int i = 0; i < 6; i++) s->depths[i] = dep[i];
  s->ndepths = 6;
  s->iters = 256;
  s->unit = unit;
  s->body = tp_body;
  s->setup = tp_setup;
  s->arg = a;
  a->base = region.addr;
  a->idx = a->f->lr == 2 ? 16 : 0;
  xts_time_line_layout(s, 1);                         /* top at byte 12 of a fetch group, whole-line depths */
  return mu_measure(ctx, s, r);
}

/* -------------------------------------------------------------------------------------------------- LHS kernels */
typedef struct lhs_arg {
  const mu_form_t *st, *ld;
  int32_t so, lo;
  uint64_t base;
} lhs_arg_t;

static int sreg(const mu_form_t *f) { return f->file == 'v' && f->vr128 ? 72 : 8; }
static int lhs_body(xts_kbuf_t *k, int depth, void *arg)
{
  lhs_arg_t *a = arg;
  for (int j = 0; j < depth; j++) {
    mu_emit_mem(k, a->st->insn, sreg(a->st), 3, 4);
    int dst = (a->ld->file == 'v' && a->ld->vr128 ? 74 : 10) + (j & 3);
    mu_emit_mem(k, a->ld->insn, dst, 3, 5);
  }
  return 0;
}
static int lhs_setup(xts_exec_t *x, void *arg)
{
  lhs_arg_t *a = arg;
  xts_set_gpr(x, 3, a->base);
  xts_set_gpr(x, 4, (uint64_t)a->so);
  xts_set_gpr(x, 5, (uint64_t)a->lo);
  xts_set_gpr(x, 8, 0);
  xts_set_fpr(x, 8, 0);
  xts_vr_t z = xts_vr4(0, 0, 0, 0);
  xts_set_vr(x, 8, z);
  xts_set_vr(x, 72, z);
  return 0;
}

static int rel_offsets(int rel, int sw, int lw, int32_t *so, int32_t *lo)
{
  int S = LHS_S;
  *so = S;
  *lo = S;
  switch (rel) {
  case R_EXACT: break;
  case R_INSIDE: if (lw >= sw) return -1; *lo = S + sw - lw; break;
  case R_COVERS: if (lw <= sw) return -1; *so = S + lw - sw; break;
  case R_PARTIAL: {
    if (sw < 2 || lw < 2) return -1;
    int kk = (sw < lw ? sw : lw) / 2;
    *lo = S + sw - kk;
    break;
  }
  case R_ADJ_AFTER: *lo = S + sw; break;
  case R_ADJ_BEFORE: *lo = S - lw; break;
  case R_SAMELINE: *lo = S + 32; break;
  default: return -1;
  }
  return 0;
}
/* legal for the pair: FP 4-byte alignment, natural alignment of vector element accesses */
static int rel_ok(const mu_form_t *st, const mu_form_t *ld, int32_t so, int32_t lo)
{
  if (traps_at(st, (uint32_t)so) || traps_at(ld, (uint32_t)lo)) return 0;
  if (st->file == 'v' && (so % st->width)) return 0;
  if (ld->file == 'v' && (lo % ld->width)) return 0;
  return 1;
}

/* ------------------------------------------------------------------------------------------------ round trips */
typedef struct rt_arg { const char *route; uint64_t base; } rt_arg_t;
static void rt_insns(char file, int *st, int *ld, int *reg)
{
  if (file == 'g') { *st = XI_stwx; *ld = XI_lwzx; *reg = 8; }
  else if (file == 'f') { *st = XI_stfsx; *ld = XI_lfsx; *reg = 8; }
  else { *st = XI_stvewx; *ld = XI_lvewx; *reg = 8; }
}
static int rt_body(xts_kbuf_t *k, int depth, void *arg)
{
  rt_arg_t *a = arg;
  int nh = (int)strlen(a->route) - 1;
  for (int i = 0; i < depth; i++)
    for (int h = 0; h < nh; h++) {
      int st, ld, rs, rl, d1, d2;
      rt_insns(a->route[h], &st, &d1, &rs);
      rt_insns(a->route[h + 1], &d2, &ld, &rl);
      mu_emit_mem(k, st, rs, 3, 16 + h);
      mu_emit_mem(k, ld, rl, 3, 16 + h);
    }
  return 0;
}
static int rt_setup(xts_exec_t *x, void *arg)
{
  rt_arg_t *a = arg;
  xts_set_gpr(x, 3, a->base);
  for (int h = 0; h < 8; h++) xts_set_gpr(x, 16 + h, (uint64_t)(64 + 128 * h));
  xts_set_gpr(x, 8, 0);
  xts_set_fpr(x, 8, 0);
  xts_set_vr(x, 8, xts_vr4(0, 0, 0, 0));
  return 0;
}

/* ----------------------------------------------------------------------------------------------- reservations */
typedef struct rs_arg { int stcx, larx, st, variant; uint64_t base; } rs_arg_t;
static int rs_body(xts_kbuf_t *k, int depth, void *arg)
{
  rs_arg_t *a = arg;
  for (int i = 0; i < depth; i++) {
    if (a->variant != 1) xk_emit(k, a->larx, 0, 8, 3, 5);
    if (a->variant == 2) xk_emit(k, a->st, 0, 9, 64, 3);
    xk_emit(k, a->stcx, 0, 8, 3, 5);
  }
  return 0;
}
static int rs_setup(xts_exec_t *x, void *arg)
{
  rs_arg_t *a = arg;
  xts_set_gpr(x, 3, a->base);
  xts_set_gpr(x, 5, 0);
  xts_set_gpr(x, 8, 0);
  xts_set_gpr(x, 9, 0);
  return 0;
}

/* -------------------------------------------------------------------------------------------------- enumeration */
static const int lhs_g_st[] = { XI_stbx, XI_sthx, XI_stwx, XI_stdx };
static const int lhs_g_ld[] = { XI_lbzx, XI_lhzx, XI_lwzx, XI_ldx };
static const int lhs_f_st[] = { XI_stfsx, XI_stfdx };
static const int lhs_f_ld[] = { XI_lfsx, XI_lfdx };
static const int lhs_v_st[] = { XI_stvebx, XI_stvehx, XI_stvewx, XI_stvx };
static const int lhs_v_ld[] = { XI_lvebx, XI_lvehx, XI_lvewx, XI_lvx };
static const struct { int st, ld, rel; } xfer_tab[] = {
  { XI_stwx, XI_lfsx, R_EXACT }, { XI_stdx, XI_lfdx, R_EXACT },          /* GPR -> FPR */
  { XI_stfsx, XI_lwzx, R_EXACT }, { XI_stfdx, XI_ldx, R_EXACT },         /* FPR -> GPR */
  { XI_stwx, XI_lvewx, R_EXACT }, { XI_stdx, XI_lvx, R_COVERS },         /* GPR -> VR */
  { XI_stvewx, XI_lwzx, R_EXACT }, { XI_stvx, XI_ldx, R_INSIDE },        /* VR -> GPR */
  { XI_stfsx, XI_lvewx, R_EXACT }, { XI_stfdx, XI_lvx, R_COVERS },       /* FPR -> VR */
  { XI_stvewx, XI_lfsx, R_EXACT }, { XI_stvx, XI_lfdx, R_INSIDE },       /* VR -> FPR */
};
static const char *const routes[] = { "gg", "ff", "vv", "gfg", "gvg", "fvf", "gfvg", "gvfg" };
static const struct { int insn; int load; } mis_tab[] = {
  { XI_lhz, 1 }, { XI_lwz, 1 }, { XI_ld, 1 }, { XI_lfd, 1 }, { XI_sth, 0 }, { XI_stw, 0 }, { XI_std, 0 }, { XI_stfd, 0 },
};

static int add_lhs(xts_caselist_t *cl, const char *pre, int st, int ld, int rel)
{
  mu_form_t fs, fl;
  if (mu_form_of(st, &fs) < 0 || mu_form_of(ld, &fl) < 0) return 0;
  int32_t so, lo;
  if (rel_offsets(rel, fs.width, fl.width, &so, &lo) < 0 || !rel_ok(&fs, &fl, so, lo) ||
      !rel_ok(&fs, &fl, so, lo + LHS_OTHER))
    return 0;
  xts_case_t *c = xts_case_addf(cl, "%s.%s.%s.%s", pre, xts_isa[st].mnem, xts_isa[ld].mnem, rel_name[rel]);
  if (!c) return -1;
  c->insn = ld;
  c->arg[0] = K_LHS;
  c->arg[1] = st;
  c->arg[2] = ld;
  c->arg[3] = rel;
  return 0;
}

static int ll_enum(xts_caselist_t *cl)
{
  mu_form_t f;
  for (int pass = 0; pass < 3; pass++)                     /* lat, then use, then throughput: similar cost per block */
    for (int id = 0; id < xts_isa_count; id++) {
      if (mu_form_of(id, &f) < 0 || !f.load) continue;
      if (pass == 1 && f.file == 'g') continue;
      xts_case_t *c = xts_case_addf(cl, "%s.%s", pass == 0 ? "lat" : pass == 1 ? "use" : "ltp", xts_isa[id].mnem);
      if (!c) return -1;
      c->insn = id;
      c->arg[0] = pass == 0 ? K_LAT : pass == 1 ? K_USE : K_LTP;
    }
  for (int id = 0; id < xts_isa_count; id++) {
    if (mu_form_of(id, &f) < 0 || f.load || f.resv) continue;
    xts_case_t *c = xts_case_addf(cl, "stp.%s", xts_isa[id].mnem);
    if (!c) return -1;
    c->insn = id;
    c->arg[0] = K_STP;
  }
  for (int rel = 0; rel < R_N; rel++) {
    for (int s = 0; s < 4; s++)
      for (int l = 0; l < 4; l++)
        if (add_lhs(cl, "lhs", lhs_g_st[s], lhs_g_ld[l], rel) < 0) return -1;
    for (int s = 0; s < 2; s++)
      for (int l = 0; l < 2; l++)
        if (add_lhs(cl, "lhs", lhs_f_st[s], lhs_f_ld[l], rel) < 0) return -1;
    for (int s = 0; s < 4; s++)
      for (int l = 0; l < 4; l++)
        if (add_lhs(cl, "lhs", lhs_v_st[s], lhs_v_ld[l], rel) < 0) return -1;
  }
  for (unsigned i = 0; i < sizeof xfer_tab / sizeof xfer_tab[0]; i++)
    if (add_lhs(cl, "xfer", xfer_tab[i].st, xfer_tab[i].ld, xfer_tab[i].rel) < 0) return -1;
  for (unsigned i = 0; i < sizeof routes / sizeof routes[0]; i++) {
    xts_case_t *c = xts_case_addf(cl, "rt.%s", routes[i]);
    if (!c) return -1;
    c->arg[0] = K_RT;
    c->arg[1] = (int64_t)i;
  }
  for (unsigned i = 0; i < sizeof mis_tab / sizeof mis_tab[0]; i++) {
    mu_form_t mf;
    mu_form_of(mis_tab[i].insn, &mf);
    for (int kind = MK_MIS1; kind < MK_N; kind++) {
      if (kind == MK_MIS1 && mf.file != 'g') continue;            /* FP: EA%4 != 0 traps */
      if (kind == MK_MIS4 && mf.width != 8) continue;
      for (int what = 0; what < (mis_tab[i].load ? 2 : 1); what++) {
        xts_case_t *c = xts_case_addf(cl, "mis.%s.%s.%s", xts_isa[mis_tab[i].insn].mnem, mk_name[kind],
                                      what ? "tput" : mis_tab[i].load ? "lat" : "tput");
        if (!c) return -1;
        c->insn = mis_tab[i].insn;
        c->arg[0] = K_MIS;
        c->arg[1] = kind;
        c->arg[2] = mis_tab[i].load ? what : 1;
      }
    }
  }
  static const char *const rv[] = { "pair", "alone", "lost" };
  for (int w = 0; w < 2; w++)
    for (int v = 0; v < 3; v++) {
      xts_case_t *c = xts_case_addf(cl, "resv.%s.%s", w ? "stdcx" : "stwcx", rv[v]);
      if (!c) return -1;
      c->insn = w ? XI_stdcx : XI_stwcx;
      c->arg[0] = K_RESV;
      c->arg[1] = v;
    }
  return 0;
}

/* -------------------------------------------------------------------------------------------------------- runs */
static const char *skip_metric = "latency";
static void emit_skip(xts_ctx_t *ctx, int insn, const char *why)
{
  mu_det_t d;
  mu_det_begin(&d);
  xj_kv_str(&d.j, "why", why);
  mu_emit_note(ctx, &d, skip_metric, insn, NAN, "cycles", XTS_ST_SKIP, NULL);
}

static void det_common(mu_det_t *d, const mu_form_t *f, const char *level, uint32_t size, uint32_t stride, uint32_t off)
{
  xj_kv_str(&d->j, "level", level);
  xj_kv_uint(&d->j, "size", size);
  xj_kv_uint(&d->j, "stride", stride);
  xj_kv_int(&d->j, "width", f ? f->width : 0);
  xj_kv_uint(&d->j, "offset", off);
  if (f) {
    char fl[16];
    snprintf(fl, sizeof fl, "%c%s%s%s%s", f->file, f->update ? "u" : "", f->xform ? "x" : "", f->brev ? "r" : "",
             f->sign ? "a" : "");
    xj_kv_str(&d->j, "form", fl);
  }
}

static int run_lat(const xts_case_t *c, xts_ctx_t *ctx)
{
  mu_form_t f;
  if (mu_form_of(c->insn, &f) < 0) return -1;
  int cs = xts_env.cache_state, dk = xts_env.data_kind;
  if (dk && cs) return 0;                                   /* misalignment only at L1 */
  int lv = cache_level();
  uint32_t doff = dk ? axis_offset(f.width) : 0;
  int page = axis_page(dk);
  if (dk) {
    if (f.file == 'v' && !f.lr) { emit_skip(ctx, c->insn, "EA low bits are ignored or select the element: no misaligned access"); return 0; }
    if (!doff) { emit_skip(ctx, c->insn, "a 1-byte access cannot straddle"); return 0; }
    if (traps_at(&f, doff)) { emit_skip(ctx, c->insn, "alignment interrupt at this offset (results4 rule): not timed"); return 0; }
    if (f.update && f.file != 'g') { emit_skip(ctx, c->insn, "update form: same data path as the non-update form"); return 0; }
  }
  const lref_t *R = lat_refs(ctx, lv);                      /* before our chain: the refs use the same region */
  mu_geom_t g;
  lvp_t p;
  int e = lat_chain(&f, doff, page, lv, &g, &p, (uint32_t)ctx->seed);
  if (e < 0) {
    emit_skip(ctx, c->insn, e == -1 ? "no chain geometry for this form/offset" : "big buffer / chain build failed");
    return 0;
  }
  static lat_arg_t a;
  memset(&a, 0, sizeof a);
  a.f = &f; a.g = g; a.c = &lchain; a.lv = p;
  a.mode = f.file == 'g' ? LM_GPR : LM_CR;
  xts_tspec_t s;
  xts_tresult_t r;
  char unit[64];
  snprintf(unit, sizeof unit, "%s ; %s", xts_isa[c->insn].mnem, a.mode == LM_GPR ? "ldx r3,r3,r4" : "cmp ; mfocrf ; rlwinm ; ori ; ldx");
  double step = lat_run(ctx, &a, &s, &r, unit);
  double lat = a.mode == LM_GPR ? step - (R->idx - R->pure) : step - R->cr + R->pure;
  mu_det_t d;
  mu_det_begin(&d);
  det_common(&d, &f, mu_level_name[lv], p.ws, g.stride, doff);
  xj_kv_str(&d.j, "method", a.mode == LM_GPR ? "index_chain: step - (ref_idx - ref_pure)"
                                             : "cr_chain: step - ref_cr + ref_pure (assumes FP/VR compare->CR = cmpd->CR)");
  xj_kv_double(&d.j, "step", step);
  xj_kv_double(&d.j, "ref_pure", R->pure);
  xj_kv_double(&d.j, a.mode == LM_GPR ? "ref_idx" : "ref_cr", a.mode == LM_GPR ? R->idx : R->cr);
  xj_kv_str(&d.j, "ref_status", xts_status_name(R->st));
  xj_kv_int(&d.j, "ref_attempts", R->attempts);
  if (!xts_status_measured(R->st)) {
    static const char *const pn[3] = { "idx", "cr", "pure" };
    xj_key(&d.j, "ref_parts");
    xj_obj(&d.j);
    for (int k = 0; k < 3; k++) {
      char v[128];
      snprintf(v, sizeof v, "%s%s%s", xts_status_name(R->pst[k]), R->pwhy[k][0] ? ": " : "", R->pwhy[k]);
      xj_kv_str(&d.j, pn[k], v);
    }
    xj_end(&d.j);
  }
  xj_kv_int(&d.j, "V", g.V);
  xj_kv_int(&d.j, "anchors", (int64_t)p.n);
  if (dk) xj_kv_int(&d.j, "data_kind", dk);
  xts_temit_t te = { .metric = "latency", XTS_TE_VALUE(lat), .no_negative = 1, .status = R->st };   /* ref status joins ours */
  if (R->st == XTS_ST_PASS && lv == MU_LV_L1 && !dk && f.file == 'g') te.baseline_key = "load_use_latency";
  te.extra_json = mu_det_end(&d);
  xts_time_emit_ex(ctx, &s, &r, c->insn, &te);
  return 0;
}

static int run_use(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (xts_env.cache_state || xts_env.data_kind) return 0;
  mu_form_t f;
  if (mu_form_of(c->insn, &f) < 0 || region_prep() < 0) return -1;
  static tp_arg_t a;
  memset(&a, 0, sizeof a);
  a.f = &f; a.pattern = P_LINE;
  xts_tspec_t sd, si;
  xts_tresult_t rd, ri;
  a.use = 1;
  double dep = tp_run(ctx, &a, &sd, &rd, "load + dependent consumer");
  static tp_arg_t b;
  b = a;
  b.use = 2;
  double ind = tp_run(ctx, &b, &si, &ri, "load + independent consumer");
  mu_det_t d;
  mu_det_begin(&d);
  det_common(&d, &f, "l1", 128, 16, 0);
  xj_kv_str(&d.j, "consumer", f.file == 'f' ? "fadd" : f.vr128 ? "vand128" : "vand");
  xj_kv_double(&d.j, "dep", dep);
  xj_kv_double(&d.j, "ind", ind);
  xts_time_emit_diff(ctx, &sd, &rd, &si, &ri, c->insn,
                     &(xts_temit_t){ .metric = "extra", .no_negative = 1, .extra_json = mu_det_end(&d) });
  return 0;
}

static int run_tp(const xts_case_t *c, xts_ctx_t *ctx, int store)
{
  if (xts_env.cache_state) return 0;
  mu_form_t f;
  if (mu_form_of(c->insn, &f) < 0) return -1;
  int dk = xts_env.data_kind;
  uint32_t off = dk ? axis_offset(f.width) : 0;
  if (dk) {
    if (f.file == 'v' && !f.lr) { emit_skip(ctx, c->insn, "EA low bits are ignored or select the element: no misaligned access"); return 0; }
    if (!off) { emit_skip(ctx, c->insn, "a 1-byte access cannot straddle"); return 0; }
    if (traps_at(&f, off)) { emit_skip(ctx, c->insn, "alignment interrupt at this offset (results4 rule): not timed"); return 0; }
  }
  if (region_prep() < 0) return -1;
  int npat = (store && !dk) ? 2 : 1;
  for (int pi = 0; pi < npat; pi++) {
    static tp_arg_t a;
    memset(&a, 0, sizeof a);
    a.f = &f;
    a.pattern = dk ? pattern_for(dk, 0) : pi ? P_LINES : P_LINE;
    a.off = off;
    xts_tspec_t s;
    xts_tresult_t r;
    char unit[48];
    snprintf(unit, sizeof unit, "%s, 8 rotating addresses", xts_isa[c->insn].mnem);
    tp_run(ctx, &a, &s, &r, unit);
    mu_det_t d;
    mu_det_begin(&d);
    det_common(&d, &f, "l1", 8u * (pi ? 128u : 16u), pi ? 128 : 16, off);
    static const char *const pn[] = { "line", "lines", "line_cross", "page4k_cross", "page64k_cross" };
    xj_kv_str(&d.j, "pattern", pn[a.pattern]);
    if (dk) xj_kv_int(&d.j, "data_kind", dk);
    xts_time_emit_ex(ctx, &s, &r, c->insn,
                     &(xts_temit_t){ .metric = "rthroughput", .floor = 1.0, .no_negative = 1, .extra_json = mu_det_end(&d) });
  }
  return 0;
}

static int run_lhs(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (xts_env.cache_state || xts_env.data_kind) return 0;
  mu_form_t fs, fl;
  if (mu_form_of((int)c->arg[1], &fs) < 0 || mu_form_of((int)c->arg[2], &fl) < 0 || region_prep() < 0) return -1;
  int32_t so, lo;
  if (rel_offsets((int)c->arg[3], fs.width, fl.width, &so, &lo) < 0) return -1;
  static lhs_arg_t a, b;
  a.st = &fs; a.ld = &fl; a.so = so; a.lo = lo; a.base = region.addr + 4096;
  b = a;
  b.lo = lo + LHS_OTHER;
  xts_tspec_t sa = XTS_TSPEC_DEFAULT, sb;
  xts_tresult_t ra, rb;
  sa.unit = "st ; ld (independent pairs)";
  sa.body = lhs_body;
  sa.setup = lhs_setup;
  sa.arg = &a;
  xts_time_line_layout(&sa, 1);                       /* both halves of the differential share the layout */
  sb = sa;
  sb.arg = &b;
  double va = mu_measure(ctx, &sa, &ra), vb = mu_measure(ctx, &sb, &rb);
  mu_det_t d;
  mu_det_begin(&d);
  xj_kv_str(&d.j, "store", xts_isa[fs.insn].mnem);
  xj_kv_str(&d.j, "load", xts_isa[fl.insn].mnem);
  xj_kv_str(&d.j, "relation", rel_name[c->arg[3]]);
  xj_kv_int(&d.j, "store_width", fs.width);
  xj_kv_int(&d.j, "width", fl.width);
  xj_kv_int(&d.j, "store_offset", so);
  xj_kv_int(&d.j, "offset", lo);
  xj_kv_int(&d.j, "baseline_offset", lo + LHS_OTHER);
  char files[8];
  snprintf(files, sizeof files, "%c>%c", fs.file, fl.file);
  xj_kv_str(&d.j, "files", files);
  xj_kv_double(&d.j, "pair_cycles", va);
  xj_kv_double(&d.j, "baseline_cycles", vb);
  xts_time_emit_diff(ctx, &sa, &ra, &sb, &rb, fl.insn,
                     &(xts_temit_t){ .metric = "penalty", .no_negative = 1, .extra_json = mu_det_end(&d) });
  return 0;
}

static int run_rt(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (xts_env.cache_state || xts_env.data_kind) return 0;
  if (region_prep() < 0) return -1;
  static rt_arg_t a;
  a.route = routes[c->arg[1]];
  a.base = region.addr + 8192;
  xts_tspec_t s = XTS_TSPEC_DEFAULT;
  xts_tresult_t r;
  s.unit = "round trip through memory (4-byte hops)";
  s.body = rt_body;
  s.setup = rt_setup;
  s.arg = &a;
  mu_measure(ctx, &s, &r);
  mu_det_t d;
  mu_det_begin(&d);
  xj_kv_str(&d.j, "route", a.route);
  xj_kv_int(&d.j, "hops", (int64_t)strlen(a.route) - 1);
  xj_kv_str(&d.j, "insns", "g: stwx/lwzx, f: stfsx/lfsx, v: stvewx/lvewx; each hop on its own line");
  xj_kv_int(&d.j, "width", 4);
  xts_time_emit_ex(ctx, &s, &r, -1, &(xts_temit_t){ .metric = "latency", .no_negative = 1, .extra_json = mu_det_end(&d) });
  return 0;
}

static int run_mis(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (xts_env.cache_state || xts_env.data_kind) return 0;
  mu_form_t f;
  if (mu_form_of(c->insn, &f) < 0) return -1;
  int kind = (int)c->arg[1], what = (int)c->arg[2];
  uint32_t off = mk_offset(kind, f.width);
  if (traps_at(&f, off)) { emit_skip(ctx, c->insn, "alignment interrupt at this offset"); return 0; }
  mu_det_t d;
  mu_det_begin(&d);
  xj_kv_str(&d.j, "kind", mk_name[kind]);
  if (what == 0) {                                           /* load latency, L1 chain, vs aligned */
    int page = kind == MK_PG4K ? 4 : kind == MK_PG64K ? 64 : 0;
    mu_geom_t g0, g1;
    lvp_t p0, p1;
    static lat_arg_t a0, a1;
    xts_tspec_t s0, s1;
    xts_tresult_t r0, r1;
    memset(&a0, 0, sizeof a0);
    if (lat_chain(&f, 0, page, MU_LV_L1, &g0, &p0, 0x515u) < 0) { emit_skip(ctx, c->insn, "chain build failed"); return 0; }
    a0.f = &f; a0.g = g0; a0.c = &lchain; a0.lv = p0; a0.mode = f.file == 'g' ? LM_GPR : LM_CR;
    double v0 = lat_run(ctx, &a0, &s0, &r0, "aligned chain step");
    if (lat_chain(&f, off, page, MU_LV_L1, &g1, &p1, 0x516u) < 0) { emit_skip(ctx, c->insn, "chain build failed"); return 0; }
    a1 = a0;
    a1.g = g1; a1.lv = p1;
    double v1 = lat_run(ctx, &a1, &s1, &r1, "misaligned chain step");
    det_common(&d, &f, "l1", p1.ws, g1.stride, off);
    xj_kv_str(&d.j, "measure", "load-to-use chain step, misaligned minus aligned");
    xj_kv_double(&d.j, "aligned", v0);
    xj_kv_double(&d.j, "misaligned", v1);
    xts_time_emit_diff(ctx, &s1, &r1, &s0, &r0, c->insn,
                       &(xts_temit_t){ .metric = "penalty", .no_negative = 1, .extra_json = mu_det_end(&d) });
    return 0;
  }
  if (region_prep() < 0) return -1;
  static tp_arg_t a0, a1;
  memset(&a0, 0, sizeof a0);
  a0.f = &f;
  a0.pattern = pattern_for(kind, 1);
  a0.off = 0;
  a1 = a0;
  a1.off = off;
  xts_tspec_t s0, s1;
  xts_tresult_t r0, r1;
  double v0 = tp_run(ctx, &a0, &s0, &r0, "aligned, same pattern");
  double v1 = tp_run(ctx, &a1, &s1, &r1, "misaligned");
  det_common(&d, &f, "l1", 0, 0, off);
  xj_kv_str(&d.j, "measure", f.load ? "load throughput, misaligned minus aligned" : "store throughput, misaligned minus aligned");
  xj_kv_double(&d.j, "aligned", v0);
  xj_kv_double(&d.j, "misaligned", v1);
  xts_time_emit_diff(ctx, &s1, &r1, &s0, &r0, c->insn,
                     &(xts_temit_t){ .metric = "penalty", .no_negative = 1, .extra_json = mu_det_end(&d) });
  return 0;
}

static int run_resv(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (xts_env.cache_state || xts_env.data_kind) return 0;
  if (region_prep() < 0) return -1;
  static rs_arg_t a;
  int dbl = c->insn == XI_stdcx;
  a.stcx = c->insn;
  a.larx = dbl ? XI_ldarx : XI_lwarx;
  a.st = dbl ? XI_std : XI_stw;
  a.variant = (int)c->arg[1];
  a.base = region.addr + 16384;
  xts_tspec_t s = XTS_TSPEC_DEFAULT;
  xts_tresult_t r;
  static const char *const un[] = { "larx ; stcx. (reservation held)", "stcx. alone (no reservation)",
                                    "larx ; store to the same line ; stcx. (reservation lost)" };
  s.unit = un[a.variant];
  s.body = rs_body;
  s.setup = rs_setup;
  s.arg = &a;
  mu_measure(ctx, &s, &r);
  mu_det_t d;
  mu_det_begin(&d);
  xj_kv_str(&d.j, "variant", a.variant == 0 ? "pair" : a.variant == 1 ? "alone" : "lost");
  xj_kv_int(&d.j, "width", dbl ? 8 : 4);
  xts_time_emit_ex(ctx, &s, &r, c->insn, &(xts_temit_t){ .metric = "cycles", .no_negative = 1, .extra_json = mu_det_end(&d) });
  return 0;
}

static int ll_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  skip_metric = c->arg[0] == K_LTP || c->arg[0] == K_STP ? "rthroughput" : c->arg[0] == K_MIS ? "penalty" : "latency";
  switch (c->arg[0]) {
  case K_LAT: return run_lat(c, ctx);
  case K_USE: return run_use(c, ctx);
  case K_LTP: return run_tp(c, ctx, 0);
  case K_STP: return run_tp(c, ctx, 1);
  case K_LHS: return run_lhs(c, ctx);
  case K_RT: return run_rt(c, ctx);
  case K_MIS: return run_mis(c, ctx);
  case K_RESV: return run_resv(c, ctx);
  default: return -1;
  }
}

XTS_FAMILY(ldst_lat, .kind = "sched", .axes = "cache,data_align", .enumerate = ll_enum, .run = ll_run, .block_cases = 32,
           .desc = "load-to-use latency per form and cache level, load/store throughput, store->load (LHS) matrix, "
                   "register-file transfers via memory, misalignment penalties, larx/stcx.");
