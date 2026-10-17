/* XTS - condition axes and the execution environment they set.
 *
 * An axis is a named machine/test condition with a list of named values, e.g. sf = 64|32. The scheduler applies one value
 * of every axis that both the profile row and the family select, for a whole block of cases, then reverts it. Every record
 * carries the value of EVERY registered axis (values[0], the default, for axes not applied).
 *
 * Adding an axis = adding src/conds/<axis>.c:
 *
 *   static const char *const fpscr_rn_values[] = { "rn", "rz", "rp", "rm", NULL };     // values[0] = default/idle
 *   static int  rn_apply(int v)  { xts_env.fpscr_set = (xts_env.fpscr_set & ~3u) | v; xts_env.fpscr_clr |= 3; return 0; }
 *   static void rn_revert(int v) { (void)v; xts_env.fpscr_set &= ~3u; xts_env.fpscr_clr &= ~3u; }
 *   XTS_AXIS(fpscr_rn, .values = fpscr_rn_values, .apply = rn_apply, .revert = rn_revert, .order = 10,
 *            .desc = "FPSCR[RN] rounding mode of the case input state");
 *
 * apply() returns 0, or <0 if the value cannot be applied on this machine (the block is then recorded as skip).
 * Axes that change the machine itself (sibling thread load, CPU speed) do the work in apply/revert; axes that change how
 * cases are built set fields of `xts_env`, which the exec engine (xts_case_init) and the kernel builder read.
 */
#ifndef XTS_COND_H
#define XTS_COND_H
#include <stdint.h>

#define XTS_MAX_AXES 32
#define XTS_MAX_AXIS_VALUES 32

typedef struct xts_axis {
  const char *name;                 /* identifier, also the profile key: "sf" */
  const char *const *values;        /* NULL-terminated; values[0] is the default (the state when the axis is not applied) */
  int (*apply)(int value_index);    /* may be NULL (pure labelling axis) */
  void (*revert)(int value_index);  /* may be NULL */
  /* optional extra filter: return 0 if this axis must not be applied to family `fam_name` of kind `kind` even if the
   * family lists it (NULL = always) */
  int (*applies_to)(const char *fam_name, const char *kind);
  int order;                        /* apply order, ascending (revert in reverse); ties by name */
  const char *desc;
} xts_axis_t;

/* Registration: one pointer per axis in linker section "xts_axis" (collected by src/core/registry.c). */
#define XTS_AXIS(id, ...)                                                                              \
  static const xts_axis_t xts_axis_##id = { .name = #id, __VA_ARGS__ };                                \
  static const xts_axis_t *const xts_axis_ptr_##id __attribute__((used, section("xts_axis"), aligned(sizeof(void *)))) = &xts_axis_##id

/* ---- execution environment (set by axes, read by xts_case_init / the kernel builder / families) ---------------------- */
typedef struct xts_env {
  int sf;                    /* 1: cases run with MSR[SF]=1 (default), 0: 32-bit mode */
  uint32_t fpscr_set, fpscr_clr;   /* xts_case_init: fpscr = (fpscr & ~clr) | set */
  uint32_t vscr_set, vscr_clr;     /* same for VSCR (word 3) */
  uint32_t code_offset;      /* byte offset of the kernel's first word within its 128-byte line (code_align axis) */
  uint32_t data_offset;      /* extra byte offset families add to data pointers (data_align axis); see data_kind */
  int data_kind;             /* 0 aligned, 1 misaligned, 2 line-crossing, 3 4 KB-crossing, 4 64 KB page-crossing (data_align axis) */
  int cache_state;           /* 0 default, 1 cold (dcbf), 2 L1-warm, 3 L2-warm, 4 memory (cache axis) */
  int thread_prio;           /* 0 leave, 1 low (or 1,1,1), 2 medium (or 2,2,2), 3 high (or 3,3,3): kernel prologue */
  int chains;                /* timing: independent chains (0 = family default) */
  int unroll;                /* timing: unroll depth list selector (0 = family default) */
  int speed;                 /* CPU speed code the run is at (speed axis), 0 = not changed */
  int sibling;               /* sibling-thread mode (SMT axes), 0 = idle loop */
  int other_cores;           /* other-core load, 0 = idle */
  int64_t user[16];          /* free slots for future axes: claim one by name with xts_env_slot() */
} xts_env_t;
extern xts_env_t xts_env;
/* Index of a named free slot in xts_env.user[] (allocated on first use, stable for the run). -1 if all are taken. */
int xts_env_slot(const char *name);
void xts_env_reset(void);   /* defaults: sf=1, everything else 0 */

/* ---- condition vector (what a record's "cond" object holds) ------------------------------------------------------------ */
typedef struct xts_condvec {
  int n;                                /* number of registered axes */
  const xts_axis_t *axis[XTS_MAX_AXES]; /* registry order (sorted by order, name) */
  int value[XTS_MAX_AXES];              /* value index per axis */
  uint32_t applied;                     /* bit i: axis i was applied for this block (vs default) */
} xts_condvec_t;
/* value name of axis `name` in a condition vector, or NULL if the axis is not registered */
const char *xts_cond_get(const xts_condvec_t *cv, const char *name);
/* "sf=64;fpscr_rn=rn" (all axes) */
int xts_cond_format(const xts_condvec_t *cv, char *buf, int size, int only_applied);

/* registry access (src/core/registry.c) */
int xts_axis_count(void);
const xts_axis_t *xts_axis_get(int i);
const xts_axis_t *xts_axis_find(const char *name);
int xts_axis_value_index(const xts_axis_t *a, const char *value);   /* -1 if unknown */
int xts_axis_nvalues(const xts_axis_t *a);
#endif
