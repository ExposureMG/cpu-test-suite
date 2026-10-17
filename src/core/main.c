/* Run driver: identity, run directory, resume, the block loop, summary, clean unmount.
 *
 * Output: <root>/xts/<console_tag>/<run_id>/{sysinfo.json, run.log, results.jsonl, results.csv, failures.jsonl,
 *         temps.csv, journal.txt, summary.json, crash.json, progress.bin}
 *   console_tag = contents of <root>/xts/label.txt if present, else <type>-<pvr>-<xenosid>
 *   run_id      = counter from <root>/xts/runs.txt (+ "-<SMC RTC>" when available)
 *   <root>/xts/<tag>/current.txt names an unfinished run ("<run_id> <profile> <build_id> <boots>"): the next boot of the
 *   same ELF resumes it; a different profile/build starts a new run.
 *
 * Storage discipline (the stick is consistent whenever test code runs):
 *   block start: journal BEGIN (fsync) -> axes -> boundary (buffered output written, fsync, fatFlush) -> breadcrumb
 *                (progress.bin, raw) -> xts_cases_running = 1
 *   cases:       records/log lines stay in RAM; before a case only the raw breadcrumb is written (every case for
 *                self/semantic families, every XTS_CRUMB_EVERY_TIMING-th case otherwise); a pending crash of another
 *                hardware thread is written into crash.json (raw). Any file operation is refused (guard violation). If
 *                the buffers pass XTS_SINK_HIGH_WATER the chunk ends at the case boundary: boundary + PART line.
 *   block end:   xts_cases_running = 0 -> axes reverted -> records -> boundary -> journal END (fsync) -> fatFlush.
 *   run end:     DONE, summary.json, last log line, files closed, fatFlush + fatUnmount; only then "safe to power off".
 *
 * Storage: after the run directory is created a probe file is written, read back and removed; any failure there (or in
 * creating the directories) halts with "FAT32 (MBR) USB stick required; write test failed: <reason>" on screen.
 *
 * Resume (a BEGIN without END in the journal): the case is named by crash.json (filled by the crash path; status crash),
 * else by progress.bin (the breadcrumb of the case that was running; status hang) when breadcrumbs were exact (every
 * case) in that block. A named case is quarantined (QUAR case=k) and the block re-runs without it. A block whose case is
 * unknown is re-run ONCE with a breadcrumb before every case (RETRY), so a second hang names the case; unknown again (or
 * 4 failed attempts, or 15 quarantined cases) quarantines the whole block. finish() blocks are never retried. Then both
 * report files are re-armed (crash.json copied to crash_boot<N>.json first).
 */
#include <xts/timing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"

#ifndef XTS_WATCHDOG_MS
#define XTS_WATCHDOG_MS 120000   /* longest a block may take before the (optional) watchdog reboots the console */
#endif
#define XTS_WATCHDOG_LONG_MS 600000   /* mem and smt families: big working sets, other-core loads */

/* breadcrumb (progress.bin) before every N-th case of timing-like families (timing, sched, mem, smt, calib, ...):
 * their cases are short and many, and they execute generated kernels whose faults are trapped inside the case. Every
 * case for self/semantic families (and host test families), where wild crashes happen. 0 = block start only. */
#ifndef XTS_CRUMB_EVERY_TIMING
#define XTS_CRUMB_EVERY_TIMING 16
#endif

const char *xts_active_profile_name;
const char *xts_current_run_id;
xts_dry_stats_t xts_dry_stats;

static char run_id[40], tag[64], rundir[256];
static int boots;
static xts_journal_t jr;
static xts_plan_t plan;
static uint8_t *plan_ran;      /* per block: 0 not run this boot, 1 ran, 2 skipped (axis/setup) */
static int counts[XTS_ST_COUNT];
static const xts_profile_t *active_prof;

int xts_profile_names_family(const char *family)
{
  for (int i = 0; active_prof && i < active_prof->nrows; i++)
    if (active_prof->rows[i].families && !strcmp(active_prof->rows[i].families, family)) return 1;
  return 0;
}

static int crumb_every(const char *kind)
{
  if (!kind || !strcmp(kind, "self") || !strcmp(kind, "semantic") || !strcmp(kind, "test")) return 1;
  return XTS_CRUMB_EVERY_TIMING;
}
static int crumb_hooked(const char *kind) { return kind && (!strcmp(kind, "self") || !strcmp(kind, "semantic")); }

/* ---- storage ---- */
static void storage_halt(const char *what, const char *path)
{
  char msg[480];
  snprintf(msg, sizeof msg, "FAT32 (MBR) USB stick required; write test failed: %s %s", what, path ? path : "");
  xts_log("%s", msg);
  xts_plat_halt(msg);
}

/* write, read back and remove a probe file in the run directory */
static void storage_probe(void)
{
  char path[320], text[96], back[128];
  xts_path(path, sizeof path, rundir, "probe.tmp", NULL, NULL);
  snprintf(text, sizeof text, "XTS write probe %s %llu\n", run_id, (unsigned long long)xts_tb());
  xts_fat_busy++;
  int w = xts_write_small(path, text);
  int r = w == 0 ? xts_read_small(path, back, sizeof back) : -1;
  int rm = remove(path);
  xts_fat_busy--;
  if (xts_storage_dead()) storage_halt("storage error (see above)", path);
  if (w != 0) storage_halt("cannot write", path);
  if (r != (int)strlen(text) || strcmp(back, text)) storage_halt("read-back mismatch", path);
  if (rm != 0) storage_halt("cannot remove", path);
}

/* the run.log line with the disc guard's numbers */
static void log_storage(void)
{
  xts_storage_status_t st;
  xts_plat_storage_status(&st);
  if (!st.guard) { xts_log("storage: disc guard off (%s)", xts_plat_is_host() ? "host directory" : "pass-through"); return; }
  xts_log("storage: disc guard on: %llu sectors read (each read twice and compared, %llu retried), %llu sectors written "
          "(each read back and compared), %llu device reads, %llu device writes, %.0f ms in the guard%s%s",
          (unsigned long long)st.sectors_read, (unsigned long long)st.read_retries, (unsigned long long)st.sectors_written,
          (unsigned long long)st.device_reads, (unsigned long long)st.device_writes, st.guard_ms,
          st.poisoned ? "; POISONED: " : "", st.poisoned ? st.why : "");
}

/* the watchdog timeout for a block (time allowed between two kicks) by family kind */
static uint32_t block_timeout_ms(const xts_family_t *f)
{
  const char *k = f && f->kind ? f->kind : "";
  return (!strcmp(k, "mem") || !strcmp(k, "smt")) ? XTS_WATCHDOG_LONG_MS : XTS_WATCHDOG_MS;
}

static void log_line(const char *s) { xts_log("%s", s); }

static void sanitize_tag(char *s)
{
  s[strcspn(s, "\r\n")] = 0;
  for (char *p = s; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.'))
      *p = '_';
}

static void count_status_cb(int st) { if (st >= 0 && st < XTS_ST_COUNT) counts[st]++; }

static const xts_block_t *find_block(const char *key)
{
  for (int k = 0; key && key[0] && k < plan.n; k++)
    if (!strcmp(plan.b[k].key, key)) return &plan.b[k];
  return NULL;
}

/* a crash/hang record whose detail embeds a report (crash.json / progress.bin, squeezed) */
static char det[24000];
static void emit_report_record(const char *fam, const char *cid, const char *block, const xts_condvec_t *cv, int status,
                               xts_json_t *j, const char *report_key, const char *report)
{
  if (report && report[0]) {
    xj_key(j, report_key);
    if (strlen(report) + j->len + 64 < sizeof det) xj_raw(j, report);
    else xj_str(j, "(too long for the record: see crash_boot<N>.json in the run directory)");
  }
  xj_finish(j);
  xts_sink_emit_core(fam, cid, block, cv, status, "result", det);
  count_status_cb(status);
}

/* thread entries "thr<i>_*" of a crash.json report -> crash records "pir<N>" */
static void emit_report_threads(const xts_prev_t *pv)
{
  for (int i = 1; i <= XTS_CRASH_THREADS; i++) {
    char k[32], v[200], blk[160] = "", pir[8] = "?";
    snprintf(k, sizeof k, "thr%d_vector", i);
    if (xts_json_get(pv->crash_text, k, v, sizeof v)) continue;
    snprintf(k, sizeof k, "thr%d_block", i);
    xts_json_get(pv->crash_text, k, blk, sizeof blk);
    snprintf(k, sizeof k, "thr%d_pir", i);
    xts_json_get(pv->crash_text, k, pir, sizeof pir);
    const xts_block_t *b = find_block(blk);
    xts_condvec_t cv;
    xts_plan_condvec(b, &cv);
    char fam[64] = "core", cid[16];
    if (b) snprintf(fam, sizeof fam, "%s", xts_family_name(xts_family_get(b->fam)));
    snprintf(cid, sizeof cid, "pir%s", pir);
    xts_json_t j;
    xj_init(&j, det, sizeof det);
    xj_obj(&j);
    xj_kv_str(&j, "reason", "unhandled exception on another hardware thread (crash.json thread entry)");
    xj_kv_int(&j, "thread_entry", i);
    xj_kv_str(&j, "case_from", "crash.json");
    emit_report_record(fam, cid, blk[0] ? blk : "-", &cv, XTS_ST_CRASH, &j, "crash_report", pv->crash_text);
    xts_screen_error("previous boot: PIR %s crashed (vector %s) during block %s", pir, v, blk[0] ? blk : "?");
  }
}

/* turn a BEGIN without END into hang/crash records + QUAR lines (or one RETRY, see the top of the file) */
static void mark_open_blocks(const xts_prev_t *pv, int fresh_report)
{
  int report_used = 0;
  for (int i = 0; i < jr.n; i++) {
    xts_jentry_t *e = &jr.e[i];
    if (!e->open) continue;
    const xts_block_t *b = find_block(e->key);
    int rel = -1, status = XTS_ST_HANG, from_crash = 0, from_crumb = 0;
    const char *src = "none (no crash report, no exact breadcrumb)";
    if (fresh_report && !strcmp(pv->crash_kind, "main") && !strcmp(pv->crash_block, e->key)) {
      status = XTS_ST_CRASH;
      from_crash = 1;
      report_used = 1;
      if (b && !strcmp(pv->crash_phase, "case") && pv->crash_case_index >= 0) {
        rel = xts_block_rel(b, pv->crash_case_index);
        if (rel >= 0) src = "crash.json";
      } else {
        src = "crash.json (outside a case)";
      }
    }
    if (!from_crash && pv->crumb && !strcmp(pv->crumb_block, e->key)) {
      from_crumb = 1;
      if (b && !strcmp(pv->crumb_phase, "case") && pv->crumb_every == 1 && pv->crumb_case_index >= 0) {
        rel = xts_block_rel(b, pv->crumb_case_index);
        if (rel >= 0) src = "progress.bin (the case that was running)";
      } else if (!strcmp(pv->crumb_phase, "case")) {
        src = "progress.bin (sparse breadcrumbs: the case is at or after the one named)";
      } else {
        src = "progress.bin (no case was running)";
      }
    }
    int attempts = e->begun - e->ended;
    int giveup = attempts >= 4 || e->nquar >= 15;
    if (giveup) rel = -1;
    int retry = rel < 0 && !giveup && !e->retry && b && !b->finish;
    int ci = (b && rel >= 0) ? xts_block_case(b, rel) : -1;
    xts_json_t j;
    xj_init(&j, det, sizeof det);
    xj_obj(&j);
    xj_kv_str(&j, "reason", status == XTS_ST_CRASH ? "unhandled exception (crash.json)"
                                                   : "no END in journal and no crash report (hang, power cut, or a crash "
                                                     "whose report could not be written safely)");
    xj_kv_str(&j, "case_from", src);
    xj_kv_str(&j, "action", retry ? "block re-run once with a breadcrumb before every case (RETRY)" :
                            rel >= 0 ? "case quarantined, block re-run without it" : "whole block quarantined");
    xj_kv_int(&j, "attempts", attempts);
    if (ci >= 0) xj_kv_int(&j, "case_index", ci);
    if (from_crash) {
      xj_kv_str(&j, "vector", pv->crash_vector);
      xj_kv_str(&j, "srr0", pv->crash_srr0);
      xj_kv_str(&j, "report_outcome", pv->crash_outcome);
    }
    xts_condvec_t cv;
    xts_plan_condvec(b, &cv);
    const char *fam = b ? xts_family_name(xts_family_get(b->fam)) : "?";
    const char *cid = ci >= 0 ? plan.cases[b->fam].v[ci].id : "*";
    if (from_crash) emit_report_record(fam, cid, e->key, &cv, status, &j, "crash_report", pv->crash_text);
    else emit_report_record(fam, cid, e->key, &cv, status, &j, "breadcrumb", from_crumb ? pv->crumb_text : NULL);
    if (retry) xts_journal_retry(&jr, e->key);
    else xts_journal_quar(&jr, e->key, rel, xts_status_name(status));
    xts_screen_error("previous boot: block %s %s, case %s (%s)%s", e->key, xts_status_name(status), cid, src,
                     retry ? ": re-running it once case by case" : "");
  }
  if (fresh_report && !strcmp(pv->crash_kind, "main") && !report_used) {   /* a crash outside every open block */
    const xts_block_t *b = find_block(pv->crash_block);
    xts_condvec_t cv;
    xts_plan_condvec(b, &cv);
    xts_json_t j;
    xj_init(&j, det, sizeof det);
    xj_obj(&j);
    xj_kv_str(&j, "reason", "unhandled exception outside the block loop's open blocks (crash.json)");
    xj_kv_str(&j, "phase", pv->crash_phase);
    xj_kv_str(&j, "vector", pv->crash_vector);
    xj_kv_str(&j, "srr0", pv->crash_srr0);
    emit_report_record("core", "crash", pv->crash_block[0] ? pv->crash_block : "-", &cv, XTS_ST_CRASH, &j, "crash_report",
                       pv->crash_text);
    xts_screen_error("previous boot crashed outside a block (phase %s, vector %s)", pv->crash_phase, pv->crash_vector);
  }
  if (fresh_report) emit_report_threads(pv);
}

/* crashes of other hardware threads recorded in RAM during this block: crash records "pir<N>" now */
static int emit_thread_records(const xts_block_t *b, const xts_condvec_t *cv, const char *fname)
{
  int persisted = 0;
  for (int i = 0; i < XTS_CRASH_THREADS; i++) {
    xts_thrcrash_t *t = &xts_thrcrash[i];
    if (t->state != 1 && t->state != 2) continue;
    persisted |= t->state == 2;
    xts_json_t j;
    xj_init(&j, det, sizeof det);
    xj_obj(&j);
    xj_kv_str(&j, "reason", "unhandled exception on another hardware thread (recorded in RAM, reported at the block end)");
    xj_kv_uint(&j, "pir", t->pir);
    xj_kv_hex(&j, "vector", t->vector, 4);
    xj_kv_hex(&j, "srr0", t->srr0, 16);
    xj_kv_hex(&j, "srr1", t->srr1, 16);
    xj_kv_hex(&j, "dar", t->dar, 16);
    xj_kv_hex(&j, "lr", t->lr, 16);
    xj_kv_hex(&j, "ctr", t->ctr, 16);
    xj_kv_hex(&j, "r1", t->r1, 16);
    xj_kv_hex(&j, "r3", t->r3, 16);
    xj_kv_uint(&j, "count", t->count);
    xj_kv_str(&j, "main_block", t->block);
    xj_kv_int(&j, "main_case_index", t->case_index);
    xj_kv_str(&j, "main_case_id", t->case_id);
    xj_kv_bool(&j, "in_crash_json", t->state == 2);
    xj_finish(&j);
    char cid[16];
    snprintf(cid, sizeof cid, "pir%u", (unsigned)t->pir);
    xts_sink_emit_core(fname, cid, b->key, cv, XTS_ST_CRASH, "result", det);
    count_status_cb(XTS_ST_CRASH);
    xts_screen_error("PIR %u crashed (vector %X srr0 %08llX) during block %s", (unsigned)t->pir, (unsigned)t->vector,
                     (unsigned long long)t->srr0, b->key);
    t->state = 3;
  }
  return persisted;
}

static void end_hooks(const char *key)
{
  int left = xts_exec_hook_count();
  if (left) xts_log("block %s: %d exec hook(s) still registered after the axes were reverted: removed", key, left);
  xts_exec_hook_clear();
}

/* a breadcrumb into progress.bin (the case or phase in xts_crumb) */
static void crumb_write(void)
{
  if (!xts_slots_armed()) return;
  int r = xts_slot_crumb_write();
  if (r) xts_log("breadcrumb: progress.bin write failed (%d)", r);   /* storage errors latch: the block end stops the run */
}

/* the block end: thread records, boundary (records + log + fatFlush), END, then disarm crash.json if a thread report
 * was written into it */
static void block_end(const xts_block_t *b, const xts_condvec_t *cv, const char *fname)
{
  int persisted = emit_thread_records(b, cv, fname);
  xts_sink_flush_block(b->key);
  if (!jr.has_cpt || jr.cpt != xts_cycles_per_tick) {   /* calib changed the scale: journal it so a resume keeps it */
    char l[64];
    snprintf(l, sizeof l, "CPT %.9g", xts_cycles_per_tick);
    xts_journal_line(&jr, l);
  }
  if (persisted) xts_journal_report(&jr, xts_crash_report_tb());
  xts_journal_end(&jr, b->key, xts_sink_seq());
  if (persisted && xts_slot_crash_disarm()) xts_log("crash.json: re-arming after the thread report failed");
  xts_crumb_phase(XTS_PH_IDLE);
}

/* the family's finish() pseudo-block (not case execution: it may read results back) */
static int run_finish(int bi, int *fam_setup)
{
  xts_block_t *b = &plan.b[bi];
  const xts_family_t *f = xts_family_get(b->fam);
  const char *fname = xts_family_name(f);
  xts_condvec_t cv;
  xts_plan_condvec(b, &cv);
  xts_sink_set_block(b->key, &cv, fname);
  xts_crumb_block(run_id, boots, b->key, fname, f->kind ? f->kind : "", 1);
  if (fam_setup[b->fam] == 0) {
    fam_setup[b->fam] = (f->setup && f->setup() < 0) ? -1 : 1;
    if (fam_setup[b->fam] < 0) xts_log("family %s: setup() failed, its blocks are skipped", fname);
  }
  xts_journal_begin(&jr, b->key);
  xts_sink_boundary();
  xts_crumb_phase(XTS_PH_FINISH);
  crumb_write();
  if (fam_setup[b->fam] > 0) {
    xts_finish_info_t fin;
    memset(&fin, 0, sizeof fin);
    fin.row = b->row;
    for (int k = 0; k < plan.n; k++) {
      const xts_block_t *o = &plan.b[k];
      if (o->row != b->row || o->fam != b->fam || o->finish) continue;
      fin.blocks++;
      if (plan_ran[k] == 1) fin.ran_this_boot++;
      else if (plan_ran[k] == 2) fin.skipped++;
      xts_jentry_t *oe = xts_journal_find(&jr, o->key);
      if (oe && (oe->nquar || oe->quar_all)) fin.quarantined++;
      else if (oe && oe->ended && plan_ran[k] == 0) fin.done_earlier++;
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
  block_end(b, &cv, fname);
  xts_plat_poll();
  xts_watchdog_kick();
  return 0;
}

/* end the chunk at a case boundary: the buffers are about to outgrow RAM. Cases 0..k-1 are on disk (PART). */
static void chunk_flush(const xts_block_t *b, int k)
{
  xts_cases_running = 0;
  xts_crumb_phase(XTS_PH_BETWEEN);
  xts_log("block %s: buffer at %lu bytes: chunk written at case boundary %d (PART)", b->key,
          (unsigned long)xts_sink_pending(), k);
  xts_sink_boundary();
  xts_journal_part(&jr, b->key, k, xts_sink_seq());
  xts_cases_running = 1;
}

static int run_block(int bi, int *fam_setup)
{
  xts_block_t *b = &plan.b[bi];
  const xts_family_t *f = xts_family_get(b->fam);
  const char *fname = xts_family_name(f);
  const char *kind = f->kind ? f->kind : "";
  xts_jentry_t *e = xts_journal_find(&jr, b->key);
  xts_condvec_t cv;
  xts_plan_condvec(b, &cv);
  xts_sink_set_block(b->key, &cv, fname);
  int percase = e && e->retry;                         /* re-run of a block whose hang named no case */
  int every = percase ? 1 : crumb_every(kind);
  xts_crumb_block(run_id, boots, b->key, fname, kind, every);
  if (fam_setup[b->fam] == 0) {
    fam_setup[b->fam] = (f->setup && f->setup() < 0) ? -1 : 1;
    if (fam_setup[b->fam] < 0) xts_log("family %s: setup() failed, its blocks are skipped", fname);
  }
  xts_journal_begin(&jr, b->key);
  e = xts_journal_find(&jr, b->key);
  int applied[XTS_MAX_AXES], na = 0, skip = fam_setup[b->fam] < 0;
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
    count_status_cb(XTS_ST_SKIP);
  } else {
    int hooked = crumb_hooked(kind) && xts_crumb_hook_install() == 0;
    int start = e ? e->part_k : 0;
    if (percase) xts_log("block %s: re-run with a breadcrumb before every case", b->key);
    if (start > 0) xts_log("block %s: cases 0..%d were written before (PART): starting at case %d", b->key, start - 1, start);
    xts_sink_boundary();                               /* the stick is consistent before any case runs */
    xts_cases_running = 1;
    for (int kk = start; kk < b->ncases; kk++) {
      int ci = xts_block_case(b, kk);
      if (xts_journal_is_quar(e, kk)) continue;
      const xts_case_t *c = &plan.cases[b->fam].v[ci];
      const char *mnem = c->insn >= 0 && c->insn < xts_isa_count ? xts_isa[c->insn].mnem : "";
      xts_crumb_case(kk, ci, 0, c->id, mnem, xts_sink_seq(), xts_case_seed(b, c, 0));
      if (xts_crash_threads_pending()) xts_crash_threads_persist();   /* another thread crashed: crash.json now */
      if (xts_crumb.ncase == 0 || (every > 0 && xts_crumb.ncase % (uint32_t)every == 0)) crumb_write();
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
        xts_watchdog_kick();                           /* per case (the console exec backend also kicks per run) */
      }
      xts_crumb_phase(XTS_PH_BETWEEN);
      if (xts_sink_pending() > XTS_SINK_HIGH_WATER && kk + 1 < b->ncases) chunk_flush(b, kk + 1);
    }
    xts_cases_running = 0;
    xts_crumb_phase(XTS_PH_BLOCK_END);
    if (hooked) xts_crumb_hook_remove();
  }
  for (int k = na - 1; k >= 0; k--) {
    const xts_axis_t *ax = cv.axis[applied[k]];
    if (ax->revert) ax->revert(b->value[applied[k]]);
  }
  xts_env_reset();                                     /* env back to defaults between blocks */
  end_hooks(b->key);                                   /* chained hooks + the v1 slot */
  xts_sink_temps(b->key);
  block_end(b, &cv, fname);
  xts_plat_poll();
  xts_watchdog_kick();
  return 0;
}

/* the previous boot's report files: records, quarantine, then the files are re-armed for this boot */
static void ingest_previous(xts_prev_t *pv, int resume)
{
  int fresh = pv->crash;
  if (fresh) {
    char tb[24] = "";
    xts_json_get(pv->crash_text, "tb", tb, sizeof tb);
    if (tb[0] && !strcmp(tb, jr.report_tb)) {
      fresh = 0;                                       /* already turned into records by an earlier boot */
      xts_log("crash.json: report tb=%s was ingested before (REPORT line): ignored", tb);
    }
  }
  if (resume) mark_open_blocks(pv, fresh);
  if (fresh) {
    char tb[24] = "", b[16] = "0", nm[48], path[320];
    xts_json_get(pv->crash_text, "tb", tb, sizeof tb);
    xts_json_get(pv->crash_text, "boot", b, sizeof b);
    snprintf(nm, sizeof nm, "crash_boot%s.json", b);
    if (xts_write_small(xts_path(path, sizeof path, rundir, nm, NULL, NULL), pv->crash_text) < 0) xts_io_error("write", path);
    else xts_log("crash.json of boot %s copied to %s", b, nm);
    xts_journal_report(&jr, strtoull(tb, NULL, 10));
  }
  xts_sink_boundary();                                 /* records + QUAR lines on the stick before the files are re-armed */
}

int xts_run(const xts_profile_t *prof, const xts_run_opts_t *o)
{
  static const xts_run_opts_t defaults;
  char path[320], txt[256];
  if (!o) o = &defaults;
  xts_active_profile_name = prof->name;
  active_prof = prof;
  memset(counts, 0, sizeof counts);
  memset(&xts_crumb, 0, sizeof xts_crumb);
  memset(xts_thrcrash, 0, sizeof xts_thrcrash);
  xts_cases_running = 0;
  int probs = xts_registry_init(log_line);
  const char *root = xts_plat_root();
  if (!root || !root[0]) { xts_plat_halt("no USB storage: nothing to write results to"); return -1; }
  if (xts_exec_init() < 0) xts_screen_error("exec: vector install failed (traps inside cases will crash)");

  /* identity / directories (any failure here: the stick is unusable -> halt with the message) */
  if (xts_storage_dead()) storage_halt("storage error at mount (see above)", root);
  if (xts_mkdirs(xts_path(path, sizeof path, root, "xts", NULL, NULL)) < 0) storage_halt("cannot create", path);
  if (xts_read_small(xts_path(path, sizeof path, root, "xts", "label.txt", NULL), tag, sizeof tag) > 0) sanitize_tag(tag);
  else tag[0] = 0;
  if (!tag[0]) xts_plat_default_tag(tag, sizeof tag);
  if (xts_mkdirs(xts_path(path, sizeof path, root, "xts", tag, NULL)) < 0) storage_halt("cannot create", path);

  int resume = 0;
  boots = 1;
  char cur_path[320];
  xts_path(cur_path, sizeof cur_path, root, "xts", tag, "current.txt");
  if (xts_read_small(cur_path, txt, sizeof txt) > 0) {
    char rid[40], pname[64], bid[96];
    int nb = 1;
    if (sscanf(txt, "%39s %63s %95s %d", rid, pname, bid, &nb) >= 3 && !strcmp(pname, prof->name) &&
        !strcmp(bid, xts_build_id())) {
      resume = 1;
      boots = nb + 1;
      snprintf(run_id, sizeof run_id, "%s", rid);
    }
  }
  if (!resume) {
    unsigned n = 0;
    if (xts_read_small(xts_path(path, sizeof path, root, "xts", "runs.txt", NULL), txt, sizeof txt) > 0) n = (unsigned)strtoul(txt, NULL, 10);
    n++;
    snprintf(txt, sizeof txt, "%u\n", n);
    if (xts_write_small(path, txt) < 0) storage_halt("cannot write", path);
    uint8_t rtc[5];
    if (xts_smc_rtc(rtc) == 0)
      snprintf(run_id, sizeof run_id, "%04u-%02X%02X%02X%02X%02X", n, rtc[4], rtc[3], rtc[2], rtc[1], rtc[0]);
    else
      snprintf(run_id, sizeof run_id, "%04u", n);
  }
  xts_current_run_id = run_id;
  xts_path(rundir, sizeof rundir, root, "xts", tag, run_id);
  if (xts_mkdirs(rundir) < 0) storage_halt("cannot create", rundir);
  storage_probe();
  snprintf(txt, sizeof txt, "%s %s %s %d\n", run_id, prof->name, xts_build_id(), boots);
  if (xts_write_small(cur_path, txt) < 0) xts_io_error("write", cur_path);

  /* the previous boot's report files (read before anything re-arms them) */
  static xts_prev_t prev;
  memset(&prev, 0, sizeof prev);
  if (resume) xts_prev_load(rundir, run_id, &prev);
  else snprintf(prev.how, sizeof prev.how, "new run");
  snprintf(xts_prev_boot_how, sizeof xts_prev_boot_how, "%s", prev.how);

  xts_journal_load(&jr, xts_path(path, sizeof path, rundir, "journal.txt", NULL, NULL));
  xts_journal_open(&jr);
  xts_sink_open(rundir, run_id, jr.max_seq);
  if (resume && jr.has_cpt) xts_cycles_per_tick = jr.cpt;   /* restored before any block runs (logged below) */
  xts_log("XTS %s profile %s run %s boot %d tag %s build %s", XTS_VERSION, prof->name, run_id, boots, tag, xts_build_id());
#ifdef XTS_WATCHDOG
  xts_log("watchdog: ON (block timeout %u s, mem/smt %u s)", XTS_WATCHDOG_MS / 1000u, XTS_WATCHDOG_LONG_MS / 1000u);
#else
  xts_log("watchdog: OFF (build with WATCHDOG=1 for automatic reboots after a hang)");
#endif
  xts_log("previous boot: %s", prev.how);
  xts_log("breadcrumbs: progress.bin before every case (self/semantic families), every %d case(s) otherwise", XTS_CRUMB_EVERY_TIMING);
  if (probs) xts_screen_error("registry: %d problem(s), see run.log", probs);
  if (resume && jr.has_cpt) xts_log("resume: cycles per timebase tick %.6g (from the journal)", xts_cycles_per_tick);
  if (!resume) {
    if (xts_sysinfo_write(xts_path(path, sizeof path, rundir, "sysinfo.json", NULL, NULL), run_id, tag, boots) < 0)
      xts_io_error("write", path);
    snprintf(txt, sizeof txt, "START %s %s", prof->name, xts_build_id());
    xts_journal_line(&jr, txt);
  } else {
    char nm[40];
    snprintf(nm, sizeof nm, "sysinfo_boot%d.json", boots);
    if (xts_sysinfo_write(xts_path(path, sizeof path, rundir, nm, NULL, NULL), run_id, tag, boots) < 0)
      xts_io_error("write", path);
    snprintf(txt, sizeof txt, "BOOT %d", boots);
    xts_journal_line(&jr, txt);
    xts_screen_error("resuming run %s (boot %d)", run_id, boots);
  }

  if (xts_plan_build(&plan, prof) < 0) { xts_screen_error("scheduler: out of memory"); xts_sink_boundary(); return -1; }
  xts_log("plan: %d blocks, %d case runs, %d families, %d axes", plan.n, plan.total_cases, xts_family_count(), xts_axis_count());
  plan_ran = calloc(plan.n + 1, 1);
  ingest_previous(&prev, resume);
  xts_slots_arm(rundir, run_id, boots);                /* crash.json + progress.bin for this boot */
  snprintf(xts_crumb.run_id, sizeof xts_crumb.run_id, "%s", run_id);
  xts_crumb.boot = boots;

  int *fam_setup = calloc(xts_family_count() + 1, sizeof(int));
  int *fam_last = calloc(xts_family_count() + 1, sizeof(int));
  for (int i = 0; i < plan.n; i++) fam_last[plan.b[i].fam] = i;
  int done = 0;
  uint32_t wd_ms = XTS_WATCHDOG_MS;
  xts_watchdog_start(wd_ms);                           /* weak no-op unless src/smt provides a watchdog */
#ifdef XTS_WATCHDOG
  const char *wd = "ON";
#else
  const char *wd = "OFF";
#endif
  for (int i = 0; i < plan.n; i++) {
    xts_jentry_t *e = xts_journal_find(&jr, plan.b[i].key);
    if (e && (e->ended || e->quar_all)) continue;
    if (o->only_family && strcmp(o->only_family, xts_family_name(xts_family_get(plan.b[i].fam)))) continue;
    if (o->max_blocks && done >= o->max_blocks) break;
    const xts_family_t *f = xts_family_get(plan.b[i].fam);
    uint32_t t = block_timeout_ms(f);
    if (t != wd_ms) { wd_ms = t; xts_watchdog_set_timeout(t); }
    const uint32_t *sc = xts_sink_status_counts();
    xts_status_line(0, "XTS %s block %d/%d %s | pass %u fail %u skip %u hang %u crash %u | write errors %u | watchdog %s",
                    prof->name, i + 1, plan.n, xts_family_name(f), (unsigned)sc[XTS_ST_PASS], (unsigned)sc[XTS_ST_FAIL],
                    (unsigned)sc[XTS_ST_SKIP], (unsigned)sc[XTS_ST_HANG], (unsigned)sc[XTS_ST_CRASH], xts_io_error_count(), wd);
    if (plan.b[i].finish) run_finish(i, fam_setup);
    else run_block(i, fam_setup);
    done++;
    if (fam_last[plan.b[i].fam] == i && f->teardown && fam_setup[plan.b[i].fam] > 0) f->teardown();
    if (xts_storage_dead()) break;                     /* nothing can be recorded any more: stop the run */
  }
  xts_watchdog_stop();
  xts_crumb_phase(XTS_PH_RUN_END);
  if (xts_storage_dead()) {
    xts_status_line(1, "XTS %s STOPPED: STORAGE ERROR after %d block(s) - stop using this stick until it has been checked",
                    prof->name, done);
    xts_sink_close();
    xts_journal_close(&jr);
    active_prof = NULL;
    free(fam_setup);
    free(fam_last);
    free(plan_ran);
    plan_ran = NULL;
    return -3;
  }
  int finished = !(o->max_blocks && done >= o->max_blocks);
  if (finished) {
    xts_journal_line(&jr, "DONE");
    xts_fat_busy++;
    int rm = remove(cur_path);
    xts_fat_busy--;
    if (rm != 0) xts_io_error("remove", cur_path);
  }
  char cs[600];
  xts_slot_crumb_stats(cs, sizeof cs);
  xts_log("run %s: %d blocks executed this boot%s; %u write error(s); %u guard violation(s); %.0f ms in block flushes", run_id,
          done, finished ? ", run complete" : "", xts_io_error_count(), xts_guard_violations(), xts_sink_flush_ms_total());
  xts_log("breadcrumbs (progress.bin, one verified sector write each): %s", cs);
  if (o->dryrun)
    xts_log("dry run: %llu single words (%llu simulated by the model, %llu not), %llu kernels (%llu words: %llu checked "
            "instructions, %llu raw/exit), %llu bad words",
            (unsigned long long)xts_dry_stats.words, (unsigned long long)xts_dry_stats.simulated,
            (unsigned long long)xts_dry_stats.not_simulated, (unsigned long long)xts_dry_stats.kernels,
            (unsigned long long)xts_dry_stats.kernel_words, (unsigned long long)xts_dry_stats.kernel_insn_words,
            (unsigned long long)xts_dry_stats.kernel_raw_words, (unsigned long long)xts_dry_stats.bad_words);
  log_storage();
  xts_sink_flush();
  if (xts_sink_summary(rundir) < 0) xts_io_error("write", "summary.json");
  const uint32_t *sc = xts_sink_status_counts();
  xts_crumb_phase(XTS_PH_RUN_END);
  crumb_write();                                       /* progress.bin: "run_end" */
  xts_log("run end: summary.json written; closing every file, final fatFlush and unmount (the screen says \"safe to power "
          "off\" only after the unmount)");
  xts_sink_close();
  xts_journal_close(&jr);
  int um = xts_plat_storage_unmount();
  if (um == 0)
    xts_status_line(1, "XTS %s done: pass %u fail %u skip %u hang %u crash %u | write errors %u | stick unmounted: safe to "
                    "power off", prof->name, (unsigned)sc[XTS_ST_PASS], (unsigned)sc[XTS_ST_FAIL], (unsigned)sc[XTS_ST_SKIP],
                    (unsigned)sc[XTS_ST_HANG], (unsigned)sc[XTS_ST_CRASH], xts_io_error_count());
  else
    xts_status_line(1, "XTS %s done: pass %u fail %u skip %u hang %u crash %u | write errors %u | UNMOUNT FAILED (%d): check "
                    "the stick on a PC", prof->name, (unsigned)sc[XTS_ST_PASS], (unsigned)sc[XTS_ST_FAIL],
                    (unsigned)sc[XTS_ST_SKIP], (unsigned)sc[XTS_ST_HANG], (unsigned)sc[XTS_ST_CRASH], xts_io_error_count(), um);
  active_prof = NULL;
  free(fam_setup);
  free(fam_last);
  free(plan_ran);
  plan_ran = NULL;
  if (um != 0) return xts_plat_storage_status(NULL) ? -3 : -4;
  return (o->dryrun && xts_dry_stats.bad_words) ? 2 : 0;
}
