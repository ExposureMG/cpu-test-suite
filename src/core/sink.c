/* Output sinks: results.jsonl, results.csv, failures.jsonl, run.log, temps.csv, summary.json.
 * Records are buffered in RAM and written only at block boundaries (xts_sink_boundary: fwrite + fflush + fsync of every
 * file - libxenon's newlib routes fsync to libfat's _FAT_fsync_r - then fatFlush), to files that stay open for the whole
 * run, so a hang or crash loses at most the current block, which the journal then marks. While cases run
 * (xts_cases_running) no file is written: a buffer that fills grows instead, main.c ends the chunk at the next case
 * boundary (XTS_SINK_HIGH_WATER), and any file operation attempted then is refused and counted as a guard violation.
 * Every write, flush, fsync, open and close is checked: errors are counted (status line, run.log, "write_errors").
 * xts_log lines go to run.log only (also the ones logged before xts_sink_open: buffered, written at the first flush);
 * the screen gets errors (xts_screen_error) and a status line at most once per second. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "core.h"

enum { B_RESULTS, B_CSV, B_FAIL, B_LOG, B_TEMPS, NB };
static const char *const fname[NB] = { "results.jsonl", "results.csv", "failures.jsonl", "run.log", "temps.csv" };
static struct { char *p; size_t len, cap; } buf[NB];
static xts_pfile_t pf[NB];
static char dir[256], run_id[40];
static uint64_t seq;
static int opened;
static char cur_block[160], cur_family[48];
static xts_condvec_t cur_cv;
static uint32_t st_count[XTS_ST_COUNT];
static unsigned io_errors;
static double flush_ms_total;
volatile int xts_fat_busy;
volatile int xts_cases_running;
static unsigned guard_violations;
xts_sink_tap_fn xts_sink_tap;
#ifdef XTS_HOST
int xts_vocab_check = 1;
#else
int xts_vocab_check = 0;      /* console: the host dry run already checked every family's records */
#endif

#define BUF_CAP (2u << 20)
#define BUF_MAX (64u << 20)      /* growth limit while cases run (a chunk ends at XTS_SINK_HIGH_WATER long before) */
static unsigned dropped_bytes;
#define CSV_HEADER "run_id,seq,family,case_id,mnem,word,cond,block,metric,value,unit,n,min,median,mad,fit_r2,status\n"

/* ---- error accounting ---- */
void xts_io_error(const char *what, const char *path)
{
  io_errors++;
  if (io_errors <= 5) xts_screen_error("USB write error #%u: %s %s", io_errors, what, path ? path : "");
  else xts_log("USB write error #%u: %s %s", io_errors, what, path ? path : "");
}

unsigned xts_io_error_count(void) { return io_errors; }
unsigned xts_guard_violations(void) { return guard_violations; }

/* the write guard: no file operation while cases run (the stick must stay consistent whenever test code executes) */
int xts_write_refused(const char *what, const char *path)
{
  if (!xts_cases_running) return 0;
  guard_violations++;
  if (guard_violations <= 3)
    xts_screen_error("BUG: file %s while cases run refused (%s): files are written at block boundaries only", what,
                     path ? path : "");
  else
    xts_log("BUG: file %s while cases run refused (%s): files are written at block boundaries only", what, path ? path : "");
  return 1;
}

/* storage poisoned (the disc guard latched an error): no more file I/O at all, one message on screen and in the log
 * buffer. Returns 1 when writing must stop. */
int xts_storage_dead(void)
{
  static int told;
  xts_storage_status_t st;
  if (!xts_plat_storage_status(&st)) return 0;
  if (!told) {
    told = 1;
    io_errors++;
    xts_screen_error("STORAGE ERROR: %s of sectors %lu-%lu failed (%s). XTS stopped writing. Stop using this stick until "
                     "it has been checked (Windows: chkdsk /f, Linux: fsck.fat -n).", st.op, (unsigned long)st.sector,
                     (unsigned long)(st.sector + (st.count ? st.count - 1 : 0)), st.why);
  }
  return 1;
}

/* ---- persistent files ---- */
int xts_pf_open(xts_pfile_t *p, const char *path)
{
  if (p->f) return 0;
  if (path != p->path) snprintf(p->path, sizeof p->path, "%s", path);
  if (xts_write_refused("open", p->path)) return -1;
  if (xts_storage_dead()) return -1;
  xts_fat_busy++;
  p->f = fopen(p->path, "ab");
  xts_fat_busy--;
  p->dirty = 0;
  if (!p->f) { xts_io_error("open", p->path); return -1; }
  return 0;
}

int xts_pf_write(xts_pfile_t *p, const void *data, size_t n)
{
  if (!n) return 0;
  if (xts_write_refused("write", p->path)) return -1;
  if (!p->f && (!p->path[0] || xts_pf_open(p, p->path) < 0)) return -1;
  if (xts_storage_dead()) return -1;
  xts_fat_busy++;
  size_t w = fwrite(data, 1, n, p->f);
  xts_fat_busy--;
  p->dirty = 1;
  if (w != n) { xts_io_error("write", p->path); clearerr(p->f); return -1; }
  return 0;
}

int xts_pf_sync(xts_pfile_t *p)
{
  if (!p->f || !p->dirty) return 0;
  if (xts_write_refused("sync", p->path)) return -1;
  if (xts_storage_dead()) return -1;
  int r = 0;
  xts_fat_busy++;
  if (fflush(p->f) != 0) { r = -1; clearerr(p->f); }
  else if (fsync(fileno(p->f)) != 0) r = -2;
  xts_fat_busy--;
  p->dirty = 0;
  if (r) xts_io_error(r == -1 ? "flush" : "fsync", p->path);
  return r;
}

void xts_pf_close(xts_pfile_t *p)
{
  if (!p->f) return;
  if (xts_write_refused("close", p->path)) return;
  if (xts_storage_dead()) { p->f = NULL; return; }   /* never touch libfat again (the FILE is dropped) */
  xts_pf_sync(p);
  xts_fat_busy++;
  int r = fclose(p->f);
  xts_fat_busy--;
  p->f = NULL;
  if (r != 0) xts_io_error("close", p->path);
}

static void bput(int b, const char *s, size_t n)
{
  if (!buf[b].p) {
    buf[b].p = malloc(BUF_CAP);
    buf[b].cap = buf[b].p ? BUF_CAP : 0;
  }
  if (buf[b].len + n > buf[b].cap) {
    if (opened && !xts_cases_running) {        /* outside cases: write out (block boundary work, resume records) */
      xts_pf_write(&pf[b], buf[b].p, buf[b].len);
      buf[b].len = 0;
    }
    if (buf[b].len + n > buf[b].cap) {         /* while cases run (or before the sink is open): grow, never write */
      size_t nc = buf[b].cap ? buf[b].cap : BUF_CAP;
      while (nc < buf[b].len + n && nc < BUF_MAX) nc *= 2;
      char *np = nc >= buf[b].len + n ? realloc(buf[b].p, nc) : NULL;
      if (!np) {
        if (!dropped_bytes) xts_screen_error("sink buffer full (%lu bytes): output dropped", (unsigned long)buf[b].len);
        dropped_bytes += (unsigned)n;
        return;
      }
      buf[b].p = np;
      buf[b].cap = nc;
    }
  }
  memcpy(buf[b].p + buf[b].len, s, n);
  buf[b].len += n;
}

size_t xts_sink_pending(void)
{
  size_t m = 0;
  for (int b = 0; b < NB; b++)
    if (buf[b].len > m) m = buf[b].len;
  return m;
}

static void bprintf(int b, const char *fmt, ...)
{
  char tmp[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof tmp - 1) n = sizeof tmp - 1;
  if (n > 0) bput(b, tmp, (size_t)n);
}

const char *xts_sink_dir(void) { return dir; }
const char *xts_run_dir(void) { return opened ? dir : ""; }
uint64_t xts_sink_seq(void) { return seq; }

int xts_sink_open(const char *d, const char *rid, uint64_t first_seq)
{
  char path[320];
  snprintf(dir, sizeof dir, "%s", d);
  snprintf(run_id, sizeof run_id, "%s", rid);
  seq = first_seq;
  memset(st_count, 0, sizeof st_count);
  int fresh[NB], bad = 0;
  for (int b = 0; b < NB; b++) {
    fresh[b] = !xts_file_exists(xts_path(path, sizeof path, dir, fname[b], NULL, NULL));
    if (xts_pf_open(&pf[b], path) < 0) bad = -1;
  }
  opened = 1;
  if (fresh[B_CSV]) xts_pf_write(&pf[B_CSV], CSV_HEADER, strlen(CSV_HEADER));
  if (fresh[B_TEMPS]) {
    const char *h = "block,tb,cpu,gpu,edram,board\n";
    xts_pf_write(&pf[B_TEMPS], h, strlen(h));
  }
  for (int b = 0; b < NB; b++) xts_pf_sync(&pf[b]);
  return bad;
}

void xts_sink_flush(void)
{
  if (!opened) return;
  uint64_t t0 = xts_tb();
  for (int b = 0; b < NB; b++) {
    if (buf[b].len) {
      xts_pf_write(&pf[b], buf[b].p, buf[b].len);
      buf[b].len = 0;
    }
    xts_pf_sync(&pf[b]);
  }
  flush_ms_total += (double)(xts_tb() - t0) * 1000.0 / (double)xts_tb_hz();
}

int xts_sink_boundary(void)
{
  if (!opened) return 0;
  xts_sink_flush();
  if (xts_storage_dead() || xts_write_refused("flush", "(volume)")) return -1;
  uint64_t t0 = xts_tb();
  xts_fat_busy++;
  int r = xts_plat_storage_flush();
  xts_fat_busy--;
  flush_ms_total += (double)(xts_tb() - t0) * 1000.0 / (double)xts_tb_hz();
  if (r) xts_io_error("flush (fatFlush)", xts_plat_root());
  return r ? -1 : 0;
}

void xts_sink_flush_block(const char *key)
{
  if (!opened) return;
  size_t bytes = 0;
  for (int b = 0; b < NB; b++) bytes += buf[b].len;
  uint64_t t0 = xts_tb();
  xts_sink_boundary();
  double ms = (double)(xts_tb() - t0) * 1000.0 / (double)xts_tb_hz();
  uint64_t cn = 0;
  double cms = 0, cmax = 0;
  xts_slot_crumb_block_stats(&cn, &cms, &cmax);
  xts_log("flush: block %s %lu bytes in %.1f ms (write + fsync + fatFlush); breadcrumbs %llu in %.1f ms (max %.2f ms)", key,
          (unsigned long)bytes, ms, (unsigned long long)cn, cms, cmax);   /* written with the next flush */
}

double xts_sink_flush_ms_total(void) { return flush_ms_total; }
const uint32_t *xts_sink_status_counts(void) { return st_count; }

void xts_sink_discard(void)
{
  for (int b = 0; b < NB; b++) buf[b].len = 0;
}

void xts_sink_close(void)
{
  xts_sink_flush();
  for (int b = 0; b < NB; b++) xts_pf_close(&pf[b]);
  opened = 0;
}

void xts_sink_set_block(const char *key, const xts_condvec_t *cv, const char *family)
{
  snprintf(cur_block, sizeof cur_block, "%s", key ? key : "");
  snprintf(cur_family, sizeof cur_family, "%s", family ? family : "");
  if (cv) cur_cv = *cv;
  else memset(&cur_cv, 0, sizeof cur_cv);
}

void xts_sink_log_raw(const char *line)
{
  bput(B_LOG, line, strlen(line));
  bput(B_LOG, "\n", 1);
}

void xts_log(const char *fmt, ...)
{
  char line[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  char ts[700];
  snprintf(ts, sizeof ts, "[%10.3f] %s", (double)xts_tb() / (double)xts_tb_hz(), line);
  xts_crash_log_note(ts);         /* RAM ring of the last lines: the crash report quotes it */
  xts_sink_log_raw(ts);           /* run.log only; before xts_sink_open it stays buffered until the first flush */
}

void xts_screen_error(const char *fmt, ...)
{
  char line[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  xts_plat_screen(line);
  xts_log("%s", line);
}

/* status line: at most once per second unless forced (libxenon redraws/flushes the framebuffer per character) */
static uint64_t status_last;
static int status_any;
static void status_v(int force, const char *fmt, va_list ap)
{
  uint64_t now = xts_tb();
  if (!force && status_any && now - status_last < xts_tb_hz()) return;
  status_last = now;
  status_any = 1;
  char line[300];
  vsnprintf(line, sizeof line, fmt, ap);
  xts_plat_status(line);
}

void xts_status_line(int force, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  status_v(force, fmt, ap);
  va_end(ap);
}

void xts_status(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  status_v(0, fmt, ap);
  va_end(ap);
}

void xts_record_init(xts_record_t *r)
{
  memset(r, 0, sizeof *r);
  r->value = r->n = r->min = r->median = r->mad = r->fit_r2 = NAN;
  r->metric = "result";
  r->unit = "";
  r->status = XTS_ST_PASS;
}

static void csv_field(char *out, size_t cap, const char *s)
{
  if (!strpbrk(s, ",\"\n")) { snprintf(out, cap, "%s", s); return; }
  size_t o = 0;
  if (o < cap - 1) out[o++] = '"';
  for (; *s && o + 3 < cap; s++) {
    if (*s == '"') out[o++] = '"';
    out[o++] = *s == '\n' ? ' ' : *s;
  }
  out[o++] = '"';
  out[o] = 0;
}

static void num(char *out, size_t cap, double v)
{
  if (isnan(v) || isinf(v)) snprintf(out, cap, "null");
  else snprintf(out, cap, "%.10g", v);
}

/* vocabulary check (host): unknown metric/unit -> logged once per name, "vocab_warning" added to the detail */
static const char *vocab_detail(const xts_record_t *r, char *out, int cap)
{
  static char seen[32][40];
  static int nseen;
  const char *metric = r->metric ? r->metric : "result", *unit = r->unit ? r->unit : "";
  int mk = xts_metric_known(metric), uk = xts_unit_known(unit);
  if (mk && uk) return r->detail;
  char what[160];
  snprintf(what, sizeof what, "%s%s%s%s%s", mk ? "" : "metric '", mk ? "" : metric, mk ? "" : "' ",
           uk ? "" : "unit '", uk ? "" : unit);
  snprintf(what + strlen(what), sizeof what - strlen(what), "%snot in the vocabulary", uk ? "" : "' ");
  char key[40];
  snprintf(key, sizeof key, "%s|%s", mk ? "" : metric, uk ? "" : unit);
  int dup = 0;
  for (int i = 0; i < nseen; i++) dup |= !strcmp(seen[i], key);
  if (!dup) {
    if (nseen < 32) snprintf(seen[nseen++], sizeof seen[0], "%s", key);
    xts_log("record vocabulary: %s (family %s)", what, cur_family);
  }
  const char *d = r->detail && r->detail[0] ? r->detail : "{}";
  size_t n = strlen(d);
  while (n && (d[n - 1] == ' ' || d[n - 1] == '\n')) n--;
  if (n < 2 || d[0] != '{' || d[n - 1] != '}') return r->detail;
  int empty = n == 2;
  int w = snprintf(out, cap, "%.*s%s\"vocab_warning\":\"%s\"}", (int)(n - 1), d, empty ? "" : ",", what);
  return (w > 0 && w < cap) ? out : r->detail;
}

static void write_record(const char *family, const char *case_id, const char *mnem, int has_word, uint32_t word,
                         const char *block, const xts_condvec_t *cv, const xts_record_t *r)
{
  static char json[24576];        /* a 16 KB detail (timing emitter) + the other fields */
  static char vdet[24576];
  xts_record_t rv;
  if (xts_vocab_check) {
    const char *d = vocab_detail(r, vdet, sizeof vdet);
    if (d != r->detail) { rv = *r; rv.detail = d; r = &rv; }
  }
  char wbuf[12], cond[512], v[6][32];
  xts_json_t j;
  seq++;
  if (has_word) snprintf(wbuf, sizeof wbuf, "%08X", word);
  else wbuf[0] = 0;
  xj_init(&j, json, sizeof json - 2);
  xj_obj(&j);
  xj_kv_str(&j, "run_id", run_id);
  xj_kv_uint(&j, "seq", seq);
  xj_kv_str(&j, "family", family);
  xj_kv_str(&j, "case_id", case_id);
  xj_kv_str(&j, "mnem", mnem ? mnem : "");
  xj_kv_str(&j, "word", wbuf);
  xj_key(&j, "cond");
  xj_obj(&j);
  for (int i = 0; cv && i < cv->n; i++) xj_kv_str(&j, cv->axis[i]->name, cv->axis[i]->values[cv->value[i]]);
  xj_end(&j);
  xj_kv_str(&j, "block", block);
  xj_kv_str(&j, "metric", r->metric ? r->metric : "result");
  xj_kv_double(&j, "value", r->value);
  xj_kv_str(&j, "unit", r->unit ? r->unit : "");
  xj_kv_double(&j, "n", r->n);
  xj_kv_double(&j, "min", r->min);
  xj_kv_double(&j, "median", r->median);
  xj_kv_double(&j, "mad", r->mad);
  xj_kv_double(&j, "fit_r2", r->fit_r2);
  xj_kv_str(&j, "status", xts_status_name(r->status));
  xj_key(&j, "detail");
  xj_raw(&j, r->detail && r->detail[0] ? r->detail : "{}");
  if (r->detail_truncated || j.truncated) {
    xj_kv_bool(&j, "truncated", 1);
  }
  xj_finish(&j);
  if (j.truncated && !strstr(json, "\"truncated\"")) {
    /* the detail did not fit: write the record without it */
    xts_record_t r2 = *r;
    r2.detail = "{\"truncated\":true}";
    r2.detail_truncated = 1;
    seq--;
    write_record(family, case_id, mnem, has_word, word, block, cv, &r2);
    return;
  }
  if (xts_sink_tap) xts_sink_tap(family, case_id, r, json);
  if (r->status >= 0 && r->status < XTS_ST_COUNT) st_count[r->status]++;
  bput(B_RESULTS, json, strlen(json));
  bput(B_RESULTS, "\n", 1);
  if (r->status == XTS_ST_FAIL || r->status == XTS_ST_HANG || r->status == XTS_ST_CRASH) {
    bput(B_FAIL, json, strlen(json));
    bput(B_FAIL, "\n", 1);
  }
  xts_cond_format(cv, cond, sizeof cond, 0);
  char f_case[160], f_cond[600], f_fam[64], f_block[200], f_metric[64], f_unit[32];
  csv_field(f_case, sizeof f_case, case_id);
  csv_field(f_cond, sizeof f_cond, cond);
  csv_field(f_fam, sizeof f_fam, family);
  csv_field(f_block, sizeof f_block, block);
  csv_field(f_metric, sizeof f_metric, r->metric ? r->metric : "result");
  csv_field(f_unit, sizeof f_unit, r->unit ? r->unit : "");
  num(v[0], 32, r->value); num(v[1], 32, r->n); num(v[2], 32, r->min); num(v[3], 32, r->median);
  num(v[4], 32, r->mad); num(v[5], 32, r->fit_r2);
  bprintf(B_CSV, "%s,%llu,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", run_id, (unsigned long long)seq, f_fam, f_case,
          mnem ? mnem : "", wbuf, f_cond, f_block, f_metric, v[0], f_unit, v[1], v[2], v[3], v[4], v[5],
          xts_status_name(r->status));
}

void xts_emit(xts_ctx_t *ctx, const xts_record_t *r)
{
  const xts_case_t *c = ctx->c;
  const char *mnem = r->mnem ? r->mnem : (c && c->insn >= 0 && c->insn < xts_isa_count ? xts_isa[c->insn].mnem : "");
  write_record(xts_family_name(ctx->fam), r->case_id ? r->case_id : (c ? c->id : ""), mnem, r->has_word, r->word,
               ctx->block_key ? ctx->block_key : cur_block, ctx->cond ? ctx->cond : &cur_cv, r);
  ctx->nrecords++;
}

void xts_emit_status(xts_ctx_t *ctx, int status, const char *detail)
{
  xts_record_t r;
  xts_record_init(&r);
  r.status = status;
  r.detail = detail;
  xts_emit(ctx, &r);
}

void xts_emit_value(xts_ctx_t *ctx, const char *metric, double value, const char *unit, int status)
{
  xts_record_t r;
  xts_record_init(&r);
  r.metric = metric;
  r.value = value;
  r.unit = unit;
  r.status = status;
  xts_emit(ctx, &r);
}

void xts_sink_emit_core(const char *family, const char *case_id, const char *block_key, const xts_condvec_t *cv,
                        int status, const char *metric, const char *detail)
{
  xts_record_t r;
  xts_record_init(&r);
  r.status = status;
  r.metric = metric ? metric : "result";
  r.detail = detail;
  write_record(family, case_id, "", 0, 0, block_key, cv, &r);
}

void xts_emit_block_info(xts_ctx_t *ctx, const char *case_id, const char *metric, double value, const char *unit,
                         const char *detail)
{
  xts_record_t r;
  xts_record_init(&r);
  r.case_id = case_id ? case_id : "block";
  r.metric = metric ? metric : "info";
  r.value = value;
  r.unit = unit ? unit : "";
  r.status = XTS_ST_INFO;
  r.detail = detail;
  r.mnem = "";
  if (ctx) { xts_emit(ctx, &r); return; }
  write_record(cur_family[0] ? cur_family : "core", r.case_id, "", 0, 0, cur_block[0] ? cur_block : "-", &cur_cv, &r);
}

int xts_results_foreach(const char *family, int (*cb)(const char *json_line, void *ud), void *ud)
{
  char path[320], needle[96];
  if (!opened) return -1;
  if (xts_write_refused("read-back (xts_results_foreach flushes the sinks)", "results.jsonl")) return -1;
  xts_sink_flush();
  xts_pf_close(&pf[B_RESULTS]);   /* no second handle on a file open for writing (libfat) */
  xts_fat_busy++;
  FILE *f = fopen(xts_path(path, sizeof path, dir, "results.jsonl", NULL, NULL), "r");
  xts_fat_busy--;
  if (!f) { xts_pf_open(&pf[B_RESULTS], pf[B_RESULTS].path); return 0; }
  xts_fat_busy++;
  static char line[32768];
  if (family) snprintf(needle, sizeof needle, "\"family\":\"%s\"", family);
  int n = 0, skipping = 0;
  while (fgets(line, sizeof line, f)) {
    size_t l = strlen(line);
    int complete = l && line[l - 1] == '\n';
    if (skipping) { skipping = !complete; continue; }    /* tail of an over-long line */
    if (!complete) { skipping = 1; continue; }
    line[--l] = 0;
    if (family && !strstr(line, needle)) continue;
    n++;
    xts_fat_busy--;
    int stop = cb(line, ud);
    xts_fat_busy++;
    if (stop) break;
  }
  fclose(f);
  xts_fat_busy--;
  xts_pf_open(&pf[B_RESULTS], pf[B_RESULTS].path);
  return n;
}

void xts_sink_temps(const char *block_key)
{
  uint16_t t[4];
  if (xts_smc_temps(t) < 0) return;
  bprintf(B_TEMPS, "%s,%llu,%.2f,%.2f,%.2f,%.2f\n", block_key, (unsigned long long)xts_tb(), t[0] / 256.0, t[1] / 256.0,
          t[2] / 256.0, t[3] / 256.0);
}

/* ---- summary.json from results.csv (covers every boot of the run) ---- */
#define SUM_MAX 512
static struct { char key[96]; uint32_t c[XTS_ST_COUNT]; } sum[SUM_MAX];
static int nsum;

static void count(const char *key, int st)
{
  for (int i = 0; i < nsum; i++)
    if (!strcmp(sum[i].key, key)) { sum[i].c[st]++; return; }
  if (nsum >= SUM_MAX) return;
  snprintf(sum[nsum].key, sizeof sum[nsum].key, "%s", key);
  memset(sum[nsum].c, 0, sizeof sum[nsum].c);
  sum[nsum++].c[st]++;
}

/* split one CSV line into fields (handles quotes); returns field count */
static int csv_split(char *line, char **f, int max)
{
  int n = 0;
  char *p = line;
  while (*p && n < max) {
    if (*p == '"') {
      char *o = ++p;
      f[n++] = o;
      while (*p) {
        if (*p == '"' && p[1] == '"') { *o++ = '"'; p += 2; continue; }
        if (*p == '"') { p++; break; }
        *o++ = *p++;
      }
      *o = 0;
      if (*p == ',') p++;
    } else {
      f[n++] = p;
      while (*p && *p != ',' && *p != '\n' && *p != '\r') p++;
      if (*p == ',') *p++ = 0;
      else { *p = 0; break; }
    }
  }
  return n;
}

int xts_sink_summary(const char *d)
{
  char path[320], line[4096];      /* results.csv rows: no detail column */
  nsum = 0;
  if (xts_storage_dead() || xts_write_refused("summary", "summary.json")) return -1;
  if (opened) xts_pf_close(&pf[B_CSV]);
  xts_fat_busy++;
  FILE *f = fopen(xts_path(path, sizeof path, d, "results.csv", NULL, NULL), "r");
  xts_fat_busy--;
  if (opened) xts_pf_open(&pf[B_CSV], pf[B_CSV].path);
  if (!f) return -1;
  xts_fat_busy++;
  uint32_t total[XTS_ST_COUNT] = { 0 };
  uint64_t nrec = 0;
  while (fgets(line, sizeof line, f)) {
    char *fl[20];
    if (!strncmp(line, "run_id,", 7)) continue;
    int n = csv_split(line, fl, 20);
    if (n < 17) continue;
    int st = -1;
    for (int s = 0; s < XTS_ST_COUNT; s++)
      if (!strcmp(fl[16], xts_status_name(s))) st = s;
    if (st < 0) continue;
    nrec++;
    total[st]++;
    char key[96];
    snprintf(key, sizeof key, "F%s", fl[2]);
    count(key, st);
    char *cond = fl[6], *save = NULL;
    for (char *kv = strtok_r(cond, ";", &save); kv; kv = strtok_r(NULL, ";", &save)) {
      snprintf(key, sizeof key, "A%s", kv);
      count(key, st);
    }
  }
  fclose(f);
  xts_fat_busy--;
  static char out[1 << 17];
  xts_json_t j;
  xj_init(&j, out, sizeof out);
  xj_obj(&j);
  xj_kv_str(&j, "run_id", run_id);
  xj_kv_str(&j, "profile", xts_active_profile_name ? xts_active_profile_name : "");
  xj_kv_uint(&j, "records", nrec);
  xj_key(&j, "status");
  xj_obj(&j);
  for (int s = 0; s < XTS_ST_COUNT; s++) xj_kv_uint(&j, xts_status_name(s), total[s]);
  xj_end(&j);
  for (int pass = 0; pass < 2; pass++) {
    xj_key(&j, pass ? "by_axis_value" : "by_family");
    xj_obj(&j);
    for (int i = 0; i < nsum; i++) {
      if (sum[i].key[0] != (pass ? 'A' : 'F')) continue;
      xj_key(&j, sum[i].key + 1);
      xj_obj(&j);
      for (int s = 0; s < XTS_ST_COUNT; s++)
        if (sum[i].c[s]) xj_kv_uint(&j, xts_status_name(s), sum[i].c[s]);
      xj_end(&j);
    }
    xj_end(&j);
  }
  xj_kv_uint(&j, "write_errors", io_errors);
  xj_kv_uint(&j, "guard_violations", guard_violations);   /* file operations attempted while cases ran (refused) */
  xj_kv_uint(&j, "dropped_bytes", dropped_bytes);
  char cs[600];
  xts_slot_crumb_stats(cs, sizeof cs);
  xj_kv_str(&j, "breadcrumbs", cs);
  xj_kv_double(&j, "flush_ms_total", flush_ms_total);
  xts_storage_status_t sst;
  xts_plat_storage_status(&sst);
  xj_key(&j, "storage");                       /* the disc guard's view of this boot (src/plat/disc_guard.c) */
  xj_obj(&j);
  xj_kv_bool(&j, "guard", sst.guard);
  xj_kv_bool(&j, "poisoned", sst.poisoned);
  xj_kv_uint(&j, "sectors_read", sst.sectors_read);
  xj_kv_uint(&j, "sectors_written", sst.sectors_written);
  xj_kv_uint(&j, "device_reads", sst.device_reads);
  xj_kv_uint(&j, "device_writes", sst.device_writes);
  xj_kv_uint(&j, "read_retries", sst.read_retries);
  xj_kv_uint(&j, "write_repairs", sst.write_repairs);
  xj_kv_double(&j, "guard_ms", sst.guard_ms);
  xj_end(&j);
  xj_kv_bool(&j, "truncated", j.truncated);
  xj_finish(&j);
  return xts_write_small(xts_path(path, sizeof path, d, "summary.json", NULL, NULL), out);
}
