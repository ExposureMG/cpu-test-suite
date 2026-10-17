/* xts/rline.h: the results.jsonl line reader used by family finish hooks. Lines come from the real sink (emit through the
 * host sink, read back with xts_results_foreach) and from hand-written lines for the shapes the sink does not produce
 * (shuffled keys, \u escapes, malformed input). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include <core/core.h>
#include "htest.h"

#define MAXL 16
static char *got[MAXL];
static int ngot;
static int keep_cb(const char *line, void *ud)
{
  (void)ud;
  if (ngot < MAXL) got[ngot++] = strdup(line);
  return 0;
}
static void free_got(void)
{
  for (int i = 0; i < ngot; i++) free(got[i]);
  ngot = 0;
}
static void set_axis(xts_condvec_t *cv, const char *axis, const char *value)
{
  for (int i = 0; i < cv->n; i++)
    if (!strcmp(cv->axis[i]->name, axis)) cv->value[i] = xts_axis_value_index(cv->axis[i], value);
}

HTEST(rline_sink_roundtrip)
{
  char dir[600], cmd[700];
  snprintf(dir, sizeof dir, "%s/rline_sink", ht_tmp);
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  HT_EQ_U64(system(cmd), 0);
  HT_EQ_U64(xts_mkdirs(dir), 0);
  xts_sink_discard();
  HT_EQ_U64(xts_sink_open(dir, "rl", 0), 0);
  static const xts_family_t fam = { .name = "rline_t", .kind = "timing" };
  xts_case_t c = { .id = "dist.int>lwz#4", .insn = -1 };
  xts_condvec_t cv, cv2;
  xts_plan_condvec(NULL, &cv);
  cv2 = cv;
  set_axis(&cv2, "sf", "32");
  set_axis(&cv2, "fpscr_rn", "rz");
  xts_ctx_t ctx = { .fam = &fam, .c = &c, .block_key = "r12:rline.t:-:c3", .cond = &cv };
  xts_record_t r;
  /* 0: plain value, nested detail */
  xts_record_init(&r);
  r.metric = "penalty"; r.value = 3.5; r.unit = "cycles"; r.status = XTS_ST_NOISY;
  r.detail = "{\"fit\":{\"penalty_err\":9,\"verdict\":\"nested\"},\"penalty_err\":0.25,\"verdict\":\"stall\"}";
  xts_emit(&ctx, &r);
  /* 1: null value, other condition vector, quote and backslash in the case id, escapes and decoys in the detail */
  xts_record_init(&r);
  r.case_id = "we\"ird\\id";
  r.metric = "latency"; r.status = XTS_ST_SKIP;
  r.detail = "{\"note\":\"say \\\"hi\\\" ,\\\"detail\\\":{} \\\\\",\"arr\":[1,{\"x\":2}],\"x\":7,\"s\":\"a\\u00e9b\\n\"}";
  ctx.cond = &cv2;
  xts_emit(&ctx, &r);
  /* 2: no detail at all */
  xts_record_init(&r);
  r.metric = "count"; r.value = -2e-3; r.status = XTS_ST_PASS;
  ctx.cond = &cv;
  ctx.block_key = "r0:rline.t:finish";
  xts_emit(&ctx, &r);
  /* 3: emitted for a condition text */
  xts_record_init(&r);
  r.case_id = "summary"; r.metric = "count"; r.value = 1; r.status = XTS_ST_INFO;
  ctx.nrecords = 0;
  xts_emit_for_cond(&ctx, "\"sf\":\"32\",\"fpscr_rn\":\"rz\",\"nosuch\":\"x\",\"cache\":\"bogus\"", &r);
  HT_EQ_U64(ctx.nrecords, 1);
  ngot = 0;
  HT_EQ_U64(xts_results_foreach("rline_t", keep_cb, NULL), 4);
  xts_sink_close();
  HT_EQ_U64(ngot, 4);
  if (ngot != 4) { free_got(); return; }

  xts_rline_t l, l1, l3;
  char buf[64];
  HT_EQ_U64(xts_rline_parse(got[0], &l), 0);
  HT_CHECK(!strcmp(l.family, "rline_t") && !strcmp(l.case_id, "dist.int>lwz#4") && !strcmp(l.metric, "penalty"));
  HT_CHECK(!strcmp(l.unit, "cycles") && l.status == XTS_ST_NOISY && l.value == 3.5);
  HT_CHECK(!strcmp(l.block, "r12:rline.t:-:c3") && xts_rline_row(&l) == 12 && l.row == 12);
  HT_CHECK(l.detail >= got[0] && l.detail < got[0] + strlen(got[0]) && l.detail[0] == '{' && l.detail[l.detail_len - 1] == '}');
  HT_CHECK(xts_rline_num(&l, "penalty_err") == 0.25);               /* not the nested fit.penalty_err = 9 */
  HT_CHECK(xts_rline_str(&l, "verdict", buf, sizeof buf) && !strcmp(buf, "stall"));
  HT_CHECK(isnan(xts_rline_num(&l, "fit")) && !xts_rline_str(&l, "fit", buf, sizeof buf) && buf[0] == 0);
  HT_CHECK(xts_rline_cond(&l, "sf", buf, sizeof buf) == buf && !strcmp(buf, "64"));
  HT_CHECK(xts_rline_cond(&l, "fpscr_rn", buf, sizeof buf) && !strcmp(buf, "rn"));
  HT_CHECK(xts_rline_cond(&l, "nosuch", buf, sizeof buf) == NULL);
  HT_CHECK(xts_rline_cond(&l, "sf", buf, 2) && !strcmp(buf, "6"));   /* truncated copy */

  HT_EQ_U64(xts_rline_parse(got[1], &l1), 0);
  HT_CHECK(!strcmp(l1.case_id, "we\"ird\\id") && isnan(l1.value) && l1.status == XTS_ST_SKIP);
  HT_CHECK(xts_rline_num(&l1, "x") == 7);                           /* not arr[1].x = 2 */
  HT_CHECK(xts_rline_str(&l1, "note", buf, sizeof buf) && !strcmp(buf, "say \"hi\" ,\"detail\":{} \\"));
  HT_CHECK(xts_rline_str(&l1, "s", buf, sizeof buf) && !strcmp(buf, "a\xc3\xa9" "b\n"));
  HT_CHECK(xts_rline_str(&l1, "note", buf, 6) && !strcmp(buf, "say \""));
  HT_CHECK(l1.condh != l.condh && strcmp(l1.cond, l.cond));
  HT_CHECK(xts_rline_cond(&l1, "sf", buf, sizeof buf) && !strcmp(buf, "32"));
  HT_CHECK(xts_rline_cond(&l1, "fpscr_rn", buf, sizeof buf) && !strcmp(buf, "rz"));

  HT_EQ_U64(xts_rline_parse(got[2], &l), 0);
  HT_CHECK(l.detail_len == 2 && !strncmp(l.detail, "{}", 2) && l.value == -2e-3 && xts_rline_row(&l) == 0);
  HT_CHECK(isnan(xts_rline_num(&l, "x")) && !xts_rline_str(&l, "x", buf, sizeof buf));

  /* the condition table keeps one slot per distinct vector */
  xts_condtab_t t;
  memset(&t, 0, sizeof t);
  xts_rline_parse(got[0], &l);
  HT_CHECK(xts_condtab_add(&t, &l) == 0 && xts_condtab_add(&t, &l1) == 1 && xts_condtab_add(&t, &l) == 0 && t.n == 2);

  /* cond_from_text round trip: the vector the sink wrote comes back; xts_emit_for_cond wrote the one named in the text */
  xts_condvec_t back;
  xts_cond_from_text(&cv, l1.cond, &back);
  HT_CHECK(!memcmp(back.value, cv2.value, sizeof(int) * (size_t)cv.n));
  xts_cond_from_text(&cv2, l.cond, &back);
  HT_CHECK(!memcmp(back.value, cv.value, sizeof(int) * (size_t)cv.n));
  HT_EQ_U64(xts_rline_parse(got[3], &l3), 0);
  HT_CHECK(!strcmp(l3.case_id, "summary") && !strcmp(l3.block, "r0:rline.t:finish"));
  HT_CHECK(!strcmp(l3.cond, l1.cond));                              /* unknown axis / value ignored: base value kept */
  free_got();
}

HTEST(rline_hand_lines)
{
  xts_rline_t l;
  char buf[48];
  /* shuffled keys, whitespace, "truncated" after the detail, a string value holding the text ,"detail": */
  const char *a =
      " { \"status\" : \"pass\" , \"detail\" : { \"v\" : 12.5e1 , \"t\" : true , \"z\" : \"q\" } , \"truncated\" : true ,"
      "\"value\":null,\"cond\":{ \"sf\" : \"64\" },\"metric\":\"result\",\"case_id\":\"x\\\"y,\\\"detail\\\":\",\"block\":\"r7:f:-:c0\"}";
  HT_EQ_U64(xts_rline_parse(a, &l), 0);
  HT_CHECK(l.status == XTS_ST_PASS && isnan(l.value) && !strcmp(l.case_id, "x\"y,\"detail\":") && xts_rline_row(&l) == 7);
  HT_CHECK(xts_rline_num(&l, "v") == 125.0 && isnan(xts_rline_num(&l, "t")) && xts_rline_str(&l, "z", buf, sizeof buf));
  HT_CHECK(xts_rline_cond(&l, "sf", buf, sizeof buf) && !strcmp(buf, "64"));
  HT_CHECK(!strcmp(l.family, "") && !strcmp(l.unit, ""));
  /* block key without a row prefix */
  HT_CHECK(xts_rline_parse("{\"case_id\":\"c\",\"metric\":\"m\",\"status\":\"fail\",\"cond\":{},\"block\":\"-\"}", &l) == 0 &&
           l.row == -1 && !strcmp(l.cond, "") && !strcmp(l.detail, "{}"));
  HT_CHECK(xts_rline_parse("{\"case_id\":\"c\",\"metric\":\"m\",\"status\":\"fail\",\"cond\":{},\"block\":\"r:x\"}", &l) == 0 && l.row == -1);
  HT_CHECK(xts_rline_parse("{\"case_id\":\"c\",\"metric\":\"m\",\"status\":\"fail\",\"cond\":{},\"block\":\"r41x\"}", &l) == 0 && l.row == -1);
  HT_CHECK(xts_rline_parse("{\"case_id\":\"c\",\"metric\":\"m\",\"status\":\"fail\",\"cond\":{}}", &l) == 0 && l.block[0] == 0 && l.row == -1);
  /* long strings are truncated, not overrun */
  char big[900];
  int o = snprintf(big, sizeof big, "{\"case_id\":\"");
  memset(big + o, 'k', 300);
  snprintf(big + o + 300, sizeof big - (size_t)o - 300, "\",\"metric\":\"m\",\"status\":\"info\",\"cond\":{}}");
  HT_CHECK(xts_rline_parse(big, &l) == 0 && strlen(l.case_id) == sizeof l.case_id - 1);
  /* not records */
  HT_CHECK(xts_rline_parse("", &l) < 0 && xts_rline_parse("[]", &l) < 0 && xts_rline_parse("{}", &l) < 0);
  HT_CHECK(xts_rline_parse(NULL, &l) < 0);
  HT_CHECK(xts_rline_parse("{\"case_id\":\"c\",\"metric\":\"m\",\"status\":\"bogus\",\"cond\":{}}", &l) < 0);
  HT_CHECK(xts_rline_parse("{\"case_id\":\"c\",\"metric\":\"m\",\"cond\":{}}", &l) < 0);                       /* no status */
  HT_CHECK(xts_rline_parse("{\"case_id\":\"c\",\"metric\":\"m\",\"status\":\"pass\"}", &l) < 0);                /* no cond */
  HT_CHECK(xts_rline_parse("{\"case_id\":\"c\",\"metric\":\"m\",\"status\":\"pass\",\"cond\":{\"a\":\"b", &l) < 0);
  HT_CHECK(xts_rline_parse("{\"case_id\":\"c\",\"status\":\"pass\",\"cond\":{}}", &l) < 0);                     /* no metric */
  char longc[XTS_RL_COND + 64], tail[] = "\"}}";
  o = snprintf(longc, sizeof longc, "{\"case_id\":\"c\",\"metric\":\"m\",\"status\":\"pass\",\"cond\":{\"a\":\"");
  memset(longc + o, 'v', XTS_RL_COND);
  strcpy(longc + o + XTS_RL_COND, tail);
  HT_CHECK(xts_rline_parse(longc, &l) < 0);
}

HTEST(rline_cond_text)
{
  xts_condvec_t base, out;
  xts_plan_condvec(NULL, &base);
  xts_cond_from_text(&base, "", &out);
  HT_CHECK(!memcmp(out.value, base.value, sizeof(int) * (size_t)base.n));
  /* keys in any order, escaped value text, nested garbage ignored */
  xts_cond_from_text(&base, "\"fpscr_rn\":\"rp\",\"zz\":\"1\",\"sf\":\"32\"", &out);
  HT_CHECK(!strcmp(xts_cond_get(&out, "sf"), "32") && !strcmp(xts_cond_get(&out, "fpscr_rn"), "rp"));
  xts_cond_from_text(&base, "\"sf\":\"3\\u0032\"", &out);
  HT_CHECK(!strcmp(xts_cond_get(&out, "sf"), "32"));
  /* a prefix of an axis name does not match */
  xts_cond_from_text(&base, "\"fpscr_r\":\"rp\"", &out);
  HT_CHECK(!strcmp(xts_cond_get(&out, "fpscr_rn"), xts_cond_get(&base, "fpscr_rn")));
  /* full table: -1 */
  xts_condtab_t t;
  memset(&t, 0, sizeof t);
  xts_rline_t l;
  char line[200];
  for (int i = 0; i < XTS_CONDTAB_MAX + 2; i++) {
    snprintf(line, sizeof line, "{\"case_id\":\"c\",\"metric\":\"m\",\"status\":\"pass\",\"cond\":{\"k\":\"%d\"}}", i);
    HT_EQ_U64(xts_rline_parse(line, &l), 0);
    int slot = xts_condtab_add(&t, &l);
    HT_CHECK(i < XTS_CONDTAB_MAX ? slot == i : slot == -1);
  }
}
