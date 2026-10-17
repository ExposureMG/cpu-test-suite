/* fpscr_rn - FPSCR[RN] rounding mode in the case input state: rn (nearest even, default), rz, rp, rm. Applied through
 * xts_env.fpscr_set/clr (bits 0-1) by xts_case_init. */
#include <string.h>
#include <xts/xts.h>

#define RN_MASK 3u

static const char *const rn_values[] = { "rn", "rz", "rp", "rm", NULL };

static int rn_apply(int v)
{
  if (v < 0 || v > 3) return -1;
  xts_env.fpscr_clr |= RN_MASK;
  xts_env.fpscr_set = (xts_env.fpscr_set & ~RN_MASK) | (uint32_t)v;
  return 0;
}

static void rn_revert(int v)
{
  (void)v;
  xts_env.fpscr_clr &= ~RN_MASK;
  xts_env.fpscr_set &= ~RN_MASK;
}

static int fp_applies(const char *fam, const char *kind)
{
  (void)fam;
  return kind && (!strcmp(kind, "semantic") || !strcmp(kind, "timing") || !strcmp(kind, "self"));
}

XTS_AXIS(fpscr_rn, .values = rn_values, .apply = rn_apply, .revert = rn_revert, .applies_to = fp_applies, .order = 10,
         .desc = "FPSCR[RN] of the case input: rn, rz, rp, rm");
