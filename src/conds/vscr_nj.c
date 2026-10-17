/* vscr_nj - VSCR[NJ] (non-Java mode, flush denormals) of the case input state: 0 (default) or 1. xts_env.vscr_set/clr,
 * applied by xts_case_init. VSCR[SAT] is left to the cases. */
#include <string.h>
#include <xts/xts.h>

static const char *const nj_values[] = { "0", "1", NULL };

static int nj_apply(int v)
{
  if (v < 0 || v > 1) return -1;
  xts_env.vscr_clr |= XTS_VSCR_NJ;
  xts_env.vscr_set = (xts_env.vscr_set & ~XTS_VSCR_NJ) | (v ? XTS_VSCR_NJ : 0);
  return 0;
}

static void nj_revert(int v)
{
  (void)v;
  xts_env.vscr_clr &= ~XTS_VSCR_NJ;
  xts_env.vscr_set &= ~XTS_VSCR_NJ;
}

static int nj_applies(const char *fam, const char *kind)
{
  (void)fam;
  return kind && (!strcmp(kind, "semantic") || !strcmp(kind, "timing") || !strcmp(kind, "self"));
}

XTS_AXIS(vscr_nj, .values = nj_values, .apply = nj_apply, .revert = nj_revert, .applies_to = nj_applies, .order = 12,
         .desc = "VSCR[NJ] of the case input: 0 or 1");
