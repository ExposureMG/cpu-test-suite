/* XTS - running cases on the CPU (src/exec).
 *
 * A case is: a full input state (every GPR/FPR/VR/CR/XER/LR/CTR/FPSCR/VSCR + 512 bytes of scratch memory), one
 * instruction word or a JIT kernel (xts/kernel.h), and the full output state + trap information.
 *
 *   static xts_exec_t x;                       // ~6 KB: keep it static or heap, not on a small stack
 *   xts_case_init(&x);                         // canary baseline + xts_env (MSR[SF], FPSCR/VSCR condition bits)
 *   xts_set_gpr(&x, 4, 0x1234);  xts_set_ptr(&x, 5, 128);   // r5 = &scratch[128]
 *   int st = xts_exec_word(&x, xts_enc(XI_lwz, 0, 3, 0, 5));
 *   if (st == XTS_EXEC_OK) ... x.out.gpr[3], x.mem_out[...], x.trap.trapped/cause/srr0 ...
 *
 * On the console the word is patched into the slot (or the kernel is copied into the kernel buffer) and runs for real;
 * the 0x600/0x700 stubs record a program/alignment interrupt and resume after the faulting instruction. A trap whose SRR0
 * is outside the slot and kernel ranges is a suite crash (libxenon's handler -> crash record), never a silent skip.
 * On the host nothing executes: single words are SIMULATED with the reference model when it implements them, kernels
 * are validated (every word decodes to the instruction it was built for) and reported as DRYRUN.
 */
#ifndef XTS_EXEC_H
#define XTS_EXEC_H
#include <stdint.h>
#include <xts/state.h>

enum xts_exec_status {
  XTS_EXEC_OK = 0,          /* ran on the CPU (trapped or not: see trap.trapped) */
  XTS_EXEC_SIMULATED = 1,   /* host: the reference model produced out/trap */
  XTS_EXEC_DRYRUN = 2,      /* host: not executed (model unimplemented, or a kernel); out == in */
  XTS_EXEC_NOTRUN = -1,     /* could not run (allocation failed, bad setup); see err */
  XTS_EXEC_BADKERNEL = -2,  /* kernel builder reported an error / dry-run decode check failed */
};

/* interrupt causes, (SRR1 >> 16) & 0xFF of a program interrupt (hardware-verified values) */
#define XTS_CAUSE_TRAP    0x02
#define XTS_CAUSE_ILLEGAL 0x08
#define XTS_CAUSE_PRIV    0x04
#define XTS_CAUSE_FP      0x10

typedef struct xts_trap {
  int trapped;          /* 1: a 0x600/0x700 interrupt happened inside the case (execution resumed at SRR0+4) */
  uint32_t vector;      /* 0x700 program, 0x600 alignment */
  uint32_t cause;       /* (srr1 >> 16) & 0xFF for vector 0x700 */
  uint64_t srr0, srr1, dar;
  int where;            /* 0 slot, 1 kernel buffer, 2 extra range (xts_exec_trap_range) */
  uint32_t offset;      /* SRR0 - start of that range (kernel: byte offset of the faulting word from the kernel start) */
} xts_trap_t;

typedef struct xts_exec {
  xts_state_t in;                          /* input state: filled by xts_case_init + setters */
  xts_state_t out;                         /* output state after the run */
  uint8_t mem_in[XTS_SCRATCH_BYTES];       /* scratch memory before the run (what the case starts with) */
  uint8_t mem_out[XTS_SCRATCH_BYTES];      /* scratch memory after the run */
  xts_trap_t trap;
  int status;                              /* enum xts_exec_status of the last run */
  char err[96];
  /* deferred pointers resolved at run time (code addresses depend on where the kernel lands) */
  struct { uint8_t reg, base; int32_t off; } code_ptr[4];
  int ncode_ptr;
  uint64_t ticks;                          /* timebase ticks around the whole slot call (coarse; timing uses mftb in-kernel) */
  uint32_t aux_status;                     /* set by xts_exec_hooks (e.g. sibling started / timed out); 0 otherwise */
  /* what the current run executes, valid inside xts_exec_hooks.pre/post: a single word (run_kernel NULL), or the kernel's
   * words as built (run_word 0; exits are still unpatched). xts_exec_again keeps the previous values. */
  uint32_t run_word;
  const uint32_t *run_kernel;
  uint32_t run_kernel_words;
} xts_exec_t;

/* Hooks around every run (console: right before / after the slot call, after all cache maintenance; host: around the
 * simulation and the kernel dry run). For per-case work that must bracket the case itself, e.g. the SMT axes starting a
 * sibling-thread kernel.
 *
 * Chained hooks (v2): xts_exec_hook_add(pre, post, ud) returns a handle (>= 0) or -1 when all XTS_EXEC_MAX_HOOKS slots
 * are taken; xts_exec_hook_remove(handle) frees it (0, or -1 for a bad handle). Either function may be NULL.
 * Order: pre hooks run in the order they were added, post hooks in reverse order. pre() returning <0 cancels the run
 * (status XTS_EXEC_NOTRUN, err set by the hook or a generic message); the post hooks of the pre hooks that already ran
 * are then called (reverse order) with x->status == XTS_EXEC_NOTRUN so they can undo what they started.
 * Add in an axis' apply(), remove in revert(). The core removes every hook still registered after each block (and logs
 * it), so a forgotten remove never leaks into the next block.
 *
 * The v1 single slot `xts_exec_hooks` still works: it behaves as an extra hook that runs FIRST (pre) and LAST (post);
 * the core clears it after every block as before. */
typedef struct xts_exec_hooks {
  int (*pre)(xts_exec_t *x);
  void (*post)(xts_exec_t *x);
} xts_exec_hooks_t;
extern xts_exec_hooks_t xts_exec_hooks;
#define XTS_EXEC_MAX_HOOKS 8
typedef int (*xts_exec_pre_fn)(xts_exec_t *x, void *ud);
typedef void (*xts_exec_post_fn)(xts_exec_t *x, void *ud);
int xts_exec_hook_add(xts_exec_pre_fn pre, xts_exec_post_fn post, void *ud);
int xts_exec_hook_remove(int handle);
int xts_exec_hook_count(void);            /* hooks registered (chained ones; not counting the v1 slot) */
void xts_exec_hook_clear(void);           /* remove all chained hooks and clear the v1 slot (core: after every block) */

/* ---- case setup ---- */
void xts_case_init(xts_exec_t *x);         /* baseline + env (sf, fpscr/vscr set/clr) + scratch baseline */
void xts_case_apply_env(xts_exec_t *x);    /* only the env part (call again after changing in.fpscr if you want it) */
static inline void xts_set_gpr(xts_exec_t *x, int r, uint64_t v) { x->in.gpr[r & 31] = v; }
static inline void xts_set_fpr(xts_exec_t *x, int r, uint64_t v) { x->in.fpr[r & 31] = v; }
static inline void xts_set_vr(xts_exec_t *x, int r, xts_vr_t v) { x->in.vr[r & 127] = v; }
static inline void xts_set_cr(xts_exec_t *x, uint32_t v) { x->in.cr = v; }
static inline void xts_set_xer(xts_exec_t *x, uint64_t v) { x->in.xer = v; }
static inline void xts_set_lr(xts_exec_t *x, uint64_t v) { x->in.lr = v; }
static inline void xts_set_ctr(xts_exec_t *x, uint64_t v) { x->in.ctr = v; }
static inline void xts_set_fpscr(xts_exec_t *x, uint32_t v) { x->in.fpscr = v; }
static inline void xts_set_vscr(xts_exec_t *x, uint32_t v) { x->in.vscr = xts_vr4(0, 0, 0, v); }
static inline void xts_set_sf(xts_exec_t *x, int sf) { x->in.mode = sf ? (x->in.mode | XTS_MODE_SF) : (x->in.mode & ~(uint64_t)XTS_MODE_SF); }
/* r = address of scratch[off] (off may be anything; the model treats addresses outside scratch as DSI) */
void xts_set_ptr(xts_exec_t *x, int r, int64_t off);
/* scratch bytes */
void xts_set_mem(xts_exec_t *x, int off, const void *bytes, int n);
void xts_set_mem_be(xts_exec_t *x, int off, int n, uint64_t v);           /* big-endian n-byte value */
/* scratch word at byte offset `at` := address of scratch[target] (u64, or u32 if as32) - pointer chases in scratch */
void xts_set_mem_ptr(xts_exec_t *x, int at, int target, int as32);
/* code pointer: reg 0-31 GPR, 32 LR, 33 CTR := base + off where base is XTS_CODE_SLOT (the slot instruction),
 * XTS_CODE_POST (the state-store epilogue), XTS_CODE_KERNEL (first word of the kernel as placed) */
enum { XTS_CODE_SLOT = 0, XTS_CODE_POST = 1, XTS_CODE_KERNEL = 2 };
int xts_set_code_ptr(xts_exec_t *x, int reg, int base, int32_t off);

/* ---- scratch / big buffer addresses ---- */
uint64_t xts_scratch_addr(void);           /* address case code sees for scratch[0] (host: a fixed fake address) */
/* Big buffer for working-set tests: one region, allocated on first use (64 KB aligned), bounded below
 * xts_plat_heap_limit (the stacks). Returns NULL if `bytes` cannot be provided; the previous size is then still allocated (xts_big_size), but
 * growing never preserves contents. *addr = address the test code sees. */
void *xts_big(uint32_t bytes, uint64_t *addr);
uint32_t xts_big_size(void);               /* bytes currently allocated (0 = none) */
void xts_set_big_ptr(xts_exec_t *x, int r, uint32_t off);   /* r = big buffer address + off (allocates if needed) */

/* Prepare a big-buffer region (port of the runner's hp_prep): returns <0 for bad parameters.
 *   mode 0: `ncycles` disjoint random pointer cycles (Sattolo) over elements of `stride` bytes, u64 pointer at slot8*8
 *   mode 1: sequential cycles (successor of i is i+ncycles)      mode 2: zero    mode 3: dcbf    mode 4: touch
 *   mode 5: xorshift word fill;  | XTS_PREP_DCBF: dcbf afterwards,  | XTS_PREP_TOUCH: then touch every line (on the test core)
 * Every mode except 4 ends with xts_dcbf_range over the region (the suite's writes must reach the test core; XTS_PREP_DCBF
 * is implied), so a prepared region is cold unless XTS_PREP_TOUCH re-warms it. */
#define XTS_PREP_DCBF  0x80
#define XTS_PREP_TOUCH 0x40
int xts_prep(uint32_t base, uint32_t nelem, uint32_t stride, int mode, int ncycles, uint32_t seed, int slot8);
/* the pure part (no cache work), shared with the host tests: buf as addressed by this program, buf_addr as seen by tests */
int xts_prep_pure(uint8_t *buf, uint32_t buf_addr, uint32_t buf_bytes, uint32_t base, uint32_t nelem, uint32_t stride,
                  int mode, int ncycles, uint32_t seed, int slot8, uint64_t *span_out);

/* ---- running ---- */
int xts_exec_word(xts_exec_t *x, uint32_t word);
struct xts_kbuf;
int xts_exec_kernel(xts_exec_t *x, struct xts_kbuf *k);
/* Run the same state again without re-copying anything but the scratch memory (timing repeats). */
int xts_exec_again(xts_exec_t *x);

/* Extra address ranges in which a 0x600/0x700 interrupt counts as "inside the case" (e.g. a sibling thread's kernel
 * buffer). idx 0..XTS_EXEC_EXTRA_RANGES-1; [lo, hi) compared on the low 32 bits; hi - lo must be < 64 KB (the vector stub
 * checks each range with one unsigned 16-bit compare). lo == hi removes it. trap.where = 2 + idx, trap.offset = SRR0 - lo.
 * Returns 0, or -1 (bad idx, range too large or inverted). Each user should own one index (e.g. the sibling axis 0,
 * other_cores 1..3). xts_exec_trap_range_get reads one back (0 ok, -1 bad idx). */
#define XTS_EXEC_EXTRA_RANGES 4
#define XTS_EXEC_RANGE_MAX 0xFFFFu
int xts_exec_trap_range(int idx, uint64_t lo, uint64_t hi);
int xts_exec_trap_range_get(int idx, uint64_t *lo, uint64_t *hi);
/* classify an interrupt address against the extra ranges: 2 + idx and *offset, or -1 if in none (shared by the backends,
 * host-tested) */
int xts_exec_trap_classify(uint64_t srr0, uint32_t *offset);

/* ---- engine info ---- */
int xts_exec_init(void);                   /* install vectors etc. (core calls it once) */
uint64_t xts_exec_slot_addr(void);         /* address of the slot instruction (SRR0 of a trapping single word) */
uint64_t xts_exec_post_addr(void);
uint64_t xts_exec_kernel_addr(void);       /* start of the kernel buffer */
uint32_t xts_exec_kernel_capacity(void);   /* words */
int xts_exec_is_host(void);

/* Cache maintenance for generated code / data (console: dcbst/sync/icbi/isync; host: no-op). xts_dcbf_range flushes on the
 * calling core AND on the test core (an xdcbt'd line in the test core's L1 survives a remote dcbf); xts_touch_range touches on the
 * test core. */
void xts_sync_code(void *addr, uint32_t len);
void xts_dcbf_range(void *addr, uint32_t len);
void xts_touch_range(const void *addr, uint32_t len);
/* `b` from address `from` to address `to` (both as the CPU sees them) */
uint32_t xts_branch_word(uint64_t from, uint64_t to);

/* ---- semantic oracle (src/exec/semantic.c) ----
 * Run `word` on the CPU with x->in / x->mem_in AND in the reference model, compare, emit ONE record for ctx:
 *   pass       model and CPU agree (both ran normally, or both took the same interrupt: vector, cause, SRR0 = slot)
 *   fail       they disagree; detail = operand inputs, CPU result, model result, every differing item
 *   skip       model unimplemented (detail keeps everything the CPU changed, so the data is not lost) or host dry run
 *   pass/fail/info  estimate instructions the model cannot answer exactly: judged against the architected bound
 *              ("model":"estimate", XTS_M_ESTIMATE; see xts_estimate_verdict below)
 * Returns the record status. `note` (may be NULL) is copied into detail.note. */
struct xts_ctx;
int xts_semantic_check(struct xts_ctx *ctx, xts_exec_t *x, uint32_t word, const char *note);
/* Memory-safety assertion for a single word a family runs itself (xts_exec_word / a one-word kernel) without xts_semantic_check:
 * returns 1 if `word` is a memory instruction whose access window (cache ops: the 128-byte line; dst/dstst: the base address;
 * string/multiple: 128 bytes; AltiVec: the aligned 16 bytes; others the 1/2/4/8 bytes they access), computed from the input state `in`, is not
 * inside the scratch area or the big buffer. `why` ("EA outside test memory: ...") says so; the word must then NOT be run
 * (record a skip). Returns 0 for non-memory words and for windows inside test memory. xts_semantic_check, xe_check_kernel and
 * both backends' xts_exec_word apply it already, whatever the model result; the model cannot see the console's MMIO. */
int xts_exec_ea_outside(uint32_t word, const xts_state_t *in, char *why, int why_size);
/* Estimate instructions (fres frsqrte vrefp vrsqrtefp vexptefp vlogefp and the 128 forms): where the model has no exact
 * answer (XTS_M_UNIMPLEMENTED: the result needs the hardware table), xts_semantic_check judges the CPU result against the
 * architected accuracy bound instead ("model":"estimate" in the detail):
 *   per lane (scalar: one lane)  exact   the model knows that lane exactly (special operand/result): bits must match
 *                                bound   |est*x - 1| <= 2^-k (recip) / est^2*x within (1 +- 2^-k)^2 (rsqrt); k = 8 fres,
 *                                        5 frsqrte, 12 vector
 *                                unknown no bound is known (vexptefp/vlogefp non-special lanes, scalar special cases)
 *   status  fail  any lane outside its bound / exact value, or any state other than the target register (and FPSCR/CR1
 *                 for the scalar forms) changed
 *           pass  at least one lane checked and all checked lanes are within
 *           info  nothing could be checked (only unknown lanes)
 * detail "lanes":[{"x":..,"cpu":..,"verdict":"bound|exact|unknown","ok":true}, ...].
 * xts_estimate_kind returns 0 recip / 1 rsqrt / 2 exp2 / 3 log2 and *vector (0/1), or -1 if insn is not an estimate.
 * xts_estimate_verdict is the check itself (in = case input, out = CPU result); writes "lanes" etc. into an open object
 * j (may be NULL) and returns the status as above. */
int xts_estimate_kind(int insn, int *vector);
struct xts_json;
int xts_estimate_verdict(int insn, uint32_t word, const xts_state_t *in, const xts_state_t *out, struct xts_json *j);

/* Restore check: 1 if the C runtime FPSCR/VSCR/VRSAVE are what they were before the last run (the slot restores them;
 * host-test and the self-test family check this) */
int xts_exec_runtime_intact(void);
#endif
