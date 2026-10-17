/* XTS - results.jsonl line reader (include/xts/rline.h). A depth-aware scanner over one complete JSON object. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include <xts/rline.h>

static const char *ws(const char *p, const char *e)
{
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
  return p;
}

/* p at an opening quote: the position after the closing quote, NULL if unterminated */
static const char *skip_str(const char *p, const char *e)
{
  if (p >= e || *p != '"') return NULL;
  for (p++; p < e; p++) {
    if (*p == '\\') p++;
    else if (*p == '"') return p + 1;
  }
  return NULL;
}

/* the position after the value starting at p (string, object/array incl. nested, or a bare token); NULL if malformed */
static const char *skip_value(const char *p, const char *e)
{
  p = ws(p, e);
  if (p >= e) return NULL;
  if (*p == '"') return skip_str(p, e);
  if (*p == '{' || *p == '[') {
    int depth = 0;
    for (; p < e; p++) {
      if (*p == '"') {
        p = skip_str(p, e);
        if (!p) return NULL;
        p--;
      } else if (*p == '{' || *p == '[') depth++;
      else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
    }
    return NULL;
  }
  while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
  return p;
}

/* decode the JSON string at p (opening quote) into out (truncated to n-1 chars); *end = after the closing quote.
 * Returns the decoded length, or -1 if p is not a well-formed string. */
static int str_decode(const char *p, const char *e, char *out, int n, const char **end)
{
  const char *q = skip_str(p, e);
  int o = 0;
  if (!q) return -1;
  if (end) *end = q;
#define PUT(c) do { if (n > 0 && o < n - 1) out[o] = (char)(c); o++; } while (0)
  for (p++; p < q - 1; p++) {
    if (*p != '\\') { PUT(*p); continue; }
    p++;
    switch (*p) {
      case 'n': PUT('\n'); break;
      case 'r': PUT('\r'); break;
      case 't': PUT('\t'); break;
      case 'b': PUT('\b'); break;
      case 'f': PUT('\f'); break;
      case 'u': {
        unsigned cp = 0;
        int k;
        for (k = 1; k <= 4 && p + k < q - 1; k++) {
          char h = p[k];
          cp = cp * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
        }
        p += k - 1;
        if (cp >= 0xD800 && cp < 0xE000) cp = '?';       /* surrogates: not produced by the sink */
        if (cp < 0x80) PUT(cp);
        else if (cp < 0x800) { PUT(0xC0 | (cp >> 6)); PUT(0x80 | (cp & 0x3F)); }
        else { PUT(0xE0 | (cp >> 12)); PUT(0x80 | ((cp >> 6) & 0x3F)); PUT(0x80 | (cp & 0x3F)); }
        break;
      }
      default: PUT(*p); break;                            /* \" \\ \/ and anything else: the character itself */
    }
  }
#undef PUT
  if (n > 0) out[o < n ? o : n - 1] = 0;
  return o;
}

/* next member of the object body [*pp, e) (no braces): key decoded into key (truncated), *val = start of its value,
 * *vend = after it. 0 at the end or on malformed input. */
static int next_member(const char **pp, const char *e, char *key, int kn, const char **val, const char **vend)
{
  const char *p = ws(*pp, e);
  while (p < e && *p == ',') p = ws(p + 1, e);
  if (p >= e || *p != '"') return 0;
  const char *q;
  if (str_decode(p, e, key, kn, &q) < 0) return 0;
  q = ws(q, e);
  if (q >= e || *q != ':') return 0;
  q = ws(q + 1, e);
  const char *ve = skip_value(q, e);
  if (!ve) return 0;
  *val = q;
  *vend = ve;
  *pp = ve;
  return 1;
}

/* the value of member `want` in the object body [b, e) at depth 1; NULL if absent */
static const char *find_member(const char *b, const char *e, const char *want, const char **vend)
{
  char key[64];
  const char *v, *ve;
  if (strlen(want) >= sizeof key - 1) return NULL;
  while (next_member(&b, e, key, sizeof key, &v, &ve))
    if (!strcmp(key, want)) { if (vend) *vend = ve; return v; }
  return NULL;
}

static double num_at(const char *v)
{
  return v && (*v == '-' || (*v >= '0' && *v <= '9')) ? strtod(v, NULL) : NAN;
}

/* ------------------------------------------------------------------------------------------------------- the line */
int xts_rline_parse(const char *line, xts_rline_t *o)
{
  memset(o, 0, sizeof *o);
  o->status = -1;
  o->value = NAN;
  o->row = -1;
  o->detail = "{}";
  o->detail_len = 2;
  if (!line) return -1;
  const char *e = line + strlen(line), *p = ws(line, e), *v, *ve;
  if (p >= e || *p != '{') return -1;
  p++;
  char key[24];
  int have_case = 0, have_metric = 0, have_cond = 0;
  while (next_member(&p, e, key, sizeof key, &v, &ve)) {
    if (!strcmp(key, "family")) str_decode(v, ve, o->family, sizeof o->family, NULL);
    else if (!strcmp(key, "case_id")) have_case = str_decode(v, ve, o->case_id, sizeof o->case_id, NULL) >= 0;
    else if (!strcmp(key, "metric")) have_metric = str_decode(v, ve, o->metric, sizeof o->metric, NULL) >= 0;
    else if (!strcmp(key, "unit")) str_decode(v, ve, o->unit, sizeof o->unit, NULL);
    else if (!strcmp(key, "block")) str_decode(v, ve, o->block, sizeof o->block, NULL);
    else if (!strcmp(key, "status")) {
      char st[16];
      o->status = str_decode(v, ve, st, sizeof st, NULL) >= 0 ? xts_status_from_name(st) : -1;
    } else if (!strcmp(key, "value")) o->value = num_at(v);
    else if (!strcmp(key, "cond")) {
      if (*v != '{' || ve - v - 2 >= XTS_RL_COND) return -1;
      memcpy(o->cond, v + 1, (size_t)(ve - v - 2));
      o->cond[ve - v - 2] = 0;
      have_cond = 1;
    } else if (!strcmp(key, "detail") && *v == '{') {
      o->detail = v;
      o->detail_len = (int)(ve - v);
    }
  }
  if (!have_case || !have_metric || !have_cond || o->status < 0) return -1;
  o->condh = xts_hash_str(0xC04D, o->cond);
  if (o->block[0] == 'r' && o->block[1] >= '0' && o->block[1] <= '9') {
    char *end;
    long r = strtol(o->block + 1, &end, 10);
    if (*end == ':' && r < 1000000) o->row = (int)r;
  }
  return 0;
}

int xts_rline_row(const xts_rline_t *l) { return l->row; }

static const char *detail_member(const xts_rline_t *l, const char *key)
{
  if (!l->detail || l->detail_len < 2) return NULL;
  return find_member(l->detail + 1, l->detail + l->detail_len - 1, key, NULL);
}

double xts_rline_num(const xts_rline_t *l, const char *detail_key) { return num_at(detail_member(l, detail_key)); }

int xts_rline_str(const xts_rline_t *l, const char *detail_key, char *buf, int n)
{
  const char *v = detail_member(l, detail_key);
  if (n > 0) buf[0] = 0;
  return v && *v == '"' && str_decode(v, l->detail + l->detail_len, buf, n, NULL) >= 0;
}

/* value of axis `axis` in the cond-object members text */
static const char *cond_value(const char *cond_text, const char *axis, char *buf, int n)
{
  const char *e = cond_text + strlen(cond_text), *ve;
  const char *v = find_member(cond_text, e, axis, &ve);
  if (!v || *v != '"' || str_decode(v, ve, buf, n, NULL) < 0) return NULL;
  return buf;
}

const char *xts_rline_cond(const xts_rline_t *l, const char *axis, char *buf, int n)
{
  return cond_value(l->cond, axis, buf, n);
}

/* --------------------------------------------------------------------------------------------- condition tables */
int xts_condtab_add(xts_condtab_t *t, const xts_rline_t *l)
{
  for (int i = 0; i < t->n; i++)
    if (t->h[i] == l->condh && !strcmp(t->text[i], l->cond)) return i;
  if (t->n >= XTS_CONDTAB_MAX) return -1;
  t->h[t->n] = l->condh;
  snprintf(t->text[t->n], XTS_RL_COND, "%s", l->cond);
  return t->n++;
}

void xts_cond_from_text(const xts_condvec_t *base, const char *cond_text, xts_condvec_t *out)
{
  *out = *base;
  for (int i = 0; i < out->n; i++) {
    char val[48];
    if (!cond_value(cond_text, out->axis[i]->name, val, sizeof val)) continue;
    int v = xts_axis_value_index(out->axis[i], val);
    if (v >= 0) out->value[i] = v;
  }
}

void xts_emit_for_cond(xts_ctx_t *ctx, const char *cond_text, const xts_record_t *r)
{
  xts_condvec_t cv;
  xts_ctx_t c2 = *ctx;
  xts_cond_from_text(ctx->cond, cond_text, &cv);
  c2.cond = &cv;
  xts_emit(&c2, r);
  ctx->nrecords = c2.nrecords;
}
