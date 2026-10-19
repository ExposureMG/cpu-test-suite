/* XTS - timing engine (src/timing), "method v2".
 *
 * A measurement runs a kernel family at several DEPTHS (how many copies of the unit under test the body holds) inside a
 * CTR loop framed by timebase reads:
 *
 *     mtctr rI ; isync ; mftb rT0 ; top: <body(depth)> ; bdnz top ; mftb rT1 ; exit
 *
 * ticks/iteration is fitted against depth by least squares: slope = ticks per unit (x cycles_per_tick = cycles per unit),
 * intercept = loop overhead. One unrecorded warm-up run of the first depth, then each depth `repeats` times in a shuffled
 * order; the first depth is measured XTS_ABA_RUNS more times at the end and A/B/A compares the minimum of those with the
 * minimum of its repeats (regime shifts move the minimum, one-off events such as an interrupt only add time). Quality gate: pass if the relative spread (MAD/median) of every depth
 * and the A/B/A shift are < 2 % and the largest fit residual is < 1 % of its point. A residual above that is
 * `nonlinear` when the points are repeatable - spread and A/B/A shift in limits and the largest absolute residual at
 * least XTS_NONLINEAR_FACTOR times the largest per-depth MAD (floored at one tick over the run, 1/iters): the cost per
 * unit then really depends on the depth (front end, code alignment), the slope is the least-squares average and
 * fit.marginal_cycles gives the cost per depth step. Otherwise noisy.
 *
 *   static int body(xts_kbuf_t *k, int depth, void *arg) {          // emit `depth` dependent adds
 *     for (int i = 0; i < depth; i++) xk_emit(k, XI_addx, 0, 3, 3, 4);
 *     return 0;
 *   }
 *   xts_tspec_t s = XTS_TSPEC_DEFAULT;  s.body = body;  s.unit = "add";
 *   xts_tresult_t r;
 *   xts_time_measure(&s, ctx, &r);                                    // fit, gate
 *   xts_time_emit(ctx, &s, &r, "latency", XI_addx);                   // record with value = cycles per unit
 *
 * v2 additions (see CONTRIBUTING.md "Timing records"):
 *   - spec.loop = XTS_LOOP_GPR: r29 counts down with addi/cmpwi/bne on CR field spec.loop_crf (CTR is free for the body);
 *     both loop modes close with a long-reach form (bdz/beq over a `b top`) when the body exceeds the bc range.
 *   - spec.setup2(x, depth, run, arg): case input per depth and run index (replaces setup when set).
 *   - xts_time_emit_ex(ctx, &s, &r, insn, &(xts_temit_t){ .metric = "penalty", .per_unit = 2, ... }): the ONE record
 *     emitter (scaling, value override, baseline key, extra detail, case-id suffix, word, unit, implausible downgrade);
 *     xts_time_emit_diff() for a - b records. Detail: {<extra members>, "fit":{...}[, "ref":{...}], ...} (schema below).
 */
#ifndef XTS_TIMING_H
#define XTS_TIMING_H
#include <stdint.h>

struct xts_kbuf;
struct xts_exec;
struct xts_ctx;
struct xts_json;

#define XTS_TMAX_DEPTHS 16
#define XTS_TMAX_REPEATS 32

extern double xts_cycles_per_tick;   /* core cycles per timebase tick; default 64 (calib family measures and sets it) */

/* ---- fit maths (pure, host-tested) ---- */
typedef struct xts_fit {
  double slope, intercept, r2;
  double max_resid;          /* largest |residual| */
  double max_resid_rel;      /* largest |residual| / |y| */
  int n;
  double slope_se;           /* standard error of the slope: sqrt(SSR / (n-2) / Sxx); 0 when n <= 2 */
} xts_fit_t;
int xts_fit_line(const double *x, const double *y, int n, xts_fit_t *f);    /* <0 if n < 2 or x degenerate */
double xts_median(const double *v, int n);                                   /* does not modify v */
double xts_mad(const double *v, int n);                                      /* median absolute deviation */
double xts_min(const double *v, int n);
/* deterministic shuffle of idx[0..n-1] */
void xts_shuffle(int *idx, int n, uint64_t seed);

/* ---- measurement spec ----
 * Frame, loop = XTS_LOOP_CTR (default; byte-identical to v1):
 *     [prio] [pre] mtctr r29 ; isync ; mftb r27 ; <align> top: <body> ; bdnz top ; mftb r28 ; exit
 * loop = XTS_LOOP_GPR (CTR belongs to the body; CR field `loop_crf` belongs to the frame):
 *     [prio] [pre] isync ; mftb r27 ; <align> top: <body> ; addi r29,r29,-1 ; cmpwi crF,r29,0 ; bne crF,top ; mftb r28 ; exit
 * Long reach (body + closure beyond the 32 KB bc range): bdnz top -> "bdz out ; b top ; out:", bne -> "beq out ; b top".
 * The kernel is placed at kbuf + (xts_env.code_offset & 124); `align` is relative to the kernel start. */
typedef struct xts_tspec {
  const char *unit;                  /* what one depth step is ("addx", "add+filler") - for records */
  /* emit the loop BODY for `depth` units (no frame, no exit). Registers r29 (iterations), r27/r28 (timebase) and the
   * reserved r1 r2 r13 r30 are taken; everything else is the body's (CTR too with XTS_LOOP_GPR, except cr<loop_crf>). */
  int (*body)(struct xts_kbuf *k, int depth, void *arg);
  /* emit code before the timed region (register setup that should not be timed), optional */
  int (*pre)(struct xts_kbuf *k, int depth, void *arg);
  /* case input state (registers the body reads, pointers), optional; called after xts_case_init */
  int (*setup)(struct xts_exec *x, void *arg);
  void *arg;
  int depths[XTS_TMAX_DEPTHS];
  int ndepths;                       /* >= 5 */
  int repeats;                       /* >= 4 */
  uint32_t iters;                    /* loop count (r29) */
  uint32_t align;                    /* align the loop top to this many bytes (0 = 32) */
  double spread_limit, resid_limit;  /* quality gate (defaults 0.02, 0.01) */
  /* ---- v2 (all optional; zero = v1 behaviour) ---- */
  int loop;                          /* XTS_LOOP_CTR (0) or XTS_LOOP_GPR (xts/kernel.h) */
  int loop_crf;                      /* XTS_LOOP_GPR: CR field of the loop compare, 1..7; 0 = cr7 */
  /* like setup, with the depth being measured and the run index (0 .. ndepths*repeats; the warm-up run uses 0, the
   * A/B/A re-runs of depths[0] use ndepths*repeats; on the host the run index is the depth index). When set, it is
   * called instead of setup. */
  int (*setup2)(struct xts_exec *x, int depth, int run, void *arg);
  /* ---- v3 ---- */
  int top_offset;                    /* bytes of nops after the `align` padding (multiple of 4): the loop top lands at
                                        align*k + top_offset. frontend.loop (2026-10-19): with the top at byte 12 of a
                                        16-byte fetch group and bodies a multiple of 4 words, the per-iteration loop
                                        cost is smooth in the body length; with the top at 0 it jumps 4-11 cycles. */
} xts_tspec_t;
#define XTS_TSPEC_DEFAULT { .unit = "", .depths = { 4, 8, 12, 16, 24, 32 }, .ndepths = 6, .repeats = 5, .iters = 256, \
                            .align = 32, .spread_limit = 0.02, .resid_limit = 0.01 }

typedef struct xts_tresult {
  int ok;                            /* 1 if every run executed (0 on the host or after a trap) */
  int status;                        /* XTS_ST_PASS, XTS_ST_NONLINEAR, XTS_ST_NOISY, XTS_ST_TRAP, XTS_ST_SKIP (host) */
  xts_fit_t fit;                     /* y = ticks per iteration, x = depth */
  double cycles_per_unit;            /* fit.slope * xts_cycles_per_tick */
  double overhead_cycles;            /* fit.intercept * xts_cycles_per_tick */
  double median[XTS_TMAX_DEPTHS];    /* ticks per iteration, median over repeats */
  double mad[XTS_TMAX_DEPTHS];
  double minv[XTS_TMAX_DEPTHS];
  double spread_max;                 /* max over depths of mad/median */
  double aba_shift;                  /* relative change of depths[0]: min of the A/B/A re-runs vs min of its repeats */
  int nruns;
  int kernel_words_max;
  char why[96];
  /* ---- v2 ---- */
  double slope_se;                   /* = fit.slope_se (ticks per unit) */
  double err_cycles;                 /* error bar per unit in cycles: cycles_per_tick * sqrt(slope_se^2 + (mad[deep]/depth[deep])^2),
                                        at least 0.01 (the sched_classes method); NaN when !ok */
  int failed;                        /* 1 if xts_time_measure returned <0 (kernel build / setup / exec error; why says) */
  /* ---- v3 ---- */
  double resid_over_noise;           /* largest |residual| / max(largest per-depth MAD, 1/iters) */
  int outliers;                      /* repeats more than spread_limit above their depth's median (diagnostics) */
} xts_tresult_t;
#define XTS_NONLINEAR_FACTOR 5.0
#define XTS_ABA_RUNS 3

/* Run the whole series. Returns 0 (result in *r, including noisy / nonlinear) or <0 if kernels could not be built. */
int xts_time_measure(const xts_tspec_t *s, struct xts_ctx *ctx, xts_tresult_t *r);
/* Same-shape differential: measure `a` and `filler` (identical layout/length, filler replaces the unit by a known
 * 1-cycle independent instruction) and report a - filler per unit in *diff_cycles. */
int xts_time_differential(const xts_tspec_t *a, const xts_tspec_t *filler, struct xts_ctx *ctx, xts_tresult_t *ra,
                          xts_tresult_t *rf, double *diff_cycles);
/* Build (and on the host validate) the framed kernel for one depth, without running it. For dry runs and tests. */
int xts_time_build(const xts_tspec_t *s, int depth, struct xts_kbuf *k);
/* Apply the quality gate to a filled result (pure; used by xts_time_measure and the host tests). */
void xts_time_gate(const xts_tspec_t *s, xts_tresult_t *r);
/* Soft baseline from the ISA table (JSON timing): returns XTS_ST_REGRESSED if `value` differs from the baseline `key`
 * of `insn` by more than rel_tol (e.g. 0.1), XTS_ST_PASS if within, -1 if there is no baseline. */
int xts_time_baseline_check(int insn, const char *key, double value, double rel_tol, double *baseline);
/* Emit one record: metric, value = cycles per unit, n/min/median/mad from the deepest depth, fit_r2, status.
 * v1 call, kept: same as xts_time_emit_ex(ctx, s, r, insn, &(xts_temit_t){ .metric = metric, .baseline_key = metric }). */
void xts_time_emit(struct xts_ctx *ctx, const xts_tspec_t *s, const xts_tresult_t *r, const char *metric, int insn);

/* ---- the record emitter (v2) ----------------------------------------------------------------------------------------
 * value   = cycles_per_unit / per_unit                    (default), or e->value when e->has_value (XTS_TE_VALUE(v))
 * min/median/mad = per-unit cycle statistics of the deepest depth, divided by per_unit, ONLY when value is that per-unit
 *           cycle figure (default, or has_value with value == cycles_per_unit / per_unit); null otherwise
 * n = r->nruns, fit_r2 = r->fit.r2 (when r->ok)
 * status  = r->status (fail if r->failed), then the baseline check (only from pass: regressed if off by > baseline_tol),
 *           then the implausible-value downgrade (pass -> noisy), then xts_status_worst(status, e->status)
 * mnem    = xts_isa[insn].mnem (insn >= 0), else the case's mnemonic;  word = e->word (0 = none)
 * case_id = e->case_id, else "<case id>/<case_suffix>" when case_suffix is set, else the case id
 * detail  = { <members of e->extra_json>, <members written by e->detail_fn>, "fit":{fit schema},
 *             ["baseline":b, "baseline_key":k], ["implausible":why, "floor":f], ["error":why] }
 * fit schema (xts_time_fit_json; every key always present, numbers null when not measured):
 *   {"unit":<spec unit text>, "iters":n, "status":"pass", ["why":".."], "cpu":<cycles per spec unit = slope_ticks *
 *    cycles_per_tick, NOT divided by per_unit>, "slope_ticks", "intercept_ticks", "r2", "max_resid_rel", "spread",
 *    "aba_shift", "slope_se", "resid_over_noise", "outliers", "depths":[..], "ticks_med":[..], "ticks_mad":[..],
 *    "marginal_cycles":[..], "cycles_per_tick", "per_unit", "kernel_words_max"}
 * ticks_* are ticks per loop iteration per depth (median / MAD over the repeats); marginal_cycles[i] is the cost of the
 * step depths[i] -> depths[i+1] in cycles per unit, divided by per_unit like the record value. */
typedef struct xts_temit {
  const char *metric;        /* required; metric vocabulary (xts/record.h) */
  const char *unit;          /* NULL -> "cycles" */
  double per_unit;           /* events per spec unit (instructions per unit, bytes per unit ...): value = cycles/unit / per_unit; 0 -> 1 */
  int has_value;             /* 1: the record's value is `value` (a penalty, difference, ratio ...), not the scaled slope */
  double value;
  const char *baseline_key;  /* ISA JSON timing key to compare with (pass -> regressed); NULL = no baseline check;
                                "*" = from the metric: latency -> "latency", rthroughput/issue_cost -> "issue", else the metric */
  double baseline_tol;       /* relative tolerance, 0 -> 0.10 */
  uint32_t word;             /* instruction word of the record (0 = none) */
  const char *case_id;       /* full case id override (NULL = the case's) */
  const char *case_suffix;   /* case id = "<case id>/<suffix>" (ignored when case_id is set) */
  const char *extra_json;    /* JSON object text whose members are copied to the front of the detail ("{\"producer\":\"x\"}") */
  void (*detail_fn)(struct xts_json *j, void *arg);   /* writes extra members into the open detail object */
  void *detail_arg;
  double floor;              /* > 0: a pass value below floor * 0.97 becomes noisy ("implausible": below the floor) */
  int no_negative;           /* 1: a pass value below -0.5 becomes noisy ("implausible": negative) */
  int status;                /* combined with xts_status_worst (0 = pass = no effect; XTS_ST_INFO marks info records) */
} xts_temit_t;
#define XTS_TE_VALUE(v) .has_value = 1, .value = (v)
void xts_time_emit_ex(struct xts_ctx *ctx, const xts_tspec_t *s, const xts_tresult_t *r, int insn, const xts_temit_t *e);
/* Differential record: value = (cycles_per_unit(a) - cycles_per_unit(b)) / per_unit (or e->value if has_value);
 * min/median/mad null; n = a.nruns + b.nruns; fit_r2 = min of both; status = worst of both (then baseline/floor/e->status
 * as above); detail has "fit" (a) and "ref" (b). sb may equal sa when both used the same spec layout. */
void xts_time_emit_diff(struct xts_ctx *ctx, const xts_tspec_t *sa, const xts_tresult_t *ra, const xts_tspec_t *sb,
                        const xts_tresult_t *rb, int insn, const xts_temit_t *e);
/* A record with a family-computed value but no fit of its own (value-only), same detail/status rules; s/r may be NULL
 * (then no "fit" member, status = e->status). If s/r are given, the fit goes into "fit" and its status is combined. */
void xts_time_emit_value(struct xts_ctx *ctx, const xts_tspec_t *s, const xts_tresult_t *r, int insn, double value,
                         const xts_temit_t *e);
/* Write the fit schema object as member `key` of an open object (for families adding extra fits in detail_fn). */
void xts_time_fit_json(struct xts_json *j, const char *key, const xts_tspec_t *s, const xts_tresult_t *r, double per_unit);
/* cycles per unit / per_unit, or NaN when the result is not usable (!ok, failed) */
double xts_time_cycles(const xts_tresult_t *r, double per_unit);
/* resolved baseline key for a metric ("*" rule above) */
const char *xts_time_baseline_key(const char *metric);
#endif
