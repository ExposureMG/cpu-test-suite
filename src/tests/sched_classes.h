/* XTS - scheduling-model test helpers (private to bypass.matrix, issue.pair, unit.conflict, ucode; owner: tsched).
 *
 * 1. Instruction CLASSES: a name, a representative JSON mnemonic, flag variant and operand overrides ("RA0=0",
 *    "BO=12,BI=30", "SPR=8"), resolved against the ISA table at first use. Nothing else is hard-wired: any instruction
 *    can be wrapped with sc_class_adhoc() and measured as a class representative.
 * 2. A generic OPERAND PLANNER (sc_plan): from the descriptor's operand classes/roles it decides, for every operand, what
 *    the kernel puts there (rotating destination bank, constant source, region pointer, zero index, immediate). It refuses
 *    (with a reason) anything it cannot run safely in a timing loop: privileged, illegal, string/multiple, CTR/LR branch
 *    targets, unknown SPRs.
 * 3. A UNIT description (sc_unit_t): up to 8 items, each "class X in register bank S, repeated n times", optionally
 *    linked (consumer operand := producer destination, or the same-file constant for the independent twin). One unit is
 *    one depth step of the timing engine.
 * 4. Measurement helpers on top of xts_time_measure: depth/iteration choice from a cost estimate, the framework's error bar
 *    (xts_tresult_t.err_cycles: slope standard error + MAD of the deepest depth) and a per-condition cache key. Records are
 *    written with the framework emitter (xts_time_emit_ex / _diff / _value); extra fits go into the detail with
 *    xts_time_fit_json. Summaries over a whole family (bypass distance sweeps, the unit.conflict graph) are built in the
 *    family's .finish hook from the family's own result lines (xts_rline_*, xts/rline.h), so they also work after a resume.
 *
 * Register map used by every kernel built here (r27-r29 frame, r1 r2 r13 r30 reserved, r0 unused):
 *   r3 -> ZERO region (GPR loads)   r4 -> STORE region   r5 -> PTR region (u64 pointers)   r6 -> FPVAL region (FP/VR)
 *   r7 = 0   r8 = CA 0x1234   r9 = CB 3   r31 = CC 0x55
 *   GPR banks: slot0 r10 r11 r12 r14 r15 r16, slot1 r17-r22, filler r23-r26 (update forms: RD = first half, RA = second)
 *   FPR: f0 1.0  f1 1.5  f2 0.75  f3 1.0;  banks slot0 f4-f15, slot1 f16-f27
 *   VR:  v0-v3 bank for VC128-forced links;  v4 identity permute, v5 1.0f, v6 1.5f, v7 0.75f, v8 ints, v9 0;
 *        legacy (5-bit) banks slot0 v12-v21, slot1 v22-v31;  VMX128 banks slot0 v64-v79, slot1 v80-v95
 *   CR:  slot0 cr2/cr3, slot1 cr4/cr5, cr7 = 0b0010 (EQ set) constant source;  LR = 0
 * Scratch (512 B, 128-aligned): [0,128) PTR  [128,256) ZERO  [256,384) STORE  [384,512) FPVAL (0x3F800000 words). */
#ifndef XTS_SCHED_CLASSES_H
#define XTS_SCHED_CLASSES_H
#include <xts/xts.h>

/* register files */
enum { SC_F_NONE = 0, SC_F_GPR, SC_F_FPR, SC_F_VR, SC_F_CR };
/* operand kinds chosen by the planner */
enum {
  SC_K_IMM = 0,     /* fixed value p->imm[i] */
  SC_K_DEST,        /* rotating register of the slot's bank */
  SC_K_SRC,         /* constant source (by ordinal) */
  SC_K_BASE,        /* memory base pointer (region by load/store kind) */
  SC_K_INDEX,       /* memory index register (= 0) */
  SC_K_DATA,        /* store data (constant source) */
  SC_K_UPD,         /* update-form base (rotating, second half of the GPR bank) */
  SC_K_TIED,        /* VMX128 tied operand: same register as the destination */
  SC_K_DISP,        /* displacement: rotating, 16-byte aligned, inside the region */
  SC_K_CRF_D, SC_K_CRF_S, SC_K_CRB_D, SC_K_CRB_S,   /* CR field / bit, destination (rotating) or source (cr7) */
  SC_K_CRM,         /* CR field mask of the slot (mtcrf / mtocrf / mfocrf) */
};

typedef struct sc_plan {
  int insn;
  unsigned flags;
  int nops;
  uint8_t kind[XTS_MAX_OPS], file[XTS_MAX_OPS], ord[XTS_MAX_OPS];   /* ord: source ordinal within its file */
  uint8_t maxreg[XTS_MAX_OPS];   /* highest encodable register for register operands (31, 127 or 7) */
  int64_t imm[XTS_MAX_OPS];
  int dest;                      /* operand index of the primary destination register, -1 */
  int dest_file;                 /* SC_F_* of dest (SC_F_CR for CRFD/CRBD destinations) */
  int is_load, is_store, is_update, is_branch;
  int region;                    /* base register for SC_K_BASE */
  int k;                         /* rotation length */
  const char *why;               /* non-NULL: cannot be timed generically */
} sc_plan_t;
/* plan `insn` with flag variant `flags` and overrides "NAME=v,NAME=v" (may be NULL). 0 or <0 (p->why says why). */
int sc_plan(int insn, unsigned flags, const char *ovr, sc_plan_t *p);

/* ---- classes ---- */
#define SC_U_PAIR   0x001   /* issue.pair matrix */
#define SC_U_UNIT   0x002   /* unit.conflict matrix */
#define SC_U_BP     0x004   /* bypass producer */
#define SC_U_BC     0x008   /* bypass consumer */
#define SC_PTR_OK   0x010   /* producer can deliver the consumer's base pointer (add r,base,0 / ld of a stored pointer) */
#define SC_IDX_OK   0x020   /* producer can deliver 0 (index operands) */
#define SC_INTVAL   0x040   /* producer result is an integer bit pattern (do not feed FP arithmetic: denormal) */
#define SC_FPARITH  0x080   /* consumer does floating-point arithmetic on the operand */
typedef struct sc_class {
  const char *name, *rep, *ovr, *desc;
  unsigned flags;            /* XTS_RC / XTS_OE variant of the representative */
  unsigned use;
  const char *slots;         /* bypass consumer: allowed operand names ("RA,RB"), NULL = all reads */
  int crimp;                 /* CR producer through an implicit field (record forms): field, else -1 */
  int crbit;                 /* bit (0-3) of the produced field that is 1 for the kernel's inputs */
  /* resolved */
  int insn;
  sc_plan_t plan;
} sc_class_t;
extern sc_class_t sc_classes[];
extern const int sc_nclasses;
int sc_classes_init(void);                 /* resolve (idempotent); number of usable classes */
int sc_class_find(const char *name);       /* index or -1 */
int sc_class_ok(int ci);                   /* resolved and planned */
/* ad-hoc class for any instruction (ucode, future families); returns 0 or <0 (c->plan.why) */
int sc_class_adhoc(sc_class_t *c, int insn, unsigned flags, const char *ovr);
double sc_class_est(const sc_class_t *c);  /* issue-cost estimate for depth/iteration choice (JSON issue, else 2) */

/* ---- units ---- */
#define SC_FILLER (-2)       /* item class: li rF,imm  (addi rF,0,imm; no register read), filler bank */
#define SC_MAX_ITEMS 8
enum { SC_LINK_NONE = 0, SC_LINK_DEP, SC_LINK_IND };
enum { SC_MODE_DATA = 0, SC_MODE_PTR, SC_MODE_IDX };
typedef struct sc_item {
  int cls;                   /* index into sc_classes, SC_FILLER, or -1 with `adhoc` set */
  const sc_class_t *adhoc;
  int slot;                  /* register bank 0 / 1 */
  int count;                 /* instances per unit */
  int link_from;             /* item index of the producer (count must be 1 on both), -1 */
  int link_op;               /* consumer operand index fed by the producer */
  int link;                  /* SC_LINK_DEP: producer's dest; SC_LINK_IND: the operand's usual constant */
  int mode;                  /* producer mode: SC_MODE_PTR (dest = consumer's base pointer) / SC_MODE_IDX (dest = 0) */
  int ptr_region;            /* PTR mode: which base register the producer must reproduce */
  int crsel;                 /* CR links: consumer reads field*4 + crsel */
} sc_item_t;
typedef struct sc_unit {
  int nitems;
  sc_item_t it[SC_MAX_ITEMS];
  /* special bodies (bypass mtctr/mtlr -> bctr/blr), when set the items are ignored */
  int special;               /* 0, 1 mtctr->bctr, 2 mtlr->blr */
  int special_dep;           /* 1 register branch, 0 control (b to the same word) */
  int special_dist;          /* fillers between the move and the branch */
  int base_word;             /* set by pre(): word index LR points at */
  int words;                 /* words per unit (computed) */
  int phase_pad;             /* 0..3 filler words in front of the body (and 4 - phase_pad after it, so the body keeps a
                                multiple of 4 words): with the line layout's loop top at byte 12, phase_pad 1 starts the
                                first unit on an even word, i.e. at byte 0 of the next fetch group (issue.parity) */
} sc_unit_t;
void sc_unit_init(sc_unit_t *u);
sc_item_t *sc_unit_add(sc_unit_t *u, int cls, int slot, int count);
int sc_unit_words(const sc_unit_t *u);
/* the setup used by every kernel here (constants, pointers, regions); usable as xts_tspec_t.setup with arg = unit */
int sc_setup(xts_exec_t *x, void *arg);

/* ---- measurement ---- */
typedef struct sc_meas {
  int ok;                    /* executed (console) */
  int status;                /* XTS_ST_* of the timing gate (SKIP on the host, FAIL if the kernel failed) */
  double cyc;                /* cycles per unit (NaN unless ok) */
  double err;                /* error bar of cyc in cycles (= r.err_cycles; NaN unless ok) */
  xts_tspec_t s;             /* the spec that was measured (for xts_time_emit_* / xts_time_fit_json) */
  xts_tresult_t r;
} sc_meas_t;
/* measure a unit; est = expected cycles per unit (for depths/iterations). Register-branch units through CTR use the GPR
 * loop frame (CTR belongs to the body). Returns 0, or <0 if the kernel could not be built/run (m->status = FAIL,
 * m->r.failed = 1: the emitters record the failure). */
int sc_measure(xts_ctx_t *ctx, sc_unit_t *u, double est, sc_meas_t *m);
/* build the framed kernel of one depth exactly as sc_measure would (host tests, dry runs) */
int sc_build(sc_unit_t *u, int depth, xts_kbuf_t *k);
/* per-condition cache key (applied axes + repeat) */
uint64_t sc_cond_key(const xts_ctx_t *ctx);

/* overlap classification helper: "yes"/"no"/"partial" for v in [0,1] with thresholds 0.8/0.2; NULL if v is NaN */
const char *sc_tri(double v, double hi, double lo, const char *yes, const char *no, const char *mid);
#endif
