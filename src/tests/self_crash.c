/* self.crash - deliberate crash, to verify the crash path end to end on the console (profile smoke_crash):
 *   ok0..ok2   pass (so the block has records before the crash)
 *   crash      executes tw 31,0,0 from C, OUTSIDE the slot / kernel buffer / extra trap ranges: libxenon's program
 *              vector -> crashdump (src/exec/vec.S: emergency stack) -> xts_crash_entry -> xts_crash_handle: the report
 *              (registers, case, last log lines) goes into the run's pre-allocated crash.json through the disc guard's
 *              raw API (no libfat), the screen says "report saved to ... crash.json" + "stick is safe to reset", spin.
 * After the reset (SMC reboot / watchdog / power cycle) the resumed run turns crash.json into a crash record (case
 * "crash", status crash, detail.crash_report = the whole report), quarantines that case and finishes the profile.
 *
 * Opt-in: the family has cases only when a row of the running profile names "self.crash" exactly, so "self.*" rows of
 * the other profiles never crash. On the PC the crash case records skip unless a host test simulates the crash path in a
 * forked boot (xts_host_crash_sim). Uses the framework-private crash interface (<core/core.h>): a harness self-test. */
#include <xts/xts.h>
#include <core/core.h>

static int crash_enum(xts_caselist_t *cl)
{
  if (!xts_profile_names_family("self.crash")) return 0;
  for (int i = 0; i < 3; i++)
    if (!xts_case_addf(cl, "ok%d", i)) return -1;
  xts_case_t *c = xts_case_add(cl, "crash");
  if (!c) return -1;
  c->arg[0] = 1;
  return 0;
}

static int crash_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (!c->arg[0]) {
    xts_emit_status(ctx, XTS_ST_PASS, "{\"note\":\"before the deliberate crash\"}");
    return 0;
  }
  /* nothing is written here (no file I/O while cases run): the block's records reach the stick only at its END, so the
   * resumed block (which re-runs ok0..ok2) does not duplicate them; crash.json is the evidence */
  int r = xts_plat_self_crash();          /* console: does not return when the crash path works */
  if (r < 0) {
    xts_emit_status(ctx, XTS_ST_SKIP, "{\"why\":\"host: the deliberate crash only runs on the console\"}");
    return 0;
  }
  xts_emit_status(ctx, XTS_ST_FAIL, "{\"reason\":\"tw 31,0,0 outside the trap ranges returned: the crash path did not run\"}");
  return 0;
}

XTS_FAMILY(self_crash, .kind = "self", .enumerate = crash_enum, .run = crash_run, .block_cases = 16,
           .desc = "deliberate crash outside the trap ranges (opt-in: only rows naming self.crash)");
