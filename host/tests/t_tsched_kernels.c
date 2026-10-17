/* Host tests for the scheduling families' kernel builder (src/tests/sched_classes.c, owner tsched):
 *   classes      every class representative resolves and plans
 *   pair_indep   every ordered pair of issue.pair classes: the two members share no register (no RAW/WAR/WAW between
 *                them), so a pair kernel measures issue, not dependences
 *   links        bypass dep/ind twins differ only in the linked consumer operand; dep reads the producer's destination
 *                (address producers reproduce the consumer's base pointer, CR links read the produced bit)
 *   regbranch    mtctr->bctr / mtlr->blr units branch to the word after the branch; the CTR form runs in the GPR loop
 *                frame with no CTR save/restore
 *   rline        the result-line reader (xts/rline.h) as the bypass distance summaries and the unit.conflict graph use it
 * TSCHED_DUMP=1 prints the bodies. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include "htest.h"
#include "../../src/tests/sched_classes.h"

static xts_kbuf_t *kb(void)
{
  static xts_kbuf_t *k;
  if (!k) k = xk_new(65536);
  return k;
}

/* body range of a framed kernel: [start, end) before the loop closure. CTR frame: the closure is the last bdnz (BO 16).
 * GPR frame (register-branch units through CTR): addi r29,r29,-1 ; cmpwi ; bne (BO 4), the body ends two words before it. */
static int body_range(const xts_kbuf_t *k, int depth, int words, int *start)
{
  for (int i = (int)k->n - 1; i >= 0; i--) {
    if (k->tag[i] != XI_bcx) continue;
    int bo = (int)xts_extract(XI_bcx, k->w[i], 0);
    if (bo == 16) {
      *start = i - depth * words;
      return i;
    }
    if (bo == 4 && k->tag[i - 1] == XI_cmpi && k->tag[i - 2] == XI_addi) {
      *start = i - 2 - depth * words;
      return i - 2;
    }
  }
  return -1;
}

typedef struct { int n; int r[64]; } regset_t;
static void rs_add(regset_t *s, int v) { for (int i = 0; i < s->n; i++) if (s->r[i] == v) return; if (s->n < 64) s->r[s->n++] = v; }
static int rs_meet(const regset_t *a, const regset_t *b)
{
  for (int i = 0; i < a->n; i++)
    for (int k = 0; k < b->n; k++)
      if (a->r[i] == b->r[k]) return a->r[i];
  return -1;
}

/* explicit register reads / writes of one word (file*256 + number; CR as fields) */
static void word_regs(int id, uint32_t w, regset_t *rd, regset_t *wr)
{
  const xts_insn_t *d = &xts_isa[id];
  rd->n = wr->n = 0;
  int mem = d->unit == XTS_UNIT_MEMORY;
  for (int i = 0; i < d->nops; i++) {
    const xts_operand_t *o = &d->ops[i];
    int64_t v = xts_extract(id, w, i);
    int key = -1;
    switch (o->cls) {
    case XTS_OC_GPR: key = 256 + (int)v; break;
    case XTS_OC_GPR0: if (v) key = 256 + (int)v; break;
    case XTS_OC_FPR: key = 512 + (int)v; break;
    case XTS_OC_VR: case XTS_OC_VR128: case XTS_OC_VRC128: key = 768 + (int)v; break;
    case XTS_OC_CRF: if (strcmp(d->mnem, "mtfsfix")) key = 1024 + (int)v; break;
    case XTS_OC_CRB: key = 1024 + (int)(v / 4); break;
    default: break;
    }
    if (key < 0) continue;
    if (o->role & XTS_ROLE_R) rs_add(rd, key);
    if (o->role & XTS_ROLE_W) rs_add(wr, key);
    (void)mem;
  }
}

static void dump(const xts_kbuf_t *k, int from, int to, const char *title)
{
  if (!getenv("TSCHED_DUMP")) return;
  printf("    -- %s\n", title);
  char b[96];
  for (int i = from; i < to; i++) printf("      %4d %08X  %s\n", i, k->w[i], xts_disasm(k->w[i], b, sizeof b));
}

HTEST(tsched_classes)
{
  int n = sc_classes_init();
  HT_CHECK(n == sc_nclasses);
  for (int i = 0; i < sc_nclasses; i++)
    if (!sc_class_ok(i)) printf("    class %s: %s\n", sc_classes[i].name, sc_classes[i].plan.why ? sc_classes[i].plan.why : "?");
  HT_CHECK(sc_class_find("filler") >= 0 && sc_class_find("vfp") >= 0 && sc_class_find("nonexistent") < 0);
}

HTEST(tsched_pair_indep)
{
  sc_classes_init();
  int bad = 0, pairs = 0;
  for (int a = 0; a < sc_nclasses; a++)
    for (int b = 0; b < sc_nclasses; b++) {
      if (!sc_class_ok(a) || !sc_class_ok(b) || !(sc_classes[a].use & SC_U_PAIR) || !(sc_classes[b].use & SC_U_PAIR)) continue;
      sc_unit_t u;
      sc_unit_init(&u);
      sc_unit_add(&u, a, 0, 1);
      sc_unit_add(&u, b, 1, 1);
      xts_kbuf_t *k = kb();
      int depth = 12, start;
      HT_CHECK(sc_build(&u, depth, k) == 0);
      int end = body_range(k, depth, 2, &start);
      HT_CHECK(end > 0 && start >= 0);
      pairs++;
      regset_t ra, wa, rb, wb, wa_all = { 0 }, wb_all = { 0 }, ra_all = { 0 }, rb_all = { 0 };
      for (int i = start; i < end; i += 2) {
        word_regs(k->tag[i], k->w[i], &ra, &wa);
        word_regs(k->tag[i + 1], k->w[i + 1], &rb, &wb);
        for (int q = 0; q < wa.n; q++) rs_add(&wa_all, wa.r[q]);
        for (int q = 0; q < wb.n; q++) rs_add(&wb_all, wb.r[q]);
        for (int q = 0; q < ra.n; q++) rs_add(&ra_all, ra.r[q]);
        for (int q = 0; q < rb.n; q++) rs_add(&rb_all, rb.r[q]);
      }
      int x1 = rs_meet(&wa_all, &rb_all), x2 = rs_meet(&wb_all, &ra_all), x3 = rs_meet(&wa_all, &wb_all);
      if (x1 >= 0 || x2 >= 0 || x3 >= 0) {
        if (bad++ < 10)
          printf("    %s>%s share register %d/%d/%d\n", sc_classes[a].name, sc_classes[b].name, x1, x2, x3);
      }
      if (a == sc_class_find("vperm") && b == sc_class_find("lwzu")) dump(k, start, start + 6, "vperm>lwzu");
    }
  HT_CHECK(pairs > 1000);
  HT_EQ_U64(bad, 0);
}

/* build dep and ind for P -> C operand `opname`; check they differ only there and dep reads P's dest */
static void check_link(const char *pn, const char *cn, const char *opname, int mode, int expect_val)
{
  int pi = sc_class_find(pn), ci = sc_class_find(cn);
  HT_CHECK(sc_class_ok(pi) && sc_class_ok(ci));
  if (!sc_class_ok(pi) || !sc_class_ok(ci)) return;
  int op = xts_op_index(&xts_isa[sc_classes[ci].insn], opname);
  HT_CHECK(op >= 0);
  static uint32_t dw[4096];
  int depth = 10, start = 0, end = 0;
  for (int v = 0; v < 2; v++) {
    sc_unit_t u;
    sc_unit_init(&u);
    sc_item_t *p = sc_unit_add(&u, pi, 0, 1);
    p->mode = mode;
    p->ptr_region = sc_classes[ci].plan.region;
    sc_item_t *c = sc_unit_add(&u, ci, 1, 1);
    c->link_from = 0;
    c->link_op = op;
    c->link = v ? SC_LINK_IND : SC_LINK_DEP;
    c->crsel = sc_classes[pi].crbit;
    xts_kbuf_t *k = kb();
    HT_CHECK(sc_build(&u, depth, k) == 0);
    int s0;
    end = body_range(k, depth, 2, &s0);
    char t[64];
    snprintf(t, sizeof t, "%s>%s.%s %s", pn, cn, opname, v ? "ind" : "dep");
    dump(k, s0, s0 + 4, t);
    if (!v) { start = s0; memcpy(dw, k->w, sizeof(uint32_t) * k->n); continue; }
    HT_CHECK(s0 == start);
    int diffs = 0, linked_ok = 1;
    for (int i = start; i < end; i++) {
      if (dw[i] == k->w[i]) continue;
      diffs++;
      HT_CHECK((i - start) % 2 == 1);                       /* only consumer words differ */
      for (int q = 0; q < xts_isa[k->tag[i]].nops; q++)
        if (q != op && xts_extract(k->tag[i], dw[i], q) != xts_extract(k->tag[i], k->w[i], q)) {
          /* tied accumulators move with the linked operand */
          if (!(xts_isa[k->tag[i]].ops[q].role & XTS_ROLE_TIED)) linked_ok = 0;
        }
      int pd = sc_classes[pi].plan.dest;
      int64_t pv = sc_classes[pi].crimp >= 0 ? sc_classes[pi].crimp : xts_extract(k->tag[i - 1], dw[i - 1], pd);
      int64_t cv = xts_extract(k->tag[i], dw[i], op);
      if (xts_isa[k->tag[i]].ops[op].cls == XTS_OC_CRB) pv = pv * 4 + sc_classes[pi].crbit;
      if (cv != pv) linked_ok = 0;
      if (expect_val >= 0 && mode == SC_MODE_PTR) {     /* producer reproduces the consumer's ind base register */
        int64_t base = xts_extract(k->tag[i], k->w[i], op);
        if (base != expect_val) linked_ok = 0;
      }
    }
    HT_EQ_U64(diffs, depth);
    if (!linked_ok) printf("    link %s>%s.%s wrong\n", pn, cn, opname);
    HT_CHECK(linked_ok);
  }
}

HTEST(tsched_links)
{
  sc_classes_init();
  check_link("int", "lwz", "RA0", SC_MODE_PTR, 3);
  check_link("ld", "stw", "RA0", SC_MODE_PTR, 4);
  check_link("lwz", "lwzx", "RB", SC_MODE_IDX, -1);
  check_link("int", "mul", "RB", SC_MODE_DATA, -1);
  check_link("vfp", "vperm", "VC128", SC_MODE_DATA, -1);
  check_link("lvx", "vint", "VA", SC_MODE_DATA, -1);
  check_link("vfp", "vfma", "VD128", SC_MODE_DATA, -1);
  check_link("fp", "stfd", "FS", SC_MODE_DATA, -1);
  check_link("cmp", "br_t", "BI", SC_MODE_IDX, -1);
  check_link("int_rc", "br_t", "BI", SC_MODE_IDX, -1);
  check_link("vcmp_rc", "crlogic", "CRBA", SC_MODE_IDX, -1);
  check_link("fcmp", "mcrf", "CRFS", SC_MODE_IDX, -1);
  /* ld in PTR mode loads the stored pointer of the consumer's region: disp selects slot (4*i + sel)*8 */
  sc_unit_t u;
  sc_unit_init(&u);
  sc_item_t *p = sc_unit_add(&u, sc_class_find("ld"), 0, 1);
  p->mode = SC_MODE_PTR;
  p->ptr_region = 4;
  xts_kbuf_t *k = kb();
  HT_CHECK(sc_build(&u, 8, k) == 0);
  int s0, e = body_range(k, 8, 1, &s0);
  for (int i = s0; i < e; i++) {
    HT_EQ_U64(xts_extract(XI_ld, k->w[i], 2), 5);
    HT_EQ_U64((xts_extract(XI_ld, k->w[i], 1) / 8) % 4, 1);
  }
}

HTEST(tsched_regbranch)
{
  for (int sp = 1; sp <= 2; sp++)
    for (int dep = 0; dep < 2; dep++) {
      sc_unit_t u;
      sc_unit_init(&u);
      u.special = sp;
      u.special_dep = dep;
      u.special_dist = 3;
      xts_kbuf_t *k = kb();
      int depth = 5;
      HT_CHECK(sc_build(&u, depth, k) == 0);
      int s0, e = body_range(k, depth, sc_unit_words(&u), &s0);
      HT_CHECK(e > 0);
      dump(k, s0, s0 + sc_unit_words(&u), sp == 1 ? (dep ? "mtctr>bctr" : "mtctr>b") : (dep ? "mtlr>blr" : "mtlr>b"));
      for (int i = s0; i < e; i++) {
        if (k->tag[i] != XI_addi || xts_extract(XI_addi, k->w[i], 1) != 17) continue;
        int target = u.base_word + (int)xts_extract(XI_addi, k->w[i], 2) / 4;
        int br = i + 2 + u.special_dist;
        HT_EQ_U64(target, br + 1);
        HT_CHECK(k->tag[br] == (dep ? (sp == 1 ? XI_bcctrx : XI_bclrx) : XI_bx));
        if (sp == 1) HT_CHECK(k->tag[i + 1] == XI_mtspr && xts_extract(XI_mtspr, k->w[i + 1], 0) == 9);   /* mtctr r18 */
      }
      for (int i = s0; i < e; i++) {
        /* no mfctr/mtctr save-restore in the body any more (the GPR frame leaves CTR to the unit) */
        if (k->tag[i] == XI_mfspr) HT_CHECK(xts_extract(XI_mfspr, k->w[i], 1) != 9);
        if (k->tag[i] == XI_mtspr && xts_extract(XI_mtspr, k->w[i], 0) == 9) HT_CHECK(sp == 1 && xts_extract(XI_mtspr, k->w[i], 1) == 18);
      }
      HT_CHECK(k->tag[u.base_word] == XI_mfspr && k->tag[u.base_word - 1] == XI_bcx);
    }
}

HTEST(tsched_rline)
{
  const char *line =
      "{\"run_id\":\"r\",\"seq\":5,\"family\":\"bypass.matrix\",\"case_id\":\"dist.int>lwz.RA0.d4\",\"mnem\":\"lwz\",\"word\":\"\","
      "\"cond\":{\"sf\":\"64\",\"sibling\":\"idle\"},\"block\":\"r2:bypass.matrix:-:c0\",\"metric\":\"penalty\",\"value\":3.5,"
      "\"unit\":\"cycles\",\"n\":60,\"min\":null,\"median\":null,\"mad\":null,\"fit_r2\":1,\"status\":\"noisy\","
      "\"detail\":{\"penalty\":3.5,\"penalty_err\":0.25,\"verdict\":\"stall\",\"nul\":null,\"fit\":{\"status\":\"pass\"}}}";
  xts_rline_t l;
  HT_CHECK(xts_rline_parse(line, &l) == 0 && xts_rline_row(&l) == 2);   /* the family scanners filter on the row */
  HT_CHECK(!strcmp(l.case_id, "dist.int>lwz.RA0.d4") && !strcmp(l.metric, "penalty"));
  HT_CHECK(l.status == XTS_ST_NOISY && l.value == 3.5);
  HT_CHECK(!strcmp(l.cond, "\"sf\":\"64\",\"sibling\":\"idle\""));
  HT_CHECK(xts_rline_num(&l, "penalty_err") == 0.25 && isnan(xts_rline_num(&l, "nul")) && isnan(xts_rline_num(&l, "absent")));
  HT_CHECK(isnan(xts_rline_num(&l, "status")));   /* depth 1 only: the "status" inside the nested fit object is not the detail's */
  char v[16];
  HT_CHECK(xts_rline_str(&l, "verdict", v, sizeof v) && !strcmp(v, "stall") && !xts_rline_str(&l, "absent", v, sizeof v));
  xts_condtab_t t;
  memset(&t, 0, sizeof t);
  HT_CHECK(xts_condtab_add(&t, &l) == 0 && xts_condtab_add(&t, &l) == 0 && t.n == 1);
}

/* every user instruction the planner accepts builds a valid ucode kernel (X ; F*4), and the refusals are the known set */
HTEST(tsched_plan_all)
{
  int planned = 0, refused = 0, bad = 0;
  static sc_class_t X;
  for (int id = 0; id < xts_isa_count; id++) {
    const xts_insn_t *d = &xts_isa[id];
    if (d->priv != XTS_PRIV_USER || d->hw == XTS_HW_ILLEGAL) continue;
    memset(&X, 0, sizeof X);
    X.crimp = -1;
    if (sc_class_adhoc(&X, id, 0, NULL) < 0) { refused++; continue; }
    planned++;
    sc_unit_t u;
    sc_unit_init(&u);
    sc_item_t *it = sc_unit_add(&u, -1, 0, 1);
    it->adhoc = &X;
    sc_unit_add(&u, SC_FILLER, 0, 4);
    xts_kbuf_t *k = kb();
    int ni, nr;
    char why[96];
    if (sc_build(&u, 7, k) < 0 || xk_validate(k, &ni, &nr, why, sizeof why)) {
      if (bad++ < 8) printf("    %s: %s %s\n", d->mnem, k->errmsg, why);
    }
  }
  HT_EQ_U64(bad, 0);
  HT_CHECK(planned >= 380);
  HT_CHECK(refused <= 12);
}
