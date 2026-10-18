/* XTS - hardware thread control (src/smt): load kernels on the test core's sibling (PIR 1) and on the spare threads of
 * cores 1/2 (PIR 3 and 5), park and wake the sibling. Used by src/conds/{sibling,other_cores}.c; families may include
 * <smt/smt.h> to read the sibling's throughput around their last run (two-sided contention data).
 *
 * Roles (xts/xcore.h): PIR 0 test executor, PIR 2 suite (this code runs there), PIR 4 transmitter - all three are
 * reserved (xts_smt_reserved). An idle spare thread spins in libxenon's `secondary` loop at `or 1,1,1` polling its
 * wait[] slot; a task is queued there. Our worker sets STARTED, runs a JIT-built load kernel until STOP, sets DONE and
 * returns to that loop. Core-0 state (CTRL[TE1], TSCR) is read and written ON core 0 (a remote call to the test core).
 * Every wait has a timeout; a thread that does not answer becomes LOST and is never used again in this boot (apply()
 * returns <0, the block is recorded as skip). There is no watchdog and no speed change any more (the speed code is only
 * read). Nothing here touches the CPU key or fuses.
 */
#ifndef XTS_SMT_H
#define XTS_SMT_H
#include <stdint.h>
#include <xts/xts.h>

enum xts_smt_load {
  XTS_SMT_NONE = 0,       /* thread in libxenon's wait loop */
  XTS_SMT_BUSY_INT,       /* 12 independent integer add chains */
  XTS_SMT_BUSY_FP,        /* 12 independent fmadd chains (values stay 1.0: no denormals, no exceptions) */
  XTS_SMT_BUSY_VMX,       /* 12 independent vmaddfp chains (1.0f) */
  XTS_SMT_BUSY_MEM,       /* random pointer chase over a private 4 MB buffer (memory latency bound) */
  XTS_SMT_SAME_INSN,      /* the case's own instruction words (safe subset), see xts_smt_same_words */
  XTS_SMT_BUSY_LD,        /* independent L1-resident loads (same_insn fallback for load/store words) */
  XTS_SMT_L2_PRESSURE,    /* random chase + store over an L2-sized slice (768 KB total) */
  XTS_SMT_MEM_STREAM,     /* sequential line loads over a 3 MB slice per thread */
  XTS_SMT_NLOADS
};
const char *xts_smt_load_name(int load);

enum xts_smt_state { XTS_SMT_ST_IDLE = 0, XTS_SMT_ST_RUNNING, XTS_SMT_ST_PARKED, XTS_SMT_ST_LOST };
const char *xts_smt_state_name(int st);

/* error codes (negative returns) */
#define XTS_SMT_E_PIR      -1   /* bad / reserved PIR */
#define XTS_SMT_E_LOST     -2   /* thread did not answer earlier: unusable this boot */
#define XTS_SMT_E_BUSY     -3   /* libxenon slot stayed busy */
#define XTS_SMT_E_START_TO -4   /* STARTED not seen in time */
#define XTS_SMT_E_STOP_TO  -5   /* DONE not seen in time */
#define XTS_SMT_E_PARK_TO  -6   /* CTRL[TE1] did not clear */
#define XTS_SMT_E_WAKE_TO  -7   /* CTRL[TE1] did not set / task did not return */
#define XTS_SMT_E_CORES    -8   /* threads 2-5 did not come up */
#define XTS_SMT_E_BUILD    -9   /* kernel build / allocation failed */
#define XTS_SMT_E_STATE    -10  /* thread not in the state the call needs */
#define XTS_SMT_E_EXIT     -11  /* the kernel exit path found an inconsistent TCB (smt_entry.S self-check) */
const char *xts_smt_err_name(int e);

/* x->aux_status bits set by the SMT exec hooks */
#define XTS_SMT_AUX_SIBLING   0x01   /* a sibling load ran around this run */
#define XTS_SMT_AUX_CORES     0x02   /* other-core loads ran around this run */
#define XTS_SMT_AUX_PARKED    0x04   /* sibling parked (CTRL[TE1] = 0 verified at the run) */
#define XTS_SMT_AUX_RESTARTED 0x08   /* same_insn: sibling (re)started with this run's words */
#define XTS_SMT_AUX_FALLBACK  0x10   /* same_insn: no safe word, a unit-matched busy kernel ran instead */
#define XTS_SMT_AUX_STALLED   0x20   /* a load thread's heartbeat stopped (run cancelled) */

/* ---- thread control ---- */
int xts_smt_init(void);                                   /* idempotent */
int xts_smt_state(int pir);
int xts_smt_cores_up(void);                               /* 0 or XTS_SMT_E_CORES */
int xts_smt_start(int pir, int load, const uint32_t *words, int nwords);   /* words: same_insn only */
int xts_smt_stop(int pir);                                /* 0, or <0 (thread now LOST) */
int xts_smt_park(void);                                   /* sibling (PIR 1) only; verified by CTRL[TE1] readback */
int xts_smt_wake(void);
int xts_smt_cores_start(int load, int *pirs_out);         /* load on PIR 3 and 5 (the spare threads); all or nothing */
int xts_smt_all_idle(void);                               /* 1 if no thread runs a load or is parked */
uint32_t xts_smt_iters(int pir);                          /* heartbeat / outer-loop counter (wraps) */
uint32_t xts_smt_ops_per_iter(int pir);                   /* body instructions per outer iteration of the current load */
int xts_smt_reserved(int pir);                            /* 1: a role thread (PIR 0 test, 2 suite, 4 transmitter) */
/* exit path self-check of the last finished load on pir: 1 = the kernel returned through xts_smt_exit with a consistent
 * TCB (exit_why 1), 0 = not run yet, <0 = XTS_SMT_E_EXIT */
int xts_smt_exit_check(int pir);

/* same_insn: pick the words of a run that are safe to loop on another thread (user-mode, executes, integer/FP/vector
 * unit, no memory/branch/LR/CTR/SPR effects). Returns the number kept in out (<= max); *fallback = load to run instead
 * when none is safe (BUSY_INT/BUSY_FP/BUSY_VMX/BUSY_LD by the unit of the first word). */
int xts_smt_same_words(const uint32_t *w, int n, uint32_t *out, int max, int *fallback);

/* ---- exec hook (one chained hook, xts_exec_hook_add; shared by the sibling and other_cores axes) ---- */
int xts_smt_hooks_install(void);          /* idempotent; 0, or -1 when no exec hook slot is free */
void xts_smt_hooks_remove(void);          /* removes it unless a hook mode (sibling / cores) is still active */
extern int xts_smt_hook_sibling_mode;     /* 0 none, -1 parked, else the sibling's enum xts_smt_load */
extern int xts_smt_hook_cores;            /* 1: other-core loads active */
/* sibling iterations / timebase ticks over the last run (between the hooks), and totals since block start */
typedef struct xts_smt_runstats {
  uint32_t sib_iters, ticks, cores_iters;
  uint32_t aux;
  uint64_t blk_runs, blk_sib_iters, blk_ticks, blk_cores_iters;
  uint32_t restarts, fallbacks, stalls, refused;
} xts_smt_runstats_t;
extern xts_smt_runstats_t xts_smt_rs;

/* ---- per-block info record (emitted from an axis revert into the current block) ---- */
struct xts_json;
void xts_smt_info_begin(struct xts_json *j, const char *axis, const char *value);   /* common fields + temps etc. */
/* record: case "smt:<axis>", metric "info", status info, value/unit as given (xts_emit_block_info) */
void xts_smt_emit_info(const char *axis, double value, const char *unit, const char *detail);
int xts_smt_temps_json(struct xts_json *j, const char *key);                         /* SMC temps as {cpu,gpu,edram,board} */

/* ---- speed (read only: the code XeLL left; changing it is no longer supported) ---- */
int xts_smt_speed_get(void);
#endif
