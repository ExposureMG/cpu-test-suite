/* sibling - what the other hardware thread of the test core (core 0: PIR 1) does while a block runs. The test executor
 * is PIR 0 (xts/xcore.h); this code runs on the suite core, and CTRL[TE1] is read/written on core 0 (src/smt).
 *
 *   idle       libxenon's wait loop (`or 1,1,1` + polling its slot): the boot state, verified (slot free, CTRL[TE1] = 1)
 *   parked     xenon_sleep_thread(1), verified by CTRL[TE1] = 0 (single-thread mode for core 0); woken at revert
 *   busy_int   12 independent add chains        busy_fp   12 fmadd chains (1.0)     busy_vmx  12 vmaddfp chains (1.0f)
 *   busy_mem   random pointer chase over a private 4 MB buffer (memory/L2 traffic, never the test's buffer)
 *   same_insn  the case's own words: before each run the exec hook reads x->run_word / x->run_kernel (first 512 words,
 *              exit placeholders dropped), keeps the words that are safe on another thread (user, executes, int/FP/vector, no
 *              memory/branch/SPR effects) and loops them on the sibling; when none is safe a unit-matched busy kernel runs
 *              (aux_status FALLBACK). The sibling is restarted only when the word set changes.
 * Every start/stop/park/wake is bounded; a failure makes apply() return <0 (block skipped and recorded) and the exec hook
 * cancels a run whose sibling heartbeat stopped. Per run the sibling's iterations are in xts_smt_rs (src/smt/smt.h) and
 * x->aux_status; per block an info record (case "smt:sibling", metric "info") carries iterations, rate, temperatures and failures.
 */
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include <xts/json.h>
#include <smt/smt.h>
#include <smt/smt_int.h>

static const char *const sib_values[] = { "idle", "parked", "busy_int", "busy_fp", "busy_vmx", "busy_mem", "same_insn", NULL };
static const int sib_load[] = { XTS_SMT_NONE, XTS_SMT_NONE, XTS_SMT_BUSY_INT, XTS_SMT_BUSY_FP, XTS_SMT_BUSY_VMX,
                                XTS_SMT_BUSY_MEM, XTS_SMT_SAME_INSN };

static char apply_why[96];
static uint16_t temps0[4];
static int have_temps0, trap_range_set;
static uint64_t t_apply;

static int sib_applies(const char *fam, const char *kind)
{
  (void)fam;
  return kind && (!strcmp(kind, "timing") || !strcmp(kind, "sched") || !strcmp(kind, "mem") || !strcmp(kind, "smt"));
}

static int sib_apply(int v)
{
  xts_smt_init();
  memset(&xts_smt_rs, 0, sizeof xts_smt_rs);
  apply_why[0] = 0;
  have_temps0 = xts_smc_temps(temps0) == 0;
  t_apply = xts_tb();
  int r = 0;
  /* every mode starts from the verified idle state */
  if (xts_smt_state(1) == XTS_SMT_ST_PARKED) r = xts_smt_wake();
  if (r == 0 && xts_smt_state(1) == XTS_SMT_ST_RUNNING) r = xts_smt_stop(1);
  if (r == 0 && xts_smt_state(1) == XTS_SMT_ST_LOST) r = XTS_SMT_E_LOST;
  if (r == 0) {
    if (v == 0) {
      if (!smtb_thread_alive(1) || smtb_slot_busy(1) || smtb_te1() == 0) r = XTS_SMT_E_STATE;
    } else if (v == 1) {
      r = xts_smt_park();
      if (r == 0) xts_smt_hook_sibling_mode = -1;
    } else if (sib_load[v] == XTS_SMT_SAME_INSN) {
      uint64_t lo, hi;
      smtb_code_range(1, &lo, &hi);
      trap_range_set = xts_exec_trap_range(0, lo, hi) == 0;   /* trap-range index 0 is the sibling's; an interrupt in its code is "inside" */
      xts_smt_hook_sibling_mode = XTS_SMT_SAME_INSN;          /* started per run by the exec hook */
    } else {
      r = xts_smt_start(1, sib_load[v], NULL, 0);
      if (r == 0) xts_smt_hook_sibling_mode = sib_load[v];
    }
  }
  if (r < 0) {
    snprintf(apply_why, sizeof apply_why, "%s", xts_smt_err_name(r));
    xts_log("sibling=%s cannot be applied: %s (sibling state %s)", sib_values[v], apply_why,
            xts_smt_state_name(xts_smt_state(1)));
    xts_smt_hook_sibling_mode = 0;
    if (trap_range_set) { xts_exec_trap_range(0, 0, 0); trap_range_set = 0; }
    return -1;
  }
  if (xts_smt_hook_sibling_mode && xts_smt_hooks_install() < 0) {
    int mode = xts_smt_hook_sibling_mode;
    xts_smt_hook_sibling_mode = 0;
    if (mode == -1) xts_smt_wake();
    else if (xts_smt_state(1) == XTS_SMT_ST_RUNNING) xts_smt_stop(1);
    if (trap_range_set) { xts_exec_trap_range(0, 0, 0); trap_range_set = 0; }
    return -1;
  }
  xts_env.sibling = v;
  return 0;
}

static void sib_revert(int v)
{
  xts_smt_hook_sibling_mode = 0;
  xts_smt_hooks_remove();
  smtb_relax(1);
  int st_before = xts_smt_state(1);
  uint32_t iters = st_before == XTS_SMT_ST_RUNNING ? xts_smt_iters(1) : 0;
  uint32_t ops = st_before == XTS_SMT_ST_RUNNING ? xts_smt_ops_per_iter(1) : 0;
  int load = st_before == XTS_SMT_ST_RUNNING ? (int)xts_smt_tcbs[1].load : XTS_SMT_NONE;
  uint32_t trapv = 0, traps = 0;
  int r = 0;
  if (st_before == XTS_SMT_ST_RUNNING) {
    r = xts_smt_stop(1);
    trapv = xts_smt_tcbs[1].trap_vector;
    traps = xts_smt_tcbs[1].trap_srr0;
  } else if (st_before == XTS_SMT_ST_PARKED) {
    r = xts_smt_wake();
  }
  if (trap_range_set) { xts_exec_trap_range(0, 0, 0); trap_range_set = 0; }
  uint64_t ticks = xts_tb() - t_apply;
  char det[1400];
  xts_json_t j;
  xj_init(&j, det, sizeof det);
  xts_smt_info_begin(&j, "sibling", sib_values[v]);
  xj_kv_int(&j, "pir", 1);
  xj_kv_str(&j, "state_at_revert", xts_smt_state_name(st_before));
  xj_kv_str(&j, "load", xts_smt_load_name(load));
  xj_kv_str(&j, "restore", r == 0 ? "ok" : xts_smt_err_name(r));
  xj_kv_str(&j, "state_after", xts_smt_state_name(xts_smt_state(1)));
  xj_kv_int(&j, "te1_after", smtb_te1());
  xj_kv_uint(&j, "iters", iters);
  xj_kv_uint(&j, "ops_per_iter", ops);
  xj_kv_uint(&j, "block_ticks", ticks);
  xj_kv_double(&j, "sibling_ops_per_tick", ticks ? (double)iters * ops / (double)ticks : 0.0);
  xj_kv_uint(&j, "runs", xts_smt_rs.blk_runs);
  xj_kv_uint(&j, "run_iters", xts_smt_rs.blk_sib_iters);
  xj_kv_uint(&j, "run_ticks", xts_smt_rs.blk_ticks);
  xj_kv_double(&j, "run_ops_per_tick", xts_smt_rs.blk_ticks ? (double)xts_smt_rs.blk_sib_iters * ops / (double)xts_smt_rs.blk_ticks : 0.0);
  xj_kv_uint(&j, "restarts", xts_smt_rs.restarts);
  xj_kv_uint(&j, "fallbacks", xts_smt_rs.fallbacks);
  xj_kv_uint(&j, "stalls", xts_smt_rs.stalls);
  xj_kv_uint(&j, "refused_runs", xts_smt_rs.refused);
  xj_kv_hex(&j, "trap_vector", trapv, 4);
  xj_kv_hex(&j, "trap_srr0", traps, 8);
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
  xts_smt_emit_info("sibling", (double)iters, "count", xj_finish(&j));
  if (r < 0) xts_log("sibling=%s revert: %s (sibling now %s)", sib_values[v], xts_smt_err_name(r), xts_smt_state_name(xts_smt_state(1)));
  xts_env.sibling = 0;
}

XTS_AXIS(sibling, .values = sib_values, .apply = sib_apply, .revert = sib_revert, .applies_to = sib_applies, .order = 86,
         .desc = "sibling hardware thread (PIR 1): idle loop, parked, or a load kernel (int/fp/vmx/mem/the case's own words)");
