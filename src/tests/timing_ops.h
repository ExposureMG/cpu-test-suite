/* Private helpers of the tlat timing families (lat.chain, tput.indep, lat.data). Not a public interface: every exported
 * symbol is prefixed tlat_ so it cannot collide with other families' helpers.
 *
 * Model: a kernel body is `depth` UNITS; a unit is `per_unit` instruction words taken cyclically from a PATTERN of up to
 * TLAT_MAXPAT pre-encoded words (pattern length must divide depth*per_unit for every depth, so the register rotation is
 * seamless across loop iterations). Cycles per instruction = timing-engine cycles per unit / per_unit.
 *
 *   chain     pattern 1 word  (dest == chained source)                         per_unit 1
 *   rotation  pattern 2 words (write r[(i+1)%2], chained source r[i%2]): isolates one source slot of an instruction
 *             whose destination is also read (VMX128 tied VD/VS, rlwimi/rldimi)            per_unit 1
 *   acc       pattern 1 word  (tied destination is the only chained operand)                per_unit 1
 *   nchains   pattern N words (N independent chains)                                         per_unit N
 *   indep     pattern P words (distinct destinations, fixed source registers)               per_unit P
 *   filler    same length, `addi r,K,1` into rotating destinations: a 1-cycle independent instruction
 */
#ifndef XTS_TESTS_TIMING_OPS_H
#define XTS_TESTS_TIMING_OPS_H
#include <xts/xts.h>

/* register classes of the chain */
enum { TLAT_NONE = 0, TLAT_GPR, TLAT_FPR, TLAT_VR, TLAT_CRB, TLAT_CRF };

#define TLAT_MAXSRC 4
#define TLAT_SLOT_ACC 100          /* slot code: the tied (destination-is-read) accumulator */
#define TLAT_MAXPAT 32
#define TLAT_MAXSET 48

/* What an instruction looks like to the chain builder. */
typedef struct tlat_shape {
  int insn;
  int cls;                       /* TLAT_* of the destination */
  int dest;                      /* operand index of the destination */
  int tied;                      /* destination is also read (VMX128 tied, rlwimi/rldimi) */
  int tied_alias;                /* operand index of the VS128 alias of the destination, -1 if none */
  int nsrc;                      /* same-class register sources (not the tied alias) */
  int src[TLAT_MAXSRC];          /* their operand indices, asm order */
  int vr128;                     /* uses 7-bit VMX128 register fields */
} tlat_shape_t;

/* 0 if the instruction is timable by a same-class register chain; <0 otherwise (*why set to a static reason). */
int tlat_shape(int insn, tlat_shape_t *s, const char **why);
const char *tlat_class_name(int cls);
/* slot name for case ids / details: operand name or "acc" */
const char *tlat_slot_name(const tlat_shape_t *s, int slot);
/* "addx", "addx.rc", "addx.oe", "addx.oe.rc" */
void tlat_variant_name(int insn, unsigned flags, char *buf, int size);
/* flag variants (subsets of RC|OE the instruction accepts), at most 4; returns the count */
int tlat_variants(int insn, unsigned *out);

/* ---- register values ---- */
typedef struct tlat_val { uint64_t u; xts_vr_t v; } tlat_val_t;   /* u: GPR/FPR bits or CR bit value; v: vector */
tlat_val_t tlat_gv(uint64_t u);                  /* GPR/FPR raw bits */
tlat_val_t tlat_dv(double d);                    /* FPR double */
tlat_val_t tlat_fv(uint32_t fbits);              /* vector splat of single bits */
tlat_val_t tlat_vv(xts_vr_t v);
#define TLAT_F32_ONE 0x3F800000u
#define TLAT_F32_075 0x3F400000u

/* ---- kernel pattern + register setup ---- */
typedef struct tlat_word { uint16_t insn; uint8_t nops; uint8_t flags; int64_t ops[XTS_MAX_OPS]; } tlat_word_t;
typedef struct tlat_set { uint8_t cls; uint8_t reg; tlat_val_t val; } tlat_set_t;

typedef struct tlat_kern {
  int npat, per_unit;
  tlat_word_t pat[TLAT_MAXPAT];
  int nset;
  tlat_set_t set[TLAT_MAXSET];
  uint32_t cr, xer, fpscr, vscr;  /* applied by setup (fpscr/vscr forced: timing runs with exceptions off) */
  const char *shape;              /* "chain", "rotation", "acc", "nchains", "indep", "filler" */
  int op_reg[XTS_MAX_OPS];        /* register of every register operand of pattern word 0 (-1: immediate) */
  int chain_reg[2];               /* chained register(s) (rotation: both), -1 if none */
  int any_len;                    /* pattern has no loop-carried reads (filler): any body length is fine */
  int err;
  char errmsg[96];
} tlat_kern_t;

void tlat_kern_init(tlat_kern_t *k, const char *shape, int per_unit);
/* add one pattern word (operands in descriptor order); returns its index or -1 (k->err set) */
int tlat_kern_word(tlat_kern_t *k, int insn, unsigned flags, const int64_t *ops);
void tlat_kern_set(tlat_kern_t *k, int cls, int reg, tlat_val_t v);

/* ---- operand assignment ---- */
/* Fill ops[] for `s` with: destination `dreg`, source slot i -> sreg[i] (i < s->nsrc), tied alias = dreg, immediates
 * set to a neutral in-range value (1, clamped). Returns 0 or <0. */
int tlat_assign(const tlat_shape_t *s, int dreg, const int *sreg, int64_t *ops);

/* register pools (deterministic). GPR pool avoids r0, r1, r2, r13, r27-r30. VR pools: legacy v0-v31; VMX128 v64-v127
 * (exercises the high register-number bits); VC128 (vperm128) v0-v7. */
int tlat_pool(int cls, int vr128, int i);         /* i-th register of the pool, -1 if out of range */
int tlat_pool_size(int cls, int vr128);
int tlat_is_vrc(const tlat_shape_t *s, int opidx);   /* operand restricted to v0-v7 */

/* ---- neutral chain data (lat.chain, tput.indep) ---- */
/* value of the chained register(s) and of every other source operand (by operand index) */
tlat_val_t tlat_neutral_chain(const tlat_shape_t *s, int slot);
tlat_val_t tlat_neutral_other(const tlat_shape_t *s, int slot, int opidx);
const char *tlat_neutral_note(const tlat_shape_t *s, int slot);   /* NULL or a note on the data (inf/NaN, zeros) */

/* ---- builders ---- */
/* one dependent chain through source slot `slot` (index into s->src, or TLAT_SLOT_ACC) */
int tlat_build_chain(const tlat_shape_t *s, unsigned flags, int slot, tlat_kern_t *k);
/* n independent chains (each through slot 0, or the accumulator for tied instructions); <0 if the pool is too small */
int tlat_build_nchains(const tlat_shape_t *s, unsigned flags, int n, tlat_kern_t *k);
/* filler of the same shape: total pattern length `npat` words, per_unit as given */
int tlat_build_filler(int npat, int per_unit, tlat_kern_t *k);

/* ---- measuring ---- */
#define TLAT_NDEPTHS 6
extern const int tlat_depths[TLAT_NDEPTHS];      /* 8 16 24 32 48 64: even, >= 32 at the top */
/* timing spec for kernel k with an estimate of cycles per unit (picks iters so the shallowest depth runs ~2^18 cycles).
 * Honours the unroll axis (xts_cond_unroll); depths are rounded up where a rotation needs whole pattern repeats. */
void tlat_spec(xts_tspec_t *sp, tlat_kern_t *k, double est_cycles_per_unit, const char *unit);
/* emit the body / set the input state (used as tspec callbacks; arg = tlat_kern_t *) */
int tlat_body(xts_kbuf_t *kb, int depth, void *arg);
int tlat_setup(xts_exec_t *x, void *arg);
/* first word of the pattern, encoded (record "word") */
uint32_t tlat_word0(const tlat_kern_t *k);
/* cycles-per-unit estimate from the JSON baseline (latency, else issue, else `dflt`) */
double tlat_est_latency(int insn, double dflt);

/* ---- output ----
 * Records are emitted with xts_time_emit_ex (xts/timing.h): .per_unit = k->per_unit, .word = tlat_word0(k); cycles per
 * instruction of a result = xts_time_cycles(&r, k->per_unit) (NaN if not executed). */
/* the vscr_nj axis applied to this block? (then *nj = its value) */
int tlat_axis_nj(const xts_ctx_t *ctx, int *nj);
/* relative "is above the filler" test used for front-end discrimination */
int tlat_discriminates(double op_cycles, double filler_cycles);

/* ---- FP operation families (lat.data bins, chain identities) ---- */
enum { TLAT_FAM_NONE = 0, TLAT_FAM_ADD, TLAT_FAM_MUL, TLAT_FAM_DIV, TLAT_FAM_FMA, TLAT_FAM_SQRT, TLAT_FAM_EST,
       TLAT_FAM_F2I, TLAT_FAM_I2F, TLAT_FAM_ROUND, TLAT_FAM_MOVE, TLAT_FAM_SEL, TLAT_FAM_MINMAX, TLAT_FAM_CMP,
       TLAT_FAM_DOT };
typedef struct tlat_fpinfo {
  const char *mnem;
  uint8_t fam;
  uint8_t single;      /* single-precision result (scalar *s ops and every vector op) */
  uint8_t sub;         /* the addend is subtracted (fsub, fmsub, fnmsub, vnmsubfp, ...) */
  uint8_t mul_ops;     /* bitmask over operand indices that are multiplicands (FMA/MUL/DIV dividend) */
} tlat_fpinfo_t;
const tlat_fpinfo_t *tlat_fpinfo(int insn);      /* NULL for non-FP instructions */
const char *tlat_fam_name(int fam);

/* lat.data (lat_data.c), for the host tests: build the kernel a case measures. chain 0: indep shape, 1: identity chain;
 * normal: the class's normal bin instead of the case's. Returns 1 built, 0 no such kernel, <0 error. */
int tlat_ld_kernel(const xts_case_t *c, int chain, int normal, int nj, tlat_kern_t *k);
#endif
