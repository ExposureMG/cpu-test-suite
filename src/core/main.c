/* Run driver: identity, HELLO, the block loop, END. Runs on the suite core (PIR 2); the test core (PIR 0) executes only
 * what the exec layer hands it through xts_xc_call (xts/xcore.h). Nothing is stored on the console: every record, log
 * line, breadcrumb and report is a message of the UDP result transport (xts/net.h, docs/PROTOCOL.md).
 *
 *   run start:   registry, plan -> HELLO x3 (sysinfo, profile, plan counts, core map, quarantine list) -> buffered log lines
 *   block:       axes applied -> per case: quarantine check (skip record) -> CRUMB (every case for self/semantic/test
 *                families, every xts_crumb_every_timing-th otherwise) confirmed on the wire (xts_net_wait_sent) -> the
 *                case runs (records stream out as REC) -> axes reverted -> crash records of other hardware threads (their
 *                crash slots) -> BLOCK {block, records, temps, ms}
 *   run end:     END x3 {status complete|aborted, per-type counts, last_seq, rec_hash} -> drain
 *   test core lost (xts_xc_call: XTS_XC_TIMEOUT / XTS_XC_CRASHED, reported by the exec layer through
 *                xts_core_test_core_lost): a hang/crash record for the current case (exact: the RAM breadcrumb) ->
 *                HANG/CRASH report -> BLOCK -> END x3 (status hang|crashed) -> drain -> halt. No resume: re-run with the
 *                case in quarantine.txt (tools/xts_recv.py --quarantine-out suggests the line).
 *
 * run_id = the run nonce (16 hex digits, xts_net_run_nonce); console_tag = xts_plat_default_tag. The receiver files the
 * run under <out>/<console_tag>/<run_id>/.
 */
#include <xts/timing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"
#include "quarantine.h"   /* generated from quarantine.txt: xts_quarantine_builtin[], XTS_QUARANTINE_N */

#ifndef XTS_CRUMB_EVERY_TIMING
#define XTS_CRUMB_EVERY_TIMING 16
#endif
#define CRUMB_WAIT_US 600000u     /* a CRUMB must be on the wire before its case runs (a wake window lasts <= 500 ms) */

const char *xts_active_profile_name;
const char *xts_current_run_id;
xts_dry_stats_t xts_dry_stats;
int xts_host_crash_sim;
int xts_crumb_every_timing = XTS_CRUMB_EVERY_TIMING;

static char run_id[40], tag[64];
static xts_plan_t plan;
static uint8_t *plan_ran;      /* per block: 0 not run, 1 ran, 2 skipped (axis/setup) */
static uint8_t *plan_quar;     /* per block: 1 if a case was skipped by the quarantine list */
static const xts_profile_t *active_prof;
static uint32_t cur_timeout_us = 2000000u;
static uint64_t crumbs_sent, crumb_wait_fail, crumb_wait_ticks, crumb_wait_max;
static uint64_t thread_tb_seen[6];

int xts_profile_names_family(const char *family)
{
  for (int i = 0; active_prof && i < active_prof->nrows; i++)
    if (active_prof->rows[i].families && !strcmp(active_prof->rows[i].families, family)) return 1;
  return 0;
}

static int crumb_every(const char *kind)
{
  if (!kind || !strcmp(kind, "self") || !strcmp(kind, "semantic") || !strcmp(kind, "test")) return 1;
  if (!strcmp(kind, "probe")) return 1;                /* probes look for the case that stops the console */
  return xts_crumb_every_timing;
}
static int crumb_hooked(const char *kind) { return kind && (!strcmp(kind, "self") || !strcmp(kind, "semantic")); }

/* how long one xts_xc_call may take before the test core counts as hung, by family kind */
static uint32_t timeout_for(const char *k)
{
  if (!k || !strcmp(k, "self") || !strcmp(k, "semantic") || !strcmp(k, "test")) return 2000000u;
  if (!strcmp(k, "mem") || !strcmp(k, "smt") || !strcmp(k, "probe")) return 60000000u;
  return 10000000u;
}
uint32_t xts_core_xc_timeout_us(void) { return cur_timeout_us; }

/* ---- quarantine ---- */
static const xts_quar_t *quar = xts_quarantine_builtin;
static int nquar = XTS_QUARANTINE_N;
void xts_quarantine_set(const xts_quar_t *list, int n)
{
  quar = list ? list : xts_quarantine_builtin;
  nquar = list ? n : XTS_QUARANTINE_N;
}
int xts_quarantined(const char *family, const char *case_id)
{
  for (int i = 0; i < nquar; i++)
    if (!strcmp(quar[i].family, family) && (!strcmp(quar[i].case_id, case_id) || !strcmp(quar[i].case_id, "*"))) return 1;
  return 0;
}
static int family_quarantined(const char *family)
{
  for (int i = 0; i < nquar; i++)
    if (!strcmp(quar[i].family, family)) return 1;
  return 0;
}

static void log_line(const char *s) { xts_log("%s", s); }

static void sanitize_tag(char *s)
{
  for (char *p = s; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.'))
      *p = '_';
}

/* ---- breadcrumbs ---- */
static void crumb_send(void)
{
  char b[1024];
  xts_json_t j;
  xj_init(&j, b, sizeof b);
  xts_crumb_json(&j);
  xj_finish(&j);
  uint32_t s = xts_core_send(XTS_PK_CRUMB, b, (uint32_t)j.len);
  if (!s) return;
  crumbs_sent++;
  uint64_t t0 = xts_tb();
  int r = xts_net_wait_sent(s, CRUMB_WAIT_US);
  uint64_t dt = xts_tb() - t0;
  crumb_wait_ticks += dt;
  if (dt > crumb_wait_max) crumb_wait_max = dt;
  if (r < 0 && ++crumb_wait_fail <= 3)
    xts_log("breadcrumb: CRUMB seq %u not confirmed sent within %u ms (case %s)", (unsigned)s, CRUMB_WAIT_US / 1000u,
            xts_crumb.case_id);
}

static double ticks_ms(uint64_t t) { return (double)t * 1000.0 / (double)xts_tb_hz(); }

/* ---- test core lost ---- */
static char lost_det[4096], lost_rep[30000];
static volatile int lost_once;

void xts_core_test_core_lost(int reason)
{
  if (lost_once++) xts_plat_halt("XTS: test core lost again while reporting the first loss");
  int crashed = reason == XTS_XC_CRASHED;
  int status = crashed ? XTS_ST_CRASH : XTS_ST_HANG;
  const xts_xc_crash_t *slot = xts_xc_crash_slot(XTS_PIR_TEST);
  int n = crashed ? xts_crash_report_json(lost_rep, sizeof lost_rep, slot)
                  : xts_hang_report_json(lost_rep, sizeof lost_rep, cur_timeout_us);
  const xts_crumb_t *c = &xts_crumb;
  int in_case = c->phase == XTS_PH_CASE;
  const char *fam = c->family[0] ? c->family : "core";
  const char *cid = in_case ? c->case_id : "*";
  const char *block = c->block[0] ? c->block : "-";
  xts_screen_error("XTS: test core %s (%s) in block %s case %s%s", crashed ? "CRASHED" : reason == XTS_XC_TIMEOUT ? "HUNG" : "LOST",
                   reason == XTS_XC_TIMEOUT ? "no completion within the timeout" : crashed ? "crash handler fired" : "down",
                   block, cid, crashed && slot && slot->valid ? "" : "");
  if (xts_sink_is_open()) {
    xts_json_t j;
    xj_init(&j, lost_det, sizeof lost_det);
    xj_obj(&j);
    xj_kv_str(&j, "reason", crashed ? "unhandled exception on the test core (CRASH report)"
                            : reason == XTS_XC_TIMEOUT ? "the test core did not complete within the timeout (HANG report)"
                                                       : "the test core was already down");
    xj_kv_str(&j, "case_from", in_case ? "breadcrumb (RAM, exact)" : "breadcrumb: no case was running");
    xj_kv_str(&j, "phase", xts_phase_name(c->phase));
    if (in_case) {
      xj_kv_int(&j, "case_index", c->case_index);
      xj_kv_int(&j, "repeat", c->repeat);
    }
    if (crashed && slot && slot->valid) {
      xj_kv_uint(&j, "pir", slot->pir);
      xj_kv_hex(&j, "vector", slot->vector, 4);
      xj_kv_hex(&j, "srr0", slot->srr0, 16);
      xj_kv_hex(&j, "dar", slot->dar, 16);
      xj_kv_hex(&j, "lr", slot->lr, 16);
    }
    if (!crashed) xj_kv_uint(&j, "timeout_us", cur_timeout_us);
    if (c->exec_valid) xj_kv_hex(&j, "last_word", c->word, 8);
    xj_kv_str(&j, "report", crashed ? "CRASH message (crash.json)" : "HANG message (crash.json)");
    xj_kv_str(&j, "action", "run stopped; add the case to quarantine.txt to skip it (xts_recv.py --quarantine-out)");
    xj_finish(&j);
    xts_sink_emit_core(fam, cid, block, xts_sink_block_cv(), status, "result", lost_det);
  }
  xts_core_send(crashed ? XTS_PK_CRASH : XTS_PK_HANG, lost_rep, (uint32_t)n);
  if (xts_sink_is_open()) {
    xts_sink_block_end(block, "{\"aborted\":true}");
    xts_log("run %s stopped: test core %s", run_id, crashed ? "crashed" : "hung");
    xts_sink_end(crashed ? "crashed" : "hang");
  } else {
    xts_net_drain(5u * 1000u * 1000u);
  }
  const uint32_t *sc = xts_sink_status_counts();
  xts_status_line(1, "XTS %s STOPPED: test core %s in %s case %s | pass %u fail %u | report sent (%s)",
                  xts_active_profile_name ? xts_active_profile_name : "", crashed ? "crash" : "hang", block, cid,
                  (unsigned)sc[XTS_ST_PASS], (unsigned)sc[XTS_ST_FAIL], crashed ? "CRASH" : "HANG");
  xts_plat_halt(crashed ? "test core crashed: CRASH report and END sent; add the case to quarantine.txt to skip it"
                        : "test core hung: HANG report and END sent; add the case to quarantine.txt to skip it");
}

int xts_core_xc_call(void (*fn)(void *, void *), void *a, void *b)
{
  int r = xts_xc_call(fn, a, b, cur_timeout_us);
  if (r != XTS_XC_OK) xts_core_test_core_lost(r);
  return XTS_XC_OK;
}

/* ---- block helpers ---- */
static void end_hooks(const char *key)
{
  int left = xts_exec_hook_count();
  if (left) xts_log("block %s: %d exec hook(s) still registered after the axes were reverted: removed", key, left);
  xts_exec_hook_clear();
}

/* other hardware threads (sibling / other-cores loads) that crashed during this block: their crash slots -> records */
static void thread_records(const xts_block_t *b, const xts_condvec_t *cv, const char *fname)
{
  static const int pirs[] = { XTS_PIR_TEST_SIBLING, XTS_PIR_SUITE_SPARE, XTS_PIR_NET_SPARE };
  for (int i = 0; i < 3; i++) {
    const xts_xc_crash_t *t = xts_xc_crash_slot(pirs[i]);
    if (!t || !t->valid || t->tb == thread_tb_seen[pirs[i]]) continue;
    thread_tb_seen[pirs[i]] = t->tb;
    char det[1024], cid[16];
    xts_json_t j;
    xj_init(&j, det, sizeof det);
    xj_obj(&j);
    xj_kv_str(&j, "reason", "unhandled exception on another hardware thread (its crash slot, reported at the block end)");
    xj_kv_uint(&j, "pir", t->pir);
    xj_kv_hex(&j, "vector", t->vector, 4);
    xj_kv_hex(&j, "srr0", t->srr0, 16);
    xj_kv_hex(&j, "srr1", t->srr1, 16);
    xj_kv_hex(&j, "dar", t->dar, 16);
    xj_kv_hex(&j, "lr", t->lr, 16);
    xj_kv_hex(&j, "ctr", t->ctr, 16);
    xj_kv_hex(&j, "r1", t->gpr[1], 16);
    xj_kv_hex(&j, "r3", t->gpr[3], 16);
    xj_kv_uint(&j, "tb", t->tb);
    xj_finish(&j);
    snprintf(cid, sizeof cid, "pir%d", pirs[i]);
    xts_sink_emit_core(fname, cid, b->key, cv, XTS_ST_CRASH, "result", det);
    xts_screen_error("PIR %d crashed (vector %X srr0 %08llX) during block %s", pirs[i], (unsigned)t->vector,
                     (unsigned long long)t->srr0, b->key);
  }
}

static void block_end(const xts_block_t *b, const xts_condvec_t *cv, const char *fname, uint64_t t0)
{
  thread_records(b, cv, fname);
  char extra[256];
  int o = snprintf(extra, sizeof extra, "{\"family\":\"%s\",\"ms\":%.3f", fname, ticks_ms(xts_tb() - t0));
  uint16_t t[4];
  if (xts_smc_temps(t) == 0 && o < (int)sizeof extra)
    o += snprintf(extra + o, sizeof extra - o, ",\"temps\":{\"cpu\":%.2f,\"gpu\":%.2f,\"edram\":%.2f,\"board\":%.2f}",
                  t[0] / 256.0, t[1] / 256.0, t[2] / 256.0, t[3] / 256.0);
  if (o < (int)sizeof extra - 1) snprintf(extra + o, sizeof extra - o, "}");
  xts_sink_block_end(b->key, extra);
  xts_crumb_phase(XTS_PH_IDLE);
}

static int family_setup(const xts_block_t *b, int *fam_setup, const xts_family_t *f, const char *fname)
{
  if (fam_setup[b->fam] == 0) {
    fam_setup[b->fam] = (f->setup && f->setup() < 0) ? -1 : 1;
    if (fam_setup[b->fam] < 0) xts_log("family %s: setup() failed, its blocks are skipped", fname);
  }
  return fam_setup[b->fam];
}

/* the family's finish() pseudo-block (not case execution: it reads the in-RAM results back) */
static void run_finish(int bi, int *fam_setup)
{
  xts_block_t *b = &plan.b[bi];
  const xts_family_t *f = xts_family_get(b->fam);
  const char *fname = xts_family_name(f);
  xts_condvec_t cv;
  uint64_t t0 = xts_tb();
  xts_plan_condvec(b, &cv);
  xts_sink_set_block(b->key, &cv, fname);
  xts_crumb_block(b->key, fname, f->kind ? f->kind : "", 1);
  cur_timeout_us = timeout_for(f->kind);
  if (family_setup(b, fam_setup, f, fname) > 0) {
    xts_crumb_phase(XTS_PH_FINISH);
    crumb_send();
    xts_finish_info_t fin;
    memset(&fin, 0, sizeof fin);
    fin.row = b->row;
    for (int k = 0; k < plan.n; k++) {
      const xts_block_t *o = &plan.b[k];
      if (o->row != b->row || o->fam != b->fam || o->finish) continue;
      fin.blocks++;
      if (plan_ran[k] == 1) fin.ran_this_boot++;
      else if (plan_ran[k] == 2) fin.skipped++;
      if (plan_quar[k]) fin.quarantined++;
    }
    fin.complete = fin.ran_this_boot == fin.blocks && !fin.quarantined && !fin.skipped;
    static xts_case_t fcase;
    memset(&fcase, 0, sizeof fcase);
    snprintf(fcase.id, sizeof fcase.id, "finish");
    fcase.insn = -1;
    xts_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.fam = f;
    ctx.c = &fcase;
    ctx.case_index = -1;
    ctx.cond = &cv;
    ctx.seed = xts_case_seed(b, &fcase, 0);
    xts_rng_seed(&ctx.rng, ctx.seed);
    ctx.block_key = b->key;
    ctx.host = xts_plat_is_host();
    ctx.fin = &fin;
    int rc = f->finish(&ctx);
    if (rc < 0 && ctx.nrecords == 0) xts_emit_status(&ctx, XTS_ST_FAIL, "{\"reason\":\"finish() returned an error\"}");
  }
  xts_crumb_phase(XTS_PH_BLOCK_END);
  xts_env_reset();
  end_hooks(b->key);
  block_end(b, &cv, fname, t0);
}

static void run_block(int bi, int *fam_setup)
{
  xts_block_t *b = &plan.b[bi];
  const xts_family_t *f = xts_family_get(b->fam);
  const char *fname = xts_family_name(f);
  const char *kind = f->kind ? f->kind : "";
  uint64_t t0 = xts_tb();
  xts_condvec_t cv;
  xts_plan_condvec(b, &cv);
  xts_sink_set_block(b->key, &cv, fname);
  int every = crumb_every(kind);
  xts_crumb_block(b->key, fname, kind, every);
  cur_timeout_us = timeout_for(kind);
  int applied[XTS_MAX_AXES], na = 0, skip = family_setup(b, fam_setup, f, fname) < 0;
  char why[160] = "family setup failed";
  for (int a = 0; a < cv.n && !skip; a++) {
    if (!(b->applied & (1u << a))) continue;
    const xts_axis_t *ax = cv.axis[a];
    if (ax->apply && ax->apply(b->value[a]) < 0) {
      snprintf(why, sizeof why, "axis %s=%s cannot be applied", ax->name, ax->values[b->value[a]]);
      skip = 1;
      break;
    }
    applied[na++] = a;
  }
  plan_ran[bi] = skip ? 2 : 1;
  if (skip) {
    char d[256];
    snprintf(d, sizeof d, "{\"reason\":\"%s\"}", why);
    xts_sink_emit_core(fname, "*", b->key, &cv, XTS_ST_SKIP, "result", d);
  } else {
    int hooked = crumb_hooked(kind) && xts_crumb_hook_install() == 0;
    int fq = family_quarantined(fname);
    for (int kk = 0; kk < b->ncases; kk++) {
      int ci = xts_block_case(b, kk);
      const xts_case_t *c = &plan.cases[b->fam].v[ci];
      if (fq && xts_quarantined(fname, c->id)) {
        xts_sink_emit_core(fname, c->id, b->key, &cv, XTS_ST_SKIP, "result",
                           "{\"reason\":\"quarantined (quarantine.txt): crashed or hung the test core in an earlier run\"}");
        plan_quar[bi] = 1;
        continue;
      }
      const char *mnem = c->insn >= 0 && c->insn < xts_isa_count ? xts_isa[c->insn].mnem : "";
      xts_crumb_case(kk, ci, 0, c->id, mnem, xts_sink_seq(), xts_case_seed(b, c, 0));
      if (xts_crumb.ncase == 0 || (every > 0 && xts_crumb.ncase % (uint32_t)every == 0)) crumb_send();
      xts_crumb.ncase++;
      for (int rep = 0; rep < b->repeats; rep++) {
        xts_ctx_t ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.fam = f;
        ctx.c = c;
        ctx.case_index = ci;
        ctx.cond = &cv;
        ctx.repeat = rep;
        ctx.seed = xts_case_seed(b, c, rep);
        xts_rng_seed(&ctx.rng, ctx.seed);
        ctx.block_key = b->key;
        ctx.host = xts_plat_is_host();
        xts_crumb.repeat = rep;
        xts_crumb.seed = ctx.seed;
        int rc = f->run(c, &ctx);
        if (rc < 0 && ctx.nrecords == 0) xts_emit_status(&ctx, XTS_ST_FAIL, "{\"reason\":\"run() returned an error\"}");
      }
      xts_crumb_phase(XTS_PH_BETWEEN);
    }
    xts_crumb_phase(XTS_PH_BLOCK_END);
    if (hooked) xts_crumb_hook_remove();
  }
  for (int k = na - 1; k >= 0; k--) {
    const xts_axis_t *ax = cv.axis[applied[k]];
    if (ax->revert) ax->revert(b->value[applied[k]]);
  }
  xts_env_reset();                                     /* env back to defaults between blocks */
  end_hooks(b->key);                                   /* chained hooks + the v1 slot */
  block_end(b, &cv, fname, t0);
}

/* HELLO: sysinfo + profile + plan + core map + quarantine list */
static int hello_json(char *buf, int cap)
{
  xts_json_t j;
  xj_init(&j, buf, cap);
  xj_obj(&j);
  xj_key(&j, "sysinfo");
  xj_obj(&j);
  xts_sysinfo_json(&j, run_id, tag);
  xj_end(&j);
  xj_kv_str(&j, "profile", active_prof->name);
  xj_kv_str(&j, "profile_desc", active_prof->desc ? active_prof->desc : "");
  xj_key(&j, "plan");
  xj_obj(&j);
  xj_kv_int(&j, "blocks", plan.n);
  xj_kv_int(&j, "cases", plan.total_cases);
  xj_kv_int(&j, "families", xts_family_count());
  xj_end(&j);
  xj_key(&j, "cores");
  xj_obj(&j);
  xj_kv_int(&j, "test", XTS_PIR_TEST);
  xj_kv_int(&j, "test_sibling", XTS_PIR_TEST_SIBLING);
  xj_kv_int(&j, "suite", XTS_PIR_SUITE);
  xj_kv_int(&j, "suite_spare", XTS_PIR_SUITE_SPARE);
  xj_kv_int(&j, "net", XTS_PIR_NET);
  xj_kv_int(&j, "net_spare", XTS_PIR_NET_SPARE);
  xj_end(&j);
  xj_kv_str(&j, "build_id", xts_build_id());
  xj_kv_str(&j, "run_id", run_id);
  xj_kv_str(&j, "console_tag", tag);
  xj_kv_int(&j, "crumb_every_timing", xts_crumb_every_timing);
  xj_key(&j, "quarantine");
  xj_arr(&j);
  for (int i = 0; i < nquar; i++) {
    xj_arr(&j);
    xj_str(&j, quar[i].family);
    xj_str(&j, quar[i].case_id);
    xj_end(&j);
  }
  xj_end(&j);
  xj_finish(&j);
  if (j.truncated) xts_log("HELLO truncated at %d bytes", cap);
  return j.len;
}

int xts_run(const xts_profile_t *prof, const xts_run_opts_t *o)
{
  static const xts_run_opts_t defaults;
  static char hello[30000];
  if (!o) o = &defaults;
  xts_active_profile_name = prof->name;
  active_prof = prof;
  memset(&xts_crumb, 0, sizeof xts_crumb);
  memset(thread_tb_seen, 0, sizeof thread_tb_seen);
  crumbs_sent = crumb_wait_fail = crumb_wait_ticks = crumb_wait_max = 0;
  lost_once = 0;
  int probs = xts_registry_init(log_line);
  if (xts_exec_init() < 0) xts_screen_error("exec: vector install failed (traps inside cases will crash)");

  xts_plat_default_tag(tag, sizeof tag);
  sanitize_tag(tag);
  snprintf(run_id, sizeof run_id, "%016llx", (unsigned long long)xts_net_run_nonce());
  xts_current_run_id = run_id;
  xts_sink_open(run_id, tag);
  for (int i = 0; i < 6; i++) {                        /* thread crashes from before the run are not this run's */
    const xts_xc_crash_t *t = xts_xc_crash_slot(i);
    if (t && t->valid) thread_tb_seen[i] = t->tb;
  }
  if (xts_plan_build(&plan, prof) < 0) {
    xts_screen_error("scheduler: out of memory");
    hello_json(hello, sizeof hello);
    xts_sink_hello(hello);
    xts_sink_end("aborted");
    xts_sink_close();
    return -1;
  }
  plan_ran = calloc(plan.n + 1, 1);
  plan_quar = calloc(plan.n + 1, 1);
  xts_sink_hello(hello_json(hello, sizeof hello) > 0 ? hello : "{}");
  xts_log("XTS %s profile %s run %s tag %s build %s", XTS_VERSION, prof->name, run_id, tag, xts_build_id());
  xts_log("transport: UDP messages (docs/PROTOCOL.md); breadcrumbs before every case (self/semantic families), every %d case(s) "
          "otherwise; %d quarantined case(s) compiled in", xts_crumb_every_timing, nquar);
  if (probs) xts_screen_error("registry: %d problem(s), see run.log", probs);
  xts_log("plan: %d blocks, %d case runs, %d families, %d axes", plan.n, plan.total_cases, xts_family_count(), xts_axis_count());

  int *fam_setup = calloc(xts_family_count() + 1, sizeof(int));
  int *fam_last = calloc(xts_family_count() + 1, sizeof(int));
  for (int i = 0; i < plan.n; i++) fam_last[plan.b[i].fam] = i;
  int done = 0, filtered = 0;
  for (int i = 0; i < plan.n; i++) {
    if (o->only_family && strcmp(o->only_family, xts_family_name(xts_family_get(plan.b[i].fam)))) { filtered++; continue; }
    if (o->max_blocks && done >= o->max_blocks) break;
    const xts_family_t *f = xts_family_get(plan.b[i].fam);
    const uint32_t *sc = xts_sink_status_counts();
    xts_status_line(0, "XTS %s block %d/%d %s | pass %u fail %u skip %u | sent %llu msgs, %u failed", prof->name, i + 1, plan.n,
                    xts_family_name(f), (unsigned)sc[XTS_ST_PASS], (unsigned)sc[XTS_ST_FAIL], (unsigned)sc[XTS_ST_SKIP],
                    (unsigned long long)(xts_sink_msg_count(XTS_PK_REC) + xts_sink_msg_count(XTS_PK_LOG)),
                    (unsigned)xts_sink_send_failures());
    if (plan.b[i].finish) run_finish(i, fam_setup);
    else run_block(i, fam_setup);
    done++;
    if (fam_last[plan.b[i].fam] == i && f->teardown && fam_setup[plan.b[i].fam] > 0) f->teardown();
  }
  xts_crumb_phase(XTS_PH_RUN_END);
  int finished = done + filtered == plan.n;
  uint64_t rb, rl, rd;
  int rev;
  xts_results_stats(&rb, &rl, &rd, &rev);
  xts_log("run %s: %d of %d blocks executed%s", run_id, done, plan.n, finished ? ", run complete" : " (stopped early)");
  xts_log("breadcrumbs: %llu sent, %.1f ms waiting for the wire (max %.2f ms), %llu not confirmed",
          (unsigned long long)crumbs_sent, ticks_ms(crumb_wait_ticks), ticks_ms(crumb_wait_max),
          (unsigned long long)crumb_wait_fail);
  xts_log("results store: %llu lines, %.1f MB in RAM, %llu not kept%s", (unsigned long long)rl, (double)rb / 1048576.0,
          (unsigned long long)rd, rev ? " (families without a finish hook were evicted)" : "");
  if (o->dryrun)
    xts_log("dry run: %llu single words (%llu simulated by the model, %llu not), %llu kernels (%llu words: %llu checked "
            "instructions, %llu raw/exit), %llu bad words",
            (unsigned long long)xts_dry_stats.words, (unsigned long long)xts_dry_stats.simulated,
            (unsigned long long)xts_dry_stats.not_simulated, (unsigned long long)xts_dry_stats.kernels,
            (unsigned long long)xts_dry_stats.kernel_words, (unsigned long long)xts_dry_stats.kernel_insn_words,
            (unsigned long long)xts_dry_stats.kernel_raw_words, (unsigned long long)xts_dry_stats.bad_words);
  xts_net_stats_t ns;
  memset(&ns, 0, sizeof ns);
  xts_net_stats(&ns);
  xts_log("transport: %llu messages, %llu frames (%llu copies), %llu bytes, %llu ring-full waits, %llu send failures, "
          "%llu fence waits", (unsigned long long)ns.messages, (unsigned long long)ns.frames,
          (unsigned long long)ns.frame_copies, (unsigned long long)ns.bytes, (unsigned long long)ns.ring_full_waits,
          (unsigned long long)ns.send_failures, (unsigned long long)ns.fence_waits);
  const uint32_t *sc = xts_sink_status_counts();
  int dr = xts_sink_end(finished ? "complete" : "aborted");
  unsigned fails = xts_sink_send_failures();
  xts_status_line(1, "XTS %s done: pass %u fail %u skip %u hang %u crash %u | END sent%s%s", prof->name,
                  (unsigned)sc[XTS_ST_PASS], (unsigned)sc[XTS_ST_FAIL], (unsigned)sc[XTS_ST_SKIP], (unsigned)sc[XTS_ST_HANG],
                  (unsigned)sc[XTS_ST_CRASH], dr ? " (drain timed out)" : "", fails ? " | SOME MESSAGES NOT SENT" : "");
  xts_sink_close();
  active_prof = NULL;
  free(fam_setup);
  free(fam_last);
  free(plan_ran);
  free(plan_quar);
  plan_ran = plan_quar = NULL;
  if (fails) return -5;
  return (o->dryrun && xts_dry_stats.bad_words) ? 2 : 0;
}
