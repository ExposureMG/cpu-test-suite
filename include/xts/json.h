/* XTS - tiny JSON text builder (no allocation). Used for record `detail` objects, sysinfo.json, summary.json.
 *
 *   char buf[512]; xts_json_t j; xj_init(&j, buf, sizeof buf);
 *   xj_obj(&j); xj_kv_str(&j, "why", "gpr mismatch"); xj_kv_hex(&j, "want", 0x1234, 16); xj_end(&j);
 *   -> {"why":"gpr mismatch","want":"0000000000001234"}
 *
 * If the buffer fills up the text is closed cleanly where possible and j.truncated is set; the record sink then adds
 * "truncated":true to the record so a cut-off result is never silent.
 */
#ifndef XTS_JSON_H
#define XTS_JSON_H
#include <stdint.h>

#define XJ_MAXDEPTH 16
typedef struct xts_json {
  char *buf;
  int cap, len;
  int truncated;
  int depth;
  uint8_t count[XJ_MAXDEPTH];   /* elements written at each depth (for commas) */
  char closer[XJ_MAXDEPTH];
  int after_key, key_mark;
  int phantom;                  /* containers that could not be opened (truncated): their xj_end is ignored */
} xts_json_t;

void xj_init(xts_json_t *j, char *buf, int cap);
void xj_obj(xts_json_t *j);                /* { */
void xj_arr(xts_json_t *j);                /* [ */
void xj_end(xts_json_t *j);                /* } or ] */
void xj_key(xts_json_t *j, const char *k);
void xj_str(xts_json_t *j, const char *s);
void xj_int(xts_json_t *j, int64_t v);
void xj_uint(xts_json_t *j, uint64_t v);
void xj_hex(xts_json_t *j, uint64_t v, int digits);     /* "00001234" (quoted, upper case, zero-padded) */
void xj_double(xts_json_t *j, double v);                /* NaN/inf -> null */
void xj_bool(xts_json_t *j, int v);
void xj_null(xts_json_t *j);
void xj_raw(xts_json_t *j, const char *json);           /* pre-rendered JSON value */
/* copy the members of a pre-rendered JSON object ("{\"a\":1,\"b\":2}") into the open object (no-op for "{}" / non-objects) */
void xj_members(xts_json_t *j, const char *obj);
static inline void xj_kv_str(xts_json_t *j, const char *k, const char *s) { xj_key(j, k); xj_str(j, s); }
static inline void xj_kv_int(xts_json_t *j, const char *k, int64_t v) { xj_key(j, k); xj_int(j, v); }
static inline void xj_kv_uint(xts_json_t *j, const char *k, uint64_t v) { xj_key(j, k); xj_uint(j, v); }
static inline void xj_kv_hex(xts_json_t *j, const char *k, uint64_t v, int digits) { xj_key(j, k); xj_hex(j, v, digits); }
static inline void xj_kv_double(xts_json_t *j, const char *k, double v) { xj_key(j, k); xj_double(j, v); }
static inline void xj_kv_bool(xts_json_t *j, const char *k, int v) { xj_key(j, k); xj_bool(j, v); }
/* finish: closes any open containers, returns buf */
const char *xj_finish(xts_json_t *j);
/* escape s into a JSON string body (no quotes) */
int xj_escape(char *out, int cap, const char *s);
#endif
