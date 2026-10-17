/* vmx128.regmap - self-checking proof of the VMX128 register-field assembly on hardware (port of the old vrmap plan, extended).
 *   vor128      vD = vA | vB with each of VD128, VA128, VB128 swept over 0..127 and the other two fixed (distinct source
 *               patterns); plus the high-register combinations (127,96,64) (65,127,33) (100,37,90) (36,100,127).
 *               Expected: register D receives A|B and no other register changes. A wrong bit position puts the result in the
 *               wrong register or reads the wrong source.
 *   vperm128    VC128 (the 3-bit control-register field, v0..v7) swept over 0..7 for three high-register triples; the control
 *               is a byte permutation of A:B, so a wrong VC128 gives a different permutation.
 *   vpermwi128  all 256 VPERM128 values (the 8-bit field is split over two instruction positions), registers varied.
 * No reference model needed: the expected result is computed here. On the host the model (when it has the handler) simulates
 * the instruction, the same oracle then checks the model's own result. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include "exec_common.h"

enum { R_VD = 0, R_VA, R_VB, R_COMBO, R_VC, R_PERMWI };
static const int combos[4][3] = { { 127, 96, 64 }, { 65, 127, 33 }, { 100, 37, 90 }, { 36, 100, 127 } };
static const int vc_triples[3][3] = { { 100, 33, 77 }, { 3, 127, 64 }, { 127, 8, 31 } };

static int rm_enumerate(xts_caselist_t *cl)
{
  static const char *const tag[] = { "vd", "va", "vb" };
  int ivor = xts_isa_find("vor128"), iperm = xts_isa_find("vperm128"), ipwi = xts_isa_find("vpermwi128");
  if (ivor < 0 || iperm < 0 || ipwi < 0) return -1;
  xts_case_t *c;
  for (int f = 0; f < 3; f++)
    for (int n = 0; n < 128; n++) {
      if (!(c = xts_case_addf(cl, "vor128.%s#%03d", tag[f], n))) return -1;
      c->insn = ivor; c->arg[0] = f == 0 ? R_VD : f == 1 ? R_VA : R_VB; c->arg[1] = n;
    }
  for (int i = 0; i < 4; i++) {
    if (!(c = xts_case_addf(cl, "vor128.combo#%d", i))) return -1;
    c->insn = ivor; c->arg[0] = R_COMBO; c->arg[1] = i;
  }
  for (int t = 0; t < 3; t++)
    for (int vc = 0; vc < 8; vc++) {
      if (!(c = xts_case_addf(cl, "vperm128.vc%d.t%d", vc, t))) return -1;
      c->insn = iperm; c->arg[0] = R_VC; c->arg[1] = vc; c->arg[2] = t;
    }
  for (int p = 0; p < 256; p++) {
    if (!(c = xts_case_addf(cl, "vpermwi128.p%03d", p))) return -1;
    c->insn = ipwi; c->arg[0] = R_PERMWI; c->arg[1] = p;
  }
  return 0;
}

static int rm_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  static xts_exec_t x;
  static xts_state_t exp;
  static xts_state_diff_t diffs[16];
  char det[1536];
  xts_rng_t *r = &ctx->rng;
  int kind = (int)c->arg[0], n = (int)c->arg[1];
  int d = 0, a = 0, b = 0, vc = 0;
  uint32_t w = 0;
  xts_vr_t va, vb, vcv, res;
  xts_case_init(&x);
  if (kind == R_VD || kind == R_VA || kind == R_VB || kind == R_COMBO) {
    d = 7; a = 5; b = 6;
    if (kind == R_VD) d = n;
    else if (kind == R_VA) { a = n; if (a == b) b = 9; }
    else if (kind == R_VB) { b = n; if (b == a) a = 9; }
    else { d = combos[n][0]; a = combos[n][1]; b = combos[n][2]; }
    va = xts_vr4(0x11110000, 0x22220000, 0x33330000, 0x44440000);
    vb = xts_vr4(0x00001111, 0x00002222, 0x00003333, 0x00004444);
    xts_set_vr(&x, a, va);
    xts_set_vr(&x, b, vb);
    w = xts_enc(XI_vor128, 0, d, a, b);
    for (int i = 0; i < 4; i++) res.w[i] = va.w[i] | vb.w[i];
  } else if (kind == R_VC) {
    int t = (int)c->arg[2];
    d = vc_triples[t][0]; a = vc_triples[t][1]; b = vc_triples[t][2]; vc = n;
    uint8_t ab[32], cb[16], out[16];
    for (int i = 0; i < 32; i++) ab[i] = (uint8_t)(i < 16 ? i : 0x40 + i);
    for (int i = 0; i < 16; i++) cb[i] = (uint8_t)(((i * 5 + 3 + 7 * t) & 0x1F) | (xts_rng_below(r, 2) ? 0x20 : 0));   /* upper bits are ignored */
    for (int i = 0; i < 16; i++) out[i] = ab[cb[i] & 0x1F];
    va = xts_vr_load(ab);
    vb = xts_vr_load(ab + 16);
    vcv = xts_vr_load(cb);
    res = xts_vr_load(out);
    xts_set_vr(&x, a, va);
    xts_set_vr(&x, b, vb);
    xts_set_vr(&x, vc, vcv);
    w = xts_enc(XI_vperm128, 0, d, a, b, vc);
  } else {
    do { d = (int)xts_rng_below(r, 128); b = (int)xts_rng_below(r, 128); } while (d == b);
    va = xts_vr4(0x01020304, 0x05060708, 0x090A0B0C, 0x0D0E0F10);
    vb = va;
    xts_set_vr(&x, b, vb);
    for (int i = 0; i < 4; i++) res.w[i] = vb.w[(n >> (6 - 2 * i)) & 3];
    w = xts_enc(XI_vpermwi128, 0, d, b, n);
  }
  if (!w) { xe_emit_why(ctx, XTS_ST_FAIL, "", 0, 0, "encoder rejected the case"); return 0; }
  int st = xts_exec_word(&x, w);
  xts_json_t j;
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  int status;
  if (st == XTS_EXEC_DRYRUN) {
    status = XTS_ST_SKIP;
    xj_kv_str(&j, "why", "host dry run: not executed (model has no handler)");
  } else if (st < 0) {
    status = XTS_ST_FAIL;
    xj_kv_str(&j, "exec_error", x.err);
  } else {
    if (st == XTS_EXEC_SIMULATED) xj_kv_str(&j, "backend", "host-model");
    xe_expect_base(&x, &exp);
    exp.vr[d] = res;
    int nd = xts_state_compare(&exp, &x.out, diffs, 16) + xts_mem_compare(x.mem_in, x.mem_out, XTS_SCRATCH_BYTES, NULL, 0);
    int bad = nd || x.trap.trapped;
    status = bad ? XTS_ST_FAIL : XTS_ST_PASS;
    if (bad) {
      xj_kv_int(&j, "vd", d);
      xj_kv_int(&j, "va", a);
      xj_kv_int(&j, "vb", b);
      if (kind == R_VC) xj_kv_int(&j, "vc", vc);
      if (kind == R_PERMWI) xj_kv_int(&j, "perm", n);
      xj_kv_bool(&j, "trapped", x.trap.trapped);
      xe_put_diffs(&j, "diffs", diffs, xts_state_compare(&exp, &x.out, diffs, 16), 8, "expected", "cpu");
    }
  }
  xe_emit_json(ctx, status, kind == R_VC ? "vperm128" : kind == R_PERMWI ? "vpermwi128" : "vor128", w, 1, &j);
  return 0;
}

XTS_FAMILY(vmx128_regmap, .kind = "semantic", .axes = "sf", .enumerate = rm_enumerate, .run = rm_run, .block_cases = 64,
           .desc = "VMX128 register-field assembly: vor128 VD/VA/VB sweeps, vperm128 VC128, vpermwi128 VPERM128 (self-checking)");
