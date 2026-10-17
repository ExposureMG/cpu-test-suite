/* Exec engine, parts shared by the console and the host build: case setup, pointers, big buffer, prep. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include <exec/exec_priv.h>

xts_exec_hooks_t xts_exec_hooks;

/* ---- chained hooks ---- */
static struct { xts_exec_pre_fn pre; xts_exec_post_fn post; void *ud; int used; } hooks[XTS_EXEC_MAX_HOOKS];
static int hook_order[XTS_EXEC_MAX_HOOKS], nhooks;    /* slot indices in add order */

int xts_exec_hook_add(xts_exec_pre_fn pre, xts_exec_post_fn post, void *ud)
{
  for (int h = 0; h < XTS_EXEC_MAX_HOOKS; h++)
    if (!hooks[h].used) {
      hooks[h].pre = pre;
      hooks[h].post = post;
      hooks[h].ud = ud;
      hooks[h].used = 1;
      hook_order[nhooks++] = h;
      return h;
    }
  return -1;
}

int xts_exec_hook_remove(int handle)
{
  if (handle < 0 || handle >= XTS_EXEC_MAX_HOOKS || !hooks[handle].used) return -1;
  memset(&hooks[handle], 0, sizeof hooks[handle]);
  int o = 0;
  for (int i = 0; i < nhooks; i++)
    if (hook_order[i] != handle) hook_order[o++] = hook_order[i];
  nhooks = o;
  return 0;
}

int xts_exec_hook_count(void) { return nhooks; }

void xts_exec_hook_clear(void)
{
  memset(hooks, 0, sizeof hooks);
  nhooks = 0;
  xts_exec_hooks.pre = NULL;
  xts_exec_hooks.post = NULL;
}

int xts_exec_hooks_run_pre(xts_exec_t *x)
{
  /* the v1 slot first, then the chain in add order; on a refusal undo (post, reverse) what already ran */
  int ran = -1;                                    /* -1: v1 slot not run yet; k >= 0: chain[0..k-1] ran */
  int refused = 0;
  if (xts_exec_hooks.pre && xts_exec_hooks.pre(x) < 0) refused = 1;
  if (!refused) {
    ran = 0;
    for (; ran < nhooks; ran++) {
      int h = hook_order[ran];
      if (hooks[h].pre && hooks[h].pre(x, hooks[h].ud) < 0) { refused = 1; break; }
    }
  }
  if (!refused) return 0;
  if (!x->err[0]) snprintf(x->err, sizeof x->err, "exec pre-hook refused the run");
  x->status = XTS_EXEC_NOTRUN;
  if (ran >= 0) {
    for (int i = ran - 1; i >= 0; i--) {
      int h = hook_order[i];
      if (hooks[h].post) hooks[h].post(x, hooks[h].ud);
    }
    if (xts_exec_hooks.post) xts_exec_hooks.post(x);
  }
  return -1;
}

void xts_exec_hooks_run_post(xts_exec_t *x)
{
  for (int i = nhooks - 1; i >= 0; i--) {
    int h = hook_order[i];
    if (hooks[h].post) hooks[h].post(x, hooks[h].ud);
  }
  if (xts_exec_hooks.post) xts_exec_hooks.post(x);
}

/* ---- extra trap ranges ---- */
xts_trange_t xts_exec_ranges[XTS_EXEC_EXTRA_RANGES];
int (*xts_exec_ranges_changed)(void);

int xts_exec_trap_range(int idx, uint64_t lo, uint64_t hi)
{
  if (idx < 0 || idx >= XTS_EXEC_EXTRA_RANGES) return -1;
  uint32_t l = (uint32_t)lo, h = (uint32_t)hi;
  if (h < l || h - l > XTS_EXEC_RANGE_MAX) return -1;
  if (h == l) l = h = 0;                           /* removed */
  xts_exec_ranges[idx].lo = l;
  xts_exec_ranges[idx].hi = h;
  return xts_exec_ranges_changed ? xts_exec_ranges_changed() : 0;
}

int xts_exec_trap_range_get(int idx, uint64_t *lo, uint64_t *hi)
{
  if (idx < 0 || idx >= XTS_EXEC_EXTRA_RANGES) return -1;
  if (lo) *lo = xts_exec_ranges[idx].lo;
  if (hi) *hi = xts_exec_ranges[idx].hi;
  return 0;
}

int xts_exec_trap_classify(uint64_t srr0, uint32_t *offset)
{
  uint32_t a = (uint32_t)srr0;
  for (int i = 0; i < XTS_EXEC_EXTRA_RANGES; i++) {
    uint32_t l = (uint32_t)xts_exec_ranges[i].lo, h = (uint32_t)xts_exec_ranges[i].hi;
    if (h > l && a - l < h - l) {                  /* same unsigned test as the vector stub */
      if (offset) *offset = a - l;
      return 2 + i;
    }
  }
  return -1;
}

void xts_case_apply_env(xts_exec_t *x)
{
  xts_set_sf(x, xts_env.sf);
  x->in.fpscr = (((uint32_t)x->in.fpscr) & ~xts_env.fpscr_clr) | xts_env.fpscr_set;
  x->in.vscr.w[3] = (x->in.vscr.w[3] & ~xts_env.vscr_clr) | xts_env.vscr_set;
}

void xts_case_init(xts_exec_t *x)
{
  xts_state_baseline(&x->in);
  memset(&x->out, 0, sizeof x->out);
  xts_scratch_baseline(x->mem_in, XTS_SCRATCH_BYTES);
  memset(x->mem_out, 0, sizeof x->mem_out);
  memset(&x->trap, 0, sizeof x->trap);
  x->status = XTS_EXEC_NOTRUN;
  x->err[0] = 0;
  x->ncode_ptr = 0;
  x->ticks = 0;
  x->aux_status = 0;
  xts_case_apply_env(x);
}

void xts_set_ptr(xts_exec_t *x, int r, int64_t off) { x->in.gpr[r & 31] = xts_scratch_addr() + (uint64_t)off; }

void xts_set_mem(xts_exec_t *x, int off, const void *bytes, int n)
{
  if (off < 0 || n < 0 || off + n > XTS_SCRATCH_BYTES) { snprintf(x->err, sizeof x->err, "xts_set_mem out of range"); return; }
  memcpy(x->mem_in + off, bytes, n);
}

void xts_set_mem_be(xts_exec_t *x, int off, int n, uint64_t v)
{
  if (off < 0 || n < 1 || n > 8 || off + n > XTS_SCRATCH_BYTES) { snprintf(x->err, sizeof x->err, "xts_set_mem_be out of range"); return; }
  xts_be_store(x->mem_in + off, n, v);
}

void xts_set_mem_ptr(xts_exec_t *x, int at, int target, int as32)
{
  xts_set_mem_be(x, at, as32 ? 4 : 8, xts_scratch_addr() + (uint64_t)target);
}

int xts_set_code_ptr(xts_exec_t *x, int reg, int base, int32_t off)
{
  if (x->ncode_ptr >= 4 || reg < 0 || reg > 33 || (reg < 32 && (XTS_RESERVED_GPRS & (1u << reg))) || base < 0 || base > 2) {
    snprintf(x->err, sizeof x->err, "xts_set_code_ptr: bad reg/base or too many");
    return -1;
  }
  x->code_ptr[x->ncode_ptr].reg = (uint8_t)reg;
  x->code_ptr[x->ncode_ptr].base = (uint8_t)base;
  x->code_ptr[x->ncode_ptr].off = off;
  x->ncode_ptr++;
  return 0;
}

/* called by the backends right before running: resolve deferred code pointers into `st` */
void xts_exec_resolve_code_ptrs(const xts_exec_t *x, xts_state_t *st, uint64_t kernel_start)
{
  for (int i = 0; i < x->ncode_ptr; i++) {
    uint64_t base = x->code_ptr[i].base == XTS_CODE_SLOT ? xts_exec_slot_addr()
                  : x->code_ptr[i].base == XTS_CODE_POST ? xts_exec_post_addr() : kernel_start;
    uint64_t a = (uint32_t)(base + (int64_t)x->code_ptr[i].off);
    int r = x->code_ptr[i].reg;
    if (r < 32) st->gpr[r] = a;
    else if (r == 32) st->lr = a;
    else st->ctr = a;
  }
}

uint32_t xts_branch_word(uint64_t from, uint64_t to)
{
  int32_t d = (int32_t)((uint32_t)to - (uint32_t)from);
  return 0x48000000u | ((uint32_t)d & 0x03FFFFFCu);
}

/* ---- big buffer ---- */
static uint8_t *big_raw, *big;
static uint32_t big_bytes;

void *xts_big(uint32_t bytes, uint64_t *addr)
{
  if (!bytes) return NULL;
  bytes = (bytes + 0xFFFFF) & ~0xFFFFFu;                 /* whole MB */
  if (bytes > big_bytes) {
    /* grow: free the old buffer first (both may not fit), and if the larger one cannot be had, get the old size back so
     * a failed growth never leaves callers without the buffer they already had */
    uint32_t old = big_bytes;
    free(big_raw);
    big_raw = big = NULL;
    big_bytes = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
      uint32_t want = attempt ? old : bytes;
      if (!want) break;
      big_raw = malloc((size_t)want + 0x10000);
      if (!big_raw) continue;
      big = (uint8_t *)(((uintptr_t)big_raw + 0xFFFF) & ~(uintptr_t)0xFFFF);
      if (!xts_plat_is_host() && (uintptr_t)big + want > xts_plat_heap_limit()) {   /* never grow into the crash page */
        free(big_raw);
        big_raw = big = NULL;
        continue;
      }
      big_bytes = want;
      if (!attempt) break;
      return NULL;                                       /* old size restored (contents are not preserved) */
    }
    if (!big_bytes) return NULL;
  }
  if (addr) *addr = xts_exec_big_addr(big);
  return big;
}

uint32_t xts_big_size(void) { return big_bytes; }
void *xts_big_ptr_internal(void) { return big; }

void xts_set_big_ptr(xts_exec_t *x, int r, uint32_t off)
{
  uint64_t a;
  if (!xts_big(off + 1, &a)) { snprintf(x->err, sizeof x->err, "big buffer allocation failed"); return; }
  if (off >= big_bytes) { snprintf(x->err, sizeof x->err, "big pointer out of range"); return; }
  x->in.gpr[r & 31] = a + off;
}

int xts_prep(uint32_t base, uint32_t nelem, uint32_t stride, int mode, int ncycles, uint32_t seed, int slot8)
{
  uint64_t span = 0, addr;
  if (!big || (uint64_t)base + (uint64_t)nelem * stride > big_bytes) return -1;
  addr = xts_exec_big_addr(big);
  int m = xts_prep_pure(big, (uint32_t)addr, big_bytes, base, nelem, stride, mode, ncycles, seed, slot8, &span);
  if (m < 0) return -1;
  int op = m & 15;
  if ((m & XTS_PREP_DCBF) || op == 3) xts_dcbf_range(big + base, (uint32_t)span);
  if ((m & XTS_PREP_TOUCH) || op == 4) xts_touch_range(big + base, (uint32_t)span);
  return 0;
}

/* ---- semantic DSI guard ----
 * A data storage interrupt is fatal on the console (libxenon's handler -> crash path): never execute a word the model
 * predicts DSI for, and for a memory word the model cannot simulate, execute it only if its effective address (from the
 * input state: (RA|0) + RB or + d/ds, 32-bit in SF=0) and a conservative access window lie inside the scratch area or the
 * big buffer. Returns 1 (with `why`) if the word must not run. Used by xts_semantic_check and the exec families' kernel
 * oracle (one word inside a kernel). */
static int within(uint64_t lo, uint64_t hi, uint64_t base, uint64_t size) { return size && lo >= base && hi <= base + size; }

int xts_exec_mem_guard(int id, uint32_t word, const xts_state_t *in, int model_result, uint64_t fault_ea, char *why,
                       int why_size)
{
  const xts_insn_t *d = xts_insn(id);
  if (model_result == XTS_M_DSI) {
    snprintf(why, why_size, "model predicts DSI at %llX: not executed (a DSI crashes the console)",
             (unsigned long long)fault_ea);
    return 1;
  }
  if (model_result != XTS_M_UNIMPLEMENTED || !d) return 0;
  if (d->unit != XTS_UNIT_MEMORY && !((d->implicit_r | d->implicit_w) & XTS_IMP_MEM)) return 0;
  int addr = 0;
  uint64_t ea = 0;
  for (int i = 0; i < d->nops; i++) {
    const xts_operand_t *o = &d->ops[i];
    int64_t v = xts_field_get(o->field, word);
    if (!strcmp(o->name, "RA") || !strcmp(o->name, "RA0")) {          /* base: (RA|0) */
      addr = 1;
      if (!(o->cls == XTS_OC_GPR0 && v == 0)) ea += in->gpr[v & 31];
    } else if (!strcmp(o->name, "RB")) {
      addr = 1;
      ea += in->gpr[v & 31];
    } else if (o->cls == XTS_OC_DISP) {
      ea += (uint64_t)v;
    }
  }
  if (!addr) return 0;                                 /* no address operands (barriers): nothing to range-check */
  if (!(in->mode & XTS_MODE_SF)) ea = (uint32_t)ea;
  const char *n = d->asm_name ? d->asm_name : d->mnem;
  uint64_t lo, hi;
  if (!strncmp(n, "dcb", 3) || !strncmp(n, "icb", 3)) { lo = ea & ~127ull; hi = lo + 128; }        /* cache blocks */
  else if (!strncmp(n, "lsw", 3) || !strncmp(n, "stsw", 4) || !strncmp(n, "lmw", 3) || !strncmp(n, "stmw", 4)) {
    lo = ea; hi = ea + 128;                                                                       /* up to 32 words */
  } else { lo = ea & ~15ull; hi = lo + 16; }                                                      /* <= 16 bytes */
  if (within(lo, hi, xts_scratch_addr(), XTS_SCRATCH_BYTES)) return 0;
  if (xts_big_size() && within(lo, hi, xts_exec_big_addr(xts_big_ptr_internal()), xts_big_size())) return 0;
  snprintf(why, why_size, "model cannot simulate %s and its EA %llX is outside the scratch area and the big buffer: "
           "not executed (a DSI crashes the console)", d->mnem, (unsigned long long)ea);
  return 1;
}
