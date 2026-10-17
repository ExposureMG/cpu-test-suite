/* semantic: every exec.* family (CPU vs reference model) and the VMX128 register map.
 * Row 2 runs everything in both MSR[SF] modes; the sweep rows then vary one FP/vector axis at a time (at SF=64), and
 * only for families sensitive to it, so integer cases are not re-run under rounding modes. */
#include <xts/profile.h>

XTS_PROFILE(semantic, "exec.* x sf, then one-axis sweeps of fpscr_rn / fpscr_en / vscr_nj; vmx128.* x sf",
  { .families = "self.*" },
  { .families = "exec.*", .axes = "sf=*" },
  { .families = "exec.*", .axes = "fpscr_rn=rz,rp,rm", .sweep = 1 },
  { .families = "exec.*", .axes = "fpscr_en=ve,oe,ue,ze,xe,all", .sweep = 1 },
  { .families = "exec.*", .axes = "vscr_nj=1", .sweep = 1 },
  { .families = "vmx128.*", .axes = "sf=*" },
);
