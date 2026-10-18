/* src/smt + sibling/other_cores/speed axes on the host simulation: state machine, timeouts -> lost, park/wake, load
 * kernels decode back, same_insn word selection, exec hooks. */
#include <string.h>
#include <xts/xts.h>
#include <smt/smt.h>
#include <smt/smt_int.h>
#include <core/core.h>        /* xts_dry_stats */
#include <exec/exec_priv.h>     /* xts_exec_hooks_run_pre/post: what the exec engine runs around a case */
#include "htest.h"

void xts_smt_test_reset(void);
extern int xts_smt_sim_dead[XTS_SMT_NPIR];
extern int xts_smt_sim_cores_fail;

HTEST(smt_loads_start_stop)
{
  xts_smt_test_reset();
  uint64_t bad0 = xts_dry_stats.bad_words, k0 = xts_dry_stats.kernels;
  static const int loads[] = { XTS_SMT_BUSY_INT, XTS_SMT_BUSY_FP, XTS_SMT_BUSY_VMX, XTS_SMT_BUSY_MEM, XTS_SMT_BUSY_LD };
  for (unsigned i = 0; i < sizeof loads / sizeof loads[0]; i++) {
    HT_EQ_U64(xts_smt_start(1, loads[i], NULL, 0), 0);
    HT_EQ_U64(xts_smt_state(1), XTS_SMT_ST_RUNNING);
    HT_CHECK(xts_smt_ops_per_iter(1) > 0);
    HT_EQ_U64(xts_smt_start(1, loads[i], NULL, 0), (uint64_t)(int64_t)XTS_SMT_E_STATE);   /* already running */
    for (int k = 0; k < 5; k++) smtb_relax(1);
    HT_CHECK(xts_smt_iters(1) > 0);
    HT_EQ_U64(xts_smt_stop(1), 0);
    HT_EQ_U64(xts_smt_state(1), XTS_SMT_ST_IDLE);
    HT_CHECK(!smtb_slot_busy(1));
  }
  uint32_t w[2] = { xts_enc(XI_addx, 0, 3, 4, 5), xts_enc(XI_faddx, 0, 1, 2, 3) };
  HT_EQ_U64(xts_smt_start(1, XTS_SMT_SAME_INSN, w, 2), 0);
  HT_EQ_U64(xts_smt_ops_per_iter(1), 64 * 32);
  HT_EQ_U64(xts_smt_stop(1), 0);
  /* other cores */
  int mask = 0;
  HT_EQ_U64(xts_smt_cores_start(XTS_SMT_L2_PRESSURE, &mask), 0);
  HT_EQ_U64(mask, 0x28);                                 /* the spare threads of cores 1 and 2: PIR 3 and 5 */
  for (int p = 1; p < 6; p++) if (mask & (1 << p)) HT_EQ_U64(xts_smt_stop(p), 0);
  HT_EQ_U64(xts_smt_cores_start(XTS_SMT_MEM_STREAM, &mask), 0);
  for (int p = 1; p < 6; p++) if (mask & (1 << p)) HT_EQ_U64(xts_smt_stop(p), 0);
  HT_CHECK(xts_dry_stats.kernels >= k0 + 8);
  HT_EQ_U64(xts_dry_stats.bad_words, bad0);              /* every load kernel decodes back */
  HT_CHECK(xts_smt_all_idle());
}

HTEST(smt_park_wake)
{
  xts_smt_test_reset();
  HT_EQ_U64(smtb_te1(), 1);
  HT_EQ_U64(xts_smt_park(), 0);
  HT_EQ_U64(xts_smt_state(1), XTS_SMT_ST_PARKED);
  HT_EQ_U64(smtb_te1(), 0);
  HT_CHECK(!xts_smt_all_idle());
  HT_EQ_U64(xts_smt_start(1, XTS_SMT_BUSY_INT, NULL, 0), (uint64_t)(int64_t)XTS_SMT_E_STATE);
  HT_EQ_U64(xts_smt_wake(), 0);
  HT_EQ_U64(smtb_te1(), 1);
  HT_EQ_U64(xts_smt_state(1), XTS_SMT_ST_IDLE);
  HT_CHECK(xts_smt_all_idle());
}

HTEST(smt_timeouts_mark_lost)
{
  xts_smt_test_reset();
  xts_smt_sim_dead[3] = 1;                                 /* never answers */
  uint64_t t0 = xts_tb();
  HT_EQ_U64(xts_smt_start(3, XTS_SMT_BUSY_INT, NULL, 0), (uint64_t)(int64_t)XTS_SMT_E_START_TO);
  HT_CHECK(xts_tb() - t0 < 2 * xts_tb_hz());             /* bounded */
  HT_EQ_U64(xts_smt_state(3), XTS_SMT_ST_LOST);
  HT_EQ_U64(xts_smt_start(3, XTS_SMT_BUSY_INT, NULL, 0), (uint64_t)(int64_t)XTS_SMT_E_LOST);
  /* a thread that stops answering while running */
  HT_EQ_U64(xts_smt_start(1, XTS_SMT_BUSY_FP, NULL, 0), 0);
  xts_smt_sim_dead[1] = 1;
  HT_EQ_U64(xts_smt_stop(1), (uint64_t)(int64_t)XTS_SMT_E_STOP_TO);
  HT_EQ_U64(xts_smt_state(1), XTS_SMT_ST_LOST);
  /* the axis refuses the block instead of hanging */
  const xts_axis_t *a = xts_axis_find("sibling");
  HT_CHECK(a != NULL);
  if (a) HT_CHECK(a->apply(xts_axis_value_index(a, "busy_int")) < 0);
  /* cores that never come up */
  xts_smt_test_reset();
  xts_smt_sim_cores_fail = 1;
  HT_EQ_U64(xts_smt_cores_up(), (uint64_t)(int64_t)XTS_SMT_E_CORES);
  const xts_axis_t *oc = xts_axis_find("other_cores");
  HT_CHECK(oc && oc->apply(1) < 0);
  HT_CHECK(oc && oc->apply(0) == 0);                       /* idle needs nothing */
  if (oc) oc->revert(0);
  xts_smt_test_reset();
}

HTEST(smt_same_words)
{
  uint32_t in[8], out[8];
  int fb = -1, n = 0;
  in[n++] = xts_enc(XI_addx, 0, 3, 4, 5);                  /* kept */
  in[n++] = xts_enc(XI_lwz, 0, 3, 0, 5);                   /* memory: dropped */
  in[n++] = xts_enc(XI_orx, 0, 1, 1, 1);                   /* or 1,1,1 priority hint: dropped */
  in[n++] = xts_enc(XI_mtspr, 0, 9, 3);                    /* mtctr: dropped */
  in[n++] = xts_enc(XI_faddx, 0, 1, 2, 3);                 /* kept */
  in[n++] = xts_enc(XI_bcx, 0, 16, 0, 8);                  /* branch: dropped */
  int k = xts_smt_same_words(in, n, out, 8, &fb);
  HT_EQ_U64(k, 2);
  HT_EQ_U64(out[0], in[0]);
  HT_EQ_U64(out[1], in[4]);
  uint32_t ld[3] = { xts_enc(XI_lwz, 0, 3, 0, 5), xts_enc(XI_stw, 0, 3, 8, 5), xts_enc(XI_lwz, 0, 4, 4, 5) };
  HT_EQ_U64(xts_smt_same_words(ld, 3, out, 8, &fb), 0);
  HT_EQ_U64(fb, XTS_SMT_BUSY_LD);
  uint32_t vv[2] = { xts_enc(XI_vaddfp, 0, 1, 2, 3), xts_enc(XI_vmaddfp, 0, 1, 2, 3, 4) };
  HT_EQ_U64(xts_smt_same_words(vv, 2, out, 8, &fb), 2);
  HT_EQ_U64(fb, XTS_SMT_BUSY_VMX);
}

HTEST(smt_axes_hooks)
{
  xts_registry_init(NULL);
  xts_smt_test_reset();
  const xts_axis_t *s = xts_axis_find("sibling"), *oc = xts_axis_find("other_cores"), *sp = xts_axis_find("speed");
  HT_CHECK(s && oc && sp);
  if (!s || !oc || !sp) return;
  HT_EQ_U64(xts_axis_nvalues(s), 7);
  HT_EQ_U64(xts_axis_nvalues(oc), 3);
  HT_CHECK(s->applies_to("lat.add", "timing") && !s->applies_to("exec.ppc", "semantic"));
  HT_CHECK(oc->applies_to("mem.chase", "mem") && !oc->applies_to("self.exec", "self"));
  static xts_exec_t x;
  for (int v = 0; v < xts_axis_nvalues(s); v++) {
    HT_EQ_U64(s->apply(v), 0);
    HT_EQ_U64(oc->apply(1), 0);
    HT_EQ_U64(xts_exec_hook_count(), 1);                  /* one chained hook, the v1 slot stays free */
    HT_CHECK(xts_exec_hooks.pre == NULL);
    xts_case_init(&x);
    x.err[0] = 0;
    HT_CHECK(xts_exec_hooks_run_pre(&x) == 0);
    HT_CHECK(x.aux_status & XTS_SMT_AUX_CORES);
    if (v >= 2) HT_CHECK(x.aux_status & XTS_SMT_AUX_SIBLING);
    if (v == 1) HT_CHECK(x.aux_status & XTS_SMT_AUX_PARKED);
    x.status = XTS_EXEC_SIMULATED;                        /* the engine has run the case */
    xts_exec_hooks_run_post(&x);
    HT_EQ_U64(xts_smt_rs.blk_runs, 1);
    oc->revert(1);
    s->revert(v);
    HT_EQ_U64(xts_exec_hook_count(), 0);
    HT_CHECK(xts_smt_all_idle());
    HT_EQ_U64(smtb_te1(), 1);
  }
  /* speed: read only - the single value "default" applies, nothing else; the code is never changed */
  int code = xts_smt_speed_get();
  HT_EQ_U64(sp->apply(0), 0);
  sp->revert(0);
  HT_CHECK(sp->apply(1) < 0);
  HT_EQ_U64(xts_smt_speed_get(), code);
  xts_smt_test_reset();
}

HTEST(smt_same_insn_hook)
{
  xts_registry_init(NULL);
  xts_smt_test_reset();
  const xts_axis_t *s = xts_axis_find("sibling");
  HT_CHECK(s != NULL);
  if (!s) return;
  uint64_t bad0 = xts_dry_stats.bad_words;
  HT_EQ_U64(s->apply(xts_axis_value_index(s, "same_insn")), 0);
  static xts_exec_t x;
  xts_case_init(&x);
  /* single word: started with that word */
  x.run_word = xts_enc(XI_addx, 0, 3, 4, 5);
  x.run_kernel = NULL;
  HT_EQ_U64(xts_exec_hooks_run_pre(&x), 0);
  HT_CHECK(x.aux_status & XTS_SMT_AUX_RESTARTED);
  HT_CHECK(x.aux_status & XTS_SMT_AUX_SIBLING);
  HT_CHECK(!(x.aux_status & XTS_SMT_AUX_FALLBACK));
  HT_EQ_U64(xts_smt_tcbs[1].load, XTS_SMT_SAME_INSN);
  x.status = XTS_EXEC_SIMULATED;
  xts_exec_hooks_run_post(&x);
  /* same word again: no restart */
  HT_EQ_U64(xts_exec_hooks_run_pre(&x), 0);
  HT_CHECK(!(x.aux_status & XTS_SMT_AUX_RESTARTED));
  x.status = XTS_EXEC_SIMULATED;
  xts_exec_hooks_run_post(&x);
  /* a load word: unit-matched fallback */
  x.run_word = xts_enc(XI_lwz, 0, 3, 0, 5);
  HT_EQ_U64(xts_exec_hooks_run_pre(&x), 0);
  HT_CHECK(x.aux_status & XTS_SMT_AUX_FALLBACK);
  HT_EQ_U64(xts_smt_tcbs[1].load, XTS_SMT_BUSY_LD);
  x.status = XTS_EXEC_SIMULATED;
  xts_exec_hooks_run_post(&x);
  /* a kernel: safe words kept, exit placeholder / mtctr / bdnz dropped */
  uint32_t kw[6] = { xts_enc(XI_addi, 0, 9, 0, 100), xts_enc(XI_mtspr, 0, 9, 9), xts_enc(XI_fmaddx, 0, 1, 2, 3, 4),
                     xts_enc(XI_fmaddx, 0, 5, 6, 7, 8), xts_enc(XI_bcx, 0, 16, 0, -8), 0x48000000u };
  x.run_word = 0;
  x.run_kernel = kw;
  x.run_kernel_words = 6;
  HT_EQ_U64(xts_exec_hooks_run_pre(&x), 0);
  HT_CHECK(x.aux_status & XTS_SMT_AUX_RESTARTED);
  HT_EQ_U64(xts_smt_tcbs[1].load, XTS_SMT_SAME_INSN);
  HT_EQ_U64(xts_smt_ops_per_iter(1), 64 * 33);              /* 3 kept words repeated to >= 32 per body */
  x.status = XTS_EXEC_SIMULATED;
  xts_exec_hooks_run_post(&x);
  HT_EQ_U64(xts_smt_rs.restarts, 3);
  HT_EQ_U64(xts_smt_rs.fallbacks, 1);
  s->revert(xts_axis_value_index(s, "same_insn"));
  HT_EQ_U64(xts_exec_hook_count(), 0);
  HT_CHECK(xts_smt_all_idle());
  HT_EQ_U64(xts_dry_stats.bad_words, bad0);
  xts_smt_test_reset();
}

/* the SMT hook is one entry of the chain: a hook added after it can refuse the run; the SMT post then sees NOTRUN and counts nothing */
static int refuse_pre(xts_exec_t *x, void *ud) { (void)x; (void)ud; return -1; }

HTEST(smt_hook_chain)
{
  xts_registry_init(NULL);
  xts_smt_test_reset();
  const xts_axis_t *oc = xts_axis_find("other_cores");
  HT_CHECK(oc != NULL);
  if (!oc) return;
  HT_EQ_U64(oc->apply(1), 0);
  int h = xts_exec_hook_add(refuse_pre, NULL, NULL);
  HT_CHECK(h >= 0);
  static xts_exec_t x;
  xts_case_init(&x);
  HT_CHECK(xts_exec_hooks_run_pre(&x) < 0);
  HT_EQ_U64(x.status, (uint64_t)(int64_t)XTS_EXEC_NOTRUN);
  HT_EQ_U64(xts_smt_rs.blk_runs, 0);
  xts_exec_hook_remove(h);
  HT_EQ_U64(xts_exec_hooks_run_pre(&x), 0);
  x.status = XTS_EXEC_SIMULATED;
  xts_exec_hooks_run_post(&x);
  HT_EQ_U64(xts_smt_rs.blk_runs, 1);
  oc->revert(1);
  HT_EQ_U64(xts_exec_hook_count(), 0);
  xts_smt_test_reset();
}
