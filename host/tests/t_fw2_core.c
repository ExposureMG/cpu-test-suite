/* framework v2: status combiner, vocabularies, JSON members, ISA aliases, crash-page thread slots, trap-range table,
 * chained exec hooks, block-info records. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include <core/core.h>
#include "htest.h"

XTS_METRIC("fw2_test_metric");
XTS_UNIT("fw2_test_unit");

/* ---- record capture through the sink tap (records stay out of every file: discarded afterwards) ---- */
static struct cap { int n; char family[48], case_id[96], metric[32], unit[32], line[8192]; xts_record_t r; char detail[8192]; } cap;
static void tap(const char *family, const char *case_id, const xts_record_t *r, const char *json)
{
  cap.n++;
  snprintf(cap.family, sizeof cap.family, "%s", family);
  snprintf(cap.case_id, sizeof cap.case_id, "%s", case_id);
  snprintf(cap.metric, sizeof cap.metric, "%s", r->metric ? r->metric : "");
  snprintf(cap.unit, sizeof cap.unit, "%s", r->unit ? r->unit : "");
  snprintf(cap.line, sizeof cap.line, "%s", json);
  snprintf(cap.detail, sizeof cap.detail, "%s", r->detail ? r->detail : "");
  cap.r = *r;
}
static void cap_begin(void) { memset(&cap, 0, sizeof cap); xts_sink_tap = tap; }
static void cap_end(void) { xts_sink_tap = NULL; xts_sink_discard(); }

HTEST(fw2_status_worst)
{
  static const int order[] = { XTS_ST_PASS, XTS_ST_INFO, XTS_ST_SKIP, XTS_ST_REGRESSED, XTS_ST_NOISY, XTS_ST_TRAP,
                               XTS_ST_FAIL, XTS_ST_HANG, XTS_ST_CRASH };
  for (int i = 0; i < 9; i++) {
    HT_EQ_U64(xts_status_rank(order[i]), i);
    for (int k = 0; k < 9; k++) {
      int w = xts_status_worst(order[i], order[k]);
      HT_EQ_U64(w, order[i > k ? i : k]);
      HT_EQ_U64(xts_status_worst(order[k], order[i]), w);        /* symmetric */
    }
  }
  HT_EQ_U64(xts_status_worst(-1, XTS_ST_NOISY), XTS_ST_NOISY);   /* out-of-range values are ignored */
  HT_EQ_U64(xts_status_worst(XTS_ST_FAIL, 99), XTS_ST_FAIL);
  HT_EQ_U64(xts_status_rank(42), (uint64_t)-1);
  /* the bugs the combiner replaces: skip never outranks fail, hang/crash are the worst, regressed/info survive */
  HT_EQ_U64(xts_status_worst(XTS_ST_SKIP, XTS_ST_FAIL), XTS_ST_FAIL);
  HT_EQ_U64(xts_status_worst(XTS_ST_HANG, XTS_ST_FAIL), XTS_ST_HANG);
  HT_EQ_U64(xts_status_worst(XTS_ST_PASS, XTS_ST_REGRESSED), XTS_ST_REGRESSED);
  HT_EQ_U64(xts_status_worst(XTS_ST_PASS, XTS_ST_INFO), XTS_ST_INFO);
  for (int s = 0; s < XTS_ST_COUNT; s++) HT_EQ_U64(xts_status_from_name(xts_status_name(s)), s);
  HT_EQ_U64(xts_status_from_name("bogus"), (uint64_t)-1);
}

HTEST(fw2_vocab)
{
  static const char *const m[] = { "latency", "rthroughput", "issue_cost", "penalty", "extra", "overlap", "cycles", "ratio",
                                   "bytes_per_cycle", "depth", "count", "result", "estimate", "tininess", "info" };
  static const char *const u[] = { "cycles", "ratio", "count", "bytes", "bytes_per_cycle", "entries", "words", "hz", "ticks", "" };
  for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) HT_CHECK(xts_metric_known(m[i]));
  for (unsigned i = 0; i < sizeof u / sizeof u[0]; i++) HT_CHECK(xts_unit_known(u[i]));
  HT_CHECK(!xts_metric_known("smt_sibling"));
  HT_CHECK(!xts_unit_known("Hz"));
  HT_CHECK(xts_metric_known("fw2_test_metric"));     /* XTS_METRIC registration */
  HT_CHECK(!xts_unit_known("fw2_test_metric"));
  HT_CHECK(xts_unit_known("fw2_test_unit"));
  /* xts_emit flags unknown names in the detail (and still writes the record) */
  static const xts_family_t fam = { .name = "fw2_vocab", .kind = "self" };
  xts_case_t c = { .id = "v#0", .insn = -1 };
  xts_ctx_t ctx = { .fam = &fam, .c = &c, .block_key = "k" };
  cap_begin();
  xts_record_t r;
  xts_record_init(&r);
  r.metric = "bogus_metric";
  r.unit = "Hz";
  r.detail = "{\"a\":1}";
  xts_emit(&ctx, &r);
  HT_EQ_U64(cap.n, 1);
  HT_CHECK(strstr(cap.line, "\"detail\":{\"a\":1,\"vocab_warning\":\"metric 'bogus_metric' unit 'Hz' not in the vocabulary\"}") != NULL);
  r.metric = "fw2_test_metric";
  r.unit = "cycles";
  r.detail = NULL;
  xts_emit(&ctx, &r);
  HT_CHECK(strstr(cap.line, "vocab_warning") == NULL);
  r.metric = "latency";
  r.unit = "furlongs";
  xts_emit(&ctx, &r);
  HT_CHECK(strstr(cap.line, "\"detail\":{\"vocab_warning\":\"unit 'furlongs' not in the vocabulary\"}") != NULL);
  cap_end();
}

HTEST(fw2_json_members)
{
  char buf[256];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_members(&j, "{}");
  xj_members(&j, " {\"p\":\"x\",\"d\":3} ");
  xj_kv_int(&j, "n", 1);
  xj_members(&j, "{\"q\":[1,2]}");
  xj_members(&j, "not an object");
  xj_end(&j);
  HT_CHECK(!strcmp(buf, "{\"p\":\"x\",\"d\":3,\"n\":1,\"q\":[1,2]}"));
  char small[16];
  xj_init(&j, small, sizeof small);
  xj_obj(&j);
  xj_members(&j, "{\"a_long_key\":\"a long value\"}");
  xj_finish(&j);
  HT_CHECK(j.truncated && !strcmp(small, "{}"));
}

HTEST(fw2_isa_aliases)
{
  HT_EQ_U64(xts_enc(XI_lwsync, 0), 0x7C2004ACu);
  HT_EQ_U64(xts_enc(XI_ptesync, 0), 0x7C4004ACu);
  HT_EQ_U64(xts_enc(XI_sync, 0), 0x7C0004ACu);
  HT_CHECK(XI_lwsync >= XTS_ISA_COUNT && XI_ptesync >= XTS_ISA_COUNT);
  HT_EQ_U64(xts_isa_alias_count, XTS_ISA_ALIAS_COUNT);
  HT_EQ_U64(xts_decode1(0x7C2004ACu), XI_sync);
  HT_CHECK(xts_decodes_as(0x7C2004ACu, XI_lwsync) && !xts_decodes_as(0x7C0004ACu, XI_lwsync));
  HT_CHECK(xts_decodes_as(0x7C4004ACu, XI_ptesync) && !xts_decodes_as(0x7C2004ACu, XI_ptesync));
  HT_CHECK(xts_insn(XI_lwsync) == &xts_isa[XI_sync]);
  HT_EQ_U64(xts_insn_canonical(XI_ptesync), XI_sync);
  HT_EQ_U64(xts_isa_find("lwsync"), XI_lwsync);
  HT_CHECK(!strcmp(xts_insn_name(XI_ptesync), "ptesync"));
  char d[64];
  HT_CHECK(!strcmp(xts_disasm(0x7C2004ACu, d, sizeof d), "lwsync"));
  HT_CHECK(!strcmp(xts_disasm(0x7C4004ACu, d, sizeof d), "ptesync"));
  HT_CHECK(!strcmp(xts_disasm(0x7C0004ACu, d, sizeof d), "sync"));
  xts_kbuf_t *k = xk_new(64);
  xk_emit(k, XI_lwsync, 0);
  xk_emit(k, XI_ptesync, 0);
  xk_exit(k);
  HT_EQ_U64(xk_finish(k), 0);
  int ni = 0, nr = 0;
  HT_EQ_U64(xk_validate(k, &ni, &nr, d, sizeof d), 0);
  HT_EQ_U64(ni, 2);
  HT_EQ_U64(k->w[0], 0x7C2004ACu);
  xk_free(k);
  /* mffs writes an FPR (table fix) */
  const xts_insn_t *m = xts_insn(XI_mffsx);
  HT_CHECK(m && m->nops == 1 && m->ops[0].cls == XTS_OC_FPR && (m->ops[0].role & XTS_ROLE_W));
}

HTEST(fw2_trap_ranges)
{
  uint64_t lo, hi;
  uint32_t off = 0;
  for (int i = 0; i < XTS_EXEC_EXTRA_RANGES; i++) HT_EQ_U64(xts_exec_trap_range(i, 0x80100000u + i * 0x10000u, 0x80104000u + i * 0x10000u), 0);
  HT_EQ_U64(xts_exec_trap_range(XTS_EXEC_EXTRA_RANGES, 0x1000, 0x2000), (uint64_t)-1);   /* bad index */
  HT_EQ_U64(xts_exec_trap_range(-1, 0x1000, 0x2000), (uint64_t)-1);
  HT_EQ_U64(xts_exec_trap_range(0, 0x1000, 0x1000 + 0x10000), (uint64_t)-1);           /* 64 KB: too large */
  HT_EQ_U64(xts_exec_trap_range(0, 0x2000, 0x1000), (uint64_t)-1);                     /* inverted */
  HT_EQ_U64(xts_exec_trap_range_get(2, &lo, &hi), 0);
  HT_EQ_U64(lo, 0x80120000u);
  HT_EQ_U64(hi, 0x80124000u);
  HT_EQ_U64(xts_exec_trap_classify(0x80120010u, &off), 4);   /* where = 2 + idx */
  HT_EQ_U64(off, 0x10);
  HT_EQ_U64(xts_exec_trap_classify(0xFFFFFFFF80130000ull, &off), 5);   /* low 32 bits compared */
  HT_EQ_U64(off, 0);
  HT_EQ_U64(xts_exec_trap_classify(0x80104000u, &off), (uint64_t)-1);  /* hi is exclusive */
  HT_EQ_U64(xts_exec_trap_classify(0x800FFFFCu, &off), (uint64_t)-1);
  HT_EQ_U64(xts_exec_trap_range(2, 0, 0), 0);                          /* removed */
  HT_EQ_U64(xts_exec_trap_classify(0x80120010u, &off), (uint64_t)-1);
  HT_EQ_U64(xts_exec_trap_range(1, 0x9000, 0x9000 + XTS_EXEC_RANGE_MAX), 0);   /* the largest allowed */
  HT_EQ_U64(xts_exec_trap_classify(0x9000 + XTS_EXEC_RANGE_MAX - 1, &off), 3);
  for (int i = 0; i < XTS_EXEC_EXTRA_RANGES; i++) xts_exec_trap_range(i, 0, 0);
}

/* ---- chained hooks ---- */
static char trace[128];
static int refuse_at = -1;
static void tr(char c) { size_t n = strlen(trace); if (n < sizeof trace - 1) { trace[n] = c; trace[n + 1] = 0; } }
static int h_pre(xts_exec_t *x, void *ud) { int id = (int)(intptr_t)ud; tr((char)('A' + id)); (void)x; return id == refuse_at ? -1 : 0; }
static void h_post(xts_exec_t *x, void *ud) { tr((char)('a' + (int)(intptr_t)ud)); tr(x->status == XTS_EXEC_NOTRUN ? '!' : '.'); }
static int v1_pre(xts_exec_t *x) { (void)x; tr('V'); return 0; }
static void v1_post(xts_exec_t *x) { (void)x; tr('v'); }

HTEST(fw2_exec_hooks)
{
  static xts_exec_t x;
  xts_kbuf_t *k = xk_new(64);
  xk_nop(k);
  xk_exit(k);
  HT_EQ_U64(xk_finish(k), 0);
  xts_exec_hook_clear();
  int h0 = xts_exec_hook_add(h_pre, h_post, (void *)0);
  int h1 = xts_exec_hook_add(h_pre, h_post, (void *)1);
  int h2 = xts_exec_hook_add(NULL, h_post, (void *)2);
  HT_CHECK(h0 >= 0 && h1 >= 0 && h2 >= 0 && h0 != h1 && h1 != h2);
  HT_EQ_U64(xts_exec_hook_count(), 3);
  xts_exec_hooks.pre = v1_pre;
  xts_exec_hooks.post = v1_post;
  trace[0] = 0;
  xts_case_init(&x);
  HT_EQ_U64(xts_exec_kernel(&x, k), XTS_EXEC_DRYRUN);
  HT_CHECK(!strcmp(trace, "VABc.b.a.v"));                 /* pre in add order (v1 first), post reversed (v1 last) */
  /* a refusal undoes the hooks that already ran, with status NOTRUN */
  refuse_at = 1;
  trace[0] = 0;
  xts_case_init(&x);
  HT_EQ_U64(xts_exec_kernel(&x, k), (uint64_t)XTS_EXEC_NOTRUN);
  HT_CHECK(!strcmp(trace, "VABa!v"));
  HT_CHECK(x.err[0] != 0);
  refuse_at = -1;
  /* remove the middle one; the slot is reused by the next add, which runs last */
  HT_EQ_U64(xts_exec_hook_remove(h1), 0);
  HT_EQ_U64(xts_exec_hook_remove(h1), (uint64_t)-1);
  HT_EQ_U64(xts_exec_hook_remove(99), (uint64_t)-1);
  int h3 = xts_exec_hook_add(h_pre, NULL, (void *)3);
  HT_EQ_U64(h3, h1);
  xts_exec_hooks.pre = NULL;
  xts_exec_hooks.post = NULL;
  trace[0] = 0;
  xts_case_init(&x);
  xts_exec_kernel(&x, k);
  HT_CHECK(!strcmp(trace, "ADc.a."));
  /* single words (model-simulated on the host) are bracketed too */
  trace[0] = 0;
  xts_case_init(&x);
  HT_EQ_U64(xts_exec_word(&x, xts_enc(XI_addx, 0, 3, 4, 5)), XTS_EXEC_SIMULATED);
  HT_CHECK(!strcmp(trace, "ADc.a."));
  /* capacity */
  int n = xts_exec_hook_count(), extra = 0;
  while (xts_exec_hook_add(h_pre, NULL, (void *)4) >= 0) extra++;
  HT_EQ_U64(n + extra, XTS_EXEC_MAX_HOOKS);
  xts_exec_hook_clear();
  HT_EQ_U64(xts_exec_hook_count(), 0);
  HT_CHECK(xts_exec_hooks.pre == NULL && xts_exec_hooks.post == NULL);
  trace[0] = 0;
  xts_case_init(&x);
  xts_exec_kernel(&x, k);
  HT_CHECK(trace[0] == 0);
  xk_free(k);
}

HTEST(fw2_block_info)
{
  xts_condvec_t cv;
  xts_plan_condvec(NULL, &cv);
  cap_begin();
  xts_sink_set_block("r3:smt.x:sibling=busy_int:c0", &cv, "smt.x");
  xts_emit_block_info(NULL, "smt:sibling", "info", 3.5, "count", "{\"axis\":\"sibling\"}");
  HT_EQ_U64(cap.n, 1);
  HT_CHECK(!strcmp(cap.family, "smt.x") && !strcmp(cap.case_id, "smt:sibling"));
  HT_EQ_U64(cap.r.status, XTS_ST_INFO);
  HT_NEAR(cap.r.value, 3.5, 0);
  HT_CHECK(strstr(cap.line, "\"block\":\"r3:smt.x:sibling=busy_int:c0\"") != NULL);
  HT_CHECK(strstr(cap.line, "\"unit\":\"count\"") != NULL);
  HT_CHECK(strstr(cap.line, "\"detail\":{\"axis\":\"sibling\"}") != NULL);
  xts_emit_block_info(NULL, NULL, NULL, NAN, NULL, NULL);
  HT_CHECK(!strcmp(cap.case_id, "block") && !strcmp(cap.metric, "info"));
  HT_CHECK(strstr(cap.line, "\"value\":null") != NULL);
  /* with a ctx: the ctx's family/block */
  static const xts_family_t fam = { .name = "fw2_info", .kind = "self" };
  xts_case_t c = { .id = "c#1", .insn = -1 };
  xts_ctx_t ctx = { .fam = &fam, .c = &c, .block_key = "r0:fw2.info:-:c0", .cond = &cv };
  xts_emit_block_info(&ctx, "note", "count", 7, "count", NULL);
  HT_CHECK(!strcmp(cap.family, "fw2_info") && !strcmp(cap.case_id, "note"));   /* unregistered: the raw id */
  HT_CHECK(strstr(cap.line, "\"block\":\"r0:fw2.info:-:c0\"") != NULL);
  HT_EQ_U64(ctx.nrecords, 1);
  xts_sink_set_block("", NULL, "");
  cap_end();
}
