/* Run driver: identity, run directory, resume via the journal, the block loop, summary.
 *
 * Output: <root>/xts/<console_tag>/<run_id>/{sysinfo.json, run.log, results.jsonl, results.csv, failures.jsonl,
 *         temps.csv, journal.txt, summary.json}
 *   console_tag = contents of <root>/xts/label.txt if present, else <type>-<pvr>-<xenosid>
 *   run_id      = counter from <root>/xts/runs.txt (+ "-<SMC RTC>" when available)
 *   <root>/xts/<tag>/current.txt names an unfinished run ("<run_id> <profile> <build_id> <boots>"): the next boot of the
 *   same ELF resumes it; a different profile/build starts a new run.
 *
 * Storage: after the run directory is created a probe file is written, read back and removed; any failure there (or in
 * creating the directories) halts with "FAT32 (MBR) USB stick required; write test failed: <reason>" on screen.
 *
 * Resume (a BEGIN without END in the journal): the case is named by the crash page (breadcrumb / crash record, if RAM
 * survived the reboot), else by a CRASH line (the crash path writes it on PIR 0), else - in a block already re-run in
 * per-case mode - by the last CASE line. A named case is quarantined (QUAR case=k) and the block re-runs without it. A
 * block whose case is unknown is re-run ONCE with a durable "CASE <key> <k>" line before every case (RETRY), so a second
 * hang names the case; unknown again (or 4 failed attempts, or 15 quarantined cases) quarantines the whole block.
 * finish() blocks are never retried.
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

const char *xts_active_profile_name;
const char *xts_current_run_id;
xts_dry_stats_t xts_dry_stats;
int xts_crashpage_survived = -1;
char xts_crashpage_how[160] = "not determined";

static char run_id[40], tag[64], rundir[256];
static xts_journal_t jr;
static xts_plan_t plan;
static uint8_t *plan_ran;      /* per block: 0 not run this boot, 1 ran, 2 skipped (axis/setup) */
static int counts[XTS_ST_COUNT];
static const xts_profile_t *active_prof;
/* what the main thread is doing, for the crash path: block index and case k (relative), -1 = none */
static volatile int cur_bi = -1, cur_k = -1;

int xts_profile_names_family(const char *family)
{
  for (int i = 0; active_prof && i < active_prof->nrows; i++)
    if (active_prof->rows[i].families && !strcmp(active_prof->rows[i].families, family)) return 1;
  return 0;
}

/* ---- crash path: the journal CRASH line (no malloc, no printf) ---- */
static size_t put_s(char *o, size_t n, size_t cap, const char *s)
{
  while (*s && n + 1 < cap) o[n++] = *s++;
  return n;
}
static size_t put_hex(char *o, size_t n, size_t cap, uint64_t v, int digits)
{
  for (int i = digits - 1; i >= 0 && n + 1 < cap; i--) o[n++] = "0123456789ABCDEF"[(v >> (4 * i)) & 15];
  return n;
}
static size_t put_dec(char *o, size_t n, size_t cap, int v)
{
  char t[12];
  int k = 0;
  unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
  do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u && k < 11);
  if (v < 0 && n + 1 < cap) o[n++] = '-';
  while (k && n + 1 < cap) o[n++] = t[--k];
  return n;
}

int xts_core_crash_journal(uint32_t vector, uint64_t srr0, uint64_t dar)
{
  int bi = cur_bi, k = cur_k;
  if (bi < 0 || bi >= plan.n || k < 0 || xts_fat_busy || !jr.pf.f) return 0;
  static char line[320];
  size_t n = 0, cap = sizeof line;
  n = put_s(line, n, cap, "CRASH ");
  n = put_s(line, n, cap, plan.b[bi].key);
  n = put_s(line, n, cap, " case=");
  n = put_dec(line, n, cap, k);
  n = put_s(line, n, cap, " vec=");
  n = put_hex(line, n, cap, vector, 4);
  n = put_s(line, n, cap, " srr0=");
  n = put_hex(line, n, cap, srr0, 16);
  n = put_s(line, n, cap, " dar=");
  n = put_hex(line, n, cap, dar, 16);
  line[n++] = '\n';
  return xts_journal_raw(&jr, line, n) == 0;
}

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
  if (w != 0) storage_halt("cannot write", path);
  if (r != (int)strlen(text) || strcmp(back, text)) storage_halt("read-back mismatch", path);
  if (rm != 0) storage_halt("cannot remove", path);
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

/* turn a BEGIN without END into hang/crash records + QUAR lines (or one RETRY, see the top of the file) */
static void mark_open_blocks(const xts_crashpage_t *cp, int cp_valid)
{
  int page = cp_valid && cp->magic == XTS_CRASH_MAGIC && !strcmp(cp->run_id, run_id);
  for (int i = 0; i < jr.n; i++) {
    xts_jentry_t *e = &jr.e[i];
    if (!e->open) continue;
    const xts_block_t *b = NULL;
    for (int k = 0; k < plan.n; k++)
      if (!strcmp(plan.b[k].key, e->key)) { b = &plan.b[k]; break; }
    int rel = -1, status = XTS_ST_HANG, from_page = 0;
    const char *src = "none";
    int on_page = page && !strcmp(cp->block_key, e->key);
    int crashed = on_page && cp->crashed;
    if (crashed) status = XTS_ST_CRASH;
    if (b && on_page && cp->case_index >= 0) {
      rel = xts_block_rel(b, cp->case_index);
      if (rel >= 0) { src = crashed ? "crash page (unhandled exception)" : "crash page breadcrumb"; from_page = 1; }
    }
    if (e->has_crash) {
      status = XTS_ST_CRASH;
      if (rel < 0 && b && e->crash_case >= 0 && e->crash_case < b->ncases) { rel = e->crash_case; src = "journal CRASH line"; }
    }
    if (rel < 0 && e->retry && b && e->last_case >= 0 && e->last_case < b->ncases) {
      rel = e->last_case;
      src = "journal CASE line (per-case re-run)";
    }
    int attempts = e->begun - e->ended;
    int giveup = attempts >= 4 || e->nquar >= 15;
    if (giveup) rel = -1;
    int retry = rel < 0 && !giveup && !e->retry && b && !b->finish;
    int ci = (b && rel >= 0) ? xts_block_case(b, rel) : -1;
    char det[1024];
    xts_json_t j;
    xj_init(&j, det, sizeof det);
    xj_obj(&j);
    xj_kv_str(&j, "reason", status == XTS_ST_CRASH ? "unhandled exception" : "no END in journal (hang, crash or power cut)");
    xj_kv_str(&j, "case_from", src);
    xj_kv_str(&j, "action", retry ? "block re-run once with per-case journal lines (RETRY)" :
                            rel >= 0 ? "case quarantined, block re-run without it" : "whole block quarantined");
    xj_kv_int(&j, "attempts", attempts);
    if (ci >= 0) xj_kv_int(&j, "case_index", ci);
    if (from_page) xj_kv_int(&j, "repeat", cp->repeat);
    if (crashed) {
      xj_kv_hex(&j, "vector", cp->vector, 4);
      xj_kv_hex(&j, "srr0", cp->srr0, 16);
      xj_kv_hex(&j, "srr1", cp->srr1, 16);
      xj_kv_hex(&j, "dar", cp->dar, 16);
      xj_kv_hex(&j, "lr", cp->lr, 16);
      xj_kv_uint(&j, "pir", cp->pir);
    } else if (e->has_crash) {
      xj_kv_hex(&j, "vector", e->crash_vec, 4);
      xj_kv_hex(&j, "srr0", e->crash_srr0, 16);
      xj_kv_hex(&j, "dar", e->crash_dar, 16);
      xj_kv_uint(&j, "pir", 0);
    }
    xj_finish(&j);
    xts_condvec_t cv;
    xts_plan_condvec(b, &cv);
    const char *fam = b ? xts_family_name(xts_family_get(b->fam)) : "?";
    const char *cid = ci >= 0 ? plan.cases[b->fam].v[ci].id : "*";
    xts_sink_emit_core(fam, cid, e->key, &cv, status, "result", det);
    count_status_cb(status);
    if (retry) xts_journal_retry(&jr, e->key);
    else xts_journal_quar(&jr, e->key, rel, xts_status_name(status));
    xts_screen_error("previous boot: block %s %s, case %s (%s)%s", e->key, xts_status_name(status), cid, src,
                     retry ? ": re-running it once case by case" : "");
  }
  xts_sink_flush();
}

/* unhandled exceptions on other hardware threads (crash page thread slots) from the previous boot */
static void report_thread_crashes(const xts_crashpage_t *cp)
{
  for (int i = 0; i < XTS_CRASH_THREADS; i++) {
    const xts_crashthr_t *t = &cp->thr[i];
    if (t->magic != XTS_CRASH_THR_MAGIC) continue;
    const xts_block_t *b = NULL;
    for (int k = 0; k < plan.n && t->block_key[0]; k++)
      if (!strncmp(plan.b[k].key, t->block_key, sizeof t->block_key - 1)) { b = &plan.b[k]; break; }
    char det[600], cid[32], fam[64] = "core";
    xts_json_t j;
    xj_init(&j, det, sizeof det);
    xj_obj(&j);
    xj_kv_str(&j, "reason", "unhandled exception on another hardware thread (crash page thread slot)");
    xj_kv_uint(&j, "pir", t->pir);
    xj_kv_hex(&j, "vector", t->vector, 4);
    xj_kv_hex(&j, "srr0", t->srr0, 16);
    xj_kv_hex(&j, "srr1", t->srr1, 16);
    xj_kv_hex(&j, "dar", t->dar, 16);
    xj_kv_hex(&j, "lr", t->lr, 16);
    xj_kv_hex(&j, "r1", t->r1, 16);
    xj_kv_uint(&j, "count", t->count);
    xj_kv_str(&j, "main_block", t->block_key);
    xj_kv_int(&j, "main_case_index", t->case_index);
    xj_finish(&j);
    xts_condvec_t cv;
    xts_plan_condvec(b, &cv);
    if (b) snprintf(fam, sizeof fam, "%s", xts_family_name(xts_family_get(b->fam)));
    snprintf(cid, sizeof cid, "pir%u", (unsigned)t->pir);
    xts_sink_emit_core(fam, cid, t->block_key[0] ? t->block_key : "-", &cv, XTS_ST_CRASH, "result", det);
    count_status_cb(XTS_ST_CRASH);
    xts_screen_error("previous boot: PIR %u crashed (vector %X srr0 %08llX) during block %s", (unsigned)t->pir, (unsigned)t->vector,
            (unsigned long long)t->srr0, t->block_key[0] ? t->block_key : "?");
  }
  xts_sink_flush();
}

static void end_hooks(const char *key)
{
  int left = xts_exec_hook_count();
  if (left) xts_log("block %s: %d exec hook(s) still registered after the axes were reverted: removed", key, left);
  xts_exec_hook_clear();
}

/* the family's finish() pseudo-block */
static int run_finish(int bi, int *fam_setup)
{
  xts_block_t *b = &plan.b[bi];
  const xts_family_t *f = xts_family_get(b->fam);
  const char *fname = xts_family_name(f);
  xts_condvec_t cv;
  xts_plan_condvec(b, &cv);
  xts_sink_set_block(b->key, &cv, fname);
  if (fam_setup[b->fam] == 0) {
    fam_setup[b->fam] = (f->setup && f->setup() < 0) ? -1 : 1;
    if (fam_setup[b->fam] < 0) xts_log("family %s: setup() failed, its blocks are skipped", fname);
  }
  xts_journal_begin(&jr, b->key);
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
    xts_crash_breadcrumb(run_id, b->key, -1, 0, "finish", xts_sink_seq());
    int rc = f->finish(&ctx);
    if (rc < 0 && ctx.nrecords == 0) xts_emit_status(&ctx, XTS_ST_FAIL, "{\"reason\":\"finish() returned an error\"}");
  }
  xts_env_reset();
  end_hooks(b->key);
  xts_crash_breadcrumb(run_id, b->key, -1, 0, "", xts_sink_seq());
  xts_sink_flush_block(b->key);
  xts_journal_end(&jr, b->key, xts_sink_seq());
  xts_plat_poll();
  xts_watchdog_kick();
  return 0;
}

static int run_block(int bi, int *fam_setup)
{
  xts_block_t *b = &plan.b[bi];
  const xts_family_t *f = xts_family_get(b->fam);
  const char *fname = xts_family_name(f);
  xts_jentry_t *e = xts_journal_find(&jr, b->key);
  xts_condvec_t cv;
  xts_plan_condvec(b, &cv);
  xts_sink_set_block(b->key, &cv, fname);
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
    char det[256];
    snprintf(det, sizeof det, "{\"reason\":\"%s\"}", why);
    xts_sink_emit_core(fname, "*", b->key, &cv, XTS_ST_SKIP, "result", det);
    count_status_cb(XTS_ST_SKIP);
  } else {
    int percase = e && e->retry;                       /* re-run of a block whose hang named no case */
    if (percase) xts_log("block %s: re-run with a journal line before every case", b->key);
    for (int kk = 0; kk < b->ncases; kk++) {
      int ci = xts_block_case(b, kk);
      if (xts_journal_is_quar(e, kk)) continue;
      if (percase) xts_journal_case(&jr, b->key, kk);
      cur_bi = bi;
      cur_k = kk;
      const xts_case_t *c = &plan.cases[b->fam].v[ci];
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
        xts_crash_breadcrumb(run_id, b->key, ci, rep, c->id, xts_sink_seq());
        int rc = f->run(c, &ctx);
        if (rc < 0 && ctx.nrecords == 0) xts_emit_status(&ctx, XTS_ST_FAIL, "{\"reason\":\"run() returned an error\"}");
        xts_watchdog_kick();                           /* per case (the console exec backend also kicks per run) */
      }
    }
    cur_k = -1;
    if (percase) xts_journal_case(&jr, b->key, -1);
  }
  for (int k = na - 1; k >= 0; k--) {
    const xts_axis_t *ax = cv.axis[applied[k]];
    if (ax->revert) ax->revert(b->value[applied[k]]);
  }
  xts_env_reset();                                     /* env back to defaults between blocks */
  end_hooks(b->key);                                   /* chained hooks + the v1 slot */
  xts_sink_temps(b->key);
  xts_crash_breadcrumb(run_id, b->key, -1, 0, "", xts_sink_seq());
  xts_sink_flush_block(b->key);
  if (!jr.has_cpt || jr.cpt != xts_cycles_per_tick) {   /* calib changed the scale: journal it so a resume keeps it */
    char l[64];
    snprintf(l, sizeof l, "CPT %.9g", xts_cycles_per_tick);
    xts_journal_line(&jr, l);
  }
  xts_journal_end(&jr, b->key, xts_sink_seq());
  xts_plat_poll();
  xts_watchdog_kick();
  return 0;
}

/* crash page survival (sysinfo.json): decided from what the previous boot must have left */
static void judge_crash_page(const xts_crashpage_t *cp, int cp_valid, int resume, int boots)
{
  int thr = 0;
  for (int i = 0; cp_valid && i < XTS_CRASH_THREADS; i++) thr |= cp->thr[i].magic == XTS_CRASH_THR_MAGIC;
  if (cp_valid && cp->magic == XTS_CRASH_MAGIC) {
    xts_crashpage_survived = 1;
    snprintf(xts_crashpage_how, sizeof xts_crashpage_how, "valid breadcrumb from an earlier boot (run %s%s)", cp->run_id,
             strcmp(cp->run_id, run_id) ? ", another run" : "");
  } else if (thr) {
    xts_crashpage_survived = 1;
    snprintf(xts_crashpage_how, sizeof xts_crashpage_how, "valid thread crash slot from an earlier boot");
  } else if (resume && jr.n > 0) {
    xts_crashpage_survived = 0;
    snprintf(xts_crashpage_how, sizeof xts_crashpage_how,
             "boot %d of this run wrote breadcrumbs (journal has blocks) but the page is empty/invalid", boots - 1);
  } else {
    xts_crashpage_survived = -1;
    snprintf(xts_crashpage_how, sizeof xts_crashpage_how,
             "no valid page and no breadcrumb expected (first XTS boot since power-on, or the page did not survive)");
  }
}

int xts_run(const xts_profile_t *prof, const xts_run_opts_t *o)
{
  static const xts_run_opts_t defaults;
  char path[320], txt[256];
  if (!o) o = &defaults;
  xts_active_profile_name = prof->name;
  active_prof = prof;
  cur_bi = cur_k = -1;
  memset(counts, 0, sizeof counts);
  int probs = xts_registry_init(log_line);
  const char *root = xts_plat_root();
  if (!root || !root[0]) { xts_plat_halt("no USB storage: nothing to write results to"); return -1; }
  if (xts_exec_init() < 0) xts_screen_error("exec: vector install failed (traps inside cases will crash)");

  xts_crashpage_t cp;
  int cp_valid = xts_crash_collect(&cp);

  /* identity / directories (any failure here: the stick is unusable -> halt with the message) */
  if (xts_mkdirs(xts_path(path, sizeof path, root, "xts", NULL, NULL)) < 0) storage_halt("cannot create", path);
  if (xts_read_small(xts_path(path, sizeof path, root, "xts", "label.txt", NULL), tag, sizeof tag) > 0) sanitize_tag(tag);
  else tag[0] = 0;
  if (!tag[0]) xts_plat_default_tag(tag, sizeof tag);
  if (xts_mkdirs(xts_path(path, sizeof path, root, "xts", tag, NULL)) < 0) storage_halt("cannot create", path);

  int resume = 0, boots = 1;
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

  xts_journal_load(&jr, xts_path(path, sizeof path, rundir, "journal.txt", NULL, NULL));
  xts_journal_open(&jr);
  xts_sink_open(rundir, run_id, jr.max_seq);
  judge_crash_page(&cp, cp_valid, resume, boots);
  if (resume && jr.has_cpt) xts_cycles_per_tick = jr.cpt;   /* restored before any block runs (logged below) */
  xts_log("XTS %s profile %s run %s boot %d tag %s build %s", XTS_VERSION, prof->name, run_id, boots, tag, xts_build_id());
#ifdef XTS_WATCHDOG
  xts_log("watchdog: ON (block timeout %u s, mem/smt %u s)", XTS_WATCHDOG_MS / 1000u, XTS_WATCHDOG_LONG_MS / 1000u);
#else
  xts_log("watchdog: OFF (build with WATCHDOG=1 for automatic reboots after a hang)");
#endif
  xts_log("crash page: survived=%d (%s)", xts_crashpage_survived, xts_crashpage_how);
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
  if (cp_valid && cp.magic == XTS_CRASH_MAGIC) xts_log("crash page from the previous boot: run %s block %s case %d crashed=%u vector %X", cp.run_id,
                        cp.block_key, cp.case_index, cp.crashed, cp.vector);

  if (xts_plan_build(&plan, prof) < 0) { xts_screen_error("scheduler: out of memory"); xts_sink_flush(); return -1; }
  xts_log("plan: %d blocks, %d case runs, %d families, %d axes", plan.n, plan.total_cases, xts_family_count(), xts_axis_count());
  plan_ran = calloc(plan.n + 1, 1);
  mark_open_blocks(&cp, cp_valid);
  if (cp_valid) report_thread_crashes(&cp);

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
    cur_bi = i;
    if (plan.b[i].finish) run_finish(i, fam_setup);
    else run_block(i, fam_setup);
    cur_bi = -1;
    done++;
    if (fam_last[plan.b[i].fam] == i && f->teardown && fam_setup[plan.b[i].fam] > 0) f->teardown();
  }
  xts_watchdog_stop();
  int finished = !(o->max_blocks && done >= o->max_blocks);
  if (finished) {
    xts_journal_line(&jr, "DONE");
    if (remove(cur_path) != 0) xts_io_error("remove", cur_path);
  }
  xts_log("run %s: %d blocks executed this boot%s; %u write error(s); %.0f ms in block flushes", run_id, done,
          finished ? ", run complete" : "", xts_io_error_count(), xts_sink_flush_ms_total());
  if (o->dryrun)
    xts_log("dry run: %llu single words (%llu simulated by the model, %llu not), %llu kernels (%llu words: %llu checked "
            "instructions, %llu raw/exit), %llu bad words",
            (unsigned long long)xts_dry_stats.words, (unsigned long long)xts_dry_stats.simulated,
            (unsigned long long)xts_dry_stats.not_simulated, (unsigned long long)xts_dry_stats.kernels,
            (unsigned long long)xts_dry_stats.kernel_words, (unsigned long long)xts_dry_stats.kernel_insn_words,
            (unsigned long long)xts_dry_stats.kernel_raw_words, (unsigned long long)xts_dry_stats.bad_words);
  xts_sink_flush();
  if (xts_sink_summary(rundir) < 0) xts_io_error("write", "summary.json");
  const uint32_t *sc = xts_sink_status_counts();
  xts_status_line(1, "XTS %s done: pass %u fail %u skip %u hang %u crash %u | write errors %u", prof->name,
                  (unsigned)sc[XTS_ST_PASS], (unsigned)sc[XTS_ST_FAIL], (unsigned)sc[XTS_ST_SKIP], (unsigned)sc[XTS_ST_HANG],
                  (unsigned)sc[XTS_ST_CRASH], xts_io_error_count());
  xts_sink_close();
  xts_journal_close(&jr);
  active_prof = NULL;
  free(fam_setup);
  free(fam_last);
  free(plan_ran);
  plan_ran = NULL;
  return (o->dryrun && xts_dry_stats.bad_words) ? 2 : 0;
}
