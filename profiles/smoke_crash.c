/* smoke_crash: verifies the crash path on the console. Self-test, a few exec blocks, then self.crash, whose last case
 * executes tw 31,0,0 from C outside every trap range. Expected:
 *   boot 1: "XTS CRASH vector 0700 ..." on screen (status row + text), journal.txt ends with
 *           "CRASH r3:self.crash:-:c0 case=3 vec=0700 srr0=... dar=...", the console spins;
 *   reboot (SMC reboot / watchdog / power-cycle) and boot the SAME ELF: the run resumes ("resuming run ..."),
 *           records case "crash" of self.crash as status crash (case_from: crash page or journal CRASH line),
 *           quarantines it (QUAR ... case=3 status=crash), runs ok0..ok2 again and finishes (DONE);
 *   sysinfo_boot2.json "crashpage_survived" says whether the RAM crash page survived that reboot. */
#include <xts/profile.h>

XTS_PROFILE(smoke_crash, "self-test + a few exec blocks + a deliberate crash (self.crash); reboot to check the resume",
  { .families = "self.exec" },
  { .families = "self.kernel" },
  { .families = "exec.ppc", .max_cases = 48 },
  { .families = "self.crash" },
);
