/* sf - MSR[SF] of the case: 64-bit mode (default, what games run in and what results4 was recorded with) or 32-bit.
 * Applied through xts_env.sf, which xts_case_init() copies into the input state's MODE. */
#include <xts/xts.h>

static const char *const sf_values[] = { "64", "32", NULL };

static int sf_apply(int v)
{
  xts_env.sf = (v == 0);
  return 0;
}

static void sf_revert(int v)
{
  (void)v;
  xts_env.sf = 1;
}

XTS_AXIS(sf, .values = sf_values, .apply = sf_apply, .revert = sf_revert, .order = 0,
         .desc = "MSR[SF] while the case runs: 64 (default) or 32-bit mode");
