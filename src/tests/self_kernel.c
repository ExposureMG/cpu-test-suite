/* self.kernel - self-test of the JIT kernel path, code pointers, landing pads and the timing engine (first console run):
 *   loop       li/mtctr/add/bdnz kernel: r3 = 100, CTR = 0, no trap
 *   ktrap      tw 31,0,0 inside a kernel: trapped (0x700, cause 0x02), trap.where = kernel, offset = 8, resumes after it
 *   codeptr    bcctr to a kernel address passed in a register with xts_set_code_ptr: r3 = 1
 *   pad        single word "b +16" from the slot: lands on landing pad 1, MARK word 0 becomes non-zero
 *   addlat     timing engine: dependent add chain, cycles per add (JSON baseline 2.0: status regressed if off by >10 %)
 * On the host every case is a dry run (kernels validated, nothing executed). */
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>

static const char *const names[] = { "loop", "ktrap", "codeptr", "pad", "addlat" };
#define NCASES 5

static int sk_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < NCASES; i++) {
    xts_case_t *c = xts_case_add(cl, names[i]);
    if (!c) return -1;
    c->arg[0] = i;
  }
  return 0;
}

static xts_kbuf_t *k;

static int add_body(xts_kbuf_t *kb, int depth, void *arg)
{
  (void)arg;
  for (int i = 0; i < depth; i++) xk_emit(kb, XI_addx, 0, 3, 3, 4);
  return 0;
}

static void finish(xts_ctx_t *ctx, int status, xts_json_t *j, const char *mnem)
{
  xj_finish(j);
  xts_record_t r;
  xts_record_init(&r);
  r.status = status;
  r.detail = j->buf;
  r.detail_truncated = j->truncated;
  r.mnem = mnem;
  xts_emit(ctx, &r);
}

static int sk_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  static xts_exec_t x;
  char det[1024];
  xts_json_t j;
  int which = (int)c->arg[0], st, status = XTS_ST_PASS;
  if (!k && !(k = xk_new(4096))) return -1;
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  xts_case_init(&x);
  xk_reset(k);
  switch (which) {
  case 0:
    xk_emit(k, XI_addi, 0, 3, 0, 0);
    xk_emit(k, XI_addi, 0, 4, 0, 1);
    xk_emit(k, XI_addi, 0, 9, 0, 100);
    xk_loop_end(k, xk_loop_begin(k, 9));            /* empty loop first: framing only */
    xk_emit(k, XI_addi, 0, 9, 0, 100);
    { int top = xk_loop_begin(k, 9); xk_emit(k, XI_addx, 0, 3, 3, 4); xk_loop_end(k, top); }
    xk_exit(k);
    break;
  case 1:
    xk_nop(k);
    xk_nop(k);
    xk_emit(k, XI_tw, 0, 31, 0, 0);
    xk_emit(k, XI_addi, 0, 3, 0, 7);
    xk_exit(k);
    break;
  case 2: {
    xts_set_code_ptr(&x, 5, XTS_CODE_KERNEL, 20);       /* word 5 */
    xk_emit(k, XI_mtspr, 0, 9, 5);                       /* 0: mtctr r5 */
    xk_emit(k, XI_bcctrx, 0, 20, 0);                     /* 1: bctr */
    xk_emit(k, XI_addi, 0, 3, 0, 2);                     /* 2: not reached */
    xk_exit(k);                                          /* 3 */
    xk_nop(k);                                           /* 4 */
    xk_emit(k, XI_addi, 0, 3, 0, 1);                     /* 5: target */
    xk_exit(k);
    break;
  }
  case 3:
    break;
  case 4: {
    xts_tspec_t s = XTS_TSPEC_DEFAULT;
    /* longer kernels than the default (256 iterations, depths 4-32 gave only a few timebase ticks per sample: the
     * first hardware smoke run measured 1.95 cycles but missed the quality gate) */
    static const int deep[6] = { 32, 64, 96, 128, 192, 256 };
    for (int i = 0; i < 6; i++) s.depths[i] = deep[i];
    s.iters = 2048;
    s.body = add_body;
    s.unit = "addx";
    xts_tresult_t r;
    if (xts_time_measure(&s, ctx, &r) < 0) {
      xj_kv_str(&j, "error", r.why);
      finish(ctx, XTS_ST_FAIL, &j, "addx");
      return 0;
    }
    xts_time_emit(ctx, &s, &r, "latency", XI_addx);
    return 0;
  }
  }
  if (which == 3) {
    st = xts_exec_word(&x, xts_enc(XI_bx, 0, XTS_SLOT_PAD1));
  } else {
    if (xk_finish(k) < 0) {
      xj_kv_str(&j, "kernel_error", k->errmsg);
      finish(ctx, XTS_ST_FAIL, &j, NULL);
      return 0;
    }
    st = xts_exec_kernel(&x, k);
  }
  if (st == XTS_EXEC_DRYRUN) {
    xj_kv_str(&j, "why", "host dry run");
    finish(ctx, XTS_ST_SKIP, &j, NULL);
    return 0;
  }
  if (st < 0) {
    xj_kv_str(&j, "exec_error", x.err);
    finish(ctx, XTS_ST_FAIL, &j, NULL);
    return 0;
  }
  xj_kv_bool(&j, "trapped", x.trap.trapped);
  xj_kv_hex(&j, "r3", x.out.gpr[3], 16);
  xj_kv_hex(&j, "ctr", x.out.ctr, 16);
  xj_kv_hex(&j, "mark0", x.out.mark[0], 8);
  xj_kv_uint(&j, "ticks", x.ticks);
  switch (which) {
  case 0: if (x.trap.trapped || x.out.gpr[3] != 100 || x.out.ctr != 0) status = XTS_ST_FAIL; break;
  case 1:
    xj_kv_int(&j, "where", x.trap.where);
    xj_kv_uint(&j, "offset", x.trap.offset);
    xj_kv_hex(&j, "cause", x.trap.cause, 2);
    if (!x.trap.trapped || x.trap.vector != 0x700 || x.trap.cause != XTS_CAUSE_TRAP || x.trap.where != 1 ||
        x.trap.offset != 8 || x.out.gpr[3] != 7) status = XTS_ST_FAIL;
    break;
  case 2: if (x.trap.trapped || x.out.gpr[3] != 1) status = XTS_ST_FAIL; break;
  case 3: if (x.trap.trapped || x.out.mark[0] == 0 || x.out.mark[1] != 0) status = XTS_ST_FAIL; break;
  }
  if (!xts_exec_runtime_intact()) { status = XTS_ST_FAIL; xj_kv_bool(&j, "runtime_intact", 0); }
  finish(ctx, status, &j, NULL);
  xts_log("self.kernel %s: %s %s", c->id, xts_status_name(status), det);
  return 0;
}

XTS_FAMILY(self_kernel, .kind = "self", .axes = "sf,code_align", .enumerate = sk_enum, .run = sk_run, .block_cases = 1,
           .desc = "JIT kernel path, trap inside a kernel, code pointers, landing pads, timing engine");
