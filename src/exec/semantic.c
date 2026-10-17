/* Semantic oracle: CPU vs reference model for one instruction word, one record (see xts/exec.h). */
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include <exec/exec_priv.h>

static void put_inputs(xts_json_t *j, const xts_insn_t *d, uint32_t word, const xts_state_t *in)
{
  xj_key(j, "in");
  xj_obj(j);
  for (int i = 0; d && i < d->nops; i++) {
    const xts_operand_t *o = &d->ops[i];
    int64_t v = xts_field_get(o->field, word);
    char k[24];
    if (o->cls == XTS_OC_GPR || (o->cls == XTS_OC_GPR0 && v)) {
      snprintf(k, sizeof k, "%s=r%d", o->name, (int)v);
      xj_kv_hex(j, k, in->gpr[v & 31], 16);
    } else if (o->cls == XTS_OC_FPR) {
      snprintf(k, sizeof k, "%s=f%d", o->name, (int)v);
      xj_kv_hex(j, k, in->fpr[v & 31], 16);
    } else if (o->cls == XTS_OC_VR || o->cls == XTS_OC_VR128 || o->cls == XTS_OC_VRC128) {
      snprintf(k, sizeof k, "%s=v%d", o->name, (int)v);
      char s[40];
      const xts_vr_t *r = &in->vr[v & 127];
      snprintf(s, sizeof s, "%08X%08X%08X%08X", r->w[0], r->w[1], r->w[2], r->w[3]);
      xj_kv_str(j, k, s);
    } else {
      xj_kv_int(j, o->name, v);
    }
  }
  xj_kv_hex(j, "cr", (uint32_t)in->cr, 8);
  xj_kv_hex(j, "xer", in->xer, 16);
  xj_kv_hex(j, "fpscr", (uint32_t)in->fpscr, 8);
  xj_kv_hex(j, "vscr", in->vscr.w[3], 8);
  xj_kv_int(j, "sf", (int)(in->mode & XTS_MODE_SF));
  xj_end(j);
}

static void put_trap(xts_json_t *j, const char *key, int trapped, uint32_t vector, uint32_t cause, uint64_t srr0)
{
  xj_key(j, key);
  xj_obj(j);
  xj_kv_bool(j, "trapped", trapped);
  if (trapped) {
    xj_kv_hex(j, "vector", vector, 4);
    xj_kv_hex(j, "cause", cause, 2);
    if (srr0) xj_kv_hex(j, "srr0", srr0, 8);
  }
  xj_end(j);
}

static void put_diffs(xts_json_t *j, const char *key, const xts_state_diff_t *d, int n, int shown, const char *a_name,
                      const char *b_name)
{
  xj_key(j, key);
  xj_arr(j);
  for (int i = 0; i < shown && i < n; i++) {
    xj_obj(j);
    xj_kv_str(j, "what", d[i].what);
    int wide = !strncmp(d[i].what, "vr", 2) || !strncmp(d[i].what, "mem", 3);
    char s[40];
    if (wide) snprintf(s, sizeof s, "%016llX%016llX", (unsigned long long)d[i].a[0], (unsigned long long)d[i].a[1]);
    else snprintf(s, sizeof s, "%016llX", (unsigned long long)d[i].a[0]);
    xj_kv_str(j, a_name, s);
    if (wide) snprintf(s, sizeof s, "%016llX%016llX", (unsigned long long)d[i].b[0], (unsigned long long)d[i].b[1]);
    else snprintf(s, sizeof s, "%016llX", (unsigned long long)d[i].b[0]);
    xj_kv_str(j, b_name, s);
    xj_end(j);
  }
  xj_end(j);
  if (n > shown) xj_kv_int(j, "more_diffs", n - shown);
}

int xts_semantic_check(xts_ctx_t *ctx, xts_exec_t *x, uint32_t word, const char *note)
{
  static xts_state_t exp;
  static uint8_t mem_exp[XTS_SCRATCH_BYTES];
  static char det[8192];
  static xts_state_diff_t diffs[64];
  int id = xts_decode1(word);
  const xts_insn_t *d = xts_insn(id);

  /* model first (its input is exactly what the CPU gets) */
  xts_mem_t m;
  xts_mem_init(&m);
  memcpy(mem_exp, x->mem_in, XTS_SCRATCH_BYTES);
  xts_mem_add(&m, xts_scratch_addr(), mem_exp, XTS_SCRATCH_BYTES);
  if (xts_big_size()) xts_mem_add(&m, xts_exec_big_addr(xts_big_ptr_internal()), xts_big_ptr_internal(), xts_big_size());
  xts_mres_t mr = xts_model_exec(NULL, word, &x->in, &m, &exp);

  char guard[200];
  if (xts_exec_mem_guard(id, word, &x->in, mr, m.fault_ea, guard, sizeof guard)) {
    /* never executed: a DSI is fatal on the console */
    xts_json_t gj;
    xj_init(&gj, det, sizeof det);
    xj_obj(&gj);
    if (note) xj_kv_str(&gj, "note", note);
    xj_kv_str(&gj, "model", xts_mres_name(mr));
    xj_kv_str(&gj, "why", guard);
    put_inputs(&gj, d, word, &x->in);
    xj_finish(&gj);
    x->status = XTS_EXEC_NOTRUN;
    xts_record_t r;
    xts_record_init(&r);
    r.mnem = d ? d->mnem : "";
    r.word = word;
    r.has_word = 1;
    r.status = XTS_ST_SKIP;
    r.detail = det;
    r.detail_truncated = gj.truncated;
    xts_emit(ctx, &r);
    return XTS_ST_SKIP;
  }

  int st = xts_exec_word(x, word);

  xts_json_t j;
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  if (note) xj_kv_str(&j, "note", note);
  int est = mr == XTS_M_UNIMPLEMENTED && st != XTS_EXEC_DRYRUN && xts_estimate_kind(id, NULL) >= 0;
  xj_kv_str(&j, "model", est ? "estimate" : xts_mres_name(mr));
  int status;
  if (st == XTS_EXEC_NOTRUN || st == XTS_EXEC_BADKERNEL) {
    status = XTS_ST_FAIL;
    xj_kv_str(&j, "exec_error", x->err);
  } else if (st == XTS_EXEC_DRYRUN) {
    status = XTS_ST_SKIP;
    xj_kv_str(&j, "why", "host dry run: not executed");
  } else {
    if (st == XTS_EXEC_SIMULATED) xj_kv_str(&j, "backend", "host-model");
    uint32_t exp_vec = 0, exp_cause = 0;
    int exp_trap = 0;
    switch (mr) {
    case XTS_M_TRAP: exp_trap = 1; exp_vec = 0x700; exp_cause = XTS_CAUSE_TRAP; break;
    case XTS_M_ILLEGAL: exp_trap = 1; exp_vec = 0x700; exp_cause = XTS_CAUSE_ILLEGAL; break;
    case XTS_M_PRIVILEGED: exp_trap = 1; exp_vec = 0x700; exp_cause = XTS_CAUSE_PRIV; break;
    case XTS_M_ALIGNMENT: exp_trap = 1; exp_vec = 0x600; break;
    default: break;
    }
    put_trap(&j, "cpu", x->trap.trapped, x->trap.vector, x->trap.cause, x->trap.srr0);
    if (est) {
      /* XTS_M_ESTIMATE: no exact model answer; the CPU result against the architected bound, lane by lane */
      status = x->trap.trapped ? XTS_ST_FAIL : xts_estimate_verdict(id, word, &x->in, &x->out, &j);
      int nm = xts_mem_compare(x->mem_in, x->mem_out, XTS_SCRATCH_BYTES, diffs, 64);
      if (nm) { status = XTS_ST_FAIL; put_diffs(&j, "mem_changes", diffs, nm, 8, "in", "cpu"); }
      if (status != XTS_ST_PASS) put_inputs(&j, d, word, &x->in);
    } else if (mr == XTS_M_UNIMPLEMENTED || mr == XTS_M_DSI) {
      /* no oracle: keep what the CPU did (changes vs input) */
      status = XTS_ST_SKIP;
      int n = xts_state_compare(&x->in, &x->out, diffs, 64);
      int nm = xts_mem_compare(x->mem_in, x->mem_out, XTS_SCRATCH_BYTES, diffs + (n < 64 ? n : 64), 64 - (n < 64 ? n : 64));
      put_inputs(&j, d, word, &x->in);
      put_diffs(&j, "changes", diffs, n + nm, 24, "in", "cpu");
    } else {
      int bad = 0;
      put_trap(&j, "expected", exp_trap, exp_vec, exp_cause, 0);
      if (exp_trap != x->trap.trapped || (exp_trap && (exp_vec != x->trap.vector || exp_cause != x->trap.cause)))
        bad = 1;
      if (x->trap.trapped && x->trap.where != 0) bad = 1, xj_kv_str(&j, "srr0_problem", "trap outside the slot");
      int n = xts_state_compare(&exp, &x->out, diffs, 64);
      int nm = xts_mem_compare(mem_exp, x->mem_out, XTS_SCRATCH_BYTES, diffs + (n < 64 ? n : 64), 64 - (n < 64 ? n : 64));
      if (n + nm) bad = 1;
      status = bad ? XTS_ST_FAIL : XTS_ST_PASS;
      if (bad) {
        put_inputs(&j, d, word, &x->in);
        put_diffs(&j, "diffs", diffs, n + nm, 24, "model", "cpu");
      }
    }
  }
  xj_finish(&j);
  xts_record_t r;
  xts_record_init(&r);
  r.mnem = d ? d->mnem : "";
  r.word = word;
  r.has_word = 1;
  r.status = status;
  r.detail = det;
  r.detail_truncated = j.truncated;
  xts_emit(ctx, &r);
  return status;
}
