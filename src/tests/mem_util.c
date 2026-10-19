/* mem_util.c - private helpers of ldst.lat and mem.hier (see mem_util.h). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include "mem_util.h"

const char *const mu_level_name[MU_LV_N] = { "l1", "l2", "mem", "cold" };

/* ------------------------------------------------------------------------------------------------------------ forms */
static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }
static int ends(const char *s, const char *p)
{
  size_t a = strlen(s), b = strlen(p);
  return a >= b && !strcmp(s + a - b, p);
}

int mu_form_of(int insn, mu_form_t *f)
{
  const xts_insn_t *d = xts_insn(insn);
  if (!d || d->unit != XTS_UNIT_MEMORY || d->priv != XTS_PRIV_USER) return -1;
  if (d->hw != XTS_HW_EXECUTES && d->hw != XTS_HW_TRAPS) return -1;
  static const char *const excl[] = { "lvsl", "lvsr", "lmw", "stmw", "lswi", "lswx", "stswi", "stswx", "eciwx", "ecowx", NULL };
  for (int i = 0; excl[i]; i++)
    if (starts(d->mnem, excl[i])) return -1;
  memset(f, 0, sizeof *f);
  f->insn = insn;
  int reg = -1;
  for (int i = 0; i < d->nops; i++) {
    switch (d->ops[i].field) {
    case XTS_F_RD: case XTS_F_FD: case XTS_F_VD: case XTS_F_VD128: reg = i; f->load = 1; break;
    case XTS_F_RS: case XTS_F_FS: case XTS_F_VS: case XTS_F_VS128: reg = i; f->load = 0; break;
    case XTS_F_RB: f->xform = 1; break;
    case XTS_F_DS14: f->ds = 1; break;
    default: break;
    }
  }
  if (reg < 0) return -1;                       /* cache ops, dst/dss */
  int cls = d->ops[reg].cls;
  f->file = cls == XTS_OC_GPR ? 'g' : cls == XTS_OC_FPR ? 'f' : 'v';
  f->vr128 = d->isa == XTS_ISA_VMX128;
  const char *m = d->mnem;
  f->update = d->isa == XTS_ISA_PPC && (ends(m, "u") || ends(m, "ux"));
  f->resv = !strcmp(m, "lwarx") || !strcmp(m, "ldarx") || !strcmp(m, "stwcx") || !strcmp(m, "stdcx");
  f->brev = strstr(m, "brx") != NULL;
  f->sign = starts(m, "lha") || (starts(m, "lwa") && strcmp(m, "lwarx"));
  if (f->file == 'g') {
    char c = m[0] == 'l' ? m[1] : m[2];
    f->width = c == 'b' ? 1 : c == 'h' ? 2 : c == 'w' ? 4 : c == 'd' ? 8 : 0;
  } else if (f->file == 'f') {
    f->width = strstr(m, "fd") ? 8 : 4;
    f->fpsingle = strstr(m, "fs") != NULL;
    f->align_trap = 4;
  } else {
    f->elem = starts(m, "lve") || starts(m, "stve");
    f->width = !f->elem ? 16 : strstr(m, "veb") ? 1 : strstr(m, "veh") ? 2 : 4;
    f->lr = (starts(m, "lvl") || starts(m, "stvl")) ? 1 : (starts(m, "lvr") || starts(m, "stvr")) ? 2 : 0;
  }
  if (f->resv) f->align_trap = f->width;
  return f->width ? 0 : -1;
}

int mu_emit_mem(xts_kbuf_t *k, int insn, int reg, int base, int64_t idx)
{
  const xts_insn_t *d = xts_insn(insn);
  int64_t ops[XTS_MAX_OPS] = { 0 };
  for (int i = 0; i < d->nops; i++) {
    switch (d->ops[i].field) {
    case XTS_F_RD: case XTS_F_RS: case XTS_F_FD: case XTS_F_FS: case XTS_F_VD: case XTS_F_VS: case XTS_F_VD128:
    case XTS_F_VS128: ops[i] = reg; break;
    case XTS_F_RA: case XTS_F_RA0: ops[i] = base; break;
    case XTS_F_RB: case XTS_F_D16: case XTS_F_DS14: ops[i] = idx; break;
    default: ops[i] = 0; break;
    }
  }
  return xk_emit_a(k, insn, 0, ops, d->nops);
}

void mu_put_value(uint8_t *p, const mu_form_t *f, uint64_t v)
{
  int n = f->width > 8 ? 8 : f->width;
  if (f->brev)
    for (int i = 0; i < n; i++) { p[i] = (uint8_t)v; v >>= 8; }
  else
    xts_be_store(p, n, v);
}

/* --------------------------------------------------------------------------------------------------------- geometry */
uint32_t mu_pow2(uint32_t v)
{
  uint32_t p = 8;
  while (p < v) p <<= 1;
  return p;
}
/* page: 0 none, 4 -> 4 KB crossing elements, 64 -> 64 KB crossing elements */
int mu_geom_for(const mu_form_t *f, uint32_t doff, int page, mu_geom_t *g)
{
  memset(g, 0, sizeof *g);
  if (f->file == 'g' && f->ds) { g->a = doff; g->di = 0; }
  else { g->a = doff & ~63u; g->di = (int32_t)(doff & 63u); }
  if (f->file == 'g') {
    uint32_t base = g->a + (uint32_t)(f->update ? g->di : 0);
    uint32_t need = g->a + (uint32_t)g->di + (uint32_t)f->width;
    g->V = (int32_t)(((need + 7u) & ~7u) - base);
    g->ptr_rel = (f->update ? g->di : 0) + g->V;
    g->data_rel = g->di;
    g->data_w = f->width;
    if (g->V > (f->width == 1 ? 255 : f->sign && f->width == 2 ? 32767 : 65535)) return -1;
  } else if (f->file == 'f') {
    if (f->update && (g->di & 7)) return -1;            /* the updated base would leave the pointer misaligned */
    g->V = (int32_t)mu_pow2((uint32_t)g->di + (uint32_t)f->width);
    g->ptr_rel = (f->update ? g->di : 0) + g->V;
    g->data_rel = g->di;
    g->data_w = f->width;
  } else {
    if (f->lr == 2) g->di += 16;                         /* lvrx family: bytes BEFORE the EA */
    g->data_rel = 0;
    g->data_w = ((g->di + 16 + 15) & ~15);
    if (g->data_w > 128) return -1;
    g->V = (int32_t)mu_pow2((uint32_t)g->data_w);
    if (g->V < 32) g->V = 32;
    g->ptr_rel = g->V;
  }
  uint32_t hi = g->a + (uint32_t)(g->ptr_rel + 8 > g->data_rel + g->data_w ? g->ptr_rel + 8 : g->data_rel + g->data_w);
  if (page == 4) g->stride = 12u * MU_KB;
  else if (page == 64) g->stride = 192u * MU_KB;
  else g->stride = hi <= 128 ? 128 : mu_pow2(hi);
  return hi <= g->stride ? 0 : -1;
}

/* ------------------------------------------------------------------------------------------------------- big buffer */
int mu_big(uint32_t need, mu_buf_t *b)
{
  uint64_t a = 0;
  uint32_t want = need > MU_BIG_BYTES ? need : MU_BIG_BYTES;
  if (xts_big_size() >= want) want = xts_big_size();       /* never shrink-and-realloc */
  uint8_t *p = xts_big(want, &a);
  if (!p) {
    memset(b, 0, sizeof *b);
    return -1;
  }
  b->p = p;
  b->addr = a;
  b->size = xts_big_size();
  return b->size >= need ? 0 : -1;
}

/* ----------------------------------------------------------------------------------------------------------- chains */
static uint32_t xs32(uint32_t *s)
{
  uint32_t x = *s;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return *s = x;
}

/* the field range of one anchor, relative to its offset: [lo, hi) */
static void chain_rel_range(const mu_chain_t *c, int32_t *lo, int32_t *hi)
{
  int32_t a = c->ptr_rel, b = c->ptr_rel + 8;
  if (c->data_w) {
    if (c->data_rel < a) a = c->data_rel;
    if (c->data_rel + c->data_w > b) b = c->data_rel + c->data_w;
  }
  *lo = a;
  *hi = b;
}

int mu_chain_verify(const mu_chain_t *c)
{
  if (!c->order || !c->n || !c->buf.p) return -1;
  uint8_t tmp[128];
  uint32_t cur = mu_chain_off(c, c->order[0]);
  for (uint32_t j = 0; j < c->n; j++) {
    uint32_t want = mu_chain_off(c, c->order[(j + 1) % c->n]);
    uint64_t ptr = xts_be_load(c->buf.p + cur + c->ptr_rel, 8);
    if (ptr < c->buf.addr || ptr - c->buf.addr != want) return -2;
    if (c->data_w) {
      int n = c->data_w;
      if (n > (int)sizeof tmp || (c->df.file != 'v' && n > 8)) return -3;
      memset(tmp, 0, sizeof tmp);
      if (c->df.file != 'v') mu_put_value(tmp, &c->df, c->data);       /* vector data is all zero */
      if (memcmp(tmp, c->buf.p + cur + c->data_rel, (size_t)n)) return -4;
    }
    cur = want;
  }
  return cur == mu_chain_off(c, c->order[0]) ? 0 : -5;
}

int mu_chain_build(mu_chain_t *c)
{
  if (!c->n || !c->buf.p || c->ptr_rel < 0 || c->data_rel < 0) return -1;
  int32_t lo, hi;
  chain_rel_range(c, &lo, &hi);
  if (c->data_w && c->data_rel < c->ptr_rel + 8 && c->ptr_rel < c->data_rel + c->data_w) return -1;   /* overlap */
  uint64_t last = 0;
  for (uint32_t i = 0; i < c->n; i++) {
    uint64_t o = (uint64_t)c->base + (uint64_t)i * c->stride + (c->jmod ? (uint64_t)(i % c->jmod) * c->jstep : 0) +
                 (c->bdiv ? (uint64_t)(i / c->bdiv) * c->bstep : 0) + c->anchor;
    if ((o + (uint64_t)c->ptr_rel) & 7) return -1;                   /* pointers are aligned doublewords */
    if (o + (uint64_t)hi > last) last = o + (uint64_t)hi;
  }
  if (last > c->buf.size) return -1;
  uint32_t *ord = realloc(c->order, (size_t)c->n * sizeof *ord);
  if (!ord) return -1;
  c->order = ord;
  for (uint32_t i = 0; i < c->n; i++) ord[i] = i;
  if (c->random && c->n > 1) {
    uint32_t s = c->seed * 2654435761u + 0x9E3779B9u;
    if (!s) s = 1;
    for (uint32_t i = c->n - 1; i > 0; i--) {                         /* Fisher-Yates */
      uint32_t j = xs32(&s) % (i + 1);
      uint32_t t = ord[i]; ord[i] = ord[j]; ord[j] = t;
    }
  }
  for (uint32_t i = 0; i < c->n; i++)                                 /* zero every anchor's fields first */
    memset(c->buf.p + mu_chain_off(c, i) + lo, 0, (size_t)(hi - lo));
  for (uint32_t i = 0; i < c->n && c->data_w; i++)
    if (c->df.file != 'v') mu_put_value(c->buf.p + mu_chain_off(c, i) + c->data_rel, &c->df, c->data);
  for (uint32_t j = 0; j < c->n; j++) {
    uint32_t o = mu_chain_off(c, ord[j]), nx = mu_chain_off(c, ord[(j + 1) % c->n]);
    xts_be_store(c->buf.p + o + c->ptr_rel, 8, (uint64_t)(uint32_t)(c->buf.addr + nx));
  }
  c->span = (uint32_t)(last - c->base);
  return mu_chain_verify(c) ? -2 : 0;
}

uint64_t mu_chain_addr_at(const mu_chain_t *c, uint32_t pos)
{
  return c->buf.addr + mu_chain_off(c, c->order[pos % c->n]);
}

void mu_chain_flush(const mu_chain_t *c)
{
  int32_t lo, hi;
  chain_rel_range(c, &lo, &hi);
  if (c->jmod == 0 && c->bdiv == 0 && c->stride <= 4 * MU_LINE) {                    /* dense: one range */
    xts_dcbf_range(c->buf.p + c->base, c->span);
    return;
  }
  for (uint32_t i = 0; i < c->n; i++) xts_dcbf_range(c->buf.p + mu_chain_off(c, i) + lo, (uint32_t)(hi - lo));
}

/* ------------------------------------------------------------------------------------------------------ measurement */
void mu_det_begin(mu_det_t *d)
{
  xj_init(&d->j, d->buf, sizeof d->buf);
  xj_obj(&d->j);
}

double mu_measure(xts_ctx_t *ctx, const xts_tspec_t *s, xts_tresult_t *r)
{
  int rc = xts_time_measure(s, ctx, r);
  if (rc < 0) {                                 /* kernel build / setup / exec error: the emitter records fail + "error" */
    r->ok = 0;
    r->failed = 1;
    r->status = XTS_ST_FAIL;
    return NAN;
  }
  return xts_time_cycles(r, 1);
}

void mu_emit_note(xts_ctx_t *ctx, mu_det_t *d, const char *metric, int insn, double value, const char *unit, int status,
                  const char *case_id)
{
  xts_temit_t e = { .metric = metric, .unit = unit, .extra_json = mu_det_end(d), .no_negative = 1, .status = status,
                    .case_id = case_id };
  xts_time_emit_value(ctx, NULL, NULL, insn, value, &e);
}

/* ------------------------------------------------------------------------------------------------------ pure chase */
int mu_chase_body(xts_kbuf_t *k, int depth, void *arg)
{
  mu_chase_t *a = arg;
  for (int i = 0; i < depth; i++) xk_emit(k, XI_ld, 0, 3, a->c->ptr_rel, 3);
  return 0;
}

int mu_chase_pre(xts_kbuf_t *k, int depth, void *arg)
{
  mu_chase_t *a = arg;
  (void)depth;
  if (!a->warm) return 0;
  xk_emit(k, XI_orx, 0, 20, 3, 3);                       /* r20 = start */
  xk_li32(k, 21, a->c->n);
  int top = xk_loop_begin(k, 21);
  xk_emit(k, XI_ld, 0, 20, a->c->ptr_rel, 20);
  xk_loop_end(k, top);
  return 0;
}

int mu_chase_setup(xts_exec_t *x, void *arg)
{
  mu_chase_t *a = arg;
  if (a->flush) mu_chain_flush(a->c);
  xts_set_gpr(x, 3, mu_chain_addr_at(a->c, a->rotate * a->runs));
  a->runs++;
  return 0;
}

double mu_chase_measure(xts_ctx_t *ctx, mu_chain_t *c, int warm, int flush, uint32_t rotate, xts_tspec_t *s,
                        xts_tresult_t *r)
{
  static mu_chase_t a;
  memset(&a, 0, sizeof a);
  a.c = c;
  a.warm = warm;
  a.flush = flush;
  a.rotate = rotate;
  s->body = mu_chase_body;
  s->pre = mu_chase_pre;
  s->setup = mu_chase_setup;
  s->arg = &a;
  if (!s->unit || !s->unit[0]) s->unit = "ld r3,0(r3)";
  if (s->line_layout) xts_time_line_layout(s, s->line_layout);   /* asked for by the caller (ldst.lat references) */
  return mu_measure(ctx, s, r);
}

/* --------------------------------------------------------------------------------------------------- level refs */
const mu_levels_t *mu_levels(xts_ctx_t *ctx)
{
  static mu_levels_t cache[8];
  static mu_chain_t c;
  int oc = xts_env.other_cores & 7;
  mu_levels_t *L = &cache[oc];
  if (L->valid) return L;
  for (int i = 0; i < 3; i++) { L->lat[i] = NAN; L->status[i] = XTS_ST_SKIP; }
  if (mu_big(MU_BIG_BYTES, &c.buf) < 0) return L;
  static const uint32_t ws[3] = { MU_WS_L1, MU_WS_L2, MU_WS_MEM };
  for (int i = 0; i < 3; i++) {
    c.base = 0;
    c.n = ws[i] / MU_LINE;
    c.stride = MU_LINE;
    c.jmod = 0;
    c.bdiv = 0;
    c.anchor = 0;
    c.ptr_rel = 0;
    c.data_w = 0;
    c.random = 1;
    c.seed = 0x1E7E1u + (uint32_t)i;
    if (mu_chain_build(&c) < 0) { L->status[i] = XTS_ST_FAIL; continue; }
    xts_tspec_t s = XTS_TSPEC_DEFAULT;
    xts_tresult_t r;
    s.unit = "ld r3,0(r3) reference chase";
    L->lat[i] = mu_chase_measure(ctx, &c, i < 2, 0, i == 2 ? 32 * 256 : 0, &s, &r);
    L->status[i] = r.status;
  }
  L->valid = 1;
  return L;
}

/* ------------------------------------------------------------------------------------------------------- analysis */
static double med_of(const double *v, int n)
{
  double t[64];
  if (n > 64) n = 64;
  memcpy(t, v, (size_t)n * sizeof *t);
  return xts_median(t, n);
}

int mu_plateaus(const double *x, const double *y, int n, mu_plateau_t *p, int maxp)
{
  (void)x;
  int idx[128], m = 0;
  double v[128];
  for (int i = 0; i < n && m < 128; i++)
    if (!isnan(y[i]) && y[i] > 0) { idx[m] = i; v[m] = y[i]; m++; }
  mu_plateau_t runs[64];
  int nr = 0, i = 0;
  while (i < m && nr < 64) {
    int j = i;
    double lo = v[i], hi = v[i];
    while (j + 1 < m) {
      double l2 = lo < v[j + 1] ? lo : v[j + 1], h2 = hi > v[j + 1] ? hi : v[j + 1];
      if (h2 / l2 > MU_PLATEAU_SPREAD) break;
      lo = l2; hi = h2; j++;
    }
    if (j > i) {
      runs[nr].lo = i;
      runs[nr].hi = j;
      runs[nr].level = med_of(v + i, j - i + 1);
      nr++;
      i = j + 1;
    } else {
      i++;
    }
  }
  int np = 0;
  for (int r = 0; r < nr; r++) {
    if (np && runs[r].lo == p[np - 1].hi + 1) {
      double a = p[np - 1].level, b = runs[r].level;
      if ((a > b ? a / b : b / a) < MU_LEVEL_FACTOR) {
        p[np - 1].hi = runs[r].hi;
        p[np - 1].level = med_of(v + p[np - 1].lo, p[np - 1].hi - p[np - 1].lo + 1);
        continue;
      }
    }
    if (np >= maxp) break;
    p[np++] = runs[r];
  }
  for (int r = 0; r < np; r++) { p[r].lo = idx[p[r].lo]; p[r].hi = idx[p[r].hi]; }   /* back to caller indices */
  return np;
}

int mu_classify(double v, const double *lev, int nlev)
{
  if (isnan(v) || v <= 0) return -1;
  int b0 = -1, b1 = -1;
  double d0 = 1e30, d1 = 1e30;
  for (int i = 0; i < nlev; i++) {
    if (isnan(lev[i]) || lev[i] <= 0) continue;
    double d = fabs(log(v / lev[i]));
    if (d < d0) { d1 = d0; b1 = b0; d0 = d; b0 = i; }
    else if (d < d1) { d1 = d; b1 = i; }
  }
  if (b1 >= 0 && d0 > 0.35 * (d0 + d1) && d0 > log(1.2)) return -1;
  return b0;
}
