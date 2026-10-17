/* Robustness: the semantic DSI guard. A word the model predicts DSI for is never executed (skip, "model predicts DSI");
 * an unsimulated memory word runs only when its EA lies in the scratch area / big buffer. */
#include <string.h>
#include <xts/xts.h>
#include <core/core.h>
#include <exec/exec_priv.h>
#include "htest.h"

static struct { int n; char line[16384]; } gcap;
static void gtap(const char *family, const char *case_id, const xts_record_t *r, const char *json_line)
{
  (void)family; (void)case_id; (void)r;
  gcap.n++;
  snprintf(gcap.line, sizeof gcap.line, "%s", json_line);
}

HTEST(rob_dsi_guard_semantic)
{
  static const xts_family_t fam = { .name = "rob_guard", .kind = "semantic" };
  xts_case_t c = { .id = "g#0", .insn = XI_lwz };
  xts_ctx_t ctx = { .fam = &fam, .c = &c, .block_key = "k" };
  static xts_exec_t x;
  uint64_t words0 = xts_dry_stats.words;
  memset(&gcap, 0, sizeof gcap);
  xts_sink_tap = gtap;
  /* lwz r3,0(r4) with r4 pointing at page zero: the model says DSI -> skip, never handed to the backend */
  xts_case_init(&x);
  x.in.gpr[4] = 0x1000;
  int st = xts_semantic_check(&ctx, &x, xts_enc(XI_lwz, 0, 3, 0, 4), NULL);
  HT_EQ_U64(st, XTS_ST_SKIP);
  HT_EQ_U64(gcap.n, 1);
  HT_CHECK(strstr(gcap.line, "\"status\":\"skip\"") && strstr(gcap.line, "model predicts DSI at 1000"));
  HT_EQ_U64(xts_dry_stats.words, words0);              /* xts_exec_word was not called */
  /* the same word on the scratch area runs (simulated on the host) and passes */
  xts_case_init(&x);
  xts_set_ptr(&x, 4, 16);
  st = xts_semantic_check(&ctx, &x, xts_enc(XI_lwz, 0, 3, 0, 4), NULL);
  HT_EQ_U64(st, XTS_ST_PASS);
  HT_EQ_U64(xts_dry_stats.words, words0 + 1);
  xts_sink_tap = NULL;
  xts_sink_discard();
}

HTEST(rob_dsi_guard_unsimulated)
{
  /* an unsimulated memory word with an RA operand (whatever the model lacks today); none = nothing to check */
  int id = -1;
  for (int i = 0; i < xts_isa_count && id < 0; i++) {
    const xts_insn_t *d = &xts_isa[i];
    if (d->unit != XTS_UNIT_MEMORY || xts_model_has(i) || d->priv != XTS_PRIV_USER) continue;
    for (int k = 0; k < d->nops; k++)
      if (!strcmp(d->ops[k].name, "RA0")) id = i;
  }
  xts_state_t in;
  xts_state_baseline(&in);
  in.mode |= XTS_MODE_SF;
  char why[200];
  /* the model's verdict alone */
  HT_EQ_U64(xts_exec_mem_guard(XI_lwz, 0x80640000u, &in, XTS_M_DSI, 0x1234, why, sizeof why), 1);
  HT_CHECK(strstr(why, "model predicts DSI at 1234") != NULL);
  HT_EQ_U64(xts_exec_mem_guard(XI_lwz, 0x80640000u, &in, XTS_M_OK, 0, why, sizeof why), 0);
  if (id < 0) { HT_CHECK(1); return; }
  /* encode "<insn> ..., RA=4, RB=5" or "RA=4 + d" by setting the fields directly */
  const xts_insn_t *d = &xts_isa[id];
  uint32_t w = d->match;
  for (int k = 0; k < d->nops; k++) {
    if (!strcmp(d->ops[k].name, "RA0")) w = xts_field_put(d->ops[k].field, 4, w);
    else if (!strcmp(d->ops[k].name, "RB")) w = xts_field_put(d->ops[k].field, 5, w);
  }
  in.gpr[4] = xts_scratch_addr();
  in.gpr[5] = 0;
  HT_EQ_U64(xts_exec_mem_guard(id, w, &in, XTS_M_UNIMPLEMENTED, 0, why, sizeof why), 0);
  in.gpr[4] = 0x2000;                                   /* page zero area: outside scratch and big buffer */
  HT_EQ_U64(xts_exec_mem_guard(id, w, &in, XTS_M_UNIMPLEMENTED, 0, why, sizeof why), 1);
  HT_CHECK(strstr(why, "outside the scratch area") != NULL);
  in.gpr[4] = 0xFFFFFFFF00000000ull | xts_scratch_addr();   /* SF=0: the EA is truncated to 32 bits -> inside */
  in.mode &= ~(uint64_t)XTS_MODE_SF;
  HT_EQ_U64(xts_exec_mem_guard(id, w, &in, XTS_M_UNIMPLEMENTED, 0, why, sizeof why), 0);
}
