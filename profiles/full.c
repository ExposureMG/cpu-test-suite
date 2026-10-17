/* full: everything - the rows of semantic, timing, sched, mem and smt, in that order. Axes are swept one at a time
 * (sweep rows) on top of a default-condition pass, which keeps the run tractable. */
#include <xts/profile.h>

XTS_PROFILE(full, "semantic + timing + sched + mem + smt",
  { .families = "self.*", .axes = "sf=*" },
  { .families = "calib*" },
  /* semantic */
  { .families = "exec.*", .axes = "sf=*" },
  { .families = "exec.*", .axes = "fpscr_rn=rz,rp,rm", .sweep = 1 },
  { .families = "exec.*", .axes = "fpscr_en=ve,oe,ue,ze,xe,all", .sweep = 1 },
  { .families = "exec.*", .axes = "vscr_nj=1", .sweep = 1 },
  { .families = "vmx128.*", .axes = "sf=*" },
  /* timing */
  { .families = "lat.*" },
  { .families = "lat.*", .axes = "code_align=4,8,16,32,64", .sweep = 1 },
  { .families = "tput.*" },
  /* sched */
  { .families = "bypass.*" },
  { .families = "issue.*" },
  { .families = "unit.*" },
  { .families = "ucode*" },
  { .families = "ldst.*" },
  { .families = "ldst.*", .axes = "cache=*", .sweep = 1 },
  { .families = "ldst.*", .axes = "data_align=*", .sweep = 1 },
  { .families = "branch*" },
  { .families = "branch*", .axes = "code_align=4,8,16,32,64", .sweep = 1 },
  { .families = "sysops*" },
  /* mem */
  { .families = "mem.*" },
  /* smt */
  { .families = "lat.*",   .axes = "sibling=*", .sweep = 1 },
  { .families = "tput.*",  .axes = "sibling=*", .sweep = 1 },
  { .families = "issue.*", .axes = "sibling=*", .sweep = 1 },
  { .families = "unit.*",  .axes = "sibling=*", .sweep = 1 },
  { .families = "ucode*",  .axes = "sibling=*", .sweep = 1 },
  { .families = "lat.*",   .axes = "thread_prio=*", .sweep = 1 },
  { .families = "lat.*",   .axes = "other_cores=*", .sweep = 1 },
  { .families = "mem.*",   .axes = "other_cores=*", .sweep = 1 },
  { .families = "ldst.*",  .axes = "other_cores=*", .sweep = 1 },
);
