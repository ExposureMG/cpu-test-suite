/* frontend.branch - the cost of a loop with one internal forward branch, by the fetch-group phase of the loop top, of
 * the internal branch and of its target.
 *
 * frontend.loop maps straight-line loops (only the closing bdnz is taken): the loop top's phase in its 16-byte fetch
 * group dominates (top at byte 4/12: 1-3 cycles per iteration of overhead, 0/8: 6-9), the closing branch's phase adds
 * 1-3. Compiled hot loops look different: an internal conditional branch per iteration (bsearch's compare, quicksort's
 * partition test, a skipped update), and the A/B console benchmark (cpu-benchmark comparison.md) showed 3-5 % swings in
 * exactly those kernels that the loop-top rule did not explain. This family measures that shape directly:
 *
 *   top:  A words (li fillers; for the alternating pattern the last two are xori r11 / cmpwi cr6,r11,0)
 *         bc    -> forward over S filler words      (the internal branch; its phase = top + 4A)
 *         S skipped fillers
 *   tgt:  6 fillers                                  (target phase = branch + 4 + 4S)
 *         bdnz top
 *
 * Cases <pattern>.t<top>.b<branch>.g<target>: top phase 0/4/8/12 (byte in the fetch group, the loop top at that offset
 * of a 128-byte line), A = 4..7 (every branch phase), S = 1..4 (every target phase), pattern taken (cr7 EQ, BO=12),
 * nottaken (BO=4) or alternate (taken every other iteration: a 2-entry pattern the predictor may or may not learn).
 * Value = cycles per iteration (minimum of 3 runs after a warm-up, as frontend.loop); detail: the phases (also of the
 * bdnz), words executed per iteration and the overhead beyond 1.02 cycles per executed word. 192 cases, well under a
 * second of measuring. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include <xts/json.h>

#define R_ITERS_REG 29
#define R_T0_REG 27
#define R_T1_REG 28
#define FB_REPEATS 3
#define FB_AFTER 6                                  /* fillers after the target */

enum { FB_TAKEN = 0, FB_NOTTAKEN, FB_ALTERNATE };
static const char *const pat_name[3] = { "taken", "nottaken", "alternate" };

static int fb_enum(xts_caselist_t *cl)
{
  for (int p = 0; p < 3; p++)
    for (int t = 0; t < 16; t += 4)
      for (int a = 4; a < 8; a++)
        for (int s = 1; s <= 4; s++) {
          int b = (t + 4 * a) & 15, g = (b + 4 + 4 * s) & 15;
          xts_case_t *c = xts_case_addf(cl, "%s.t%d.b%d.g%d", pat_name[p], t, b, g);
          if (!c) return -1;
          c->arg[0] = p;
          c->arg[1] = t;
          c->arg[2] = a;
          c->arg[3] = s;
        }
  return 0;
}

static void filler(xts_kbuf_t *k, int *n)
{
  xk_emit(k, XI_addi, 0, 3 + (*n & 7), 0, *n & 0x7FFF);
  (*n)++;
}

static int fb_build(xts_kbuf_t *k, int pat, int top, int a, int s)
{
  int n = 0;
  xk_reset(k);
  xk_prio_prologue(k);
  xk_emit(k, XI_mtspr, 0, 9, R_ITERS_REG);         /* mtctr r29 */
  xk_emit(k, XI_isync, 0);
  xk_emit(k, XI_mftb, 0, R_T0_REG, 268);
  xk_align(k, 128);
  for (int i = 0; i < top / 4; i++) xk_emit(k, XI_ori, 0, 0, 0, 0);   /* nop: loop top at `top` in the line */
  int loop = xk_here(k);
  for (int i = 0; i < (pat == FB_ALTERNATE ? a - 2 : a); i++) filler(k, &n);
  if (pat == FB_ALTERNATE) {
    xk_emit(k, XI_xori, 0, 11, 11, 1);             /* r11 ^= 1 */
    xk_emit(k, XI_cmpi, 0, 6, 0, 11, 0);           /* cmpwi cr6,r11,0: EQ every other iteration */
  }
  int tgt = xk_label(k);
  if (pat == FB_ALTERNATE) xk_bc(k, 12, 26, tgt);  /* bc if cr6 EQ */
  else xk_bc(k, pat == FB_TAKEN ? 12 : 4, 30, tgt);   /* cr7 EQ is set: BO=12 always taken, BO=4 never */
  for (int i = 0; i < s; i++) filler(k, &n);
  xk_bind(k, tgt);
  for (int i = 0; i < FB_AFTER; i++) filler(k, &n);
  xk_loop_close(k, loop, XTS_LOOP_CTR, R_ITERS_REG, 7);
  xk_emit(k, XI_mftb, 0, R_T1_REG, 268);
  xk_exit(k);
  return xk_finish(k);
}

static int fb_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  int pat = (int)c->arg[0], top = (int)c->arg[1], a = (int)c->arg[2], s = (int)c->arg[3];
  static xts_kbuf_t *k;
  static xts_exec_t x;
  if (!k && !(k = xk_new(512))) return -1;
  int b = (top + 4 * a) & 15, g = (b + 4 + 4 * s) & 15, bdnz = (g + 4 * FB_AFTER) & 15;
  /* words executed per iteration: A, bc, S when not taken, the fillers after the target, bdnz */
  double words = a + 1 + FB_AFTER + 1 + (pat == FB_NOTTAKEN ? s : pat == FB_ALTERNATE ? s / 2.0 : 0);
  int host = ctx->host || xts_exec_is_host();
  int ok = fb_build(k, pat, top, a, s) == 0;
  double best = INFINITY;
  uint32_t iters = 2000;
  for (int rep = -1; ok && !host && rep < FB_REPEATS; rep++) {   /* rep -1: warm-up (trains the predictor) */
    xts_case_init(&x);
    x.in.gpr[R_ITERS_REG] = iters;
    x.in.gpr[11] = 0;
    x.in.cr = 0x00000002;                          /* cr7 = EQ */
    if (xts_exec_kernel(&x, k) != XTS_EXEC_OK || x.trap.trapped) { ok = 0; break; }
    double t = (double)(uint32_t)(x.out.gpr[R_T1_REG] - x.out.gpr[R_T0_REG]) * xts_cycles_per_tick / iters;
    if (rep >= 0 && t < best) best = t;
  }
  xts_watchdog_kick();
  char det[700];
  xts_json_t j;
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  xj_kv_str(&j, "pattern", pat_name[pat]);
  xj_kv_int(&j, "top_phase", top);
  xj_kv_int(&j, "branch_phase", b);
  xj_kv_int(&j, "target_phase", g);
  xj_kv_int(&j, "bdnz_phase", bdnz);
  xj_kv_int(&j, "words_before_branch", a);
  xj_kv_int(&j, "skipped_words", s);
  xj_kv_double(&j, "words_executed", words);
  xj_kv_double(&j, "overhead", ok && !host ? best - 1.02 * words : NAN);
  xj_kv_double(&j, "cycles_per_tick", xts_cycles_per_tick);
  if (!ok) xj_kv_str(&j, "error", k->err ? k->errmsg : "kernel did not run");
  xj_end(&j);
  xts_record_t r;
  xts_record_init(&r);
  r.metric = "cycles";
  r.unit = "cycles";
  r.value = ok && !host ? best : NAN;
  r.status = host ? XTS_ST_SKIP : ok ? XTS_ST_INFO : XTS_ST_FAIL;
  r.detail = xj_finish(&j);
  r.detail_truncated = j.truncated;
  xts_emit(ctx, &r);
  return 0;
}

XTS_FAMILY(frontend_branch, .kind = "calib", .enumerate = fb_enum, .run = fb_run, .block_cases = 48,
           .desc = "loop with one internal forward branch: cost by fetch-group phase of the top, the branch and its target");
