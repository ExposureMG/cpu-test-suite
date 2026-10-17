/* ldst.lat / mem.hier helpers (src/tests/mem_util.c): form classification, the generic memory emitter, latency-chain
 * geometry walked with each form's own semantics (width, sign, byte reversal, update, X-form index, Cell lvlx/lvrx),
 * pointer chains with jitter/blocks, CR-bit -> V rotation, and the plateau/knee analysis on planted curves. */
#include <math.h>
#include <string.h>
#include <xts/xts.h>
#include "htest.h"
#include "../../src/tests/mem_util.h"

HTEST(tmem_forms)
{
  mu_form_t f;
  HT_CHECK(mu_form_of(XI_lhaux, &f) == 0 && f.file == 'g' && f.load && f.update && f.xform && f.width == 2 && f.sign);
  HT_CHECK(mu_form_of(XI_lwarx, &f) == 0 && f.resv && !f.sign && f.width == 4 && f.align_trap == 4);
  HT_CHECK(mu_form_of(XI_ldbrx, &f) == 0 && f.brev && f.width == 8);
  HT_CHECK(mu_form_of(XI_lwa, &f) == 0 && f.ds && f.sign && f.width == 4);
  HT_CHECK(mu_form_of(XI_lfsu, &f) == 0 && f.file == 'f' && f.update && f.fpsingle && f.align_trap == 4);
  HT_CHECK(mu_form_of(XI_stfiwx, &f) == 0 && !f.load && !f.fpsingle && f.width == 4);
  HT_CHECK(mu_form_of(XI_lvrxl128, &f) == 0 && f.file == 'v' && f.vr128 && f.lr == 2 && f.width == 16);
  HT_CHECK(mu_form_of(XI_lvehx, &f) == 0 && f.elem && f.width == 2 && !f.update);
  HT_CHECK(mu_form_of(XI_stdcx, &f) == 0 && f.resv && !f.load);
  HT_CHECK(mu_form_of(XI_dcbz, &f) < 0);
  HT_CHECK(mu_form_of(XI_lmw, &f) < 0);
  HT_CHECK(mu_form_of(XI_addx, &f) < 0);
  int nl = 0, ns = 0;
  for (int id = 0; id < xts_isa_count; id++)
    if (mu_form_of(id, &f) == 0) { if (f.load) nl++; else ns++; }
  if (nl != 52 || ns != 46) printf("    loads %d stores %d\n", nl, ns);
  HT_CHECK(nl == 52);
  HT_CHECK(ns == 46);
}

HTEST(tmem_emit)
{
  xts_kbuf_t *k = xk_new(64);
  mu_emit_mem(k, XI_lbzux, 4, 3, 5);
  mu_emit_mem(k, XI_lwz, 4, 3, 40);
  mu_emit_mem(k, XI_ld, 3, 3, 8);
  mu_emit_mem(k, XI_stvx128, 72, 16, 5);
  mu_emit_mem(k, XI_stfsx, 8, 3, 4);
  xk_li32(k, 21, 0x12345);
  HT_EQ_U64(k->w[0], xts_enc(XI_lbzux, 0, 4, 3, 5));
  HT_EQ_U64(k->w[1], xts_enc(XI_lwz, 0, 4, 40, 3));
  HT_EQ_U64(k->w[2], xts_enc(XI_ld, 0, 3, 8, 3));
  HT_EQ_U64(k->w[3], xts_enc(XI_stvx128, 0, 72, 16, 5));
  HT_EQ_U64(k->w[4], xts_enc(XI_stfsx, 0, 8, 3, 4));
  HT_EQ_U64(k->w[5], xts_enc(XI_addis, 0, 21, 0, 1));
  HT_EQ_U64(k->w[6], xts_enc(XI_ori, 0, 21, 21, 0x2345));
  HT_CHECK(k->err == 0);
  xk_free(k);
}

/* the value a load of form f reads at p */
static uint64_t sim_load(const mu_form_t *f, const uint8_t *p)
{
  uint64_t v = 0;
  if (f->brev)
    for (int i = f->width - 1; i >= 0; i--) v = (v << 8) | p[i];
  else
    v = xts_be_load(p, f->width);
  if (f->sign) {
    int sh = 64 - 8 * f->width;
    v = (uint64_t)((int64_t)(v << sh) >> sh);
  }
  return v;
}

/* walk n steps the way the ldst.lat kernel does; 0 if every step lands on the next anchor */
static int sim_walk(const mu_chain_t *c, const mu_form_t *f, const mu_geom_t *g, uint32_t doff)
{
  uint8_t *p = c->buf.p;
  uint64_t A = mu_chain_addr_at(c, 0);
  for (uint32_t j = 0; j < c->n; j++) {
    uint64_t ea = A + (uint64_t)g->di, next = mu_chain_addr_at(c, j + 1);
    uint32_t eo = (uint32_t)(ea - c->buf.addr);
    if (f->file != 'v' && (eo - c->base) % g->stride != doff) return -10;          /* the requested misalignment */
    uint64_t step;
    if (f->file == 'g') {
      step = sim_load(f, p + eo);
    } else if (f->file == 'f') {
      uint64_t bits = xts_be_load(p + eo, f->width);
      if (bits != (f->width == 4 ? 0x3F800000ull : 0x3FF0000000000000ull)) return -11;
      step = (uint64_t)g->V;
    } else {
      uint32_t lo, hi;
      if (f->elem) { lo = eo & ~(uint32_t)(f->width - 1); hi = lo + (uint32_t)f->width; }
      else if (f->lr == 1) { lo = eo; hi = (eo | 15) + 1; }
      else if (f->lr == 2) { lo = eo & ~15u; hi = eo; }
      else { lo = eo & ~15u; hi = lo + 16; }
      for (uint32_t q = lo; q < hi; q++)
        if (p[q]) return -12;
      step = (uint64_t)g->V;
    }
    uint64_t base = f->update ? ea : A;
    uint64_t ptr = xts_be_load(p + (uint32_t)(base + step - c->buf.addr), 8);
    if (ptr != next) return -13;
    A = next;
  }
  return 0;
}

HTEST(tmem_lat_geometry)
{
  static mu_chain_t c;
  HT_CHECK(mu_big(MU_BIG_BYTES, &c.buf) == 0);
  int built = 0, walked = 0;
  for (int id = 0; id < xts_isa_count; id++) {
    mu_form_t f;
    if (mu_form_of(id, &f) < 0 || !f.load) continue;
    for (int kind = 0; kind < 7; kind++) {
      static const uint32_t offs[7] = { 0, 1, 4, 0, 0, 0, 0 };
      uint32_t doff = kind < 3 ? offs[kind] : kind == 3 ? 128u - f.width / 2 : kind == 4 ? 4096u - f.width / 2
                    : kind == 5 ? 65536u - f.width / 2 : 124;
      int page = kind == 4 ? 4 : kind == 5 ? 64 : 0;
      if (f.width == 1 && kind >= 3 && kind <= 5) continue;
      if (f.align_trap && doff % (uint32_t)f.align_trap) continue;
      if (f.file == 'v' && kind && !f.lr) continue;
      mu_geom_t g;
      if (mu_geom_for(&f, doff, page, &g) < 0) {
        HT_CHECK(f.file == 'f' && f.update && (doff & 7));                         /* the only refusal */
        continue;
      }
      c.base = 0; c.n = page ? 4 : 64; c.stride = g.stride; c.jmod = 0; c.bdiv = 0; c.anchor = g.a;
      c.ptr_rel = g.ptr_rel; c.data_rel = g.data_rel; c.data_w = g.data_w; c.df = f;
      c.data = f.file == 'g' ? (uint64_t)g.V : f.fpsingle ? 0x3F800000ull : 0x3FF0000000000000ull;
      c.random = 1; c.seed = (uint32_t)id * 7u + (uint32_t)kind;
      int e = mu_chain_build(&c);
      if (e) printf("    %s doff %u: build %d\n", xts_isa[id].mnem, doff, e);
      HT_CHECK(e == 0);
      if (e) continue;
      built++;
      int w = sim_walk(&c, &f, &g, doff);
      if (w) printf("    %s doff %u: walk %d\n", xts_isa[id].mnem, doff, w);
      HT_CHECK(w == 0);
      walked += w == 0;
      if (f.file != 'g') HT_CHECK((g.V & (g.V - 1)) == 0 && g.V >= 8);              /* CR bit -> power of two */
    }
  }
  HT_CHECK(built > 150 && built == walked);
}

/* rlwinm rT,rT,(b-src)&31,31-b,31-b turns the CR bit at LSB position src into the value 1<<b */
static uint32_t rlwinm(uint32_t v, int sh, int mb, int me)
{
  uint32_t r = (v << sh) | (sh ? v >> (32 - sh) : 0), m = 0;
  for (int i = mb; i <= me; i++) m |= 0x80000000u >> i;
  return r & m;
}
HTEST(tmem_cr_bits)
{
  for (int b = 3; b <= 13; b++) {
    HT_EQ_U64(rlwinm(1u << 25, (b - 25) & 31, 31 - b, 31 - b), 1u << b);         /* CR1[EQ] = CR bit 6 */
    HT_EQ_U64(rlwinm(1u << 7, (b - 7) & 31, 31 - b, 31 - b), 1u << b);           /* CR6[0] = CR bit 24 */
    HT_EQ_U64(rlwinm(0xFFFFFFFFu & ~(1u << 25), (b - 25) & 31, 31 - b, 31 - b), 0); /* bit clear -> 0 */
  }
  HT_EQ_U64(rlwinm(0x12345678u, 0, 19, 24), 0x12345678u & 0x1F80u);               /* X mod 8K, line aligned */
}

HTEST(tmem_chain_layouts)
{
  static mu_chain_t c;
  HT_CHECK(mu_big(MU_BIG_BYTES, &c.buf) == 0);
  /* tlb: one line per 64 KB page, jitter spreads the sets; xdcbt2: 256 lines per 128 KB block at 96K..128K */
  c.base = 0; c.n = 1024; c.stride = MU_PAGE; c.jmod = 512; c.jstep = MU_LINE; c.bdiv = 0; c.anchor = 0; c.ptr_rel = 0;
  c.data_w = 0; c.random = 1; c.seed = 3;
  HT_CHECK(mu_chain_build(&c) == 0);
  int sets[64] = { 0 };
  for (uint32_t i = 0; i < c.n; i++) sets[(mu_chain_off(&c, i) / MU_LINE) % 64]++;
  for (int s = 0; s < 64; s++) HT_CHECK(sets[s] == 16);
  c.base = MU_MB + 96 * MU_KB; c.n = 2112; c.stride = MU_LINE; c.jmod = 0; c.bdiv = 256; c.bstep = 96 * MU_KB;
  HT_CHECK(mu_chain_build(&c) == 0);
  for (uint32_t i = 0; i < c.n; i++) {
    uint32_t o = mu_chain_off(&c, i);
    HT_CHECK(((o >> 13) & 15) >= 12);                                              /* L2 sets 768..1023 only */
  }
  HT_CHECK(c.span < 2 * MU_MB);
  /* a broken pointer is caught */
  c.buf.p[mu_chain_off(&c, 5)] ^= 0x40;
  HT_CHECK(mu_chain_verify(&c) != 0);
  /* sequential order */
  c.base = 0; c.n = 100; c.bdiv = 0; c.random = 0;
  HT_CHECK(mu_chain_build(&c) == 0);
  HT_EQ_U64(mu_chain_addr_at(&c, 7), c.buf.addr + 7 * MU_LINE);
  /* fields that overlap are refused */
  c.data_w = 8; c.data_rel = 4; c.ptr_rel = 0; c.df.file = 'g'; c.df.width = 8;
  HT_CHECK(mu_chain_build(&c) < 0);
}

HTEST(tmem_plateaus)
{
  static const double kb[] = { 4, 8, 16, 24, 28, 32, 36, 40, 48, 64, 128, 256, 512, 768, 1024, 1152, 1536, 2048, 4096, 8192, 32768 };
  int n = (int)(sizeof kb / sizeof kb[0]);
  double x[32], y[32];
  for (int i = 0; i < n; i++) {
    x[i] = kb[i] * 1024;
    y[i] = kb[i] <= 32 ? 5.0 + 0.01 * i : kb[i] <= 1024 ? 45.0 + 0.3 * (i & 1) : kb[i] == 1152 ? 150 : kb[i] == 1536 ? 400 : 638 + i;
  }
  y[6] = 12.0;                                                                     /* a partial-miss point at 36K */
  mu_plateau_t p[8];
  int np = mu_plateaus(x, y, n, p, 8);
  HT_CHECK(np == 3);
  if (np == 3) {
    HT_NEAR(p[0].level, 5.0, 0.1);
    HT_NEAR(x[p[0].hi], 32 * 1024.0, 0.5);
    HT_NEAR(p[1].level, 45.15, 0.3);
    HT_NEAR(x[p[1].hi], 1024 * 1024.0, 0.5);
    HT_CHECK(p[2].level > 600);
  }
  double lev[3] = { 5, 45, 638 };
  HT_CHECK(mu_classify(5.3, lev, 3) == 0);
  HT_CHECK(mu_classify(47, lev, 3) == 1);
  HT_CHECK(mu_classify(600, lev, 3) == 2);
  HT_CHECK(mu_classify(15, lev, 3) == -1);                                          /* between L1 and L2 */
  HT_CHECK(mu_classify(NAN, lev, 3) == -1);
}

/* run the kernel's words (model-implemented ones) until the first load; the final state */
static int run_prefix(const xts_kbuf_t *k, xts_state_t *out)
{
  static xts_state_t a, b;
  xts_state_baseline(&a);
  a.mode = XTS_MODE_SF;
  for (uint32_t i = 0; i < k->n; i++) {
    if (k->tag[i] == XK_TAG_EXIT || (k->w[i] >> 26) == 58) break;          /* the first ld of the chase loop */
    xts_mem_t m;
    xts_mem_init(&m);
    if (xts_model_exec(NULL, k->w[i], &a, &m, &b) != XTS_M_OK) return -1;
    a = b;
  }
  *out = a;
  return 0;
}

/* mu_li32 used to drop bit 31 (addis with a 15-bit high half): values and addresses >= 0x80000000 must load whole */
HTEST(tmem_li_bit31)
{
  static const uint32_t vals[] = { 0x7FFFFFFFu, 0x80000000u, 0x80008000u, 0x9D000000u, 0xC0000000u, 0xFFFF8000u, 0xFFFFFFFFu,
                                   0x80000005u };
  xts_kbuf_t *k = xk_new(64);
  for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++) {
    xk_reset(k);
    xk_li32(k, 21, vals[i]);
    xk_exit(k);
    xk_finish(k);
    xts_state_t st;
    HT_CHECK(run_prefix(k, &st) == 0);
    HT_EQ_U64(st.gpr[21], vals[i]);
  }
  /* the family path: the warm-walk prologue of the pure chase puts the chain length into the loop count (CTR) */
  static mu_chain_t c;
  static mu_chase_t a;
  c.n = 0x80000005u;                                     /* never built: only the immediate is checked */
  a.c = &c;
  a.warm = 1;
  xk_reset(k);
  HT_CHECK(mu_chase_pre(k, 8, &a) == 0);
  xk_exit(k);
  xk_finish(k);
  xts_state_t st;
  HT_CHECK(run_prefix(k, &st) == 0);
  HT_EQ_U64(st.gpr[21], 0x80000005u);
  HT_EQ_U64(st.ctr, 0x80000005u);
  xk_free(k);
}
