/* framework v2: xk_li64 / xk_li32 / xk_li32s, checked by running every emitted word through the reference model in both
 * SF modes; xts_cond_applied / xts_cond_value. */
#include <string.h>
#include <xts/xts.h>
#include "htest.h"

/* run the kernel's words (straight-line, model-implemented) on a baseline state; returns the final state's gpr[reg] */
static int run_words(const xts_kbuf_t *k, int reg, int sf, uint64_t *out)
{
  static xts_state_t a, b;
  xts_state_baseline(&a);
  a.mode = sf ? XTS_MODE_SF : 0;
  for (uint32_t i = 0; i < k->n; i++) {
    if (k->tag[i] == XK_TAG_EXIT) break;
    xts_mem_t m;
    xts_mem_init(&m);
    if (xts_model_exec(NULL, k->w[i], &a, &m, &b) != XTS_M_OK) return -1;
    a = b;
  }
  *out = a.gpr[reg];
  return 0;
}

static int check_value(xts_kbuf_t *k, int reg, uint64_t v)
{
  int bad = 0;
  xk_reset(k);
  int n = xk_li64(k, reg, v);
  xk_exit(k);
  if (n < 1 || n > 5 || n != xk_li64_words(v) || xk_finish(k) < 0) return 1;
  if (xk_validate(k, NULL, NULL, NULL, 0)) return 1;
  for (int sf = 0; sf < 2; sf++) {
    uint64_t got = 0;
    if (run_words(k, reg, sf, &got) < 0 || got != v) {
      printf("    xk_li64(r%d, %016llX) sf=%d -> %016llX (%d words)\n", reg, (unsigned long long)v, sf, (unsigned long long)got, n);
      bad++;
    }
  }
  /* nothing but the target register changes: no CR / XER / other GPR side effects */
  return bad;
}

HTEST(fw2_kgen_li64)
{
  xts_kbuf_t *k = xk_new(64);
  static const uint64_t edge[] = {
    0, 1, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x12345678, 0x7FFFFFFF, 0x80000000ull, 0x80000001ull, 0xFFFFFFFFull, 0xFFFF0000ull,
    0x00008000ull, 0xFFFFFFFF80000000ull, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFF8000ull, 0xFFFFFFFF7FFFFFFFull,
    0x100000000ull, 0x1FFFFFFFFull, 0x123456789ABCDEF0ull, 0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull,
    0x0000800000000000ull, 0xFFFF800000000000ull, 0x00000000DEADBEEFull, 0xDEADBEEF00000000ull, 0xC0DE00000000005Aull,
  };
  int bad = 0;
  for (unsigned i = 0; i < sizeof edge / sizeof edge[0]; i++) bad += check_value(k, 5, edge[i]);
  HT_EQ_U64(bad, 0);
  /* word counts of the short forms */
  HT_EQ_U64(xk_li64_words(0x7FFF), 1);
  HT_EQ_U64(xk_li64_words(0xFFFFFFFFFFFF8000ull), 1);
  HT_EQ_U64(xk_li64_words(0x12340000), 1);
  HT_EQ_U64(xk_li64_words(0xFFFFFFFF80000000ull), 1);
  HT_EQ_U64(xk_li64_words(0x80000000ull), 2);
  HT_EQ_U64(xk_li64_words(0xFFFFFFFFull), 3);
  HT_EQ_U64(xk_li64_words(0x123456789ABCDEF0ull), 5);
  /* r0 as the target (addi/addis read the literal 0 for RA=0) and random values */
  bad = check_value(k, 0, 0x80000000ull) + check_value(k, 0, 0x123456789ABCDEF0ull) + check_value(k, 31, 0xFFFFFFFFull);
  xts_rng_t r;
  xts_rng_seed(&r, 2026);
  for (int i = 0; i < 2000; i++) {
    uint64_t v = xts_rng_next(&r);
    switch (i & 3) {
    case 1: v &= 0xFFFFFFFFull; break;
    case 2: v = (uint64_t)(int64_t)(int32_t)v; break;
    case 3: v &= 0xFFFF0000FFFF0000ull; break;
    }
    bad += check_value(k, 3 + (i % 10), v);
  }
  HT_EQ_U64(bad, 0);
  /* the 32-bit forms */
  uint64_t got = 0;
  xk_reset(k);
  xk_li32(k, 6, 0x80000000u);
  xk_exit(k);
  xk_finish(k);
  HT_EQ_U64(run_words(k, 6, 1, &got), 0);
  HT_EQ_U64(got, 0x80000000ull);
  xk_reset(k);
  xk_li32s(k, 6, 0x80000000u);
  xk_exit(k);
  xk_finish(k);
  HT_EQ_U64(run_words(k, 6, 1, &got), 0);
  HT_EQ_U64(got, 0xFFFFFFFF80000000ull);
  xk_reset(k);
  HT_EQ_U64(xk_li64(k, 32, 1), (uint64_t)-1);
  xk_free(k);
}

HTEST(fw2_cond_applied)
{
  xts_condvec_t cv;
  memset(&cv, 0, sizeof cv);
  int n = xts_axis_count();
  cv.n = n;
  for (int i = 0; i < n; i++) cv.axis[i] = xts_axis_get(i);
  int sf = -1;
  for (int i = 0; i < n; i++)
    if (!strcmp(cv.axis[i]->name, "sf")) sf = i;
  HT_CHECK(sf >= 0);
  if (sf < 0) return;
  xts_ctx_t ctx = { .cond = &cv };
  HT_EQ_U64(xts_cond_applied(&ctx, "sf"), 0);
  HT_EQ_U64(xts_cond_value(&ctx, "sf"), 0);
  cv.applied = 1u << sf;
  cv.value[sf] = 1;
  HT_EQ_U64(xts_cond_applied(&ctx, "sf"), 1);
  HT_EQ_U64(xts_cond_value(&ctx, "sf"), 1);
  HT_EQ_U64(xts_cond_applied(&ctx, "no_such_axis"), (uint64_t)-1);
  HT_EQ_U64(xts_cond_applied(NULL, "sf"), (uint64_t)-1);
}
