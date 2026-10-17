/* code_align - byte offset of the kernel's first word within its 128-byte line. xts_exec_kernel() places the kernel at
 * kbuf (128-byte aligned) + (xts_env.code_offset & 124) with never-executed nops in front, so only word offsets 0..124
 * apply. Loop tops aligned with xk_align()/spec.align are aligned relative to the kernel start, i.e. their absolute
 * offset is code_offset + that. */
#include <string.h>
#include <xts/xts.h>

static const char *const ca_values[] = { "0", "4", "8", "16", "32", "64", NULL };

static int ca_apply(int v)
{
  static const uint32_t off[] = { 0, 4, 8, 16, 32, 64 };
  if (v < 0 || v > 5) return -1;
  xts_env.code_offset = off[v];
  return 0;
}

static void ca_revert(int v)
{
  (void)v;
  xts_env.code_offset = 0;
}

static int ca_applies(const char *fam, const char *kind)
{
  (void)fam;
  return kind && (!strcmp(kind, "timing") || !strcmp(kind, "sched") || !strcmp(kind, "smt") || !strcmp(kind, "self"));
}

XTS_AXIS(code_align, .values = ca_values, .apply = ca_apply, .revert = ca_revert, .applies_to = ca_applies, .order = 30,
         .desc = "offset of the kernel start within its 128-byte line: 0 4 8 16 32 64 bytes");
