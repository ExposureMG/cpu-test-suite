/* XTS - result records: the one schema of every REC message (one results.jsonl line); the receiver (tools/xts_recv.py)
 * derives results.csv, failures.jsonl and summary.json from it.
 *
 * Fields (results.jsonl, one JSON object per line):
 *   run_id   string   the run nonce, 16 hex digits (the receiver files the run under <out>/<console_tag>/<run_id>/)
 *   seq      int      record number within the run
 *   family   string   test family name ("exec.ppc")
 *   case_id  string   family-defined, stable across runs ("addx.oe.rc#3")
 *   mnem     string   JSON mnemonic or "" (non-instruction cases)
 *   word     string   8 hex digits of the instruction word under test, or "" if none
 *   cond     object   the FULL condition vector: every registered axis -> value name (defaults for axes not applied)
 *   block    string   scheduler block key the record was produced in
 *   metric   string   what `value` is ("result", "latency", "rthroughput", "ticks", ...)
 *   value    number   or null
 *   unit     string   "cycles", "ticks", "bool", "" ...
 *   n, min, median, mad, fit_r2   numbers (null when not applicable)
 *   status   string   pass | fail | trap | skip | noisy | nonlinear | regressed | hang | crash | info   (xts_status_worst ranks them)
 *   detail   object   family-defined; semantic mismatches attach in/out/expected state diffs here
 *   truncated true    only present if the detail JSON did not fit
 * results.csv has the same columns (cond as "k=v;k=v", detail omitted). failures.jsonl gets every record whose status is
 * fail, hang or crash (with detail). summary.json aggregates status counts per family and per axis value.
 */
#ifndef XTS_RECORD_H
#define XTS_RECORD_H
#include <stdint.h>

enum xts_status {
  XTS_ST_PASS = 0, XTS_ST_FAIL, XTS_ST_TRAP, XTS_ST_SKIP, XTS_ST_NOISY, XTS_ST_REGRESSED, XTS_ST_HANG, XTS_ST_CRASH,
  XTS_ST_INFO,
  XTS_ST_NONLINEAR,   /* timing: repeatable, but the cost per unit depends on the depth (xts/timing.h quality gate) */
  XTS_ST_COUNT
};
const char *xts_status_name(int st);
/* The one status combiner: the more severe of a and b. Ranking (most severe first):
 *   crash > hang > fail > trap > noisy > nonlinear > regressed > skip > info > pass
 * A value outside the enum is ignored (the other one is returned). Use it everywhere a record's status is derived from
 * several measurements/checks; never write a local ranking. */
int xts_status_worst(int a, int b);
int xts_status_rank(int st);      /* 0 (pass) .. 9 (crash); -1 for a value outside the enum */
int xts_status_from_name(const char *name);   /* -1 if unknown */

/* ---- vocabularies (checked by xts_emit in host builds: unknown names are logged once and flagged in the detail as
 * "vocab_warning", the record is still written) ----
 * metric: latency rthroughput issue_cost penalty extra overlap cycles ratio bytes_per_cycle depth count result estimate
 *         tininess info                                      + anything registered with XTS_METRIC("name")
 * unit:   cycles ratio count bytes bytes_per_cycle entries words hz ticks ""   + XTS_UNIT("name") */
int xts_metric_known(const char *metric);
int xts_unit_known(const char *unit);
typedef struct xts_vocab_entry { const char *name; int is_unit; } xts_vocab_entry_t;
#define XTS_VOCAB_CAT2(a, b) a##b
#define XTS_VOCAB_CAT(a, b) XTS_VOCAB_CAT2(a, b)
#define XTS_VOCAB_REG_(str, unit)                                                                                   \
  static const xts_vocab_entry_t XTS_VOCAB_CAT(xts_vocab_e_, __LINE__) = { str, unit };                          \
  static const xts_vocab_entry_t *const XTS_VOCAB_CAT(xts_vocab_p_, __LINE__)                                    \
      __attribute__((used, section("xts_vocab"), aligned(sizeof(void *)))) = &XTS_VOCAB_CAT(xts_vocab_e_, __LINE__)
/* one per source line (the symbols are keyed on __LINE__) */
#define XTS_METRIC(name_str) XTS_VOCAB_REG_(name_str, 0)
#define XTS_UNIT(name_str) XTS_VOCAB_REG_(name_str, 1)

typedef struct xts_record {
  const char *case_id;      /* NULL -> the case's id */
  const char *mnem;         /* NULL -> the case's mnemonic (if any) */
  uint32_t word;
  int has_word;
  const char *metric;       /* default "result" */
  double value;             /* NaN -> null */
  const char *unit;         /* default "" */
  double n, min, median, mad, fit_r2;   /* NaN -> null */
  int status;               /* enum xts_status */
  const char *detail;       /* JSON object text or NULL (e.g. from xts_json_t) */
  int detail_truncated;     /* set from xts_json_t.truncated */
} xts_record_t;

/* A record with every numeric field NaN, metric "result", status pass. */
void xts_record_init(xts_record_t *r);

struct xts_ctx;
/* Emit a record for the current case (fills run_id, seq, family, cond, block). Sent as one REC message right away. */
void xts_emit(struct xts_ctx *ctx, const xts_record_t *r);
/* Shortcuts */
void xts_emit_status(struct xts_ctx *ctx, int status, const char *detail);    /* metric "result" */
void xts_emit_value(struct xts_ctx *ctx, const char *metric, double value, const char *unit, int status);
/* An informational record (status info) for the CURRENT BLOCK, usable where there is no case context: from an axis'
 * apply()/revert(), a hook, or a family's setup. ctx may be NULL: family, block key and condition vector are then the
 * block the core is running (set before the axes are applied). case_id NULL -> "block". value NaN -> null. unit NULL ->
 * "". detail = JSON object text or NULL. Records outside any block (before the first one) go to family "core". */
void xts_emit_block_info(struct xts_ctx *ctx, const char *case_id, const char *metric, double value, const char *unit,
                         const char *detail);
#endif
