/* Private helpers of the tlat timing families (see timing_ops.h). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include "timing_ops.h"
#include <xts/cond_util.h>

const int tlat_depths[TLAT_NDEPTHS] = { 8, 16, 24, 32, 48, 64 };

/* ------------------------------------------------------------------------------------------------ FP families */
#define M(i) (1u << (i))
static const tlat_fpinfo_t fptab[] = {
  /* scalar: operand 0 is FD */
  { "faddx", TLAT_FAM_ADD, 0, 0, 0 },          { "fsubx", TLAT_FAM_ADD, 0, 1, 0 },
  { "faddsx", TLAT_FAM_ADD, 1, 0, 0 },         { "fsubsx", TLAT_FAM_ADD, 1, 1, 0 },
  { "fmulx", TLAT_FAM_MUL, 0, 0, M(1) | M(2) }, { "fmulsx", TLAT_FAM_MUL, 1, 0, M(1) | M(2) },
  { "fdivx", TLAT_FAM_DIV, 0, 0, 0 },          { "fdivsx", TLAT_FAM_DIV, 1, 0, 0 },
  { "fmaddx", TLAT_FAM_FMA, 0, 0, M(1) | M(2) },  { "fmsubx", TLAT_FAM_FMA, 0, 1, M(1) | M(2) },
  { "fnmaddx", TLAT_FAM_FMA, 0, 0, M(1) | M(2) }, { "fnmsubx", TLAT_FAM_FMA, 0, 1, M(1) | M(2) },
  { "fmaddsx", TLAT_FAM_FMA, 1, 0, M(1) | M(2) },  { "fmsubsx", TLAT_FAM_FMA, 1, 1, M(1) | M(2) },
  { "fnmaddsx", TLAT_FAM_FMA, 1, 0, M(1) | M(2) }, { "fnmsubsx", TLAT_FAM_FMA, 1, 1, M(1) | M(2) },
  { "fsqrtx", TLAT_FAM_SQRT, 0, 0, 0 },        { "fsqrtsx", TLAT_FAM_SQRT, 1, 0, 0 },
  { "fresx", TLAT_FAM_EST, 1, 0, 0 },          { "frsqrtex", TLAT_FAM_EST, 0, 0, 0 },
  { "frspx", TLAT_FAM_ROUND, 1, 0, 0 },
  { "fctiwx", TLAT_FAM_F2I, 0, 0, 0 },  { "fctiwzx", TLAT_FAM_F2I, 0, 0, 0 },
  { "fctidx", TLAT_FAM_F2I, 0, 0, 0 },  { "fctidzx", TLAT_FAM_F2I, 0, 0, 0 },
  { "fcfidx", TLAT_FAM_I2F, 0, 0, 0 },
  { "fmrx", TLAT_FAM_MOVE, 0, 0, 0 },   { "fnegx", TLAT_FAM_MOVE, 0, 0, 0 },
  { "fabsx", TLAT_FAM_MOVE, 0, 0, 0 },  { "fnabsx", TLAT_FAM_MOVE, 0, 0, 0 },
  { "fselx", TLAT_FAM_SEL, 0, 0, 0 },
  /* vector (single precision) */
  { "vaddfp", TLAT_FAM_ADD, 1, 0, 0 },    { "vsubfp", TLAT_FAM_ADD, 1, 1, 0 },
  { "vaddfp128", TLAT_FAM_ADD, 1, 0, 0 }, { "vsubfp128", TLAT_FAM_ADD, 1, 1, 0 },
  { "vmulfp128", TLAT_FAM_MUL, 1, 0, M(1) | M(2) },
  { "vmaddfp", TLAT_FAM_FMA, 1, 0, M(1) | M(2) },        /* VD = VA*VC + VB */
  { "vnmsubfp", TLAT_FAM_FMA, 1, 1, M(1) | M(2) },       /* VD = -(VA*VC - VB) */
  { "vmaddfp128", TLAT_FAM_FMA, 1, 0, M(1) | M(2) },     /* VD = VA*VB + VD (tied addend) */
  { "vnmsubfp128", TLAT_FAM_FMA, 1, 1, M(1) | M(2) },    /* VD = -(VA*VB - VD) */
  { "vmaddcfp128", TLAT_FAM_FMA, 1, 0, M(0) | M(1) | M(2) },   /* VD = VA*VD + VB (tied multiplicand, VS at 2) */
  { "vrefp", TLAT_FAM_EST, 1, 0, 0 },    { "vrsqrtefp", TLAT_FAM_EST, 1, 0, 0 },
  { "vexptefp", TLAT_FAM_EST, 1, 0, 0 }, { "vlogefp", TLAT_FAM_EST, 1, 0, 0 },
  { "vrefp128", TLAT_FAM_EST, 1, 0, 0 }, { "vrsqrtefp128", TLAT_FAM_EST, 1, 0, 0 },
  { "vexptefp128", TLAT_FAM_EST, 1, 0, 0 }, { "vlogefp128", TLAT_FAM_EST, 1, 0, 0 },
  { "vrfin", TLAT_FAM_ROUND, 1, 0, 0 },  { "vrfiz", TLAT_FAM_ROUND, 1, 0, 0 },
  { "vrfip", TLAT_FAM_ROUND, 1, 0, 0 },  { "vrfim", TLAT_FAM_ROUND, 1, 0, 0 },
  { "vrfin128", TLAT_FAM_ROUND, 1, 0, 0 }, { "vrfiz128", TLAT_FAM_ROUND, 1, 0, 0 },
  { "vrfip128", TLAT_FAM_ROUND, 1, 0, 0 }, { "vrfim128", TLAT_FAM_ROUND, 1, 0, 0 },
  { "vctsxs", TLAT_FAM_F2I, 1, 0, 0 },   { "vctuxs", TLAT_FAM_F2I, 1, 0, 0 },
  { "vcfpsxws128", TLAT_FAM_F2I, 1, 0, 0 }, { "vcfpuxws128", TLAT_FAM_F2I, 1, 0, 0 },
  { "vcfsx", TLAT_FAM_I2F, 1, 0, 0 },    { "vcfux", TLAT_FAM_I2F, 1, 0, 0 },
  { "vcsxwfp128", TLAT_FAM_I2F, 1, 0, 0 }, { "vcuxwfp128", TLAT_FAM_I2F, 1, 0, 0 },
  { "vmaxfp", TLAT_FAM_MINMAX, 1, 0, 0 }, { "vminfp", TLAT_FAM_MINMAX, 1, 1, 0 },
  { "vmaxfp128", TLAT_FAM_MINMAX, 1, 0, 0 }, { "vminfp128", TLAT_FAM_MINMAX, 1, 1, 0 },
  { "vcmpeqfp", TLAT_FAM_CMP, 1, 0, 0 }, { "vcmpgefp", TLAT_FAM_CMP, 1, 0, 0 },
  { "vcmpgtfp", TLAT_FAM_CMP, 1, 0, 0 }, { "vcmpbfp", TLAT_FAM_CMP, 1, 0, 0 },
  { "vcmpeqfp128", TLAT_FAM_CMP, 1, 0, 0 }, { "vcmpgefp128", TLAT_FAM_CMP, 1, 0, 0 },
  { "vcmpgtfp128", TLAT_FAM_CMP, 1, 0, 0 }, { "vcmpbfp128", TLAT_FAM_CMP, 1, 0, 0 },
  { "vmsum3fp128", TLAT_FAM_DOT, 1, 0, M(1) | M(2) }, { "vmsum4fp128", TLAT_FAM_DOT, 1, 0, M(1) | M(2) },
};
#undef M

const tlat_fpinfo_t *tlat_fpinfo(int insn)
{
  const xts_insn_t *d = xts_insn(insn);
  if (!d) return NULL;
  for (unsigned i = 0; i < sizeof fptab / sizeof fptab[0]; i++)
    if (!strcmp(fptab[i].mnem, d->mnem)) return &fptab[i];
  return NULL;
}

const char *tlat_fam_name(int fam)
{
  static const char *const n[] = { "none", "add", "mul", "div", "fma", "sqrt", "estimate", "f2i", "i2f", "round",
                                   "move", "select", "minmax", "compare", "dot" };
  return fam >= 0 && fam < (int)(sizeof n / sizeof n[0]) ? n[fam] : "?";
}

/* ------------------------------------------------------------------------------------------------ shapes */
static int class_of(int oc)
{
  switch (oc) {
  case XTS_OC_GPR: case XTS_OC_GPR0: return TLAT_GPR;
  case XTS_OC_FPR: return TLAT_FPR;
  case XTS_OC_VR: case XTS_OC_VR128: case XTS_OC_VRC128: return TLAT_VR;
  case XTS_OC_CRB: return TLAT_CRB;
  case XTS_OC_CRF: return TLAT_CRF;
  default: return TLAT_NONE;
  }
}

const char *tlat_class_name(int cls)
{
  static const char *const n[] = { "none", "gpr", "fpr", "vr", "crb", "crf" };
  return cls >= 0 && cls <= TLAT_CRF ? n[cls] : "?";
}

/* destination also read although the JSON reads list does not say so */
static const char *const implicit_read_dest[] = { "rlwimix", "rldimix", NULL };
/* CRF operands that are FPSCR fields, not CR fields */
static const char *const not_cr[] = { "mcrfs", "mtfsfix", NULL };

static int in_list(const char *const *l, const char *s)
{
  for (; *l; l++)
    if (!strcmp(*l, s)) return 1;
  return 0;
}

int tlat_shape(int insn, tlat_shape_t *s, const char **why)
{
  static const char *dummy;
  if (!why) why = &dummy;
  memset(s, 0, sizeof *s);
  s->insn = insn;
  s->dest = -1;
  s->tied_alias = -1;
  const xts_insn_t *d = xts_insn(insn);
  if (!d) { *why = "no such instruction"; return -1; }
  if (d->priv != XTS_PRIV_USER) { *why = "privileged"; return -1; }
  if (d->hw != XTS_HW_EXECUTES) { *why = "hardware status is not executes"; return -1; }
  if (d->unit == XTS_UNIT_MEMORY) { *why = "memory unit (ldst families)"; return -1; }
  if (d->unit == XTS_UNIT_BRANCH) { *why = "branch unit"; return -1; }
  if (in_list(not_cr, d->mnem)) { *why = "FPSCR field operands"; return -1; }
  int srcs[XTS_MAX_OPS], nsrcs = 0;
  for (int i = 0; i < d->nops; i++) {
    const xts_operand_t *o = &d->ops[i];
    int cls = class_of(o->cls);
    if (o->cls == XTS_OC_VR128 || o->cls == XTS_OC_VRC128) s->vr128 = 1;
    if (!cls) continue;
    if ((o->role & XTS_ROLE_W) && s->dest < 0) {
      s->dest = i;
      s->cls = cls;
    } else if ((o->role & XTS_ROLE_TIED) && !(o->role & XTS_ROLE_W)) {
      s->tied_alias = i;
    } else if (o->role & XTS_ROLE_R) {
      srcs[nsrcs++] = i;
    }
  }
  if (s->dest < 0) { *why = "no register destination"; return -1; }
  for (int j = 0; j < nsrcs; j++) {
    if (class_of(d->ops[srcs[j]].cls) != s->cls) { *why = "sources of another register class (bypass family)"; return -1; }
    if (s->nsrc >= TLAT_MAXSRC) { *why = "too many sources"; return -1; }
    s->src[s->nsrc++] = srcs[j];
  }
  s->tied = (d->ops[s->dest].role & XTS_ROLE_TIED) || s->tied_alias >= 0 || in_list(implicit_read_dest, d->mnem);
  if (!s->nsrc && !s->tied) { *why = "no same-class source"; return -1; }
  return 0;
}

const char *tlat_slot_name(const tlat_shape_t *s, int slot)
{
  if (slot == TLAT_SLOT_ACC) return "acc";
  if (slot < 0 || slot >= s->nsrc) return "?";
  return xts_isa[s->insn].ops[s->src[slot]].name;
}

int tlat_variants(int insn, unsigned *out)
{
  unsigned f = xts_isa[insn].flags & (XTS_RC | XTS_OE);
  static const unsigned cand[4] = { 0, XTS_RC, XTS_OE, XTS_OE | XTS_RC };
  int n = 0;
  for (int i = 0; i < 4; i++)
    if ((cand[i] & ~f) == 0) out[n++] = cand[i];
  return n;
}

void tlat_variant_name(int insn, unsigned flags, char *buf, int size)
{
  snprintf(buf, size, "%s%s%s", xts_isa[insn].mnem, (flags & XTS_OE) ? ".oe" : "", (flags & XTS_RC) ? ".rc" : "");
}

/* ------------------------------------------------------------------------------------------------ values */
tlat_val_t tlat_gv(uint64_t u) { tlat_val_t v; memset(&v, 0, sizeof v); v.u = u; return v; }
tlat_val_t tlat_dv(double d) { uint64_t u; memcpy(&u, &d, 8); return tlat_gv(u); }
tlat_val_t tlat_fv(uint32_t f) { tlat_val_t v; memset(&v, 0, sizeof v); v.v = xts_vr4(f, f, f, f); v.u = f; return v; }
tlat_val_t tlat_vv(xts_vr_t x) { tlat_val_t v; memset(&v, 0, sizeof v); v.v = x; v.u = x.w[0]; return v; }

/* ------------------------------------------------------------------------------------------------ pools */
static const uint8_t gpr_pool[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 31 };
#define POOL_VRC (-1)

int tlat_pool_size(int cls, int vr128)
{
  switch (cls) {
  case TLAT_GPR: return (int)sizeof gpr_pool;
  case TLAT_FPR: return 32;
  case TLAT_VR: return vr128 == POOL_VRC ? 8 : vr128 ? 64 : 32;
  case TLAT_CRB: return 32;
  case TLAT_CRF: return 8;
  }
  return 0;
}

int tlat_pool(int cls, int vr128, int i)
{
  if (i < 0 || i >= tlat_pool_size(cls, vr128)) return -1;
  switch (cls) {
  case TLAT_GPR: return gpr_pool[i];
  case TLAT_VR: return vr128 == POOL_VRC ? i : vr128 ? 64 + i : i;
  default: return i;
  }
}

int tlat_is_vrc(const tlat_shape_t *s, int opidx)
{
  return opidx >= 0 && xts_isa[s->insn].ops[opidx].cls == XTS_OC_VRC128;
}

/* register allocator over one pool kind */
typedef struct alloc { int cls, vr128; int next; int next_vrc; uint8_t used[128]; } alloc_t;

static void al_init(alloc_t *a, const tlat_shape_t *s)
{
  memset(a, 0, sizeof *a);
  a->cls = s->cls;
  a->vr128 = s->vr128;
}

/* next free register; vrc = restricted to v0-v7 */
static int al_get(alloc_t *a, int vrc)
{
  if (vrc) {
    for (int i = 0; i < 8; i++)
      if (!a->used[i]) { a->used[i] = 1; return i; }
    return -1;
  }
  int n = tlat_pool_size(a->cls, a->vr128);
  while (a->next < n) {
    int r = tlat_pool(a->cls, a->vr128, a->next++);
    if (!a->used[r]) { a->used[r] = 1; return r; }
  }
  return -1;
}

/* ------------------------------------------------------------------------------------------------ kernels */
void tlat_kern_init(tlat_kern_t *k, const char *shape, int per_unit)
{
  memset(k, 0, sizeof *k);
  k->shape = shape;
  k->per_unit = per_unit;
  k->cr = 0xFFFFFFFFu;
  for (int i = 0; i < XTS_MAX_OPS; i++) k->op_reg[i] = -1;
  k->chain_reg[0] = k->chain_reg[1] = -1;
}

static void kerr(tlat_kern_t *k, const char *msg)
{
  if (!k->err) {
    k->err = -1;
    snprintf(k->errmsg, sizeof k->errmsg, "%s", msg);
  }
}

int tlat_kern_word(tlat_kern_t *k, int insn, unsigned flags, const int64_t *ops)
{
  if (k->npat >= TLAT_MAXPAT) { kerr(k, "pattern too long"); return -1; }
  const xts_insn_t *d = xts_insn(insn);
  uint32_t w;
  int bad;
  if (!d || xts_encode(insn, flags, ops, &w, &bad) != XTS_ENC_OK) {
    char m[96];
    snprintf(m, sizeof m, "encode %s failed (operand %d)", d ? d->mnem : "?", bad);
    kerr(k, m);
    return -1;
  }
  tlat_word_t *t = &k->pat[k->npat];
  t->insn = (uint16_t)insn;
  t->flags = (uint8_t)flags;
  t->nops = d->nops;
  memcpy(t->ops, ops, sizeof(int64_t) * XTS_MAX_OPS);
  return k->npat++;
}

void tlat_kern_set(tlat_kern_t *k, int cls, int reg, tlat_val_t v)
{
  if (cls == TLAT_CRB) {
    if (reg >= 0 && reg < 32) k->cr = v.u ? (k->cr | (0x80000000u >> reg)) : (k->cr & ~(0x80000000u >> reg));
    return;
  }
  if (cls == TLAT_CRF || cls == TLAT_NONE) return;
  for (int i = 0; i < k->nset; i++)
    if (k->set[i].cls == cls && k->set[i].reg == reg) { k->set[i].val = v; return; }
  if (k->nset >= TLAT_MAXSET) { kerr(k, "too many register values"); return; }
  k->set[k->nset].cls = (uint8_t)cls;
  k->set[k->nset].reg = (uint8_t)reg;
  k->set[k->nset].val = v;
  k->nset++;
}

int tlat_assign(const tlat_shape_t *s, int dreg, const int *sreg, int64_t *ops)
{
  const xts_insn_t *d = &xts_isa[s->insn];
  for (int i = 0; i < XTS_MAX_OPS; i++) ops[i] = 0;
  for (int i = 0; i < d->nops; i++) {
    const xts_operand_t *o = &d->ops[i];
    if (i == s->dest || i == s->tied_alias) { ops[i] = dreg; continue; }
    int j;
    for (j = 0; j < s->nsrc; j++)
      if (s->src[j] == i) break;
    if (j < s->nsrc) { ops[i] = sreg[j]; continue; }
    if (class_of(o->cls)) return -1;              /* a register operand we do not know how to fill */
    ops[i] = xts_field_check(o->field, 1) ? 1 : 0; /* neutral immediate */
  }
  return 0;
}

/* ------------------------------------------------------------------------------------------------ neutral data */
static int is_mnem(const tlat_shape_t *s, const char *a, const char *b)
{
  const char *m = xts_isa[s->insn].mnem;
  return !strcmp(m, a) || (b && !strcmp(m, b));
}

static int rotation_fp(const tlat_shape_t *s, int slot)
{
  const tlat_fpinfo_t *fi = tlat_fpinfo(s->insn);
  return s->tied && slot != TLAT_SLOT_ACC && fi && fi->fam == TLAT_FAM_FMA;
}

tlat_val_t tlat_neutral_chain(const tlat_shape_t *s, int slot)
{
  const tlat_fpinfo_t *fi = tlat_fpinfo(s->insn);
  switch (s->cls) {
  case TLAT_FPR:
    if (fi && fi->fam == TLAT_FAM_F2I) return tlat_gv(0);
    return tlat_dv(0.75);
  case TLAT_VR:
    if (fi && fi->fam == TLAT_FAM_F2I) return tlat_fv(0);
    if (is_mnem(s, "vexptefp", "vexptefp128")) return tlat_fv(0x7F800000u);
    if (is_mnem(s, "vlogefp", "vlogefp128")) return tlat_fv(0x7FC00000u);
    if (rotation_fp(s, slot)) return tlat_fv(TLAT_F32_ONE);
    if (fi && fi->fam == TLAT_FAM_ROUND) return tlat_fv(0x3FC00000u);   /* 1.5: rounds to 1 or 2, never to 0 */
    return tlat_fv(TLAT_F32_075);
  default:
    return tlat_gv(1);
  }
}

tlat_val_t tlat_neutral_other(const tlat_shape_t *s, int slot, int opidx)
{
  (void)opidx;
  switch (s->cls) {
  case TLAT_FPR: return tlat_dv(1.0);
  case TLAT_VR:
    if (rotation_fp(s, slot)) return tlat_fv(0);                         /* keeps the 2-register rotation bounded */
    if (is_mnem(s, "vmsum3fp128", NULL)) return tlat_vv(xts_vr4(0x3F000000u, 0x3E800000u, 0x3E800000u, 0)); /* .5 .25 .25 */
    if (is_mnem(s, "vmsum4fp128", NULL)) return tlat_fv(0x3E800000u);  /* 0.25: 4 x 0.25 x v = v */
    return tlat_fv(TLAT_F32_ONE);
  default:
    return tlat_gv(1);
  }
}

const char *tlat_neutral_note(const tlat_shape_t *s, int slot)
{
  const tlat_fpinfo_t *fi = tlat_fpinfo(s->insn);
  if (is_mnem(s, "vexptefp", "vexptefp128")) return "chain value +inf (2^x has no finite fixed point)";
  if (is_mnem(s, "vlogefp", "vlogefp128")) return "chain value QNaN (log2 has no finite fixed point)";
  if (fi && fi->fam == TLAT_FAM_F2I) return "chain value 0 (float->int chains are stable only at 0)";
  if (fi && fi->fam == TLAT_FAM_I2F) return "chain feeds float bits as integers (values stay large normal)";
  if (rotation_fp(s, slot)) return "rotation: other sources +0.0 keep the values at 1.0";
  return NULL;
}

/* ------------------------------------------------------------------------------------------------ builders */
static int set_others(const tlat_shape_t *s, int slot, alloc_t *a, int *sreg, int skip, tlat_kern_t *k)
{
  for (int j = 0; j < s->nsrc; j++) {
    if (j == skip) continue;
    int r = al_get(a, tlat_is_vrc(s, s->src[j]));
    if (r < 0) { kerr(k, "register pool exhausted"); return -1; }
    sreg[j] = r;
    tlat_kern_set(k, s->cls, r, tlat_neutral_other(s, slot, s->src[j]));
  }
  return 0;
}

int tlat_build_chain(const tlat_shape_t *s, unsigned flags, int slot, tlat_kern_t *k)
{
  alloc_t a;
  al_init(&a, s);
  int sreg[TLAT_MAXSRC] = { 0 };
  int64_t ops[XTS_MAX_OPS];
  if (slot == TLAT_SLOT_ACC) {
    if (!s->tied) { kerr(k, "acc slot on an untied instruction"); return -1; }
    tlat_kern_init(k, "acc", 1);
    int d = al_get(&a, 0);
    if (set_others(s, slot, &a, sreg, -1, k) < 0 || d < 0) return -1;
    tlat_kern_set(k, s->cls, d, tlat_neutral_chain(s, slot));
    if (tlat_assign(s, d, sreg, ops) < 0) { kerr(k, "operand assignment"); return -1; }
    tlat_kern_word(k, s->insn, flags, ops);
    k->chain_reg[0] = d;
  } else {
    if (slot < 0 || slot >= s->nsrc) { kerr(k, "bad slot"); return -1; }
    int vrc = tlat_is_vrc(s, s->src[slot]);
    if (s->tied) {                                  /* rotation: isolates this slot from the tied accumulator */
      tlat_kern_init(k, "rotation", 1);
      int r0 = al_get(&a, vrc), r1 = al_get(&a, vrc);
      if (r0 < 0 || r1 < 0 || set_others(s, slot, &a, sreg, slot, k) < 0) { kerr(k, "register pool exhausted"); return -1; }
      tlat_kern_set(k, s->cls, r0, tlat_neutral_chain(s, slot));
      tlat_kern_set(k, s->cls, r1, tlat_neutral_chain(s, slot));
      sreg[slot] = r0;
      if (tlat_assign(s, r1, sreg, ops) < 0) { kerr(k, "operand assignment"); return -1; }
      tlat_kern_word(k, s->insn, flags, ops);       /* r1 <- f(slot = r0, acc = r1) */
      sreg[slot] = r1;
      if (tlat_assign(s, r0, sreg, ops) < 0) { kerr(k, "operand assignment"); return -1; }
      tlat_kern_word(k, s->insn, flags, ops);       /* r0 <- f(slot = r1, acc = r0) */
      k->chain_reg[0] = r0;
      k->chain_reg[1] = r1;
    } else {
      tlat_kern_init(k, "chain", 1);
      int r = al_get(&a, vrc);
      if (r < 0 || set_others(s, slot, &a, sreg, slot, k) < 0) { kerr(k, "register pool exhausted"); return -1; }
      tlat_kern_set(k, s->cls, r, tlat_neutral_chain(s, slot));
      sreg[slot] = r;
      if (tlat_assign(s, r, sreg, ops) < 0) { kerr(k, "operand assignment"); return -1; }
      tlat_kern_word(k, s->insn, flags, ops);
      k->chain_reg[0] = r;
    }
  }
  const xts_insn_t *d = &xts_isa[s->insn];
  for (int i = 0; i < d->nops; i++) k->op_reg[i] = class_of(d->ops[i].cls) ? (int)k->pat[0].ops[i] : -1;
  return k->err;
}

int tlat_build_nchains(const tlat_shape_t *s, unsigned flags, int n, tlat_kern_t *k)
{
  tlat_kern_init(k, "nchains", n);
  if (n < 1 || n > TLAT_MAXPAT) { kerr(k, "bad chain count"); return -1; }
  alloc_t a;
  al_init(&a, s);
  int slot = s->tied ? TLAT_SLOT_ACC : 0;
  int vrc = slot == 0 && tlat_is_vrc(s, s->src[0]);
  int dreg[TLAT_MAXPAT], sreg[TLAT_MAXSRC] = { 0 };
  for (int c = 0; c < n; c++) {
    dreg[c] = al_get(&a, vrc);
    if (dreg[c] < 0) { kerr(k, "register pool too small for this chain count"); return -1; }
    tlat_kern_set(k, s->cls, dreg[c], tlat_neutral_chain(s, slot));
  }
  if (set_others(s, slot, &a, sreg, slot == TLAT_SLOT_ACC ? -1 : 0, k) < 0) return -1;
  int64_t ops[XTS_MAX_OPS];
  for (int c = 0; c < n; c++) {
    if (slot == 0) sreg[0] = dreg[c];
    if (tlat_assign(s, dreg[c], sreg, ops) < 0) { kerr(k, "operand assignment"); return -1; }
    tlat_kern_word(k, s->insn, flags, ops);
  }
  k->chain_reg[0] = dreg[0];
  return k->err;
}

int tlat_build_filler(int npat, int per_unit, tlat_kern_t *k)
{
  tlat_kern_init(k, "filler", per_unit);
  k->any_len = 1;
  if (npat < 1 || npat > TLAT_MAXPAT) { kerr(k, "bad filler length"); return -1; }
  int K = gpr_pool[0];
  tlat_kern_set(k, TLAT_GPR, K, tlat_gv(1));
  for (int i = 0; i < npat; i++) {
    int64_t ops[XTS_MAX_OPS] = { gpr_pool[1 + i % 8], K, 1 };
    tlat_kern_word(k, XI_addi, 0, ops);
  }
  return k->err;
}

/* ------------------------------------------------------------------------------------------------ measuring */
int tlat_body(xts_kbuf_t *kb, int depth, void *arg)
{
  const tlat_kern_t *k = arg;
  int n = depth * k->per_unit;
  if (k->npat < 1 || (!k->any_len && n % k->npat)) return -1;   /* rotation would not be seamless across iterations */
  for (int i = 0; i < n; i++) {
    const tlat_word_t *w = &k->pat[i % k->npat];
    xk_emit_a(kb, w->insn, w->flags, w->ops, w->nops);
  }
  return 0;
}

int tlat_setup(xts_exec_t *x, void *arg)
{
  const tlat_kern_t *k = arg;
  for (int i = 0; i < k->nset; i++) {
    const tlat_set_t *t = &k->set[i];
    switch (t->cls) {
    case TLAT_GPR: xts_set_gpr(x, t->reg, t->val.u); break;
    case TLAT_FPR: xts_set_fpr(x, t->reg, t->val.u); break;
    case TLAT_VR: xts_set_vr(x, t->reg, t->val.v); break;
    default: break;
    }
  }
  xts_set_cr(x, k->cr);
  xts_set_xer(x, k->xer);
  xts_set_fpscr(x, k->fpscr);
  xts_set_vscr(x, k->vscr);
  return 0;
}

void tlat_spec(xts_tspec_t *sp, tlat_kern_t *k, double est, const char *unit)
{
  xts_tspec_t d = XTS_TSPEC_DEFAULT;
  *sp = d;
  sp->unit = unit;
  sp->body = tlat_body;
  sp->setup = tlat_setup;
  sp->arg = k;
  sp->ndepths = TLAT_NDEPTHS;
  for (int i = 0; i < TLAT_NDEPTHS; i++) sp->depths[i] = tlat_depths[i];
  sp->repeats = 5;
  if (xts_cond_unroll(sp) && !k->any_len && k->npat > 1) {
    for (int i = 0; i < sp->ndepths; i++)
      while ((sp->depths[i] * k->per_unit) % k->npat) sp->depths[i]++;
  }
  if (!(est > 0.1)) est = 4;
  double it = 262144.0 / (est * sp->depths[0]);
  if (it < 32) it = 32;
  if (it > 65536) it = 65536;
  sp->iters = (uint32_t)it;
}

uint32_t tlat_word0(const tlat_kern_t *k)
{
  if (k->npat < 1) return 0;
  const tlat_word_t *w = &k->pat[0];
  return xts_enc_a(w->insn, w->flags, w->ops, w->nops);
}

double tlat_est_latency(int insn, double dflt)
{
  const xts_insn_t *d = xts_insn(insn);
  if (d && d->lat > 0) return d->lat;
  if (d && d->issue > 0) return d->issue;
  return dflt;
}

/* ------------------------------------------------------------------------------------------------ output */
int tlat_axis_nj(const xts_ctx_t *ctx, int *nj)
{
  if (xts_cond_applied(ctx, "vscr_nj") != 1) return 0;
  *nj = (xts_env.vscr_set & XTS_VSCR_NJ) ? 1 : 0;
  return 1;
}

int tlat_discriminates(double op, double filler)
{
  if (isnan(op) || isnan(filler)) return 1;      /* nothing to compare against: report as measured */
  return op > filler * 1.05 + 0.02;
}
