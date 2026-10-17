/* Exec engine, host backend (PC): nothing executes. Single words are simulated with the reference model when it has a
 * handler; kernels are validated word by word (decode == intended instruction) and reported as dry runs. Addresses are
 * fixed fake values so records and pointer arithmetic look like the console's. */
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include <core/core.h>          /* xts_dry_stats */
#include <exec/exec_priv.h>
#include <model/model_priv.h>   /* model reservation state (src/model/ldst.c) */

#define HOST_SLOT    0x80010000u
#define HOST_POST    0x80010080u
#define HOST_KBUF    0x80100000u
#define HOST_SCRATCH 0x80200000u
#define HOST_BIG     0x90000000u
#define KBUF_WORDS 65536


int xts_exec_is_host(void) { return 1; }
uint64_t xts_scratch_addr(void) { return HOST_SCRATCH; }
uint64_t xts_exec_big_addr(const void *p) { (void)p; return HOST_BIG; }
uint64_t xts_exec_slot_addr(void) { return HOST_SLOT; }
uint64_t xts_exec_post_addr(void) { return HOST_POST; }
uint64_t xts_exec_kernel_addr(void) { return HOST_KBUF; }
uint32_t xts_exec_kernel_capacity(void) { return KBUF_WORDS; }
int xts_exec_init(void) { return 0; }
void xts_sync_code(void *addr, uint32_t len) { (void)addr; (void)len; }
void xts_dcbf_range(void *addr, uint32_t len) { (void)addr; (void)len; }
void xts_touch_range(const void *addr, uint32_t len) { (void)addr; (void)len; }
int xts_exec_runtime_intact(void) { return 1; }

static void dry(xts_exec_t *x, int status)
{
  x->out = x->in;
  memcpy(x->mem_out, x->mem_in, XTS_SCRATCH_BYTES);
  memset(&x->trap, 0, sizeof x->trap);
  x->status = status;
}

/* kernel runs are not simulated, but the hooks still bracket them (the SMT axes count per-run sibling work) */
static int dry_hooked(xts_exec_t *x)
{
  if (xts_exec_hooks_run_pre(x) < 0) return x->status;
  dry(x, XTS_EXEC_DRYRUN);
  xts_exec_hooks_run_post(x);
  return x->status;
}

int xts_exec_word(xts_exec_t *x, uint32_t word)
{
  xts_dry_stats.words++;
  if (x->err[0]) { x->status = XTS_EXEC_NOTRUN; return x->status; }
  if (xts_exec_word_assert(x, word) < 0) return x->status;      /* same rule as the console: never run a wild EA */
  x->run_word = word;
  x->run_kernel = NULL;
  x->run_kernel_words = 0;
  xts_state_t in = x->in;
  xts_exec_resolve_code_ptrs(x, &in, HOST_KBUF);
  int id = xts_decode1(word);
  if (id >= 0 && !xts_model_has(id)) {
    xts_dry_stats.not_simulated++;
    dry(x, XTS_EXEC_DRYRUN);
    return x->status;
  }
  xts_mem_t m;
  xts_mem_init(&m);
  memcpy(x->mem_out, x->mem_in, XTS_SCRATCH_BYTES);
  xts_mem_add(&m, HOST_SCRATCH, x->mem_out, XTS_SCRATCH_BYTES);
  if (xts_big_size()) xts_mem_add(&m, HOST_BIG, xts_big_ptr_internal(), xts_big_size());
  if (xts_exec_hooks_run_pre(x) < 0) return x->status;
  /* The simulated "CPU" and the oracle (xts_semantic_check) both run the model on the same word: keep the model's hidden
   * larx/stcx reservation (src/model/ldst.c) as the oracle will see it, so a simulated stcx does not consume it first. */
  int rv;
  uint64_t rl;
  xts_model_resv_get(&rv, &rl);
  xts_mres_t r = xts_model_exec(NULL, word, &in, &m, &x->out);
  xts_model_resv_set(rv, rl);
  x->status = XTS_EXEC_SIMULATED;                    /* what the post hooks see (DSI/unimplemented corrected below) */
  xts_exec_hooks_run_post(x);
  memset(&x->trap, 0, sizeof x->trap);
  x->status = XTS_EXEC_SIMULATED;
  xts_dry_stats.simulated++;
  switch (r) {
  case XTS_M_OK: break;
  case XTS_M_TRAP: x->trap.trapped = 1; x->trap.vector = 0x700; x->trap.cause = XTS_CAUSE_TRAP; break;
  case XTS_M_ILLEGAL: x->trap.trapped = 1; x->trap.vector = 0x700; x->trap.cause = XTS_CAUSE_ILLEGAL; break;
  case XTS_M_PRIVILEGED: x->trap.trapped = 1; x->trap.vector = 0x700; x->trap.cause = XTS_CAUSE_PRIV; break;
  case XTS_M_ALIGNMENT: x->trap.trapped = 1; x->trap.vector = 0x600; x->trap.dar = m.fault_ea; break;
  case XTS_M_DSI:
    snprintf(x->err, sizeof x->err, "model: DSI at %llX (would crash the console)", (unsigned long long)m.fault_ea);
    x->status = XTS_EXEC_NOTRUN;
    return x->status;
  case XTS_M_UNIMPLEMENTED:
    xts_dry_stats.simulated--;
    xts_dry_stats.not_simulated++;
    dry(x, XTS_EXEC_DRYRUN);
    return x->status;
  }
  if (x->trap.trapped) {
    x->trap.srr0 = HOST_SLOT;
    x->trap.srr1 = (uint64_t)x->trap.cause << 16;
    memcpy(x->mem_out, x->mem_in, XTS_SCRATCH_BYTES);
  }
  return x->status;
}

int xts_exec_kernel(xts_exec_t *x, struct xts_kbuf *k)
{
  if (x->err[0]) { x->status = XTS_EXEC_NOTRUN; return x->status; }
  if (!k || k->err || !k->finished || !k->nexits) {
    snprintf(x->err, sizeof x->err, "kernel not finished or has errors: %s", k ? k->errmsg : "null");
    xts_dry_stats.bad_words++;
    x->status = XTS_EXEC_BADKERNEL;
    return x->status;
  }
  int ni = 0, nr = 0;
  char why[96];
  int bad = xk_validate(k, &ni, &nr, why, sizeof why);
  xts_dry_stats.kernels++;
  xts_dry_stats.kernel_words += k->n;
  xts_dry_stats.kernel_insn_words += (uint64_t)ni;
  xts_dry_stats.kernel_raw_words += (uint64_t)nr;
  if (bad) {
    xts_dry_stats.bad_words += (uint64_t)bad;
    snprintf(x->err, sizeof x->err, "kernel validation: %s", why);
    x->status = XTS_EXEC_BADKERNEL;
    return x->status;
  }
  if (k->n + (xts_env.code_offset & 124) / 4 > KBUF_WORDS) {
    snprintf(x->err, sizeof x->err, "kernel too large (%u words)", (unsigned)k->n);
    x->status = XTS_EXEC_BADKERNEL;
    return x->status;
  }
  x->run_word = 0;
  x->run_kernel = k->w;
  x->run_kernel_words = k->n;
  return dry_hooked(x);
}

int xts_exec_again(xts_exec_t *x)
{
  return dry_hooked(x);
}
