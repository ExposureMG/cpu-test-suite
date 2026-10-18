/* self.crash - deliberate crash, to verify the crash path end to end on the console (profile smoke_crash):
 *   ok0..ok2   pass (so the block has records before the crash)
 *   crash      xts_plat_self_crash: executes tw 31,0,0 on the TEST core, outside the slot / kernel buffer / extra trap
 *              ranges: its crash handler fills the PIR 0 crash slot (xts/xcore.h), xts_xc_call returns XTS_XC_CRASHED
 *              and the suite (xts_core_test_core_lost) sends a crash record for this case, the CRASH report (registers,
 *              case, input state, last log lines) and END with status "crashed", then halts. Nothing is stored.
 * The receiver writes crash.json and suggests the quarantine.txt line ("self.crash crash"); do NOT add it - the profile
 * exists to crash.
 *
 * Opt-in: the family has cases only when a row of the running profile names "self.crash" exactly, so "self.*" rows of
 * the other profiles never crash. On the PC the crash case records skip unless a host test simulates the crash in a
 * forked child (xts_host_crash_sim). Uses the framework-private crash interface (<core/core.h>): a harness self-test. */
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
  /* the records of ok0..ok2 are already on the wire; the CRASH report is the evidence */
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
