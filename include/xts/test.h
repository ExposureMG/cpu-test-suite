/* XTS - test families.
 *
 * A family enumerates cases once, then the scheduler runs every case for every combination of the condition-axis values
 * the profile asks for (restricted to the axes the family lists in .axes), in blocks. Adding a family = adding
 * src/tests/<family>.c; nothing else changes (registration is by linker section).
 *
 *   static int my_enum(xts_caselist_t *cl) {
 *     xts_case_t *c = xts_case_add(cl, "addx#0");  c->insn = XI_addx;  c->arg[0] = 0;   // up to 4 integer args
 *     return 0;
 *   }
 *   static int my_run(const xts_case_t *c, xts_ctx_t *ctx) {
 *     xts_exec_t *x = xts_exec_new();            // or a static one
 *     xts_case_init(x);                          // canary baseline + env (sf, fpscr, vscr)
 *     ... set inputs, xts_exec_word(x, word), compare with the model ...
 *     xts_emit_status(ctx, XTS_ST_PASS, NULL);
 *     return 0;                                  // <0 = the family itself failed (logged, case recorded as fail)
 *   }
 *   XTS_FAMILY(my_family, .kind = "semantic", .axes = "sf", .enumerate = my_enum, .run = my_run,
 *              .desc = "what it checks");
 *
 * Name: the FIRST '_' of the id becomes '.', so XTS_FAMILY(exec_ppc, ...) is "exec.ppc", XTS_FAMILY(lat_chain, ...) is
 * "lat.chain", XTS_FAMILY(ucode, ...) is "ucode". Kinds: self, semantic, timing, sched, mem, smt, calib.
 */
#ifndef XTS_TEST_H
#define XTS_TEST_H
#include <stdint.h>
#include <xts/cond.h>
#include <xts/xts.h>

typedef struct xts_case {
  char id[48];          /* stable case id (unique within the family) */
  int insn;             /* XI_* id or -1 */
  uint32_t flags;       /* family-defined (e.g. XTS_RC|XTS_OE variant) */
  int64_t arg[4];       /* family-defined */
  void *data;           /* family-defined, owned by the family (allocated in enumerate, never freed: one run) */
} xts_case_t;

typedef struct xts_caselist {
  xts_case_t *v;
  int n, cap;
} xts_caselist_t;
/* Append a case (insn = -1, everything else 0); NULL if out of memory. The id is copied (truncated to 47 chars). */
xts_case_t *xts_case_add(xts_caselist_t *cl, const char *id);
xts_case_t *xts_case_addf(xts_caselist_t *cl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

typedef struct xts_family xts_family_t;

/* Handed to a family's finish() (ctx->fin; NULL in run()). See XTS_FAMILY .finish below. */
typedef struct xts_finish_info {
  int row;                 /* profile row */
  int blocks;              /* the family's case blocks in this row (all axis combinations) */
  int ran_this_boot;       /* of those, executed in this boot: in-memory state built by run() covers only these */
  int done_earlier;        /* completed in an earlier boot (resume): their records are on disk (xts_results_foreach) */
  int quarantined;         /* blocks with a hang/crash (some or all of their cases were given up) */
  int skipped;             /* blocks skipped (an axis could not be applied / family setup failed) */
  int complete;            /* 1 if ran_this_boot == blocks and nothing was quarantined or skipped */
} xts_finish_info_t;

/* Per-case context handed to run(). Everything a record needs is in here. */
typedef struct xts_ctx {
  const xts_family_t *fam;
  const xts_case_t *c;
  int case_index;              /* index in the family's case list */
  const xts_condvec_t *cond;   /* condition vector of the current block */
  uint64_t seed;               /* deterministic: f(profile seed, family, case id, condition values, repeat) */
  xts_rng_t rng;               /* seeded with `seed` */
  int repeat;                  /* 0 .. repeats-1 (profile row) */
  const char *block_key;
  int host;                    /* 1 on the PC (host dry run / host tests): nothing really executes */
  int nrecords;                /* records emitted for this case so far */
  const xts_finish_info_t *fin;  /* only in finish(): what the row did (else NULL) */
} xts_ctx_t;

struct xts_family {
  const char *name;            /* the XTS_FAMILY id ("exec_ppc"); use xts_family_name() for the dotted name */
  const char *kind;            /* self | semantic | timing | sched | mem | smt | calib */
  const char *axes;            /* comma-separated axis names the family is sensitive to ("sf,fpscr_rn"), NULL = none */
  int (*enumerate)(xts_caselist_t *cl);
  int (*run)(const xts_case_t *c, xts_ctx_t *ctx);
  int (*setup)(void);          /* optional, once before the family's first block; <0 = skip the family */
  void (*teardown)(void);      /* optional, after the family's last block */
  int block_cases;             /* cases per scheduler block (0 = 64). Smaller blocks = finer hang quarantine. */
  const char *desc;
  /* optional: summaries after the family's LAST block of a profile row (all axis combinations of that row).
   * It runs as its own journal block (key "r<row>:<family>:finish", condition vector = defaults), so it runs exactly
   * once per row and run: after a resume it still runs (once), even when every case block finished in an earlier boot.
   * ctx: c = a synthetic case (id "finish", insn -1), case_index -1, repeat 0, fin = what the row did. Records it emits
   * belong to the finish block; set case_id per record. In-memory state collected by run() covers only the blocks of
   * THIS boot (fin->ran_this_boot): recompute from xts_results_foreach() or emit a record noting what is missing.
   * Called after setup() (setup runs first if this boot has not called it yet); not called if setup() failed.
   * A hang/crash inside finish() is recorded like any block (status hang/crash, case "*") and not retried. <0 = error
   * (recorded as fail if nothing was emitted). */
  int (*finish)(xts_ctx_t *ctx);
};

/* Registration: one pointer per family in linker section "xts_fam". */
#define XTS_FAMILY(id, ...)                                                                            \
  static const xts_family_t xts_fam_##id = { .name = #id, __VA_ARGS__ };                               \
  static const xts_family_t *const xts_fam_ptr_##id __attribute__((used, section("xts_fam"), aligned(sizeof(void *)))) = &xts_fam_##id

/* registry access (src/core/registry.c) */
int xts_family_count(void);
const xts_family_t *xts_family_get(int i);
const char *xts_family_name(const xts_family_t *f);     /* normalised name */
const xts_family_t *xts_family_find(const char *name);
int xts_family_has_axis(const xts_family_t *f, const char *axis);

/* Results of this run so far (every boot), for finish() summaries: calls cb for every results.jsonl line whose family is
 * `family` (NULL = all), after flushing the current block's buffered records. cb returns non-zero to stop. Returns the
 * number of lines passed to cb, or -1 if there is no run directory (host unit tests without a run). Lines are complete
 * JSON objects (no newline), at most 32 KB. */
int xts_results_foreach(const char *family, int (*cb)(const char *json_line, void *ud), void *ud);
const char *xts_run_dir(void);   /* "<root>/xts/<tag>/<run_id>" of the current run, "" outside a run */
#endif
