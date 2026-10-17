/* depth - the timing fit's depth list and the number of independent chains, as two axes (the names match xts_env):
 *   unroll: default | short {2,3,4,5,6,8} | long {16,24,32,48,64,96} | pow2 {1,2,4,8,16,32} | odd {3,5,7,9,11,13}
 *   chains: default | 1 | 2 | 4 | 8 | 16
 * The timing engine takes its depths from the family's xts_tspec_t, so the families must opt in: call
 * xts_cond_unroll(&spec) and xts_cond_chains(default) from cond_util.h when building the spec. */
#include <string.h>
#include <xts/xts.h>

static const char *const un_values[] = { "default", "short", "long", "pow2", "odd", NULL };
static const char *const ch_values[] = { "default", "1", "2", "4", "8", "16", NULL };

static int un_apply(int v)
{
  if (v < 0 || v > 4) return -1;
  xts_env.unroll = v;
  return 0;
}
static void un_revert(int v) { (void)v; xts_env.unroll = 0; }

static int ch_apply(int v)
{
  static const int n[] = { 0, 1, 2, 4, 8, 16 };
  if (v < 0 || v > 5) return -1;
  xts_env.chains = n[v];
  return 0;
}
static void ch_revert(int v) { (void)v; xts_env.chains = 0; }

static int td_applies(const char *fam, const char *kind)
{
  (void)fam;
  return kind && !strcmp(kind, "timing");
}

XTS_AXIS(unroll, .values = un_values, .apply = un_apply, .revert = un_revert, .applies_to = td_applies, .order = 50,
         .desc = "timing depth list: default, short, long, pow2, odd (families call xts_cond_unroll)");
XTS_AXIS(chains, .values = ch_values, .apply = ch_apply, .revert = ch_revert, .applies_to = td_applies, .order = 51,
         .desc = "independent chains of a throughput body: default, 1, 2, 4, 8, 16 (families call xts_cond_chains)");
