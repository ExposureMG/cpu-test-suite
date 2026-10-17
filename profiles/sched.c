/* sched: what a scheduling compiler needs beyond latency: bypass, pairing, pipes, microcode, load/store, branch, sysops. */
#include <xts/profile.h>

XTS_PROFILE(sched, "bypass, issue pairing, unit conflicts, ucode, ldst, branch, sysops",
  { .families = "self.*" },
  { .families = "calib*" },
  { .families = "bypass.*" },
  { .families = "issue.*" },
  { .families = "unit.*" },
  { .families = "ucode*" },
  { .families = "ldst.*" },
  { .families = "ldst.*", .axes = "data_align=*", .sweep = 1 },
  { .families = "branch*" },
  { .families = "branch*", .axes = "code_align=4,8,16,32,64", .sweep = 1 },
  { .families = "sysops*" },
);
