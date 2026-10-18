/* other_cores - load on the spare threads of cores 1 and 2 while a block runs: shared-L2 and memory-bus contention.
 * Cores 1 and 2 carry the suite (PIR 2) and the transmitter (PIR 4), so the loads run on their siblings PIR 3 and PIR 5
 * only (xts/xcore.h). The suite is idle while the test core measures and the transmitter is fenced, so during a
 * measurement these two threads are the only other activity.
 *
 *   idle         PIR 3 and 5 in libxenon's wait loop
 *   l2_pressure  each thread: random pointer chase + a store per line over its slice of a 768 KB buffer (L2-resident,
 *                dirty lines: competes for L2 capacity and bandwidth)
 *   mem_stream   each thread: sequential line loads over its own 3 MB slice (memory bandwidth)
 * Every load start/stop is bounded; a failure -> apply() < 0.
 * Trap-range indexes 1..3 (xts_exec_trap_range) are reserved for this axis but not used: the load kernels are fixed and
 * never trap, and an unhandled interrupt on PIR 3/5 lands in that PIR's uncached crash slot (reported at the block end).
 * Per block an info record (case "smt:other_cores", metric "info") has per-thread iterations, the rate and temperatures.
 */
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include <xts/json.h>
#include <smt/smt.h>
#include <smt/smt_int.h>

static const char *const oc_values[] = { "idle", "l2_pressure", "mem_stream", NULL };
static const int oc_load[] = { XTS_SMT_NONE, XTS_SMT_L2_PRESSURE, XTS_SMT_MEM_STREAM };

static uint16_t temps0[4];
static int have_temps0, pirs_mask;
static uint64_t t_apply;

static int oc_applies(const char *fam, const char *kind)
{
  (void)fam;
  return kind && (!strcmp(kind, "timing") || !strcmp(kind, "sched") || !strcmp(kind, "mem") || !strcmp(kind, "smt"));
}

static int oc_apply(int v)
{
  xts_smt_init();
  memset(&xts_smt_rs, 0, sizeof xts_smt_rs);
  have_temps0 = xts_smc_temps(temps0) == 0;
  t_apply = xts_tb();
  pirs_mask = 0;
  int r = 0;
  for (int p = 2; p < XTS_SMT_NPIR && r == 0; p++) {
    if (xts_smt_reserved(p)) continue;
    if (xts_smt_state(p) == XTS_SMT_ST_RUNNING) r = xts_smt_stop(p);
    if (r == 0 && xts_smt_state(p) == XTS_SMT_ST_LOST) r = XTS_SMT_E_LOST;
  }
  if (r == 0 && v > 0) r = xts_smt_cores_start(oc_load[v], &pirs_mask);
  if (r < 0) {
    xts_log("other_cores=%s cannot be applied: %s", oc_values[v], xts_smt_err_name(r));
    return -1;
  }
  if (v > 0) {
    xts_smt_hook_cores = 1;
    if (xts_smt_hooks_install() < 0) {
      xts_smt_hook_cores = 0;
      for (int p = 2; p < XTS_SMT_NPIR; p++)
        if (pirs_mask & (1 << p)) xts_smt_stop(p);
      pirs_mask = 0;
      return -1;
    }
  }
  xts_env.other_cores = v;
  return 0;
}

static void oc_revert(int v)
{
  xts_smt_hook_cores = 0;
  xts_smt_hooks_remove();
  uint64_t ticks = xts_tb() - t_apply;
  char det[1400];
  xts_json_t j;
  xj_init(&j, det, sizeof det);
  xts_smt_info_begin(&j, "other_cores", oc_values[v]);
  xj_key(&j, "threads");
  xj_arr(&j);
  uint64_t total = 0;
  uint32_t ops = 0;
  int fails = 0;
  for (int p = 2; p < XTS_SMT_NPIR; p++) {
    if (!(pirs_mask & (1 << p))) continue;
    smtb_relax(p);
    uint32_t it = xts_smt_iters(p);
    ops = xts_smt_ops_per_iter(p);
    int r = xts_smt_stop(p);
    if (r < 0) fails++;
    total += it;
    xj_obj(&j);
    xj_kv_int(&j, "pir", p);
    xj_kv_uint(&j, "iters", it);
    xj_kv_str(&j, "restore", r == 0 ? "ok" : xts_smt_err_name(r));
    xj_kv_hex(&j, "trap_vector", xts_smt_tcbs[p].trap_vector, 4);
    xj_end(&j);
  }
  xj_end(&j);
  xj_kv_str(&j, "reserved", "PIR 0 test, 2 suite, 4 transmitter");
  xj_kv_uint(&j, "iters", total);
  xj_kv_uint(&j, "ops_per_iter", ops);
  xj_kv_uint(&j, "block_ticks", ticks);
  xj_kv_double(&j, "ops_per_tick", ticks ? (double)total * ops / (double)ticks : 0.0);
  xj_kv_uint(&j, "runs", xts_smt_rs.blk_runs);
  xj_kv_uint(&j, "run_iters", xts_smt_rs.blk_cores_iters);
  xj_kv_uint(&j, "run_ticks", xts_smt_rs.blk_ticks);
  xj_kv_uint(&j, "stalls", xts_smt_rs.stalls);
  if (have_temps0) {
    xj_key(&j, "temps_start");
    xj_obj(&j);
    xj_kv_double(&j, "cpu", temps0[0] / 256.0);
    xj_kv_double(&j, "gpu", temps0[1] / 256.0);
    xj_kv_double(&j, "edram", temps0[2] / 256.0);
    xj_kv_double(&j, "board", temps0[3] / 256.0);
    xj_end(&j);
  }
  xts_smt_temps_json(&j, "temps_end");
  xj_end(&j);
  xts_smt_emit_info("other_cores", (double)total, "count", xj_finish(&j));
  if (fails) xts_log("other_cores=%s revert: %d thread(s) did not stop", oc_values[v], fails);
  pirs_mask = 0;
  xts_env.other_cores = 0;
}

XTS_AXIS(other_cores, .values = oc_values, .apply = oc_apply, .revert = oc_revert, .applies_to = oc_applies, .order = 85,
         .desc = "load on the spare threads of cores 1 and 2 (PIR 3, 5): idle, L2 pressure, memory stream");
