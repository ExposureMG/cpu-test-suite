/* self.cores - the core runtime (xts/xcore.h), checked from the suite core:
 *   roles        the suite runs on PIR 2 and a remote call lands on PIR 0 (console)
 *   pingpong     10,000 remote no-op calls (xts_core_xc_call): round-trip latency in timebase ticks, min/median/max
 *   heartbeat    the heartbeats of PIR 0 (executor), 2 (suite) and 4 (transmitter) all advance within 20 ms (console)
 *   code_sync    1,000 times: write `li r3,k; blr` on the suite core (xts_sync_code: dcbst, sync, icbi), call it on the
 *                test core (the executor does sync; isync first) and get k back - cross-core instruction-cache coherence
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

enum { C_ROLES, C_PING, C_HB, C_SYNC, C_SMT };

static int cores_enum(xts_caselist_t *cl)
{
  static const char *const names[] = { "roles", "pingpong", "heartbeat", "code_sync" };
  for (int i = 0; i < 4; i++) {
    xts_case_t *c = xts_case_add(cl, names[i]);
    if (!c) return -1;
    c->arg[0] = i;
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

static void skip(xts_ctx_t *ctx, const char *why)
{
  char det[128];
  snprintf(det, sizeof det, "{\"why\":\"%s\"}", why);
  xts_emit_status(ctx, XTS_ST_SKIP, det);
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
    if (ctx->host) { skip(ctx, "host: no hardware threads"); return 0; }
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
    if (ctx->host) { skip(ctx, "host: no hardware threads"); return 0; }
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
    if (ctx->host) { skip(ctx, "host: generated code does not execute on the PC"); return 0; }
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
           .desc = "core runtime: role PIRs, remote-call latency, heartbeats, cross-core code sync, SMT exit self-check");
