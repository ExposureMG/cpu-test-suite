/* calib - what the timing numbers of every other family are measured against. Runs first in the profiles.
 *
 *   tb_rtc       timebase frequency against the SMC real-time clock (query 0x04): edge-to-edge over XTS_CALIB_RTC_MS
 *                (compile-time, default 2000 ms). The RTC counter's own rate is not assumed: it is estimated from the
 *                nominal timebase frequency and snapped to the nearest of 1, 10, 100, 1000, 1024, 32768 counts/s
 *                (within 5 %), then the timebase frequency follows from the counts. Record: tb_hz (count, hz), ratio to the
 *                nominal xts_tb_hz(). Skipped on the host or when the SMC does not answer.
 *   mftb_cost    mftb throughput (timing engine) and the distance between two back-to-back mftb reads in ticks
 *                (ratio of pairs that straddle a tick boundary -> cycles = fraction * cycles_per_tick: this half uses
 *                the current cycles-per-tick, say so in the detail).
 *   noise_floor  one framed empty loop run many times: record value = (max - min) of the raw tick counts, in cycles per loop
 *                iteration (the timer noise floor); min/median/mad are null (they would describe the run time, not this value;
 *                the raw tick statistics and the quantisation are in the detail).
 *   cpt          cycles per timebase tick from a dependent add chain (2 cycles/add) and independent adds (1/cycle): both
 *                give a number with the SAME two architectural assumptions, which are named in the detail. The tick count
 *                itself (slope in ticks per add) does not depend on xts_cycles_per_tick, so this is not circular in the
 *                scale; it IS circular in the assumed add latency and issue rate, and their ratio (dependent / independent
 *                slope, scale free) is reported as a check. xts_cycles_per_tick is set only when the two estimates agree
 *                (8 %), the ratio is 2 +- 7.5 % and the result differs from the current value by more than 3 %; a value
 *                within 3 % of an integer is snapped to it (the hardware ratio is an integer divider).
 *
 * Axis speed: the family is sensitive to the CPU speed code (a slower core changes cycles per tick); each speed value
 * re-measures and re-sets xts_cycles_per_tick. NOTE: the value is process state; after a watchdog reboot / resume it is
 * back at the default until calib runs again (it is first in the profiles, so a resumed run past it keeps 64).
 *
 * Exported for the host unit tests: tsys_rtc_snap_rate, tsys_cpt_decide. */
#include "tsys_priv.h"

#pragma GCC diagnostic ignored "-Wunused-function"

#ifndef XTS_CALIB_RTC_MS
#define XTS_CALIB_RTC_MS 2000
#endif
#ifndef XTS_CALIB_PAIRS
#define XTS_CALIB_PAIRS 2000          /* back-to-back mftb pair executions */
#endif
#ifndef XTS_CALIB_NOISE_RUNS
#define XTS_CALIB_NOISE_RUNS 48
#endif
#define R_ITERS_REG 29                /* the timing frame's registers (CONTRIBUTING: r27-r29) */
#define R_T0_REG 27
#define R_T1_REG 28

#define TM(p, c, v) tm_make((p), (c), (v), 1, -1)
#define W_MFTB 0x7C6C42E6u              /* mftb r3,268 (what the timebase reads in this file use) */

/* ---- pure maths (host-tested) ------------------------------------------------------------------------------------------ */
/* RTC counts per second: nearest of the usual rates within 5 %, else 0 */
double tsys_rtc_snap_rate(double rate, double *err_rel)
{
  static const double cand[] = { 1, 10, 100, 1000, 1024, 32768 };
  double best = 0, be = 1e9;
  for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++) {
    double e = fabs(rate - cand[i]) / cand[i];
    if (e < be) { be = e; best = cand[i]; }
  }
  if (err_rel) *err_rel = be;
  return be <= 0.05 ? best : 0;
}

static double snap_cpt(double est)
{
  double n = floor(est + 0.5);
  return n >= 1 && fabs(est - n) / n < 0.03 ? n : est;
}

/* slopes: timebase ticks per instruction of a dependent add chain (2 cycles each) and of independent adds (1 cycle each) */
void tsys_cpt_decide(double slope_dep, double slope_ind, double current, tsys_cptd_t *o)
{
  memset(o, 0, sizeof *o);
  o->new_cpt = current;
  if (!(slope_dep > 0) || !(slope_ind > 0)) { snprintf(o->why, sizeof o->why, "no usable slope"); return; }
  o->est_dep = 2.0 / slope_dep;
  o->est_ind = 1.0 / slope_ind;
  o->est = 0.5 * (o->est_dep + o->est_ind);
  o->ratio = slope_dep / slope_ind;
  if (o->ratio < 1.85 || o->ratio > 2.15) { snprintf(o->why, sizeof o->why, "dependent/independent slope ratio %.3f not 2 +- 7.5%%", o->ratio); return; }
  if (fabs(o->est_dep - o->est_ind) > 0.08 * o->est) { snprintf(o->why, sizeof o->why, "estimates %.2f and %.2f disagree by more than 8%%", o->est_dep, o->est_ind); return; }
  if (o->est < 8 || o->est > 512) { snprintf(o->why, sizeof o->why, "estimate %.2f outside 8..512", o->est); return; }
  o->consistent = 1;
  double s = snap_cpt(o->est);
  if (fabs(s - current) / current > 0.03) { o->set = 1; o->new_cpt = s; snprintf(o->why, sizeof o->why, "changed from %.4g", current); }
  else snprintf(o->why, sizeof o->why, "confirms %.4g", current);
}

/* ---- measurement helpers ---------------------------------------------------------------------------------------------- */
static int add_dep_body(xts_kbuf_t *k, int depth, void *arg)
{
  for (int i = 0; i < depth; i++) xk_emit(k, XI_addx, 0, 3, 3, 4);
  return 0;
}
static int add_ind_body(xts_kbuf_t *k, int depth, void *arg)
{
  for (int i = 0; i < depth; i++) xk_emit(k, XI_addx, 0, 5 + (i & 7), 3, 4);
  return 0;
}
static int mftb_body(xts_kbuf_t *k, int depth, void *arg)
{
  for (int i = 0; i < depth; i++) xk_emit(k, XI_mftb, 0, 5 + (i & 3), 268);
  return 0;
}
static int nop_body(xts_kbuf_t *k, int depth, void *arg)
{
  for (int i = 0; i < depth; i++) xk_nop(k);
  return 0;
}
static int add_setup(xts_exec_t *x, void *arg)
{
  xts_set_gpr(x, 3, 1);
  xts_set_gpr(x, 4, 1);
  return 0;
}

static void spec_long(meas_t *m, const char *unit, int (*body)(xts_kbuf_t *, int, void *), uint32_t iters)
{
  static const int dl[6] = { 32, 64, 96, 128, 192, 256 };
  m->s = (xts_tspec_t)XTS_TSPEC_DEFAULT;
  mset_unit(m, unit);
  m->s.body = body;
  m->s.setup = add_setup;
  m->s.iters = iters;
  m->s.ndepths = 6;
  for (int i = 0; i < 6; i++) m->s.depths[i] = dl[i];
}

static double g_tb_hz_meas;          /* from tb_rtc, this boot (0 = not measured) */

static int rtc_read(uint64_t *v)
{
  uint8_t raw[5];
  if (xts_smc_rtc(raw) < 0) return -1;
  uint64_t x = 0;
  for (int i = 4; i >= 0; i--) x = (x << 8) | raw[i];
  *v = x;
  return 0;
}

typedef struct edge { uint64_t v, tb_prev, tb_det; uint32_t polls; uint64_t max_poll; } edge_t;

/* poll until the RTC value changes; tb_prev is the timebase of the last poll that still saw the old value */
static int rtc_edge(uint64_t timeout_ticks, edge_t *e)
{
  uint64_t v0, v, t_start = xts_tb(), prev = t_start;
  if (rtc_read(&v0) < 0) return -1;
  e->polls = 0;
  e->max_poll = 0;
  for (;;) {
    uint64_t t = xts_tb();
    if (rtc_read(&v) < 0) return -1;
    uint64_t t2 = xts_tb();
    e->polls++;
    if (t2 - t > e->max_poll) e->max_poll = t2 - t;
    if (v != v0) { e->v = v; e->tb_prev = prev; e->tb_det = t2; return 0; }
    prev = t2;
    xts_watchdog_kick();
    if (t2 - t_start > timeout_ticks) return -2;
  }
}

/* A record with a family-computed value and no fit. `extra` is a JSON object whose members become the detail (NULL: none). */
static void vrec(xts_ctx_t *ctx, const char *sub, const char *metric, const char *unit, int insn, uint32_t word, double value,
                 int status, const char *extra)
{
  xts_time_emit_value(ctx, NULL, NULL, insn, value,
                      &(xts_temit_t){ .metric = metric, .unit = unit, .word = word, .case_suffix = sub, .status = status,
                                      .extra_json = extra });
}

static void skip_rec(xts_ctx_t *ctx, const char *sub, const char *metric, const char *unit, const char *why)   /* names mftb */
{
  char buf[200];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_kv_str(&j, "why", why);
  xj_finish(&j);
  vrec(ctx, sub, metric, unit, XI_mftb, W_MFTB, NAN, XTS_ST_SKIP, j.buf);
}

/* ---- tb_rtc ----------------------------------------------------------------------------------------------------------- */
static int h_tb_rtc(xts_ctx_t *ctx)
{
  uint64_t nom = xts_tb_hz();
  if (ctx->host || xts_exec_is_host()) { skip_rec(ctx, "tb_hz", "count", "hz", "host dry run"); return 0; }
  edge_t a, b;
  int r = rtc_edge(3 * nom, &a);
  if (r < 0) { skip_rec(ctx, "tb_hz", "count", "hz", r == -1 ? "SMC RTC query failed" : "SMC RTC never changed within 3 s"); return 0; }
  uint64_t want = a.tb_det + (uint64_t)XTS_CALIB_RTC_MS * nom / 1000;
  while (xts_tb() < want) xts_watchdog_kick();
  r = rtc_edge(3 * nom, &b);
  if (r < 0) { skip_rec(ctx, "tb_hz", "count", "hz", "second RTC edge not seen"); return 0; }
  /* edge times: the change happened between the last old-value poll and the detecting poll; use the midpoints */
  double ta = 0.5 * ((double)a.tb_prev + (double)a.tb_det), tb2 = 0.5 * ((double)b.tb_prev + (double)b.tb_det);
  double dticks = tb2 - ta, dcounts = (double)(b.v - a.v);
  double unc = 0.5 * ((double)(a.tb_det - a.tb_prev) + (double)(b.tb_det - b.tb_prev));
  double rate_est = dcounts / (dticks / (double)nom), err, rate = tsys_rtc_snap_rate(rate_est, &err);
  char buf[1400];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_kv_str(&j, "producer", "smc_rtc");
  xj_kv_str(&j, "consumer", "mftb");
  xj_kv_str(&j, "variant", "tb_hz_vs_rtc");
  xj_kv_uint(&j, "interval_ms_requested", XTS_CALIB_RTC_MS);
  xj_kv_double(&j, "tb_ticks", dticks);
  xj_kv_double(&j, "rtc_counts", dcounts);
  xj_kv_double(&j, "rtc_rate_estimated_per_s", rate_est);
  xj_kv_double(&j, "rtc_rate_assumed_per_s", rate);
  xj_kv_double(&j, "edge_uncertainty_ticks", unc);
  xj_kv_double(&j, "rel_uncertainty", dticks > 0 ? unc / dticks : NAN);
  xj_kv_uint(&j, "polls", (uint64_t)a.polls + b.polls);
  xj_kv_uint(&j, "max_poll_ticks", a.max_poll > b.max_poll ? a.max_poll : b.max_poll);
  xj_kv_uint(&j, "nominal_hz", nom);
  xj_kv_str(&j, "note", "RTC rate snapped to a known rate (nominal timebase used only to pick it); not circular in the result");
  xj_finish(&j);
  if (rate <= 0 || dcounts <= 0) {
    vrec(ctx, "tb_hz", "count", "hz", XI_mftb, W_MFTB, NAN, XTS_ST_NOISY, j.buf);
    return 0;
  }
  double hz = dticks / (dcounts / rate);
  g_tb_hz_meas = hz;
  vrec(ctx, "tb_hz", "count", "hz", XI_mftb, W_MFTB, hz, XTS_ST_PASS, j.buf);
  double ratio = hz / (double)nom;
  vrec(ctx, "tb_ratio", "ratio", "ratio", XI_mftb, W_MFTB, ratio, fabs(ratio - 1) < 0.01 ? XTS_ST_PASS : XTS_ST_INFO, j.buf);   /* measured / nominal */
  return 0;
}

/* ---- mftb_cost -------------------------------------------------------------------------------------------------------- */
static meas_t M[6];

static int h_mftb_cost(xts_ctx_t *ctx)
{
  meas_t *m = &M[0];
  m->s = (xts_tspec_t)XTS_TSPEC_DEFAULT;
  mset_unit(m, "mftb rD,268");
  m->s.body = mftb_body;
  m->s.iters = pick_iters(44);
  mrun(m, ctx);
  tmeta_t t = TM("mftb", NULL, "back_to_back_throughput");
  t.baseline = 1;
  emit_meas(ctx, "tput", "issue_cost", XI_mftb, xts_enc(XI_mftb, 0, 5, 268), m, &t);
  if (ctx->host || xts_exec_is_host()) {
    skip_rec(ctx, "pair_fraction", "ratio", "ratio", "host dry run");
    return 0;
  }
  static xts_kbuf_t *k;
  static xts_exec_t x;
  if (!k && !(k = xk_new(64))) return -1;
  xk_reset(k);
  xk_emit(k, XI_mftb, 0, 3, 268);
  xk_emit(k, XI_mftb, 0, 4, 268);
  xk_exit(k);
  if (xk_finish(k) < 0) return -1;
  int n = 0, straddle = 0, big = 0, bad = 0;
  for (int i = 0; i < XTS_CALIB_PAIRS; i++) {
    xts_case_init(&x);
    if (xts_exec_kernel(&x, k) != XTS_EXEC_OK || x.trap.trapped) { bad++; continue; }
    uint32_t d = (uint32_t)(x.out.gpr[4] - x.out.gpr[3]);
    n++;
    if (d >= 1) straddle++;
    if (d >= 2) big++;
    xts_watchdog_kick();
  }
  char buf[900];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_kv_str(&j, "producer", "mftb");
  xj_kv_str(&j, "consumer", "mftb");
  xj_kv_str(&j, "variant", "back_to_back_pairs");
  xj_kv_int(&j, "pairs", n);
  xj_kv_int(&j, "pairs_with_tick_boundary", straddle);
  xj_kv_int(&j, "pairs_delta_2_or_more", big);
  xj_kv_int(&j, "bad_runs", bad);
  xj_kv_double(&j, "cycles_per_tick_used", xts_cycles_per_tick);
  xj_kv_str(&j, "note", "cycles = fraction * cycles_per_tick uses the current cycles-per-tick (valid while the read distance is below one tick)");
  xj_finish(&j);
  double frac = n ? (double)straddle / n : NAN;
  vrec(ctx, "pair_fraction", "ratio", "ratio", XI_mftb, xts_enc(XI_mftb, 0, 3, 268), frac,
       n < XTS_CALIB_PAIRS / 2 ? XTS_ST_NOISY : XTS_ST_PASS, j.buf);
  vrec(ctx, "pair_cycles", "cycles", "cycles", XI_mftb, xts_enc(XI_mftb, 0, 3, 268), big ? NAN : frac * xts_cycles_per_tick,
       n < XTS_CALIB_PAIRS / 2 || big ? XTS_ST_NOISY : XTS_ST_PASS, j.buf);
  return 0;
}

/* ---- noise_floor ------------------------------------------------------------------------------------------------------ */
static int h_noise(xts_ctx_t *ctx)
{
  if (ctx->host || xts_exec_is_host()) {
    meas_t *m = &M[0];
    m->s = (xts_tspec_t)XTS_TSPEC_DEFAULT;
    mset_unit(m, "nop");
    m->s.body = nop_body;
    mrun(m, ctx);                                   /* builds + validates the kernel */
    skip_rec(ctx, "spread", "cycles", "cycles", "host dry run");
    return 0;
  }
  static xts_kbuf_t *k;
  static xts_exec_t x;
  if (!k) k = xk_new(xts_exec_kernel_capacity());
  if (!k) return -1;
  xts_tspec_t s = XTS_TSPEC_DEFAULT;
  s.body = nop_body;
  s.iters = 1024;
  if (xts_time_build(&s, 16, k) < 0) return -1;
  double v[XTS_CALIB_NOISE_RUNS];
  int n = 0;
  for (int i = 0; i < XTS_CALIB_NOISE_RUNS; i++) {
    xts_case_init(&x);
    x.in.gpr[R_ITERS_REG] = s.iters;
    if (xts_exec_kernel(&x, k) != XTS_EXEC_OK || x.trap.trapped) continue;
    v[n++] = (double)(uint32_t)(x.out.gpr[R_T1_REG] - x.out.gpr[R_T0_REG]);
    xts_watchdog_kick();
  }
  if (n < 8) { skip_rec(ctx, "spread", "cycles", "cycles", "kernel did not run"); return 0; }
  double med = xts_median(v, n), mad = xts_mad(v, n), mn = xts_min(v, n), mx = mn;
  int distinct = 0;
  for (int i = 0; i < n; i++) {
    if (v[i] > mx) mx = v[i];
    int seen = 0;
    for (int q = 0; q < i; q++) seen |= v[q] == v[i];
    if (!seen) distinct++;
  }
  char buf[1100];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_kv_str(&j, "producer", "mftb");
  xj_kv_str(&j, "consumer", "mftb");
  xj_kv_str(&j, "variant", "empty_loop_spread");
  xj_kv_int(&j, "runs", n);
  xj_kv_uint(&j, "iters", s.iters);
  xj_kv_double(&j, "ticks_median", med);
  xj_kv_double(&j, "ticks_mad", mad);
  xj_kv_double(&j, "ticks_min", mn);
  xj_kv_double(&j, "ticks_max", mx);
  xj_kv_double(&j, "spread_ticks", mx - mn);
  xj_kv_double(&j, "median_cycles_per_iteration", med * xts_cycles_per_tick / s.iters);
  xj_kv_int(&j, "distinct_values", distinct);
  xj_kv_double(&j, "cycles_per_tick", xts_cycles_per_tick);
  xj_kv_double(&j, "quantum_cycles_per_iteration", xts_cycles_per_tick / s.iters);
  xj_kv_str(&j, "note", "value = (max - min) ticks of the identical runs, in cycles per loop iteration (the timer noise floor); "
                        "min/median/mad are null because they would describe the raw run time, not this value (see ticks_* and "
                        "median_cycles_per_iteration); the quantisation of one tick is quantum_cycles_per_iteration");
  xj_finish(&j);
  vrec(ctx, "spread", "cycles", "cycles", XI_mftb, W_MFTB, (mx - mn) * xts_cycles_per_tick / s.iters, XTS_ST_PASS, j.buf);
  return 0;
}

/* ---- cpt -------------------------------------------------------------------------------------------------------------- */
static int h_cpt(xts_ctx_t *ctx)
{
  spec_long(&M[0], "dependent add", add_dep_body, pick_iters(2) / 8 + 1);
  spec_long(&M[1], "independent add", add_ind_body, pick_iters(1) / 8 + 1);
  mrun(&M[0], ctx);
  mrun(&M[1], ctx);
  uint32_t w = xts_enc(XI_addx, 0, 3, 3, 4);
  tmeta_t t = TM("addx", "addx", "dependent_chain");
  t.baseline = 1;
  emit_meas(ctx, "add_dep", "latency", XI_addx, w, &M[0], &t);
  t = TM("addx", NULL, "independent_8_destinations");
  t.baseline = 1;
  emit_meas(ctx, "add_ind", "rthroughput", XI_addx, w, &M[1], &t);
  int ok = M[0].r.ok && !M[0].r.failed && M[1].r.ok && !M[1].r.failed;
  double sd = ok ? M[0].r.fit.slope : NAN, si = ok ? M[1].r.fit.slope : NAN;
  tsys_cptd_t d;
  tsys_cpt_decide(sd, si, xts_cycles_per_tick, &d);
  int st = xts_status_worst(mstatus(&M[0]), mstatus(&M[1]));
  if (!ok && st == XTS_ST_PASS) st = XTS_ST_SKIP;
  t = TM("addx", "addx", "dependent_over_independent_slope");
  emit_val(ctx, "dep_over_indep", "ratio", "ratio", XI_addx, w, ok ? d.ratio : NAN,
           ok && (d.ratio < 1.85 || d.ratio > 2.15) ? xts_status_worst(st, XTS_ST_NOISY) : st, &M[0], &t,
           "tick slopes of the two chains; expected 2 (2-cycle dependent add, 1 add per cycle); independent of cycles_per_tick");
  /* the decision */
  double prev = xts_cycles_per_tick;
  if (ok && d.consistent && d.set) xts_cycles_per_tick = d.new_cpt;
  char buf[1800];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_kv_str(&j, "producer", "addx");
  xj_kv_str(&j, "consumer", "addx");
  xj_kv_str(&j, "variant", "cycles_per_tick_from_add_chains");
  xj_kv_double(&j, "estimate_from_dependent", d.est_dep);
  xj_kv_double(&j, "estimate_from_independent", d.est_ind);
  xj_kv_double(&j, "slope_ticks_dependent", sd);
  xj_kv_double(&j, "slope_ticks_independent", si);
  xj_kv_double(&j, "previous", prev);
  xj_kv_double(&j, "current", xts_cycles_per_tick);
  xj_kv_bool(&j, "consistent", d.consistent);
  xj_kv_bool(&j, "set", ok && d.consistent && d.set);
  xj_kv_str(&j, "decision", d.why);
  xj_kv_int(&j, "speed_code", xts_env.speed);
  xj_kv_double(&j, "tb_hz_measured", g_tb_hz_meas);
  xj_kv_double(&j, "core_hz_implied", g_tb_hz_meas > 0 ? g_tb_hz_meas * xts_cycles_per_tick : NAN);
  xj_kv_str(&j, "assumptions", "add latency 2 cycles, 1 independent add per cycle (PPE); the ratio of the two slopes is scale free and checks them");
  xj_kv_bool(&j, "circular_in_scale", 0);
  xj_kv_bool(&j, "circular_in_assumed_latency", 1);
  xj_finish(&j);
  int st2 = !ok ? (st == XTS_ST_PASS ? XTS_ST_SKIP : st) : (d.consistent ? st : xts_status_worst(st, XTS_ST_NOISY));
  vrec(ctx, "cycles_per_tick", "ratio", "ratio", XI_addx, w, ok ? (d.consistent ? xts_cycles_per_tick : d.est) : NAN, st2, j.buf);
  return 0;
}

/* ---- family ----------------------------------------------------------------------------------------------------------- */
static const char *const cnames[] = { "tb_rtc", "mftb_cost", "noise_floor", "cpt" };
#define NC 4

static int cal_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < NC; i++) {
    xts_case_t *c = xts_case_add(cl, cnames[i]);
    if (!c) return -1;
    c->arg[0] = i;
  }
  return 0;
}

static int cal_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  switch ((int)c->arg[0]) {
  case 0: return h_tb_rtc(ctx);
  case 1: return h_mftb_cost(ctx);
  case 2: return h_noise(ctx);
  case 3: return h_cpt(ctx);
  }
  return -1;
}

XTS_FAMILY(calib, .kind = "calib", .axes = "speed", .enumerate = cal_enum, .run = cal_run, .block_cases = 1,
           .desc = "timebase vs SMC RTC, mftb cost, timer noise floor, cycles per timebase tick");
