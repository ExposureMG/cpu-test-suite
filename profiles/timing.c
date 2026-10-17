/* timing: calibration first (sets cycles per timebase tick), then latency, throughput and data-dependent latency. */
#include <xts/profile.h>

XTS_PROFILE(timing, "calib, lat.*, tput.*",
  { .families = "self.*" },
  { .families = "calib*" },
  { .families = "lat.*" },
  { .families = "lat.*", .axes = "code_align=4,8,16,32,64", .sweep = 1 },
  { .families = "tput.*" },
);
