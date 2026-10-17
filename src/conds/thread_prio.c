/* thread_prio - SMT thread priority of the test thread while kernels run: default (untouched), low `or 1,1,1`, medium
 * `or 2,2,2`, high `or 3,3,3` (the encodings are the ISA table's cctpl/cctpm/cctph; Power ISA 2.02 Book I lists 1 low,
 * 2 medium, 6 medium-low; high needs a privileged state, we run in hypervisor state). The kernel builder's
 * xk_prio_prologue() (used by the timing frame) emits the nop at the start of every kernel from xts_env.thread_prio; the
 * priority persists after the kernel, so revert puts the thread back at medium. */
#include <string.h>
#include <xts/xts.h>
#include "cond_util.h"

static const char *const tp_values[] = { "default", "low", "medium", "high", NULL };

static int tp_apply(int v)
{
  if (v < 0 || v > 3) return -1;
  xts_env.thread_prio = v;
  return 0;
}

static void tp_revert(int v)
{
  xts_env.thread_prio = 0;
  if (v > 0) xts_cond_set_thread_prio(2);
}

static int tp_applies(const char *fam, const char *kind)
{
  (void)fam;
  return kind && (!strcmp(kind, "timing") || !strcmp(kind, "sched") || !strcmp(kind, "smt") || !strcmp(kind, "mem") ||
                  !strcmp(kind, "self"));
}

XTS_AXIS(thread_prio, .values = tp_values, .apply = tp_apply, .revert = tp_revert, .applies_to = tp_applies, .order = 40,
         .desc = "thread priority nop at kernel start: default, low (or 1,1,1), medium (or 2,2,2), high (or 3,3,3)");
