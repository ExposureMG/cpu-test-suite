/* unit.conflict - do two independent instruction streams share a pipe/port, and how long does an unpipelined op keep it?
 *
 *   alone.<C>     C per unit, independent: T(C) = reciprocal throughput (metric rthroughput)
 *   mix.<A>+<B>   A and B interleaved at a:b = 1:1, 1:2, 2:1 and the "balanced" ratio a T(A) ~ b T(B) (largest contrast).
 *                 Serial bound S = a T(A) + b T(B), parallel bound Q = max(a T(A), b T(B), (a+b)/2 issue width).
 *                 overlap = (S - P) / (S - Q): 1 = throughputs add (different pipes), 0 = they serialise (shared
 *                 pipe/port). Per ratio one record (metric cycles); summary record (metric overlap) from the ratio with the
 *                 best contrast/error, verdict separate / shared / partial / UNDETERMINED (noise or no contrast).
 *   busy.<A>/<B>  A followed by k independent B (k = 0,1,2,4,8 and enough B to cover T(A)): cost(k) - k T(B) is what A
 *                 adds to B's stream. penalty at the covering k ~ 0..1: A's unit is busy but B keeps issuing
 *                 (non-blocking); ~ T(A): A holds the pipe/issue B needs (blocking). Records per k + summary (penalty).
 *   graph         (family finish hook, case id "graph", metric count) the pipe-class graph of each condition vector of the
 *                 profile row: every mix verdict as an edge, and the connected components of the "shared" edges (classes
 *                 that compete for one pipe). Built from the family's own mix records on disk (xts_results_foreach), so a
 *                 resumed run sees the mixes of earlier boots; pairs without a record are counted as missing. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sched_classes.h"

#define MAXU 32
typedef struct { uint64_t key; int valid; double cyc, err; int status; } uc_cache_t;
static uc_cache_t alone[MAXU];
static int ucls[MAXU], nucls;

static const char *const busy_pairs[][2] = {
  { "div", "mul" }, { "mul", "div" }, { "div", "int" }, { "mul", "int" }, { "varshift", "int" }, { "varshift", "mul" },
  { "recimm", "int" }, { "fdiv", "fp" }, { "fsqrt", "fp" }, { "fdiv", "vfp" }, { "fdiv", "int" }, { "div", "fp" },
  { "vest", "vfp" }, { "div", "lwz" }, { "fdiv", "lfd" }, { "fsqrt", "fdiv" },
};
#define NBUSY ((int)(sizeof busy_pairs / sizeof busy_pairs[0]))

static int upos(int ci)
{
  for (int i = 0; i < nucls; i++)
    if (ucls[i] == ci) return i;
  return -1;
}

static void build_ucls(void)
{
  sc_classes_init();
  nucls = 0;
  for (int i = 0; i < sc_nclasses && nucls < MAXU; i++)
    if (sc_class_ok(i) && (sc_classes[i].use & SC_U_UNIT) && strcmp(sc_classes[i].name, "filler")) ucls[nucls++] = i;
}

static int uc_enum(xts_caselist_t *cl)
{
  build_ucls();
  xts_case_t *c;
  for (int i = 0; i < nucls; i++) {
    if (!(c = xts_case_addf(cl, "alone.%s", sc_classes[ucls[i]].name))) return -1;
    c->arg[0] = 0;
    c->arg[1] = i;
    c->insn = sc_classes[ucls[i]].insn;
  }
  for (int i = 0; i < nucls; i++)
    for (int k = i + 1; k < nucls; k++) {
      if (!(c = xts_case_addf(cl, "mix.%s+%s", sc_classes[ucls[i]].name, sc_classes[ucls[k]].name))) return -1;
      c->arg[0] = 1;
      c->arg[1] = i;
      c->arg[2] = k;
    }
  for (int b = 0; b < NBUSY; b++) {
    int a = upos(sc_class_find(busy_pairs[b][0])), bb = upos(sc_class_find(busy_pairs[b][1]));
    if (a < 0 || bb < 0) continue;
    if (!(c = xts_case_addf(cl, "busy.%s/%s", busy_pairs[b][0], busy_pairs[b][1]))) return -1;
    c->arg[0] = 2;
    c->arg[1] = a;
    c->arg[2] = bb;
  }
  return 0;
}

static const uc_cache_t *get_alone(xts_ctx_t *ctx, int ui, int emit)
{
  uint64_t key = sc_cond_key(ctx);
  if (!emit && alone[ui].valid && alone[ui].key == key) return &alone[ui];
  static sc_meas_t m;
  int ci = ucls[ui];
  sc_unit_t u;
  sc_unit_init(&u);
  sc_unit_add(&u, ci, 0, 1);
  sc_measure(ctx, &u, sc_class_est(&sc_classes[ci]), &m);
  alone[ui].key = key;
  alone[ui].valid = 1;
  alone[ui].cyc = m.cyc;
  alone[ui].err = m.err;
  alone[ui].status = m.status;                           /* FAIL when the measurement failed */
  if (emit) {
    char buf[600];
    xts_json_t j;
    xj_init(&j, buf, sizeof buf);
    xj_obj(&j);
    xj_kv_str(&j, "class", sc_classes[ci].name);
    xj_kv_str(&j, "desc", sc_classes[ci].desc);
    xj_kv_str(&j, "shape", "one instruction per unit, independent");
    xj_kv_double(&j, "err", m.err);
    xj_finish(&j);
    xts_time_emit_ex(ctx, &m.s, &m.r, sc_classes[ci].insn, &(xts_temit_t){ .metric = "rthroughput", .extra_json = buf });
  }
  return &alone[ui];
}

/* unit with a A's and b B's spread evenly (blocks when the ratio does not fit the item limit) */
static void build_mix(sc_unit_t *u, int ca, int a, int cb, int b)
{
  sc_unit_init(u);
  if (a + b <= SC_MAX_ITEMS) {
    int acc = 0;
    for (int i = 0; i < a + b; i++) {           /* Bresenham: place the a A's among a+b positions */
      acc += a;
      if (acc >= a + b) { acc -= a + b; sc_unit_add(u, ca, 0, 1); }
      else sc_unit_add(u, cb, 1, 1);
    }
  } else {
    sc_unit_add(u, ca, 0, a);
    sc_unit_add(u, cb, 1, b);
  }
}

static int run_mix(xts_ctx_t *ctx, int ia, int ib)
{
  int ca = ucls[ia], cb = ucls[ib];
  const uc_cache_t *A = get_alone(ctx, ia, 0), *B = get_alone(ctx, ib, 0);
  int ratios[4][2] = { { 1, 1 }, { 1, 2 }, { 2, 1 }, { 0, 0 } }, nr = 3;
  double ta = A->cyc, tb = B->cyc;
  double ea = sc_class_est(&sc_classes[ca]), eb = sc_class_est(&sc_classes[cb]);
  double ra = (ta > 0 ? ta : ea), rb = (tb > 0 ? tb : eb);   /* the host has no measurement: use the estimates */
  int ba = 1, bb = 1;
  if (ra >= rb) bb = (int)floor(ra / rb + 0.5);
  else ba = (int)floor(rb / ra + 0.5);
  if (ba > 32) ba = 32;
  if (bb > 32) bb = 32;
  if (ba < 1) ba = 1;
  if (bb < 1) bb = 1;
  if (!((ba == 1 && bb <= 2) || (ba == 2 && bb == 1))) { ratios[3][0] = ba; ratios[3][1] = bb; nr = 4; }
  double best_q = -1, best_ov = NAN, best_eo = NAN;
  int best = -1, all_st = xts_status_worst(A->status, B->status);
  static sc_meas_t m[4];
  double ov[4], eo[4], S[4], Q[4];
  for (int r = 0; r < nr; r++) {
    int a = ratios[r][0], b = ratios[r][1];
    sc_unit_t u;
    build_mix(&u, ca, a, cb, b);
    sc_measure(ctx, &u, a * ea + b * eb, &m[r]);
    int st = xts_status_worst(m[r].status, xts_status_worst(A->status, B->status));
    all_st = xts_status_worst(all_st, st);
    S[r] = a * ta + b * tb;
    Q[r] = fmax(fmax(a * ta, b * tb), 0.5 * (a + b));
    double err = m[r].err + a * A->err + b * B->err, span = S[r] - Q[r];
    ov[r] = span > 0 ? (S[r] - m[r].cyc) / span : NAN;
    eo[r] = span > 0 ? err / span : NAN;
    const char *v = "UNDETERMINED", *why = NULL;
    if (st == XTS_ST_SKIP) why = "not executed (host)";
    else if (st != XTS_ST_PASS) why = "a measurement did not pass the quality gate";
    else if (!(span > 4 * err)) why = "serial and parallel bounds too close for the error bar";
    else if (m[r].cyc < Q[r] - 2 * err) { v = "anomaly_faster"; why = "faster than the parallel bound"; }
    else if (m[r].cyc > S[r] + 2 * err) { v = "interfere"; why = "slower than serial"; }
    else {
      const char *c0 = sc_tri(ov[r], 0.8, 0.2, "separate", "shared", "partial");
      if (!strcmp(c0, sc_tri(ov[r] - 2 * eo[r], 0.8, 0.2, "separate", "shared", "partial")) &&
          !strcmp(c0, sc_tri(ov[r] + 2 * eo[r], 0.8, 0.2, "separate", "shared", "partial"))) {
        v = c0;
        double q = span / err;
        if (q > best_q) { best_q = q; best = r; best_ov = ov[r]; best_eo = eo[r]; }
      } else why = "classification changes within the error bar";
    }
    char buf[1200];
    xts_json_t j;
    xj_init(&j, buf, sizeof buf);
    xj_obj(&j);
    xj_key(&j, "classes");
    xj_arr(&j);
    xj_str(&j, sc_classes[ca].name);
    xj_str(&j, sc_classes[cb].name);
    xj_end(&j);
    xj_key(&j, "ratio");
    xj_arr(&j);
    xj_int(&j, a);
    xj_int(&j, b);
    xj_end(&j);
    xj_kv_str(&j, "order", a + b <= SC_MAX_ITEMS ? "interleaved" : "blocks");
    xj_kv_double(&j, "t_a", ta);
    xj_kv_double(&j, "t_b", tb);
    xj_kv_double(&j, "serial", S[r]);
    xj_kv_double(&j, "parallel", Q[r]);
    xj_kv_double(&j, "overlap", ov[r]);
    xj_kv_double(&j, "overlap_err", eo[r]);
    xj_kv_str(&j, "verdict", v);
    if (why) xj_kv_str(&j, "why", why);
    xj_finish(&j);
    xts_time_emit_ex(ctx, &m[r].s, &m[r].r, -1, &(xts_temit_t){ .metric = "cycles", .extra_json = buf, .status = st });
  }
  const char *verdict = best >= 0 ? sc_tri(best_ov, 0.8, 0.2, "separate", "shared", "partial") : "UNDETERMINED";
  int consistent = 1;
  for (int r = 0; r < nr && best >= 0; r++)
    if (!isnan(ov[r]) && !isnan(eo[r]) && fabs(ov[r] - best_ov) > 2 * (eo[r] + best_eo) + 0.15) consistent = 0;
  char buf[1200];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_key(&j, "classes");
  xj_arr(&j);
  xj_str(&j, sc_classes[ca].name);
  xj_str(&j, sc_classes[cb].name);
  xj_end(&j);
  xj_kv_str(&j, "verdict", verdict);
  xj_kv_double(&j, "overlap_err", best_eo);
  if (best >= 0) {
    xj_key(&j, "from_ratio");
    xj_arr(&j);
    xj_int(&j, ratios[best][0]);
    xj_int(&j, ratios[best][1]);
    xj_end(&j);
    xj_kv_bool(&j, "ratios_consistent", consistent);
  }
  xj_key(&j, "overlap_by_ratio");
  xj_arr(&j);
  for (int r = 0; r < nr; r++) xj_double(&j, ov[r]);
  xj_end(&j);
  int st = all_st;
  if (st == XTS_ST_PASS && best < 0) st = XTS_ST_NOISY;
  xj_finish(&j);
  /* value = the best ratio's overlap (not the cycle figure): the fit shown is that ratio's, statistics stay null */
  xts_time_emit_value(ctx, best >= 0 ? &m[best].s : NULL, best >= 0 ? &m[best].r : NULL, -1, best_ov,
                      &(xts_temit_t){ .metric = "overlap", .unit = "ratio", .extra_json = buf, .status = st });
  return 0;
}

static int run_busy(xts_ctx_t *ctx, int ia, int ib)
{
  int ca = ucls[ia], cb = ucls[ib];
  const uc_cache_t *A = get_alone(ctx, ia, 0), *B = get_alone(ctx, ib, 0);
  double ta = A->cyc, tb = B->cyc;
  double ea = sc_class_est(&sc_classes[ca]), eb = sc_class_est(&sc_classes[cb]);
  double ra = ta > 0 ? ta : ea, rb = tb > 0 ? tb : eb;
  int kc = (int)ceil(ra / rb) + 2;
  if (kc > 96) kc = 96;
  int ks[6] = { 0, 1, 2, 4, 8, kc }, nk = 6;
  if (kc <= 8) nk = 5;
  static sc_meas_t m[6];
  double extra[6];
  int all_st = xts_status_worst(A->status, B->status);
  for (int i = 0; i < nk; i++) {
    sc_unit_t u;
    sc_unit_init(&u);
    sc_unit_add(&u, ca, 0, 1);
    if (ks[i]) sc_unit_add(&u, cb, 1, ks[i]);
    sc_measure(ctx, &u, fmax(ea, ks[i] * eb) + 1, &m[i]);
    int st = xts_status_worst(m[i].status, xts_status_worst(A->status, B->status));
    all_st = xts_status_worst(all_st, st);
    extra[i] = m[i].cyc - ks[i] * tb;
    char buf[600];
    xts_json_t j;
    xj_init(&j, buf, sizeof buf);
    xj_obj(&j);
    xj_kv_str(&j, "producer", sc_classes[ca].name);
    xj_kv_str(&j, "stream", sc_classes[cb].name);
    xj_kv_int(&j, "k", ks[i]);
    xj_kv_double(&j, "t_a", ta);
    xj_kv_double(&j, "t_b", tb);
    xj_kv_double(&j, "a_adds", extra[i]);
    xj_kv_double(&j, "hidden", ta + ks[i] * tb - m[i].cyc);
    xj_finish(&j);
    xts_time_emit_ex(ctx, &m[i].s, &m[i].r, sc_classes[ca].insn, &(xts_temit_t){ .metric = "cycles", .extra_json = buf, .status = st });
  }
  int last = nk - 1;
  double pen = extra[last], err = m[last].err + ks[last] * B->err;
  const char *v = "UNDETERMINED", *why = NULL;
  if (all_st == XTS_ST_SKIP) why = "not executed (host)";
  else if (all_st != XTS_ST_PASS) why = "a measurement did not pass the quality gate";
  else if (ks[last] * tb < ta) why = "not enough B work to cover A";
  else {
    double lo = fmax(1.5, 0.2 * ta), hi = 0.8 * ta;
    if (pen + 2 * err <= lo) v = "non-blocking";
    else if (pen - 2 * err >= hi) v = "blocking";
    else if (pen - 2 * err > lo && pen + 2 * err < hi) v = "partial";
    else why = "classification changes within the error bar";
  }
  char buf[1000];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_kv_str(&j, "producer", sc_classes[ca].name);
  xj_kv_str(&j, "stream", sc_classes[cb].name);
  xj_kv_double(&j, "t_a", ta);
  xj_kv_double(&j, "t_b", tb);
  xj_kv_int(&j, "k_cover", ks[last]);
  xj_kv_double(&j, "busy_cycles", pen);
  xj_kv_double(&j, "err", err);
  xj_key(&j, "k");
  xj_arr(&j);
  for (int i = 0; i < nk; i++) xj_int(&j, ks[i]);
  xj_end(&j);
  xj_key(&j, "a_adds");
  xj_arr(&j);
  for (int i = 0; i < nk; i++) xj_double(&j, extra[i]);
  xj_end(&j);
  xj_kv_str(&j, "verdict", v);
  if (why) xj_kv_str(&j, "why", why);
  int st = all_st;
  if (st == XTS_ST_PASS && !strcmp(v, "UNDETERMINED")) st = XTS_ST_NOISY;
  xj_finish(&j);
  /* value = the busy time A adds to B's stream, not a per-unit cycle figure */
  xts_time_emit_value(ctx, &m[last].s, &m[last].r, sc_classes[ca].insn, pen,
                      &(xts_temit_t){ .metric = "penalty", .extra_json = buf, .status = st });
  return 0;
}

/* ---- graph (family finish) ---- */
#define UC_MAXE 4096
typedef struct { int cs, rep, a, b; char code; } uc_edge_t;
static struct {
  int row, n, dropped;
  xts_condtab_t ct;
  uc_edge_t e[UC_MAXE];
} gscan;

/* one mix.A+B overlap record of the row -> an edge of its condition vector (the n-th record of a case = repeat n) */
static int graph_cb(const char *line, void *ud)
{
  xts_rline_t l;
  (void)ud;
  if (xts_rline_parse(line, &l) < 0 || xts_rline_row(&l) != gscan.row || strcmp(l.metric, "overlap") ||
      strncmp(l.case_id, "mix.", 4))
    return 0;
  char *plus = strchr(l.case_id + 4, '+');
  if (!plus) return 0;
  *plus = 0;
  int a = upos(sc_class_find(l.case_id + 4)), b = upos(sc_class_find(plus + 1));
  if (a < 0 || b < 0) return 0;
  char v[24] = "";
  xts_rline_str(&l, "verdict", v, sizeof v);
  int cs = xts_condtab_add(&gscan.ct, &l);
  if (cs < 0 || gscan.n >= UC_MAXE) { gscan.dropped++; return 0; }
  int rep = 0;
  for (int i = 0; i < gscan.n; i++)
    if (gscan.e[i].cs == cs && gscan.e[i].a == a && gscan.e[i].b == b) rep++;
  uc_edge_t *e = &gscan.e[gscan.n++];
  e->cs = cs; e->rep = rep; e->a = a; e->b = b;
  e->code = !strcmp(v, "separate") ? 'P' : !strcmp(v, "shared") ? 'S' : !strcmp(v, "partial") ? 'X' : '?';
  return 0;
}

/* the graph of condition vector `cs` (index into gscan.ct, -1: none seen) and repeat `rep` */
static void emit_graph(xts_ctx_t *ctx, int cs, int rep)
{
  char mat[MAXU][MAXU + 1];
  int parent[MAXU], missing = 0, total = 0;
  for (int i = 0; i < nucls; i++) {
    parent[i] = i;
    memset(mat[i], '-', nucls);
    mat[i][i] = '=';
    mat[i][nucls] = 0;
  }
  for (int i = 0; i < gscan.n; i++) {
    const uc_edge_t *e = &gscan.e[i];
    if (e->cs != cs || e->rep != rep) continue;
    mat[e->a][e->b] = mat[e->b][e->a] = e->code;
    if (e->code == 'S') {
      int ra = e->a, rb = e->b;
      while (parent[ra] != ra) ra = parent[ra];
      while (parent[rb] != rb) rb = parent[rb];
      if (ra != rb) parent[rb] = ra;
    }
  }
  for (int a = 0; a < nucls; a++)
    for (int b = a + 1; b < nucls; b++) {
      total++;
      if (mat[a][b] == '-') missing++;
    }
  static char buf[12000];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_kv_str(&j, "legend", "P separate pipes (throughputs add), S shared (serialise), X partial, ? undetermined, - missing");
  xj_key(&j, "classes");
  xj_arr(&j);
  for (int i = 0; i < nucls; i++) xj_str(&j, sc_classes[ucls[i]].name);
  xj_end(&j);
  xj_key(&j, "matrix");                      /* one string per class row, column order = classes */
  xj_arr(&j);
  for (int a = 0; a < nucls; a++) xj_str(&j, mat[a]);
  xj_end(&j);
  xj_key(&j, "shared_groups");
  xj_arr(&j);
  for (int r = 0; r < nucls; r++) {
    int root = r;
    while (parent[root] != root) root = parent[root];
    if (root != r) continue;
    xj_arr(&j);
    for (int i = 0; i < nucls; i++) {
      int ri = i;
      while (parent[ri] != ri) ri = parent[ri];
      if (ri == r) xj_str(&j, sc_classes[ucls[i]].name);
    }
    xj_end(&j);
  }
  xj_end(&j);
  xj_kv_int(&j, "pairs", total);
  xj_kv_int(&j, "missing", missing);
  if (ctx->host) xj_kv_str(&j, "why", "not executed (host)");
  xj_finish(&j);
  xts_record_t r;
  xts_record_init(&r);
  r.case_id = "graph";
  r.metric = "count";
  r.value = total - missing;
  r.unit = "count";
  r.status = ctx->host ? XTS_ST_SKIP : XTS_ST_INFO;
  r.detail = buf;
  r.detail_truncated = j.truncated;
  if (cs >= 0) xts_emit_for_cond(ctx, gscan.ct.text[cs], &r);   /* the record carries the vector it summarises */
  else xts_emit(ctx, &r);
}

static int uc_finish(xts_ctx_t *ctx)
{
  if (sc_classes_init() <= 0) return -1;
  build_ucls();
  memset(&gscan, 0, sizeof gscan);
  gscan.row = ctx->fin->row;
  xts_results_foreach(xts_family_name(ctx->fam), graph_cb, NULL);
  int groups = 0;
  for (int cs = 0; cs < gscan.ct.n; cs++) {
    int maxrep = -1;
    for (int i = 0; i < gscan.n; i++)
      if (gscan.e[i].cs == cs && gscan.e[i].rep > maxrep) maxrep = gscan.e[i].rep;
    for (int rep = 0; rep <= maxrep; rep++, groups++) emit_graph(ctx, cs, rep);
  }
  if (!groups) emit_graph(ctx, -1, 0);       /* nothing recorded for this row: every pair is missing */
  return 0;
}

static int uc_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (sc_classes_init() <= 0) return -1;
  if (!nucls) build_ucls();                   /* deterministic: same order as the enumeration */
  switch ((int)c->arg[0]) {
  case 0: get_alone(ctx, (int)c->arg[1], 1); return 0;
  case 1: return run_mix(ctx, (int)c->arg[1], (int)c->arg[2]);
  case 2: return run_busy(ctx, (int)c->arg[1], (int)c->arg[2]);
  }
  return -1;
}

XTS_FAMILY(unit_conflict, .kind = "sched", .axes = "sibling", .enumerate = uc_enum, .run = uc_run, .finish = uc_finish,
           .block_cases = 16,
           .desc = "interleaved independent streams: shared vs separate pipes, unpipelined busy time, pipe-class graph");
