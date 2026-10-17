/* self.exec - harness self-test, the first thing to read from a console run:
 *   nop          must not trap and must change nothing (proves the slot loads/stores the full state faithfully)
 *   tw 31,0,0    must take a program interrupt, cause 0x02 (trap), SRR0 = the slot
 *   word 0       must take a program interrupt, cause 0x08 (illegal)
 * and after each, the C runtime FPSCR/VSCR/VRSAVE must be intact (the slot restores them).
 * The three words live in section .xts_testwords (data, excluded from tools/check_illegal.py). */
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>

static const uint32_t self_words[3] __attribute__((section(".xts_testwords"), used)) = {
  0x60000000u,   /* ori 0,0,0 */
  0x7FE00008u,   /* tw 31,0,0 */
  0x00000000u,   /* no table entry decodes it */
};
static const char *const names[3] = { "nop", "trap", "illegal" };

static int self_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < 3; i++) {
    xts_case_t *c = xts_case_add(cl, names[i]);
    if (!c) return -1;
    c->arg[0] = i;
  }
  return 0;
}

static int self_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  static xts_exec_t x;
  static xts_state_diff_t diffs[16];
  int which = (int)c->arg[0];
  uint32_t word = self_words[which];
  char det[1024];
  xts_json_t j;
  xts_case_init(&x);
  int st = xts_exec_word(&x, word);
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  int status = XTS_ST_PASS;
  if (st == XTS_EXEC_DRYRUN) {
    status = XTS_ST_SKIP;
    xj_kv_str(&j, "why", "host dry run");
  } else if (st < 0) {
    status = XTS_ST_FAIL;
    xj_kv_str(&j, "exec_error", x.err);
  } else {
    xj_kv_bool(&j, "trapped", x.trap.trapped);
    xj_kv_hex(&j, "vector", x.trap.vector, 4);
    xj_kv_hex(&j, "cause", x.trap.cause, 2);
    xj_kv_hex(&j, "srr0", x.trap.srr0, 8);
    xj_kv_hex(&j, "srr1", x.trap.srr1, 16);
    xj_kv_hex(&j, "slot", xts_exec_slot_addr(), 8);
    xj_kv_uint(&j, "ticks", x.ticks);
    int n = xts_state_compare(&x.in, &x.out, diffs, 16) + xts_mem_compare(x.mem_in, x.mem_out, XTS_SCRATCH_BYTES, NULL, 0);
    xj_kv_int(&j, "changed", n);
    if (which == 0 && (x.trap.trapped || n)) status = XTS_ST_FAIL;
    if (which == 1 && !(x.trap.trapped && x.trap.vector == 0x700 && x.trap.cause == XTS_CAUSE_TRAP &&
                        x.trap.srr0 == xts_exec_slot_addr() && n == 0)) status = XTS_ST_FAIL;
    if (which == 2 && !(x.trap.trapped && x.trap.vector == 0x700 && x.trap.cause == XTS_CAUSE_ILLEGAL && n == 0))
      status = XTS_ST_FAIL;
    if (n && n <= 16) {
      xj_key(&j, "diffs");
      xj_arr(&j);
      for (int i = 0; i < n; i++) xj_str(&j, diffs[i].what);
      xj_end(&j);
    }
    int intact = xts_exec_runtime_intact();
    xj_kv_bool(&j, "runtime_fpscr_vscr_vrsave_intact", intact);
    if (!intact) status = XTS_ST_FAIL;
  }
  xj_finish(&j);
  xts_record_t r;
  xts_record_init(&r);
  r.word = word;
  r.has_word = 1;
  r.mnem = which == 0 ? "ori" : which == 1 ? "tw" : "";
  r.status = status;
  r.detail = det;
  r.detail_truncated = j.truncated;
  xts_emit(ctx, &r);
  xts_log("self.exec %s: %s %s", c->id, xts_status_name(status), det);
  return 0;
}

XTS_FAMILY(self_exec, .kind = "self", .axes = "sf", .enumerate = self_enum, .run = self_run, .block_cases = 1,
           .desc = "harness self-test: nop changes nothing, tw 31,0,0 traps (0x02), word 0 is illegal (0x08)");
