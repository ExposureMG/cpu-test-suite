/* speed - CPU clock code (libxenon xenon_get_speed / xenon_make_it_faster) while a block runs.
 *
 *   default  leave it as XeLL left it (recorded in the info record and sysinfo.json)
 *   full     code 1 (XENON_SPEED_FULL)
 *   code2    code 2 (XENON_SPEED_3_2)   code3: 3 (XENON_SPEED_1_2)   code4: 4 (XENON_SPEED_1_3)
 * A value equal to the current code is a no-op. Changing the code is RISKY (it rewrites the VID/clock registers through
 * libxenon's xenon_set_speed, puts every other thread to sleep, sleeps thread 0 and wakes all six) and is compiled in only
 * with CFLAGS_EXTRA=-DXTS_SPEED_RISKY; otherwise such a block is skipped with the reason. Even then apply() refuses unless
 * the sibling and the other cores are idle (this axis applies before sibling/other_cores, order 80); a running watchdog
 * is stopped around the change and restarted. revert() restores the code found at the first change.
 * Timebase is a fixed reference, so cycles-per-tick changes with the code: timing families should rely on calib.
 */
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include <xts/json.h>
#include <smt/smt.h>

static const char *const speed_values[] = { "default", "full", "code2", "code3", "code4", NULL };

static int orig_code = -1, changed;
static int code_before;

static int speed_applies(const char *fam, const char *kind)
{
  (void)fam;
  return kind && (!strcmp(kind, "timing") || !strcmp(kind, "sched") || !strcmp(kind, "mem") || !strcmp(kind, "smt") ||
                  !strcmp(kind, "calib"));
}

static int speed_apply(int v)
{
  changed = 0;
  code_before = xts_smt_speed_get();
  if (orig_code < 0) orig_code = code_before;
  if (v == 0) return 0;
  int code = v;              /* values[1..4] are codes 1..4 */
  xts_env.speed = code;
  if (code == code_before) return 0;
#ifndef XTS_SPEED_RISKY
  xts_log("speed=%s: changing the CPU speed code (%d -> %d) is compiled out (CFLAGS_EXTRA=-DXTS_SPEED_RISKY)",
          speed_values[v], code_before, code);
  xts_env.speed = 0;
  return -1;
#else
  char why[120] = "";
  if (xts_smt_speed_set(code, why, sizeof why) < 0) {
    xts_log("speed=%s: not changed (%d -> %d): %s", speed_values[v], code_before, code, why);
    xts_env.speed = 0;
    return -1;
  }
  changed = 1;
  xts_log("speed=%s: CPU speed code %d -> %d", speed_values[v], code_before, code);
  return 0;
#endif
}

static void speed_revert(int v)
{
  int now = xts_smt_speed_get();
  char det[512], why[120] = "";
  int r = 0;
  if (changed && orig_code > 0 && now != orig_code) {
    r = xts_smt_speed_set(orig_code, why, sizeof why);
    if (r < 0) xts_log("speed revert to code %d FAILED: %s", orig_code, why);
  }
  if (v != 0) {
    xts_json_t j;
    xj_init(&j, det, sizeof det);
    xts_smt_info_begin(&j, "speed", speed_values[v]);
    xj_kv_int(&j, "code_before", code_before);
    xj_kv_int(&j, "code_in_block", now);
    xj_kv_bool(&j, "changed", changed);
    xj_kv_int(&j, "code_after", xts_smt_speed_get());
    xj_kv_str(&j, "restore", r == 0 ? "ok" : why);
#ifdef XTS_SPEED_RISKY
    xj_kv_bool(&j, "risky_build", 1);
#else
    xj_kv_bool(&j, "risky_build", 0);
#endif
    xts_smt_temps_json(&j, "temps_end");
    xj_end(&j);
    xts_smt_emit_info("speed", (double)now, "", xj_finish(&j));
  }
  changed = 0;
  xts_env.speed = 0;
}

XTS_AXIS(speed, .values = speed_values, .apply = speed_apply, .revert = speed_revert, .applies_to = speed_applies,
         .order = 80, .desc = "CPU speed code (libxenon); changes need -DXTS_SPEED_RISKY and idle sibling/other cores");
