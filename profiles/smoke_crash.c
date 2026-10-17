/* smoke_crash: verifies the crash path on the console. Self-test, a few exec blocks, then self.crash, whose last case
 * executes tw 31,0,0 from C outside every trap range (not inside the slot). Expected:
 *   boot 1: rows 0-5 of the screen: "XTS CRASH vector 0700 srr0 ...", "in case block r3:self.crash:-:c0 case 3 crash",
 *           "report saved to uda:/xts/<tag>/<run>/crash.json", "stick is safe to reset"; the console spins.
 *           crash.json (valid JSON on a PC) has "state":"crashed", the registers, the case and the last 50 log lines;
 *           journal.txt ends with "BEGIN r3:self.crash:-:c0" (no END); progress.bin names case 3 ("crash").
 *   reset (SMC reboot / watchdog / power-cycle) and boot the SAME ELF: the run resumes ("resuming run ..."), records
 *           case "crash" of self.crash as status crash (detail.case_from "crash.json", detail.crash_report = the report;
 *           also copied to crash_boot1.json), quarantines it (QUAR ... case=3 status=crash), runs ok0..ok2 again and
 *           finishes (DONE, "stick unmounted: safe to power off"). tools/fat_check.py on an image of the stick: clean. */
#include <xts/profile.h>

XTS_PROFILE(smoke_crash, "self-test + a few exec blocks + a deliberate crash (self.crash); reboot to check the resume",
  { .families = "self.exec" },
  { .families = "self.kernel" },
  { .families = "exec.ppc", .max_cases = 48 },
  { .families = "self.crash" },
);
