/* Host unit-test registration (make host-test). Add a file host/tests/<name>.c:
 *
 *   #include "htest.h"
 *   HTEST(my_thing) { HT_CHECK(1 + 1 == 2); HT_EQ_U64(x, 5); }
 *
 * Tests run in registration-section order; a failing HT_* prints file:line and marks the test failed. */
#ifndef XTS_HTEST_H
#define XTS_HTEST_H
#include <stdint.h>
#include <stdio.h>

typedef struct htest { const char *name; void (*fn)(void); } htest_t;
extern int ht_failures, ht_checks;
extern const char *ht_tmp;          /* scratch directory for this test run (--tmp) */

#define HTEST(id)                                                                                        \
  static void htest_fn_##id(void);                                                                       \
  static const htest_t htest_##id = { #id, htest_fn_##id };                                              \
  static const htest_t *const htest_ptr_##id __attribute__((used, section("xts_htest"), aligned(sizeof(void *)))) = &htest_##id; \
  static void htest_fn_##id(void)

#define HT_CHECK(c) do { ht_checks++; if (!(c)) { ht_failures++; printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define HT_EQ_U64(a, b) do { unsigned long long a_ = (unsigned long long)(a), b_ = (unsigned long long)(b); ht_checks++;      \
    if (a_ != b_) { ht_failures++; printf("    FAIL %s:%d: %s == %s (%llX vs %llX)\n", __FILE__, __LINE__, #a, #b, a_, b_); } } while (0)
#define HT_NEAR(a, b, tol) do { double a_ = (a), b_ = (b); ht_checks++;                                                  \
    if (!(a_ - b_ <= (tol) && b_ - a_ <= (tol))) { ht_failures++; printf("    FAIL %s:%d: %s ~ %s (%g vs %g)\n", __FILE__, __LINE__, #a, #b, a_, b_); } } while (0)
/* read a whole file into a static buffer (NULL if missing) */
const char *ht_slurp(const char *path);
int ht_count_substr(const char *hay, const char *needle);
#endif
