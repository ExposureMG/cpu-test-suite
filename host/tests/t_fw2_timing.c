/* framework v2 timing: the record emitter (scaling, value override, baseline keys, status rules, detail schema), loop
 * modes with long reach (decoded and stepped through a tiny branch interpreter), setup2, slope standard error. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include <core/core.h>
#include "htest.h"

static struct { int n; char case_id[96], line[8192], detail[8192]; xts_record_t r; } cap;
static void tap(const char *family, const char *case_id, const xts_record_t *r, const char *json)
{
  (void)family;
  cap.n++;
  snprintf(cap.case_id, sizeof cap.case_id, "%s", case_id);
  snprintf(cap.line, sizeof cap.line, "%s", json);
  snprintf(cap.detail, sizeof cap.detail, "%s", r->detail ? r->detail : "");
  cap.r = *r;
}

static const xts_family_t fam = { .name = "fw2_temit", .kind = "timing" };
static xts_case_t tcase = { .id = "addx#1", .insn = XI_addx };
static xts_ctx_t tctx = { .fam = &fam, .c = &tcase, .block_key = "r0:fw2.temit:-:c0" };

/* a measured result: ticks/iter = intercept + slope * depth, MAD 1 % of the deepest point */
static void planted(xts_tspec_t *s, xts_tresult_t *r, double slope_ticks, double intercept_ticks)
{
  xts_tspec_t d = XTS_TSPEC_DEFAULT;
  *s = d;
  s->unit = "3 adds";
  memset(r, 0, sizeof *r);
  double xs[XTS_TMAX_DEPTHS], ys[XTS_TMAX_DEPTHS];
  for (int i = 0; i < s->ndepths; i++) {
    xs[i] = s->depths[i];
    ys[i] = r->median[i] = intercept_ticks + slope_ticks * s->depths[i];
    r->mad[i] = 0.001 * r->median[i];
    r->minv[i] = r->median[i] - 0.002;
  }
  r->ok = 1;
  r->nruns = s->ndepths * s->repeats + 1;
  r->kernel_words_max = 77;
  xts_fit_line(xs, ys, s->ndepths, &r->fit);
  xts_time_gate(s, r);
}

static void emit(const xts_tspec_t *s, const xts_tresult_t *r, int insn, const xts_temit_t *e)
{
  cap.n = 0;
  xts_time_emit_ex(&tctx, s, r, insn, e);
}

static void d_fn(xts_json_t *j, void *arg) { xj_kv_int(j, "fn", (int)(intptr_t)arg); }

HTEST(fw2_temit_scaling)
{
  double cpt = xts_cycles_per_tick;
  xts_cycles_per_tick = 64;
  xts_sink_tap = tap;
  xts_tspec_t s;
  xts_tresult_t r;
  planted(&s, &r, 6.0 / 64, 0.5);                 /* 6 cycles per spec unit, overhead 32 cycles */
  HT_EQ_U64(r.status, XTS_ST_PASS);
  HT_NEAR(r.cycles_per_unit, 6.0, 1e-9);
  /* per_unit 3: value = 2 cycles per instruction; min/median/mad per instruction; baseline from the metric */
  emit(&s, &r, XI_addx, &(xts_temit_t){ .metric = "latency", .per_unit = 3, .baseline_key = "*",
                                        .extra_json = "{\"producer\":\"addx\",\"variant\":\"chain\"}",
                                        .detail_fn = d_fn, .detail_arg = (void *)1 });
  HT_EQ_U64(cap.n, 1);
  HT_NEAR(cap.r.value, 2.0, 1e-9);
  HT_NEAR(cap.r.median, 2.0, 1e-9);
  HT_NEAR(cap.r.mad, 0.001 * (0.5 + 6.0 / 64 * 32) / 32 * 64 / 3, 1e-9);
  HT_NEAR(cap.r.min, ((0.5 + 6.0 / 64 * 32) - 0.002 - 0.5) / 32 * 64 / 3, 1e-9);
  HT_NEAR(cap.r.n, 31, 0);
  HT_NEAR(cap.r.fit_r2, 1.0, 1e-9);
  HT_EQ_U64(cap.r.status, XTS_ST_PASS);           /* 2.0 vs JSON latency 2.04: within 10 % */
  HT_CHECK(!strcmp(cap.r.unit, "cycles") && !strcmp(cap.r.metric, "latency") && !strcmp(cap.r.mnem, "addx"));
  const char *pre = "{\"producer\":\"addx\",\"variant\":\"chain\",\"fn\":1,\"fit\":{\"unit\":\"3 adds\",\"iters\":256,"
                    "\"status\":\"pass\",\"cpu\":6,\"slope_ticks\":";
  HT_CHECK(!strncmp(cap.detail, pre, strlen(pre)));
  static const char *const keys[] = { "\"slope_ticks\":", "\"intercept_ticks\":0.5", "\"r2\":1", "\"max_resid_rel\":", "\"spread\":",
                                      "\"aba_shift\":0", "\"slope_se\":", "\"depths\":[4,8,12,16,24,32]", "\"ticks_med\":[0.875,",
                                      "\"ticks_mad\":[", "\"cycles_per_tick\":64", "\"per_unit\":3", "\"kernel_words_max\":77}",
                                      "\"baseline\":2.04", "\"baseline_key\":\"latency\"" };
  for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++)
    if (!strstr(cap.detail, keys[i])) { printf("    missing %s in %s\n", keys[i], cap.detail); HT_CHECK(!keys[i]); }
  HT_CHECK(!strcmp(cap.case_id, "addx#1"));
  HT_CHECK(strstr(cap.line, "\"word\":\"\"") != NULL);

  /* rthroughput with "*" compares with the JSON "issue" (0.98): 2.0 is off by > 10 % -> regressed */
  emit(&s, &r, XI_addx, &(xts_temit_t){ .metric = "rthroughput", .per_unit = 3, .baseline_key = "*" });
  HT_EQ_U64(cap.r.status, XTS_ST_REGRESSED);
  HT_CHECK(strstr(cap.detail, "\"baseline_key\":\"issue\"") != NULL);
  /* explicit key, wider tolerance */
  emit(&s, &r, XI_addx, &(xts_temit_t){ .metric = "issue_cost", .per_unit = 3, .baseline_key = "issue", .baseline_tol = 1.5 });
  HT_EQ_U64(cap.r.status, XTS_ST_PASS);
  /* no baseline key: no check */
  emit(&s, &r, XI_addx, &(xts_temit_t){ .metric = "rthroughput", .per_unit = 3 });
  HT_EQ_U64(cap.r.status, XTS_ST_PASS);
  HT_CHECK(strstr(cap.detail, "baseline") == NULL);

  /* value override: a penalty is not the cycle figure -> min/median/mad null; unit, word, suffix, info status */
  emit(&s, &r, XI_addx, &(xts_temit_t){ .metric = "penalty", XTS_TE_VALUE(4.25), .word = 0x7C642A14u, .case_suffix = "dep",
                                        .unit = "cycles", .status = XTS_ST_INFO });
  HT_NEAR(cap.r.value, 4.25, 0);
  HT_CHECK(isnan(cap.r.median) && isnan(cap.r.min) && isnan(cap.r.mad));
  HT_NEAR(cap.r.fit_r2, 1.0, 1e-9);
  HT_EQ_U64(cap.r.status, XTS_ST_INFO);
  HT_CHECK(!strcmp(cap.case_id, "addx#1/dep"));
  HT_CHECK(strstr(cap.line, "\"word\":\"7C642A14\"") != NULL);
  /* an override equal to the scaled slope keeps the statistics */
  emit(&s, &r, -1, &(xts_temit_t){ .metric = "cycles", .per_unit = 2, XTS_TE_VALUE(3.0) });
  HT_NEAR(cap.r.median, 3.0, 1e-9);
  HT_CHECK(!strcmp(cap.r.mnem ? cap.r.mnem : "", ""));          /* no insn: the case's mnemonic (filled by xts_emit) */
  HT_CHECK(strstr(cap.line, "\"mnem\":\"addx\"") != NULL);
  /* ratio unit, full case id override */
  emit(&s, &r, -1, &(xts_temit_t){ .metric = "ratio", .unit = "ratio", XTS_TE_VALUE(0.5), .case_id = "other" });
  HT_CHECK(!strcmp(cap.case_id, "other") && !strcmp(cap.r.unit, "ratio"));

  /* implausible: below the floor, negative */
  emit(&s, &r, -1, &(xts_temit_t){ .metric = "rthroughput", .per_unit = 3, .floor = 3.0 });
  HT_EQ_U64(cap.r.status, XTS_ST_NOISY);
  HT_CHECK(strstr(cap.detail, "\"implausible\":\"below the physical floor\",\"floor\":3") != NULL);
  emit(&s, &r, -1, &(xts_temit_t){ .metric = "rthroughput", .per_unit = 3, .floor = 2.0 });   /* 2.0 >= 0.97 * 2 */
  HT_EQ_U64(cap.r.status, XTS_ST_PASS);
  emit(&s, &r, -1, &(xts_temit_t){ .metric = "extra", XTS_TE_VALUE(-1.0), .no_negative = 1 });
  HT_EQ_U64(cap.r.status, XTS_ST_NOISY);
  HT_CHECK(strstr(cap.detail, "negative cost") != NULL);
  emit(&s, &r, -1, &(xts_temit_t){ .metric = "extra", XTS_TE_VALUE(-1.0) });
  HT_EQ_U64(cap.r.status, XTS_ST_PASS);
  /* a noisy measurement is never upgraded and never baseline-checked */
  xts_tresult_t rn = r;
  rn.status = XTS_ST_NOISY;
  emit(&s, &rn, XI_addx, &(xts_temit_t){ .metric = "latency", .per_unit = 3, .baseline_key = "*" });
  HT_EQ_U64(cap.r.status, XTS_ST_NOISY);
  HT_CHECK(strstr(cap.detail, "baseline") == NULL);

  /* failed measurement: fail, value null, error */
  xts_tresult_t rf;
  memset(&rf, 0, sizeof rf);
  rf.failed = 1;
  rf.status = XTS_ST_SKIP;
  snprintf(rf.why, sizeof rf.why, "kernel build (depth 4): boom");
  emit(&s, &rf, XI_addx, &(xts_temit_t){ .metric = "latency" });
  HT_EQ_U64(cap.r.status, XTS_ST_FAIL);
  HT_CHECK(isnan(cap.r.value));
  HT_CHECK(strstr(cap.detail, "\"error\":\"kernel build (depth 4): boom\"") != NULL);
  HT_CHECK(strstr(cap.detail, "\"status\":\"fail\",\"why\":\"kernel build (depth 4): boom\",\"cpu\":null") != NULL);
  /* host dry run: skip, value null, fit keys null */
  xts_tresult_t rh;
  memset(&rh, 0, sizeof rh);
  rh.status = XTS_ST_SKIP;
  rh.nruns = 6;
  snprintf(rh.why, sizeof rh.why, "not executed (host dry run)");
  emit(&s, &rh, XI_addx, &(xts_temit_t){ .metric = "latency", .baseline_key = "*" });
  HT_EQ_U64(cap.r.status, XTS_ST_SKIP);
  HT_CHECK(isnan(cap.r.value) && isnan(cap.r.median));
  HT_NEAR(cap.r.n, 6, 0);
  HT_CHECK(strstr(cap.detail, "\"ticks_med\":[null,null,null,null,null,null]") != NULL);

  /* differential: (6 - 2) / per; statistics null; worst status; both fits */
  xts_tspec_t sb;
  xts_tresult_t rb;
  planted(&sb, &rb, 2.0 / 64, 0.4);
  rb.status = XTS_ST_NOISY;
  cap.n = 0;
  xts_time_emit_diff(&tctx, &s, &r, &sb, &rb, XI_addx, &(xts_temit_t){ .metric = "extra", .per_unit = 2, .case_suffix = "rc" });
  HT_NEAR(cap.r.value, 2.0, 1e-9);
  HT_CHECK(isnan(cap.r.median) && isnan(cap.r.min));
  HT_NEAR(cap.r.n, 62, 0);
  HT_EQ_U64(cap.r.status, XTS_ST_NOISY);
  HT_CHECK(strstr(cap.detail, "\"fit\":{") && strstr(cap.detail, "\"ref\":{"));
  HT_CHECK(!strcmp(cap.case_id, "addx#1/rc"));
  /* value-only record without a fit */
  cap.n = 0;
  xts_time_emit_value(&tctx, NULL, NULL, -1, 0.75, &(xts_temit_t){ .metric = "ratio", .unit = "ratio", .status = XTS_ST_INFO });
  HT_NEAR(cap.r.value, 0.75, 0);
  HT_EQ_U64(cap.r.status, XTS_ST_INFO);
  HT_CHECK(strstr(cap.detail, "\"fit\"") == NULL);

  /* the v1 call: value = cycles per spec unit, baseline from the metric (6 vs 2.04 -> regressed), unified detail */
  cap.n = 0;
  xts_time_emit(&tctx, &s, &r, "latency", XI_addx);
  HT_NEAR(cap.r.value, 6.0, 1e-9);
  HT_NEAR(cap.r.median, 6.0, 1e-9);
  HT_EQ_U64(cap.r.status, XTS_ST_REGRESSED);
  HT_CHECK(!strncmp(cap.detail, "{\"fit\":{", 8));
  HT_EQ_U64(xts_time_cycles(&r, 3) * 1000 + 0.5, 2000);
  HT_CHECK(isnan(xts_time_cycles(&rf, 1)));
  HT_CHECK(!strcmp(xts_time_baseline_key("issue_cost"), "issue") && !strcmp(xts_time_baseline_key("penalty"), "penalty"));
  xts_sink_tap = NULL;
  xts_sink_discard();
  xts_cycles_per_tick = cpt;
}

HTEST(fw2_slope_se)
{
  static const double x[] = { 4, 8, 12, 16, 24, 32 }, y[] = { 10.1, 19.8, 30.3, 39.9, 60.2, 79.7 };
  xts_fit_t f;
  HT_EQ_U64(xts_fit_line(x, y, 6, &f), 0);
  double mx = 16, sxx = 0, ssr = 0;
  for (int i = 0; i < 6; i++) {
    sxx += (x[i] - mx) * (x[i] - mx);
    double r = y[i] - (f.intercept + f.slope * x[i]);
    ssr += r * r;
  }
  HT_NEAR(f.slope_se, sqrt(ssr / 4 / sxx), 1e-12);
  xts_fit_line(x, y, 2, &f);
  HT_NEAR(f.slope_se, 0, 0);
  /* err_cycles: the sched_classes error bar (slope SE and deepest MAD), floor 0.01 */
  xts_tspec_t s = XTS_TSPEC_DEFAULT;
  xts_tresult_t r;
  memset(&r, 0, sizeof r);
  for (int i = 0; i < 6; i++) { r.median[i] = y[i]; r.mad[i] = 0.32; }
  r.ok = 1;
  xts_fit_line(x, y, 6, &r.fit);
  xts_time_gate(&s, &r);
  HT_NEAR(r.slope_se, r.fit.slope_se, 0);
  HT_NEAR(r.err_cycles, xts_cycles_per_tick * sqrt(r.slope_se * r.slope_se + 0.01 * 0.01), 1e-9);
}

/* ---- loop modes ---- */
static int nop_body(xts_kbuf_t *k, int depth, void *arg)
{
  (void)arg;
  for (int i = 0; i < depth; i++) xk_nop(k);
  return 0;
}

/* step the framed kernel from word 0: returns how many times the first body word ran (-1 on an unexpected word) */
static int step_loop(const xts_kbuf_t *k, int body_at, uint64_t iters)
{
  uint64_t gpr[32] = { 0 }, ctr = 0;
  uint32_t cr = 0;
  gpr[29] = iters;
  int body_runs = 0, steps = 0;
  for (uint32_t pc = 0; pc < k->n && steps < 10000000; steps++) {
    uint32_t w = k->w[pc];
    if (k->tag[pc] == XK_TAG_EXIT) return body_runs;
    if ((int)pc == body_at) body_runs++;
    int id = xts_decode1(w);
    const xts_insn_t *d = xts_insn(id);
    if (!d) return -1;
    if (id == XI_ori || id == XI_isync || id == XI_mftb) { pc++; continue; }
    if (id == XI_mtspr && xts_extract(id, w, 0) == 9) { ctr = gpr[xts_extract(id, w, 1)]; pc++; continue; }
    if (id == XI_addi) {
      int rd = (int)xts_extract(id, w, 0), ra = (int)xts_extract(id, w, 1);
      gpr[rd] = (ra ? gpr[ra] : 0) + (uint64_t)xts_extract(id, w, 2);
      pc++;
      continue;
    }
    if (id == XI_cmpi) {
      int crf = (int)xts_extract(id, w, 0);
      int64_t a = (int32_t)gpr[xts_extract(id, w, 2)], b = xts_extract(id, w, 3);
      uint32_t c = a < b ? 8 : a > b ? 4 : 2;
      cr = (cr & ~(0xFu << (28 - 4 * crf))) | (c << (28 - 4 * crf));
      pc++;
      continue;
    }
    if (id == XI_bcx) {
      int bo = (int)xts_extract(id, w, 0), bi = (int)xts_extract(id, w, 1);
      int64_t bd = xts_extract(id, w, 2);
      if (!(bo & 4)) ctr--;
      int ctr_ok = (bo & 4) || ((ctr != 0) ^ ((bo >> 1) & 1));
      int cond_ok = (bo & 16) || (((cr >> (31 - bi)) & 1) == (uint32_t)((bo >> 3) & 1));
      pc = (ctr_ok && cond_ok) ? (uint32_t)((int64_t)pc + bd / 4) : pc + 1;
      continue;
    }
    if (id == XI_bx) { pc = (uint32_t)((int64_t)pc + xts_extract(id, w, 0) / 4); continue; }
    return -1;
  }
  return -1;
}

static int find_body(const xts_kbuf_t *k)
{
  /* first word after the mftb r27 + alignment nops: the loop top is 32-byte aligned */
  for (uint32_t i = 0; i < k->n; i++)
    if (xts_decode1(k->w[i]) == XI_mftb) return (int)((i + 1 + 7) & ~7u);
  return -1;
}

HTEST(fw2_loop_modes)
{
  xts_kbuf_t *k = xk_new(20000);
  xts_tspec_t s = XTS_TSPEC_DEFAULT;
  s.body = nop_body;
  char why[96];
  /* CTR mode is the v1 frame, word for word */
  HT_EQ_U64(xts_time_build(&s, 3, k), 0);
  static const uint32_t v1[] = { 0x7FA903A6u, 0x4C00012Cu, 0x7F6C42E6u, 0x60000000u, 0x60000000u, 0x60000000u, 0x60000000u,
                                 0x60000000u, 0x60000000u, 0x60000000u, 0x60000000u, 0x4200FFF4u, 0x7F8C42E6u };
  HT_EQ_U64(k->n, 14);
  for (unsigned i = 0; i < 13 && i < k->n; i++) HT_EQ_U64(k->w[i], v1[i]);
  HT_EQ_U64(k->tag[13], XK_TAG_EXIT);
  for (int mode = 0; mode < 2; mode++)
    for (int far = 0; far < 2; far++) {
      s.loop = mode ? XTS_LOOP_GPR : XTS_LOOP_CTR;
      s.loop_crf = mode ? 3 : 0;
      int depth = far ? 9000 : 5;                        /* 9000 words = 36 KB: beyond the bc reach */
      HT_EQ_U64(xts_time_build(&s, depth, k), 0);
      HT_EQ_U64(xk_validate(k, NULL, NULL, why, sizeof why), 0);
      int body = find_body(k), nmtctr = 0, nbc = 0, nb = 0;
      for (uint32_t i = 0; i < k->n; i++) {
        int id = xts_decode1(k->w[i]);
        nmtctr += id == XI_mtspr;
        nbc += id == XI_bcx;
        nb += id == XI_bx && k->tag[i] != XK_TAG_EXIT;
      }
      HT_EQ_U64(nmtctr, mode ? 0 : 1);                    /* GPR mode never touches CTR */
      HT_EQ_U64(nbc, 1);
      HT_EQ_U64(nb, far ? 1 : 0);                         /* long reach: bdz/beq out ; b top */
      for (uint64_t it = 1; it <= 7; it += 3) HT_EQ_U64(step_loop(k, body, it), it);
      if (mode) {
        /* the closure: addi r29,r29,-1 ; cmpwi cr3,r29,0 ; bne cr3 / beq cr3 */
        uint32_t c = far ? k->n - 6 : k->n - 5;
        HT_EQ_U64(k->w[c], xts_enc(XI_addi, 0, 29, 29, -1));
        HT_EQ_U64(k->w[c + 1], xts_enc(XI_cmpi, 0, 3, 0, 29, 0));
        HT_EQ_U64(xts_extract(XI_bcx, k->w[c + 2], 0), far ? 12 : 4);
        HT_EQ_U64(xts_extract(XI_bcx, k->w[c + 2], 1), 14);
      }
    }
  s.loop = 5;
  HT_CHECK(xts_time_build(&s, 4, k) < 0);
  /* xk_loop_close directly */
  xk_reset(k);
  int top = xk_here(k);
  xk_nop(k);
  HT_CHECK(xk_loop_close(k, top, XTS_LOOP_GPR, 1, 0) < 0);  /* r1 is reserved */
  xk_reset(k);
  top = xk_here(k);
  xk_nop(k);
  HT_CHECK(xk_loop_close(k, top, XTS_LOOP_GPR, 29, 0) >= 0);
  xk_exit(k);
  HT_EQ_U64(xk_finish(k), 0);
  HT_EQ_U64(step_loop(k, 0, 4), 4);
  xk_free(k);
}

/* setup2: depth and run index (host: one run per depth, run = depth index) */
static int s2_calls, s2_depth[16], s2_run[16];
static int s2_setup(xts_exec_t *x, int depth, int run, void *arg)
{
  (void)x;
  (void)arg;
  if (s2_calls < 16) { s2_depth[s2_calls] = depth; s2_run[s2_calls] = run; }
  s2_calls++;
  return 0;
}
static int s1_calls;
static int s1_setup(xts_exec_t *x, void *arg) { (void)x; (void)arg; s1_calls++; return 0; }

HTEST(fw2_setup2)
{
  xts_tspec_t s = XTS_TSPEC_DEFAULT;
  xts_tresult_t r;
  s.body = nop_body;
  s.setup = s1_setup;
  s.setup2 = s2_setup;
  s.loop = XTS_LOOP_GPR;
  HT_EQ_U64(xts_time_measure(&s, NULL, &r), 0);
  HT_EQ_U64(r.failed, 0);
  HT_EQ_U64(s2_calls, 6);
  HT_EQ_U64(s1_calls, 0);                                 /* setup2 replaces setup */
  for (int i = 0; i < 6; i++) { HT_EQ_U64(s2_depth[i], s.depths[i]); HT_EQ_U64(s2_run[i], i); }
  s.setup2 = NULL;
  HT_EQ_U64(xts_time_measure(&s, NULL, &r), 0);
  HT_EQ_U64(s1_calls, 6);
  s.ndepths = 1;                                          /* bad spec: failed */
  HT_CHECK(xts_time_measure(&s, NULL, &r) < 0);
  HT_EQ_U64(r.failed, 1);
}
