/* XTS core - private interfaces between the core modules (not for families/axes/models). */
#ifndef XTS_CORE_H
#define XTS_CORE_H
#include <stddef.h>
#include <stdint.h>
#include <xts/xts.h>
#include <xts/net.h>
#include <xts/xcore.h>

/* ---- registry (registry.c) ---- */
int xts_registry_init(void (*print)(const char *));   /* collect + sort + validate; returns number of problems */

/* ---- scheduler (sched.c) ---- */
typedef struct xts_block {
  int row;                        /* profile row */
  int fam;                        /* family index (registry) */
  int first_case, ncases;         /* cases first_case + k * stride, k = 0 .. ncases-1 */
  int stride;                     /* 1, or the row's max_cases sampling stride */
  int finish;                     /* 1: the family's finish() pseudo-block of this row (ncases 0) */
  int repeats;
  uint64_t seed;                  /* row seed */
  uint8_t value[XTS_MAX_AXES];    /* value index per registered axis */
  uint32_t applied;               /* axes applied for this block */
  char key[160];                  /* block key ("r<row>:<family>:<cond>:c<n>"), the record's "block" */
} xts_block_t;

typedef struct xts_plan {
  const xts_profile_t *profile;
  xts_block_t *b;
  int n, cap;
  xts_caselist_t *cases;          /* per family (index = registry index), enumerated once */
  int *enumerated;
  int total_cases;                /* sum over blocks of ncases * repeats */
} xts_plan_t;
int xts_plan_build(xts_plan_t *p, const xts_profile_t *prof);   /* enumerate everything; 0 ok */
int xts_block_case(const xts_block_t *b, int k);          /* case index of the k-th case of the block */
int xts_block_rel(const xts_block_t *b, int case_index);  /* k of a case index in the block, -1 if not in it */
void xts_plan_condvec(const xts_block_t *b, xts_condvec_t *cv);
uint64_t xts_case_seed(const xts_block_t *b, const xts_case_t *c, int repeat);

/* ---- breadcrumbs and crash/hang reports (crash.c) ----
 * xts_crumb (RAM, suite core) always says what the suite is doing: block, case (index, id, family, mnemonic), phase and,
 * in self/semantic blocks, the last executed word and its input registers (an exec pre/post hook). main.c sends it as a
 * CRUMB message before cases (every case for self/semantic families, every XTS_CRUMB_EVERY_TIMING-th otherwise) and the
 * report builder quotes it in CRASH/HANG reports. */
enum { XTS_PH_IDLE = 0, XTS_PH_BLOCK_START, XTS_PH_CASE, XTS_PH_BETWEEN, XTS_PH_BLOCK_END, XTS_PH_FINISH, XTS_PH_RUN_END };
const char *xts_phase_name(int ph);
typedef struct xts_crumb {
  char block[160];
  char family[48];
  char kind[16];
  char case_id[48];
  char mnem[24];
  int32_t case_rel, case_index, repeat;
  int phase;
  int every;                      /* CRUMB message before every Nth case of this block (1 = every case = exact) */
  uint64_t seq, tb, seed;         /* seq: records emitted so far */
  uint32_t ncase;                 /* cases started in this block */
  /* exec hook (self/semantic blocks): the last execution of the current case */
  int exec_valid, in_exec;
  uint32_t word, kwords;
  uint64_t execs;
  uint64_t gpr_in[32];
  uint64_t lr_in, ctr_in, xer_in;
  uint32_t cr_in;
} xts_crumb_t;
extern xts_crumb_t xts_crumb;
void xts_crumb_block(const char *block, const char *family, const char *kind, int every);
void xts_crumb_case(int rel, int case_index, int repeat, const char *case_id, const char *mnem, uint64_t seq, uint64_t seed);
void xts_crumb_phase(int phase);
int xts_crumb_hook_install(void);         /* exec pre/post hook filling word + input registers; 0 or -1 */
void xts_crumb_hook_remove(void);
/* the crumb as a JSON object (the CRUMB message payload, and "crumb" in CRASH/HANG reports) */
void xts_crumb_json(struct xts_json *j);
/* the crumb's input state as a JSON object ({} if no execution was seen) */
void xts_crumb_input_json(struct xts_json *j);

/* run.log tail ring (every xts_log line; the last XTS_LOGRING are quoted by reports) */
#define XTS_LOGRING 50
void xts_crash_log_note(const char *line);
void xts_crash_log_tail_json(struct xts_json *j);    /* ["line", ...] oldest first */

/* Report builders (pure: write into buf, return the length; never send). CRASH: the crash slot of the PIR that crashed
 * (xts/xcore.h) + crumb + input state + log tail; HANG: crumb + timeout + heartbeats + input state + log tail.
 * Payload formats: docs/PROTOCOL.md. */
int xts_crash_report_json(char *buf, int cap, const xts_xc_crash_t *slot);
int xts_hang_report_json(char *buf, int cap, uint32_t timeout_us);

/* The test core is gone (xts_xc_call returned XTS_XC_TIMEOUT / XTS_XC_CRASHED / XTS_XC_DOWN). Called by the exec layer
 * (or anything else that hands work to the test core) on the suite core. Emits a hang/crash record for the current case
 * (from xts_crumb), sends the CRASH/HANG report, BLOCK and END (status "crashed" / "hang"), drains the transport and halts.
 * Never returns. Safe before/after a run (then only the report and END are sent if the transport is up). */
void xts_core_test_core_lost(int reason) __attribute__((noreturn));
/* xts_xc_call with the current case timeout (per family kind, xts_core_xc_timeout_us); on anything but XTS_XC_OK it
 * calls xts_core_test_core_lost (no return). Returns XTS_XC_OK. */
int xts_core_xc_call(void (*fn)(void *, void *), void *a, void *b);
uint32_t xts_core_xc_timeout_us(void);    /* self/semantic/test: 2 s; timing/sched/calib: 10 s; mem/smt: 60 s */

/* deliberate crash for the self.crash family (plat): console executes tw 31,0,0 on the test core outside every trap
 * range (does not return if the crash path works; returns 0 if it did). Host: -1 (not simulated) unless
 * xts_host_crash_sim is set (host tests in a forked child): then the host test core "crashes" (xcore_host.c) and
 * xts_core_test_core_lost runs as on the console (CRASH + END crashed, exit(1)). */
int xts_plat_self_crash(void);
extern int xts_host_crash_sim;
#ifdef XTS_HOST
/* host xts_xc_* simulation (src/plat/xcore_host.c): only valid inside a function run by xts_xc_call. crash: fills PIR 0's
 * crash slot (vector, srr0, gpr from regs or zeros) and makes xts_xc_call return XTS_XC_CRASHED; hang: XTS_XC_TIMEOUT. */
void xts_host_xc_crash(uint32_t vector, uint64_t srr0, const uint64_t *gpr32) __attribute__((noreturn));
void xts_host_xc_hang(void) __attribute__((noreturn));
/* fill the crash slot of another PIR (a thread crash the suite reports at the block end); NULL clears it */
void xts_host_xc_set_slot(int pir, const xts_xc_crash_t *c);
/* host transport statistics for tests (src/plat/net_host.c): frames dropped on purpose (XTS_NET_DROP) */
uint32_t xts_net_host_dropped(uint32_t *seqs, uint32_t max);
#endif
/* 1 if a row of the running profile names `family` exactly (no glob): opt-in families such as self.crash */
int xts_profile_names_family(const char *family);
extern const char *xts_active_profile_name;

/* ---- screen ---- */
void xts_screen_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* screen + run.log */
void xts_status_line(int force, const char *fmt, ...) __attribute__((format(printf, 2, 3)));   /* <= 1/s unless forced */

/* ---- sink (sink.c): every record -> REC, every xts_log line -> LOG, nothing stored on the console ----
 * Before xts_sink_open (and in host unit tests without a run) records only reach xts_sink_tap and log lines are kept in a
 * RAM buffer (sent after HELLO). All sends go through xts_core_send, which counts messages per type, the last frame seq,
 * send failures and the REC hash for END. */
int xts_sink_open(const char *run_id, const char *tag);   /* new run: counters, results store; 0 ok */
void xts_sink_hello(const char *hello_json);   /* HELLO x3, then the buffered log lines */
void xts_sink_set_block(const char *key, const xts_condvec_t *cv, const char *family);
void xts_sink_block_end(const char *key, const char *extra_members);   /* BLOCK {"block","records",<extra>} */
/* END x3 {"status", "last_seq", "messages", "rec_hash", ...} then drain; 0 ok, -1 drain timeout */
int xts_sink_end(const char *status);
void xts_sink_close(void);
int xts_sink_is_open(void);
uint64_t xts_sink_seq(void);                 /* records emitted (the last record's seq) */
uint32_t xts_core_send(int type, const char *payload, uint32_t len);   /* counted xts_net_send; 0 = not queued */
uint32_t xts_sink_send_failures(void);
uint64_t xts_sink_msg_count(int type);
const uint32_t *xts_sink_status_counts(void);   /* records per status this run */
const char *xts_sink_run_id(void);
/* write a record without a case context (hang/crash/skip records produced by the core) */
void xts_sink_emit_core(const char *family, const char *case_id, const char *block_key, const xts_condvec_t *cv,
                        int status, const char *metric, const char *detail);
const xts_condvec_t *xts_sink_block_cv(void);   /* condition vector of the current block */
/* test tap: called for every record (after defaults/vocabulary handling) with the final record line */
typedef void (*xts_sink_tap_fn)(const char *family, const char *case_id, const xts_record_t *r, const char *json_line);
extern xts_sink_tap_fn xts_sink_tap;
extern int xts_vocab_check;          /* 1 (default on the host): check metric/unit vocabularies in xts_emit */
void xts_sink_discard(void);         /* host tests: drop the pre-run log buffer */

/* ---- in-RAM results store (results.c): the REC lines of this run, for xts_results_foreach / finish hooks ----
 * Lines are kept per family in RAM chunks. Total cap XTS_RESULTS_CAP (64 MB): when it is reached, the stored lines of
 * families WITHOUT a .finish hook are freed and such lines are no longer stored (nothing reads them but
 * xts_results_foreach(NULL, ...)); lines of families WITH a .finish hook are always kept, up to the cap counted over
 * them alone (beyond it they are dropped and counted, and the run.log says so once). */
#define XTS_RESULTS_CAP (64u << 20)
void xts_results_reset(void);
void xts_results_add(const char *family, uint64_t seq, const char *line, uint32_t len);
void xts_results_stats(uint64_t *bytes, uint64_t *lines, uint64_t *dropped_lines, int *evicted);
void xts_results_set_cap(uint32_t cap);      /* tests */

/* ---- quarantine (main.c; quarantine.txt -> $(BUILD)/gen/quarantine.h at build time) ---- */
typedef struct xts_quar { const char *family, *case_id; } xts_quar_t;
void xts_quarantine_set(const xts_quar_t *list, int n);   /* tests: replace the compiled-in list (NULL = compiled-in) */
int xts_quarantined(const char *family, const char *case_id);

/* ---- sysinfo (sysinfo.c) ---- */
int xts_sysinfo_json(struct xts_json *j, const char *run_id, const char *tag);   /* members of an open object */
const char *xts_build_id(void);
const char *xts_git_hash(void);

/* ---- run (main.c) ---- */
typedef struct xts_run_opts {
  int dryrun;                     /* host dry run: also print the kernel/word statistics */
  int max_blocks;                 /* testing: stop after this many blocks (0 = all; END status "aborted") */
  const char *only_family;        /* testing */
} xts_run_opts_t;
/* The run: HELLO, blocks, END. The transport must be up (xts_net_init). Returns 0 (2: dry run with bad words, -1 plan
 * failure, -5 messages could not be queued). */
int xts_run(const xts_profile_t *prof, const xts_run_opts_t *o);
extern const char *xts_current_run_id;
/* breadcrumb cadence for families that are not self/semantic (0 = only at the block start) */
extern int xts_crumb_every_timing;

/* dry-run statistics (exec_host.c / kbuf.c fill them) */
typedef struct xts_dry_stats {
  uint64_t words, kernels, kernel_words, kernel_insn_words, kernel_raw_words, bad_words, simulated, not_simulated;
  uint64_t enc_errors;
} xts_dry_stats_t;
extern xts_dry_stats_t xts_dry_stats;
#endif
