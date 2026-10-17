/* sysinfo.json: console identity and build provenance. The platform layer adds the hardware part
 * (xts_plat_sysinfo). Never contains the CPU key, the keyvault or fuse lines 3-6. */
#include <stdio.h>
#include <string.h>
#include "core.h"
#include "build_id.h"   /* generated: XTS_GIT_HASH, XTS_BUILD_ID */

#ifndef XTS_GIT_HASH
#define XTS_GIT_HASH "unknown"
#endif
#ifndef XTS_BUILD_ID
#define XTS_BUILD_ID "unknown"
#endif

const char *xts_build_id(void) { return XTS_BUILD_ID; }
const char *xts_git_hash(void) { return XTS_GIT_HASH; }

int xts_sysinfo_write(const char *path, const char *run_id, const char *tag, int boot)
{
  static char buf[16384];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_kv_str(&j, "suite", "xts");
  xj_kv_str(&j, "version", XTS_VERSION);
  xj_kv_str(&j, "build_id", xts_build_id());
  xj_kv_str(&j, "git_hash", xts_git_hash());
  xj_kv_str(&j, "profile", xts_active_profile_name ? xts_active_profile_name : "");
  xj_kv_str(&j, "isa_sha256", xts_isa_sha256);
  xj_kv_int(&j, "isa_instructions", xts_isa_count);
  xj_kv_int(&j, "model_version", XTS_MODEL_VERSION);
  xj_kv_int(&j, "model_handlers", xts_model_handler_count());
  xj_kv_str(&j, "run_id", run_id);
  xj_kv_str(&j, "console_tag", tag);
  xj_kv_int(&j, "boot", boot);
  xj_kv_double(&j, "cycles_per_tick", xts_cycles_per_tick);
  xj_key(&j, "crashpage_survived");                /* did the RAM crash page survive the last reboot (main.c) */
  if (xts_crashpage_survived < 0) xj_null(&j);
  else xj_bool(&j, xts_crashpage_survived);
  xj_kv_str(&j, "crashpage_how", xts_crashpage_how);
  xj_key(&j, "families");
  xj_arr(&j);
  for (int i = 0; i < xts_family_count(); i++) xj_str(&j, xts_family_name(xts_family_get(i)));
  xj_end(&j);
  xj_key(&j, "axes");
  xj_obj(&j);
  for (int i = 0; i < xts_axis_count(); i++) {
    const xts_axis_t *a = xts_axis_get(i);
    xj_key(&j, a->name);
    xj_arr(&j);
    for (int v = 0; a->values[v]; v++) xj_str(&j, a->values[v]);
    xj_end(&j);
  }
  xj_end(&j);
  xj_key(&j, "platform");
  xj_obj(&j);
  xts_plat_sysinfo(&j);
  xj_end(&j);
  xj_finish(&j);
  if (j.truncated) xts_log("sysinfo.json truncated");
  return xts_write_small(path, buf);
}
