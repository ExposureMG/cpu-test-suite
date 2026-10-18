/* Breadcrumbs (RAM), the run.log tail ring and the CRASH/HANG report builders. See core.h "breadcrumbs and crash/hang
 * reports".
 *
 * xts_crumb (RAM, suite core) always says what the suite is doing: block, case (index, id, family, mnemonic), phase,
 * record seq and, in self/semantic blocks, the last executed word and its input registers (an exec pre/post hook).
 * main.c sends it as a CRUMB message before cases; when the test core is lost (xts_core_test_core_lost, main.c) the
 * report builders below quote it together with the crash slot the test core's crash handler filled (xts/xcore.h) and the
 * last XTS_LOGRING log lines. The builders are plain functions running on the suite core: nothing here touches a device.
 */
#include <string.h>
#include <xts/exec.h>
#include "core.h"

#define RING_W 160

xts_crumb_t xts_crumb;

static char ring[XTS_LOGRING][RING_W];
static uint32_t ring_n;

static const char *const phase_names[] = { "idle", "block_start", "case", "between_cases", "block_end", "finish", "run_end" };
const char *xts_phase_name(int ph) { return ph >= 0 && ph <= XTS_PH_RUN_END ? phase_names[ph] : "?"; }

static void scopy(char *d, size_t cap, const char *s)
{
  size_t i = 0;
  for (; s && s[i] && i + 1 < cap; i++) d[i] = s[i];
  d[i] = 0;
}

void xts_crash_log_note(const char *line)
{
  scopy(ring[ring_n % XTS_LOGRING], RING_W, line);
  ring_n++;
}

void xts_crash_log_tail_json(xts_json_t *j)
{
  uint32_t n = ring_n, first = n > XTS_LOGRING ? n - XTS_LOGRING : 0;
  xj_arr(j);
  for (uint32_t i = first; i < n; i++) xj_str(j, ring[i % XTS_LOGRING]);
  xj_end(j);
}

/* ---- breadcrumb (RAM) ---- */
void xts_crumb_block(const char *block, const char *family, const char *kind, int every)
{
  xts_crumb_t *c = &xts_crumb;
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

void xts_crumb_json(xts_json_t *j)
{
  const xts_crumb_t *c = &xts_crumb;
  int in_case = c->phase == XTS_PH_CASE;
  xj_obj(j);
  xj_kv_str(j, "block", c->block);
  xj_kv_int(j, "case_index", in_case ? c->case_index : -1);
  xj_kv_int(j, "case_rel", in_case ? c->case_rel : -1);
  xj_kv_str(j, "case_id", in_case ? c->case_id : "");
  xj_kv_str(j, "family", c->family);
  xj_kv_str(j, "kind", c->kind);
  xj_kv_str(j, "mnem", in_case ? c->mnem : "");
  if (in_case && c->exec_valid) xj_kv_hex(j, "word", c->word, 8);
  else xj_kv_str(j, "word", "");
  xj_kv_str(j, "phase", xts_phase_name(c->phase));
  xj_kv_int(j, "repeat", c->repeat);
  xj_kv_hex(j, "seed", c->seed, 16);
  xj_kv_uint(j, "seq", c->seq);
  xj_kv_int(j, "every", c->every);
  xj_kv_uint(j, "ncase", c->ncase);
  xj_kv_uint(j, "tb", xts_tb());
  xj_end(j);
}

void xts_crumb_input_json(xts_json_t *j)
{
  const xts_crumb_t *c = &xts_crumb;
  xj_obj(j);
  if (c->exec_valid) {                          /* the case's input state as the exec hook saw it */
    xj_kv_hex(j, "word", c->word, 8);
    xj_kv_uint(j, "kernel_words", c->kwords);
    xj_kv_uint(j, "exec_runs", c->execs);
    xj_kv_bool(j, "in_exec", c->in_exec);
    xj_key(j, "gpr");
    xj_arr(j);
    for (int r = 0; r < 32; r++) xj_hex(j, c->gpr_in[r], 16);
    xj_end(j);
    xj_kv_hex(j, "cr", c->cr_in, 8);
    xj_kv_hex(j, "xer", c->xer_in, 16);
    xj_kv_hex(j, "lr", c->lr_in, 16);
    xj_kv_hex(j, "ctr", c->ctr_in, 16);
  }
  xj_end(j);
}

static void common_tail(xts_json_t *j)
{
  xj_key(j, "crumb");
  xts_crumb_json(j);
  xj_key(j, "input");
  xts_crumb_input_json(j);
  xj_kv_str(j, "build_id", xts_build_id());
  xj_key(j, "log_tail");
  xts_crash_log_tail_json(j);
}

int xts_crash_report_json(char *buf, int cap, const xts_xc_crash_t *s)
{
  xts_json_t j;
  xj_init(&j, buf, cap);
  xj_obj(&j);
  if (s && s->valid) {
    xj_kv_uint(&j, "pir", s->pir);
    xj_kv_hex(&j, "vector", s->vector, 4);
    xj_kv_hex(&j, "srr0", s->srr0, 16);
    xj_kv_hex(&j, "srr1", s->srr1, 16);
    xj_kv_hex(&j, "dar", s->dar, 16);
    xj_kv_hex(&j, "dsisr", s->dsisr, 8);
    xj_kv_hex(&j, "lr", s->lr, 16);
    xj_kv_hex(&j, "ctr", s->ctr, 16);
    xj_kv_hex(&j, "cr", s->cr, 8);
    xj_kv_hex(&j, "xer", s->xer, 16);
    xj_key(&j, "gpr");
    xj_arr(&j);
    for (int r = 0; r < 32; r++) xj_hex(&j, s->gpr[r], 16);
    xj_end(&j);
    xj_kv_uint(&j, "tb", s->tb);
  } else {
    xj_kv_str(&j, "note", "the test core reported a crash but its crash slot is not valid");
  }
  common_tail(&j);
  xj_finish(&j);
  return j.len;
}

int xts_hang_report_json(char *buf, int cap, uint32_t timeout_us)
{
  xts_json_t j;
  xj_init(&j, buf, cap);
  xj_obj(&j);
  xj_kv_uint(&j, "timeout_us", timeout_us);
  xj_key(&j, "heartbeats");
  xj_obj(&j);
  static const int pirs[] = { XTS_PIR_TEST, XTS_PIR_SUITE, XTS_PIR_NET };
  for (int i = 0; i < 3; i++) {
    char k[4] = { (char)('0' + pirs[i]), 0 };
    xj_kv_uint(&j, k, xts_xc_heartbeat(pirs[i]));
  }
  xj_end(&j);
  xj_kv_bool(&j, "measuring", xts_xc_measuring());
  common_tail(&j);
  xj_finish(&j);
  return j.len;
}
