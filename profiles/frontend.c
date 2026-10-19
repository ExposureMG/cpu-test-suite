/* frontend: map the loop cost per iteration against body length and loop-top offset (frontend.loop), to choose the
 * timing depth method. Quick: 32 offsets x 160 lengths, about a second of measuring.
 * Expected on the PC: complete run, every frontend.loop record info (the map is in detail.cyc). */
#include <xts/profile.h>

XTS_PROFILE(frontend, "loop cost vs body length and loop-top offset (depth method data)",
  { .families = "self.cores" },
  { .families = "calib" },
  { .families = "frontend.*" },
);
