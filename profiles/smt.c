/* smt: the timing/scheduling families under sibling-thread load, other-core load and thread priority, one axis at a
 * time (the default-condition baseline comes from the timing/sched profiles). */
#include <xts/profile.h>

XTS_PROFILE(smt, "timing/scheduling families x sibling, x other_cores, x thread_prio (one axis at a time)",
  { .families = "self.*" },
  { .families = "calib*" },
  { .families = "lat.*",   .axes = "sibling=*", .sweep = 1 },
  { .families = "tput.*",  .axes = "sibling=*", .sweep = 1 },
  { .families = "issue.*", .axes = "sibling=*", .sweep = 1 },
  { .families = "unit.*",  .axes = "sibling=*", .sweep = 1 },
  { .families = "ucode*",  .axes = "sibling=*", .sweep = 1 },
  { .families = "lat.*",   .axes = "thread_prio=*", .sweep = 1 },
  { .families = "tput.*",  .axes = "thread_prio=*", .sweep = 1 },
  { .families = "lat.*",   .axes = "other_cores=*", .sweep = 1 },
  { .families = "mem.*",   .axes = "other_cores=*", .sweep = 1 },
  { .families = "ldst.*",  .axes = "other_cores=*", .sweep = 1 },
);
