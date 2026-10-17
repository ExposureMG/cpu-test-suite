/* smoke: first console run (about a minute). USB + identity (sysinfo.json) + harness self-test + an evenly strided
 * sample of every exec.* family in both MSR[SF] modes (max_cases strides over the enumeration order, so every
 * instruction group of an instruction-ordered family is still hit). exec.est writes ~18 records per case (lane sweeps),
 * hence its small cap. Under 500 records (host dry run; host test rob_smoke_small). self.crash is opt-in: not here. */
#include <xts/profile.h>

XTS_PROFILE(smoke, "USB + identity + self-test (nop / tw 31,0,0 / word 0, kernels) x sf + sampled exec.* x sf",
  { .families = "self.*", .axes = "sf=*" },
  { .families = "exec.ppc", .axes = "sf=*", .max_cases = 32 },
  { .families = "exec.fpu", .axes = "sf=*", .max_cases = 24 },
  { .families = "exec.altivec", .axes = "sf=*", .max_cases = 24 },
  { .families = "exec.vmx128", .axes = "sf=*", .max_cases = 24 },
  { .families = "exec.illegal", .axes = "sf=*", .max_cases = 16 },
  { .families = "exec.est", .axes = "sf=*", .max_cases = 4 },
);
