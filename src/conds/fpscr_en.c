/* fpscr_en - FPSCR exception-enable bits of the case input state: none (default), ve, oe, ue, ze, xe, all. MSR[FE0/FE1]
 * stay 0, so no floating-point exception interrupt is taken, but an enabled exception changes what the instruction does to
 * the target and to FPSCR (e.g. no result written for an enabled invalid/zero-divide, FEX/VX behaviour). Bits (low 32 of
 * FPSCR, IBM bits 24-28): VE 0x80, OE 0x40, UE 0x20, ZE 0x10, XE 0x08. FPSCR[NI] (bit 29) is NOT covered: it cannot be
 * set on Xenon (src/model/KNOWN.md), so there is no fpscr_ni axis. */
#include <string.h>
#include <xts/xts.h>

#define EN_MASK 0xF8u

static const char *const en_values[] = { "none", "ve", "oe", "ue", "ze", "xe", "all", NULL };

static int en_apply(int v)
{
  static const uint32_t bits[] = { 0, 0x80, 0x40, 0x20, 0x10, 0x08, EN_MASK };
  if (v < 0 || v > 6) return -1;
  xts_env.fpscr_clr |= EN_MASK;
  xts_env.fpscr_set = (xts_env.fpscr_set & ~EN_MASK) | bits[v];
  return 0;
}

static void en_revert(int v)
{
  (void)v;
  xts_env.fpscr_clr &= ~EN_MASK;
  xts_env.fpscr_set &= ~EN_MASK;
}

static int fp_applies(const char *fam, const char *kind)
{
  (void)fam;
  return kind && (!strcmp(kind, "semantic") || !strcmp(kind, "timing") || !strcmp(kind, "self"));
}

XTS_AXIS(fpscr_en, .values = en_values, .apply = en_apply, .revert = en_revert, .applies_to = fp_applies, .order = 11,
         .desc = "FPSCR exception-enable bits of the case input: none, ve, oe, ue, ze, xe, all");
