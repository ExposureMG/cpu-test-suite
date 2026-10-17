/* JIT kernel builder (xts/kernel.h). Portable: the same code builds kernels on the console and in the host dry run. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>

static void fail(xts_kbuf_t *k, const char *fmt, ...)
{
  if (k->err) return;
  k->err = -1;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(k->errmsg, sizeof k->errmsg, fmt, ap);
  va_end(ap);
}

xts_kbuf_t *xk_new(uint32_t cap)
{
  xts_kbuf_t *k = calloc(1, sizeof *k);
  if (!k) return NULL;
  k->w = malloc(sizeof(uint32_t) * cap);
  k->tag = malloc(sizeof(uint16_t) * cap);
  if (!k->w || !k->tag) { xk_free(k); return NULL; }
  k->cap = cap;
  xk_reset(k);
  return k;
}

void xk_free(xts_kbuf_t *k)
{
  if (!k) return;
  free(k->w);
  free(k->tag);
  free(k);
}

void xk_reset(xts_kbuf_t *k)
{
  k->n = 0;
  k->nlabels = 0;
  k->nfix = 0;
  k->nexits = 0;
  k->err = 0;
  k->errmsg[0] = 0;
  k->finished = 0;
  k->loop_iters_hint = 0;
}

static int put(xts_kbuf_t *k, uint32_t w, uint16_t tag)
{
  if (k->err) return -1;
  if (k->finished) { fail(k, "emit after xk_finish"); return -1; }
  if (k->n >= k->cap) { fail(k, "kernel buffer full (%u words)", (unsigned)k->cap); return -1; }
  k->w[k->n] = w;
  k->tag[k->n] = tag;
  return (int)k->n++;
}

int xk_emit_a(xts_kbuf_t *k, int insn, unsigned flags, const int64_t *ops, int nops)
{
  if (k->err) return -1;
  const xts_insn_t *d = xts_insn(insn);
  if (!d) { fail(k, "bad instruction id %d", insn); return -1; }
  if (nops != d->nops) { fail(k, "%s: %d operands given, %d expected", d->mnem, nops, d->nops); return -1; }
  uint32_t w;
  int bad = -1, e = xts_encode(insn, flags, ops, &w, &bad);
  if (e) {
    fail(k, "%s: %s (operand %d = %lld)", d->mnem, xts_enc_strerror(e), bad, bad >= 0 ? (long long)ops[bad] : 0LL);
    return -1;
  }
  return put(k, w, (uint16_t)insn);
}

int xk_word(xts_kbuf_t *k, uint32_t word) { return put(k, word, XK_TAG_RAW); }
int xk_data(xts_kbuf_t *k, uint32_t word) { return put(k, word, XK_TAG_DATA); }
int xk_nop(xts_kbuf_t *k) { return xk_emit(k, XI_ori, 0, 0, 0, 0); }

int xk_align(xts_kbuf_t *k, uint32_t bytes)
{
  if (bytes < 4 || (bytes & 3)) return 0;
  while (!k->err && (k->n * 4) % bytes) xk_nop(k);
  return k->err ? -1 : 0;
}

int xk_exit(xts_kbuf_t *k)
{
  int i = put(k, 0x48000000u, XK_TAG_EXIT);    /* "b ." until the engine patches it */
  if (i >= 0) k->nexits++;
  return i;
}

int xk_label(xts_kbuf_t *k)
{
  if (k->nlabels >= XK_MAX_LABELS) { fail(k, "too many labels"); return -1; }
  k->label_pos[k->nlabels] = -1;
  return k->nlabels++;
}

void xk_bind(xts_kbuf_t *k, int label)
{
  if (label < 0 || label >= k->nlabels) { fail(k, "bind: bad label %d", label); return; }
  if (k->label_pos[label] >= 0) { fail(k, "label %d bound twice", label); return; }
  k->label_pos[label] = (int32_t)k->n;
}

int xk_here(xts_kbuf_t *k)
{
  int l = xk_label(k);
  if (l >= 0) xk_bind(k, l);
  return l;
}

static int branch(xts_kbuf_t *k, int insn, unsigned flags, int bo, int bi, int label, int kind)
{
  if (label < 0 || label >= k->nlabels) { fail(k, "branch to bad label %d", label); return -1; }
  if (k->nfix >= XK_MAX_FIXUPS) { fail(k, "too many branch fixups"); return -1; }
  int at = kind == 0 ? xk_emit(k, insn, flags, 0) : xk_emit(k, insn, flags, bo, bi, 0);
  if (at < 0) return -1;
  k->fix[k->nfix].at = (uint32_t)at;
  k->fix[k->nfix].label = label;
  k->fix[k->nfix].kind = kind;
  k->nfix++;
  return at;
}

int xk_b(xts_kbuf_t *k, int label) { return branch(k, XI_bx, 0, 0, 0, label, 0); }
int xk_bl(xts_kbuf_t *k, int label) { return branch(k, XI_bx, XTS_LK, 0, 0, label, 0); }
int xk_bc(xts_kbuf_t *k, int bo, int bi, int label) { return branch(k, XI_bcx, 0, bo, bi, label, 1); }
int xk_bdnz(xts_kbuf_t *k, int label) { return xk_bc(k, 16, 0, label); }

int xk_loop_begin(xts_kbuf_t *k, int count_reg)
{
  xk_emit(k, XI_mtspr, 0, 9, count_reg);         /* mtctr */
  return xk_here(k);
}

int xk_loop_end(xts_kbuf_t *k, int top) { return xk_bdnz(k, top); }

int xk_loop_close(xts_kbuf_t *k, int top, int mode, int count_reg, int crf)
{
  if (k->err) return -1;
  if (top < 0 || top >= k->nlabels || k->label_pos[top] < 0) { fail(k, "loop close: label %d not bound", top); return -1; }
  if (crf < 0 || crf > 7) { fail(k, "loop close: bad CR field %d", crf); return -1; }
  if (mode == XTS_LOOP_GPR) {
    if (count_reg <= 0 || count_reg > 31 || (XTS_RESERVED_GPRS & (1u << count_reg))) { fail(k, "loop close: bad count register %d", count_reg); return -1; }
    xk_emit(k, XI_addi, 0, count_reg, count_reg, -1);
    xk_emit(k, XI_cmpi, 0, crf, 0, count_reg, 0);       /* cmpwi crF, rC, 0 */
  } else if (mode != XTS_LOOP_CTR) {
    fail(k, "loop close: bad mode %d", mode);
    return -1;
  }
  /* displacement of a conditional branch emitted at the next word back to top */
  int64_t back = ((int64_t)k->label_pos[top] - (int64_t)k->n) * 4;
  int bo_loop = mode == XTS_LOOP_GPR ? 4 : 16, bo_exit = mode == XTS_LOOP_GPR ? 12 : 18, bi = mode == XTS_LOOP_GPR ? 4 * crf + 2 : 0;
  if (xts_field_check(XTS_F_BD, back)) return xk_bc(k, bo_loop, bi, top);   /* bne crF,top / bdnz top */
  int out = xk_label(k);
  xk_bc(k, bo_exit, bi, out);                           /* beq crF,out / bdz out */
  int at = xk_b(k, top);
  xk_bind(k, out);
  return k->err ? -1 : at;
}

int xk_prio_prologue(xts_kbuf_t *k)
{
  int p = xts_env.thread_prio;
  if (p < 1 || p > 3) return 0;
  return xk_emit(k, XI_orx, 0, p, p, p) < 0 ? -1 : 0;
}

int xk_finish(xts_kbuf_t *k)
{
  if (k->err) return -1;
  for (int i = 0; i < k->nfix; i++) {
    int l = k->fix[i].label;
    if (k->label_pos[l] < 0) { fail(k, "label %d never bound", l); return -1; }
    int64_t disp = ((int64_t)k->label_pos[l] - (int64_t)k->fix[i].at) * 4;
    int field = k->fix[i].kind == 0 ? XTS_F_LI : XTS_F_BD;
    if (!xts_field_check(field, disp)) { fail(k, "branch at word %u: displacement %lld out of range", k->fix[i].at, (long long)disp); return -1; }
    k->w[k->fix[i].at] = xts_field_put(field, disp, k->w[k->fix[i].at]);
  }
  if (!k->nexits) { fail(k, "kernel has no xk_exit()"); return -1; }
  k->finished = 1;
  return 0;
}

int xk_validate(const xts_kbuf_t *k, int *n_insn, int *n_raw, char *why, int why_size)
{
  int bad = 0, ni = 0, nr = 0;
  if (why && why_size) why[0] = 0;
  for (uint32_t i = 0; i < k->n; i++) {
    uint16_t t = k->tag[i];
    if (t == XK_TAG_RAW || t == XK_TAG_DATA || t == XK_TAG_EXIT) { nr++; continue; }
    ni++;
    const xts_insn_t *d = xts_insn(t);               /* aliases (XI_lwsync) -> base descriptor */
    if (!d || !xts_decodes_as(k->w[i], t)) {
      if (!bad && why) {
        char dis[64];
        snprintf(why, why_size, "word %u = %08X (%s) does not decode as %s", (unsigned)i, k->w[i],
                 xts_disasm(k->w[i], dis, sizeof dis), xts_insn_name(t));
      }
      bad++;
    } else if (d->hw == XTS_HW_ILLEGAL) {
      if (!bad && why)
        snprintf(why, why_size, "word %u is %s, which is illegal on the console (use xk_data for intended ones)",
                 (unsigned)i, xts_insn_name(t));
      bad++;
    }
  }
  if (n_insn) *n_insn = ni;
  if (n_raw) *n_raw = nr;
  return bad;
}
