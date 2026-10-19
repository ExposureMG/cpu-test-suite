/* branch - branch costs and predictor structure, by timing-engine fits (see src/timing).
 *
 *   pred            taken / not taken / unconditional to the next word / taken over one word, per branch; taken-extra
 *   tk_bc.d<n> tk_b.d<n>   taken conditional / unconditional branch over n skipped words: the taken-branch bubble per distance
 *   mispredict      xorshift64 sign branch (50 % mispredicts) against the same code with the branch never / always taken:
 *                   penalty = 2*rand - never - always  (needs no assumption about the cost of a taken branch or of addi)
 *                   The random kernel uses a 3 % fit-residual limit (its mispredicts scatter per depth).
 *   pattern.p<n>    branch direction from a rotating 64-bit pattern of period n: extra cost over the predictable mean and an
 *                   estimate of the mispredict rate, relative to the random reference (how long a history is learned)
 *   bctr.d<n> bclr.d<n>   register branch after mtctr / mtlr with n independent addi in between (n = 0,2,4,8,16,32): the
 *                   penalty against the same unit with "b .+4" in place of the register branch
 *                   (bctr runs in the GPR-counted loop, XTS_LOOP_GPR: CTR is the branch's, not the loop's; the bht.* cases
 *                   stay in the CTR frame, whose layout their build-time check depends on)
 *   linkstack       call trees of depth 1..16 (bl, mflr/mtlr, blr): cycles per tree vs depth, knee fit -> link stack depth
 *   loop            loop overhead (fit intercept) and per-instruction cost for bodies of 1..16 independent li
 *   align.*         taken branch whose target (and branch) sit at byte phase p of a 128-byte line: fetch-group effects
 *   bht.k<n>        BHT aliasing: a taken and a not-taken branch 2^n bytes apart vs 1.5 * 2^n apart (n = 8..17; extra = aliasing)
 *
 * Ported from OpenXenos isa/hwtest branch/bhtcap plans and analysis_timing.py (mispredict formula, link-stack fit); the
 * alignment, distance and aliasing cases are new. Metric names follow the shared timing convention (rthroughput, penalty,
 * extra, cycles, ratio, depth). Cycles use xts_cycles_per_tick (calib). On the host every case is a dry run.
 *
 * Exported for the host unit tests: tsys_mispredict_penalty, tsys_pattern_rate, tsys_fit_linkstack. */
#include "tsys_priv.h"

#pragma GCC diagnostic ignored "-Wunused-function"

/* ---- pure analysis (host-tested) -------------------------------------------------------------------------------------- */
double tsys_mispredict_penalty(double rand_c, double never_c, double always_c)
{
  /* a perfect predictor would pay the mean of the never-taken and always-taken costs; a random 50 % branch mispredicts
   * half the time: rand = mean + 0.5 * penalty */
  return 2.0 * rand_c - never_c - always_c;
}

double tsys_pattern_rate(double pat_c, double pat_never, double pat_always, double rand_c, double rand_never, double rand_always)
{
  double extra_pat = pat_c - 0.5 * (pat_never + pat_always);
  double extra_rand = rand_c - 0.5 * (rand_never + rand_always);     /* = 0.5 * penalty */
  return extra_rand > 0 ? extra_pat / (2.0 * extra_rand) : NAN;
}

/* y = a + c*D + M*max(0, D-S): least squares for every S, best S, and every S within 5 % of the best residual */
static int ls_lsq(const double *d, const double *y, int n, int S, double *coef, double *sse)
{
  int m = S > 0 ? 3 : 2;
  double A[3][4] = { { 0 } };
  for (int k = 0; k < n; k++) {
    double x[3] = { 1.0, d[k], S > 0 ? (d[k] > S ? d[k] - S : 0.0) : 0.0 };
    for (int i = 0; i < m; i++) {
      for (int j = 0; j < m; j++) A[i][j] += x[i] * x[j];
      A[i][m] += x[i] * y[k];
    }
  }
  for (int i = 0; i < m; i++) {
    int piv = i;
    for (int r = i + 1; r < m; r++)
      if (fabs(A[r][i]) > fabs(A[piv][i])) piv = r;
    if (fabs(A[piv][i]) < 1e-12) return -1;
    for (int j = 0; j <= m; j++) { double t = A[i][j]; A[i][j] = A[piv][j]; A[piv][j] = t; }
    for (int r = 0; r < m; r++) {
      if (r == i) continue;
      double f = A[r][i] / A[i][i];
      for (int j = 0; j <= m; j++) A[r][j] -= f * A[i][j];
    }
  }
  for (int i = 0; i < m; i++) coef[i] = A[i][m] / A[i][i];
  if (m == 2) coef[2] = 0;
  double e = 0;
  for (int k = 0; k < n; k++) {
    double p = coef[0] + coef[1] * d[k] + (S > 0 && d[k] > S ? coef[2] * (d[k] - S) : 0.0);
    e += (p - y[k]) * (p - y[k]);
  }
  *sse = e;
  return 0;
}

int tsys_fit_linkstack(const double *d, const double *y, int n, tsys_ls_t *o)
{
  memset(o, 0, sizeof *o);
  if (n < 5) return -1;
  double coef[3], sse, dmax = 0, ymean = 0;
  for (int k = 0; k < n; k++) { if (d[k] > dmax) dmax = d[k]; ymean += y[k] / n; }
  if (ls_lsq(d, y, n, 0, coef, &sse) < 0) return -1;
  o->sse_lin = sse;
  o->per_level_lin = coef[1];
  double bestsse = INFINITY, bc[3] = { 0 };
  int best = 0;
  static double sses[64];
  for (int S = 1; S < (int)dmax && S < 64; S++) {
    sses[S] = INFINITY;
    if (ls_lsq(d, y, n, S, coef, &sse) < 0) continue;
    sses[S] = sse;
    if (sse < bestsse) { bestsse = sse; best = S; memcpy(bc, coef, sizeof bc); }
  }
  if (!best) return -1;
  double floor_ = 0.01 * ymean;
  o->lo = o->hi = best;
  for (int S = 1; S < (int)dmax && S < 64; S++) {
    if (sses[S] <= bestsse * 1.05 + floor_ * floor_ * n) {
      if (S < o->lo) o->lo = S;
      if (S > o->hi) o->hi = S;
    }
  }
  o->S = best;
  o->a = bc[0];
  o->c = bc[1];
  o->M = bc[2];
  o->sse = bestsse;
  o->jump = bc[2] > (3.0 > 0.5 * bc[1] ? 3.0 : 0.5 * bc[1]) && bestsse < 0.5 * o->sse_lin;
  o->ok = 1;
  return 0;
}

/* ---- kernel bodies ---------------------------------------------------------------------------------------------------- */
#define CR_EQ0 0x20000000u              /* CR0[EQ]: "beq cr0" is taken, "bne cr0" is not */
#define CR7_EQ 0x00000002u              /* CR7[EQ]: constant-true field untouched by the compares below */
#define BI_RAND 0                       /* CR0[LT]: sign of the last compare */
#define BI_NEVER 4                      /* CR1[LT]: never set */
#define BI_ALWAYS 30                    /* CR7[EQ]: always set */
#define R_BASE 25
#define TM(p, c, v) tm_make((p), (c), (v), 1, -1)

enum { K_BC_T, K_BC_NT, K_B, K_LCG, K_PAT, K_REG, K_CALL, K_LOOP, K_ALIGN, K_BHT };

typedef struct bp {
  int kind, a, b, c;
  uint64_t pat;
  int base_idx;                         /* written by pre(): word index whose address mflr/bl delivered */
} bp_t;

static void nops(xts_kbuf_t *k, int n)
{
  for (int i = 0; i < n; i++) xk_nop(k);
}

/* word position of the next word within its 128-byte line (the kernel starts at code_offset in the line) */
static int phase_w(const xts_kbuf_t *k) { return (int)(((xts_env.code_offset & 124u) / 4 + k->n) % 32); }
static void pad_to_phase(xts_kbuf_t *k, int pw)
{
  while (!k->err && phase_w(k) != pw) xk_nop(k);
}
static void pad_to_index(xts_kbuf_t *k, uint32_t idx)
{
  while (!k->err && k->n < idx) xk_nop(k);
}

static int bht_gap(const bp_t *p) { return p->c ? p->a + p->a / 2 : p->a; }

static int br_pre(xts_kbuf_t *k, int depth, void *arg)
{
  bp_t *p = arg;
  if (p->kind == K_REG) {
    int l = xk_label(k);
    xk_bl(k, l);
    xk_bind(k, l);
    p->base_idx = xk_emit(k, XI_mfspr, 0, R_BASE, 8);   /* mflr: LR = address of this very word */
  } else if (p->kind == K_BHT) {
    /* The not-taken branches B_q live here, BEFORE the loop (the loop's bdnz reaches only +-32 KB, the gap may be 128 KB),
     * `gap` bytes below the taken branches A_q at the loop top T. Frame after pre: mtctr, isync, mftb, then the loop top
     * (already 32-byte aligned when T is a multiple of 8 words). */
    int gw = bht_gap(p) / 4;
    if (4 * depth + 3 > gw) { snprintf(k->errmsg, sizeof k->errmsg, "BHT gap too small"); k->err = -1; return -1; }
    uint32_t s0 = k->n, T = (s0 + 1 + (uint32_t)gw + 7u) & ~7u;
    p->base_idx = (int)T;
    xk_emit(k, XI_bx, 0, 4 * (int)(T - 3 - s0));        /* step over the region */
    pad_to_index(k, T - (uint32_t)gw);
    for (int q = 0; q < depth; q++) {
      xk_emit(k, XI_bcx, 0, 4, 2, 4);                   /* B_q: never taken */
      int cur = (int)(T - (uint32_t)gw) + 4 * q + 1;
      int target = (int)T + (q < depth - 1 ? 4 * (q + 1) : 4 * depth);
      xk_emit(k, XI_bx, 0, 4 * (target - cur));         /* back into the loop: A_q+1, or the loop's bdnz */
      xk_nop(k);
      xk_nop(k);
    }
    pad_to_index(k, T - 3);
  }
  return 0;
}

static void body_lcg(xts_kbuf_t *k, int depth, int bi, int pattern)
{
  for (int j = 0; j < depth; j++) {
    if (!pattern) {
      xk_emit(k, XI_rldicrx, 0, 6, 5, 13, 50);          /* t = x << 13 */
      xk_emit(k, XI_xorx, 0, 5, 5, 6);
      xk_emit(k, XI_rldiclx, 0, 6, 5, 57, 7);          /* t = x >> 7 */
      xk_emit(k, XI_xorx, 0, 5, 5, 6);
      xk_emit(k, XI_rldicrx, 0, 6, 5, 17, 46);         /* t = x << 17 */
      xk_emit(k, XI_xorx, 0, 5, 5, 6);
      xk_emit(k, XI_cmpi, 0, 0, 1, 5, 0);              /* cmpdi cr0,x,0 */
    } else {
      xk_emit(k, XI_rldiclx, 0, 8, 8, 1, 0);           /* rotldi P,P,1 */
      xk_emit(k, XI_cmpi, 0, 0, 1, 8, 0);
    }
    xk_emit(k, XI_bcx, 0, 12, bi, 8);                  /* skip the addi when the bit is set */
    xk_emit(k, XI_addi, 0, 7, 7, 1);
  }
}

static int body_reg(xts_kbuf_t *k, int depth, bp_t *p)
{
  int spr = p->a, d = p->b, ctl = p->c;
  for (int j = 0; j < depth; j++) {
    int at = (int)k->n;
    int64_t off = 4 * ((int64_t)at + 3 + d - p->base_idx);   /* target: the word after the branch */
    xk_emit(k, XI_addi, 0, 5, R_BASE, off);
    xk_emit(k, XI_mtspr, 0, spr, 5);
    for (int i = 0; i < d; i++) xk_emit(k, XI_addi, 0, 9 + (i & 3), 9 + (i & 3), 1);
    if (ctl) xk_emit(k, XI_bx, 0, 4);
    else if (spr == 9) xk_emit(k, XI_bcctrx, 0, 20, 0);
    else xk_emit(k, XI_bclrx, 0, 20, 0);
  }
  return 0;
}

/* r3-r12 then r14-r25: never r13 (ABI) or the frame's r27-r29 */
static int save_reg(int level) { return level <= 10 ? 2 + level : 3 + level; }

static int body_call(xts_kbuf_t *k, int depth, int D)
{
  int f[20], skip = xk_label(k);
  for (int i = 1; i <= D; i++) f[i] = xk_label(k);
  for (int j = 0; j < depth; j++) xk_bl(k, f[1]);
  xk_b(k, skip);
  for (int i = 1; i <= D; i++) {
    xk_bind(k, f[i]);
    if (i < D) {
      xk_emit(k, XI_mfspr, 0, save_reg(i), 8);          /* one save register per level */
      xk_bl(k, f[i + 1]);
      xk_emit(k, XI_mtspr, 0, 8, save_reg(i));
    }
    xk_emit(k, XI_bclrx, 0, 20, 0);
  }
  xk_bind(k, skip);
  return 0;
}

static int body_align(xts_kbuf_t *k, int depth, int pt, int m, int cond)
{
  int ptw = pt / 4, pbw = ptw + m;
  if (pbw > 31) { snprintf(k->errmsg, sizeof k->errmsg, "branch phase out of the line"); k->err = -1; return -1; }
  pad_to_phase(k, 0);
  uint32_t start = k->n;
  for (int u = 0; u < depth; u++) {
    pad_to_index(k, start + 32u * u + pbw);
    int disp = 4 * (32 + ptw - pbw);
    if (cond) xk_emit(k, XI_bcx, 0, 12, 2, disp);
    else xk_emit(k, XI_bx, 0, disp);
    pad_to_index(k, start + 32u * (u + 1));
  }
  pad_to_index(k, start + 32u * depth + ptw);            /* the loop's bdnz is the landing word */
  return 0;
}

/* BHT aliasing, loop part: A_q = "bc 12,2,.+8" (taken) at T+4q, then "b B_q" back to the region built by pre(). */
static int body_bht(xts_kbuf_t *k, int depth, const bp_t *p)
{
  int gw = bht_gap(p) / 4;
  if ((int)k->n != p->base_idx) {
    snprintf(k->errmsg, sizeof k->errmsg, "BHT: loop top is word %u, expected %d (frame layout changed)", (unsigned)k->n, p->base_idx);
    k->err = -1;
    return -1;
  }
  for (int q = 0; q < depth; q++) {
    xk_emit(k, XI_bcx, 0, 12, 2, 8);
    xk_nop(k);
    xk_emit(k, XI_bx, 0, -4 * (gw + 2));                 /* from word 4q+2 to B_q at 4q - gw */
    xk_nop(k);
  }
  return 0;
}

static int br_body(xts_kbuf_t *k, int depth, void *arg)
{
  bp_t *p = arg;
  switch (p->kind) {
  case K_BC_T:
    for (int j = 0; j < depth; j++) { xk_emit(k, XI_bcx, 0, 12, 2, 4 * (p->a + 1)); nops(k, p->a); }
    break;
  case K_BC_NT:
    for (int j = 0; j < depth; j++) xk_emit(k, XI_bcx, 0, 4, 2, 4);
    break;
  case K_B:
    for (int j = 0; j < depth; j++) { xk_emit(k, XI_bx, 0, 4 * (p->a + 1)); nops(k, p->a); }
    break;
  case K_LCG: body_lcg(k, depth, p->a == 0 ? BI_RAND : p->a == 1 ? BI_NEVER : BI_ALWAYS, 0); break;
  case K_PAT: body_lcg(k, depth, p->a == 0 ? BI_RAND : p->a == 1 ? BI_NEVER : BI_ALWAYS, 1); break;
  case K_REG: return body_reg(k, depth, p);
  case K_CALL: return body_call(k, depth, p->a);
  case K_LOOP:
    for (int j = 0; j < depth; j++) xk_emit(k, XI_addi, 0, 5 + (j & 7), 0, 1);
    break;
  case K_ALIGN: return body_align(k, depth, p->a, p->b, p->c);
  case K_BHT: return body_bht(k, depth, p);
  }
  return 0;
}

static int br_setup(xts_exec_t *x, void *arg)
{
  const bp_t *p = arg;
  xts_set_cr(x, CR_EQ0 | CR7_EQ);
  xts_set_gpr(x, 5, 0x9E3779B97F4A7C15ull);              /* xorshift64 state */
  xts_set_gpr(x, 7, 0);
  if (p->kind == K_PAT) xts_set_gpr(x, 8, p->pat);
  return 0;
}

static meas_t M[16];
static bp_t P[16];

static int run_bp(meas_t *m, xts_ctx_t *ctx, bp_t *p, const char *unit, double est)
{
  static const int dl[8] = { 1, 2, 3, 4, 6, 8, 12, 16 };
  m->s = (xts_tspec_t)XTS_TSPEC_DEFAULT;
  mset_unit(m, unit);
  m->s.body = br_body;
  m->s.pre = br_pre;
  m->s.setup = br_setup;
  m->s.arg = p;
  m->s.iters = pick_iters(est);
  if (p->kind == K_REG && p->a == 9) m->s.loop = XTS_LOOP_GPR;   /* mtctr/bcctr bodies: CTR must not be the loop counter */
  if (p->kind == K_ALIGN) m->s.align = 128;                /* the pad before the first line then costs nothing */
  if (p->kind == K_LOOP) {
    m->s.ndepths = 8;
    for (int i = 0; i < 8; i++) m->s.depths[i] = dl[i];
  } else if (p->kind == K_BHT || p->kind == K_CALL) {
    m->s.ndepths = 6;
    for (int i = 0; i < 6; i++) m->s.depths[i] = dl[i];
  } else if (p->kind != K_ALIGN) {
    /* plain branch bodies (taken / not taken / b, random and pattern mispredicts, register branches): the line layout of
     * xts/timing.h (top at byte 12 of a fetch group, whole-line depths). loop measures the overhead of short bodies,
     * align the line placement, bht/call depth means history / call depth: those keep their layout. */
    xts_time_line_layout(&m->s, 1);
  }
  /* the random-outcome kernel (xorshift sign branch, ~50 % mispredicts): which branches mispredict differs from depth
   * to depth, so its per-depth cost scatters by a few percent however repeatable each run is (sched run 2026-10-19:
   * 27.3-28.7 cycles per step, residual 1.06 %). A 3 % residual limit for it alone; never/always keep the default 1 %. */
  if (p->kind == K_LCG && p->a == 0) m->s.resid_limit = 0.03;
  return mrun(m, ctx);
}

static void bp_init(bp_t *p, int kind, int a, int b, int c)
{
  memset(p, 0, sizeof *p);
  p->kind = kind;
  p->a = a;
  p->b = b;
  p->c = c;
}

typedef struct ent {
  const char *id;
  int (*fn)(xts_ctx_t *, const struct ent *);
  int a, b, c;
} ent_t;

/* ---- predictable branches --------------------------------------------------------------------------------------------- */
static int h_pred(xts_ctx_t *ctx, const ent_t *e)
{
  bp_init(&P[0], K_BC_T, 0, 0, 0);
  bp_init(&P[1], K_BC_NT, 0, 0, 0);
  bp_init(&P[2], K_B, 0, 0, 0);
  bp_init(&P[3], K_BC_T, 1, 0, 0);
  run_bp(&M[0], ctx, &P[0], "bc 12,2,.+4 (taken)", 6);
  run_bp(&M[1], ctx, &P[1], "bc 4,2,.+4 (not taken)", 5);
  run_bp(&M[2], ctx, &P[2], "b .+4", 8);
  run_bp(&M[3], ctx, &P[3], "bc 12,2,.+8 over one word", 8);
  uint32_t wt = xts_enc(XI_bcx, 0, 12, 2, 4), wn = xts_enc(XI_bcx, 0, 4, 2, 4), wb = xts_enc(XI_bx, 0, 4),
           ws = xts_enc(XI_bcx, 0, 12, 2, 8);
  tmeta_t t = TM("bcx", NULL, "taken_next");
  emit_meas(ctx, "taken_next", "rthroughput", XI_bcx, wt, &M[0], &t);
  t = TM("bcx", NULL, "not_taken");
  emit_meas(ctx, "not_taken", "rthroughput", XI_bcx, wn, &M[1], &t);
  t = TM("bx", NULL, "uncond_next");
  emit_meas(ctx, "uncond_next", "rthroughput", XI_bx, wb, &M[2], &t);
  t = TM("bcx", NULL, "taken_skip_one");
  emit_meas(ctx, "taken_skip", "rthroughput", XI_bcx, ws, &M[3], &t);
  t = TM("bcx", NULL, "taken_minus_not_taken");
  emit_diff(ctx, "taken_extra", "extra", XI_bcx, wt, &M[0], &M[1], &t);
  t = TM("bcx", "bx", "taken_conditional_minus_unconditional");
  emit_diff(ctx, "cond_vs_uncond", "extra", XI_bcx, wt, &M[0], &M[2], &t);
  t = TM("bcx", NULL, "taken_skip_one_minus_taken_next");
  emit_diff(ctx, "skip_extra", "extra", XI_bcx, ws, &M[3], &M[0], &t);
  return 0;
}

/* taken-branch cost against the number of skipped words */
static int h_tk(xts_ctx_t *ctx, const ent_t *e)
{
  int cond = e->a, d = e->b;
  bp_init(&P[0], cond ? K_BC_T : K_B, d, 0, 0);
  char u[48];
  snprintf(u, sizeof u, cond ? "bc 12,2,.+%d" : "b .+%d", 4 * (d + 1));
  run_bp(&M[0], ctx, &P[0], u, 6 + 0.1 * d);
  uint32_t w = cond ? xts_enc(XI_bcx, 0, 12, 2, 4 * (d + 1)) : xts_enc(XI_bx, 0, 4 * (d + 1));
  tmeta_t t = tm_make(cond ? "bcx" : "bx", NULL, cond ? "taken_conditional" : "unconditional", 1, d);
  t.xkey = "skipped_words";
  t.xval = d;
  emit_meas(ctx, "tput", "rthroughput", cond ? XI_bcx : XI_bx, w, &M[0], &t);
  return 0;
}

/* ---- misprediction ---------------------------------------------------------------------------------------------------- */
static int h_mispredict(xts_ctx_t *ctx, const ent_t *e)
{
  for (int v = 0; v < 3; v++) bp_init(&P[v], K_LCG, v, 0, 0);
  run_bp(&M[0], ctx, &P[0], "xorshift64 step ; cmpdi ; branch on sign (random)", 40);
  run_bp(&M[1], ctx, &P[1], "same, branch never taken", 16);
  run_bp(&M[2], ctx, &P[2], "same, branch always taken", 16);
  uint32_t w = xts_enc(XI_bcx, 0, 12, BI_RAND, 8);
  tmeta_t t = TM("bcx", NULL, "xorshift_random_50pct");
  emit_meas(ctx, "rand", "cycles", XI_bcx, w, &M[0], &t);
  t = TM("bcx", NULL, "xorshift_never_taken");
  emit_meas(ctx, "never", "cycles", XI_bcx, w, &M[1], &t);
  t = TM("bcx", NULL, "xorshift_always_taken");
  emit_meas(ctx, "always", "cycles", XI_bcx, w, &M[2], &t);
  double pen = tsys_mispredict_penalty(mcyc(&M[0]), mcyc(&M[1]), mcyc(&M[2]));
  int st = xts_status_worst(xts_status_worst(mstatus(&M[0]), mstatus(&M[1])), mstatus(&M[2]));
  t = TM("bcx", NULL, "mispredict_penalty");
  emit_val(ctx, "penalty", "penalty", "cycles", XI_bcx, w, pen, st, &M[0], &t,
           "2*rand - never - always; assumes 50% of the random branches mispredict (xorshift64 sign bit is unpredictable); "
           "a predictor that partly learns it makes this a lower bound");
  return 0;
}

static uint64_t pattern_bits(int period)
{
  /* deterministic balanced pattern of the given period, tiled over 64 bits */
  uint8_t bit[64];
  for (int i = 0; i < period; i++) bit[i] = i < period / 2;
  xts_rng_t r;
  xts_rng_seed(&r, 0x5eed0000u + (uint64_t)period);
  for (int i = period - 1; i > 0; i--) {
    int j = (int)xts_rng_below(&r, (uint32_t)i + 1);
    uint8_t t = bit[i]; bit[i] = bit[j]; bit[j] = t;
  }
  uint64_t v = 0;
  for (int i = 0; i < 64; i++) v = (v << 1) | bit[i % period];
  return v;
}

static int h_pattern(xts_ctx_t *ctx, const ent_t *e)
{
  int period = e->a;
  uint64_t pat = pattern_bits(period);
  for (int v = 0; v < 3; v++) bp_init(&P[v], K_LCG, v, 0, 0);
  for (int v = 0; v < 3; v++) { bp_init(&P[3 + v], K_PAT, v, 0, 0); P[3 + v].pat = pat; }
  run_bp(&M[0], ctx, &P[0], "xorshift64 random branch", 40);
  run_bp(&M[1], ctx, &P[1], "xorshift64 never taken", 16);
  run_bp(&M[2], ctx, &P[2], "xorshift64 always taken", 16);
  run_bp(&M[3], ctx, &P[3], "pattern branch", 30);
  run_bp(&M[4], ctx, &P[4], "pattern never taken", 6);
  run_bp(&M[5], ctx, &P[5], "pattern always taken", 6);
  uint32_t w = xts_enc(XI_bcx, 0, 12, BI_RAND, 8);
  char var[32];
  snprintf(var, sizeof var, "pattern_period_%d", period);
  tmeta_t t = TM("bcx", NULL, var);
  t.xkey = "period";
  t.xval = period;
  emit_meas(ctx, "cycles", "cycles", XI_bcx, w, &M[3], &t);
  double mean_c = 0.5 * (mcyc(&M[4]) + mcyc(&M[5]));
  int stp = xts_status_worst(xts_status_worst(mstatus(&M[3]), mstatus(&M[4])), mstatus(&M[5]));
  tmeta_t t2 = t;
  t2.variant = "pattern_minus_predictable_mean";
  emit_val(ctx, "extra", "extra", "cycles", XI_bcx, w, mcyc(&M[3]) - mean_c, stp, &M[3], &t2,
           "cycles per step over the mean of the never/always-taken controls of the same code");
  double rate = tsys_pattern_rate(mcyc(&M[3]), mcyc(&M[4]), mcyc(&M[5]), mcyc(&M[0]), mcyc(&M[1]), mcyc(&M[2]));
  int st = xts_status_worst(stp, xts_status_worst(xts_status_worst(mstatus(&M[0]), mstatus(&M[1])), mstatus(&M[2])));
  t2.variant = "mispredict_rate_estimate";
  emit_val(ctx, "rate", "ratio", "ratio", XI_bcx, w, rate, st, &M[3], &t2,
           "extra cost of the pattern relative to the random reference (taken as 50% mispredicts): fraction of pattern "
           "branches mispredicted; 0 = the period is learned, ~0.5 = no better than random");
  return 0;
}

/* ---- register branches ------------------------------------------------------------------------------------------------ */
static int h_regbr(xts_ctx_t *ctx, const ent_t *e)
{
  int spr = e->a, d = e->b;
  const char *nm = spr == 9 ? "bcctr" : "bclr", *mt = spr == 9 ? "mtctr" : "mtlr";
  bp_init(&P[0], K_REG, spr, d, 0);
  bp_init(&P[1], K_REG, spr, d, 1);
  char u0[56], u1[56];
  snprintf(u0, sizeof u0, "addi ; %s ; %d addi ; %s", mt, d, nm);
  snprintf(u1, sizeof u1, "addi ; %s ; %d addi ; b .+4", mt, d);
  run_bp(&M[0], ctx, &P[0], u0, 30 + d);
  run_bp(&M[1], ctx, &P[1], u1, 8 + d);
  int insn = spr == 9 ? XI_bcctrx : XI_bclrx;
  uint32_t w = xts_enc(insn, 0, 20, 0);
  tmeta_t t = tm_make(mt, nm, "register_branch_minus_b_next", 1, d);
  t.xkey = "filler_insns";
  t.xval = d;
  emit_diff(ctx, "penalty", "penalty", insn, w, &M[0], &M[1], &t);
  t = tm_make(mt, nm, "register_branch", 1, d);
  emit_meas(ctx, "cycles", "cycles", insn, w, &M[0], &t);
  return 0;
}

/* ---- link stack ------------------------------------------------------------------------------------------------------- */
static int h_linkstack(xts_ctx_t *ctx, const ent_t *e)
{
  static const int Ds[] = { 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16 };
  enum { ND = sizeof Ds / sizeof Ds[0] };
  static meas_t LM[ND];
  double d[ND], y[ND];
  int n = 0, st = XTS_ST_PASS;
  uint32_t w = xts_enc(XI_bclrx, 0, 20, 0);
  for (int i = 0; i < ND; i++) {
    bp_init(&P[0], K_CALL, Ds[i], 0, 0);
    char u[48];
    snprintf(u, sizeof u, "call tree depth %d (bl/blr x%d)", Ds[i], Ds[i]);
    run_bp(&LM[i], ctx, &P[0], u, 12.0 * Ds[i]);
    char s[24];
    snprintf(s, sizeof s, "tree_d%d", Ds[i]);
    tmeta_t t = tm_make("bl", "bclr", "call_tree_cycles_per_tree", 1, Ds[i]);
    t.xkey = "tree_depth";
    t.xval = Ds[i];
    emit_meas(ctx, s, "cycles", XI_bclrx, w, &LM[i], &t);
    st = xts_status_worst(st, mstatus(&LM[i]));
    if (LM[i].r.ok && !LM[i].r.failed) { d[n] = Ds[i]; y[n++] = mcyc(&LM[i]); }
  }
  tmeta_t t = TM("bl", "bclr", "link_stack_depth_fit");
  if (n < 5) {
    emit_val(ctx, "depth", "depth", "entries", XI_bclrx, w, NAN, st == XTS_ST_PASS ? XTS_ST_SKIP : st, NULL, &t,
             "too few call-tree measurements");
    return 0;
  }
  tsys_ls_t f;
  if (tsys_fit_linkstack(d, y, n, &f) < 0) {
    emit_val(ctx, "depth", "depth", "entries", XI_bclrx, w, NAN, XTS_ST_NOISY, NULL, &t, "fit failed");
    return 0;
  }
  char buf[1100];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  j_tdet(&j, &(tdet_t){ &t, NULL });
  xj_kv_bool(&j, "jump", f.jump);
  xj_kv_double(&j, "per_level_cycles", f.jump ? f.c : f.per_level_lin);
  xj_kv_double(&j, "overflow_cycles_per_level", f.jump ? f.M : 0);
  xj_kv_double(&j, "range_lo", f.jump ? f.lo : NAN);
  xj_kv_double(&j, "range_hi", f.jump ? f.hi : NAN);
  xj_kv_double(&j, "sse", f.sse);
  xj_kv_double(&j, "sse_linear", f.sse_lin);
  xj_kv_double(&j, "max_depth_measured", d[n - 1]);
  xj_kv_str(&j, "model", "cycles/tree = a + c*D + M*max(0, D-S); S = number of link-stack entries");
  xj_finish(&j);
  xts_time_emit_value(ctx, NULL, NULL, XI_bclrx, f.jump ? f.S : NAN,
                      &(xts_temit_t){ .metric = "depth", .unit = "entries", .word = w, .case_suffix = "depth",
                                      .extra_json = j.buf,
                                      .status = f.jump ? (f.hi - f.lo <= 1 ? st : xts_status_worst(st, XTS_ST_NOISY))
                                                       : xts_status_worst(st, XTS_ST_INFO) });
  return 0;
}

/* ---- loop overhead ---------------------------------------------------------------------------------------------------- */
static int h_loop(xts_ctx_t *ctx, const ent_t *e)
{
  bp_init(&P[0], K_LOOP, 0, 0, 0);
  run_bp(&M[0], ctx, &P[0], "li rD,1 (independent)", 1);
  uint32_t w = xts_enc(XI_bcx, 0, 16, 0, -4);
  tmeta_t t = TM("bcx", NULL, "bdnz_loop_overhead");
  emit_val(ctx, "overhead", "cycles", "cycles", XI_bcx, w, M[0].r.ok && !M[0].r.failed ? M[0].r.overhead_cycles : NAN,
           XTS_ST_PASS, &M[0], &t,
           "fit intercept: cycles per loop iteration independent of the body size (taken bdnz and loop refill)");
  t = TM("addi", NULL, "li_independent");
  emit_meas(ctx, "body_tput", "rthroughput", XI_addi, xts_enc(XI_addi, 0, 5, 0, 1), &M[0], &t);
  return 0;
}

/* ---- taken-branch target alignment ------------------------------------------------------------------------------------ */
static int h_align(xts_ctx_t *ctx, const ent_t *e)
{
  int pt = e->a, m = e->b, cond = e->c;
  bp_init(&P[0], K_ALIGN, pt, m, cond);
  char u[56];
  snprintf(u, sizeof u, "%s over 128 B, target phase %d, %d nops before it", cond ? "bc" : "b", pt, m);
  run_bp(&M[0], ctx, &P[0], u, 8 + m);
  uint32_t w = cond ? xts_enc(XI_bcx, 0, 12, 2, 128 - 4 * m) : xts_enc(XI_bx, 0, 128 - 4 * m);
  char var[40];
  snprintf(var, sizeof var, "%s_target_phase_%d_pre_nops_%d", cond ? "bc" : "b", pt, m);
  tmeta_t t = tm_make(cond ? "bcx" : "bx", NULL, var, 1, pt);
  t.xkey = "pre_nops";
  t.xval = m;
  emit_meas(ctx, "tput", "rthroughput", cond ? XI_bcx : XI_bx, w, &M[0], &t);
  return 0;
}

/* ---- BHT aliasing ----------------------------------------------------------------------------------------------------- */
static int h_bht(xts_ctx_t *ctx, const ent_t *e)
{
  int k = e->a, gap = 1 << k;
  bp_init(&P[0], K_BHT, gap, 0, 0);
  bp_init(&P[1], K_BHT, gap, 0, 1);
  char u0[56], u1[56];
  snprintf(u0, sizeof u0, "taken/not-taken pair, %d bytes apart", gap);
  snprintf(u1, sizeof u1, "same pair, %d bytes apart", gap + gap / 2);
  run_bp(&M[0], ctx, &P[0], u0, 34);
  run_bp(&M[1], ctx, &P[1], u1, 34);
  uint32_t w = xts_enc(XI_bcx, 0, 12, 2, 8);
  tmeta_t t = tm_make("bcx", "bcx", "aliasing_distance_minus_control", 1, gap);
  t.xkey = "log2_distance";
  t.xval = k;
  emit_diff(ctx, "extra", "extra", XI_bcx, w, &M[0], &M[1], &t);
  t = tm_make("bcx", "bcx", "pair_at_distance", 1, gap);
  emit_meas(ctx, "pair", "cycles", XI_bcx, w, &M[0], &t);
  return 0;
}

/* ---- case table (built once) ------------------------------------------------------------------------------------------ */
#define MAXENT 256
static ent_t ents[MAXENT];
static char ids[MAXENT][32];
static int nents;

static void add_ent(const char *fmt, int (*fn)(xts_ctx_t *, const ent_t *), int a, int b, int c, int x)
{
  if (nents >= MAXENT) return;
  snprintf(ids[nents], sizeof ids[nents], fmt, x);
  ents[nents] = (ent_t){ ids[nents], fn, a, b, c };
  nents++;
}

static void build_table(void)
{
  static const int dist[] = { 0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63 };
  static const int regd[] = { 0, 2, 4, 8, 16, 32 };
  static const int periods[] = { 2, 4, 8, 16, 32, 64 };
  if (nents) return;
  add_ent("pred", h_pred, 0, 0, 0, 0);
  for (unsigned i = 0; i < sizeof dist / sizeof dist[0]; i++) add_ent("tk_bc.d%d", h_tk, 1, dist[i], 0, dist[i]);
  for (unsigned i = 0; i < sizeof dist / sizeof dist[0]; i++) add_ent("tk_b.d%d", h_tk, 0, dist[i], 0, dist[i]);
  add_ent("mispredict", h_mispredict, 0, 0, 0, 0);
  for (unsigned i = 0; i < sizeof periods / sizeof periods[0]; i++) add_ent("pattern.p%d", h_pattern, periods[i], 0, 0, periods[i]);
  for (unsigned i = 0; i < sizeof regd / sizeof regd[0]; i++) add_ent("bctr.d%d", h_regbr, 9, regd[i], 0, regd[i]);
  for (unsigned i = 0; i < sizeof regd / sizeof regd[0]; i++) add_ent("blr.d%d", h_regbr, 8, regd[i], 0, regd[i]);
  add_ent("linkstack", h_linkstack, 0, 0, 0, 0);
  add_ent("loop", h_loop, 0, 0, 0, 0);
  for (int pt = 0; pt < 128; pt += 4) add_ent("align.b.m0.p%d", h_align, pt, 0, 0, pt);
  for (int pt = 0; pt < 128; pt += 8) add_ent("align.bc.m0.p%d", h_align, pt, 0, 1, pt);
  for (int pt = 0; pt <= 112; pt += 8) add_ent("align.b.m3.p%d", h_align, pt, 3, 0, pt);
  for (int k = 8; k <= 17; k++) add_ent("bht.k%d", h_bht, k, 0, 0, k);
}

static int br_enum(xts_caselist_t *cl)
{
  build_table();
  for (int i = 0; i < nents; i++) {
    xts_case_t *c = xts_case_add(cl, ents[i].id);
    if (!c) return -1;
    c->arg[0] = i;
  }
  return 0;
}

static int br_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  build_table();
  int i = (int)c->arg[0];
  if (i < 0 || i >= nents) return -1;
  return ents[i].fn(ctx, &ents[i]);
}

XTS_FAMILY(branch, .kind = "sched", .axes = "code_align", .enumerate = br_enum, .run = br_run, .block_cases = 2,
           .desc = "taken/not-taken/mispredict, register-branch penalty, link stack, loop overhead, target alignment, BHT aliasing");
