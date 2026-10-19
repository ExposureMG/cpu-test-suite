/* sysops - system and special-register operations, and the record-form / carry costs a scheduler needs next to them.
 *
 * Every case is a set of timing-engine measurements (src/timing: framed CTR loop, depth fit, shuffled repeats, A/B/A,
 * quality gate); derived numbers (dependency penalties, extra cost over a baseline) are differences of two fits and are
 * emitted as their own records. Ported from OpenXenos isa/hwtest sysops/syncprobe plans; new here: lwsync/ptesync next
 * to sync, record-form (Rc=1) extra cost per ALU op, an andi./andis. investigation (the 14-cycle anomaly: issue, chain,
 * result zero or not, overlap with following independent adds, dependent branch), carry (XER[CA]) chains, larx/stcx
 * variants.
 *
 *   cmp*.L<n>        compare issue rate; compare -> dependent branch penalty (against a branch on an unrelated field)
 *   cr<op>           CR logical: chain latency, independent throughput, same-field throughput
 *   mcrf mfcr mtcrf mtocrf mfocrf   CR <-> GPR moves, CR field moves, round trips
 *   spr.{lr,ctr,xer} mfspr / mtspr throughput and mt->mf round trip (spr.ctr runs in the GPR-counted loop: CTR is free)
 *   mftb             mftb 268 / 269 throughput
 *   fpscr.*          mffs, mtfsf, mtfsfi, mtfsb0/1, mcrfs; record forms
 *   vscr             mfvscr, mtvscr, round trip
 *   barrier.<b>      sync lwsync ptesync eieio isync: alone, after a store, after a load, and the extra over alone
 *   reserve.{w,d}    larx alone, stcx alone (no reservation), larx+stcx pair, atomic read-modify-write
 *   rc.<op>          Rc=1 vs Rc=0: latency and throughput, extra cost
 *   rc.andi          andi. / andis. investigation
 *   ca.chain         adde / addze / subfe / addc carry chains, carry-only dependency
 *
 * Metric names (shared timing convention): latency, rthroughput, issue_cost, penalty, extra, overlap, cycles. Detail
 * carries producer / consumer / variant. Units are cycles unless a record says otherwise (conversion uses
 * xts_cycles_per_tick, set by the calib family). On the host everything is a dry run: kernels are built and decoded back. */
#include "tsys_priv.h"

#pragma GCC diagnostic ignored "-Wunused-function"

/* ---- generic unit generator -----------------------------------------------------------------------------------------
 * A unit is a short list of instruction slots. Operand i of a slot is op[i] + (j % rot[i]) * step[i] for unit number j
 * (rot[i] == 0: constant), so rotating destinations are one-liners. Registers: r3/r4 sources, r5-r8 rotating
 * destinations, r9 SPR/immediate source, r20 scratch pointer, r21 store value, r22-r25 load destinations. */
#define MAXSL 6

typedef struct islot {
  int insn;
  unsigned flags;
  int nops;
  int64_t op[6];
  int rot[6], step[6];
} islot_t;

typedef struct gen {
  int nsl;
  islot_t sl[MAXSL];
  int gpr_loop;                                   /* the units use CTR: run in the GPR-counted loop (XTS_LOOP_GPR) */
  int ngpr; struct { int r; uint64_t v; } gpr[8];
  int nptr; struct { int r; int off; } ptr[4];
  int nfpr; struct { int r; uint64_t v; } fpr[4];
  int nvz; int vz[8];
  int has_cr; uint32_t cr;
} gen_t;

static void g_init(gen_t *g) { memset(g, 0, sizeof *g); }

static islot_t *g_add(gen_t *g, int insn, unsigned flags, const int64_t *ops, int nops)
{
  if (g->nsl >= MAXSL) return &g->sl[MAXSL - 1];
  islot_t *s = &g->sl[g->nsl++];
  memset(s, 0, sizeof *s);
  s->insn = insn;
  s->flags = flags;
  s->nops = nops;
  for (int i = 0; i < nops && i < 6; i++) s->op[i] = ops[i];
  return s;
}
#define GA(g, insn, flags, ...) \
  g_add((g), (insn), (flags), (const int64_t[]){ __VA_ARGS__ }, (int)(sizeof((const int64_t[]){ __VA_ARGS__ }) / sizeof(int64_t)))
static void g_rot(gen_t *g, int opi, int mod, int step)
{
  islot_t *s = &g->sl[g->nsl - 1];
  s->rot[opi] = mod;
  s->step[opi] = step;
}
static void g_gpr(gen_t *g, int r, uint64_t v) { if (g->ngpr < 8) { g->gpr[g->ngpr].r = r; g->gpr[g->ngpr++].v = v; } }
static void g_ptr(gen_t *g, int r, int off) { if (g->nptr < 4) { g->ptr[g->nptr].r = r; g->ptr[g->nptr++].off = off; } }
static void g_fpr(gen_t *g, int r, uint64_t v) { if (g->nfpr < 4) { g->fpr[g->nfpr].r = r; g->fpr[g->nfpr++].v = v; } }
static void g_vz(gen_t *g, int r) { if (g->nvz < 8) g->vz[g->nvz++] = r; }
static void g_cr(gen_t *g, uint32_t cr) { g->has_cr = 1; g->cr = cr; }

static void emit_unit(xts_kbuf_t *k, const gen_t *g, int j)
{
  for (int s = 0; s < g->nsl; s++) {
    const islot_t *sl = &g->sl[s];
    int64_t o[6];
    for (int i = 0; i < sl->nops && i < 6; i++) o[i] = sl->op[i] + (sl->rot[i] ? (int64_t)(j % sl->rot[i]) * sl->step[i] : 0);
    xk_emit_a(k, sl->insn, sl->flags, o, sl->nops);
  }
}

static int gen_body(xts_kbuf_t *k, int depth, void *arg)
{
  const gen_t *g = arg;
  for (int j = 0; j < depth; j++) emit_unit(k, g, j);
  return 0;
}

static int gen_setup(xts_exec_t *x, void *arg)
{
  const gen_t *g = arg;
  for (int i = 0; i < g->ngpr; i++) xts_set_gpr(x, g->gpr[i].r, g->gpr[i].v);
  for (int i = 0; i < g->nptr; i++) xts_set_ptr(x, g->ptr[i].r, g->ptr[i].off);
  for (int i = 0; i < g->nfpr; i++) xts_set_fpr(x, g->fpr[i].r, g->fpr[i].v);
  for (int i = 0; i < g->nvz; i++) xts_set_vr(x, g->vz[i], xts_vr4(0, 0, 0, 0));
  if (g->has_cr) xts_set_cr(x, g->cr);
  return 0;
}

/* word of slot `s` for unit 0 (what the record names); 0 if it does not encode */
static uint32_t gword(const gen_t *g, int s)
{
  const islot_t *sl = &g->sl[s];
  uint32_t w = 0;
  int bad;
  if (xts_encode(sl->insn, sl->flags, sl->op, &w, &bad)) return 0;
  return w;
}

static int run_gen(meas_t *m, xts_ctx_t *ctx, gen_t *g, const char *unit, double est)
{
  m->s = (xts_tspec_t)XTS_TSPEC_DEFAULT;
  mset_unit(m, unit);
  m->s.body = gen_body;
  m->s.setup = gen_setup;
  m->s.arg = g;
  if (g->gpr_loop) m->s.loop = XTS_LOOP_GPR;
  m->s.iters = pick_iters(est);
  xts_time_line_layout(&m->s, 1);                     /* top at byte 12 of a fetch group, whole-line depths */
  return mrun(m, ctx);
}

typedef struct ent {
  const char *id;
  int (*fn)(xts_ctx_t *, const struct ent *);
  int a, b;
} ent_t;

#define CR_EQ0 0x20000000u              /* CR0[EQ]: "beq cr0" is taken, "bne cr0" is not */
#define CR7_EQ 0x00000002u              /* CR7[EQ] */
#define TM(p, c, v) tm_make((p), (c), (v), 1, -1)

static meas_t M[20];
static gen_t G[20];

/* ---- compares: issue rate and compare -> branch dependency ------------------------------------------------------------ */
static int h_compare(xts_ctx_t *ctx, const ent_t *e)
{
  int insn = e->a, L = e->b, imm = insn == XI_cmpi || insn == XI_cmpli;
  const char *mn = xts_isa[insn].mnem;
  for (int v = 0; v < 3; v++) {
    gen_t *g = &G[v];
    g_init(g);
    g_gpr(g, 3, 0);
    g_gpr(g, 4, 0);
    g_cr(g, CR_EQ0);
    int crf = v == 1 ? 0 : 1;
    if (imm) GA(g, insn, 0, crf, L, 3, 0);
    else GA(g, insn, 0, crf, L, 3, 4);
    if (v == 0) g_rot(g, 0, 6, 1);                     /* CR fields 1-6 */
    else GA(g, XI_bcx, 0, 12, 2, 4);                   /* beq cr0,.+4 (taken) */
  }
  run_gen(&M[0], ctx, &G[0], "cmp to rotating CR fields 1-6", 1);
  run_gen(&M[1], ctx, &G[1], "cmp cr0 ; beq cr0,.+4", 6);
  run_gen(&M[2], ctx, &G[2], "cmp cr1 ; beq cr0,.+4", 6);
  uint32_t w = gword(&G[0], 0);
  tmeta_t t = TM(mn, NULL, "rotating_cr_fields");
  emit_meas(ctx, "tput", "rthroughput", insn, w, &M[0], &t);
  t = TM(mn, "bcx", "branch_on_same_field");
  emit_meas(ctx, "bc_dep", "cycles", insn, w, &M[1], &t);
  t = TM(mn, "bcx", "branch_on_other_field");
  emit_meas(ctx, "bc_ind", "cycles", insn, w, &M[2], &t);
  t = TM(mn, "bcx", "dependent_minus_independent");
  emit_diff(ctx, "penalty", "penalty", insn, w, &M[1], &M[2], &t);
  return 0;
}

/* ---- CR logical ------------------------------------------------------------------------------------------------------- */
static int h_crlogical(xts_ctx_t *ctx, const ent_t *e)
{
  int insn = e->a;
  const char *mn = xts_isa[insn].mnem;
  for (int v = 0; v < 3; v++) {
    gen_t *g = &G[v];
    g_init(g);
    if (v == 0) GA(g, insn, 0, 8, 8, 31);               /* bit 8 = op(bit 8, bit 31): dependent chain */
    else GA(g, insn, 0, 8, 28, 31);                     /* sources from CR7, never written */
    if (v == 1) g_rot(g, 0, 4, 4);                      /* bits 8,12,16,20: four CR fields */
    if (v == 2) g_rot(g, 0, 4, 1);                      /* bits 8-11: one CR field */
  }
  run_gen(&M[0], ctx, &G[0], "cr op b8,b8,b31 (chain)", 1);
  run_gen(&M[1], ctx, &G[1], "cr op to bits 8,12,16,20", 1);
  run_gen(&M[2], ctx, &G[2], "cr op to bits 8-11 (one field)", 1);
  uint32_t w = gword(&G[0], 0);
  tmeta_t t = TM(mn, mn, "chain");
  emit_meas(ctx, "chain", "latency", insn, w, &M[0], &t);
  t = TM(mn, mn, "four_fields");
  emit_meas(ctx, "tput", "rthroughput", insn, w, &M[1], &t);
  t = TM(mn, mn, "same_field");
  emit_meas(ctx, "samefield", "rthroughput", insn, w, &M[2], &t);
  t = TM(mn, mn, "same_field_minus_four_fields");
  emit_diff(ctx, "samefield_extra", "extra", insn, w, &M[2], &M[1], &t);
  return 0;
}

static int h_mcrf(xts_ctx_t *ctx, const ent_t *e)
{
  g_init(&G[0]);
  GA(&G[0], XI_mcrf, 0, 3, 2);
  GA(&G[0], XI_mcrf, 0, 2, 3);
  g_init(&G[1]);
  GA(&G[1], XI_mcrf, 0, 2, 7);
  g_rot(&G[1], 0, 4, 1);
  run_gen(&M[0], ctx, &G[0], "mcrf 3,2 ; mcrf 2,3", 2);
  run_gen(&M[1], ctx, &G[1], "mcrf crF,cr7 (F = 2-5)", 1);
  uint32_t w = gword(&G[0], 0);
  tmeta_t t = tm_make("mcrf", "mcrf", "chain", 2, -1);
  emit_meas(ctx, "chain", "latency", XI_mcrf, w, &M[0], &t);
  t = TM("mcrf", "mcrf", "rotating_fields");
  emit_meas(ctx, "tput", "rthroughput", XI_mcrf, w, &M[1], &t);
  return 0;
}

/* ---- CR <-> GPR moves ------------------------------------------------------------------------------------------------- */
static int h_mfcr(xts_ctx_t *ctx, const ent_t *e)
{
  g_init(&G[0]);
  GA(&G[0], XI_mfcr, 0, 5);
  g_rot(&G[0], 0, 4, 1);
  run_gen(&M[0], ctx, &G[0], "mfcr rD", 8);
  tmeta_t t = TM("mfcr", NULL, "rotating_dest");
  emit_meas(ctx, "tput", "rthroughput", XI_mfcr, gword(&G[0], 0), &M[0], &t);
  return 0;
}

static int h_mtcrf(xts_ctx_t *ctx, const ent_t *e)
{
  static const int fxm[3] = { 0x01, 0x80, 0xFF };
  for (int v = 0; v < 3; v++) {
    g_init(&G[v]);
    g_gpr(&G[v], 9, CR_EQ0);
    GA(&G[v], XI_mtcrf, 0, fxm[v], 9);
  }
  g_init(&G[3]);
  g_gpr(&G[3], 5, CR_EQ0);
  GA(&G[3], XI_mtcrf, 0, 0xFF, 5);
  GA(&G[3], XI_mfcr, 0, 5);
  static const char *const un[3] = { "mtcrf 0x01,rS", "mtcrf 0x80,rS", "mtcrf 0xFF,rS" };
  static const char *const va[3] = { "fxm_01", "fxm_80", "fxm_FF" };
  static const char *const su[3] = { "tput_01", "tput_80", "tput_FF" };
  for (int v = 0; v < 3; v++) {
    run_gen(&M[v], ctx, &G[v], un[v], 20);
    tmeta_t t = TM("mtcrf", NULL, va[v]);
    emit_meas(ctx, su[v], "rthroughput", XI_mtcrf, gword(&G[v], 0), &M[v], &t);
  }
  run_gen(&M[3], ctx, &G[3], "mtcrf 0xFF,r ; mfcr r", 46);
  tmeta_t t = TM("mtcrf", "mfcr", "roundtrip");
  emit_meas(ctx, "roundtrip", "latency", XI_mtcrf, gword(&G[3], 0), &M[3], &t);
  return 0;
}

static int h_mtocrf(xts_ctx_t *ctx, const ent_t *e)
{
  static const int fxm[2] = { 0x01, 0x80 };
  for (int v = 0; v < 2; v++) {
    g_init(&G[v]);
    g_gpr(&G[v], 9, CR_EQ0);
    GA(&G[v], XI_mtocrf, 0, fxm[v], 9);
    run_gen(&M[v], ctx, &G[v], v ? "mtocrf 0x80,rS" : "mtocrf 0x01,rS", 1);
    tmeta_t t = TM("mtocrf", NULL, v ? "fxm_80" : "fxm_01");
    emit_meas(ctx, v ? "tput_80" : "tput_01", "rthroughput", XI_mtocrf, gword(&G[v], 0), &M[v], &t);
  }
  g_init(&G[2]);
  g_gpr(&G[2], 5, CR_EQ0);
  GA(&G[2], XI_mtocrf, 0, 0x20, 5);
  GA(&G[2], XI_mfocrf, 0, 5, 0x20);
  run_gen(&M[2], ctx, &G[2], "mtocrf 0x20,r ; mfocrf r,0x20", 28);
  tmeta_t t = TM("mtocrf", "mfocrf", "roundtrip");
  emit_meas(ctx, "roundtrip", "latency", XI_mtocrf, gword(&G[2], 0), &M[2], &t);
  return 0;
}

static int h_mfocrf(xts_ctx_t *ctx, const ent_t *e)
{
  static const int fxm[2] = { 0x01, 0x80 };
  for (int v = 0; v < 2; v++) {
    g_init(&G[v]);
    GA(&G[v], XI_mfocrf, 0, 5, fxm[v]);
    g_rot(&G[v], 0, 4, 1);
    run_gen(&M[v], ctx, &G[v], v ? "mfocrf rD,0x80" : "mfocrf rD,0x01", 26);
    tmeta_t t = TM("mfocrf", NULL, v ? "fxm_80" : "fxm_01");
    emit_meas(ctx, v ? "tput_80" : "tput_01", "rthroughput", XI_mfocrf, gword(&G[v], 0), &M[v], &t);
  }
  /* compare -> mfocrf: the CR field written by a compare read back as a GPR */
  g_init(&G[2]);
  g_gpr(&G[2], 3, 0);
  g_gpr(&G[2], 4, 0);
  GA(&G[2], XI_cmp, 0, 1, 1, 3, 4);
  GA(&G[2], XI_mfocrf, 0, 5, 0x40);
  run_gen(&M[2], ctx, &G[2], "cmp cr1 ; mfocrf rD,cr1", 27);
  tmeta_t t = TM("cmp", "mfocrf", "compare_then_mfocrf");
  emit_meas(ctx, "cmp_dep", "cycles", XI_mfocrf, gword(&G[2], 1), &M[2], &t);
  t = TM("cmp", "mfocrf", "dependent_minus_mfocrf_alone");
  emit_diff(ctx, "cmp_dep_extra", "extra", XI_mfocrf, gword(&G[2], 1), &M[2], &M[0], &t);
  return 0;
}

/* ---- LR / CTR / XER moves --------------------------------------------------------------------------------------------- */
static int h_spr(xts_ctx_t *ctx, const ent_t *e)
{
  int spr = e->a;
  const char *nm = spr == 8 ? "lr" : spr == 9 ? "ctr" : "xer";
  uint64_t val = spr == 1 ? 0 : 0x40;
  char mf[8], mt[8];
  snprintf(mf, sizeof mf, "mf%s", nm);
  snprintf(mt, sizeof mt, "mt%s", nm);
  for (int v = 0; v < 3; v++) {
    g_init(&G[v]);
    G[v].gpr_loop = spr == 9;
    g_gpr(&G[v], 9, val);
    g_gpr(&G[v], 5, val);
  }
  GA(&G[0], XI_mfspr, 0, 5, spr);
  g_rot(&G[0], 0, 4, 1);
  GA(&G[1], XI_mtspr, 0, spr, 9);
  GA(&G[2], XI_mtspr, 0, spr, 5);
  GA(&G[2], XI_mfspr, 0, 5, spr);
  double est = spr == 1 ? 25 : 3;
  run_gen(&M[0], ctx, &G[0], "mf<spr> rD", est);
  run_gen(&M[1], ctx, &G[1], "mt<spr> rS", est);
  run_gen(&M[2], ctx, &G[2], "mt<spr> r ; mf<spr> r", est * 2 + 4);
  tmeta_t t = TM(mf, NULL, "rotating_dest");
  emit_meas(ctx, "mf_tput", "rthroughput", XI_mfspr, gword(&G[0], 0), &M[0], &t);
  t = TM(mt, NULL, "same_source");
  emit_meas(ctx, "mt_tput", "rthroughput", XI_mtspr, gword(&G[1], 0), &M[1], &t);
  t = TM(mt, mf, "roundtrip");
  emit_meas(ctx, "roundtrip", "latency", XI_mtspr, gword(&G[2], 0), &M[2], &t);
  return 0;
}

static int h_mftb(xts_ctx_t *ctx, const ent_t *e)
{
  static const int tbr[2] = { 268, 269 };
  for (int v = 0; v < 2; v++) {
    g_init(&G[v]);
    GA(&G[v], XI_mftb, 0, 5, tbr[v]);
    g_rot(&G[v], 0, 4, 1);
    run_gen(&M[v], ctx, &G[v], v ? "mftb rD,269" : "mftb rD,268", 44);
    tmeta_t t = TM("mftb", NULL, v ? "tbu_269" : "tbl_268");
    emit_meas(ctx, v ? "tput_269" : "tput_268", "rthroughput", XI_mftb, gword(&G[v], 0), &M[v], &t);
  }
  return 0;
}

/* ---- FPSCR ------------------------------------------------------------------------------------------------------------ */
static int h_mffs(xts_ctx_t *ctx, const ent_t *e)
{
  for (int v = 0; v < 2; v++) {
    g_init(&G[v]);
    GA(&G[v], XI_mffsx, v ? XTS_RC : 0, 0);
    g_rot(&G[v], 0, 4, 1);
  }
  run_gen(&M[0], ctx, &G[0], "mffs fD", 30);
  run_gen(&M[1], ctx, &G[1], "mffs. fD", 30);
  tmeta_t t = TM("mffs", NULL, "rotating_dest");
  emit_meas(ctx, "tput", "rthroughput", XI_mffsx, gword(&G[0], 0), &M[0], &t);
  t = TM("mffs.", NULL, "rc_rotating_dest");
  emit_meas(ctx, "tput_rc", "rthroughput", XI_mffsx, gword(&G[1], 0), &M[1], &t);
  t = TM("mffs.", "mffs", "rc_minus_plain");
  emit_diff(ctx, "rc_extra", "extra", XI_mffsx, gword(&G[1], 0), &M[1], &M[0], &t);
  return 0;
}

static int h_mtfsf(xts_ctx_t *ctx, const ent_t *e)
{
  static const int fm[2] = { 0x01, 0xFF };
  for (int v = 0; v < 2; v++) {
    g_init(&G[v]);
    g_fpr(&G[v], 4, 0);
    GA(&G[v], XI_mtfsfx, 0, fm[v], 4);
  }
  g_init(&G[2]);
  g_fpr(&G[2], 4, 0);
  GA(&G[2], XI_mtfsfx, 0, 0xFF, 4);
  GA(&G[2], XI_mffsx, 0, 4);
  g_init(&G[3]);
  g_fpr(&G[3], 4, 0);
  GA(&G[3], XI_mtfsfx, XTS_RC, 0xFF, 4);
  run_gen(&M[0], ctx, &G[0], "mtfsf 0x01,f(0.0)", 1);
  run_gen(&M[1], ctx, &G[1], "mtfsf 0xFF,f(0.0)", 1);
  run_gen(&M[2], ctx, &G[2], "mtfsf 0xFF,f ; mffs f", 40);
  run_gen(&M[3], ctx, &G[3], "mtfsf. 0xFF,f(0.0)", 1);
  tmeta_t t = TM("mtfsf", NULL, "fm_01");
  emit_meas(ctx, "tput_01", "rthroughput", XI_mtfsfx, gword(&G[0], 0), &M[0], &t);
  t = TM("mtfsf", NULL, "fm_FF");
  emit_meas(ctx, "tput_FF", "rthroughput", XI_mtfsfx, gword(&G[1], 0), &M[1], &t);
  t = TM("mtfsf", "mffs", "roundtrip");
  emit_meas(ctx, "roundtrip", "latency", XI_mtfsfx, gword(&G[2], 0), &M[2], &t);
  t = TM("mtfsf.", NULL, "rc_fm_FF");
  emit_meas(ctx, "tput_rc", "rthroughput", XI_mtfsfx, gword(&G[3], 0), &M[3], &t);
  t = TM("mtfsf.", "mtfsf", "rc_minus_plain");
  emit_diff(ctx, "rc_extra", "extra", XI_mtfsfx, gword(&G[3], 0), &M[3], &M[1], &t);
  return 0;
}

static int h_mtfsfi(xts_ctx_t *ctx, const ent_t *e)
{
  g_init(&G[0]);
  GA(&G[0], XI_mtfsfix, 0, 7, 0);
  g_init(&G[1]);
  GA(&G[1], XI_mtfsfix, XTS_RC, 7, 0);
  run_gen(&M[0], ctx, &G[0], "mtfsfi 7,0", 1);
  run_gen(&M[1], ctx, &G[1], "mtfsfi. 7,0", 1);
  tmeta_t t = TM("mtfsfi", NULL, "field_7");
  emit_meas(ctx, "tput", "rthroughput", XI_mtfsfix, gword(&G[0], 0), &M[0], &t);
  t = TM("mtfsfi.", NULL, "rc_field_7");
  emit_meas(ctx, "tput_rc", "rthroughput", XI_mtfsfix, gword(&G[1], 0), &M[1], &t);
  t = TM("mtfsfi.", "mtfsfi", "rc_minus_plain");
  emit_diff(ctx, "rc_extra", "extra", XI_mtfsfix, gword(&G[1], 0), &M[1], &M[0], &t);
  return 0;
}

static int h_mtfsb(xts_ctx_t *ctx, const ent_t *e)
{
  g_init(&G[0]);
  GA(&G[0], XI_mtfsb1x, 0, 31);
  g_init(&G[1]);
  GA(&G[1], XI_mtfsb0x, 0, 31);
  run_gen(&M[0], ctx, &G[0], "mtfsb1 31", 1);
  run_gen(&M[1], ctx, &G[1], "mtfsb0 31", 1);
  tmeta_t t = TM("mtfsb1", NULL, "bit_31");
  emit_meas(ctx, "tput_mtfsb1", "rthroughput", XI_mtfsb1x, gword(&G[0], 0), &M[0], &t);
  t = TM("mtfsb0", NULL, "bit_31");
  emit_meas(ctx, "tput_mtfsb0", "rthroughput", XI_mtfsb0x, gword(&G[1], 0), &M[1], &t);
  return 0;
}

static int h_mcrfs(xts_ctx_t *ctx, const ent_t *e)
{
  g_init(&G[0]);
  GA(&G[0], XI_mcrfs, 0, 2, 7);
  g_rot(&G[0], 0, 4, 1);
  run_gen(&M[0], ctx, &G[0], "mcrfs crF,7", 13);
  tmeta_t t = TM("mcrfs", NULL, "rotating_cr_dest");
  emit_meas(ctx, "tput", "rthroughput", XI_mcrfs, gword(&G[0], 0), &M[0], &t);
  return 0;
}

/* ---- VSCR ------------------------------------------------------------------------------------------------------------- */
static int h_vscr(xts_ctx_t *ctx, const ent_t *e)
{
  for (int v = 0; v < 3; v++) {
    g_init(&G[v]);
    for (int r = 0; r < 6; r++) g_vz(&G[v], r);        /* VSCR source registers zero: NJ and SAT stay clear */
  }
  GA(&G[0], XI_mfvscr, 0, 0);
  g_rot(&G[0], 0, 4, 1);
  GA(&G[1], XI_mtvscr, 0, 4);
  GA(&G[2], XI_mtvscr, 0, 5);
  GA(&G[2], XI_mfvscr, 0, 5);
  run_gen(&M[0], ctx, &G[0], "mfvscr vD", 40);
  run_gen(&M[1], ctx, &G[1], "mtvscr vB(0)", 40);
  run_gen(&M[2], ctx, &G[2], "mtvscr v ; mfvscr v", 80);
  tmeta_t t = TM("mfvscr", NULL, "rotating_dest");
  emit_meas(ctx, "mf_tput", "rthroughput", XI_mfvscr, gword(&G[0], 0), &M[0], &t);
  t = TM("mtvscr", NULL, "zero_source");
  emit_meas(ctx, "mt_tput", "rthroughput", XI_mtvscr, gword(&G[1], 0), &M[1], &t);
  t = TM("mtvscr", "mfvscr", "roundtrip");
  emit_meas(ctx, "roundtrip", "latency", XI_mtvscr, gword(&G[2], 0), &M[2], &t);
  return 0;
}

/* ---- barriers --------------------------------------------------------------------------------------------------------- */
static const char *const bar_name[5] = { "sync", "lwsync", "ptesync", "eieio", "isync" };

static void bar_slot(gen_t *g, int which)
{
  switch (which) {
  case 0: GA(g, XI_sync, 0); break;
  case 1: GA(g, XI_lwsync, 0); break;
  case 2: GA(g, XI_ptesync, 0); break;
  case 3: GA(g, XI_eieio, 0); break;
  default: GA(g, XI_isync, 0); break;
  }
}

static int h_barrier(xts_ctx_t *ctx, const ent_t *e)
{
  int which = e->a, insn = which == 3 ? XI_eieio : which == 4 ? XI_isync : XI_sync;
  double est = e->b;
  const char *bn = bar_name[which];
  for (int v = 0; v < 3; v++) {
    g_init(&G[v]);
    g_ptr(&G[v], 20, 0);
    g_gpr(&G[v], 21, 1);
    if (v == 1) GA(&G[v], XI_stw, 0, 21, 0, 20);
    if (v == 2) GA(&G[v], XI_lwz, 0, 22, 0, 20);
    bar_slot(&G[v], which);
  }
  char u0[40], u1[40], u2[40];
  snprintf(u0, sizeof u0, "%s", bn);
  snprintf(u1, sizeof u1, "stw ; %s", bn);
  snprintf(u2, sizeof u2, "lwz ; %s", bn);
  run_gen(&M[0], ctx, &G[0], u0, est);
  run_gen(&M[1], ctx, &G[1], u1, est + 30);
  run_gen(&M[2], ctx, &G[2], u2, est + 20);
  uint32_t w = gword(&G[0], 0);
  tmeta_t t = TM(bn, NULL, "alone");
  emit_meas(ctx, "alone", "issue_cost", insn, w, &M[0], &t);
  t = TM("stw", bn, "after_store");
  emit_meas(ctx, "after_store", "cycles", insn, w, &M[1], &t);
  t = TM("lwz", bn, "after_load");
  emit_meas(ctx, "after_load", "cycles", insn, w, &M[2], &t);
  t = TM("stw", bn, "after_store_minus_alone");
  emit_diff(ctx, "extra_after_store", "extra", insn, w, &M[1], &M[0], &t);
  t = TM("lwz", bn, "after_load_minus_alone");
  emit_diff(ctx, "extra_after_load", "extra", insn, w, &M[2], &M[0], &t);
  return 0;
}

/* ---- load-reserve / store-conditional --------------------------------------------------------------------------------- */
static int h_reserve(xts_ctx_t *ctx, const ent_t *e)
{
  int dw = e->a, la = dw ? XI_ldarx : XI_lwarx, sc = dw ? XI_stdcx : XI_stwcx;
  const char *ln = dw ? "ldarx" : "lwarx", *sn = dw ? "stdcx." : "stwcx.";
  for (int v = 0; v < 4; v++) {
    g_init(&G[v]);
    g_ptr(&G[v], 20, 0);
    g_gpr(&G[v], 21, 1);
  }
  GA(&G[0], la, 0, 5, 0, 20);
  g_rot(&G[0], 0, 4, 1);
  GA(&G[1], sc, 0, 21, 0, 20);                         /* no reservation held: the store fails */
  GA(&G[2], la, 0, 5, 0, 20);
  GA(&G[2], sc, 0, 5, 0, 20);
  GA(&G[3], la, 0, 5, 0, 20);
  GA(&G[3], XI_addi, 0, 5, 5, 1);
  GA(&G[3], sc, 0, 5, 0, 20);
  run_gen(&M[0], ctx, &G[0], "larx rD,0,rP", 65);
  run_gen(&M[1], ctx, &G[1], "stcx. rS,0,rP (no reservation)", 76);
  run_gen(&M[2], ctx, &G[2], "larx ; stcx.", 127);
  run_gen(&M[3], ctx, &G[3], "larx ; addi ; stcx.", 130);
  tmeta_t t = TM(ln, NULL, "alone");
  emit_meas(ctx, "larx_alone", "issue_cost", la, gword(&G[0], 0), &M[0], &t);
  t = TM(sn, NULL, "alone_no_reservation");
  emit_meas(ctx, "stcx_alone", "issue_cost", sc, gword(&G[1], 0), &M[1], &t);
  t = TM(ln, sn, "pair_success");
  emit_meas(ctx, "pair", "cycles", sc, gword(&G[2], 1), &M[2], &t);
  t = TM(ln, sn, "read_modify_write");
  emit_meas(ctx, "rmw", "cycles", sc, gword(&G[3], 2), &M[3], &t);
  t = TM("addi", sn, "rmw_minus_pair");
  emit_diff(ctx, "rmw_extra", "extra", XI_addi, gword(&G[3], 1), &M[3], &M[2], &t);
  return 0;
}

/* ---- record-form cost ------------------------------------------------------------------------------------------------- */
static const struct rcop { int insn; int shape; double est; } rcops[] = {
  { XI_addx, 0, 2 }, { XI_subfx, 0, 2 }, { XI_andx, 0, 2 }, { XI_orx, 0, 2 }, { XI_xorx, 0, 2 }, { XI_norx, 0, 2 },
  { XI_negx, 1, 2 }, { XI_extsbx, 1, 2 }, { XI_cntlzwx, 1, 2 }, { XI_rlwinmx, 2, 2 }, { XI_rldiclx, 3, 2 },
  { XI_slwx, 0, 16 }, { XI_mullwx, 0, 11 }, { XI_addcx, 0, 2 },
};

/* shape 0: op rd,ra,rb  1: op rd,ra  2: rlwinm ra,rs,1,0,31  3: rldicl ra,rs,1,0.  lat: chained through r3, else rotating dest */
static void rc_slot(gen_t *g, int insn, unsigned fl, int shape, int lat)
{
  int d = lat ? 3 : 5;
  switch (shape) {
  case 0: GA(g, insn, fl, d, 3, 4); break;
  case 1: GA(g, insn, fl, d, 3); break;
  case 2: GA(g, insn, fl, d, 3, 1, 0, 31); break;
  default: GA(g, insn, fl, d, 3, 1, 0); break;
  }
  if (!lat) g_rot(g, 0, 4, 1);
}

static int h_rc(xts_ctx_t *ctx, const ent_t *e)
{
  const struct rcop *o = &rcops[e->a];
  const char *mn = xts_isa[o->insn].mnem;
  for (int v = 0; v < 4; v++) {
    g_init(&G[v]);
    g_gpr(&G[v], 3, 0x123456789ull);
    g_gpr(&G[v], 4, 0x7);
    rc_slot(&G[v], o->insn, (v & 1) ? XTS_RC : 0, o->shape, v < 2);
  }
  run_gen(&M[0], ctx, &G[0], "plain chain", o->est);
  run_gen(&M[1], ctx, &G[1], "record-form chain", o->est);
  run_gen(&M[2], ctx, &G[2], "plain independent", o->est);
  run_gen(&M[3], ctx, &G[3], "record-form independent", o->est);
  uint32_t wp = gword(&G[0], 0), wr = gword(&G[1], 0);
  tmeta_t t = TM(mn, mn, "chain_plain");
  t.baseline = 1;
  emit_meas(ctx, "lat_plain", "latency", o->insn, wp, &M[0], &t);
  t = TM(mn, mn, "chain_rc");
  emit_meas(ctx, "lat_rc", "latency", o->insn, wr, &M[1], &t);
  t = TM(mn, mn, "rc_minus_plain_chain");
  emit_diff(ctx, "lat_rc_extra", "extra", o->insn, wr, &M[1], &M[0], &t);
  t = TM(mn, mn, "independent_plain");
  t.baseline = 1;
  emit_meas(ctx, "tput_plain", "rthroughput", o->insn, wp, &M[2], &t);
  t = TM(mn, mn, "independent_rc");
  emit_meas(ctx, "tput_rc", "rthroughput", o->insn, wr, &M[3], &t);
  t = TM(mn, mn, "rc_minus_plain_independent");
  emit_diff(ctx, "tput_rc_extra", "extra", o->insn, wr, &M[3], &M[2], &t);
  return 0;
}

/* ---- andi. / andis.: why 14 cycles --------------------------------------------------------------------------------------
 * andi. is a record-form immediate; every other record-form ALU op costs about one extra cycle at most. Measured here:
 * issue (independent) and chain, andis., a zero result vs a non-zero result, the same mask with rlwinm. and and.,
 * overlap with k following independent li (does the pipeline keep going while andi. is "busy"?), and a dependent
 * branch on CR0 after andi. */
static int h_andi(xts_ctx_t *ctx, const ent_t *e)
{
  /* 0 andi. indep, 1 andi. chain, 2 andis. indep, 3 andis. chain, 4 andi. zero result, 5 rlwinm., 6 and. */
  for (int v = 0; v < 7; v++) {
    g_init(&G[v]);
    g_gpr(&G[v], 3, 0x00000000C0DE5A5Aull);
    g_gpr(&G[v], 4, 0x00000000C0DE5A5Aull);
  }
  GA(&G[0], XI_andi_, 0, 5, 3, 0xFF);                  g_rot(&G[0], 0, 4, 1);
  GA(&G[1], XI_andi_, 0, 3, 3, 0xFFFF);
  GA(&G[2], XI_andis_, 0, 5, 3, 0xFFFF);               g_rot(&G[2], 0, 4, 1);
  GA(&G[3], XI_andis_, 0, 3, 3, 0xFFFF);
  GA(&G[4], XI_andi_, 0, 5, 3, 0);                     g_rot(&G[4], 0, 4, 1);
  GA(&G[5], XI_rlwinmx, XTS_RC, 5, 3, 0, 24, 31);      g_rot(&G[5], 0, 4, 1);
  GA(&G[6], XI_andx, XTS_RC, 5, 3, 4);                 g_rot(&G[6], 0, 4, 1);
  static const char *const un[7] = { "andi. rD,rS,0xFF", "andi. r3,r3,0xFFFF", "andis. rD,rS,0xFFFF", "andis. r3,r3,0xFFFF",
                                     "andi. rD,rS,0 (result 0)", "rlwinm. rD,rS,0,24,31", "and. rD,rS,rB" };
  static const char *const va[7] = { "andi_independent", "andi_chain", "andis_independent", "andis_chain", "andi_zero_result",
                                     "rlwinm_rc_same_mask", "and_rc" };
  static const char *const su[7] = { "andi_tput", "andi_chain", "andis_tput", "andis_chain", "andi_zero", "rlwinm_rc", "and_rc" };
  static const char *const me[7] = { "rthroughput", "latency", "rthroughput", "latency", "rthroughput", "rthroughput", "rthroughput" };
  static const int ins[7] = { XI_andi_, XI_andi_, XI_andis_, XI_andis_, XI_andi_, XI_rlwinmx, XI_andx };
  for (int v = 0; v < 7; v++) {
    run_gen(&M[v], ctx, &G[v], un[v], v == 5 || v == 6 ? 2 : 14);
    tmeta_t t = TM(xts_isa[ins[v]].mnem, NULL, va[v]);
    t.baseline = v < 2;
    emit_meas(ctx, su[v], me[v], ins[v], gword(&G[v], 0), &M[v], &t);
  }
  /* overlap with following independent li: unit = andi. + k li; reference: k li alone */
  /* a unit holds at most MAXSL slots, so the sweep is k = 1..5 li after the andi. */
  static const int kk[5] = { 1, 2, 3, 4, 5 };
  for (int q = 0; q < 5; q++) {
    int k = kk[q];
    gen_t *g = &G[7 + q], *h = &G[12 + q];
    g_init(g);
    g_init(h);
    g_gpr(g, 3, 0x00000000C0DE5A5Aull);
    GA(g, XI_andi_, 0, 5, 3, 0xFF);
    for (int i = 0; i < k; i++) { GA(g, XI_addi, 0, 14 + i, 0, 1); GA(h, XI_addi, 0, 14 + i, 0, 1); }
  }
  for (int q = 0; q < 5; q++) {
    char u[40], hu[40];
    snprintf(u, sizeof u, "andi. + %d li", kk[q]);
    snprintf(hu, sizeof hu, "%d li alone", kk[q]);
    run_gen(&M[7 + q], ctx, &G[7 + q], u, 14 + kk[q]);
    run_gen(&M[12 + q], ctx, &G[12 + q], hu, kk[q]);
  }
  for (int q = 0; q < 5; q++) {
    char s1[24], s2[24], s3[24], var[32];
    snprintf(s1, sizeof s1, "overlap_k%d", kk[q]);
    snprintf(var, sizeof var, "andi_plus_%d_li", kk[q]);
    tmeta_t t = TM("andi.", "addi", var);
    /* hidden cycles = andi. alone + k li alone - (andi. + k li) */
    double hidden = mcyc(&M[0]) + mcyc(&M[12 + q]) - mcyc(&M[7 + q]);
    int st = xts_status_worst(xts_status_worst(mstatus(&M[0]), mstatus(&M[12 + q])), mstatus(&M[7 + q]));
    emit_val(ctx, s1, "overlap", "cycles", XI_andi_, gword(&G[0], 0), hidden, st, &M[7 + q], &t,
             "andi. alone + k li alone - (andi. + k li): cycles of the li stream hidden behind andi.");
    snprintf(s2, sizeof s2, "andi_plus_k%d", kk[q]);
    emit_meas(ctx, s2, "cycles", XI_andi_, gword(&G[0], 0), &M[7 + q], &t);
    snprintf(s3, sizeof s3, "li_alone_k%d", kk[q]);
    t = tm_make("addi", NULL, "li_alone", kk[q], -1);
    emit_meas(ctx, s3, "rthroughput", XI_addi, gword(&G[12 + q], 0), &M[12 + q], &t);
  }
  /* dependent branch on CR0 after andi. vs a branch on an unrelated field (both taken to the next word) */
  g_init(&G[17]);
  g_gpr(&G[17], 3, 0x00000000C0DE5A5Aull);
  GA(&G[17], XI_andi_, 0, 5, 3, 0xFF);
  GA(&G[17], XI_bcx, 0, 4, 2, 4);                      /* bne cr0 (result non-zero: taken) */
  g_init(&G[18]);
  g_gpr(&G[18], 3, 0x00000000C0DE5A5Aull);
  GA(&G[18], XI_andi_, 0, 5, 3, 0xFF);
  GA(&G[18], XI_bcx, 0, 4, 30, 4);                     /* bne cr7 (zero field: taken) */
  run_gen(&M[17], ctx, &G[17], "andi. ; bne cr0,.+4", 20);
  run_gen(&M[18], ctx, &G[18], "andi. ; bne cr7,.+4", 20);
  tmeta_t t = TM("andi.", "bcx", "branch_on_cr0");
  emit_meas(ctx, "andi_bc_dep", "cycles", XI_andi_, gword(&G[17], 0), &M[17], &t);
  t = TM("andi.", "bcx", "branch_on_other_field");
  emit_meas(ctx, "andi_bc_ind", "cycles", XI_andi_, gword(&G[18], 0), &M[18], &t);
  t = TM("andi.", "bcx", "dependent_minus_independent");
  emit_diff(ctx, "andi_bc_penalty", "penalty", XI_andi_, gword(&G[17], 0), &M[17], &M[18], &t);
  return 0;
}

/* ---- carry (XER[CA]) chains ------------------------------------------------------------------------------------------- */
static int h_ca(xts_ctx_t *ctx, const ent_t *e)
{
  for (int v = 0; v < 9; v++) {
    g_init(&G[v]);
    g_gpr(&G[v], 3, 0x00000000FFFFFFF0ull);
    g_gpr(&G[v], 4, 0x0000000000000021ull);
  }
  GA(&G[0], XI_addex, 0, 3, 3, 4);                                         /* dependent through RA and CA */
  GA(&G[1], XI_addex, 0, 5, 3, 4);  g_rot(&G[1], 0, 4, 1);                 /* dependent only through CA */
  GA(&G[2], XI_addcx, 0, 5, 3, 4);  g_rot(&G[2], 0, 4, 1);                 /* writes CA, reads nothing */
  GA(&G[3], XI_addcx, 0, 5, 3, 4);  GA(&G[3], XI_addex, 0, 6, 3, 4);       /* addc -> adde */
  GA(&G[4], XI_addzex, 0, 3, 3);                                           /* addze chain */
  GA(&G[5], XI_subfex, 0, 3, 3, 4);                                        /* subfe chain */
  GA(&G[6], XI_addic, 0, 5, 3, 1);  g_rot(&G[6], 0, 4, 1);                 /* addic: writes CA */
  GA(&G[7], XI_addcx, 0, 5, 3, 4);  GA(&G[7], XI_mfspr, 0, 6, 1);          /* addc ; mfxer */
  GA(&G[8], XI_mfspr, 0, 6, 1);                                            /* mfxer alone */
  static const char *const un[9] = { "adde r3,r3,r4", "adde rD,r3,r4 (CA only)", "addc rD,r3,r4", "addc ; adde", "addze r3,r3",
                                     "subfe r3,r3,r4", "addic rD,r3,1", "addc ; mfxer", "mfxer" };
  static const double es[9] = { 2, 2, 2, 4, 2, 2, 2, 28, 25 };
  for (int v = 0; v < 9; v++) run_gen(&M[v], ctx, &G[v], un[v], es[v]);
  uint32_t wa = gword(&G[0], 0);
  tmeta_t t = TM("adde", "adde", "chain_ra_and_ca");
  t.baseline = 1;
  emit_meas(ctx, "adde_chain", "latency", XI_addex, wa, &M[0], &t);
  t = TM("adde", "adde", "ca_only_dependency");
  emit_meas(ctx, "adde_ca_only", "rthroughput", XI_addex, gword(&G[1], 0), &M[1], &t);
  t = TM("addc", NULL, "independent");
  emit_meas(ctx, "addc_tput", "rthroughput", XI_addcx, gword(&G[2], 0), &M[2], &t);
  t = TM("addc", "adde", "ca_dependency_minus_addc_independent");
  emit_diff(ctx, "ca_read_extra", "extra", XI_addex, gword(&G[1], 0), &M[1], &M[2], &t);
  t = tm_make("addc", "adde", "pair", 2, -1);
  emit_meas(ctx, "addc_adde_pair", "rthroughput", XI_addex, gword(&G[3], 1), &M[3], &t);
  t = TM("addze", "addze", "chain_ra_and_ca");
  emit_meas(ctx, "addze_chain", "latency", XI_addzex, gword(&G[4], 0), &M[4], &t);
  t = TM("subfe", "subfe", "chain_ra_and_ca");
  emit_meas(ctx, "subfe_chain", "latency", XI_subfex, gword(&G[5], 0), &M[5], &t);
  t = TM("addic", NULL, "independent");
  emit_meas(ctx, "addic_tput", "rthroughput", XI_addic, gword(&G[6], 0), &M[6], &t);
  t = TM("addc", "mfspr", "addc_then_mfxer");
  emit_meas(ctx, "addc_mfxer", "cycles", XI_mfspr, gword(&G[7], 1), &M[7], &t);
  t = TM("mfspr", NULL, "mfxer_alone");
  emit_meas(ctx, "mfxer_alone", "rthroughput", XI_mfspr, gword(&G[8], 0), &M[8], &t);
  t = TM("addc", "mfspr", "addc_then_mfxer_minus_mfxer_alone");
  emit_diff(ctx, "ca_to_xer_extra", "extra", XI_mfspr, gword(&G[7], 1), &M[7], &M[8], &t);
  return 0;
}

/* ---- case table --------------------------------------------------------------------------------------------------------- */
static const ent_t ents[] = {
  { "cmp.L0", h_compare, XI_cmp, 0 }, { "cmp.L1", h_compare, XI_cmp, 1 },
  { "cmpl.L0", h_compare, XI_cmpl, 0 }, { "cmpl.L1", h_compare, XI_cmpl, 1 },
  { "cmpi.L0", h_compare, XI_cmpi, 0 }, { "cmpi.L1", h_compare, XI_cmpi, 1 },
  { "cmpli.L0", h_compare, XI_cmpli, 0 }, { "cmpli.L1", h_compare, XI_cmpli, 1 },
  { "crand", h_crlogical, XI_crand, 0 }, { "crandc", h_crlogical, XI_crandc, 0 }, { "creqv", h_crlogical, XI_creqv, 0 },
  { "crnand", h_crlogical, XI_crnand, 0 }, { "crnor", h_crlogical, XI_crnor, 0 }, { "cror", h_crlogical, XI_cror, 0 },
  { "crorc", h_crlogical, XI_crorc, 0 }, { "crxor", h_crlogical, XI_crxor, 0 },
  { "mcrf", h_mcrf, 0, 0 }, { "mfcr", h_mfcr, 0, 0 }, { "mtcrf", h_mtcrf, 0, 0 }, { "mtocrf", h_mtocrf, 0, 0 },
  { "mfocrf", h_mfocrf, 0, 0 },
  { "spr.lr", h_spr, 8, 0 }, { "spr.ctr", h_spr, 9, 0 }, { "spr.xer", h_spr, 1, 0 },
  { "mftb", h_mftb, 0, 0 },
  { "fpscr.mffs", h_mffs, 0, 0 }, { "fpscr.mtfsf", h_mtfsf, 0, 0 }, { "fpscr.mtfsfi", h_mtfsfi, 0, 0 },
  { "fpscr.mtfsb", h_mtfsb, 0, 0 }, { "fpscr.mcrfs", h_mcrfs, 0, 0 },
  { "vscr", h_vscr, 0, 0 },
  { "barrier.sync", h_barrier, 0, 165 }, { "barrier.lwsync", h_barrier, 1, 35 }, { "barrier.ptesync", h_barrier, 2, 165 },
  { "barrier.eieio", h_barrier, 3, 92 }, { "barrier.isync", h_barrier, 4, 28 },
  { "reserve.w", h_reserve, 0, 0 }, { "reserve.d", h_reserve, 1, 0 },
  { "rc.addx", h_rc, 0, 0 }, { "rc.subfx", h_rc, 1, 0 }, { "rc.andx", h_rc, 2, 0 }, { "rc.orx", h_rc, 3, 0 },
  { "rc.xorx", h_rc, 4, 0 }, { "rc.norx", h_rc, 5, 0 }, { "rc.negx", h_rc, 6, 0 }, { "rc.extsbx", h_rc, 7, 0 },
  { "rc.cntlzwx", h_rc, 8, 0 }, { "rc.rlwinmx", h_rc, 9, 0 }, { "rc.rldiclx", h_rc, 10, 0 }, { "rc.slwx", h_rc, 11, 0 },
  { "rc.mullwx", h_rc, 12, 0 }, { "rc.addcx", h_rc, 13, 0 },
  { "rc.andi", h_andi, 0, 0 },
  { "ca.chain", h_ca, 0, 0 },
};
#define NENTS ((int)(sizeof ents / sizeof ents[0]))

static int sys_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < NENTS; i++) {
    xts_case_t *c = xts_case_add(cl, ents[i].id);
    if (!c) return -1;
    c->arg[0] = i;
  }
  return 0;
}

static int sys_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  int i = (int)c->arg[0];
  if (i < 0 || i >= NENTS) return -1;
  return ents[i].fn(ctx, &ents[i]);
}

XTS_FAMILY(sysops, .kind = "sched", .enumerate = sys_enum, .run = sys_run, .block_cases = 2,
           .desc = "CR / LR / CTR / XER / FPSCR / VSCR moves, barriers, larx/stcx, record-form and carry costs");
