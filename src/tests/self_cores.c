/* self.cores - the core runtime (xts/xcore.h), checked from the suite core:
 *   roles        the suite runs on PIR 2 and a remote call lands on PIR 0 (console)
 *   pingpong     10,000 remote no-op calls (xts_core_xc_call): round-trip latency in timebase ticks, min/median/max
 *   heartbeat    the heartbeats of PIR 0 (executor), 2 (suite) and 4 (transmitter) all advance within 20 ms (console)
 *   code_sync    1,000 times: write `li r3,k; blr` on the suite core (xts_sync_code: dcbst, sync, icbi), call it on the
 *                test core (the executor does sync; isync first) and get k back - cross-core instruction-cache coherence
 *   data_sync_<how> 1,000 rounds of cross-core DATA hand-over through a 512-byte cached buffer (the exec engine's
 *                scratch path), counting rounds in which the reader saw any stale word:
 *     c2t_read   suite writes + dcbf, test core reads; the test core had read the buffer the round before   (info)
 *     c2t_store  same, but the test core had STORED to the buffer the round before (a store case, then a load case)  (info)
 *     c2t_staged as c2t_store, and the test core dcbf's its own copy first (what the exec engine does)     (must be 0)
 *     t2c_plain  test core writes, suite reads; the suite had read the buffer the round before          (info)
 *     t2c_staged as t2c_plain, and the suite dcbf's its own copy before reading (what the exec engine does) (must be 0)
 *     c2t_xdcbt  as c2t_read, but afterwards the test core drops the lines (dcbf) and refetches them with xdcbt,
 *                as in the semantic run, where 24 dcbz128 cases (which drop the line from L1) preceded the xdcbt ones (info)
 *     c2t_xdcbt_staged  as c2t_xdcbt, and the test core dcbf's its own copy first                       (must be 0)
 *                Hardware (cores runs 2026-10-19, Jasper): every variant 0 except c2t_xdcbt: 999 of 1,000 rounds
 *                stale (127,840 of 128,000 words) - a line xdcbt FETCHES into a core's L1 is not invalidated by
 *                another core's stores and dcbf. An xdcbt on a line already in L1 changes nothing (also 0).
 *                c2t_xdcbt_staged 0: the reader's own dcbf removes the line.
 *   irq_ee<0|1>  on the test core, 200 timed runs of a fixed ~0.4 ms bdnz loop with external interrupts enabled (ee1,
 *                as every measurement runs today) and masked (ee0, MSR[EE] = 0 for the probe only): how many runs carry
 *                extra time and how much (info). The sched run showed a ~5,300-cycle event roughly every 1.4 ms on the
 *                test core; if ee0 has no outliers and ee1 has, it is an interrupt.
 *   smt_exit_p<n> a busy_int load on PIR 1/3/5 starts, its heartbeat advances, it stops through xts_smt_exit and the exit
 *                self-check (smt_entry.S: SPRG2 == table == TCB self word, PIR) passed (exit_why ok)
 * A lost test core never returns from xts_core_xc_call (the core reports the crash/hang and halts). Host: roles,
 * heartbeat and code_sync skip (nothing executes on the PC); pingpong and smt_exit run on the host simulations. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include <xts/json.h>
#include <xts/xcore.h>
#include <core/core.h>              /* xts_core_xc_call */
#include <smt/smt.h>
#include <smt/smt_int.h>            /* smtb_relax (host simulation step), xts_smt_tcbs */

#define PINGS 10000
#define SYNCS 1000
#define DSYNC_WORDS 128                        /* 512 bytes: XTS_SCRATCH_BYTES, 4 cache lines */

enum { C_ROLES, C_PING, C_HB, C_SYNC, C_SMT, C_DSYNC, C_IRQ };
#define IRQ_RUNS 200
#define IRQ_LOOP 400000u                       /* bdnz iterations per run: ~0.4 ms */
enum { D_C2T_READ, D_C2T_STORE, D_C2T_STAGED, D_C2T_XDCBT, D_C2T_XDCBT_STAGED, D_T2C_PLAIN, D_T2C_STAGED, D_N };
static const char *const dsync_names[D_N] = { "c2t_read", "c2t_store", "c2t_staged", "c2t_xdcbt", "c2t_xdcbt_staged",
                                              "t2c_plain", "t2c_staged" };
#define D_IS_C2T(m) ((m) <= D_C2T_XDCBT_STAGED)
#define D_STAGED(m) ((m) == D_C2T_STAGED || (m) == D_C2T_XDCBT_STAGED || (m) == D_T2C_STAGED)

static int cores_enum(xts_caselist_t *cl)
{
  static const char *const names[] = { "roles", "pingpong", "heartbeat", "code_sync" };
  for (int i = 0; i < 4; i++) {
    xts_case_t *c = xts_case_add(cl, names[i]);
    if (!c) return -1;
    c->arg[0] = i;
  }
  for (int i = 0; i < D_N; i++) {
    xts_case_t *c = xts_case_addf(cl, "data_sync_%s", dsync_names[i]);
    if (!c) return -1;
    c->arg[0] = C_DSYNC;
    c->arg[1] = i;
  }
  for (int ee = 1; ee >= 0; ee--) {
    xts_case_t *c = xts_case_addf(cl, "irq_ee%d", ee);
    if (!c) return -1;
    c->arg[0] = C_IRQ;
    c->arg[1] = ee;
  }
  static const int pirs[] = { 1, 3, 5 };
  for (int i = 0; i < 3; i++) {
    xts_case_t *c = xts_case_addf(cl, "smt_exit_p%d", pirs[i]);
    if (!c) return -1;
    c->arg[0] = C_SMT;
    c->arg[1] = pirs[i];
  }
  return 0;
}

static void nop_job(void *a, void *b) { (void)a; (void)b; }
static void pir_job(void *a, void *b)
{
  (void)b;
  *(uint64_t *)a = xts_mfspr64(1023);
}
static void code_job(void *a, void *b)
{
  uint32_t (*f)(void) = (uint32_t (*)(void))a;
  *(uint32_t *)b = f();
}

/* data_sync: the buffer and the round's job; each round's pattern is distinct in every word */
static uint32_t dbuf[DSYNC_WORDS] __attribute__((aligned(128)));
typedef struct dsync_job {
  uint32_t round, mode, stale, sink;
  uint32_t xdcbt_fn;                           /* code: xdcbt 0,r3 ; blr (c2t_xdcbt*) */
} dsync_job_t;
static uint32_t xd_code[32] __attribute__((aligned(128)));
static uint32_t dpat(uint32_t round, uint32_t i) { return (round * 0x9E3779B1u) ^ (i * 0x01000193u) ^ 0x5A5A0000u; }
/* dcbf on THIS core only (xts_dcbf_range would also flush on the test core, which is what is being measured) */
static void dflush(void)
{
  __sync_synchronize();
#if defined(__powerpc__) || defined(__PPC__)
  for (uint32_t o = 0; o < sizeof dbuf; o += 128) asm volatile("dcbf 0, %0" : : "r"((uint8_t *)dbuf + o) : "memory");
#endif
  __sync_synchronize();
}
/* test core, one round: (staged: drop own copy) read and count stale words, then warm the buffer for the next round the
 * way the mode asks (read, or store the round's pattern back unchanged) */
static void dsync_c2t_job(void *a, void *b)
{
  dsync_job_t *j = a;
  (void)b;
  if (j->mode == D_C2T_STAGED || j->mode == D_C2T_XDCBT_STAGED) dflush();
  const volatile uint32_t *p = dbuf;
  uint32_t stale = 0;
  for (uint32_t i = 0; i < DSYNC_WORDS; i++) stale += p[i] != dpat(j->round, i);
  j->stale = stale;
  if (j->mode == D_C2T_STORE || j->mode == D_C2T_STAGED)
    for (uint32_t i = 0; i < DSYNC_WORDS; i++) ((volatile uint32_t *)dbuf)[i] = dpat(j->round, i);
  if (j->xdcbt_fn) {
    dflush();                                  /* out of this core's L1, so the xdcbt has to fetch the line */
    for (uint32_t o = 0; o < sizeof dbuf; o += 128) ((void (*)(void *))j->xdcbt_fn)((uint8_t *)dbuf + o);
  }
  __sync_synchronize();
}
static void dflush_job(void *a, void *b) { (void)a; (void)b; dflush(); }
static void dsync_t2c_job(void *a, void *b)
{
  dsync_job_t *j = a;
  (void)b;
  for (uint32_t i = 0; i < DSYNC_WORDS; i++) ((volatile uint32_t *)dbuf)[i] = dpat(j->round, i);
  __sync_synchronize();
}

/* irq probe: runs on the test core inside the measurement fence */
static uint32_t irq_dt[IRQ_RUNS];
static void irq_job(void *a, void *b)
{
  int mask = !(int)(uintptr_t)a;
  (void)b;
#if defined(__powerpc__) || defined(__PPC__)
  uint64_t msr;
  asm volatile("mfmsr %0" : "=r"(msr));
  if (mask) asm volatile("mtmsrd %0, 1" : : "r"(msr & ~0x8000ull) : "memory");   /* EE off (L=1: EE and RI only) */
  for (int k = 0; k < IRQ_RUNS; k++) {
    uint64_t t0 = xts_tb();
    asm volatile("mtctr %0\n1:\tbdnz 1b" : : "r"(IRQ_LOOP) : "ctr");
    irq_dt[k] = (uint32_t)(xts_tb() - t0);
  }
  if (mask) asm volatile("mtmsrd %0, 1" : : "r"(msr) : "memory");
#else
  (void)mask;
#endif
}

static int cmp_u32(const void *a, const void *b)
{
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : x > y;
}

static void emit(xts_ctx_t *ctx, int status, const char *metric, double v, const char *unit, xts_json_t *j)
{
  xts_record_t r;
  xts_record_init(&r);
  r.metric = metric;
  r.unit = unit;
  r.value = v;
  r.status = status;
  r.detail = xj_finish(j);
  r.detail_truncated = j->truncated;
  xts_emit(ctx, &r);
}

/* skip record with the same metric/unit the case emits on hardware, so host and console key sets match */
static void skip(xts_ctx_t *ctx, const char *why, const char *metric, const char *unit)
{
  char det[128];
  snprintf(det, sizeof det, "{\"why\":\"%s\"}", why);
  xts_record_t r;
  xts_record_init(&r);
  r.metric = metric;
  r.unit = unit;
  r.status = XTS_ST_SKIP;
  r.detail = det;
  xts_emit(ctx, &r);
}

static int cores_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  static uint32_t lat[PINGS];
  static uint32_t code[32] __attribute__((aligned(128)));
  char det[768];
  xts_json_t j;
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  switch ((int)c->arg[0]) {
  case C_ROLES: {
    if (ctx->host) { skip(ctx, "host: no hardware threads", "result", ""); return 0; }
    uint64_t remote = 99, here = xts_mfspr64(1023);
    xts_core_xc_call(pir_job, &remote, NULL);
    xj_kv_uint(&j, "suite_pir", here);
    xj_kv_uint(&j, "test_pir", remote);
    xj_end(&j);
    emit(ctx, here == XTS_PIR_SUITE && remote == XTS_PIR_TEST ? XTS_ST_PASS : XTS_ST_FAIL, "result", 0, "", &j);
    return 0;
  }
  case C_PING: {
    for (int i = 0; i < PINGS; i++) {
      uint64_t t0 = xts_tb();
      xts_core_xc_call(nop_job, NULL, NULL);
      uint64_t dt = xts_tb() - t0;
      lat[i] = dt > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)dt;
    }
    qsort(lat, PINGS, sizeof lat[0], cmp_u32);
    uint64_t sum = 0;
    for (int i = 0; i < PINGS; i++) sum += lat[i];
    xj_kv_int(&j, "calls", PINGS);
    xj_kv_uint(&j, "min_ticks", lat[0]);
    xj_kv_uint(&j, "median_ticks", lat[PINGS / 2]);
    xj_kv_uint(&j, "p99_ticks", lat[PINGS * 99 / 100]);
    xj_kv_uint(&j, "max_ticks", lat[PINGS - 1]);
    xj_kv_double(&j, "mean_ticks", (double)sum / PINGS);
    xj_kv_double(&j, "median_us", (double)lat[PINGS / 2] * 1e6 / (double)xts_tb_hz());
    xj_end(&j);
    xts_record_t r;
    xts_record_init(&r);
    r.metric = "latency";
    r.unit = "ticks";
    r.value = lat[PINGS / 2];
    r.n = PINGS;
    r.min = lat[0];
    r.median = lat[PINGS / 2];
    r.status = XTS_ST_PASS;
    r.detail = xj_finish(&j);
    r.detail_truncated = j.truncated;
    xts_emit(ctx, &r);
    return 0;
  }
  case C_HB: {
    if (ctx->host) { skip(ctx, "host: no hardware threads", "result", ""); return 0; }
    static const int pirs[3] = { XTS_PIR_TEST, XTS_PIR_SUITE, XTS_PIR_NET };
    uint64_t a[3], b[3];
    for (int i = 0; i < 3; i++) a[i] = xts_xc_heartbeat(pirs[i]);
    xts_delay_ms(20);
    xts_core_xc_call(nop_job, NULL, NULL);     /* the suite's own heartbeat moves with its calls */
    int ok = 1;
    xj_key(&j, "heartbeats");
    xj_obj(&j);
    for (int i = 0; i < 3; i++) {
      b[i] = xts_xc_heartbeat(pirs[i]);
      char k[8];
      snprintf(k, sizeof k, "%d", pirs[i]);
      xj_kv_uint(&j, k, b[i] - a[i]);
      if (b[i] == a[i]) ok = 0;
    }
    xj_end(&j);
    xj_kv_bool(&j, "measuring", xts_xc_measuring());
    xj_end(&j);
    emit(ctx, ok ? XTS_ST_PASS : XTS_ST_FAIL, "result", 0, "", &j);
    return 0;
  }
  case C_SYNC: {
    if (ctx->host) { skip(ctx, "host: generated code does not execute on the PC", "count", "count"); return 0; }
    int bad = 0, first_bad = -1;
    for (int k = 0; k < SYNCS; k++) {
      uint32_t want = (uint32_t)(k * 37 + 1) & 0x7FFF, got = 0xFFFFFFFFu;
      code[0] = 0x38600000u | want;            /* li r3, want */
      code[1] = 0x4E800020u;                   /* blr */
      xts_sync_code(code, 8);
      xts_core_xc_call(code_job, code, &got);
      if (got != want) { bad++; if (first_bad < 0) first_bad = k; }
    }
    xj_kv_int(&j, "rounds", SYNCS);
    xj_kv_int(&j, "stale", bad);
    xj_kv_int(&j, "first_stale", first_bad);
    xj_end(&j);
    emit(ctx, bad ? XTS_ST_FAIL : XTS_ST_PASS, "count", bad, "count", &j);
    return 0;
  }
  case C_DSYNC: {
    int mode = (int)c->arg[1];
    int must_pass = D_STAGED(mode);
    if (ctx->host) { skip(ctx, "host: no second core", "count", "count"); return 0; }
    static dsync_job_t dj __attribute__((aligned(128)));
    uint32_t xfn = 0;
    if (mode == D_C2T_XDCBT || mode == D_C2T_XDCBT_STAGED) {
      xd_code[0] = xts_enc(XI_xdcbt, 0, 0, 3);
      xd_code[1] = 0x4E800020u;                /* blr */
      xts_sync_code(xd_code, 8);
      xfn = (uint32_t)(uintptr_t)xd_code;
    }
    xts_core_xc_call(dflush_job, NULL, NULL);  /* start clean on the test core */
    int bad = 0, first_bad = -1;
    uint32_t words = 0;
    for (uint32_t k = 1; k <= SYNCS; k++) {
      uint32_t stale = 0;
      if (D_IS_C2T(mode)) {
        for (uint32_t i = 0; i < DSYNC_WORDS; i++) ((volatile uint32_t *)dbuf)[i] = dpat(k, i);
        dflush();                              /* what the exec engine does with scratch on the suite core */
        memset(&dj, 0, sizeof dj);
        dj.round = k;
        dj.mode = (uint32_t)mode;
        dj.xdcbt_fn = xfn;
        __sync_synchronize();
        xts_core_xc_call(dsync_c2t_job, &dj, NULL);
        dflush();                              /* dj itself: written on the test core */
        stale = dj.stale;
      } else {
        const volatile uint32_t *p = dbuf;
        uint32_t warm = 0;
        for (uint32_t i = 0; i < DSYNC_WORDS; i++) warm += p[i];   /* the suite holds the lines (as after a result read) */
        dj.round = k;
        dj.sink = warm;
        __sync_synchronize();
        xts_core_xc_call(dsync_t2c_job, &dj, NULL);
        if (mode == D_T2C_STAGED) dflush();
        for (uint32_t i = 0; i < DSYNC_WORDS; i++) stale += p[i] != dpat(k, i);
      }
      if (stale) { bad++; words += stale; if (first_bad < 0) first_bad = (int)k; }
    }
    xts_core_xc_call(dflush_job, NULL, NULL);  /* leave no line of dbuf behind in the test core's L1 */
    xj_kv_str(&j, "how", dsync_names[mode]);
    xj_kv_int(&j, "rounds", SYNCS);
    xj_kv_int(&j, "stale_rounds", bad);
    xj_kv_uint(&j, "stale_words", words);
    xj_kv_int(&j, "first_stale", first_bad);
    xj_kv_bool(&j, "must_be_zero", must_pass);
    xj_end(&j);
    emit(ctx, must_pass ? (bad ? XTS_ST_FAIL : XTS_ST_PASS) : XTS_ST_INFO, "count", bad, "count", &j);
    return 0;
  }
  case C_IRQ: {
    int ee = (int)c->arg[1];
    if (ctx->host) { skip(ctx, "host: no test core", "count", "count"); return 0; }
    static uint32_t srt[IRQ_RUNS];
    xts_core_xc_call(irq_job, (void *)(uintptr_t)ee, NULL);
    __sync_synchronize();
    memcpy(srt, irq_dt, sizeof srt);
    qsort(srt, IRQ_RUNS, sizeof srt[0], cmp_u32);
    uint32_t mn = srt[0], med = srt[IRQ_RUNS / 2], thr = mn + 8;   /* 8 ticks = 512 cycles above the fastest run */
    int out = 0;
    uint64_t extra = 0;
    for (int k = 0; k < IRQ_RUNS; k++)
      if (irq_dt[k] > thr) { out++; extra += irq_dt[k] - mn; }
    xj_kv_int(&j, "ee", ee);
    xj_kv_int(&j, "runs", IRQ_RUNS);
    xj_kv_uint(&j, "loop_iters", IRQ_LOOP);
    xj_kv_uint(&j, "min_ticks", mn);
    xj_kv_uint(&j, "median_ticks", med);
    xj_kv_uint(&j, "max_ticks", srt[IRQ_RUNS - 1]);
    xj_kv_int(&j, "outlier_runs", out);
    xj_kv_double(&j, "outlier_mean_extra_ticks", out ? (double)extra / out : 0);
    xj_kv_double(&j, "outliers_per_ms", out * (double)xts_tb_hz() / ((double)med * IRQ_RUNS * 1000.0));
    xj_end(&j);
    emit(ctx, XTS_ST_INFO, "count", out, "count", &j);
    return 0;
  }
  case C_SMT: {
    int pir = (int)c->arg[1];
    int r = xts_smt_start(pir, XTS_SMT_BUSY_INT, NULL, 0);
    uint32_t it = 0;
    int stop = 0, chk = 0;
    if (r == 0) {
      uint64_t t0 = xts_tb();
      while (xts_tb() - t0 < xts_tb_hz() / 200 && !(it = xts_smt_iters(pir))) smtb_relax(pir);   /* <= 5 ms */
      stop = xts_smt_stop(pir);
      chk = xts_smt_exit_check(pir);
    }
    xj_kv_int(&j, "pir", pir);
    xj_kv_str(&j, "start", xts_smt_err_name(r));
    xj_kv_uint(&j, "iters", it);
    xj_kv_str(&j, "stop", r == 0 ? xts_smt_err_name(stop) : "-");
    xj_kv_int(&j, "exit_check", chk);
    xj_kv_hex(&j, "exit_why", xts_smt_tcbs[pir].exit_why, 4);
    xj_kv_str(&j, "state_after", xts_smt_state_name(xts_smt_state(pir)));
    xj_end(&j);
    int ok = r == 0 && stop == 0 && chk == 1 && (it > 0 || ctx->host);
    emit(ctx, ok ? XTS_ST_PASS : XTS_ST_FAIL, "result", 0, "", &j);
    return 0;
  }
  }
  return -1;
}

XTS_FAMILY(self_cores, .kind = "self", .enumerate = cores_enum, .run = cores_run, .block_cases = 16,
           .desc = "core runtime: role PIRs, remote-call latency, heartbeats, cross-core code and data sync, SMT exit self-check");
