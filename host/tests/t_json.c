/* JSON builder, glob, prep */
#include <string.h>
#include <xts/xts.h>
#include "htest.h"

HTEST(json_builder)
{
  char buf[256];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_kv_str(&j, "a", "x\"y");
  xj_kv_hex(&j, "h", 0x1234, 8);
  xj_key(&j, "arr");
  xj_arr(&j);
  xj_int(&j, 1);
  xj_int(&j, -2);
  xj_obj(&j);
  xj_end(&j);
  xj_end(&j);
  xj_kv_double(&j, "n", 0.0 / 0.0);
  xj_end(&j);
  HT_CHECK(!strcmp(buf, "{\"a\":\"x\\\"y\",\"h\":\"00001234\",\"arr\":[1,-2,{}],\"n\":null}"));
  HT_CHECK(!j.truncated);
  /* truncation keeps the text valid and flags it */
  char small[40];
  xj_init(&j, small, sizeof small);
  xj_obj(&j);
  for (int i = 0; i < 20; i++) xj_kv_int(&j, "key", i);
  xj_key(&j, "inner");
  xj_obj(&j);
  xj_kv_str(&j, "s", "a long string that will not fit at all");
  xj_end(&j);
  xj_finish(&j);
  HT_CHECK(j.truncated);
  HT_CHECK(small[0] == '{' && small[strlen(small) - 1] == '}');
  HT_EQ_U64(ht_count_substr(small, "{"), ht_count_substr(small, "}"));
  HT_CHECK(strstr(small, ":}") == NULL && strstr(small, ",}") == NULL);
}

HTEST(glob_and_cond)
{
  HT_CHECK(xts_glob("exec.*", "exec.ppc"));
  HT_CHECK(!xts_glob("exec.*", "lat.chain"));
  HT_CHECK(xts_glob("*", "anything"));
  HT_CHECK(xts_glob("ucode*", "ucode"));
  HT_CHECK(xts_glob("l?t.*", "lat.chain"));
  HT_CHECK(!xts_glob("lat", "lat.chain"));
}

HTEST(prep_cycles)
{
  static uint8_t buf[1 << 16];
  uint64_t span;
  /* 64 elements of 64 bytes, 4 random cycles, pointer in slot 1 */
  HT_EQ_U64(xts_prep_pure(buf, 0x80000000u, sizeof buf, 0, 64, 64, 0, 4, 7, 1, &span), 0);
  HT_EQ_U64(span, 64 * 64);
  int seen[64] = { 0 }, bad = 0, total = 0;
  for (int c = 0; c < 4; c++) {
    int i = c, len = 0;
    do {
      if (seen[i]++) { bad++; break; }
      total++;
      len++;
      uint64_t p = xts_be_load(buf + i * 64 + 8, 8);
      uint64_t rel = p - (0x80000000u + 8);
      if (rel % 64 || rel / 64 >= 64) { bad++; break; }
      i = (int)(rel / 64);
    } while (i != c && len < 100);
    HT_EQ_U64(len, 16);
  }
  HT_EQ_U64(bad, 0);
  HT_EQ_U64(total, 64);
  HT_CHECK(xts_prep_pure(buf, 0, sizeof buf, 4, 8, 8, 0, 1, 1, 0, &span) < 0);   /* base not 8-aligned */
  HT_CHECK(xts_prep_pure(buf, 0, sizeof buf, 0, 1 << 20, 8, 2, 1, 1, 0, &span) < 0);   /* too big */
}
