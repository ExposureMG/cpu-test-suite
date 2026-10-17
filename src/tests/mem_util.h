/* mem_util.h - private helpers of the ldst.lat and mem.hier families (src/tests/ldst_lat.c, mem_hier.c).
 *
 *   forms    mu_form_of(insn, &f): what a load/store descriptor is (register file, width, update, X-form, byte-reversed,
 *            algebraic, reservation, Cell left/right, VMX128); mu_emit_mem() emits any of them generically.
 *   buffer   mu_big(need): the big buffer (xts_big), always requested at >= MU_BIG_BYTES so one allocation serves every
 *            case of both families (xts_big re-allocates, i.e. loses the contents, only when a larger size is asked for).
 *   chains   mu_chain_*: pointer cycles over "anchors" in the big buffer, written by us (not xts_prep) because the anchors
 *            carry per-form data values and need not be element aligned (misaligned / line- / page-crossing data). Every
 *            chain is walked back on the host AND on the console before it is used: a wrong pointer would be a wild load.
 *   timing   mu_measure() wraps xts_time_measure(); records go through xts_time_emit_ex/_diff/_value with our detail keys
 *            as .extra_json (mu_det_t); mu_emit_note() for records without a fit.
 *   levels   mu_levels(): pure pointer-chase latency at the L1 / L2 / memory working sets, measured once per boot (cached,
 *            per other_cores value) as the reference both families compare against.
 *   analysis mu_plateaus(): port of OpenXenos analysis_timing.plateaus()/curve() (levels and knees of a latency curve).
 */
#ifndef XTS_MEM_UTIL_H
#define XTS_MEM_UTIL_H
#include <math.h>
#include <stdint.h>
#include <xts/xts.h>

#define MU_KB 1024u
#define MU_MB (1024u * 1024u)
#define MU_BIG_BYTES (72u * MU_MB)     /* 64 MB chase + room; the TLB-overflow cases ask for more (mu_big(136 MB)) */
#define MU_LINE 128u
#define MU_L1_BYTES (32u * MU_KB)       /* 4-way, 64 sets, set stride 8 KB */
#define MU_L2_BYTES (1024u * MU_KB)     /* 8-way, 1024 sets, set stride 128 KB */
#define MU_PAGE (64u * MU_KB)           /* libxenon maps 64 KB pages (software TLB reload) */

/* working sets that define the levels (random cycle, one anchor per 128-byte line) */
#define MU_WS_L1 (16u * MU_KB)          /* half of L1D */
#define MU_WS_L2 (256u * MU_KB)         /* 8 x L1D, 1/4 of L2 */
#define MU_WS_MEM (3u * MU_MB)          /* 3 x L2, 48 pages: within the 64-entry ERAT, so no translation misses */

enum { MU_LV_L1 = 0, MU_LV_L2, MU_LV_MEM, MU_LV_COLD, MU_LV_N };
extern const char *const mu_level_name[MU_LV_N];   /* "l1" "l2" "mem" "cold" */

/* ---- forms ---- */
typedef struct mu_form {
  int insn;
  char file;        /* 'g' GPR, 'f' FPR, 'v' VR */
  int load;         /* 1 load, 0 store */
  int update;       /* RA receives the EA */
  int xform;        /* RB operand (else D/DS displacement) */
  int ds;           /* DS-form: displacement multiple of 4 */
  int width;        /* bytes accessed (vector element loads: element size; lvlx/lvrx: 16) */
  int sign;         /* lha / lwa: sign extension */
  int brev;         /* byte-reversed */
  int resv;         /* lwarx/ldarx/stwcx./stdcx. */
  int lr;           /* 1 lvlx/stvlx family, 2 lvrx/stvrx family */
  int elem;         /* lvebx/lvehx/lvewx(128), stve*x */
  int vr128;        /* VMX128 register field */
  int fpsingle;     /* lfs/stfs family (single format in memory) */
  int align_trap;   /* EA must be a multiple of this or the access takes an alignment interrupt (results4); 0 = never */
} mu_form_t;
/* 0 and *f filled for a timable load/store; <0 for everything else (cache ops, string/multiple, eciwx, dst, lvsl...) */
int mu_form_of(int insn, mu_form_t *f);
/* load/store with register `reg`, base `base` (RA/RA0) and either RB = `idx` (X-form) or displacement `idx` (D/DS) */
int mu_emit_mem(xts_kbuf_t *k, int insn, int reg, int base, int64_t idx);
/* bytes a load of form f reads as value v (big-endian, byte-reversed for brx; FP: v is the bit pattern) */
void mu_put_value(uint8_t *p, const mu_form_t *f, uint64_t v);

/* ---- latency-chain geometry (ldst.lat) ----
 * Element layout for a load form whose data starts `doff` bytes into its element: the chased pointer targets the anchor
 * (element + a), the form's EA is anchor + di, and the step adds V (GPR: the loaded value; FPR/VR: a power of two made
 * from a CR bit) to reach the next pointer at anchor + ptr_rel. page: 0, 4 (12 KB elements, 4 KB crossing) or 64 (192 KB
 * elements, 64 KB crossing). <0: no geometry (an FP update form with a non-doubleword offset, a value out of range). */
typedef struct mu_geom {
  uint32_t a;           /* anchor offset inside the element */
  int32_t di;           /* displacement / index of the form's EA from the anchor */
  int32_t V;            /* value the chain step adds to the anchor (GPR: the loaded value) */
  int32_t ptr_rel, data_rel, data_w;
  uint32_t stride;
} mu_geom_t;
int mu_geom_for(const mu_form_t *f, uint32_t doff, int page, mu_geom_t *g);
uint32_t mu_pow2(uint32_t v);          /* smallest power of two >= v, at least 8 */

/* ---- big buffer ---- */
typedef struct mu_buf { uint8_t *p; uint64_t addr; uint32_t size; } mu_buf_t;
int mu_big(uint32_t need, mu_buf_t *b);      /* 0 or <0 (allocation failed) */

/* ---- pointer chains ----
 * anchor i is at  off(i) = base + i*stride + (i % jmod)*jstep + (i / bdiv)*bstep + anchor  (jmod/bdiv 0 = none)
 * the 8-byte big-endian pointer at off(order[j]) + ptr_rel holds the ADDRESS of anchor order[j+1] (cyclic).
 * data_w bytes at off(i) + data_rel hold `data` encoded for form `df` (data_w 0 = none). */
typedef struct mu_chain {
  uint32_t base, n, stride, jmod, jstep, bdiv, bstep, anchor;
  int32_t ptr_rel, data_rel;
  int data_w;
  mu_form_t df;
  uint64_t data;
  int random;              /* 1 random cyclic order (seeded), 0 sequential */
  uint32_t seed;
  uint32_t *order;         /* n entries (malloc'd by mu_chain_build, kept for reuse until the next build) */
  uint32_t span;           /* bytes from base to the end of the last anchor's data/pointer */
  mu_buf_t buf;
} mu_chain_t;
static inline uint32_t mu_chain_off(const mu_chain_t *c, uint32_t i)
{
  return c->base + i * c->stride + (c->jmod ? (i % c->jmod) * c->jstep : 0) + (c->bdiv ? (i / c->bdiv) * c->bstep : 0) +
         c->anchor;
}
/* write the chain (needs c->buf); 0 or <0 (bad geometry, out of buffer, overlapping fields, verification failed) */
int mu_chain_build(mu_chain_t *c);
/* walk the chain from anchor order[0]: 0 if it visits all n anchors and returns (pure, host-tested) */
int mu_chain_verify(const mu_chain_t *c);
/* address of the anchor at cycle position pos (mod n) */
uint64_t mu_chain_addr_at(const mu_chain_t *c, uint32_t pos);
/* cache maintenance over the chain's span: dcbf (cold) */
void mu_chain_flush(const mu_chain_t *c);

/* ---- measurement and records ----
 * Records are written with xts_time_emit_ex / _diff / _value (xts/timing.h). Our own detail members (level, size, stride,
 * width, offset, ...) are built in a mu_det_t and handed over as `.extra_json = mu_det_end(&d)`; the emitter adds the
 * unified "fit" (and "ref" for differences), the baseline / implausible keys and the status. Every record of these
 * families sets .no_negative = 1 (a negative cost is noisy); throughput records add .floor = 1.0 (one access per cycle). */
typedef struct mu_det { char buf[1800]; xts_json_t j; } mu_det_t;
void mu_det_begin(mu_det_t *d);                       /* xj_obj started: add keys with xj_kv_* on &d->j */
static inline const char *mu_det_end(mu_det_t *d) { return xj_finish(&d->j); }   /* the members, for .extra_json */
/* run the series; returns cycles per unit (NaN on the host / after a trap / on failure; r->failed + status fail if the
 * kernel could not be built or run) */
double mu_measure(xts_ctx_t *ctx, const xts_tspec_t *s, xts_tresult_t *r);
/* a record without a measurement of its own (skip / failure / analysis summary): xts_time_emit_value with no fit.
 * `case_id` NULL = the case's; `status` is combined with the negative-value check. Unit NULL -> "cycles". */
void mu_emit_note(xts_ctx_t *ctx, mu_det_t *d, const char *metric, int insn, double value, const char *unit, int status,
                  const char *case_id);

/* ---- reference levels ---- */
typedef struct mu_levels {
  int valid;
  double lat[3];           /* l1 l2 mem cycles (NaN if not measured / host) */
  int status[3];
} mu_levels_t;
const mu_levels_t *mu_levels(xts_ctx_t *ctx);

/* pure chase kernels shared by both families: arg = mu_chase_t */
typedef struct mu_chase {
  mu_chain_t *c;
  int warm;                /* pre(): walk the whole cycle once before the timed region */
  int flush;               /* setup(): dcbf the span before every run (cold) */
  uint32_t rotate;         /* setup(): start position advances by this many anchors per run (0 = always order[0]) */
  uint32_t runs;           /* runs so far (setup calls) */
} mu_chase_t;
int mu_chase_body(xts_kbuf_t *k, int depth, void *arg);      /* ld r3,ptr_rel(r3) x depth */
int mu_chase_pre(xts_kbuf_t *k, int depth, void *arg);       /* warm walk with r20/r21 + CTR */
int mu_chase_setup(xts_exec_t *x, void *arg);                /* r3 = start anchor */
/* measure a pure chase over chain c: cycles per load */
double mu_chase_measure(xts_ctx_t *ctx, mu_chain_t *c, int warm, int flush, uint32_t rotate, xts_tspec_t *s,
                        xts_tresult_t *r);

/* ---- latency curves (port of analysis_timing.plateaus/curve) ---- */
#define MU_PLATEAU_SPREAD 1.2
#define MU_LEVEL_FACTOR 1.5
typedef struct mu_plateau { int lo, hi; double level; } mu_plateau_t;   /* point indices lo..hi (inclusive) */
/* x ascending; y may contain NaN (skipped). Returns the number of plateaus (<= maxp). */
int mu_plateaus(const double *x, const double *y, int n, mu_plateau_t *p, int maxp);
/* nearest of `nlev` levels in log space, -1 if in the middle third between two (a partial miss rate) */
int mu_classify(double v, const double *lev, int nlev);
#endif
