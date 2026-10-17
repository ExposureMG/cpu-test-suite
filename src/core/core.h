/* XTS core - private interfaces between the core modules (not for families/axes/models). */
#ifndef XTS_CORE_H
#define XTS_CORE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <xts/xts.h>

/* ---- registry (registry.c) ---- */
int xts_registry_init(void (*print)(const char *));   /* collect + sort + validate; returns number of problems */

/* ---- scheduler (sched.c) ---- */
typedef struct xts_block {
  int row;                        /* profile row */
  int fam;                        /* family index (registry) */
  int first_case, ncases;         /* cases first_case + k * stride, k = 0 .. ncases-1 (journal QUAR case= is k) */
  int stride;                     /* 1, or the row's max_cases sampling stride */
  int finish;                     /* 1: the family's finish() pseudo-block of this row (ncases 0) */
  int repeats;
  uint64_t seed;                  /* row seed */
  uint8_t value[XTS_MAX_AXES];    /* value index per registered axis */
  uint32_t applied;               /* axes applied for this block */
  char key[160];                  /* journal key */
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

/* ---- crash page (crash.c) ---- */
#define XTS_CRASH_MAGIC 0x58545343u   /* 'XTSC' */
#define XTS_CRASH_THR_MAGIC 0x58545354u   /* 'XTST' */
#define XTS_CRASH_THREADS 5           /* slots for PIR 1..5 (thr[pir - 1]) */
/* An unhandled exception on another hardware thread (PIR != 0: sibling / other-core loads). Kept in its own slot with its
 * own magic + checksum, so the main thread's breadcrumbs (which keep running) never clear it. */
typedef struct xts_crashthr {
  uint32_t magic, checksum;
  uint32_t vector, pir;
  uint32_t count;                 /* crashes recorded in this slot since the last collect (the first one is kept) */
  int32_t case_index;             /* main thread's breadcrumb at the time */
  uint64_t srr0, srr1, dar, lr, r1, r3;
  uint64_t seq;
  char block_key[96];             /* main thread's block at the time (truncated) */
} xts_crashthr_t;
typedef struct xts_crashpage {
  uint32_t magic, version, checksum, boot;
  char run_id[32];
  char block_key[160];
  int32_t case_index;             /* index within the family's case list, -1 = not inside a case */
  int32_t repeat;
  char case_id[48];
  uint64_t seq;
  /* filled by the exception path only */
  uint32_t crashed;               /* 1 = the fields below are valid */
  uint32_t vector;
  uint64_t srr0, srr1, dar, lr, msr, pir;
  uint64_t gpr[32];
  /* ---- not covered by `checksum`, not cleared by breadcrumbs ---- */
  xts_crashthr_t thr[XTS_CRASH_THREADS];
} xts_crashpage_t;
#define XTS_CRASH_MAIN_BYTES offsetof(xts_crashpage_t, thr)
void xts_crash_breadcrumb(const char *run_id, const char *block_key, int case_index, int repeat, const char *case_id,
                          uint64_t seq);
void xts_crash_clear(void);
/* read and validate the page from the previous boot; returns 1 and copies it if the main record or any thread slot is
 * valid (invalid parts are zeroed in *out: main magic 0 / thr[i].magic 0), then clears the page */
int xts_crash_collect(xts_crashpage_t *out);
uint32_t xts_crash_checksum(const xts_crashpage_t *p);
uint32_t xts_crash_thr_checksum(const xts_crashthr_t *t);
/* called from the exception path (console vec.S -> xts_crash_entry) and by host tests */
void xts_crash_record(uint32_t vector, const uint64_t *ctx40);
/* the core part of an unhandled exception (crash path, no malloc): crash page record, then on PIR 0 the journal CRASH
 * line when a case was running and no file operation was in progress (xts_core_crash_journal) */
void xts_crash_handle(uint32_t vector, const uint64_t *ctx40);
/* main.c: append "CRASH <key> case=<k> vec=<v> srr0=<a> dar=<a>" to the open journal (fflush + fsync) if a case is
 * running (k relative to the block, like QUAR) and xts_fat_busy == 0. Crash-path safe: no malloc, no printf. 1 written */
int xts_core_crash_journal(uint32_t vector, uint64_t srr0, uint64_t dar);
/* crash page survival as determined at this boot (sysinfo.json "crashpage_survived"): 1 yes, 0 no, -1 unknown */
extern int xts_crashpage_survived;
extern char xts_crashpage_how[160];
/* deliberate crash for the self.crash family (plat): console executes tw 31,0,0 from C outside every trap range (does not
 * return if the crash path works; returns 0 if it did). Host: -1 (not simulated) unless xts_host_crash_sim is set (host
 * tests in a forked boot): then the crash path is simulated (crash page + journal CRASH line) and the process _exit(9)s */
int xts_plat_self_crash(void);
extern int xts_host_crash_sim;
/* 1 if a row of the running profile names `family` exactly (no glob): opt-in families such as self.crash */
int xts_profile_names_family(const char *family);
extern const char *xts_active_profile_name;

/* ---- persistent output files (sink.c): kept open for the whole run, fflush + fsync at block end ---- */
typedef struct xts_pfile {
  FILE *f;
  char path[320];
  int dirty;                      /* written since the last sync */
} xts_pfile_t;
int xts_pf_open(xts_pfile_t *pf, const char *path);          /* fopen "ab"; 0 ok (errors counted) */
int xts_pf_write(xts_pfile_t *pf, const void *data, size_t n);  /* 0 ok; reopens a file that failed to open */
int xts_pf_sync(xts_pfile_t *pf);                             /* fflush + fsync(fileno); 0 ok */
void xts_pf_close(xts_pfile_t *pf);
/* file-operation guard: non-zero while the core/plat is inside libfat (the crash path never touches FAT then) */
extern volatile int xts_fat_busy;
/* write/open/close/fsync/mkdir error accounting (run.log, screen, summary.json "write_errors") */
void xts_io_error(const char *what, const char *path);
unsigned xts_io_error_count(void);
/* screen: errors and halts (screen + run.log); the status line is rate-limited to once per second */
void xts_screen_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void xts_status_line(int force, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
const uint32_t *xts_sink_status_counts(void);               /* records per status written this boot */
void xts_sink_flush_block(const char *key);                 /* xts_sink_flush + per-block flush time in run.log */
double xts_sink_flush_ms_total(void);

/* ---- sinks (sink.c) ---- */
int xts_sink_open(const char *dir, const char *run_id, uint64_t first_seq);
void xts_sink_set_block(const char *key, const xts_condvec_t *cv, const char *family);
void xts_sink_flush(void);            /* append buffered text to the files (durable) */
void xts_sink_close(void);
uint64_t xts_sink_seq(void);
void xts_sink_temps(const char *block_key);
int xts_sink_summary(const char *dir);   /* summary.json from results.csv */
void xts_sink_log_raw(const char *line);
const char *xts_sink_dir(void);
/* write a record without a case context (hang/crash/skip records produced by the core) */
void xts_sink_emit_core(const char *family, const char *case_id, const char *block_key, const xts_condvec_t *cv,
                        int status, const char *metric, const char *detail);
/* test tap: called for every record written (after defaults/vocabulary handling) with the final record line */
typedef void (*xts_sink_tap_fn)(const char *family, const char *case_id, const xts_record_t *r, const char *json_line);
extern xts_sink_tap_fn xts_sink_tap;
extern int xts_vocab_check;          /* 1 (default on the host): check metric/unit vocabularies in xts_emit */
void xts_sink_discard(void);         /* host tests: drop buffered (unflushed) text of every sink file */

/* ---- journal (journal.c) ---- */
typedef struct xts_jentry {
  char key[160];
  int begun, ended;               /* counts */
  int open;                       /* last BEGIN not followed by END */
  int quar_all;                   /* whole block quarantined */
  int nquar;
  int32_t quar[16];               /* quarantined case indices (relative to the block's first case) */
  int retry;                      /* RETRY seen: the block re-runs with a CASE line before every case */
  int last_case;                  /* last CASE k since the last BEGIN (-1 = all cases done, -2 = none) */
  int has_crash;                  /* CRASH line since the last BEGIN (crash path on PIR 0) */
  int crash_case;
  uint32_t crash_vec;
  uint64_t crash_srr0, crash_dar;
} xts_jentry_t;
typedef struct xts_journal {
  char path[256];
  xts_jentry_t *e;
  int n, cap;
  uint64_t max_seq;
  int done;                       /* DONE line seen */
  int *hidx;                      /* open-addressing index into e[] (-1 empty), hcap = power of two */
  int hcap;
  int has_cpt;                    /* a "CPT <cycles per tick>" line was seen (calibration survives a reboot/resume) */
  double cpt;                     /* the last one */
  xts_pfile_t pf;                 /* journal.txt open for append (xts_journal_open .. xts_journal_close) */
} xts_journal_t;
int xts_journal_load(xts_journal_t *j, const char *path);     /* missing file = empty journal */
int xts_journal_open(xts_journal_t *j);                       /* keep journal.txt open for appending; 0 ok */
void xts_journal_close(xts_journal_t *j);
xts_jentry_t *xts_journal_find(xts_journal_t *j, const char *key);
int xts_journal_begin(xts_journal_t *j, const char *key);
int xts_journal_end(xts_journal_t *j, const char *key, uint64_t seq);
int xts_journal_quar(xts_journal_t *j, const char *key, int case_rel, const char *status);
int xts_journal_line(xts_journal_t *j, const char *line);    /* free-form (DONE, BOOT, ...) */
int xts_journal_is_quar(const xts_jentry_t *e, int case_rel);
int xts_journal_case(xts_journal_t *j, const char *key, int case_rel);   /* CASE <key> <k> (per-case re-run mode) */
int xts_journal_retry(xts_journal_t *j, const char *key);                 /* RETRY <key> */
/* crash path: append one prepared line ("...\n") to the open journal + fflush + fsync; no malloc/printf */
int xts_journal_raw(xts_journal_t *j, const char *line, size_t n);

/* ---- sysinfo (sysinfo.c) ---- */
int xts_sysinfo_write(const char *path, const char *run_id, const char *tag, int boot);
const char *xts_build_id(void);
const char *xts_git_hash(void);

/* ---- run (main.c) ---- */
typedef struct xts_run_opts {
  const char *root_override;      /* host: output root */
  int dryrun;                     /* host dry run: also print the kernel/word statistics */
  int max_blocks;                 /* testing: stop after this many blocks (0 = all) */
  const char *only_family;        /* testing */
} xts_run_opts_t;
int xts_run(const xts_profile_t *prof, const xts_run_opts_t *o);
extern const char *xts_current_run_id;

/* dry-run statistics (exec_host.c / kbuf.c fill them) */
typedef struct xts_dry_stats {
  uint64_t words, kernels, kernel_words, kernel_insn_words, kernel_raw_words, bad_words, simulated, not_simulated;
  uint64_t enc_errors;
} xts_dry_stats_t;
extern xts_dry_stats_t xts_dry_stats;
#endif
