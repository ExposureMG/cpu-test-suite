/* toolchain: everything the Xenon GCC cost model and scheduler (libxenon-toolchain, tools/gen_xenon_sched.py) are
 * calibrated from, five times in one boot. Each pass is a complete run (its own run directory on the PC, base seed
 * SEED + pass); the receiver waits for all five, writes session-<id>/session.json and merges the passes
 * (session-<id>/merged), so every figure comes with a repeat spread instead of a single run.
 *
 * Per pass (estimated from the Jasper runs of 2026-10-19/20: about 20 minutes, the session about 1 h 40 min):
 *   - the loop maps (frontend.loop: straight-line bodies; frontend.branch: an internal forward branch at every fetch-group
 *     phase of top, branch and target) and the timing / scheduling / memory families at the default condition, including
 *     ldst.*'s direct FP/VR load-to-use chains (luse.*, never run on hardware before) and issue.parity (does pairing
 *     depend on the word parity of a pair: the order asymmetries GCC's DFA cannot express);
 *   - tc.* (when built in): the GCC model's own scheduler kernels (tc.kernels) and the correctness probes of the
 *     replacement vector multiplies, estimates, -mrecip and vpkd3d128 (tc.probes);
 *   - the SMT inputs of the cost model, sampled with max_cases (the full smt profile takes 95 minutes): latency, pairing
 *     and throughput under every sibling load, throughput at low thread priority, and latency / memory under other-core
 *     load. Sibling and priority rows skip the default value (the default-condition rows above are the baseline).
 * Families that do not exist in a build (tc.* without the toolchain) simply match nothing. */
#include <xts/profile.h>

#define SIB "sibling=parked,busy_int,busy_fp,busy_vmx,busy_mem,same_insn"

XTS_PROFILE_PASSES(toolchain, 5, "GCC model calibration: timing, sched, mem, loop map, parity, tc.*, sampled SMT (x5)",
  { .families = "self.*" },
  { .families = "calib*" },
  { .families = "frontend.*" },
  /* timing */
  { .families = "lat.*" },
  { .families = "tput.*" },
  /* scheduling */
  { .families = "bypass.*" },
  { .families = "issue.*" },
  { .families = "unit.*" },
  { .families = "ucode*" },
  { .families = "ldst.*" },
  { .families = "ldst.*", .axes = "cache=*", .sweep = 1 },
  { .families = "ldst.*", .axes = "data_align=*", .sweep = 1 },
  { .families = "branch*" },
  { .families = "branch*", .axes = "code_align=4,8,16,32,64", .sweep = 1 },
  { .families = "sysops*" },
  /* memory hierarchy */
  { .families = "mem.*" },
  /* the toolchain's own kernels and probes */
  { .families = "tc.*" },
  /* SMT (sampled) */
  { .families = "lat.*",   .axes = SIB, .sweep = 1, .max_cases = 250 },
  { .families = "issue.*", .axes = SIB, .sweep = 1, .max_cases = 400 },
  { .families = "tput.*",  .axes = SIB, .sweep = 1, .max_cases = 80 },
  { .families = "tput.*",  .axes = "thread_prio=low", .sweep = 1, .max_cases = 20 },
  { .families = "lat.*",   .axes = "other_cores=l2_pressure,mem_stream", .sweep = 1, .max_cases = 250 },
  { .families = "mem.*",   .axes = "other_cores=l2_pressure,mem_stream", .sweep = 1 },
);
