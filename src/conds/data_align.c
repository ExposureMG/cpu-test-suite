/* data_align - alignment of the data addresses a family generates: aligned (default), mis1 (base+1), mis4 (base+4: word
 * aligned, not doubleword/quadword aligned), line_cross (access straddles a 128-byte line end), page_cross (straddles a
 * 4 KB boundary), page64k_cross (straddles a 64 KB boundary - libxenon maps 64 KB pages, so only this one crosses a real
 * page/TLB entry). Sets xts_env.data_kind / data_offset; families add xts_cond_data_offset(width) (cond_util.h) to a
 * 128-byte aligned base. Page crossing is only possible in a xts_big() buffer. */
#include <string.h>
#include <xts/xts.h>

static const char *const da_values[] = { "aligned", "mis1", "mis4", "line_cross", "page_cross", "page64k_cross", NULL };

static int da_apply(int v)
{
  static const int kind[] = { 0, 1, 1, 2, 3, 4 };
  static const uint32_t off[] = { 0, 1, 4, 124, 4092, 65532 };
  if (v < 0 || v > 5) return -1;
  xts_env.data_kind = kind[v];
  xts_env.data_offset = off[v];
  return 0;
}

static void da_revert(int v)
{
  (void)v;
  xts_env.data_kind = 0;
  xts_env.data_offset = 0;
}

static int da_applies(const char *fam, const char *kind)
{
  (void)fam;
  return kind && (!strcmp(kind, "timing") || !strcmp(kind, "sched") || !strcmp(kind, "mem") || !strcmp(kind, "semantic") ||
                  !strcmp(kind, "smt") || !strcmp(kind, "self"));
}

XTS_AXIS(data_align, .values = da_values, .apply = da_apply, .revert = da_revert, .applies_to = da_applies, .order = 31,
         .desc = "data address alignment: aligned, mis1, mis4, line_cross, page_cross (4 KB), page64k_cross (64 KB: libxenon page size)");
