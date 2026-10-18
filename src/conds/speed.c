/* speed - the CPU speed code (libxenon xenon_get_speed) a block runs at. READ ONLY: XTS no longer changes the speed
 * (that needed every other thread asleep - impossible with the suite and the transmitter on cores 1 and 2). The single
 * value "default" leaves the code as XeLL set it; revert() records it in an info record (case "smt:speed") so timing
 * results can be filtered on it. sysinfo.json has it too. Timebase is a fixed reference, so cycles-per-tick depend on
 * the code: timing families rely on calib. */
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include <xts/json.h>
#include <smt/smt.h>

static const char *const speed_values[] = { "default", NULL };
static int code_before;

static int speed_applies(const char *fam, const char *kind)
{
  (void)fam;
  return kind && (!strcmp(kind, "timing") || !strcmp(kind, "sched") || !strcmp(kind, "mem") || !strcmp(kind, "smt") ||
                  !strcmp(kind, "calib"));
}

static int speed_apply(int v)
{
  code_before = xts_smt_speed_get();
  xts_env.speed = 0;
  return v == 0 ? 0 : -1;
}

static void speed_revert(int v)
{
  char det[512];
  xts_json_t j;
  int now = xts_smt_speed_get();
  xj_init(&j, det, sizeof det);
  (void)v;
  xts_smt_info_begin(&j, "speed", speed_values[0]);
  xj_kv_int(&j, "code_before", code_before);
  xj_kv_int(&j, "code_after", now);
  xj_kv_bool(&j, "changed_by_xts", 0);
  xts_smt_temps_json(&j, "temps_end");
  xj_end(&j);
  xts_smt_emit_info("speed", (double)now, "", xj_finish(&j));
  xts_env.speed = 0;
}

XTS_AXIS(speed, .values = speed_values, .apply = speed_apply, .revert = speed_revert, .applies_to = speed_applies,
         .order = 80, .desc = "CPU speed code (read only: recorded, never changed)");
