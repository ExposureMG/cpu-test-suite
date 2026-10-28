/* Scheduler: profile rows x matching families x (cartesian product of the row's axis values that the family is sensitive
 * to) x case blocks. Everything is enumerated up front so the run can show progress and the journal keys are stable. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"

/* SEED=<n> (make): base seed of every row that does not set its own, so repeated runs draw new random cases */
#ifndef XTS_RUN_SEED
#define XTS_RUN_SEED 1
#endif

typedef struct { int axis; int nv; uint8_t v[XTS_MAX_AXIS_VALUES]; } axsel_t;

static int warned_axis(const char *name)
{
  static char seen[16][40];
  static int n;
  for (int i = 0; i < n; i++)
    if (!strcmp(seen[i], name)) return 1;
  if (n < 16) snprintf(seen[n++], sizeof seen[0], "%s", name);
  return 0;
}

/* "sf=64,32;fpscr_rn=*" -> selections for registered axes */
static int parse_axes(const char *spec, axsel_t *sel, int max)
{
  int n = 0;
  const char *p = spec;
  while (p && *p) {
    size_t len = strcspn(p, ";");
    char item[256];
    snprintf(item, sizeof item, "%.*s", (int)(len < 255 ? len : 255), p);
    p += len;
    if (*p == ';') p++;
    char *eq = strchr(item, '=');
    if (!eq) continue;
    *eq = 0;
    char *name = item, *vals = eq + 1;
    while (*name == ' ') name++;
    const xts_axis_t *a = xts_axis_find(name);
    if (!a) {
      if (!warned_axis(name)) xts_log("profile: axis '%s' is not registered (yet): ignored", name);
      continue;
    }
    int ai = -1;
    for (int i = 0; i < xts_axis_count(); i++)
      if (xts_axis_get(i) == a) ai = i;
    if (n >= max) break;
    axsel_t *s = &sel[n];
    s->axis = ai;
    s->nv = 0;
    if (!strcmp(vals, "*")) {
      for (int v = 0; v < xts_axis_nvalues(a) && s->nv < XTS_MAX_AXIS_VALUES; v++) s->v[s->nv++] = (uint8_t)v;
    } else {
      char *save = NULL;
      for (char *t = strtok_r(vals, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        int v = xts_axis_value_index(a, t);
        if (v < 0) { xts_log("profile: axis %s has no value '%s': ignored", a->name, t); continue; }
        if (s->nv < XTS_MAX_AXIS_VALUES) s->v[s->nv++] = (uint8_t)v;
      }
    }
    if (s->nv) n++;
  }
  return n;
}

static void sanitize(char *s)
{
  for (; *s; s++)
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr("_.=,+-:", *s))) *s = '_';
}

static int add_block(xts_plan_t *p, const xts_block_t *b)
{
  if (p->n >= p->cap) {
    int nc = p->cap ? p->cap * 2 : 256;
    xts_block_t *nb = realloc(p->b, sizeof *nb * nc);
    if (!nb) return -1;
    p->b = nb;
    p->cap = nc;
  }
  p->b[p->n++] = *b;
  return 0;
}

int xts_plan_build(xts_plan_t *p, const xts_profile_t *prof)
{
  memset(p, 0, sizeof *p);
  p->profile = prof;
  int nf = xts_family_count(), na = xts_axis_count();
  p->cases = calloc(nf ? nf : 1, sizeof *p->cases);
  p->enumerated = calloc(nf ? nf : 1, sizeof *p->enumerated);
  if (!p->cases || !p->enumerated) return -1;
  for (int ri = 0; ri < prof->nrows; ri++) {
    const xts_profile_row_t *row = &prof->rows[ri];
    axsel_t sel[XTS_MAX_AXES];
    int ns = parse_axes(row->axes, sel, XTS_MAX_AXES);
    for (int fi = 0; fi < nf; fi++) {
      const xts_family_t *f = xts_family_get(fi);
      const char *fname = xts_family_name(f);
      if (!xts_glob(row->families, fname)) continue;
      if (row->kind && (!f->kind || strcmp(row->kind, f->kind))) continue;
      if (!p->enumerated[fi]) {
        p->enumerated[fi] = 1;
        if (f->enumerate(&p->cases[fi]) < 0) xts_log("family %s: enumerate() failed", fname);
      }
      int ncases = p->cases[fi].n;
      if (!ncases) continue;
      /* max_cases: evenly strided sample of the enumeration order (stride 1 = every case) */
      int stride = 1, nsel = ncases;
      if (row->max_cases > 0 && ncases > row->max_cases) {
        stride = (ncases + row->max_cases - 1) / row->max_cases;
        nsel = (ncases + stride - 1) / stride;
      }
      /* axes of this row that apply to this family */
      axsel_t use[XTS_MAX_AXES];
      int nu = 0;
      for (int s = 0; s < ns; s++) {
        const xts_axis_t *a = xts_axis_get(sel[s].axis);
        if (!xts_family_has_axis(f, a->name)) continue;
        if (a->applies_to && !a->applies_to(fname, f->kind ? f->kind : "")) continue;
        use[nu++] = sel[s];
      }
      if (row->sweep && nu == 0) continue;            /* sweep row: this family is not sensitive to its axes */
      int idx[XTS_MAX_AXES] = { 0 };
      int per = f->block_cases > 0 ? f->block_cases : 64;
      int nblocks_before = p->n;
      for (;;) {
        xts_block_t b;
        memset(&b, 0, sizeof b);
        b.row = ri;
        b.fam = fi;
        b.repeats = row->repeats > 0 ? row->repeats : 1;
        b.seed = row->seed ? row->seed : XTS_RUN_SEED;
        for (int u = 0; u < nu; u++) {
          b.value[use[u].axis] = use[u].v[idx[u]];
          b.applied |= 1u << use[u].axis;
        }
        char cond[120];
        int o = 0;
        cond[0] = 0;
        for (int a = 0; a < na; a++)
          if (b.applied & (1u << a))
            o += snprintf(cond + o, sizeof cond - o > 0 ? sizeof cond - o : 0, "%s%s=%s", o ? "," : "", xts_axis_get(a)->name,
                          xts_axis_get(a)->values[b.value[a]]);
        if (o >= (int)sizeof cond - 1) {                 /* too long for a key: use a hash of the full vector */
          uint64_t h = 0;
          for (int a = 0; a < na; a++)
            if (b.applied & (1u << a)) h = xts_hash_str(xts_hash_str(h, xts_axis_get(a)->name), xts_axis_get(a)->values[b.value[a]]);
          o = snprintf(cond, sizeof cond, "h%016llx", (unsigned long long)h);
        }
        b.stride = stride;
        for (int first = 0; first < nsel; first += per) {     /* first, per: positions in the sampled list */
          b.first_case = first * stride;
          b.ncases = nsel - first < per ? nsel - first : per;
          snprintf(b.key, sizeof b.key, "r%d:%s:%s:c%d", ri, fname, o ? cond : "-", first / per);
          sanitize(b.key);
          if (add_block(p, &b) < 0) return -1;
          p->total_cases += b.ncases * b.repeats;
        }
        /* next combination (last axis fastest) */
        int u = nu - 1;
        while (u >= 0 && ++idx[u] >= use[u].nv) idx[u--] = 0;
        if (u < 0) break;
      }
      if (f->finish && p->n > nblocks_before) {          /* summaries after the family's last block of this row */
        xts_block_t b;
        memset(&b, 0, sizeof b);
        b.row = ri;
        b.fam = fi;
        b.stride = 1;
        b.finish = 1;
        b.repeats = 1;
        b.seed = row->seed ? row->seed : XTS_RUN_SEED;
        snprintf(b.key, sizeof b.key, "r%d:%s:finish", ri, fname);
        sanitize(b.key);
        if (add_block(p, &b) < 0) return -1;
      }
    }
  }
  return 0;
}

int xts_block_case(const xts_block_t *b, int k) { return b->first_case + k * (b->stride > 0 ? b->stride : 1); }

int xts_block_rel(const xts_block_t *b, int case_index)
{
  int st = b->stride > 0 ? b->stride : 1, d = case_index - b->first_case;
  if (b->finish || d < 0 || d % st) return -1;
  return d / st < b->ncases ? d / st : -1;
}

void xts_plan_condvec(const xts_block_t *b, xts_condvec_t *cv)
{
  memset(cv, 0, sizeof *cv);
  cv->n = xts_axis_count();
  for (int i = 0; i < cv->n; i++) {
    cv->axis[i] = xts_axis_get(i);
    cv->value[i] = b ? b->value[i] : 0;
  }
  cv->applied = b ? b->applied : 0;
}

uint64_t xts_run_seed(void) { return (uint64_t)XTS_RUN_SEED; }

uint64_t xts_case_seed(const xts_block_t *b, const xts_case_t *c, int repeat)
{
  char cond[256];
  xts_condvec_t cv;
  xts_plan_condvec(b, &cv);
  xts_cond_format(&cv, cond, sizeof cond, 1);
  uint64_t h = xts_hash_u64(0, b->seed);
  h = xts_hash_str(h, xts_family_name(xts_family_get(b->fam)));
  h = xts_hash_str(h, c->id);
  h = xts_hash_str(h, cond);
  return xts_hash_u64(h, (uint64_t)repeat);
}
