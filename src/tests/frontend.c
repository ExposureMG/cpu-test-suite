/* frontend.loop - the cost of a loop iteration as a function of body length and loop-top offset.
 *
 * Every timing family fits ticks/iteration against depth and takes the slope as the per-unit cost. That only works if
 * the per-iteration loop cost (bdnz, fetch of the body) is the same at every depth. The sched run (2026-10-19) showed
 * it is not at code_align=0: the 1-cycle filler measured 0.76 .. 1.42 cycles per word across 8-word depth steps.
 *
 * This family maps the loop itself: for each loop-top offset within a 128-byte I-cache line (case "off<NN>", 0..124
 * in steps of 4) the body is N independent `li rX,imm` (8 rotating destinations, no register reads) for N = 1 ..
 * FE_MAXLEN, framed exactly like xts_time_build (mtctr; isync; mftb; <align 128 + offset> top: body; bdnz top; mftb).
 * Each point is the minimum of FE_REPEATS runs after one warm-up run; iterations are chosen for ~30k cycles per run.
 * Record value: cycles per iteration at N = 128 minus 128 (the loop overhead with a long body); detail.cyc[N-1] =
 * cycles per iteration at length N. The data picks the depth method (line-sized steps, minimum body length).
 *
 * method.<old|new>.<li|add>: the timing engine itself (xts_time_measure) on two units, with the layout every family
 * uses today (old: loop top aligned to 32 bytes, depths 8..48 units) and the candidate (new: top at byte 12 of a
 * 16-byte fetch group, depths 32..192 units = whole 128-byte lines). li = independent `li` stream (expected 1 cycle
 * per word), add = dependent `add r3,r3,r4` chain (expected 2 cycles). A record per method and unit: status, slope,
 * fit residual and marginal costs show whether the new layout gives clean linear fits. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include <xts/json.h>

#define FE_MAXLEN 160
#define FE_REPEATS 3
#define FE_OFFSETS 32                          /* 0 .. 124 step 4 */
#define R_ITERS_REG 29                         /* the timing frame's registers (CONTRIBUTING: r27-r29) */
#define R_T0_REG 27
#define R_T1_REG 28

enum { FE_MAP = 0, FE_METHOD = 1 };

static int fe_enum(xts_caselist_t *cl)
{
  static const char *const meth[2] = { "old", "new" }, *const unit[2] = { "li", "add" };
  for (int m = 0; m < 2; m++)
    for (int u = 0; u < 2; u++) {
      xts_case_t *c = xts_case_addf(cl, "method.%s.%s", meth[m], unit[u]);
      if (!c) return -1;
      c->arg[0] = FE_METHOD;
      c->arg[1] = m;
      c->arg[2] = u;
    }
  for (int o = 0; o < FE_OFFSETS; o++) {
    xts_case_t *c = xts_case_addf(cl, "off%03d", o * 4);
    if (!c) return -1;
    c->arg[0] = FE_MAP;
    c->arg[1] = o * 4;
  }
  return 0;
}

static int body_li(xts_kbuf_t *k, int depth, void *arg)
{
  (void)arg;
  for (int i = 0; i < depth; i++) xk_emit(k, XI_addi, 0, 3 + (i & 7), 0, i & 0x7FFF);
  return 0;
}

static int body_add(xts_kbuf_t *k, int depth, void *arg)
{
  (void)arg;
  for (int i = 0; i < depth; i++) xk_emit(k, XI_addx, 0, 3, 3, 4);
  return 0;
}

static int fe_method(const xts_case_t *c, xts_ctx_t *ctx)
{
  int nw = (int)c->arg[1], add = (int)c->arg[2];
  static const int old_d[6] = { 8, 16, 24, 32, 40, 48 }, new_d[6] = { 32, 64, 96, 128, 160, 192 };
  xts_tspec_t s = XTS_TSPEC_DEFAULT;
  s.unit = add ? "add r3,r3,r4 (dependent)" : "li rX,imm (independent)";
  s.body = add ? body_add : body_li;
  s.ndepths = 6;
  for (int i = 0; i < 6; i++) s.depths[i] = nw ? new_d[i] : old_d[i];
  s.repeats = 5;
  s.align = nw ? 16 : 32;
  s.top_offset = nw ? 12 : 0;
  s.iters = (uint32_t)(60000 / ((add ? 2 : 1) * s.depths[0] + 8));   /* ~60k cycles at the first depth */
  if (s.iters < 64) s.iters = 64;
  xts_tresult_t r;
  if (xts_time_measure(&s, ctx, &r) < 0) return -1;
  char ex[160];
  snprintf(ex, sizeof ex, "{\"method\":\"%s\",\"align\":%u,\"top_offset\":%d,\"expected\":%d}", nw ? "new" : "old",
           (unsigned)s.align, s.top_offset, add ? 2 : 1);
  xts_time_emit_ex(ctx, &s, &r, add ? XI_addx : XI_addi,
                   &(xts_temit_t){ .metric = add ? "latency" : "issue_cost", .extra_json = ex, .status = XTS_ST_INFO });
  return 0;
}

static int fe_build(xts_kbuf_t *k, int len, int off)
{
  xk_reset(k);
  xk_prio_prologue(k);
  xk_emit(k, XI_mtspr, 0, 9, R_ITERS_REG);     /* mtctr r29 */
  xk_emit(k, XI_isync, 0);
  xk_emit(k, XI_mftb, 0, R_T0_REG, 268);
  xk_align(k, 128);
  for (int i = 0; i < off / 4; i++) xk_emit(k, XI_ori, 0, 0, 0, 0);   /* nop: loop top at offset `off` in the line */
  int top = xk_here(k);
  for (int i = 0; i < len; i++) xk_emit(k, XI_addi, 0, 3 + (i & 7), 0, i & 0x7FFF);
  xk_loop_close(k, top, XTS_LOOP_CTR, R_ITERS_REG, 7);
  xk_emit(k, XI_mftb, 0, R_T1_REG, 268);
  xk_exit(k);
  return xk_finish(k);
}

static int fe_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (c->arg[0] == FE_METHOD) return fe_method(c, ctx);
  int off = (int)c->arg[1];
  static xts_kbuf_t *k;
  static xts_exec_t x;
  static double cyc[FE_MAXLEN];
  if (!k && !(k = xk_new(512))) return -1;
  char det[2600];
  xts_json_t j;
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  xj_kv_int(&j, "offset", off);
  xj_kv_str(&j, "body", "N x li rX,imm (8 rotating destinations), bdnz loop, top at 128-byte line + offset");
  int ok = 1, host = ctx->host || xts_exec_is_host();
  for (int n = 1; n <= FE_MAXLEN && ok; n++) {
    if (fe_build(k, n, off) < 0) { xj_kv_str(&j, "error", k->errmsg); ok = 0; break; }
    if (host) { cyc[n - 1] = NAN; continue; }
    uint32_t iters = (uint32_t)(30000 / (n + 4));
    if (iters < 64) iters = 64;
    double best = INFINITY;
    for (int rep = -1; rep < FE_REPEATS; rep++) {   /* rep -1: warm-up, not recorded */
      xts_case_init(&x);
      x.in.gpr[R_ITERS_REG] = iters;
      if (xts_exec_kernel(&x, k) != XTS_EXEC_OK || x.trap.trapped) { xj_kv_str(&j, "error", "kernel did not run"); ok = 0; break; }
      double t = (double)(uint32_t)(x.out.gpr[R_T1_REG] - x.out.gpr[R_T0_REG]) * xts_cycles_per_tick / iters;
      if (rep >= 0 && t < best) best = t;
    }
    cyc[n - 1] = best;
    xts_watchdog_kick();
  }
  xj_key(&j, "cyc");
  xj_arr(&j);
  for (int n = 0; n < FE_MAXLEN; n++) xj_double(&j, ok && !host ? round(cyc[n] * 100) / 100 : NAN);
  xj_end(&j);
  xj_kv_double(&j, "cycles_per_tick", xts_cycles_per_tick);
  xj_end(&j);
  xts_record_t r;
  xts_record_init(&r);
  r.metric = "cycles";
  r.unit = "cycles";
  r.value = ok && !host ? cyc[127] - 128 : NAN;
  r.status = host ? XTS_ST_SKIP : ok ? XTS_ST_INFO : XTS_ST_FAIL;
  r.detail = xj_finish(&j);
  r.detail_truncated = j.truncated;
  xts_emit(ctx, &r);
  return 0;
}

XTS_FAMILY(frontend_loop, .kind = "calib", .enumerate = fe_enum, .run = fe_run, .block_cases = 36,
           .desc = "loop cost per iteration vs body length (1-160 words) and loop-top offset in the I-cache line");
