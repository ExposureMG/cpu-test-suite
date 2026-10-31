/* Output sink: every record is one REC message (the exact results.jsonl line), every xts_log line one LOG message, block
 * ends are BLOCK messages, the run starts with HELLO x3 and ends with END x3 (docs/PROTOCOL.md). Nothing is written on
 * the console: results.csv, failures.jsonl, summary.json and the rest are produced by the receiver (tools/xts_recv.py).
 *
 * Every send goes through xts_core_send, which keeps what END reports: messages per type, the last frame seq, send
 * failures (messages that could not be queued) and rec_hash (FNV-1a 64 over the REC payloads in message order).
 * REC lines are also kept in the in-RAM results store (results.c) for xts_results_foreach.
 * Log lines before HELLO (and in host unit tests without a run) are kept in a RAM buffer and sent right after HELLO.
 * Sends happen only on the suite core between test-core executions, never while a case is being measured. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"
#include "netfmt.h"

static char run_id[40], tag[64];
static uint64_t seq;
static int opened, helloed;
static char cur_block[160], cur_family[48];
static xts_condvec_t cur_cv;
static uint32_t st_count[XTS_ST_COUNT];
static uint64_t msg_count[XTS_PK_MAX + 1], rec_hash = XTS_FNV64_INIT, block_records;
static uint32_t last_seq, send_failures;
static int sending;               /* recursion guard: a failing send logs, the log line sends */
static uint64_t sess_id;          /* multi-pass session (xts_sink_set_session); sess_passes 0 = none */
static int sess_pass, sess_passes;
xts_sink_tap_fn xts_sink_tap;
#ifdef XTS_HOST
int xts_vocab_check = 1;
#else
int xts_vocab_check = 0;      /* console: the host dry run already checked every family's records */
#endif

/* log lines waiting for HELLO */
#define PRE_CAP (256u << 10)
static char *pre;
static size_t pre_len;
static unsigned pre_dropped;

uint32_t xts_core_send(int type, const char *payload, uint32_t len)
{
  if (sending) return 0;
  sending = 1;
  uint32_t s = xts_net_send(type, payload, len);
  if (s) {
    if (type > 0 && type <= XTS_PK_MAX) msg_count[type]++;
    if (s > last_seq) last_seq = s;
    if (type == XTS_PK_REC) rec_hash = xts_fnv1a64(rec_hash, payload, len);
  } else {
    send_failures++;
  }
  sending = 0;
  if (!s && send_failures <= 5)
    xts_screen_error("transport: %s message of %u bytes could not be queued (failure #%u)", xts_net_type_name(type),
                     (unsigned)len, (unsigned)send_failures);
  return s;
}

uint32_t xts_sink_send_failures(void) { return send_failures; }
uint64_t xts_sink_msg_count(int type) { return type > 0 && type <= XTS_PK_MAX ? msg_count[type] : 0; }
const uint32_t *xts_sink_status_counts(void) { return st_count; }
int xts_sink_is_open(void) { return opened; }
uint64_t xts_sink_seq(void) { return seq; }
const char *xts_sink_run_id(void) { return run_id; }
const xts_condvec_t *xts_sink_block_cv(void) { return &cur_cv; }
/* where the receiver files this run (relative to its output directory) */
const char *xts_run_dir(void)
{
  static char d[120];
  if (!opened) return "";
  snprintf(d, sizeof d, "%s/%s", tag, run_id);
  return d;
}

int xts_sink_open(const char *rid, const char *t)
{
  snprintf(run_id, sizeof run_id, "%s", rid ? rid : "");
  snprintf(tag, sizeof tag, "%s", t ? t : "");
  seq = 0;
  memset(st_count, 0, sizeof st_count);
  memset(msg_count, 0, sizeof msg_count);
  rec_hash = XTS_FNV64_INIT;
  last_seq = send_failures = 0;
  block_records = 0;
  xts_results_reset();
  opened = 1;
  helloed = 0;
  return 0;
}

static void pre_flush(void)
{
  size_t o = 0;
  while (o < pre_len) {
    size_t l = strlen(pre + o);
    xts_core_send(XTS_PK_LOG, pre + o, (uint32_t)l);
    o += l + 1;
  }
  pre_len = 0;
  if (pre_dropped) {
    char l[96];
    snprintf(l, sizeof l, "[%10.3f] %u log line(s) before HELLO did not fit the buffer and were dropped",
             (double)xts_tb() / (double)xts_tb_hz(), pre_dropped);
    xts_core_send(XTS_PK_LOG, l, (uint32_t)strlen(l));
    pre_dropped = 0;
  }
}

void xts_sink_hello(const char *hello)
{
  uint32_t n = (uint32_t)strlen(hello);
  for (int i = 0; i < 3; i++) xts_core_send(XTS_PK_HELLO, hello, n);
  helloed = 1;
  pre_flush();
}

void xts_sink_discard(void)
{
  pre_len = 0;
  pre_dropped = 0;
}

void xts_sink_close(void)
{
  opened = helloed = 0;
}

void xts_sink_set_session(uint64_t id, int pass, int passes)
{
  sess_id = id;
  sess_pass = pass;
  sess_passes = passes > 1 ? passes : 0;
}

void xts_sink_set_block(const char *key, const xts_condvec_t *cv, const char *family)
{
  snprintf(cur_block, sizeof cur_block, "%s", key ? key : "");
  snprintf(cur_family, sizeof cur_family, "%s", family ? family : "");
  if (cv) cur_cv = *cv;
  else memset(&cur_cv, 0, sizeof cur_cv);
  block_records = 0;
}

void xts_sink_block_end(const char *key, const char *extra)
{
  if (!helloed) return;
  char b[1024];
  xts_json_t j;
  xj_init(&j, b, sizeof b);
  xj_obj(&j);
  xj_kv_str(&j, "block", key);
  xj_kv_uint(&j, "records", block_records);
  xj_kv_uint(&j, "seq", seq);
  if (extra && extra[0]) xj_members(&j, extra);
  xj_finish(&j);
  xts_core_send(XTS_PK_BLOCK, b, (uint32_t)j.len);
  block_records = 0;
}

int xts_sink_end(const char *status)
{
  if (!helloed) return -1;
  char b[2048], h[20];
  xts_json_t j;
  xj_init(&j, b, sizeof b);
  xj_obj(&j);
  xj_kv_str(&j, "status", status);
  xj_kv_uint(&j, "last_seq", last_seq);
  xj_key(&j, "messages");
  xj_obj(&j);
  for (int t = 1; t <= XTS_PK_MAX; t++)
    if (t != XTS_PK_END) xj_kv_uint(&j, xts_net_type_name(t), msg_count[t]);
  xj_end(&j);
  snprintf(h, sizeof h, "%016llx", (unsigned long long)rec_hash);
  xj_kv_str(&j, "rec_hash", h);
  xj_kv_str(&j, "run_id", run_id);
  xj_kv_uint(&j, "records", seq);
  xj_kv_uint(&j, "send_failures", send_failures);
  if (sess_passes) {
    xj_key(&j, "session");
    xj_obj(&j);
    char sid[20];
    snprintf(sid, sizeof sid, "%016llx", (unsigned long long)sess_id);   /* = pass 1's run_id */
    xj_kv_str(&j, "id", sid);
    xj_kv_int(&j, "pass", sess_pass);
    xj_kv_int(&j, "passes", sess_passes);
    xj_end(&j);
  }
  xj_key(&j, "status_counts");
  xj_obj(&j);
  for (int s = 0; s < XTS_ST_COUNT; s++) xj_kv_uint(&j, xts_status_name(s), st_count[s]);
  xj_end(&j);
  xts_net_stats_t ns;
  memset(&ns, 0, sizeof ns);
  xts_net_stats(&ns);
  xj_key(&j, "net");
  xj_obj(&j);
  xj_kv_uint(&j, "frames", ns.frames);
  xj_kv_uint(&j, "frame_copies", ns.frame_copies);
  xj_kv_uint(&j, "bytes", ns.bytes);
  xj_kv_uint(&j, "ring_full_waits", ns.ring_full_waits);
  xj_kv_uint(&j, "send_failures", ns.send_failures);
  xj_kv_uint(&j, "tx_timeouts", ns.tx_timeouts);
  xj_kv_uint(&j, "fence_waits", ns.fence_waits);
  xj_kv_uint(&j, "fence_wait_ticks", ns.fence_wait_ticks);
  xj_kv_uint(&j, "wake_windows", ns.wake_windows);
  xj_end(&j);
  xj_finish(&j);
  for (int i = 0; i < 3; i++) xts_core_send(XTS_PK_END, b, (uint32_t)j.len);
  return xts_net_drain(10u * 1000u * 1000u);
}

/* ---- log ---- */
static void log_line(const char *line)
{
  size_t n = strlen(line);
  if (helloed) {
    xts_core_send(XTS_PK_LOG, line, (uint32_t)n);
    return;
  }
  if (!pre) pre = malloc(PRE_CAP);
  if (!pre || pre_len + n + 1 > PRE_CAP) { pre_dropped++; return; }
  memcpy(pre + pre_len, line, n + 1);
  pre_len += n + 1;
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
  xts_crash_log_note(ts);         /* RAM ring of the last lines: crash/hang reports quote it */
  log_line(ts);
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

/* ---- records ---- */
void xts_record_init(xts_record_t *r)
{
  memset(r, 0, sizeof *r);
  r->value = r->n = r->min = r->median = r->mad = r->fit_r2 = NAN;
  r->metric = "result";
  r->unit = "";
  r->status = XTS_ST_PASS;
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
  static char json[24576];        /* a 16 KB detail (timing emitter) + the other fields; < XTS_NET_MAX_MESSAGE */
  static char vdet[24576];
  xts_record_t rv;
  if (xts_vocab_check) {
    const char *d = vocab_detail(r, vdet, sizeof vdet);
    if (d != r->detail) { rv = *r; rv.detail = d; r = &rv; }
  }
  char wbuf[12];
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
  block_records++;
  if (!opened) return;
  uint32_t len = (uint32_t)strlen(json);
  xts_results_add(family, seq, json, len);
  if (helloed) xts_core_send(XTS_PK_REC, json, len);   /* before HELLO (host unit tests): kept in RAM only */
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
