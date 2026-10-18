/* Test core lost: a crash or hang of the test core (simulated by the host xts_xc_* runtime, src/plat/xcore_host.c)
 * reaches the receiver as a crash/hang record for the exact case, a CRASH/HANG report (crash.json) and END with status
 * crashed/hang; then the run halts (exit 1). The compiled-in quarantine list skips such cases; other hardware threads'
 * crash slots become "pir<N>" crash records at the block end; self.crash is opt-in. Every run goes through the real
 * receiver (ht_recv_run). */
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include <core/core.h>
#include "htest.h"

static int nc_crash_at = -1, nc_hang_at = -1, nc_thread_at = -1;
static int nc_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < 20; i++) xts_case_addf(cl, "c%02d", i);
  return 0;
}
static void crash_fn(void *a, void *b)
{
  (void)a; (void)b;
  uint64_t g[32];
  for (int r = 0; r < 32; r++) g[r] = 0xAB00 + (uint64_t)r;
  xts_host_xc_crash(0x300, 0x80001234, g);
}
static void hang_fn(void *a, void *b) { (void)a; (void)b; xts_host_xc_hang(); }
static void ok_fn(void *a, void *b) { (*(int *)a)++; (void)b; }
static int nc_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  int n = 0;
  xts_core_xc_call(ok_fn, &n, NULL);                   /* a normal round trip to the test core */
  if (ctx->case_index == nc_crash_at) xts_core_xc_call(crash_fn, NULL, NULL);   /* does not return */
  if (ctx->case_index == nc_hang_at) xts_core_xc_call(hang_fn, NULL, NULL);     /* does not return */
  if (ctx->case_index == nc_thread_at) {
    xts_xc_crash_t t;
    memset(&t, 0, sizeof t);
    t.vector = 0x700;
    t.srr0 = 0x80007777;
    t.tb = 42;
    xts_host_xc_set_slot(3, &t);                       /* PIR 3 (other-cores load) crashed; the suite goes on */
  }
  xts_emit_value(ctx, "index", ctx->case_index, "", n == 1 ? XTS_ST_PASS : XTS_ST_FAIL);
  (void)c;
  return 0;
}
XTS_FAMILY(t_netcrash, .kind = "test", .enumerate = nc_enum, .run = nc_run, .block_cases = 5);
XTS_METRIC("index");

static const xts_profile_row_t nc_rows[] = { { .families = "t.netcrash" } };
static const xts_profile_t nc_prof = { "tnetcrash", "test core lost", nc_rows, 1 };
static const xts_profile_row_t sc_rows[] = { { .families = "t.netcrash", .max_cases = 5 }, { .families = "self.crash" } };
static const xts_profile_t sc_prof = { "tnetself", "self.crash test", sc_rows, 2 };
static const xts_profile_row_t glob_rows[] = { { .families = "self.*" } };
static const xts_profile_t glob_prof = { "tnetglob", "self.* without self.crash", glob_rows, 1 };

typedef struct { const xts_profile_t *p; int crash_at, hang_at, thread_at, sim, quar; } cfg_t;
static const xts_quar_t quar_list[] = { { "t.netcrash", "c07" }, { "t.netcrash", "c12" } };
static int child(void *ud)
{
  const cfg_t *c = ud;
  nc_crash_at = c->crash_at;
  nc_hang_at = c->hang_at;
  nc_thread_at = c->thread_at;
  xts_host_crash_sim = c->sim;
  if (c->quar) xts_quarantine_set(quar_list, 2);
  return xts_run(c->p, NULL) == 0 ? 0 : 1;
}

static const char *recv_log(const char *name)
{
  char p[700];
  snprintf(p, sizeof p, "%s/%s/recv.log", ht_tmp, name);
  return ht_slurp(p);
}

HTEST(net_crash_report)
{
  char rd[700];
  int rrc = -1;
  cfg_t c = { &nc_prof, 7, -1, -1, 0, 0 };
  HT_EQ_U64(ht_recv_run("net_crash", child, &c, NULL, rd, sizeof rd, &rrc), 1);   /* halt */
  HT_EQ_U64(rrc, 1);                                   /* complete, but END status is not "complete" */
  const char *rcv = ht_run_file(rd, "receive.json");
  HT_CHECK(rcv && strstr(rcv, "\"complete\": true") && strstr(rcv, "\"status\": \"crashed\""));
  const char *cj = ht_run_file(rd, "crash.json");
  HT_CHECK(cj && strstr(cj, "\"kind\": \"crash\"") && strstr(cj, "\"vector\": \"0300\"") &&
           strstr(cj, "\"srr0\": \"0000000080001234\"") && strstr(cj, "\"000000000000AB1F\""));
  HT_CHECK(cj && strstr(cj, "\"case_id\": \"c07\"") && strstr(cj, "\"block\": \"r0:t.netcrash:-:c1\"") &&
           strstr(cj, "\"log_tail\": ["));
  const char *fl = ht_run_file(rd, "failures.jsonl");
  HT_EQ_U64(ht_count_substr(fl, "\n"), 1);
  HT_CHECK(fl && strstr(fl, "\"case_id\":\"c07\"") && strstr(fl, "\"status\":\"crash\"") &&
           strstr(fl, "\"case_from\":\"breadcrumb (RAM, exact)\"") && strstr(fl, "\"vector\":\"0300\""));
  const char *res = ht_run_file(rd, "results.jsonl");
  HT_EQ_U64(ht_count_substr(res, "\"status\":\"pass\""), 7);
  const char *rl = recv_log("net_crash");
  HT_CHECK(rl && strstr(rl, "suggested quarantine.txt line(s):") && strstr(rl, "t.netcrash c07  # crash vector 0300"));
}

HTEST(net_hang_report)
{
  char rd[700];
  int rrc = -1;
  cfg_t c = { &nc_prof, -1, 12, -1, 0, 0 };
  HT_EQ_U64(ht_recv_run("net_hang", child, &c, NULL, rd, sizeof rd, &rrc), 1);
  const char *rcv = ht_run_file(rd, "receive.json");
  HT_CHECK(rcv && strstr(rcv, "\"complete\": true") && strstr(rcv, "\"status\": \"hang\""));
  const char *cj = ht_run_file(rd, "crash.json");
  HT_CHECK(cj && strstr(cj, "\"kind\": \"hang\"") && strstr(cj, "\"timeout_us\": 2000000") && strstr(cj, "\"case_id\": \"c12\"") &&
           strstr(cj, "\"heartbeats\""));
  const char *fl = ht_run_file(rd, "failures.jsonl");
  HT_CHECK(fl && ht_count_substr(fl, "\n") == 1 && strstr(fl, "\"case_id\":\"c12\"") && strstr(fl, "\"status\":\"hang\""));
  const char *rl = recv_log("net_hang");
  HT_CHECK(rl && strstr(rl, "t.netcrash c12  # hang (timeout 2000000 us)"));
}

/* the quarantine list skips both culprits: the run completes */
HTEST(net_quarantine_skips)
{
  char rd[700];
  int rrc = -1;
  cfg_t c = { &nc_prof, 7, 12, -1, 0, 1 };
  HT_EQ_U64(ht_recv_run("net_quar", child, &c, NULL, rd, sizeof rd, &rrc), 0);
  HT_EQ_U64(rrc, 0);
  const char *res = ht_run_file(rd, "results.jsonl");
  HT_EQ_U64(ht_count_substr(res, "\"status\":\"pass\""), 18);
  HT_EQ_U64(ht_count_substr(res, "quarantined (quarantine.txt)"), 2);
  HT_CHECK(res && strstr(res, "\"case_id\":\"c07\"") && strstr(res, "\"case_id\":\"c12\""));
  const char *si = ht_run_file(rd, "receive.json");
  HT_CHECK(si && strstr(si, "\"status\": \"complete\""));
}

/* a crash slot of another PIR filled during a block: a crash record "pir3" at that block's end, the run goes on */
HTEST(net_thread_crash_record)
{
  char rd[700];
  int rrc = -1;
  cfg_t c = { &nc_prof, -1, -1, 6, 0, 0 };
  HT_EQ_U64(ht_recv_run("net_thread", child, &c, NULL, rd, sizeof rd, &rrc), 0);
  const char *fl = ht_run_file(rd, "failures.jsonl");
  HT_CHECK(fl && ht_count_substr(fl, "\n") == 1 && strstr(fl, "\"case_id\":\"pir3\"") &&
           strstr(fl, "\"block\":\"r0:t.netcrash:-:c1\"") && strstr(fl, "\"srr0\":\"0000000080007777\""));
}

/* self.crash: opt-in (a self.* row alone gives it no cases); named in a row, its last case crashes the test core */
HTEST(net_self_crash_family)
{
  char rd[700];
  int rrc = -1;
  cfg_t g = { &glob_prof, -1, -1, -1, 1, 0 };
  HT_EQ_U64(ht_recv_run("net_selfglob", child, &g, NULL, rd, sizeof rd, &rrc), 0);
  const char *res = ht_run_file(rd, "results.jsonl");
  HT_CHECK(res && !strstr(res, "self.crash"));
  cfg_t s = { &sc_prof, -1, -1, -1, 1, 0 };
  HT_EQ_U64(ht_recv_run("net_selfcrash", child, &s, NULL, rd, sizeof rd, &rrc), 1);
  const char *cj = ht_run_file(rd, "crash.json");
  HT_CHECK(cj && strstr(cj, "\"block\": \"r1:self.crash:-:c0\"") && strstr(cj, "\"case_id\": \"crash\"") &&
           strstr(cj, "\"vector\": \"0700\""));
  res = ht_run_file(rd, "results.jsonl");
  HT_EQ_U64(ht_count_substr(res, "\"family\":\"self.crash\""), 4);   /* ok0..ok2 + the crash record */
  cfg_t n = { &sc_prof, -1, -1, -1, 0, 0 };                           /* PC dry run: the crash case is a skip */
  HT_EQ_U64(ht_recv_run("net_selfnosim", child, &n, NULL, rd, sizeof rd, &rrc), 0);
  res = ht_run_file(rd, "results.jsonl");
  HT_CHECK(res && strstr(res, "\"case_id\":\"crash\"") && ht_count_substr(res, "\"status\":\"skip\"") == 1);
}
