/* mem: memory hierarchy (pointer chases, strides, prefetch, dcbz, TLB) and load/store latency by cache state. */
#include <xts/profile.h>

XTS_PROFILE(mem, "mem.* and ldst.* across cache state and data alignment",
  { .families = "self.*" },
  { .families = "calib*" },
  { .families = "mem.*" },
  { .families = "ldst.*" },
  { .families = "ldst.*", .axes = "cache=*", .sweep = 1 },
  { .families = "ldst.*", .axes = "data_align=*", .sweep = 1 },
);
