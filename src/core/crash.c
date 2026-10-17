/* Crash path and breadcrumbs (RAM side). See core.h "crash reports and breadcrumbs" and src/core/slot.c.
 *
 * xts_crumb (RAM) always says what PIR 0 is doing: run, block, case (index, id, family, mnemonic), phase, sink seq and,
 * in self/semantic blocks, the last executed word and its input registers (an exec pre/post hook). main.c writes it to
 * progress.bin before cases (xts_slot_crumb_write) for hangs; the crash path quotes it in crash.json.
 *
 * xts_crash_handle runs from exception context (console: vec.S crashdump -> plat xts_crash_entry, on an emergency
 * stack): no libfat, no malloc, no stdio (own formatting; slot.c's packer; xts_plat_raw_write = the disc guard's raw
 * API). PIR != 0: the thread slot only. PIR 0: unless storage is busy, poisoned or the slot is not armed, the report goes
 * into crash.json, sector 0 last. libfat is never entered and nothing is flushed or unmounted: the stick keeps what the
 * last block boundary left (plus the raw sectors of our own two files).
 */
#include <string.h>
#include <xts/exec.h>
#include "core.h"

#define XTS_LOGRING 50
#define RING_W 160

xts_crumb_t xts_crumb;
xts_thrcrash_t xts_thrcrash[XTS_CRASH_THREADS];
char xts_crash_screen[XTS_CRASH_LINES][120];
volatile int xts_crash_outcome = -1;

static char ring[XTS_LOGRING][RING_W];
static volatile uint32_t ring_n;

static const char *const phase_names[] = { "idle", "block_start", "case", "between_cases", "block_end", "finish", "run_end" };
const char *xts_phase_name(int ph) { return ph >= 0 && ph <= XTS_PH_RUN_END ? phase_names[ph] : "?"; }

static const char *const outcome_names[] = { "saved", "saved_fs_busy", "not_saved_storage_busy", "not_saved_poisoned",
                                             "not_saved_no_slot", "not_saved_write_error", "thread_recorded" };
const char *xts_crash_outcome_name(int o) { return o >= 0 && o <= XTS_CRASH_THREAD ? outcome_names[o] : "?"; }

/* ---- crash-safe string helpers ---- */
static void scopy(char *d, size_t cap, const char *s)
{
  size_t i = 0;
  for (; s && s[i] && i + 1 < cap; i++) d[i] = s[i];
  d[i] = 0;
}

void xts_crash_log_note(const char *line)
{
  uint32_t i = ring_n % XTS_LOGRING;
  char *d = ring[i];
  d[0] = 0;
  scopy(d, RING_W, line);
  ring_n++;
}

/* ---- breadcrumb (RAM) ---- */
void xts_crumb_block(const char *run_id, int boot, const char *block, const char *family, const char *kind, int every)
{
  xts_crumb_t *c = &xts_crumb;
  scopy(c->run_id, sizeof c->run_id, run_id);
  c->boot = boot;
  scopy(c->block, sizeof c->block, block);
  scopy(c->family, sizeof c->family, family);
  scopy(c->kind, sizeof c->kind, kind);
  c->case_id[0] = c->mnem[0] = 0;
  c->case_rel = c->case_index = -1;
  c->repeat = 0;
  c->every = every;
  c->ncase = 0;
  c->exec_valid = c->in_exec = 0;
  c->phase = XTS_PH_BLOCK_START;
}

void xts_crumb_case(int rel, int case_index, int repeat, const char *case_id, const char *mnem, uint64_t seq, uint64_t seed)
{
  xts_crumb_t *c = &xts_crumb;
  c->phase = XTS_PH_BETWEEN;            /* fields change: not "in case" until they are consistent */
  c->case_rel = rel;
  c->case_index = case_index;
  c->repeat = repeat;
  scopy(c->case_id, sizeof c->case_id, case_id);
  scopy(c->mnem, sizeof c->mnem, mnem);
  c->seq = seq;
  c->seed = seed;
  c->tb = xts_tb();
  c->exec_valid = c->in_exec = 0;
  c->phase = XTS_PH_CASE;
}

void xts_crumb_phase(int phase)
{
  xts_crumb.in_exec = 0;
  xts_crumb.phase = phase;
}

static int hook_handle = -1;
static int crumb_pre(xts_exec_t *x, void *ud)
{
  (void)ud;
  xts_crumb_t *c = &xts_crumb;
  c->word = x->run_word;
  c->kwords = x->run_kernel ? x->run_kernel_words : 0;
  memcpy(c->gpr_in, x->in.gpr, sizeof c->gpr_in);
  c->lr_in = x->in.lr;
  c->ctr_in = x->in.ctr;
  c->xer_in = x->in.xer;
  c->cr_in = x->in.cr;
  c->execs++;
  c->exec_valid = 1;
  c->in_exec = 1;
  return 0;
}
static void crumb_post(xts_exec_t *x, void *ud)
{
  (void)x;
  (void)ud;
  xts_crumb.in_exec = 0;
}

int xts_crumb_hook_install(void)
{
  if (hook_handle >= 0) return 0;
  hook_handle = xts_exec_hook_add(crumb_pre, crumb_post, NULL);
  return hook_handle >= 0 ? 0 : -1;
}

void xts_crumb_hook_remove(void)
{
  if (hook_handle >= 0) xts_exec_hook_remove(hook_handle);
  hook_handle = -1;
}

/* ---- report builder (crash-safe) ---- */
typedef struct mb {
  char b[640];
  uint32_t n;
} mb_t;
static void mb_c(mb_t *m, char c)
{
  if (m->n + 1 < sizeof m->b) m->b[m->n++] = c;
}
static void mb_s(mb_t *m, const char *s)
{
  while (s && *s) mb_c(m, *s++);
}
static void mb_key(mb_t *m, const char *k)
{
  m->n = 0;
  mb_s(m, ",\"");
  mb_s(m, k);
  mb_s(m, "\":");
}
static void mb_str(mb_t *m, const char *s, uint32_t max)
{
  mb_c(m, '"');
  for (uint32_t i = 0; s && s[i] && i < max; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') { mb_c(m, '\\'); mb_c(m, (char)c); }
    else if (c < 0x20 || c >= 0x7F) mb_c(m, '?');
    else mb_c(m, (char)c);
  }
  mb_c(m, '"');
}
static void mb_hex(mb_t *m, uint64_t v, int digits)
{
  mb_c(m, '"');
  for (int i = digits - 1; i >= 0; i--) mb_c(m, "0123456789ABCDEF"[(v >> (4 * i)) & 15]);
  mb_c(m, '"');
}
static void mb_dec(mb_t *m, int64_t v)
{
  char t[24];
  int k = 0;
  uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u && k < 22);
  if (v < 0) mb_c(m, '-');
  while (k) mb_c(m, t[--k]);
}

static xts_spack_t pk;
static mb_t mbuf;
static void put(void) { xts_spack_put(&pk, mbuf.b, mbuf.n); }
static void kv_str(const char *k, const char *v) { mb_key(&mbuf, k); mb_str(&mbuf, v, 200); put(); }
static void kv_hex(const char *k, uint64_t v, int d) { mb_key(&mbuf, k); mb_hex(&mbuf, v, d); put(); }
static void kv_dec(const char *k, int64_t v) { mb_key(&mbuf, k); mb_dec(&mbuf, v); put(); }

/* key with a number in it: "<a><n><b>" */
static void key_n(char *o, const char *a, int n, const char *b)
{
  uint32_t i = 0;
  while (*a && i < 20) o[i++] = *a++;
  if (n >= 10) o[i++] = (char)('0' + n / 10 % 10);
  o[i++] = (char)('0' + n % 10);
  while (*b && i < 30) o[i++] = *b++;
  o[i] = 0;
}

static void put_threads(void)
{
  for (int i = 0; i < XTS_CRASH_THREADS; i++) {
    xts_thrcrash_t *t = &xts_thrcrash[i];
    if (t->state != 1 && t->state != 2) continue;
    char k[32];
    key_n(k, "thr", i + 1, "_pir"); kv_dec(k, t->pir);
    key_n(k, "thr", i + 1, "_vector"); kv_hex(k, t->vector, 4);
    key_n(k, "thr", i + 1, "_srr0"); kv_hex(k, t->srr0, 16);
    key_n(k, "thr", i + 1, "_srr1"); kv_hex(k, t->srr1, 16);
    key_n(k, "thr", i + 1, "_dar"); kv_hex(k, t->dar, 16);
    key_n(k, "thr", i + 1, "_lr"); kv_hex(k, t->lr, 16);
    key_n(k, "thr", i + 1, "_r1"); kv_hex(k, t->r1, 16);
    key_n(k, "thr", i + 1, "_count"); kv_dec(k, t->count);
    key_n(k, "thr", i + 1, "_block"); kv_str(k, t->block);
    key_n(k, "thr", i + 1, "_case_index"); kv_dec(k, t->case_index);
    key_n(k, "thr", i + 1, "_case_rel"); kv_dec(k, t->case_rel);
    key_n(k, "thr", i + 1, "_case_id"); kv_str(k, t->case_id);
  }
}

static void put_crumb(void)
{
  const xts_crumb_t *c = &xts_crumb;
  kv_str("run_id", c->run_id);
  kv_dec("boot", c->boot);
  kv_str("block", c->block);
  kv_dec("case_index", c->phase == XTS_PH_CASE ? c->case_index : -1);
  kv_dec("case_rel", c->phase == XTS_PH_CASE ? c->case_rel : -1);
  kv_str("case_id", c->phase == XTS_PH_CASE ? c->case_id : "");
  kv_str("phase", xts_phase_name(c->phase));
}

static void put_case_detail(void)
{
  const xts_crumb_t *c = &xts_crumb;
  kv_str("last_case_id", c->case_id);
  kv_dec("last_case_index", c->case_index);
  kv_dec("repeat", c->repeat);
  kv_str("family", c->family);
  kv_str("kind", c->kind);
  kv_str("mnem", c->mnem);
  kv_dec("seq", (int64_t)c->seq);
  kv_hex("seed", c->seed, 16);
  kv_dec("breadcrumb_every", c->every);
  kv_dec("cases_started_in_block", c->ncase);
  kv_dec("exec_runs", (int64_t)c->execs);
  kv_dec("in_exec", c->in_exec);
  if (c->exec_valid) {                          /* the case's input state as the exec hook saw it */
    kv_hex("word", c->word, 8);
    kv_dec("kernel_words", c->kwords);
    char k[16];
    for (int r = 0; r < 32; r++) { key_n(k, "in_r", r, ""); kv_hex(k, c->gpr_in[r], 16); }
    kv_hex("in_cr", c->cr_in, 8);
    kv_hex("in_xer", c->xer_in, 16);
    kv_hex("in_lr", c->lr_in, 16);
    kv_hex("in_ctr", c->ctr_in, 16);
  }
}

static void put_log(void)
{
  uint32_t n = ring_n, first = n > XTS_LOGRING ? n - XTS_LOGRING : 0;
  char k[16];
  for (uint32_t i = first, j = 0; i < n; i++, j++) {
    key_n(k, "log_", (int)j, "");
    kv_str(k, ring[i % XTS_LOGRING]);
  }
}

static void put_storage(int busy, int fsbusy, int poisoned)
{
  kv_dec("storage_busy", busy);
  kv_dec("fat_busy", fsbusy);
  kv_dec("storage_poisoned", poisoned);
  kv_dec("cases_running", xts_cases_running);
  kv_str("report_path", xts_slot_crash_path());
  kv_dec("sector_size", xts_slot_sector_size());
}

/* ---- screen lines ---- */
static char *ln_put(char *o, char *end, const char *s)
{
  while (s && *s && o < end) *o++ = *s++;
  *o = 0;
  return o;
}
static char *ln_hex(char *o, char *end, uint64_t v, int digits)
{
  for (int i = digits - 1; i >= 0 && o < end; i--) *o++ = "0123456789ABCDEF"[(v >> (4 * i)) & 15];
  *o = 0;
  return o;
}
static char *ln_dec(char *o, char *end, int64_t v)
{
  char t[24];
  int k = 0;
  uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u && k < 22);
  if (v < 0 && o < end) *o++ = '-';
  while (k && o < end) *o++ = t[--k];
  *o = 0;
  return o;
}
static void lines_clear(void)
{
  for (int i = 0; i < XTS_CRASH_LINES; i++) xts_crash_screen[i][0] = 0;
}
#define LINE(i) xts_crash_screen[i], xts_crash_screen[i] + sizeof xts_crash_screen[i] - 1

static uint8_t img[XTS_CRASH_JSON_BYTES];
static uint64_t report_tb;
uint64_t xts_crash_report_tb(void) { return report_tb; }

/* a report into img (kind "main" / "thread"); returns the number of sectors used */
static uint32_t build(const char *kind, uint32_t vector, const uint64_t *ctx, uint64_t dsisr, int outcome, int busy,
                      int fsbusy, int poisoned)
{
  xts_spack_init(&pk, img, xts_slot_sector_size(), xts_slot_crash_sectors(), xts_slot_magic());
  kv_str("state", "crashed");                   /* the fields the next boot needs come first (sector 0) */
  kv_str("kind", kind);
  put_crumb();
  kv_str("outcome", xts_crash_outcome_name(outcome));
  if (ctx) {
    kv_hex("vector", vector, 4);
    kv_hex("srr0", ctx[36], 16);
    kv_hex("srr1", ctx[37], 16);
    kv_hex("dar", ctx[38], 16);
    if (vector == 0x300 || vector == 0x600) kv_hex("dsisr", dsisr, 8);
    kv_dec("pir", (int64_t)ctx[39]);
    kv_hex("lr", ctx[32], 16);
    kv_hex("ctr", ctx[33], 16);
    kv_hex("cr", ctx[34], 8);
    kv_hex("xer", ctx[35], 16);
    char k[8];
    for (int r = 0; r < 32; r++) { key_n(k, "r", r, ""); kv_hex(k, ctx[r], 16); }
  }
  kv_str("build_id", xts_build_id());
  report_tb = xts_tb();
  kv_dec("tb", (int64_t)report_tb);
  put_storage(busy, fsbusy, poisoned);
  put_case_detail();
  put_threads();
  put_log();
  kv_dec("members_dropped", pk.dropped);
  return xts_spack_used(&pk);
}

/* other hardware thread: its slot only (PIR 0 keeps running and owns the stick) */
static void thread_record(uint32_t vector, const uint64_t *ctx)
{
  uint64_t pir = ctx ? ctx[39] : 0;
  if (pir < 1 || pir > XTS_CRASH_THREADS) return;
  xts_thrcrash_t *t = &xts_thrcrash[pir - 1];
  if (t->state == 1 || t->state == 2) { t->count++; return; }   /* keep the first, count the rest */
  t->vector = vector;
  t->pir = (uint32_t)pir;
  t->count = 1;
  t->srr0 = ctx[36];
  t->srr1 = ctx[37];
  t->dar = ctx[38];
  t->lr = ctx[32];
  t->ctr = ctx[33];
  t->r1 = ctx[1];
  t->r3 = ctx[3];
  t->tb = xts_tb();
  t->case_index = xts_crumb.phase == XTS_PH_CASE ? xts_crumb.case_index : -1;
  t->case_rel = xts_crumb.phase == XTS_PH_CASE ? xts_crumb.case_rel : -1;
  scopy(t->block, sizeof t->block, xts_crumb.block);
  scopy(t->case_id, sizeof t->case_id, xts_crumb.phase == XTS_PH_CASE ? xts_crumb.case_id : "");
#ifdef XTS_CONSOLE
  __asm__ volatile("sync" ::: "memory");
#else
  __sync_synchronize();
#endif
  t->state = 1;
}

int xts_crash_threads_pending(void)
{
  int n = 0;
  for (int i = 0; i < XTS_CRASH_THREADS; i++) n += xts_thrcrash[i].state == 1;
  return n;
}

int xts_crash_threads_persist(void)
{
  if (!xts_crash_threads_pending() || !xts_slots_armed() || xts_plat_storage_busy() || xts_plat_storage_poisoned()) return -1;
  uint32_t used = build("thread", 0, NULL, 0, XTS_CRASH_THREAD, 0, xts_fat_busy, 0);
  if (xts_slot_crash_write(img, used)) return -1;
  for (int i = 0; i < XTS_CRASH_THREADS; i++)
    if (xts_thrcrash[i].state == 1) xts_thrcrash[i].state = 2;
  return 0;
}

int xts_crash_handle(uint32_t vector, const uint64_t *ctx, uint64_t dsisr)
{
  uint64_t pir = ctx ? ctx[39] : 0;
  if (pir != 0) {
    thread_record(vector, ctx);
    return XTS_CRASH_THREAD;
  }
  int phase = xts_crumb.phase;
  int busy = xts_plat_storage_busy(), fsbusy = xts_fat_busy != 0, poisoned = xts_plat_storage_poisoned();
  int outcome, wr = 0;
  if (busy) outcome = XTS_CRASH_NOT_SAVED_BUSY;
  else if (poisoned) outcome = XTS_CRASH_NOT_SAVED_POISONED;
  else if (!xts_slots_armed()) outcome = XTS_CRASH_NOT_SAVED_NOSLOT;
  else {
    outcome = fsbusy ? XTS_CRASH_SAVED_FS_BUSY : XTS_CRASH_SAVED;
    uint32_t used = build("main", vector, ctx, dsisr, outcome, busy, fsbusy, poisoned);
    wr = xts_slot_crash_write(img, used);
    if (wr) outcome = XTS_CRASH_NOT_SAVED_WRITE;
  }
  xts_crash_outcome = outcome;
  /* screen */
  lines_clear();
  char *o = xts_crash_screen[0], *e = o + sizeof xts_crash_screen[0] - 1;
  o = ln_put(o, e, "XTS CRASH vector ");
  o = ln_hex(o, e, vector, 4);
  o = ln_put(o, e, " srr0 ");
  o = ln_hex(o, e, ctx ? ctx[36] : 0, 8);
  o = ln_put(o, e, " dar ");
  o = ln_hex(o, e, ctx ? ctx[38] : 0, 8);
  o = ln_put(o, e, " lr ");
  o = ln_hex(o, e, ctx ? ctx[32] : 0, 8);
  o = xts_crash_screen[1];
  e = o + sizeof xts_crash_screen[1] - 1;
  o = ln_put(o, e, "in ");
  o = ln_put(o, e, xts_phase_name(phase));
  o = ln_put(o, e, " block ");
  o = ln_put(o, e, xts_crumb.block[0] ? xts_crumb.block : "-");
  if (phase == XTS_PH_CASE) {
    o = ln_put(o, e, " case ");
    o = ln_dec(o, e, xts_crumb.case_rel);
    o = ln_put(o, e, " ");
    o = ln_put(o, e, xts_crumb.case_id);
  }
  o = xts_crash_screen[2];
  e = o + sizeof xts_crash_screen[2] - 1;
  if (outcome == XTS_CRASH_SAVED || outcome == XTS_CRASH_SAVED_FS_BUSY) {
    o = ln_put(o, e, "report saved to ");
    o = ln_put(o, e, xts_slot_crash_path());
  } else {
    o = ln_put(o, e, "report NOT saved: ");
    o = ln_put(o, e, outcome == XTS_CRASH_NOT_SAVED_BUSY ? "crash inside the USB driver / storage code (unsafe to write)" :
                     outcome == XTS_CRASH_NOT_SAVED_POISONED ? "storage poisoned (no writes)" :
                     outcome == XTS_CRASH_NOT_SAVED_NOSLOT ? "crash.json not armed" : "raw write failed (storage poisoned)");
  }
  o = xts_crash_screen[3];
  e = o + sizeof xts_crash_screen[3] - 1;
  if (outcome == XTS_CRASH_NOT_SAVED_BUSY) {
    o = ln_put(o, e, "a storage operation was interrupted: check the stick on a PC (fsck.fat -n / chkdsk) before reuse");
  } else if (poisoned || outcome == XTS_CRASH_NOT_SAVED_WRITE) {
    o = ln_put(o, e, "STORAGE ERROR: stop using this stick until it has been checked (fsck.fat -n / chkdsk)");
  } else if (fsbusy) {
    o = ln_put(o, e, "crash during a file-system update: the stick may need a check (fsck.fat -n / chkdsk)");
  } else if (outcome == XTS_CRASH_SAVED) {
    o = ln_put(o, e, "stick is safe to reset");
  } else {
    o = ln_put(o, e, "stick untouched since the last block boundary; safe to reset");
  }
  o = xts_crash_screen[4];
  e = o + sizeof xts_crash_screen[4] - 1;
  if (outcome != XTS_CRASH_SAVED && outcome != XTS_CRASH_SAVED_FS_BUSY)
    o = ln_put(o, e, "the last breadcrumb in progress.bin names the case (see run.log after the next boot)");
  o = xts_crash_screen[5];
  e = o + sizeof xts_crash_screen[5] - 1;
  o = ln_put(o, e, "reset (or let the watchdog reboot) and boot the same ELF to resume the run");
  (void)wr;
  return outcome;
}
