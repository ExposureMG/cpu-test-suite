/* Tiny JSON text builder (xts/json.h). On overflow the last item (and its key) is rolled back, every open container is
 * still closed, and j->truncated is set: the text stays valid JSON. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <xts/json.h>

void xj_init(xts_json_t *j, char *buf, int cap)
{
  memset(j, 0, sizeof *j);
  j->buf = buf;
  j->cap = cap;
  if (cap > 0) buf[0] = 0;
}

/* position to roll back to if the item about to be written does not fit */
static int item_begin(xts_json_t *j)
{
  int pos;
  if (j->after_key) {
    pos = j->key_mark;
    j->after_key = 0;
    return pos;
  }
  pos = j->len;
  if (j->depth > 0 && j->count[j->depth - 1] && !j->truncated) {
    if (j->len + 1 + j->depth + 1 <= j->cap) j->buf[j->len++] = ',';
    else j->truncated = 1;
  }
  return pos;
}

static void rollback(xts_json_t *j, int pos)
{
  j->len = pos;
  if (j->cap > 0) j->buf[pos] = 0;
  j->truncated = 1;
}

/* append n bytes, keeping room for every open container's closer + NUL */
static int put(xts_json_t *j, const char *s, int n)
{
  if (j->truncated) return -1;
  if (j->len + n + j->depth + 1 > j->cap) return -1;
  memcpy(j->buf + j->len, s, n);
  j->len += n;
  j->buf[j->len] = 0;
  return 0;
}

static void item_done(xts_json_t *j)
{
  if (j->depth > 0) j->count[j->depth - 1] = 1;
}

static void open_c(xts_json_t *j, char o, char c)
{
  int pos = item_begin(j);
  if (j->truncated || j->depth >= XJ_MAXDEPTH) { rollback(j, pos); j->phantom++; return; }
  j->closer[j->depth] = c;
  j->count[j->depth] = 0;
  j->depth++;                         /* reserve the closer before writing the opener */
  if (put(j, &o, 1) < 0) { j->depth--; rollback(j, pos); j->phantom++; return; }
  if (j->depth >= 2) j->count[j->depth - 2] = 1;   /* the parent now has an element */
}

void xj_obj(xts_json_t *j) { open_c(j, '{', '}'); }
void xj_arr(xts_json_t *j) { open_c(j, '[', ']'); }

void xj_end(xts_json_t *j)
{
  if (j->phantom > 0) { j->phantom--; return; }
  if (j->depth <= 0) return;
  if (j->after_key) { rollback(j, j->key_mark); j->after_key = 0; }   /* dangling key */
  j->depth--;
  if (j->len + 2 <= j->cap) {         /* always true: put() kept the room */
    j->buf[j->len++] = j->closer[j->depth];
    j->buf[j->len] = 0;
  }
  item_done(j);
}

int xj_escape(char *out, int cap, const char *s)
{
  int o = 0;
  for (; s && *s; s++) {
    char tmp[8];
    const char *r = tmp;
    unsigned char ch = (unsigned char)*s;
    if (ch == '"') r = "\\\"";
    else if (ch == '\\') r = "\\\\";
    else if (ch == '\n') r = "\\n";
    else if (ch == '\r') r = "\\r";
    else if (ch == '\t') r = "\\t";
    else if (ch < 0x20) snprintf(tmp, sizeof tmp, "\\u%04x", ch);
    else { tmp[0] = (char)ch; tmp[1] = 0; }
    int n = (int)strlen(r);
    if (o + n >= cap) break;
    memcpy(out + o, r, n);
    o += n;
  }
  if (cap > 0) out[o < cap ? o : cap - 1] = 0;
  return o;
}

void xj_key(xts_json_t *j, const char *k)
{
  if (j->after_key) rollback(j, j->key_mark), j->after_key = 0;   /* key without value */
  int pos = item_begin(j);
  char e[128];
  xj_escape(e, sizeof e, k);
  if (put(j, "\"", 1) < 0 || put(j, e, (int)strlen(e)) < 0 || put(j, "\":", 2) < 0) { rollback(j, pos); return; }
  j->key_mark = pos;
  j->after_key = 1;
}

static void scalar(xts_json_t *j, const char *a, const char *b, const char *c)
{
  int pos = item_begin(j);
  if (put(j, a, (int)strlen(a)) < 0 || (b && put(j, b, (int)strlen(b)) < 0) || (c && put(j, c, (int)strlen(c)) < 0)) {
    rollback(j, pos);
    return;
  }
  item_done(j);
}

void xj_str(xts_json_t *j, const char *s)
{
  char e[1024];
  xj_escape(e, sizeof e, s ? s : "");
  scalar(j, "\"", e, "\"");
}

void xj_int(xts_json_t *j, int64_t v) { char b[32]; snprintf(b, sizeof b, "%lld", (long long)v); scalar(j, b, NULL, NULL); }
void xj_uint(xts_json_t *j, uint64_t v) { char b[32]; snprintf(b, sizeof b, "%llu", (unsigned long long)v); scalar(j, b, NULL, NULL); }
void xj_hex(xts_json_t *j, uint64_t v, int digits)
{
  char b[40];
  if (digits < 1 || digits > 16) digits = 16;
  snprintf(b, sizeof b, "\"%0*llX\"", digits, (unsigned long long)v);
  scalar(j, b, NULL, NULL);
}
void xj_double(xts_json_t *j, double v)
{
  char b[40];
  if (isnan(v) || isinf(v)) { scalar(j, "null", NULL, NULL); return; }
  snprintf(b, sizeof b, "%.10g", v);
  scalar(j, b, NULL, NULL);
}
void xj_bool(xts_json_t *j, int v) { scalar(j, v ? "true" : "false", NULL, NULL); }
void xj_null(xts_json_t *j) { scalar(j, "null", NULL, NULL); }
void xj_raw(xts_json_t *j, const char *json) { scalar(j, json && *json ? json : "null", NULL, NULL); }

void xj_members(xts_json_t *j, const char *obj)
{
  if (!obj) return;
  while (*obj == ' ' || *obj == '\n' || *obj == '\t') obj++;
  int n = (int)strlen(obj);
  while (n && (obj[n - 1] == ' ' || obj[n - 1] == '\n' || obj[n - 1] == '\t')) n--;
  if (n < 2 || obj[0] != '{' || obj[n - 1] != '}') return;
  const char *b = obj + 1;
  int m = n - 2;
  while (m && (*b == ' ' || *b == '\n' || *b == '\t')) b++, m--;
  if (!m) return;                                   /* "{}" */
  if (j->depth <= 0 || j->closer[j->depth - 1] != '}') return;   /* only inside an object */
  int pos = item_begin(j);
  if (put(j, b, m) < 0) { rollback(j, pos); return; }
  item_done(j);
}

const char *xj_finish(xts_json_t *j)
{
  while (j->phantom > 0 || j->depth > 0) xj_end(j);
  return j->buf;
}
