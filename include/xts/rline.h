/* XTS - reading results.jsonl lines back: the parser for family .finish hooks (and any other consumer).
 *
 *   static int cb(const char *line, void *ud) {
 *     xts_rline_t l;
 *     if (xts_rline_parse(line, &l) < 0 || xts_rline_row(&l) != my_row || strcmp(l.metric, "penalty")) return 0;
 *     double err = xts_rline_num(&l, "penalty_err");              // number member of the detail (NaN if absent / null)
 *     char who[24]; xts_rline_str(&l, "verdict", who, sizeof who); // string member of the detail
 *     char oc[16]; const char *o = xts_rline_cond(&l, "other_cores", oc, sizeof oc);   // one axis value, NULL if absent
 *     int cs = xts_condtab_add(&tab, &l);                          // small id of the record's condition vector
 *     return 0;
 *   }
 *   xts_results_foreach("family", cb, NULL);
 *   ... xts_emit_for_cond(ctx, tab.text[cs], &r);    // a finish record carrying the condition vector it summarises
 *
 * The parser is a small JSON scanner, not a format guess: top-level members are found at depth 1 in any order, strings
 * may contain escaped quotes / backslashes / \uXXXX (decoded into the fixed buffers, truncated if too long), and a
 * "detail" member holding nested objects or arrays is skipped as a whole (xts_rline_num/str look at the detail's own
 * members only, never at members of nested objects). Lines are complete JSON objects of at most 32 KB (see
 * xts_results_foreach); nothing is allocated. Members the line lacks keep their defaults (empty string, NaN, row -1). */
#ifndef XTS_RLINE_H
#define XTS_RLINE_H
#include <stdint.h>
#include <xts/cond.h>
#include <xts/record.h>

#define XTS_RL_COND 640        /* longest "cond" object text (members only) a line may carry; longer lines do not parse */
#define XTS_CONDTAB_MAX 64

typedef struct xts_rline {
  char family[48];
  char case_id[96];
  char metric[32];
  char unit[24];
  char block[96];            /* scheduler block key, e.g. "r2:bypass.matrix:-:c0" */
  int status;                /* enum xts_status */
  double value;              /* NaN for null / absent */
  int row;                   /* profile row from the block key "r<row>:..."; -1 if the block has no such prefix */
  char cond[XTS_RL_COND];    /* members of the "cond" object (no braces), e.g. "sf":"64","sibling":"idle" */
  uint64_t condh;            /* hash of cond */
  const char *detail;        /* the detail object text ("{...}", "{}" if the line has none), inside the line: valid only
                              * while the line is (during the xts_results_foreach callback) */
  int detail_len;
} xts_rline_t;

/* Parse one results.jsonl line. 0 if it is a record (case_id, metric, a known status and a cond object present), else <0
 * (not a record, malformed, or the cond object is longer than XTS_RL_COND). Does not filter on family or row. */
int xts_rline_parse(const char *line, xts_rline_t *o);
int xts_rline_row(const xts_rline_t *l);                               /* l->row */
/* Members of the detail object (depth 1 only). num: NaN if absent, null or not a number. str: 1 and the decoded text in
 * buf (truncated to n-1 chars) if the member is a string, else 0 and buf[0] = 0. */
double xts_rline_num(const xts_rline_t *l, const char *detail_key);
int xts_rline_str(const xts_rline_t *l, const char *detail_key, char *buf, int n);
/* Value name of one axis in the record's cond object, copied into buf; buf, or NULL if the axis is absent. */
const char *xts_rline_cond(const xts_rline_t *l, const char *axis, char *buf, int n);

/* The distinct condition vectors seen in the scanned lines (by their cond text). */
typedef struct xts_condtab {
  int n;
  uint64_t h[XTS_CONDTAB_MAX];
  char text[XTS_CONDTAB_MAX][XTS_RL_COND];
} xts_condtab_t;
/* slot index of l's condition vector (added if new); -1 if the table is full. Zero the table (memset) before first use. */
int xts_condtab_add(xts_condtab_t *t, const xts_rline_t *l);
/* A copy of `base` with the axis values named in cond-object members `cond_text` (axes/values it does not name or the
 * registry does not know keep base's value). */
void xts_cond_from_text(const xts_condvec_t *base, const char *cond_text, xts_condvec_t *out);
/* Emit through a copy of ctx whose condition vector is cond_text's (so a finish record carries the vector it summarises);
 * ctx->nrecords is updated. */
void xts_emit_for_cond(struct xts_ctx *ctx, const char *cond_text, const xts_record_t *r);
#endif
