/* mem.hier - memory hierarchy (port + redesign of OpenXenos plans.memlat_jobs / analysis_timing.an_memlat).
 *
 * Cases (case id prefix):
 *   chase.rand.<size>   load-to-use latency of a random pointer chase (one pointer per 128-byte line) for 4K..64M, fine
 *                       steps around the 32K and 1M knees. <= 8M: the whole cycle is walked once before the timed loop;
 *                       > 8M: every run starts 8192 anchors further along the cycle, so no run re-reads recent lines.
 *   chase.levels        levels and knees of that curve (plateaus as in analysis_timing.curve); L1/L2/memory latency and
 *                       the capacities where they end. A summary of the family's chase.rand records, emitted by the
 *                       family's finish() hook (so also after a resume), per other_cores value; points that no record
 *                       has are measured then (not after a hang/crash block, not on the host).
 *   chase.seq.<size>    the same in address order (hardware prefetch / open-page effects): ratio to random in detail
 *   stride.<s>          64 lines at stride 128..256K (incl. the 8K-64K gap the old run missed), random order
 *   assoc.l1, assoc.l2  conflict sets: m lines at a stride that is a multiple of the set stride; per stride the number of
 *                       lines that still hit the level, then ways = capacity at the largest stride, set stride = smallest
 *                       stride with that capacity; confident only if another stride agrees or the stride below holds 2x.
 *                       L2 strides stay <= 256K with <= 16 lines there so the 2-way 64-entry ERAT never conflicts.
 *   stream.<kind>.<cold|l2>  bytes per cycle of load / store / dcbz+store / stvx streams over flushed or L2-resident lines
 *   pfstream.<op>       load stream with dcbt/dcbtst/xdcbt 8 lines ahead vs none
 *   cop.<op>.<state>    dcbt dcbtst xdcbt dcbz dcbz128 dcbst dcbf on a resident line / after a store / on cold lines,
 *                       each minus the same kernel with ori 0,0,0 in its place (issue_cost)
 *   prefetch.xdcbt2     ROADMAP xdcbt2: per fresh line X: prefetch (none, ld, dcbt, dcbtst, xdcbt), ~900 cycles of
 *                       dependent mulld, an L1 eviction set of 12 lines (3 x 4 ways, in L2 sets X never uses) touched
 *                       twice (or the same loads to another L1 set = no eviction), then the timed reload of X whose value
 *                       is the next X. Controls: ld+evict must reload at L2 level, none+evict at memory level, else the
 *                       verdict is "undetermined".
 *   tlb.<pages>         one line per 64 KB page (spread over all L1/L2 sets) vs the same lines packed (control):
 *                       translation penalty per load for 8..2048 pages (ERAT 64 entries, TLB 1K entries; libxenon reloads
 *                       the TLB in software - first touch of a page is a miss interrupt). tlb.knees summarises the
 *                       tlb.* records (finish() hook, like chase.levels).
 * Axis: other_cores (applied later by the SMT work): only the latency/bandwidth cases run under it (chase.*, stream.*). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include "mem_util.h"

enum { H_RAND = 1, H_SEQ, H_STRIDE, H_ASSOC, H_STREAM, H_PFSTREAM, H_COP, H_XDCBT, H_TLB };

static const uint32_t rand_kb[] = { 4, 8, 12, 16, 20, 24, 28, 30, 32, 34, 36, 40, 48, 56, 64, 96, 128, 192, 256, 384, 512,
                                    640, 768, 832, 896, 960, 1024, 1088, 1152, 1280, 1536, 2048, 3072, 4096, 6144, 8192,
                                    16384, 32768, 65536 };
#define NRAND (int)(sizeof rand_kb / sizeof rand_kb[0])
static const uint32_t seq_kb[] = { 16, 64, 256, 512, 1024, 2048, 4096, 32768 };
static const uint32_t strides[] = { 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144 };
static const uint32_t tlb_pages[] = { 8, 16, 32, 48, 64, 80, 96, 128, 192, 256, 384, 512, 768, 1024, 1280, 1536, 2048 };
#define NTLB (int)(sizeof tlb_pages / sizeof tlb_pages[0])
static const char *const stream_kinds[] = { "ld_line", "ld_full", "lvx_full", "st_sparse", "st_full", "stvx_full",
                                            "dcbz_stvx", "dcbz128_stvx", "dcbz_only" };
#define NSTREAM 9
static const int cop_ops[] = { XI_dcbt, XI_dcbtst, XI_xdcbt, XI_dcbz, XI_dcbz128, XI_dcbst, XI_dcbf };
static const char *const cop_states[] = { "resident", "after_store", "cold_stream" };
static const int pf_ops[] = { -1, XI_dcbt, XI_dcbtst, XI_xdcbt };

#define WARM_MAX (8u * MU_MB)
#define STEPS 8192u              /* deepest default depth (32) x 256 iterations */

/* per boot, per other_cores value: the random-chase points measured so far (for the chase.seq ratio and for finish()) */
static double rand_pts[8][NRAND];
static int rand_have[8][NRAND];

static int oc_index(void) { return xts_env.other_cores & 7; }

/* ------------------------------------------------------------------------------------------------------- chases */
static mu_chain_t hc;
static double chase_point(xts_ctx_t *ctx, uint32_t base, uint32_t n, uint32_t stride, uint32_t jmod, uint32_t jstep,
                          int random, uint32_t seed, int warm, uint32_t rotate, xts_tspec_t *s, xts_tresult_t *r, int *err)
{
  *err = 0;
  if (mu_big(base + n * stride + (jmod ? jmod * jstep : 0) + MU_PAGE, &hc.buf) < 0) { *err = -2; return NAN; }
  hc.base = base;
  hc.n = n;
  hc.stride = stride;
  hc.jmod = jmod;
  hc.jstep = jstep;
  hc.bdiv = 0;
  hc.anchor = 0;
  hc.ptr_rel = 0;
  hc.data_w = 0;
  hc.random = random;
  hc.seed = seed;
  if (mu_chain_build(&hc) < 0) { *err = -3; return NAN; }
  *s = (xts_tspec_t)XTS_TSPEC_DEFAULT;
  s->unit = random ? "ld r3,0(r3) random cycle" : "ld r3,0(r3) sequential cycle";
  return mu_chase_measure(ctx, &hc, warm, 0, rotate, s, r);
}

static void det_chase(mu_det_t *d, const char *level, uint32_t size, uint32_t stride, uint32_t n, const char *order)
{
  xj_kv_str(&d->j, "level", level);
  xj_kv_uint(&d->j, "size", size);
  xj_kv_uint(&d->j, "stride", stride);
  xj_kv_int(&d->j, "width", 8);
  xj_kv_uint(&d->j, "offset", 0);
  xj_kv_uint(&d->j, "lines", n);
  xj_kv_str(&d->j, "order", order);
}

static const char *level_of(xts_ctx_t *ctx, double v)
{
  const mu_levels_t *L = mu_levels(ctx);
  int k = mu_classify(v, L->lat, 3);
  return k == 0 ? "l1" : k == 1 ? "l2" : k == 2 ? "mem" : "between";
}

static void emit_fail(xts_ctx_t *ctx, const char *metric, int err, const char *what)
{
  mu_det_t d;
  mu_det_begin(&d);
  xj_kv_str(&d.j, "why", err == -2 ? "big buffer allocation failed" : "chain build/verify failed");
  xj_kv_str(&d.j, "what", what);
  mu_emit_note(ctx, &d, metric, -1, NAN, !strcmp(metric, "bytes_per_cycle") ? "bytes_per_cycle" : "cycles",
               err == -2 ? XTS_ST_SKIP : XTS_ST_FAIL, NULL);
}

static double rand_measure(xts_ctx_t *ctx, int i, xts_tspec_t *s, xts_tresult_t *r, int *err)
{
  uint32_t ws = rand_kb[i] * MU_KB, n = ws / MU_LINE;
  int warm = ws <= WARM_MAX;
  double v = chase_point(ctx, 0, n, MU_LINE, 0, 0, 1, 0xC4A5Eu + (uint32_t)i, warm, warm ? 0 : STEPS, s, r, err);
  if (!*err && r->ok && (r->status == XTS_ST_PASS || r->status == XTS_ST_NOISY)) { rand_pts[oc_index()][i] = v; rand_have[oc_index()][i] = 1; }
  return v;
}

static int run_rand(const xts_case_t *c, xts_ctx_t *ctx)
{
  int i = (int)c->arg[1], err;
  mu_levels(ctx);                                          /* references first (they share the buffer) */
  xts_tspec_t s;
  xts_tresult_t r;
  double v = rand_measure(ctx, i, &s, &r, &err);
  if (err) { emit_fail(ctx, "latency", err, c->id); return 0; }
  mu_det_t d;
  mu_det_begin(&d);
  uint32_t ws = rand_kb[i] * MU_KB;
  det_chase(&d, level_of(ctx, v), ws, MU_LINE, ws / MU_LINE, "random");
  xj_kv_uint(&d.j, "pages", (ws + MU_PAGE - 1) / MU_PAGE);
  xj_kv_str(&d.j, "start", ws <= WARM_MAX ? "whole cycle walked once first" : "rotating start, cold lines");
  xts_time_emit_ex(ctx, &s, &r, XI_ld, &(xts_temit_t){ .metric = "latency", .no_negative = 1, .extra_json = mu_det_end(&d) });
  return 0;
}

static int run_seq(const xts_case_t *c, xts_ctx_t *ctx)
{
  int i = (int)c->arg[1], err;
  mu_levels(ctx);
  uint32_t ws = seq_kb[i] * MU_KB, n = ws / MU_LINE;
  xts_tspec_t s;
  xts_tresult_t r;
  int warm = ws <= WARM_MAX;
  double v = chase_point(ctx, 0, n, MU_LINE, 0, 0, 0, 0, warm, warm ? 0 : STEPS, &s, &r, &err);
  if (err) { emit_fail(ctx, "latency", err, c->id); return 0; }
  mu_det_t d;
  mu_det_begin(&d);
  det_chase(&d, level_of(ctx, v), ws, MU_LINE, n, "sequential");
  for (int k = 0; k < NRAND; k++)
    if (rand_kb[k] == seq_kb[i] && rand_have[oc_index()][k]) xj_kv_double(&d.j, "ratio_to_random", v / rand_pts[oc_index()][k]);
  xts_time_emit_ex(ctx, &s, &r, XI_ld, &(xts_temit_t){ .metric = "latency", .no_negative = 1, .extra_json = mu_det_end(&d) });
  return 0;
}

static int run_stride(const xts_case_t *c, xts_ctx_t *ctx)
{
  uint32_t st = strides[c->arg[1]];
  int err;
  mu_levels(ctx);
  xts_tspec_t s;
  xts_tresult_t r;
  double v = chase_point(ctx, 0, 64, st, 0, 0, 1, 0x57u + st, 1, 0, &s, &r, &err);
  if (err) { emit_fail(ctx, "latency", err, c->id); return 0; }
  mu_det_t d;
  mu_det_begin(&d);
  det_chase(&d, level_of(ctx, v), 64 * st, st, 64, "random");
  xj_kv_uint(&d.j, "l1_sets_used", st >= 8192 ? 1 : 8192 / st < 64 ? 8192 / st : 64);
  xj_kv_uint(&d.j, "l2_sets_used_if_unhashed", st >= 131072 ? 1 : 131072 / st < 64 ? 131072 / st : 64);
  xts_time_emit_ex(ctx, &s, &r, XI_ld, &(xts_temit_t){ .metric = "latency", .no_negative = 1, .extra_json = mu_det_end(&d) });
  return 0;
}

/* ---------------------------------------------------------------------------------------------- associativity */
static int run_assoc(const xts_case_t *c, xts_ctx_t *ctx)
{
  int l2 = (int)c->arg[1];
  static const uint32_t st1[] = { 4096, 8192, 16384, 32768 }, st2[] = { 32768, 65536, 131072, 262144 };
  static const int m1[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16 };
  static const int m2[] = { 4, 6, 7, 8, 9, 10, 12, 14, 16, 18, 20, 24, 32 };
  const uint32_t *sts = l2 ? st2 : st1;
  const int *ms = l2 ? m2 : m1;
  int nm = l2 ? 13 : 14, target = l2 ? 1 : 0;
  const mu_levels_t *L = mu_levels(ctx);
  int caps[4], cens[4], amb[4];
  for (int si = 0; si < 4; si++) {
    uint32_t st = sts[si];
    int cap = -1, censored = 1, ambiguous = 0, stop = 0;
    for (int mi = 0; mi < nm; mi++) {
      int m = ms[mi], err;
      if (l2 && st == 262144 && m > 16) break;              /* 18+ pages at 256K stride would conflict in the ERAT */
      xts_tspec_t s;
      xts_tresult_t r;
      double v = chase_point(ctx, 0, (uint32_t)m, st, 0, 0, 1, 0xA55u + (uint32_t)m, 1, 0, &s, &r, &err);
      if (err) { emit_fail(ctx, "latency", err, c->id); return 0; }
      int k = mu_classify(v, L->lat, 3);
      if (!stop) {
        if (isnan(v)) { censored = 0; stop = 1; }
        else if (k < 0) { ambiguous = 1; censored = 0; stop = 1; }
        else if (k <= target) cap = m;
        else { censored = 0; stop = 1; }
      }
      mu_det_t d;
      mu_det_begin(&d);
      det_chase(&d, k == 0 ? "l1" : k == 1 ? "l2" : k == 2 ? "mem" : "between", (uint32_t)m * st, st, (uint32_t)m, "random");
      xj_kv_str(&d.j, "series", l2 ? "assoc.l2" : "assoc.l1");
      xts_time_emit_ex(ctx, &s, &r, XI_ld,
                       &(xts_temit_t){ .metric = "latency", .no_negative = 1, .extra_json = mu_det_end(&d) });
    }
    caps[si] = cap; cens[si] = censored && cap > 0; amb[si] = ambiguous;
    mu_det_t d;
    mu_det_begin(&d);
    xj_kv_str(&d.j, "what", "lines_that_fit");
    xj_kv_str(&d.j, "level", l2 ? "l2" : "l1");
    xj_kv_uint(&d.j, "stride", st);
    xj_kv_bool(&d.j, "censored", cens[si]);
    xj_kv_bool(&d.j, "ambiguous", amb[si]);
    mu_emit_note(ctx, &d, "count", -1, cap > 0 ? cap : NAN, "", ctx->host ? XTS_ST_SKIP : cap > 0 ? XTS_ST_PASS : XTS_ST_INFO,
                 NULL);
  }
  /* ways = capacity at the largest uncensored stride; set stride = smallest stride with that capacity */
  int ways = -1, set_i = -1;
  for (int si = 3; si >= 0; si--)
    if (caps[si] > 0 && !cens[si] && !amb[si]) { ways = caps[si]; break; }
  int agree = 0, doubles = 0;
  if (ways > 0) {
    for (int si = 0; si < 4; si++)
      if (caps[si] == ways && !cens[si] && !amb[si]) { if (set_i < 0) set_i = si; agree++; }
    if (set_i > 0) doubles = caps[set_i - 1] >= 2 * ways || cens[set_i - 1];
  }
  int confident = ways > 0 && (agree >= 2 || doubles);
  mu_det_t d;
  mu_det_begin(&d);
  xj_kv_str(&d.j, "what", "ways");
  xj_kv_str(&d.j, "level", l2 ? "l2" : "l1");
  if (ways > 0) {
    xj_kv_uint(&d.j, "set_stride", sts[set_i]);
    xj_kv_uint(&d.j, "size", (uint64_t)ways * sts[set_i]);
    xj_kv_int(&d.j, "strides_agreeing", agree);
    xj_kv_bool(&d.j, "doubles_below", doubles);
  }
  char vb[128];
  if (ways > 0)
    snprintf(vb, sizeof vb, "%d-way, set stride %uK (=> %uK)%s", ways, sts[set_i] / 1024, ways * sts[set_i] / 1024,
             confident ? "" : " TENTATIVE (no second stride agrees, no doubling below)");
  else
    snprintf(vb, sizeof vb, "undetermined: no stride with a clear capacity");
  xj_kv_str(&d.j, "verdict", vb);
  xj_kv_str(&d.j, "cited", l2 ? "L2 1 MB 8-way [FPF]" : "L1D 32 KB 4-way [FPF]");
  mu_emit_note(ctx, &d, "count", -1, ways > 0 ? ways : NAN, "", ctx->host ? XTS_ST_SKIP : confident ? XTS_ST_PASS : XTS_ST_INFO,
               NULL);
  return 0;
}

/* ---------------------------------------------------------------------------------------------------- streams */
typedef struct st_arg { int kind, op, cold; uint32_t bytes; mu_buf_t b; int pf; } st_arg_t;
static int stream_body(xts_kbuf_t *k, int depth, void *arg)
{
  st_arg_t *a = arg;
  for (int i = 0; i < depth; i++) {
    switch (a->kind) {
    case 0: xk_emit(k, XI_ld, 0, 4 + (i & 7), 0, 3); break;
    case 1: for (int q = 0; q < 16; q++) xk_emit(k, XI_ld, 0, 4 + (q & 7), 8 * q, 3); break;
    case 2: for (int q = 0; q < 8; q++) xk_emit(k, XI_lvx128, 0, 64 + q, 3, 14 + q); break;
    case 3: xk_emit(k, XI_std, 0, 12, 0, 3); break;
    case 4: for (int q = 0; q < 16; q++) xk_emit(k, XI_std, 0, 12, 8 * q, 3); break;
    case 5: for (int q = 0; q < 8; q++) xk_emit(k, XI_stvx128, 0, 32, 3, 14 + q); break;
    case 6: case 7:
      xk_emit(k, a->kind == 6 ? XI_dcbz : XI_dcbz128, 0, 0, 3);
      for (int q = 0; q < 8; q++) xk_emit(k, XI_stvx128, 0, 32, 3, 14 + q);
      break;
    case 8: xk_emit(k, XI_dcbz, 0, 0, 3); break;
    case 9:                                                  /* pfstream: op 8 lines ahead + one load */
      if (a->pf < 0) xk_nop(k);
      else xk_emit(k, a->pf, 0, 3, 22);
      xk_emit(k, XI_ld, 0, 4 + (i & 7), 0, 3);
      break;
    }
    xk_emit(k, XI_addi, 0, 3, 3, 128);
  }
  return 0;
}
static int stream_setup(xts_exec_t *x, void *arg)
{
  st_arg_t *a = arg;
  if (a->cold) xts_dcbf_range(a->b.p, a->bytes + 2048);
  else xts_touch_range(a->b.p, a->bytes);
  xts_set_gpr(x, 3, a->b.addr);
  for (int q = 0; q < 8; q++) xts_set_gpr(x, 14 + q, (uint64_t)(16 * q));
  xts_set_gpr(x, 12, 0);
  xts_set_gpr(x, 22, 1024);
  xts_set_vr(x, 32, xts_vr4(0, 0, 0, 0));
  return 0;
}

static int run_stream(const xts_case_t *c, xts_ctx_t *ctx, int pf)
{
  static st_arg_t a;
  memset(&a, 0, sizeof a);
  a.kind = pf ? 9 : (int)c->arg[1];
  a.pf = pf ? pf_ops[c->arg[1]] : -1;
  a.cold = pf ? 1 : !c->arg[2];
  xts_tspec_t s = XTS_TSPEC_DEFAULT;
  s.iters = a.cold ? 512 : 64;
  a.bytes = (uint32_t)s.depths[s.ndepths - 1] * s.iters * MU_LINE;
  if (mu_big(MU_BIG_BYTES, &a.b) < 0) { emit_fail(ctx, "bytes_per_cycle", -2, c->id); return 0; }
  memset(a.b.p, 0, a.bytes + 2048);
  s.unit = "one 128-byte line";
  s.body = stream_body;
  s.setup = stream_setup;
  s.arg = &a;
  xts_tresult_t r;
  double v = mu_measure(ctx, &s, &r);
  mu_det_t d;
  mu_det_begin(&d);
  xj_kv_str(&d.j, "level", a.cold ? "mem" : "l2");
  xj_kv_uint(&d.j, "size", a.bytes);
  xj_kv_uint(&d.j, "stride", MU_LINE);
  xj_kv_int(&d.j, "width", a.kind == 2 || (a.kind >= 5 && a.kind <= 7) ? 16 : a.kind == 8 ? 128 : 8);
  xj_kv_uint(&d.j, "offset", 0);
  xj_kv_str(&d.j, "kind", pf ? "ld_line_prefetch" : stream_kinds[a.kind]);
  if (pf) { xj_kv_str(&d.j, "prefetch", a.pf < 0 ? "none" : xts_isa[a.pf].mnem); xj_kv_int(&d.j, "distance_lines", 8); }
  xj_kv_double(&d.j, "cycles_per_line", v);
  xts_time_emit_ex(ctx, &s, &r, pf && a.pf >= 0 ? a.pf : -1,
                   &(xts_temit_t){ .metric = "bytes_per_cycle", .unit = "bytes_per_cycle", XTS_TE_VALUE(v > 0 ? 128.0 / v : NAN),
                                   .no_negative = 1, .extra_json = mu_det_end(&d) });
  return 0;
}

/* ------------------------------------------------------------------------------------------------- cache ops */
typedef struct cop_arg { int op, state, filler; uint32_t bytes; mu_buf_t b; } cop_arg_t;
static int cop_body(xts_kbuf_t *k, int depth, void *arg)
{
  cop_arg_t *a = arg;
  for (int i = 0; i < depth; i++) {
    int base = a->state == 2 ? 3 : 16 + (i & 7);
    if (a->state == 0) xk_emit(k, XI_ld, 0, 9, 0, base);
    if (a->state == 1) xk_emit(k, XI_stw, 0, 8, 0, base);
    if (a->filler) xk_nop(k);
    else xk_emit(k, a->op, 0, 0, base);
    if (a->state == 2) xk_emit(k, XI_addi, 0, 3, 3, 128);
  }
  return 0;
}
static int cop_setup(xts_exec_t *x, void *arg)
{
  cop_arg_t *a = arg;
  if (a->state == 2) xts_dcbf_range(a->b.p, a->bytes);
  else xts_touch_range(a->b.p, 8 * MU_LINE);
  xts_set_gpr(x, 3, a->b.addr);
  for (int q = 0; q < 8; q++) xts_set_gpr(x, 16 + q, a->b.addr + (uint64_t)(MU_LINE * q));
  xts_set_gpr(x, 8, 0x55);
  return 0;
}
static int run_cop(const xts_case_t *c, xts_ctx_t *ctx)
{
  static cop_arg_t a, f;
  memset(&a, 0, sizeof a);
  a.op = cop_ops[c->arg[1]];
  a.state = (int)c->arg[2];
  xts_tspec_t s = XTS_TSPEC_DEFAULT, sf;
  s.iters = 256;
  a.bytes = (uint32_t)s.depths[s.ndepths - 1] * s.iters * MU_LINE;
  if (mu_big(MU_BIG_BYTES, &a.b) < 0) { emit_fail(ctx, "issue_cost", -2, c->id); return 0; }
  memset(a.b.p, 0, a.state == 2 ? a.bytes : 8 * MU_LINE);
  f = a;
  f.filler = 1;
  s.unit = cop_states[a.state];
  s.body = cop_body;
  s.setup = cop_setup;
  s.arg = &a;
  sf = s;
  sf.arg = &f;
  xts_tresult_t r, rf;
  double v = mu_measure(ctx, &s, &r), vf = mu_measure(ctx, &sf, &rf);
  mu_det_t d;
  mu_det_begin(&d);
  xj_kv_str(&d.j, "level", a.state == 2 ? "mem" : "l1");
  xj_kv_uint(&d.j, "size", a.state == 2 ? a.bytes : 8 * MU_LINE);
  xj_kv_uint(&d.j, "stride", MU_LINE);
  xj_kv_int(&d.j, "width", 128);
  xj_kv_uint(&d.j, "offset", 0);
  xj_kv_str(&d.j, "state", cop_states[a.state]);
  xj_kv_str(&d.j, "kernel", a.state == 0 ? "ld r9,0(rA) ; op 0,rA (8 lines; includes any reload the op causes)"
                            : a.state == 1 ? "stw r8,0(rA) ; op 0,rA (8 lines)" : "op 0,r3 ; addi r3,r3,128 (flushed lines)");
  xj_kv_double(&d.j, "op_cycles", v);
  xj_kv_double(&d.j, "filler_cycles", vf);
  xts_time_emit_diff(ctx, &s, &r, &sf, &rf, a.op,
                     &(xts_temit_t){ .metric = "issue_cost", .no_negative = 1, .extra_json = mu_det_end(&d) });
  return 0;
}

/* -------------------------------------------------------------------------------------------- xdcbt2 placement */
#define XD_EBASE 0u                         /* eviction region: 12 x 8 KB (+4 KB for the no-evict variant) */
#define XD_XBASE MU_MB                      /* X lines: offsets 96K..128K of each 128K block (L2 sets 768..1023) */
#define XD_NX (2048u + 64u)
enum { XP_NONE = 0, XP_LOAD, XP_DCBT, XP_DCBTST, XP_XDCBT, XP_N };
static const char *const xp_name[XP_N] = { "none", "ld", "dcbt", "dcbtst", "xdcbt" };
typedef struct xd_arg { int pf, evict; mu_chain_t *c; } xd_arg_t;
static mu_chain_t xc;
static int xd_body(xts_kbuf_t *k, int depth, void *arg)
{
  xd_arg_t *a = arg;
  for (int i = 0; i < depth; i++) {
    switch (a->pf) {
    case XP_NONE: xk_nop(k); break;
    case XP_LOAD: xk_emit(k, XI_ld, 0, 5, 64, 3); break;
    case XP_DCBT: xk_emit(k, XI_dcbt, 0, 0, 3); break;
    case XP_DCBTST: xk_emit(k, XI_dcbtst, 0, 0, 3); break;
    default: xk_emit(k, XI_xdcbt, 0, 0, 3); break;
    }
    for (int q = 0; q < 64; q++) xk_emit(k, XI_mulldx, 0, 6, 6, 7);        /* ~900 cycles: the prefetch completes */
    xk_emit(k, XI_rlwinmx, 0, 8, 3, 0, 19, 24);                              /* X mod 8K, line aligned: X's L1 set */
    xk_emit(k, XI_addx, 0, 9, 4, 8);
    xk_emit(k, XI_addx, 0, 10, 9, 14);
    xk_emit(k, XI_addx, 0, 11, 9, 15);
    for (int pass = 0; pass < 2; pass++)
      for (int b = 9; b <= 11; b++)
        for (int q = 0; q < 4; q++) xk_emit(k, XI_ld, 0, 12, 8192 * q, b);   /* 12 lines of X's L1 set, twice */
    xk_emit(k, XI_ld, 0, 3, 0, 3);                                           /* timed reload: X holds the next X */
  }
  return 0;
}
static mu_buf_t xbuf;
static int xd_setup(xts_exec_t *x, void *arg)
{
  xd_arg_t *a = arg;
  mu_chain_flush(a->c);
  xts_touch_range(xbuf.p + XD_EBASE, 96u * MU_KB + 8192u);
  xts_set_gpr(x, 3, mu_chain_addr_at(a->c, 0));
  xts_set_gpr(x, 4, xbuf.addr + XD_EBASE + (a->evict ? 0 : 4096));   /* no-evict: the same loads, another L1 set */
  xts_set_gpr(x, 6, 3);
  xts_set_gpr(x, 7, 1);
  xts_set_gpr(x, 14, 32768);
  xts_set_gpr(x, 15, 65536);
  return 0;
}

/* nearest of n reference values (linear), confident if the nearest is < half as far as the second */
static int nearest(double v, const double *ref, int n, int *confident)
{
  int b0 = -1, b1 = -1;
  double d0 = 1e30, d1 = 1e30;
  for (int i = 0; i < n; i++) {
    double d = fabs(v - ref[i]);
    if (d < d0) { d1 = d0; b1 = b0; d0 = d; b0 = i; }
    else if (d < d1) { d1 = d; b1 = i; }
  }
  *confident = b1 < 0 || d0 < 0.5 * d1;
  return isnan(v) ? -1 : b0;
}

static int run_xdcbt(const xts_case_t *c, xts_ctx_t *ctx)
{
  (void)c;
  const mu_levels_t *L = mu_levels(ctx);
  if (mu_big(MU_BIG_BYTES, &xbuf) < 0) { emit_fail(ctx, "cycles", -2, c->id); return 0; }
  xc.buf = xbuf;
  xc.base = XD_XBASE + 96u * MU_KB;
  xc.n = XD_NX;
  xc.stride = MU_LINE;
  xc.jmod = 0;
  xc.bdiv = 256;                                       /* 256 lines (32 KB) per 128 KB block */
  xc.bstep = 96u * MU_KB;
  xc.anchor = 0;
  xc.ptr_rel = 0;
  xc.data_w = 0;
  xc.random = 1;
  xc.seed = 0xD7B7u;
  if (mu_chain_build(&xc) < 0) { emit_fail(ctx, "cycles", -3, c->id); return 0; }
  double U[XP_N][2];
  int st[XP_N][2];
  for (int pf = 0; pf < XP_N; pf++)
    for (int ev = 0; ev < 2; ev++) {
      static xd_arg_t a;
      a.pf = pf; a.evict = ev; a.c = &xc;
      static const int dep[6] = { 2, 4, 6, 8, 12, 16 };
      xts_tspec_t s = XTS_TSPEC_DEFAULT;
      for (int i = 0; i < 6; i++) s.depths[i] = dep[i];
      s.iters = 128;
      s.unit = "prefetch ; 64 mulld ; 24 eviction loads ; reload";
      s.body = xd_body;
      s.setup = xd_setup;
      s.arg = &a;
      xts_tresult_t r;
      U[pf][ev] = mu_measure(ctx, &s, &r);
      st[pf][ev] = r.status;
      mu_det_t d;
      mu_det_begin(&d);
      xj_kv_str(&d.j, "prefetch", xp_name[pf]);
      xj_kv_bool(&d.j, "evict", ev);
      xj_kv_uint(&d.j, "stride", MU_LINE);
      xj_kv_int(&d.j, "width", 8);
      xj_kv_uint(&d.j, "size", XD_NX * MU_LINE);
      xts_time_emit_ex(ctx, &s, &r, pf >= XP_DCBT ? (pf == XP_DCBT ? XI_dcbt : pf == XP_DCBTST ? XI_dcbtst : XI_xdcbt) : XI_ld,
                       &(xts_temit_t){ .metric = "cycles", .no_negative = 1, .extra_json = mu_det_end(&d) });
    }
  double L1 = L->lat[0], L2 = L->lat[1], M = L->lat[2];
  double gap_l2 = U[XP_LOAD][1] - U[XP_LOAD][0], gap_mem = U[XP_NONE][1] - U[XP_LOAD][1];
  double none_shift = U[XP_NONE][1] - U[XP_NONE][0];
  int evict_ok = !isnan(gap_l2) && !isnan(L2 - L1) && gap_l2 >= 0.5 * (L2 - L1) && gap_l2 <= 2.0 * (L2 - L1);
  int fresh_ok = !isnan(gap_mem) && !isnan(M - L2) && gap_mem >= 0.5 * (M - L2);
  int neutral = !isnan(none_shift) && !isnan(M - L1) && fabs(none_shift) <= 0.25 * (M - L1);
  int worst = XTS_ST_PASS;
  for (int pf = 0; pf < XP_N; pf++)
    for (int ev = 0; ev < 2; ev++)
      if (st[pf][ev] != XTS_ST_PASS) worst = st[pf][ev] == XTS_ST_SKIP || worst == XTS_ST_SKIP ? XTS_ST_SKIP : XTS_ST_NOISY;
  for (int pf = XP_DCBT; pf < XP_N; pf++) {
    double ref3[3] = { U[XP_LOAD][0], U[XP_LOAD][1], U[XP_NONE][1] };
    double ref2[2] = { U[XP_LOAD][0], U[XP_NONE][0] };
    int c1, c2;
    int after = nearest(U[pf][1], ref3, 3, &c1);
    int deliv = nearest(U[pf][0], ref2, 2, &c2);
    static const char *const lvn[3] = { "l1", "l2", "mem" };
    char verdict[160], why[200] = "";
    int status = worst;
    if (!evict_ok) snprintf(why, sizeof why, "control: ld+evict - ld (%.1f) is not one L2 access (L2-L1 %.1f): eviction did not work", gap_l2, L2 - L1);
    else if (!fresh_ok) snprintf(why, sizeof why, "control: none+evict - ld+evict (%.1f) is not a memory access (mem-L2 %.1f): X lines not fresh", gap_mem, M - L2);
    else if (!neutral) snprintf(why, sizeof why, "control: eviction loads change a memory reload by %.1f", none_shift);
    else if (after < 0 || !c1) snprintf(why, sizeof why, "reload after eviction (%.1f) lies between levels", U[pf][1]);
    if (why[0]) {
      snprintf(verdict, sizeof verdict, "undetermined");
      if (status == XTS_ST_PASS) status = XTS_ST_INFO;
    } else if (after == 1) {
      snprintf(verdict, sizeof verdict, "placed in L2 (and L1): reload after L1 eviction at L2 level");
    } else if (after == 2) {
      snprintf(verdict, sizeof verdict, deliv == 0 && c2 ? "bypasses L2: delivered to L1, reload after L1 eviction from memory"
                                                          : "not in L1 or L2: prefetch ineffective or dropped");
    } else {
      snprintf(verdict, sizeof verdict, "still in L1 after the eviction set (unexpected)");
      if (status == XTS_ST_PASS) status = XTS_ST_INFO;
    }
    mu_det_t d;
    mu_det_begin(&d);
    xj_kv_str(&d.j, "verdict", verdict);
    if (why[0]) xj_kv_str(&d.j, "why", why);
    xj_kv_str(&d.j, "level", after >= 0 ? lvn[after] : "between");
    xj_kv_str(&d.j, "delivered_to", deliv == 0 ? "l1" : deliv == 1 ? "not_l1" : "?");
    xj_kv_double(&d.j, "cycles_evict", U[pf][1]);
    xj_kv_double(&d.j, "cycles_noevict", U[pf][0]);
    xj_kv_double(&d.j, "ctl_ld_noevict", U[XP_LOAD][0]);
    xj_kv_double(&d.j, "ctl_ld_evict", U[XP_LOAD][1]);
    xj_kv_double(&d.j, "ctl_none_evict", U[XP_NONE][1]);
    xj_kv_double(&d.j, "ctl_none_noevict", U[XP_NONE][0]);
    xj_kv_bool(&d.j, "eviction_ok", evict_ok);
    xj_kv_bool(&d.j, "fresh_ok", fresh_ok);
    xj_kv_bool(&d.j, "eviction_neutral", neutral);
    xj_kv_double(&d.j, "l1", L1);
    xj_kv_double(&d.j, "l2", L2);
    xj_kv_double(&d.j, "mem", M);
    if (pf == XP_XDCBT) xj_kv_str(&d.j, "cited", "xdcbt delivers to L1 and never to L2 [FPF][AB06]");
    double est = L1 + (U[pf][1] - U[XP_LOAD][0]);
    mu_emit_note(ctx, &d, "latency", pf == XP_DCBT ? XI_dcbt : pf == XP_DCBTST ? XI_dcbtst : XI_xdcbt, est, "cycles",
                 ctx->host ? XTS_ST_SKIP : status, NULL);
  }
  return 0;
}

/* --------------------------------------------------------------------------------------------------- TLB/ERAT */
static int tlb_measure(xts_ctx_t *ctx, int i, double *pen, double *vp, double *vc, xts_tspec_t *s, xts_tresult_t *rp,
                       xts_tresult_t *rc, int *err)
{
  uint32_t n = tlb_pages[i];
  *vc = chase_point(ctx, 0, n, MU_LINE, 0, 0, 1, 0x7E1u + n, 1, 0, s, rc, err);
  if (*err) return -1;
  *vp = chase_point(ctx, 0, n, MU_PAGE, 512, MU_LINE, 1, 0x7E1u + n, 1, 0, s, rp, err);
  if (*err) return -1;
  *pen = *vp - *vc;
  return 0;
}

static int run_tlb(const xts_case_t *c, xts_ctx_t *ctx)
{
  int i = (int)c->arg[1], err;
  xts_tspec_t s;
  xts_tresult_t rp, rc;
  double pen, vp, vc;
  if (tlb_measure(ctx, i, &pen, &vp, &vc, &s, &rp, &rc, &err) < 0) { emit_fail(ctx, "penalty", err, c->id); return 0; }
  mu_det_t d;
  mu_det_begin(&d);
  uint32_t n = tlb_pages[i];
  xj_kv_str(&d.j, "level", n <= 192 ? "l1" : "l2");
  xj_kv_uint(&d.j, "size", (uint64_t)n * MU_PAGE);
  xj_kv_uint(&d.j, "stride", MU_PAGE + MU_LINE);
  xj_kv_int(&d.j, "width", 8);
  xj_kv_uint(&d.j, "offset", 0);
  xj_kv_uint(&d.j, "pages", n);
  xj_kv_double(&d.j, "spread_cycles", vp);
  xj_kv_double(&d.j, "packed_cycles", vc);
  xj_kv_str(&d.j, "note", "one line per 64 KB page vs the same number of lines packed; same L1/L2 set spread");
  (void)pen;
  xts_time_emit_diff(ctx, &s, &rp, &s, &rc, XI_ld,
                     &(xts_temit_t){ .metric = "penalty", .no_negative = 1, .extra_json = mu_det_end(&d) });
  return 0;
}

/* ------------------------------------------------------------------------------- chase.levels / tlb.knees (finish) */
/* The summaries are recomputed from the family's own records (xts_results_foreach: every boot, so they also work after a
 * resume), not from in-memory state of run(), with the framework's line reader (xts/rline.h). */
#define FIN_GROUPS 8
typedef struct fin_group { char oc[24]; double y[NRAND]; } fin_group_t;
typedef struct fin_data {
  fin_group_t g[FIN_GROUPS];
  int ng;
  double tlb[NTLB];
} fin_data_t;
static fin_data_t fd;

static fin_group_t *fin_group(const char *oc)
{
  for (int i = 0; i < fd.ng; i++)
    if (!strcmp(fd.g[i].oc, oc)) return &fd.g[i];
  if (fd.ng >= FIN_GROUPS) return NULL;
  fin_group_t *g = &fd.g[fd.ng++];
  snprintf(g->oc, sizeof g->oc, "%s", oc);
  for (int i = 0; i < NRAND; i++) g->y[i] = NAN;
  return g;
}

static int fin_line(const char *line, void *ud)
{
  (void)ud;
  xts_rline_t l;
  unsigned n = 0;
  if (xts_rline_parse(line, &l) < 0 || isnan(l.value)) return 0;
  if (!strcmp(l.metric, "latency") && sscanf(l.case_id, "chase.rand.%uk", &n) == 1) {
    if (l.status != XTS_ST_PASS && l.status != XTS_ST_NOISY) return 0;
    char oc[24];
    if (!xts_rline_cond(&l, "other_cores", oc, sizeof oc)) oc[0] = 0;
    fin_group_t *g = fin_group(oc);
    for (int i = 0; g && i < NRAND; i++)
      if (rand_kb[i] == n) g->y[i] = l.value;
  } else if (!strcmp(l.metric, "penalty") && sscanf(l.case_id, "tlb.%u", &n) == 1) {
    if (l.status != XTS_ST_PASS && l.status != XTS_ST_NOISY && l.status != XTS_ST_REGRESSED) return 0;
    for (int i = 0; i < NTLB; i++)
      if (tlb_pages[i] == n) fd.tlb[i] = l.value;
  }
  return 0;
}

static void det_fin(mu_det_t *d, xts_ctx_t *ctx, const fin_group_t *g, int missing, int measured)
{
  const xts_finish_info_t *fi = ctx->fin;
  if (g && fd.ng > 1) xj_kv_str(&d->j, "other_cores", g->oc);
  xj_kv_int(&d->j, "missing_points", missing);
  xj_kv_int(&d->j, "remeasured", measured);
  if (fi) {
    xj_kv_int(&d->j, "blocks_quarantined", fi->quarantined);
    xj_kv_int(&d->j, "blocks_skipped", fi->skipped);
  }
}

static int mh_finish(xts_ctx_t *ctx)
{
  memset(&fd, 0, sizeof fd);
  for (int i = 0; i < NTLB; i++) fd.tlb[i] = NAN;
  xts_results_foreach("mem.hier", fin_line, NULL);
  /* points no record has are measured now (default conditions) unless a block of this row hung/crashed (it would again) */
  const xts_axis_t *oc_axis = xts_axis_find("other_cores");
  const char *dflt = oc_axis && oc_axis->values ? oc_axis->values[0] : "";
  int remeasure = !ctx->host && !(ctx->fin && ctx->fin->quarantined);
  if (fd.ng == 0) fin_group(dflt);
  /* missing chase points are measured at the default conditions: only meaningful for the default other_cores group */
  int rand_ok = remeasure && fd.ng == 1 && !strcmp(fd.g[0].oc, dflt);
  int measured = 0, tlb_measured = 0;
  if (remeasure) {
    mu_levels(ctx);
    for (int i = 0; rand_ok && i < NRAND; i++)
      if (isnan(fd.g[0].y[i])) {
        xts_tspec_t s;
        xts_tresult_t r;
        int err;
        rand_measure(ctx, i, &s, &r, &err);
        if (!err && rand_have[oc_index()][i]) fd.g[0].y[i] = rand_pts[oc_index()][i];
        measured++;
      }
    for (int i = 0; i < NTLB; i++)
      if (isnan(fd.tlb[i])) {
        xts_tspec_t s;
        xts_tresult_t rp, rc;
        double pen, vp, vc;
        int err;
        if (tlb_measure(ctx, i, &pen, &vp, &vc, &s, &rp, &rc, &err) == 0 && rp.ok && rc.ok && rp.status != XTS_ST_TRAP &&
            rc.status != XTS_ST_TRAP)
          fd.tlb[i] = pen;
        tlb_measured++;
      }
  }

  /* ---- chase.levels ---- */
  for (int gi = 0; gi < fd.ng; gi++) {
    const fin_group_t *g = &fd.g[gi];
    double x[NRAND];
    int missing = 0;
    for (int i = 0; i < NRAND; i++) {
      x[i] = rand_kb[i] * (double)MU_KB;
      missing += isnan(g->y[i]);
    }
    mu_plateau_t p[8];
    int np = mu_plateaus(x, g->y, NRAND, p, 8);
    static const char *const nm[3] = { "l1", "l2", "mem" };
    int named = np == 3 && p[0].level < p[1].level && p[1].level < p[2].level;
    for (int k = 0; k < np; k++) {
      mu_det_t d;
      mu_det_begin(&d);
      xj_kv_str(&d.j, "level", named ? nm[k] : "unnamed");
      xj_kv_uint(&d.j, "size", (uint64_t)x[p[k].hi]);
      xj_kv_uint(&d.j, "from_size", (uint64_t)x[p[k].lo]);
      xj_kv_uint(&d.j, "stride", MU_LINE);
      xj_kv_int(&d.j, "width", 8);
      xj_kv_uint(&d.j, "offset", 0);
      xj_kv_int(&d.j, "points", p[k].hi - p[k].lo + 1);
      det_fin(&d, ctx, g, missing, measured);
      if (!named) xj_kv_str(&d.j, "verdict", "AMBIGUOUS: expected three plateaus (L1, L2, memory)");
      mu_emit_note(ctx, &d, "latency", XI_ld, p[k].level, "cycles", named ? XTS_ST_PASS : XTS_ST_INFO, "chase.levels");
      if (k + 1 < np && p[k + 1].level / p[k].level >= MU_LEVEL_FACTOR) {   /* a knee: capacity of level k */
        mu_det_t e;
        mu_det_begin(&e);
        xj_kv_str(&e.j, "what", "capacity");
        xj_kv_str(&e.j, "level", named ? nm[k] : "unnamed");
        xj_kv_double(&e.j, "first_above", x[p[k].hi + 1 < NRAND ? p[k].hi + 1 : p[k].hi]);
        xj_kv_double(&e.j, "from_cycles", p[k].level);
        xj_kv_double(&e.j, "to_cycles", p[k + 1].level);
        xj_kv_str(&e.j, "cited", k == 0 ? "L1D 32 KB [FPF]" : "L2 1 MB [FPF]");
        det_fin(&e, ctx, g, missing, measured);
        /* metric "count" (registered), unit "bytes": the capacity in bytes is a count of bytes */
        mu_emit_note(ctx, &e, "count", -1, x[p[k].hi], "bytes", named ? XTS_ST_PASS : XTS_ST_INFO, "chase.levels");
      }
    }
    if (np == 0) {
      mu_det_t d;
      mu_det_begin(&d);
      xj_kv_str(&d.j, "verdict", ctx->host ? "host dry run: no points" : "undetermined: no plateau in the chase points");
      det_fin(&d, ctx, g, missing, measured);
      mu_emit_note(ctx, &d, "latency", XI_ld, NAN, "cycles", ctx->host ? XTS_ST_SKIP : XTS_ST_INFO, "chase.levels");
    }
  }

  /* ---- tlb.knees: reach = the largest page count before the penalty first exceeds a threshold (ERAT 3, TLB 40 cycles) ---- */
  static const double thr[2] = { 3.0, 40.0 };
  static const char *const what[2] = { "erat_reach_pages", "tlb_reach_pages" };
  int tmissing = 0;
  for (int i = 0; i < NTLB; i++) tmissing += isnan(fd.tlb[i]);
  for (int t = 0; t < 2; t++) {
    int reach = -1, above = -1;
    for (int i = 0; i < NTLB; i++) {
      if (isnan(fd.tlb[i])) continue;
      if (fd.tlb[i] > thr[t]) { above = (int)tlb_pages[i]; break; }
      reach = (int)tlb_pages[i];
    }
    mu_det_t d;
    mu_det_begin(&d);
    xj_kv_str(&d.j, "what", what[t]);
    xj_kv_double(&d.j, "threshold_cycles", thr[t]);
    xj_kv_int(&d.j, "first_above", above);
    det_fin(&d, ctx, NULL, tmissing, tlb_measured);
    xj_kv_str(&d.j, "cited", t ? "TLB 1K entries, 4-way [FPF]: 1024 pages" : "ERAT 64 entries, 2-way [FPF]: 64 pages");
    int st = ctx->host ? XTS_ST_SKIP : reach > 0 && above > 0 ? XTS_ST_PASS : XTS_ST_INFO;
    if (st == XTS_ST_INFO) xj_kv_str(&d.j, "verdict", above < 0 ? "undetermined: no knee up to the largest page count"
                                                                : "undetermined: penalty already above at the smallest count");
    mu_emit_note(ctx, &d, "count", -1, reach > 0 && above > 0 ? reach : NAN, "", st, "tlb.knees");
  }
  return 0;
}

/* ------------------------------------------------------------------------------------------------- enumeration */
static xts_case_t *addc(xts_caselist_t *cl, int kind, int64_t a1, int64_t a2, const char *id)
{
  xts_case_t *c = xts_case_add(cl, id);
  if (c) { c->arg[0] = kind; c->arg[1] = a1; c->arg[2] = a2; }
  return c;
}

static int mh_enum(xts_caselist_t *cl)
{
  char id[48];
  for (int i = 0; i < NRAND; i++) {
    snprintf(id, sizeof id, "chase.rand.%uk", rand_kb[i]);
    if (!addc(cl, H_RAND, i, 0, id)) return -1;
  }
  for (unsigned i = 0; i < sizeof seq_kb / sizeof seq_kb[0]; i++) {
    snprintf(id, sizeof id, "chase.seq.%uk", seq_kb[i]);
    if (!addc(cl, H_SEQ, i, 0, id)) return -1;
  }
  for (unsigned i = 0; i < sizeof strides / sizeof strides[0]; i++) {
    snprintf(id, sizeof id, "stride.%u", strides[i]);
    if (!addc(cl, H_STRIDE, i, 0, id)) return -1;
  }
  if (!addc(cl, H_ASSOC, 0, 0, "assoc.l1") || !addc(cl, H_ASSOC, 1, 0, "assoc.l2")) return -1;
  for (int k = 0; k < NSTREAM; k++)
    for (int l2 = 0; l2 < 2; l2++) {
      snprintf(id, sizeof id, "stream.%s.%s", stream_kinds[k], l2 ? "l2" : "cold");
      if (!addc(cl, H_STREAM, k, l2, id)) return -1;
    }
  for (int p = 0; p < 4; p++) {
    snprintf(id, sizeof id, "pfstream.%s", p ? xts_isa[pf_ops[p]].mnem : "none");
    if (!addc(cl, H_PFSTREAM, p, 0, id)) return -1;
  }
  for (unsigned o = 0; o < sizeof cop_ops / sizeof cop_ops[0]; o++)
    for (int s = 0; s < 3; s++) {
      snprintf(id, sizeof id, "cop.%s.%s", xts_isa[cop_ops[o]].mnem, cop_states[s]);
      xts_case_t *c = addc(cl, H_COP, o, s, id);
      if (!c) return -1;
      c->insn = cop_ops[o];
    }
  if (!addc(cl, H_XDCBT, 0, 0, "prefetch.xdcbt2")) return -1;
  for (int i = 0; i < NTLB; i++) {                         /* last: the riskiest (software TLB reloads), own blocks */
    snprintf(id, sizeof id, "tlb.%u", tlb_pages[i]);
    if (!addc(cl, H_TLB, i, 0, id)) return -1;
  }
  return 0;
}

static int mh_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  int kind = (int)c->arg[0];
  if (xts_cond_applied(ctx, "other_cores") > 0 && !(kind == H_RAND || kind == H_SEQ || kind == H_STREAM))
    return 0;                                              /* not sensitive to the other cores: measured once */
  switch (kind) {
  case H_RAND: return run_rand(c, ctx);
  case H_SEQ: return run_seq(c, ctx);
  case H_STRIDE: return run_stride(c, ctx);
  case H_ASSOC: return run_assoc(c, ctx);
  case H_STREAM: return run_stream(c, ctx, 0);
  case H_PFSTREAM: return run_stream(c, ctx, 1);
  case H_COP: return run_cop(c, ctx);
  case H_XDCBT: return run_xdcbt(c, ctx);
  case H_TLB: return run_tlb(c, ctx);
  default: return -1;
  }
}

XTS_FAMILY(mem_hier, .kind = "mem", .axes = "other_cores", .enumerate = mh_enum, .run = mh_run, .finish = mh_finish, .block_cases = 4,
           .desc = "pointer-chase latency 4K-64M with levels/knees, strides, L1/L2 associativity by conflict sets, "
                   "streams, prefetch (xdcbt2 method), cache-op costs, ERAT/TLB translation cost");
