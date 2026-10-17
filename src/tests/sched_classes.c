/* XTS - scheduling-model helpers: classes, generic operand planner, unit bodies, measurement (see sched_classes.h). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sched_classes.h"

/* ------------------------------------------------------------------------------------------------ class table
 * Representatives are JSON mnemonics; vector FP is split by the measured latency tiers (4 simple / 12 arithmetic and
 * conversion / 14 estimate and dot product), loads and stores per register file, CR producers and consumers separate.
 * `slots` limits bypass consumer operands where other operands would change the data-dependent latency (divide). */
#define P SC_U_PAIR
#define U SC_U_UNIT
#define BP SC_U_BP
#define BC SC_U_BC
sc_class_t sc_classes[] = {
  { "filler",   "addi",        "RA0=0", "li rD,imm: the 1-cycle independent filler (no register read)", 0, P | U, NULL, -1, 0 },
  { "int",      "addx",        NULL, "add (simple integer)", 0, P | U | BP | BC | SC_PTR_OK | SC_IDX_OK, NULL, -1, 0 },
  { "int_rc",   "addx",        NULL, "add. (record form: CR0 producer)", XTS_RC, BP | SC_IDX_OK, NULL, 0, 2 },
  { "logic",    "orx",         NULL, "or (logical)", 0, P, NULL, -1, 0 },
  { "rotimm",   "rlwinmx",     NULL, "rlwinm (rotate by immediate)", 0, P | BP | BC | SC_IDX_OK, NULL, -1, 0 },
  { "cmp",      "cmpi",        "SIMM=0", "cmpwi crF,rA,0 (compare -> CR)", 0, P | U | BP | BC | SC_IDX_OK, NULL, -1, 2 },
  { "carry",    "addex",       NULL, "adde (carry chain through XER[CA])", 0, P, NULL, -1, 0 },
  { "recimm",   "andi.",       NULL, "andi. (record-form immediate)", 0, P | U, NULL, -1, 0 },
  { "mulli",    "mulli",       NULL, "mulli", 0, P, NULL, -1, 0 },
  { "mul",      "mullwx",      NULL, "mullw (32-bit multiply)", 0, P | U | BP | BC | SC_IDX_OK, NULL, -1, 0 },
  { "mul64",    "mulldx",      NULL, "mulld (64-bit multiply)", 0, P, NULL, -1, 0 },
  { "div",      "divwx",       NULL, "divw (dividend slot only as a consumer)", 0, P | U | BC, "RA", -1, 0 },
  { "div64",    "divdx",       NULL, "divd", 0, P, NULL, -1, 0 },
  { "varshift", "slwx",        NULL, "slw (shift by register)", 0, P | U | BP | BC | SC_IDX_OK, NULL, -1, 0 },
  { "lwz",      "lwz",         NULL, "lwz (GPR load)", 0, P | U | BP | BC | SC_IDX_OK, NULL, -1, 0 },
  { "ld",       "ld",          NULL, "ld (64-bit GPR load)", 0, P | BP | SC_PTR_OK | SC_IDX_OK, NULL, -1, 0 },
  { "lbz",      "lbz",         NULL, "lbz (byte load)", 0, P, NULL, -1, 0 },
  { "lha",      "lha",         NULL, "lha (sign-extending load)", 0, P, NULL, -1, 0 },
  { "lwzx",     "lwzx",        NULL, "lwzx (indexed GPR load)", 0, P | BC, NULL, -1, 0 },
  { "lwzu",     "lwzu",        NULL, "lwzu (update-form load)", 0, P, NULL, -1, 0 },
  { "lfd",      "lfd",         NULL, "lfd (FPR load)", 0, P | U | BP, NULL, -1, 0 },
  { "lfs",      "lfs",         NULL, "lfs (single FPR load, converts)", 0, P | BP, NULL, -1, 0 },
  { "lvx",      "lvx128",      NULL, "lvx128 (VR load)", 0, P | U | BP, NULL, -1, 0 },
  { "lvlx",     "lvlx",        NULL, "lvlx (Cell unaligned-left VR load)", 0, P | BP, NULL, -1, 0 },
  { "stw",      "stw",         NULL, "stw (GPR store)", 0, P | U | BC, NULL, -1, 0 },
  { "std",      "std",         NULL, "std (64-bit GPR store)", 0, P | BC, NULL, -1, 0 },
  { "stwu",     "stwu",        NULL, "stwu (update-form store)", 0, P, NULL, -1, 0 },
  { "stfd",     "stfd",        NULL, "stfd (FPR store)", 0, P | U | BC, NULL, -1, 0 },
  { "stfiwx",   "stfiwx",      NULL, "stfiwx (FPR integer word store)", 0, BC, NULL, -1, 0 },
  { "stvx",     "stvx128",     NULL, "stvx128 (VR store)", 0, P | U | BC, NULL, -1, 0 },
  { "fp",       "faddx",       NULL, "fadd (scalar FP, 10-cycle tier)", 0, P | U | BP | BC | SC_FPARITH, NULL, -1, 0 },
  { "fma",      "fmaddx",      NULL, "fmadd (scalar FMA)", 0, P | BP | BC | SC_FPARITH, NULL, -1, 0 },
  { "fconv",    "fctiwzx",     NULL, "fctiwz (FP -> integer in FPR)", 0, P | BP | BC | SC_INTVAL | SC_FPARITH, NULL, -1, 0 },
  { "fcmp",     "fcmpu",       NULL, "fcmpu (FP compare -> CR: 1.0 < 1.5 sets LT)", 0, P | BP | BC | SC_FPARITH, NULL, -1, 0 },
  { "fdiv",     "fdivx",       NULL, "fdiv (unpipelined)", 0, P | U | BP | BC | SC_FPARITH, "FA", -1, 0 },
  { "fsqrt",    "fsqrtx",      NULL, "fsqrt (unpipelined)", 0, U, NULL, -1, 0 },
  { "vlogic",   "vand128",     NULL, "vand128 (vector simple, 4-cycle tier)", 0, P | U | BP | BC, NULL, -1, 0 },
  { "vint",     "vadduwm",     NULL, "vadduwm (vector simple integer, legacy VR)", 0, P | BP | BC, NULL, -1, 0 },
  { "vperm",    "vperm128",    NULL, "vperm128 (permute)", 0, P | U | BP | BC, NULL, -1, 0 },
  { "vsplt",    "vspltw128",   NULL, "vspltw128 (splat)", 0, P | BP | BC, NULL, -1, 0 },
  { "vfp",      "vaddfp128",   NULL, "vaddfp128 (vector FP, 12-cycle tier)", 0, P | U | BP | BC | SC_FPARITH, NULL, -1, 0 },
  { "vfma",     "vmaddfp128",  NULL, "vmaddfp128 (vector FMA, tied accumulator)", 0, P | BP | BC | SC_FPARITH, NULL, -1, 0 },
  { "vest",     "vrefp128",    NULL, "vrefp128 (estimate, 14-cycle tier)", 0, P | U | BP | BC | SC_FPARITH, NULL, -1, 0 },
  { "vdot",     "vmsum4fp128", NULL, "vmsum4fp128 (dot product, 14-cycle tier)", 0, P | BP | BC | SC_FPARITH, NULL, -1, 0 },
  { "vconv",    "vcfpsxws128", NULL, "vcfpsxws128 (FP -> int conversion, 12-cycle tier)", 0, P | BP | BC | SC_INTVAL | SC_FPARITH, NULL, -1, 0 },
  { "vcmp",     "vcmpeqfp128", NULL, "vcmpeqfp128 (vector compare, no CR)", 0, P, NULL, -1, 0 },
  { "vcmp_rc",  "vcmpequw128", NULL, "vcmpequw128. (vector compare -> CR6)", XTS_RC, BP, NULL, 6, 2 },
  { "crlogic",  "creqv",       NULL, "creqv (CR logical)", 0, P | U | BP | BC, NULL, -1, 2 },
  { "mcrf",     "mcrf",        NULL, "mcrf (CR field move)", 0, BC, NULL, -1, 2 },
  { "mfcr",     "mfcr",        NULL, "mfcr", 0, P, NULL, -1, 0 },
  { "mtcrf",    "mtcrf",       NULL, "mtcrf (slot fields)", 0, P, NULL, -1, 0 },
  { "mflr",     "mfspr",       "SPR=8", "mflr", 0, P | BP | SC_IDX_OK, NULL, -1, 0 },
  { "mtlr",     "mtspr",       "SPR=8", "mtlr", 0, P | BC, NULL, -1, 0 },
  { "br_t",     "bcx",         "BO=12,BI=30", "bc 12,cr7.eq,.+4 (taken to the next word)", 0, P | U | BC, NULL, -1, 0 },
  { "br_nt",    "bcx",         "BO=4,BI=30", "bc 4,cr7.eq,.+4 (not taken)", 0, P, NULL, -1, 0 },
  { "b",        "bx",          NULL, "b .+4", 0, P, NULL, -1, 0 },
};
#undef P
#undef U
#undef BP
#undef BC
const int sc_nclasses = (int)(sizeof sc_classes / sizeof sc_classes[0]);

/* ------------------------------------------------------------------------------------------------ planner */
static const char *const excluded[] = {   /* user instructions a timing loop cannot run generically */
  "lmw", "stmw", "lswi", "lswx", "stswi", "stswx", "sc", "bclrx", "bcctrx", "eciwx", "ecowx", NULL
};
static const char *const untested_ok[] = { "bx", "bcx", "mfspr", "mtspr", "mftb", NULL };

static int in_list(const char *const *l, const char *s)
{
  for (; *l; l++)
    if (!strcmp(*l, s)) return 1;
  return 0;
}

/* "NAME=v,NAME=v": value for operand `name`, 1 if present */
static int ovr_get(const char *ovr, const char *name, int64_t *v)
{
  size_t nl = strlen(name);
  for (const char *s = ovr; s && *s;) {
    const char *eq = strchr(s, '='), *end = strchr(s, ',');
    if (!eq) return 0;
    if ((size_t)(eq - s) == nl && !strncmp(s, name, nl)) {
      *v = strtoll(eq + 1, NULL, 0);
      return 1;
    }
    if (!end) break;
    s = end + 1;
  }
  return 0;
}

static int64_t imm_default(int field, int *bad)
{
  switch (field) {
  case XTS_F_SIMM16: case XTS_F_UIMM16: case XTS_F_SH5: case XTS_F_SH6: case XTS_F_UIMM5: case XTS_F_SIMM5: return 1;
  case XTS_F_MB5: case XTS_F_MB6: return 0;
  case XTS_F_ME5: return 31;
  case XTS_F_ME6: return 63;
  case XTS_F_SHB: return 4;
  case XTS_F_VPERM128: return 0x1B;
  case XTS_F_IMM: case XTS_F_VD3D0: case XTS_F_VD3D1: case XTS_F_VD3D2: case XTS_F_TO: case XTS_F_L: case XTS_F_STRM: return 0;
  case XTS_F_BO: return 20;
  case XTS_F_LI: case XTS_F_BD: return 4;
  case XTS_F_FPSCRD: return 31;   /* RN low bit: toggles rounding, never an enable bit */
  case XTS_F_TBR: return 268;
  case XTS_F_SPR: return 8;       /* LR; CTR would break the frame's loop */
  case XTS_F_FM: return 1;
  default: *bad = 1; return 0;
  }
}

static int is_data_field(int f) { return f == XTS_F_RS || f == XTS_F_FS || f == XTS_F_VS || f == XTS_F_VS128; }

int sc_plan(int insn, unsigned flags, const char *ovr, sc_plan_t *p)
{
  memset(p, 0, sizeof *p);
  p->insn = insn;
  p->flags = flags;
  p->dest = -1;
  p->region = 4;
  const xts_insn_t *d = xts_insn(insn);
  if (!d) { p->why = "unknown instruction"; return -1; }
  p->nops = d->nops;
  if (d->priv != XTS_PRIV_USER) { p->why = "privileged"; return -1; }
  if (d->hw == XTS_HW_ILLEGAL) { p->why = "illegal on hardware"; return -1; }
  if (flags & ~(unsigned)d->flags) { p->why = "flag variant not accepted"; return -1; }
  if (in_list(excluded, d->mnem)) { p->why = "needs a dedicated kernel (string/multiple, sc, register branch, external control)"; return -1; }
  if (d->hw == XTS_HW_UNTESTED && !in_list(untested_ok, d->mnem)) { p->why = "no hardware evidence and not generically safe"; return -1; }
  int mem = d->unit == XTS_UNIT_MEMORY;
  for (int i = 0; i < d->nops; i++) {
    const xts_operand_t *o = &d->ops[i];
    if (mem && o->field == XTS_F_RA && (o->role & XTS_ROLE_W)) p->is_update = 1;
    if (mem && (o->role & XTS_ROLE_R) && !(o->role & XTS_ROLE_W) && is_data_field(o->field)) p->is_store = 1;
  }
  p->is_branch = d->unit == XTS_UNIT_BRANCH && d->asm_name[0] == 'b';
  int nsrc[5] = { 0 };
  for (int i = 0; i < d->nops; i++) {
    const xts_operand_t *o = &d->ops[i];
    int64_t v;
    p->maxreg[i] = 31;
    if (ovr_get(ovr, o->name, &v)) { p->kind[i] = SC_K_IMM; p->imm[i] = v; continue; }
    int file = SC_F_NONE;
    switch (o->cls) {
    case XTS_OC_GPR: case XTS_OC_GPR0: file = SC_F_GPR; break;
    case XTS_OC_FPR: file = SC_F_FPR; break;
    case XTS_OC_VR: file = SC_F_VR; break;
    case XTS_OC_VR128: file = SC_F_VR; p->maxreg[i] = 127; break;
    case XTS_OC_VRC128: file = SC_F_VR; p->maxreg[i] = 7; break;
    default: break;
    }
    p->file[i] = (uint8_t)file;
    if (file != SC_F_NONE) {
      if ((o->role & XTS_ROLE_TIED) && !(o->role & XTS_ROLE_W)) p->kind[i] = SC_K_TIED;
      else if (o->role & XTS_ROLE_W) {
        if (mem && o->field == XTS_F_RA) p->kind[i] = SC_K_UPD;
        else if (p->dest < 0) { p->kind[i] = SC_K_DEST; p->dest = i; p->dest_file = file; }
        else p->kind[i] = SC_K_DEST;
      } else if (mem && (o->field == XTS_F_RA0 || o->field == XTS_F_RA)) p->kind[i] = SC_K_BASE;
      else if (mem && o->field == XTS_F_RB) p->kind[i] = SC_K_INDEX;
      else if (mem && is_data_field(o->field)) { p->kind[i] = SC_K_DATA; p->ord[i] = (uint8_t)nsrc[file]++; }
      else { p->kind[i] = SC_K_SRC; p->ord[i] = (uint8_t)nsrc[file]++; }
      continue;
    }
    int bad = 0;
    switch (o->cls) {
    case XTS_OC_CRF:
      if (!strcmp(d->mnem, "mtfsfix")) { p->kind[i] = SC_K_IMM; p->imm[i] = 7; break; }   /* FPSCR field, not CR */
      p->file[i] = SC_F_CR;
      if (o->role & XTS_ROLE_W) { p->kind[i] = SC_K_CRF_D; if (p->dest < 0) { p->dest = i; p->dest_file = SC_F_CR; } }
      else p->kind[i] = SC_K_CRF_S;
      break;
    case XTS_OC_CRB:
      p->file[i] = SC_F_CR;
      if (o->role & XTS_ROLE_W) { p->kind[i] = SC_K_CRB_D; if (p->dest < 0) { p->dest = i; p->dest_file = SC_F_CR; } }
      else p->kind[i] = SC_K_CRB_S;
      break;
    case XTS_OC_DISP:
      p->kind[i] = SC_K_DISP;
      break;
    case XTS_OC_MASK:
      if (o->field == XTS_F_CRM) p->kind[i] = SC_K_CRM;
      else { p->kind[i] = SC_K_IMM; p->imm[i] = 1; }
      break;
    case XTS_OC_SR:
      p->why = "segment register operand";
      return -1;
    default:
      p->kind[i] = SC_K_IMM;
      p->imm[i] = imm_default(o->field, &bad);
      if (bad) { p->why = "operand without a safe default (NB/LEV/...)"; return -1; }
      break;
    }
  }
  if (p->dest >= 0 && p->dest_file != SC_F_CR && mem) p->is_load = 1;
  if (p->is_load) p->region = p->dest_file == SC_F_GPR ? 3 : 6;
  /* rotation length: enough destinations to cover the latency at the issue rate (WAW-free) */
  p->k = 6;
  if (p->dest >= 0) {
    switch (p->dest_file) {
    case SC_F_GPR: p->k = p->is_update ? 3 : 6; break;
    case SC_F_FPR: p->k = 12; break;
    case SC_F_VR: p->k = p->maxreg[p->dest] >= 127 ? 16 : 10; break;
    case SC_F_CR: p->k = 2; break;
    }
  } else if (p->is_update) p->k = 3;
  return 0;
}

/* ------------------------------------------------------------------------------------------------ classes */
static int classes_done;

int sc_class_adhoc(sc_class_t *c, int insn, unsigned flags, const char *ovr)
{
  c->insn = insn;
  return sc_plan(insn, flags, ovr, &c->plan);
}

int sc_classes_init(void)
{
  int n = 0;
  if (!classes_done) {
    for (int i = 0; i < sc_nclasses; i++) {
      sc_class_t *c = &sc_classes[i];
      c->insn = xts_isa_find(c->rep);
      if (c->insn < 0) { memset(&c->plan, 0, sizeof c->plan); c->plan.why = "representative not in the ISA table"; continue; }
      if (sc_plan(c->insn, c->flags, c->ovr, &c->plan) < 0) c->insn = -1;
    }
    classes_done = 1;
  }
  for (int i = 0; i < sc_nclasses; i++) n += sc_class_ok(i);
  return n;
}

int sc_class_find(const char *name)
{
  for (int i = 0; i < sc_nclasses; i++)
    if (!strcmp(sc_classes[i].name, name)) return i;
  return -1;
}

int sc_class_ok(int ci) { return ci >= 0 && ci < sc_nclasses && sc_classes[ci].insn >= 0 && !sc_classes[ci].plan.why; }

double sc_class_est(const sc_class_t *c)
{
  const xts_insn_t *d = xts_insn(c->insn);
  if (d && d->issue > 0) return d->issue;
  return 2.0;
}

/* ------------------------------------------------------------------------------------------------ registers */
static const uint8_t gbank[3][6] = { { 10, 11, 12, 14, 15, 16 }, { 17, 18, 19, 20, 21, 22 }, { 23, 24, 25, 26, 23, 24 } };

static int bank_reg(int file, int maxreg, int slot, int n, int update)
{
  slot &= 1;
  switch (file) {
  case SC_F_GPR: return update ? gbank[slot][n % 3] : gbank[slot][n % 6];
  case SC_F_FPR: return 4 + slot * 12 + n % 12;
  case SC_F_VR:
    if (maxreg <= 7) return n % 4;
    if (maxreg <= 31) return 12 + slot * 10 + n % 10;
    return 64 + slot * 16 + n % 16;
  case SC_F_CR: return (slot ? 4 : 2) + n % 2;
  }
  return 0;
}

static int const_reg(int file, int ord, int maxreg)
{
  switch (file) {
  case SC_F_GPR: return ord == 0 ? 8 : ord == 1 ? 9 : 31;
  case SC_F_FPR: return ord > 2 ? 2 : ord;
  case SC_F_VR: return maxreg <= 7 ? 4 : 5 + (ord > 2 ? 2 : ord);
  }
  return 0;
}

static int region_sel(int base_reg) { return base_reg == 3 ? 0 : base_reg == 4 ? 1 : base_reg == 6 ? 2 : 3; }

/* emission state (one body at a time) */
static struct {
  int ctr[3];                     /* rotation counters per bank */
  int last[SC_MAX_ITEMS];         /* rotation index of each item's last instance */
} es;

static const sc_class_t *item_class(const sc_item_t *it)
{
  if (it->cls >= 0 && it->cls < sc_nclasses) return &sc_classes[it->cls];
  return it->adhoc;
}

/* destination register (CR: field) of item `ii` at rotation index n; for_link: what a linked consumer reads (record
 * forms: their implicit CR field) */
static int item_dest(const sc_unit_t *u, int ii, int n, int *file, int for_link)
{
  const sc_item_t *it = &u->it[ii];
  if (it->cls == SC_FILLER) { if (file) *file = SC_F_GPR; return gbank[2][n % 4]; }
  const sc_class_t *c = item_class(it);
  const sc_plan_t *p = &c->plan;
  if (for_link && c->crimp >= 0 && c->crimp <= 7) { if (file) *file = SC_F_CR; return c->crimp; }
  if (p->dest < 0) { if (file) *file = SC_F_NONE; return -1; }
  int maxreg = p->maxreg[p->dest];
  /* a link into a more restrictive operand class (VC128 v0-v7, 5-bit VR) narrows the producer's bank */
  for (int k = 0; k < u->nitems; k++)
    if (u->it[k].link_from == ii && u->it[k].link != SC_LINK_NONE) {   /* dep AND ind twins: same producer words */
      const sc_class_t *cc = item_class(&u->it[k]);
      if (cc && cc->plan.maxreg[u->it[k].link_op] < maxreg) maxreg = cc->plan.maxreg[u->it[k].link_op];
    }
  if (file) *file = p->dest_file;
  return bank_reg(p->dest_file, maxreg, it->slot, n, p->is_update);
}

static void emit_filler(xts_kbuf_t *k)
{
  int n = es.ctr[2]++;
  xk_emit(k, XI_addi, 0, gbank[2][n % 4], 0, n & 0x7FFF);
}

static void emit_item(xts_kbuf_t *k, const sc_unit_t *u, int ii)
{
  const sc_item_t *it = &u->it[ii];
  if (it->cls == SC_FILLER) { emit_filler(k); return; }
  const sc_class_t *c = item_class(it);
  const sc_plan_t *p = &c->plan;
  const xts_insn_t *d = &xts_isa[c->insn];
  int slot = it->slot & 1;
  int n = es.ctr[slot]++;
  es.last[ii] = n;
  int64_t ops[XTS_MAX_OPS];
  int dreg = p->dest >= 0 ? item_dest(u, ii, n, NULL, 0) : 0;
  int linked_src = -1, linked_file = SC_F_NONE;
  if (it->link == SC_LINK_DEP && it->link_from >= 0) {
    linked_src = item_dest(u, it->link_from, es.last[it->link_from], &linked_file, 1);
    /* accumulator links (tied VD128/VS128): the consumer accumulates into the producer's register */
    if (linked_src >= 0 && it->link_op >= 0 && (it->link_op == p->dest || p->kind[it->link_op] == SC_K_TIED)) dreg = linked_src;
  }
  for (int i = 0; i < p->nops; i++) {
    int64_t v = 0;
    int kind = p->kind[i], file = p->file[i];
    if (i == it->link_op && it->link == SC_LINK_DEP && linked_src >= 0) {
      if (kind == SC_K_CRB_S || (kind == SC_K_IMM && d->ops[i].cls == XTS_OC_CRB)) v = linked_src * 4 + it->crsel;
      else v = linked_src;
      ops[i] = v;
      continue;
    }
    switch (kind) {
    case SC_K_IMM: v = p->imm[i]; break;
    case SC_K_DEST: v = i == p->dest ? dreg : bank_reg(file, p->maxreg[i], slot, n + 1, 0); break;
    case SC_K_TIED: v = dreg; break;
    case SC_K_SRC:
    case SC_K_DATA:
      v = const_reg(file, p->ord[i], p->maxreg[i]);
      if (file == SC_F_GPR && it->mode == SC_MODE_IDX) v = 7;
      if (file == SC_F_GPR && it->mode == SC_MODE_PTR) v = p->ord[i] == 0 ? it->ptr_region : 7;
      break;
    case SC_K_BASE:
      v = p->region;
      if (p->is_load && it->mode == SC_MODE_PTR) v = 5;
      break;
    case SC_K_INDEX: v = 7; break;
    case SC_K_UPD: v = gbank[slot][3 + n % 3]; break;
    case SC_K_DISP:
      if (p->is_update) v = 0;
      else if (p->is_load && it->mode == SC_MODE_PTR) v = 8 * (4 * (n % 4) + region_sel(it->ptr_region));
      else if (p->is_load) v = (n % 8) * 16;
      else v = slot * 64 + (n % 4) * 16;
      break;
    case SC_K_CRF_D: v = bank_reg(SC_F_CR, 7, slot, n, 0); break;
    case SC_K_CRF_S: v = 7; break;
    case SC_K_CRB_D: v = bank_reg(SC_F_CR, 7, slot, n, 0) * 4 + 2; break;
    case SC_K_CRB_S: v = 31; break;
    case SC_K_CRM:
      v = (d->ops[i].role & XTS_ROLE_W) && !strcmp(d->mnem, "mtcrf") ? (slot ? 0x0C : 0x30) : (slot ? 0x08 : 0x20);
      break;
    }
    ops[i] = v;
  }
  xk_emit_a(k, c->insn, p->flags, ops, p->nops);
}

int sc_unit_words(const sc_unit_t *u)
{
  if (u->special) return 3 + u->special_dist;
  int w = 0;
  for (int i = 0; i < u->nitems; i++) w += u->it[i].count;
  return w;
}

void sc_unit_init(sc_unit_t *u) { memset(u, 0, sizeof *u); }

sc_item_t *sc_unit_add(sc_unit_t *u, int cls, int slot, int count)
{
  if (u->nitems >= SC_MAX_ITEMS) return NULL;
  sc_item_t *it = &u->it[u->nitems++];
  memset(it, 0, sizeof *it);
  it->cls = cls;
  it->slot = slot;
  it->count = count;
  it->link_from = -1;
  it->link_op = -1;
  return it;
}

/* special bodies: register branches whose target is the next unit's first word.
 * pre:   bcl 20,31,.+4 ; mflr r17          (r17 = address of the mflr word = base_word)
 * unit:  addi r18,r17,off ; mt{ctr,lr} r18 ; F x dist ; b{c,l}r | b .+4
 * The CTR form is measured in the GPR loop frame (the counter is r29, CTR belongs to the unit), so it needs no save and
 * restore of the frame's counter; the LR form uses the same unit (3 + dist words), so dep/ind twins of both have equal length. */
static int sc_pre(xts_kbuf_t *k, int depth, void *arg)
{
  sc_unit_t *u = arg;
  (void)depth;
  if (!u->special) return 0;
  xk_emit(k, XI_bcx, XTS_LK, 20, 31, 4);
  u->base_word = (int)k->n;
  xk_emit(k, XI_mfspr, 0, 17, 8);
  return 0;
}

static void emit_special(xts_kbuf_t *k, sc_unit_t *u)
{
  int ctr = u->special == 1;
  int target = (int)k->n + 3 + u->special_dist;         /* word after the branch */
  xk_emit(k, XI_addi, 0, 18, 17, (target - u->base_word) * 4);
  xk_emit(k, XI_mtspr, 0, ctr ? 9 : 8, 18);
  for (int i = 0; i < u->special_dist; i++) emit_filler(k);
  if (u->special_dep) xk_emit(k, ctr ? XI_bcctrx : XI_bclrx, 0, 20, 0);
  else xk_emit(k, XI_bx, 0, 4);
}

static int sc_body(xts_kbuf_t *k, int depth, void *arg)
{
  sc_unit_t *u = arg;
  memset(&es, 0, sizeof es);
  for (int j = 0; j < depth; j++) {
    if (u->special) { emit_special(k, u); continue; }
    for (int i = 0; i < u->nitems; i++)
      for (int c = 0; c < u->it[i].count; c++) emit_item(k, u, i);
  }
  return k->err ? -1 : 0;
}

int sc_setup(xts_exec_t *x, void *arg)
{
  (void)arg;
  xts_set_ptr(x, 3, 128);
  xts_set_ptr(x, 4, 256);
  xts_set_ptr(x, 5, 0);
  xts_set_ptr(x, 6, 384);
  xts_set_gpr(x, 7, 0);
  xts_set_gpr(x, 8, 0x1234);
  xts_set_gpr(x, 9, 3);
  xts_set_gpr(x, 31, 0x55);
  for (int s = 0; s < 2; s++)
    for (int i = 0; i < 6; i++) {
      if (i < 3) xts_set_gpr(x, gbank[s][i], 0x77);
      else xts_set_ptr(x, gbank[s][i], 256);              /* update-form bases: STORE region */
    }
  for (int i = 23; i <= 26; i++) xts_set_gpr(x, i, 0);
  xts_set_fpr(x, 0, 0x3FF0000000000000ull);
  xts_set_fpr(x, 1, 0x3FF8000000000000ull);
  xts_set_fpr(x, 2, 0x3FE8000000000000ull);
  for (int i = 3; i < 32; i++) xts_set_fpr(x, i, 0x3FF0000000000000ull);
  for (int i = 0; i < 128; i++) xts_set_vr(x, i, xts_vr4(0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000));
  xts_set_vr(x, 4, xts_vr4(0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F));
  xts_set_vr(x, 6, xts_vr4(0x3FC00000, 0x3FC00000, 0x3FC00000, 0x3FC00000));
  xts_set_vr(x, 7, xts_vr4(0x3F400000, 0x3F400000, 0x3F400000, 0x3F400000));
  xts_set_vr(x, 8, xts_vr4(1, 2, 3, 4));
  xts_set_vr(x, 9, xts_vr4(0, 0, 0, 0));
  xts_set_cr(x, 0x00000002);                                /* cr7 = EQ */
  xts_set_lr(x, 0);
  xts_set_xer(x, 0);
  static const int tgt[4] = { 128, 256, 384, 0 };
  for (int i = 0; i < 16; i++) xts_set_mem_ptr(x, i * 8, tgt[i % 4], 0);
  for (int off = 128; off < 384; off += 8) xts_set_mem_be(x, off, 8, 0);
  for (int off = 384; off < 512; off += 4) xts_set_mem_be(x, off, 4, 0x3F800000);
  return 0;
}

/* ------------------------------------------------------------------------------------------------ measurement */
static void unit_spec(sc_unit_t *u, xts_tspec_t *s)
{
  *s = (xts_tspec_t)XTS_TSPEC_DEFAULT;
  s->unit = "sched unit";
  s->body = sc_body;
  s->pre = u->special ? sc_pre : NULL;
  s->setup = sc_setup;
  s->arg = u;
  s->loop = u->special == 1 ? XTS_LOOP_GPR : XTS_LOOP_CTR;   /* mtctr -> bctr uses CTR itself */
}

int sc_measure(xts_ctx_t *ctx, sc_unit_t *u, double est, sc_meas_t *m)
{
  memset(m, 0, sizeof *m);
  m->cyc = m->err = NAN;
  m->r.err_cycles = NAN;
  m->status = XTS_ST_SKIP;
  int w = sc_unit_words(u);
  u->words = w;
  unit_spec(u, &m->s);
  xts_tspec_t *s = &m->s;
  if (w <= 0) {
    snprintf(m->r.why, sizeof m->r.why, "empty unit");
    m->r.failed = 1;
    m->status = XTS_ST_FAIL;
    return -1;
  }
  int dmax = 2048 / w;
  if (dmax > 48) dmax = 48;
  if (dmax < 6) dmax = 6;
  s->ndepths = 6;
  for (int i = 0; i < 6; i++) {
    int dv = (int)(dmax * (i + 1) / 6.0 + 0.5);
    if (dv < 1) dv = 1;
    if (i && dv <= s->depths[i - 1]) dv = s->depths[i - 1] + 1;
    s->depths[i] = dv;
  }
  if (!(est > 0) || isnan(est)) est = 2.0 * w;
  double c0 = est * s->depths[0] + 12;
  double it = ceil(131072.0 / c0);
  if (it < 32) it = 32;
  if (it > 8192) it = 8192;
  s->iters = (uint32_t)it;
  int e = xts_time_measure(s, ctx, &m->r);
  if (e < 0) { m->status = XTS_ST_FAIL; return -1; }
  m->ok = m->r.ok;
  m->status = m->r.status;
  if (!m->ok) return 0;
  m->cyc = m->r.cycles_per_unit;
  m->err = m->r.err_cycles;
  return 0;
}

int sc_build(sc_unit_t *u, int depth, xts_kbuf_t *k)
{
  xts_tspec_t s;
  u->words = sc_unit_words(u);
  unit_spec(u, &s);
  return xts_time_build(&s, depth, k);
}

uint64_t sc_cond_key(const xts_ctx_t *ctx)
{
  char buf[512];
  buf[0] = 0;
  if (ctx && ctx->cond) xts_cond_format(ctx->cond, buf, sizeof buf, 1);
  return xts_hash_u64(xts_hash_str(0x5C4ED, buf), ctx ? (uint64_t)ctx->repeat : 0);
}

const char *sc_tri(double v, double hi, double lo, const char *yes, const char *no, const char *mid)
{
  if (isnan(v)) return NULL;
  return v >= hi ? yes : v <= lo ? no : mid;
}
