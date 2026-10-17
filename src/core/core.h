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

/* ---- crash reports and breadcrumbs (crash.c: RAM state + the crash path; slot.c: the report files on the stick) ----
 * Per run, two files are pre-allocated in the run directory while storage is healthy (xts_slots_arm, at a block
 * boundary): crash.json (64 KB) and progress.bin (1 sector). Every sector of both starts with
 *   <'{' or ','>"slot_NNN":"<16-char magic>"
 * (the magic at byte XTS_SLOT_MAGIC_OFF) and is otherwise JSON members + space padding + '\n'; the last sector ends
 * with "}\n". Both files are valid JSON at all times (armed, filled, partly filled). Their device sectors are located
 * once (xts_plat_file_extent) and kept in RAM with a checksum; afterwards they are only written raw, one verified
 * sector at a time (xts_plat_raw_write: only over a sector that holds the magic): progress.bin before cases (the
 * breadcrumb), crash.json by the crash path. libfat never writes them again in this boot. */
#define XTS_SLOT_MAGIC_LEN 16
#define XTS_SLOT_MAGIC_OFF 13
#define XTS_SLOT_HDR 30                  /* header bytes per sector */
#define XTS_CRASH_JSON_BYTES 65536u
#define XTS_CRASH_THREADS 5              /* thread slots for PIR 1..5 */
#define XTS_CRASH_LINES 6                /* screen lines the crash path leaves (xts_crash_screen) */

/* sector packer: builds a slot file image (template headers, members, padding). No malloc, no stdio. */
typedef struct xts_spack {
  uint8_t *img;
  uint32_t ss, nsec, cur, pos;
  const char *magic;
  int dropped;                    /* members that did not fit */
} xts_spack_t;
void xts_spack_init(xts_spack_t *p, uint8_t *img, uint32_t ss, uint32_t nsec, const char *magic);
int xts_spack_put(xts_spack_t *p, const char *member, uint32_t len);   /* member = ',"key":value'; 0 ok, -1 dropped */
uint32_t xts_spack_used(const xts_spack_t *p);                          /* sectors holding members (>= 1) */

/* what PIR 0 is doing (RAM; read by the crash path) */
enum { XTS_PH_IDLE = 0, XTS_PH_BLOCK_START, XTS_PH_CASE, XTS_PH_BETWEEN, XTS_PH_BLOCK_END, XTS_PH_FINISH, XTS_PH_RUN_END };
const char *xts_phase_name(int ph);
typedef struct xts_crumb {
  char run_id[40];
  int boot;
  char block[160];
  char family[48];
  char kind[16];
  char case_id[48];
  char mnem[24];
  int32_t case_rel, case_index, repeat;
  int phase;
  int every;                      /* progress.bin before every Nth case of this block (1 = every case = exact) */
  uint64_t seq, tb, seed;
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
void xts_crumb_block(const char *run_id, int boot, const char *block, const char *family, const char *kind, int every);
void xts_crumb_case(int rel, int case_index, int repeat, const char *case_id, const char *mnem, uint64_t seq, uint64_t seed);
void xts_crumb_phase(int phase);
int xts_crumb_hook_install(void);         /* exec pre/post hook filling word + input registers; 0 or -1 */
void xts_crumb_hook_remove(void);

/* unhandled exception on another hardware thread: kept in RAM, written by PIR 0 at its next case boundary
 * (crash.json, kind "thread") and turned into crash records at the block end */
typedef struct xts_thrcrash {
  volatile uint32_t state;        /* 0 empty, 1 recorded, 2 written to crash.json, 3 reported (record emitted) */
  uint32_t vector, pir, count;
  uint64_t srr0, srr1, dar, lr, ctr, r1, r3, tb;
  int32_t case_index, case_rel;
  char block[160];
  char case_id[48];
} xts_thrcrash_t;
extern xts_thrcrash_t xts_thrcrash[XTS_CRASH_THREADS];
int xts_crash_threads_pending(void);      /* slots in state 1 */
int xts_crash_threads_persist(void);      /* PIR 0, case boundary: crash.json kind "thread"; 0 written */
uint64_t xts_crash_report_tb(void);       /* "tb" of the last report built this boot (0 = none) */

/* crash path (console: vec.S crashdump -> xts_crash_entry; host: xts_plat_self_crash and tests). No libfat, no malloc,
 * no stdio. PIR != 0: thread slot only. PIR 0: if storage is busy (USB driver / disc guard inside an operation) the stick
 * is not touched; else the report is built and written into crash.json (sectors 1.., then sector 0 = the commit).
 * Leaves XTS_CRASH_LINES screen lines (xts_crash_screen) and returns the outcome. */
enum {
  XTS_CRASH_SAVED = 0,            /* report in crash.json; no file operation was in progress */
  XTS_CRASH_SAVED_FS_BUSY,        /* report in crash.json, but libfat was inside an update (the stick may need a check) */
  XTS_CRASH_NOT_SAVED_BUSY,       /* storage busy: nothing written */
  XTS_CRASH_NOT_SAVED_POISONED,   /* storage poisoned: nothing written */
  XTS_CRASH_NOT_SAVED_NOSLOT,     /* crash.json not armed (or its checksum is wrong): nothing written */
  XTS_CRASH_NOT_SAVED_WRITE,      /* a raw write failed */
  XTS_CRASH_THREAD,               /* PIR != 0: recorded in RAM */
};
int xts_crash_handle(uint32_t vector, const uint64_t *ctx40, uint64_t dsisr);
const char *xts_crash_outcome_name(int outcome);
extern char xts_crash_screen[XTS_CRASH_LINES][120];
extern volatile int xts_crash_outcome;    /* last outcome (-1 none) */
void xts_crash_log_note(const char *line);   /* run.log ring (last XTS_LOGRING lines) for the report */

/* report files (slot.c) */
int xts_slots_arm(const char *rundir, const char *run_id, int boot);   /* (re)create, flush, locate; 0 armed */
int xts_slots_armed(void);                /* 1 armed and the RAM metadata checksum is right (crash-path safe) */
const char *xts_slot_crash_path(void);
const char *xts_slot_progress_path(void);
const char *xts_slot_magic(void);
uint32_t xts_slot_sector_size(void);
uint32_t xts_slot_crash_sectors(void);
int xts_slot_crash_write(const uint8_t *img, uint32_t used);   /* crash-path safe; sectors 1..used-1, then 0 */
int xts_slot_crash_disarm(void);          /* rewrite the armed template over what xts_slot_crash_write wrote */
int xts_slot_crumb_write(void);           /* progress.bin from xts_crumb (timed); 0 ok */
void xts_slot_crumb_stats(char *out, size_t cap);    /* "kind n=.. avg .. ms max .. ms; ..." */
void xts_slot_crumb_block_stats(uint64_t *n, double *ms, double *max_ms);   /* since the last call */

/* the previous boot's report files (read at a resume, before re-arming) */
typedef struct xts_prev {
  int crash;                      /* crash.json filled (state crashed) for this run */
  char crash_kind[16];            /* "main" / "thread" */
  char crash_block[160], crash_case_id[48], crash_phase[16], crash_outcome[32];
  int crash_case_index, crash_case_rel;
  char crash_vector[8], crash_srr0[20];
  int crumb;                      /* progress.bin holds a breadcrumb of this run */
  char crumb_block[160], crumb_case_id[48], crumb_phase[16];
  int crumb_case_index, crumb_case_rel, crumb_every, crumb_boot;
  int nthr;                       /* thread entries in the report (thr<i>_pir present) */
  char *crash_text;               /* crash.json squeezed (no padding, no slot_NNN members), "" if none */
  char *crumb_text;               /* progress.bin squeezed */
  char how[200];                  /* one line for sysinfo.json / run.log */
} xts_prev_t;
int xts_prev_load(const char *rundir, const char *run_id, xts_prev_t *p);
/* the value of "<key>": in a squeezed report (string or number, copied as text); 0 found */
int xts_json_get(const char *json, const char *key, char *out, size_t cap);
/* copy JSON text without insignificant whitespace and without "slot_NNN" members; returns the length */
size_t xts_json_squeeze(const char *in, size_t n, char *out, size_t cap);
extern char xts_prev_boot_how[200];

/* deliberate crash for the self.crash family (plat): console executes tw 31,0,0 from C outside every trap range (does not
 * return if the crash path works; returns 0 if it did). Host: -1 (not simulated) unless xts_host_crash_sim is set (host
 * tests in a forked boot): then the crash path runs (xts_crash_handle: crash.json) and the process _exit(9)s */
int xts_plat_self_crash(void);
extern int xts_host_crash_sim;
#ifdef XTS_HOST
/* host image mode: called before every device read/write (tests: crash inside a storage operation) */
extern void (*xts_host_disc_hook)(int write, uint32_t sector, uint32_t count);
void xts_host_crash_screen(void);         /* the crash path's screen lines -> stderr */
#endif
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
/* file-operation guard: non-zero while the core/plat is inside libfat (crash path: "the stick may hold an unfinished
 * update") */
extern volatile int xts_fat_busy;
/* 1 while a block's cases run (main.c sets it after the block-start boundary, clears it before the block end). No file
 * may be written then: every sink/journal/plat file operation refuses (returns -1) and counts a guard violation
 * (run.log "BUG: ...", summary.json "guard_violations"). Only the raw breadcrumb/crash-report writes happen. */
extern volatile int xts_cases_running;
/* 1 (and the violation counted + logged) if a file operation `what` on `path` is attempted while cases run */
int xts_write_refused(const char *what, const char *path);
unsigned xts_guard_violations(void);
/* write/open/close/fsync/mkdir error accounting (run.log, screen, summary.json "write_errors") */
void xts_io_error(const char *what, const char *path);
unsigned xts_io_error_count(void);
/* 1 = storage poisoned (xts_plat_storage_status): every sink/journal file operation is refused from then on; the first
 * call puts "STORAGE ERROR ..." on screen (and in the run.log buffer). Not crash-path safe the first time. */
int xts_storage_dead(void);
/* screen: errors and halts (screen + run.log); the status line is rate-limited to once per second */
void xts_screen_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void xts_status_line(int force, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
const uint32_t *xts_sink_status_counts(void);               /* records per status written this boot */
void xts_sink_flush_block(const char *key);                 /* xts_sink_boundary + per-block flush time in run.log */
/* block boundary: buffered output -> files (fwrite, fflush, fsync) -> xts_plat_storage_flush (fatFlush: FAT copies,
 * FSInfo, every dirty page). 0 ok */
int xts_sink_boundary(void);
size_t xts_sink_pending(void);       /* largest buffered (unwritten) amount of any sink file, bytes */
#define XTS_SINK_HIGH_WATER (1536u << 10)   /* main.c ends a block's chunk early (flush at a case boundary) above this */
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
  int retry;                      /* RETRY seen: the block re-runs with a breadcrumb before every case */
  int part_k;                     /* PART k=<n>: cases 0..n-1 of the open block are done and on disk (chunk flush) */
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
  char report_tb[24];             /* last "REPORT tb=<t>": the crash.json report with that "tb" was already ingested */
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
int xts_journal_retry(xts_journal_t *j, const char *key);                 /* RETRY <key> */
int xts_journal_part(xts_journal_t *j, const char *key, int k, uint64_t seq);   /* PART <key> k=<k> seq=<seq> */
int xts_journal_raw(xts_journal_t *j, const char *line, size_t n);       /* append + fflush + fsync */
int xts_journal_report(xts_journal_t *j, uint64_t tb);                    /* REPORT tb=<tb> */

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
