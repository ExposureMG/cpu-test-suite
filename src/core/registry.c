/* Registry: families, axes and model handlers registered by linker section (XTS_FAMILY, XTS_AXIS, XTS_MODEL).
 *
 * Each registration macro puts ONE POINTER into a section named xts_fam / xts_axis / xts_model. The host link gets
 * __start_/__stop_ symbols from GNU ld automatically (C-identifier section names); the console link script
 * (tools/xts.lds) defines them explicitly and KEEPs the sections under --gc-sections. The symbols are declared weak so a
 * binary with no entry in a section still links.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"

extern const xts_family_t *const __start_xts_fam[] __attribute__((weak));
extern const xts_family_t *const __stop_xts_fam[] __attribute__((weak));
extern const xts_axis_t *const __start_xts_axis[] __attribute__((weak));
extern const xts_axis_t *const __stop_xts_axis[] __attribute__((weak));

#define MAX_FAMILIES 256
static const xts_family_t *fams[MAX_FAMILIES];
static char fam_names[MAX_FAMILIES][48];
static int nfams;
static const xts_axis_t *axes[XTS_MAX_AXES];
static int naxes;
static int inited;

xts_env_t xts_env;

static int cmp_fam(const void *a, const void *b)
{
  return strcmp(((const xts_family_t *const *)a)[0]->name, ((const xts_family_t *const *)b)[0]->name);
}

static int cmp_axis(const void *a, const void *b)
{
  const xts_axis_t *x = *(const xts_axis_t *const *)a, *y = *(const xts_axis_t *const *)b;
  if (x->order != y->order) return x->order < y->order ? -1 : 1;
  return strcmp(x->name, y->name);
}

static void normalise(const char *id, char *out, size_t n)
{
  snprintf(out, n, "%s", id);
  char *u = strchr(out, '_');
  if (u && !strchr(out, '.')) *u = '.';
}

int xts_registry_init(void (*print)(const char *))
{
  char msg[200];
  int bad = 0;
  if (inited) return 0;
  inited = 1;
  nfams = 0;
  if (__start_xts_fam && __stop_xts_fam)
    for (const xts_family_t *const *p = __start_xts_fam; p < __stop_xts_fam; p++) {
      if (!*p) continue;
      if (nfams >= MAX_FAMILIES) { if (print) print("registry: too many families"); bad++; break; }
      fams[nfams++] = *p;
    }
  qsort(fams, nfams, sizeof fams[0], cmp_fam);
  for (int i = 0; i < nfams; i++) {
    normalise(fams[i]->name, fam_names[i], sizeof fam_names[i]);
    if (!fams[i]->run || !fams[i]->enumerate) {
      snprintf(msg, sizeof msg, "registry: family %s has no run/enumerate", fam_names[i]);
      if (print) print(msg);
      bad++;
    }
    for (int k = 0; k < i; k++)
      if (!strcmp(fam_names[k], fam_names[i])) {
        snprintf(msg, sizeof msg, "registry: duplicate family %s", fam_names[i]);
        if (print) print(msg);
        bad++;
      }
  }
  naxes = 0;
  if (__start_xts_axis && __stop_xts_axis)
    for (const xts_axis_t *const *p = __start_xts_axis; p < __stop_xts_axis; p++) {
      if (!*p) continue;
      if (naxes >= XTS_MAX_AXES) { if (print) print("registry: too many axes"); bad++; break; }
      axes[naxes++] = *p;
    }
  qsort(axes, naxes, sizeof axes[0], cmp_axis);
  for (int i = 0; i < naxes; i++) {
    int nv = xts_axis_nvalues(axes[i]);
    if (nv < 1 || nv > XTS_MAX_AXIS_VALUES) {
      snprintf(msg, sizeof msg, "registry: axis %s has %d values (1..%d allowed)", axes[i]->name, nv, XTS_MAX_AXIS_VALUES);
      if (print) print(msg);
      bad++;
    }
    for (int k = 0; k < i; k++)
      if (!strcmp(axes[k]->name, axes[i]->name)) {
        snprintf(msg, sizeof msg, "registry: duplicate axis %s", axes[i]->name);
        if (print) print(msg);
        bad++;
      }
  }
  /* families may only name registered axes? No: an axis written later must be allowed. Just log unknown names. */
  for (int i = 0; i < nfams; i++) {
    const char *a = fams[i]->axes;
    while (a && *a) {
      char nm[40];
      int n = (int)strcspn(a, ",");
      snprintf(nm, sizeof nm, "%.*s", n < 39 ? n : 39, a);
      if (nm[0] && !xts_axis_find(nm)) {
        snprintf(msg, sizeof msg, "registry: family %s lists axis '%s', which is not registered (ignored)", fam_names[i], nm);
        if (print) print(msg);
      }
      a += n;
      if (*a == ',') a++;
    }
  }
  bad += xts_model_check(print);
  xts_env_reset();
  return bad;
}

int xts_family_count(void) { xts_registry_init(NULL); return nfams; }
const xts_family_t *xts_family_get(int i) { xts_registry_init(NULL); return (i >= 0 && i < nfams) ? fams[i] : NULL; }

const char *xts_family_name(const xts_family_t *f)
{
  xts_registry_init(NULL);
  for (int i = 0; i < nfams; i++)
    if (fams[i] == f) return fam_names[i];
  return f ? f->name : "?";
}

const xts_family_t *xts_family_find(const char *name)
{
  xts_registry_init(NULL);
  for (int i = 0; i < nfams; i++)
    if (!strcmp(fam_names[i], name) || !strcmp(fams[i]->name, name)) return fams[i];
  return NULL;
}

int xts_family_has_axis(const xts_family_t *f, const char *axis)
{
  const char *a = f ? f->axes : NULL;
  size_t l = strlen(axis);
  while (a && *a) {
    size_t n = strcspn(a, ",");
    while (n && a[0] == ' ') { a++; n--; }
    if (n == l && !strncmp(a, axis, l)) return 1;
    a += n;
    if (*a == ',') a++;
  }
  return 0;
}

int xts_axis_count(void) { xts_registry_init(NULL); return naxes; }
const xts_axis_t *xts_axis_get(int i) { xts_registry_init(NULL); return (i >= 0 && i < naxes) ? axes[i] : NULL; }

const xts_axis_t *xts_axis_find(const char *name)
{
  for (int i = 0; i < naxes; i++)
    if (!strcmp(axes[i]->name, name)) return axes[i];
  return NULL;
}

int xts_axis_nvalues(const xts_axis_t *a)
{
  int n = 0;
  while (a && a->values && a->values[n]) n++;
  return n;
}

int xts_axis_value_index(const xts_axis_t *a, const char *value)
{
  for (int i = 0; a && a->values && a->values[i]; i++)
    if (!strcmp(a->values[i], value)) return i;
  return -1;
}

const char *xts_cond_get(const xts_condvec_t *cv, const char *name)
{
  for (int i = 0; cv && i < cv->n; i++)
    if (!strcmp(cv->axis[i]->name, name)) return cv->axis[i]->values[cv->value[i]];
  return NULL;
}

int xts_cond_format(const xts_condvec_t *cv, char *buf, int size, int only_applied)
{
  int o = 0;
  if (size > 0) buf[0] = 0;
  for (int i = 0; cv && i < cv->n; i++) {
    if (only_applied && !(cv->applied & (1u << i))) continue;
    int w = snprintf(buf + o, size > o ? size - o : 0, "%s%s=%s", o ? ";" : "", cv->axis[i]->name,
                     cv->axis[i]->values[cv->value[i]]);
    if (w > 0) o += w;
    if (o >= size) { o = size - 1; break; }
  }
  return o;
}

/* ---- env ---- */
static char env_slot_names[16][32];

void xts_env_reset(void)
{
  memset(&xts_env, 0, sizeof xts_env);
  xts_env.sf = 1;
}

int xts_env_slot(const char *name)
{
  for (int i = 0; i < 16; i++) {
    if (!env_slot_names[i][0]) { snprintf(env_slot_names[i], sizeof env_slot_names[i], "%s", name); return i; }
    if (!strcmp(env_slot_names[i], name)) return i;
  }
  return -1;
}

/* ---- small utilities ---- */
int xts_glob(const char *p, const char *s)
{
  if (!p) return 1;
  while (*p) {
    if (*p == '*') {
      while (*p == '*') p++;
      if (!*p) return 1;
      for (; *s; s++)
        if (xts_glob(p, s)) return 1;
      return 0;
    }
    if (!*s || (*p != '?' && *p != *s)) return 0;
    p++;
    s++;
  }
  return *s == 0;
}

const char *xts_status_name(int st)
{
  static const char *const n[XTS_ST_COUNT] = { "pass", "fail", "trap", "skip", "noisy", "regressed", "hang", "crash", "info",
                                               "nonlinear" };
  return (st >= 0 && st < XTS_ST_COUNT) ? n[st] : "?";
}

int xts_status_from_name(const char *name)
{
  for (int s = 0; name && s < XTS_ST_COUNT; s++)
    if (!strcmp(name, xts_status_name(s))) return s;
  return -1;
}

/* crash > hang > fail > trap > noisy > nonlinear > regressed > skip > info > pass */
int xts_status_rank(int st)
{
  static const int8_t rank[XTS_ST_COUNT] = { [XTS_ST_PASS] = 0, [XTS_ST_INFO] = 1, [XTS_ST_SKIP] = 2, [XTS_ST_REGRESSED] = 3,
                                            [XTS_ST_NONLINEAR] = 4, [XTS_ST_NOISY] = 5, [XTS_ST_TRAP] = 6, [XTS_ST_FAIL] = 7,
                                            [XTS_ST_HANG] = 8, [XTS_ST_CRASH] = 9 };
  return (st >= 0 && st < XTS_ST_COUNT) ? rank[st] : -1;
}

int xts_status_measured(int st) { return st == XTS_ST_PASS || st == XTS_ST_NONLINEAR; }

int xts_status_worst(int a, int b)
{
  int ra = xts_status_rank(a), rb = xts_status_rank(b);
  if (ra < 0) return rb < 0 ? XTS_ST_PASS : b;
  if (rb < 0) return a;
  return rb > ra ? b : a;
}

/* ---- vocabularies ---- */
extern const xts_vocab_entry_t *const __start_xts_vocab[] __attribute__((weak));
extern const xts_vocab_entry_t *const __stop_xts_vocab[] __attribute__((weak));
static const char *const metric_vocab[] = { "latency", "rthroughput", "issue_cost", "penalty", "extra", "overlap", "cycles",
                                            "ratio", "bytes_per_cycle", "depth", "count", "result", "estimate", "tininess",
                                            "info", NULL };
static const char *const unit_vocab[] = { "cycles", "ratio", "count", "bytes", "bytes_per_cycle", "entries", "words", "hz",
                                          "ticks", "", NULL };

static int vocab_known(const char *name, const char *const *builtin, int is_unit)
{
  if (!name) name = is_unit ? "" : "result";
  for (int i = 0; builtin[i]; i++)
    if (!strcmp(builtin[i], name)) return 1;
  if (__start_xts_vocab && __stop_xts_vocab)
    for (const xts_vocab_entry_t *const *p = __start_xts_vocab; p < __stop_xts_vocab; p++)
      if (*p && (*p)->is_unit == is_unit && (*p)->name && !strcmp((*p)->name, name)) return 1;
  return 0;
}
int xts_metric_known(const char *metric) { return vocab_known(metric, metric_vocab, 0); }
int xts_unit_known(const char *unit) { return vocab_known(unit, unit_vocab, 1); }

/* ---- condition queries (xts/kgen.h) ---- */
static int cond_index(const xts_ctx_t *ctx, const char *axis)
{
  if (!ctx || !ctx->cond || !axis) return -1;
  for (int i = 0; i < ctx->cond->n; i++)
    if (ctx->cond->axis[i] && !strcmp(ctx->cond->axis[i]->name, axis)) return i;
  return -1;
}
int xts_cond_applied(const xts_ctx_t *ctx, const char *axis)
{
  int i = cond_index(ctx, axis);
  return i < 0 ? -1 : (int)((ctx->cond->applied >> i) & 1u);
}
int xts_cond_value(const xts_ctx_t *ctx, const char *axis)
{
  int i = cond_index(ctx, axis);
  return i < 0 ? -1 : ctx->cond->value[i];
}
