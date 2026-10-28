/* loaduse: ldst.* at the default condition - the direct load-to-use chains (luse.*) next to the CR-detour lat.* values
 * they replace for FPR/VR forms, and the use.* / ltp.* / lhs.* records of the same run. About a minute. */
#include <xts/profile.h>

XTS_PROFILE(loaduse, "ldst.* at the default condition (direct FP/VR load-to-use chains)",
  { .families = "self.cores" },
  { .families = "calib" },
  { .families = "ldst.*" },
);
