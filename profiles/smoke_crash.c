/* smoke_crash: verifies the crash path on the console (step 4 of the first console run, README "Output"). Self-test, a
 * few exec blocks, then self.crash, whose last case executes tw 31,0,0 on the test core outside every trap range.
 * Expected at the receiver (tools/xts_recv.py): the records of every block before it, a crash record for case "crash"
 * of block r3:self.crash:-:c0, a CRASH report (crash.json: "vector":"0700", the registers, the case, the last 50 log
 * lines) and END with status "crashed" - a COMPLETE transfer whose verdict says "END status crashed". The console screen
 * says the test core crashed and that the report was sent; it halts (power-cycle). The transmitter core survives the
 * crash: that is what this profile proves. No resume: the next boot starts a new run. */
#include <xts/profile.h>

XTS_PROFILE(smoke_crash, "self-test + a few exec blocks + a deliberate crash (self.crash): CRASH report + END crashed",
  { .families = "self.exec" },
  { .families = "self.kernel" },
  { .families = "exec.ppc", .max_cases = 48 },
  { .families = "self.crash" },
);
