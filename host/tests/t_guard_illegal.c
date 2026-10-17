/* memory guard in the exec families: exec.illegal's memory-form words (update-form indexed with RA = 0, RA = RT, the hardware-
 * illegal dcba) are expected to trap before touching memory, but if a core revision executed one the access must land in scratch.
 * The old input had GPR[0] = the 0xC0DE... canary (EA = r0 + RB, wild) and RB could be r0 itself; now r0 = 0 and RB is never r0.
 * Also: every memory case of the exec.* families (the ones that run through xts_semantic_check / xe_check_kernel) keeps its
 * EA inside scratch, so the guard never skips a legitimate case. */
#include <string.h>
#include <xts/xts.h>
#include <exec/exec_priv.h>
#include <core/core.h>
#include "htest.h"

static xts_exec_t x;
static struct { int n, guarded, fail, other; char first[400]; } cap;
static void tap(const char *family, const char *case_id, const xts_record_t *r, const char *json)
{
  (void)family; (void)case_id;
  cap.n++;
  if (strstr(json, "EA outside test memory")) {
    if (!cap.guarded++) snprintf(cap.first, sizeof cap.first, "%s", json);
  }
  if (r->status == XTS_ST_FAIL) cap.fail++;
}

static int run_family(const char *name, int memory_only, int stride)
{
  const xts_family_t *f = xts_family_find(name);
  static xts_caselist_t cl;
  memset(&cl, 0, sizeof cl);
  if (!f || f->enumerate(&cl) < 0) return -1;
  xts_ctx_t ctx;
  int ran = 0;
  for (int i = 0; i < cl.n; i += stride) {
    const xts_insn_t *d = cl.v[i].insn >= 0 ? xts_insn(cl.v[i].insn) : NULL;
    if (memory_only && (!d || d->unit != XTS_UNIT_MEMORY)) continue;
    memset(&ctx, 0, sizeof ctx);
    ctx.fam = f;
    ctx.c = &cl.v[i];
    ctx.case_index = i;
    ctx.host = 1;
    ctx.block_key = "r0:guard.scan:-:c0";
    ctx.seed = 0x9E3779B97F4A7C15ull * (uint64_t)(i + 1);
    xts_rng_seed(&ctx.rng, ctx.seed);
    f->run(&cl.v[i], &ctx);
    ran++;
  }
  return ran;
}

HTEST(guard_illegal_inputs_land_in_scratch)
{
  memset(&cap, 0, sizeof cap);
  xts_sink_tap = tap;
  const xts_family_t *f = xts_family_find("exec_illegal");
  HT_CHECK(f != NULL);
  int ran = run_family("exec_illegal", 0, 1);
  xts_sink_tap = NULL;
  xts_sink_discard();
  HT_CHECK(ran > 250);                       /* hw 29 x 4, ra0 14 x 4, rart 11 x 4, stcx 12, mulh 48 ... */
  printf("    exec.illegal: %d cases, none guarded\n", ran);
  HT_EQ_U64(cap.n, ran);                     /* exactly one record per case */
  if (cap.guarded) printf("      guarded: %s\n", cap.first);
  HT_EQ_U64(cap.guarded, 0);                 /* no case needed the guard: all inputs are in scratch */
  HT_EQ_U64(cap.fail, 0);
}

HTEST(guard_illegal_ra0_canary_would_be_wild)
{
  /* the old input: update-form indexed, RA = 0, RB = scratch pointer, r0 = canary. Read as GPR[0] + RB it is wild, so the
   * guard (which checks both readings of an RA = 0 field in a non-RA|0 form) blocks it; with r0 = 0 both readings are scratch. */
  static const int ra0_ops[] = { XI_lbzux, XI_lhzux, XI_lhaux, XI_lwzux, XI_lwaux, XI_ldux, XI_stbux, XI_sthux, XI_stwux,
                                 XI_stdux, XI_lfsux, XI_lfdux, XI_stfsux, XI_stfdux };
  char why[256];
  for (unsigned i = 0; i < sizeof ra0_ops / sizeof *ra0_ops; i++) {
    uint32_t w = xts_enc(ra0_ops[i], 0, 5, 0, 6);          /* op rT, 0 (RA field), r6 */
    HT_CHECK(w != 0);
    xts_case_init(&x);
    xts_set_sf(&x, 1);
    xts_set_ptr(&x, 6, 128 + 16);
    HT_CHECK(x.in.gpr[0] != 0);                            /* the canary */
    HT_CHECK(xts_exec_mem_guard(xts_decode1(w), w, &x.in, XTS_M_ILLEGAL, 0, why, sizeof why) && strstr(why, "EA outside test memory"));
    xts_set_gpr(&x, 0, 0);
    HT_CHECK(!xts_exec_mem_guard(xts_decode1(w), w, &x.in, XTS_M_ILLEGAL, 0, why, sizeof why));
    /* RB = r0 itself (r0 = pointer): r0 + RB = 2 * pointer, wild */
    xts_set_ptr(&x, 0, 128);
    uint32_t w0 = xts_enc(ra0_ops[i], 0, 5, 0, 0);
    HT_CHECK(xts_exec_mem_guard(xts_decode1(w0), w0, &x.in, XTS_M_ILLEGAL, 0, why, sizeof why));
  }
}

HTEST(guard_legit_memory_cases_unchanged)
{
  /* every memory case (stride-sampled to keep the run short) of the exec.* families: none is skipped by the guard, none fails */
  static const char *const fams[] = { "exec_ppc", "exec_altivec", "exec_vmx128", "exec_fpu" };
  for (unsigned k = 0; k < sizeof fams / sizeof *fams; k++) {
    memset(&cap, 0, sizeof cap);
    xts_sink_tap = tap;
    int ran = run_family(fams[k], 1, 1);
    xts_sink_tap = NULL;
    xts_sink_discard();
    if (cap.guarded) printf("      %s guarded: %s\n", fams[k], cap.first);
    printf("    %s: %d memory cases, none guarded\n", fams[k], ran);
    HT_CHECK(ran >= 0);
    HT_EQ_U64(cap.guarded, 0);
    HT_EQ_U64(cap.fail, 0);
  }
}
