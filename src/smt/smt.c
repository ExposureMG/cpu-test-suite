/* src/smt common part (console and host): thread state machine with timeouts, JIT load kernels, the SMT exec hooks,
 * per-block info records, the speed-change precondition and the watchdog logic. Hardware access goes through the backend
 * (smt_console.c: libxenon; smt_host.c: a simulation so the host dry run walks the same code). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include <xts/kernel.h>
#include <xts/exec.h>
#include <xts/json.h>
#include "smt_int.h"

xts_smt_tcb_t xts_smt_tcbs[XTS_SMT_NPIR];
uint32_t xts_smt_tcbp[XTS_SMT_NPIR];
xts_smt_wd_t xts_smt_wd;
xts_smt_runstats_t xts_smt_rs;
int xts_smt_hook_sibling_mode;
int xts_smt_hook_cores;

/* timeouts (ms) */
#define TO_SLOT   100
#define TO_START  200
#define TO_STOP   1000
#define TO_PARK   200
#define TO_WAKE   500
#define TO_CORES  2000
#define TO_STALL  250

/* load kernel shape: INNER bodies per outer iteration (the heartbeat counts outer iterations) */
#define BODY_SAME_MIN 32
#define MEM_SIB_BYTES (4u << 20)          /* busy_mem: private random chase, well beyond the 1 MB L2 */
#define L2P_BYTES     (768u << 10)        /* l2_pressure: total over all load threads */
#define STREAM_SLICE  (3u << 20)          /* mem_stream: per thread, multiple of 32 KB */
#define CORE_BUF      (4u * STREAM_SLICE)

static const char *const load_names[XTS_SMT_NLOADS] = {
  "none", "busy_int", "busy_fp", "busy_vmx", "busy_mem", "same_insn", "busy_ld", "l2_pressure", "mem_stream", "watchdog"
};
const char *xts_smt_load_name(int l) { return l >= 0 && l < XTS_SMT_NLOADS ? load_names[l] : "?"; }
static const char *const st_names[] = { "idle", "running", "parked", "lost" };
const char *xts_smt_state_name(int s) { return s >= 0 && s < 4 ? st_names[s] : "?"; }
const char *xts_smt_err_name(int e)
{
  switch (e) {
  case 0: return "ok";
  case XTS_SMT_E_PIR: return "bad or reserved PIR";
  case XTS_SMT_E_LOST: return "thread lost earlier this boot";
  case XTS_SMT_E_BUSY: return "libxenon slot busy";
  case XTS_SMT_E_START_TO: return "start timeout";
  case XTS_SMT_E_STOP_TO: return "stop timeout";
  case XTS_SMT_E_PARK_TO: return "park timeout (CTRL TE1 stayed set)";
  case XTS_SMT_E_WAKE_TO: return "wake timeout";
  case XTS_SMT_E_CORES: return "threads 2-5 not up";
  case XTS_SMT_E_BUILD: return "kernel build/alloc failed";
  case XTS_SMT_E_STATE: return "wrong thread state";
  }
  return "?";
}

static struct {
  int st, load;
  uint32_t gen, ops;
  uint32_t last_iters;
  uint64_t last_change_tb;
  uint64_t start_tb;
  uint64_t same_hash;          /* same_insn: hash of the kept words */
} th[XTS_SMT_NPIR];
static int inited, cores_state;          /* cores_state: 0 unknown, 1 up, -1 failed */
static uint64_t park_tscr;
static xts_kbuf_t *kb;
static uint8_t *sib_mem, *core_mem;      /* lazily allocated load buffers (never the tests' big buffer) */
static uint8_t l1bufs[XTS_SMT_NPIR][4096] __attribute__((aligned(128)));
static int wd_running;
static uint32_t wd_timeout_ms;

static uint64_t ms_ticks(uint32_t ms) { return (uint64_t)ms * xts_tb_hz() / 1000u; }
static uint32_t tb10(void) { return smtb_tb10(); }

int xts_smt_init(void)
{
  if (inited) return 0;
  inited = 1;
  memset(xts_smt_tcbs, 0, sizeof xts_smt_tcbs);
  for (int i = 0; i < XTS_SMT_NPIR; i++) {
    xts_smt_tcbp[i] = smtb_addr(&xts_smt_tcbs[i]);
    xts_smt_tcbs[i].k_one = 1.0;
    xts_smt_tcbs[i].k_zero = 0.0;
    xts_smt_tcbs[i].k_l1buf = smtb_addr(l1bufs[i]);
  }
  smt_sync();
  return 0;
}

int xts_smt_reserved(int pir)
{
#ifdef XTS_WATCHDOG
  return pir == 4 || pir == 5;   /* core 2: watchdog on PIR 4, its sibling left alone so the watchdog keeps its core */
#else
  (void)pir;
  return 0;
#endif
}

int xts_smt_state(int pir) { return pir >= 1 && pir < XTS_SMT_NPIR ? th[pir].st : XTS_SMT_ST_LOST; }
uint32_t xts_smt_iters(int pir) { return pir >= 1 && pir < XTS_SMT_NPIR ? xts_smt_tcbs[pir].iters : 0; }
uint32_t xts_smt_ops_per_iter(int pir) { return pir >= 1 && pir < XTS_SMT_NPIR ? th[pir].ops : 0; }

int xts_smt_all_idle(void)
{
  for (int p = 1; p < XTS_SMT_NPIR; p++) {
    if (th[p].st == XTS_SMT_ST_RUNNING && th[p].load == XTS_SMT_WATCHDOG) continue;
    if (th[p].st != XTS_SMT_ST_IDLE) return 0;
  }
  return 1;
}

/* ---- waiting (always bounded) ---------------------------------------------------------------------------------------- */
typedef int (*cond_fn)(int pir, uint32_t arg);
static int wait_for(cond_fn f, int pir, uint32_t arg, uint32_t ms)
{
  uint64_t t0 = xts_tb(), lim = ms_ticks(ms);
  for (;;) {
    if (f(pir, arg)) return 1;
    if (xts_tb() - t0 > lim) return f(pir, arg);
    smtb_relax(pir);
  }
}
static int c_slot_free(int pir, uint32_t a) { (void)a; return !smtb_slot_busy(pir); }
static int c_started(int pir, uint32_t gen) { return xts_smt_tcbs[pir].started == gen; }
static int c_done_free(int pir, uint32_t gen) { return xts_smt_tcbs[pir].done == gen && !smtb_slot_busy(pir); }
static int c_te1(int pir, uint32_t v) { (void)pir; return smtb_te1() == (int)v; }
static int c_awake(int pir, uint32_t a) { (void)a; return smtb_te1() == 1 && !smtb_slot_busy(pir); }

int xts_smt_cores_up(void)
{
  xts_smt_init();
  if (cores_state == 1) return 0;
  if (cores_state < 0) return XTS_SMT_E_CORES;
  int r = smtb_cores_up(TO_CORES);
  cores_state = r == 0 ? 1 : -1;
  xts_log("smt: threads 2-5 %s (%s)", r == 0 ? "up" : "did NOT come up", smtb_name());
  return r == 0 ? 0 : XTS_SMT_E_CORES;
}

/* ---- load kernels ----------------------------------------------------------------------------------------------------- */
/* r11/r12/cr7 are the tail's; the tail re-derives everything from constants, so a body may clobber any register */
static int build_kernel(int pir, int load, const uint32_t *words, int nwords, uint32_t *ops_out)
{
  if (!kb) kb = xk_new(4096);
  if (!kb) return -1;
  xts_kbuf_t *k = kb;
  uint32_t tcb = xts_smt_tcbp[pir];
  int inner = 64, body = 0, stream = 0;
  xk_reset(k);
  switch (load) {           /* prologue (r3 = tcb on entry) */
  case XTS_SMT_BUSY_INT:
    for (int i = 0; i < 12; i++) xk_li64(k, 14 + i, i + 1);
    xk_li64(k, 26, 1);
    inner = 256;
    break;
  case XTS_SMT_BUSY_FP:
    xk_emit(k, XI_lfd, 0, 1, TCB_K_ONE, 3);
    xk_emit(k, XI_lfd, 0, 0, TCB_K_ZERO, 3);
    for (int i = 2; i < 14; i++) xk_emit(k, XI_fmrx, 0, i, 1);
    inner = 128;
    break;
  case XTS_SMT_BUSY_VMX:
    xk_emit(k, XI_vxor, 0, 0, 0, 0);
    xk_emit(k, XI_vspltisw, 0, 1, 1);
    xk_emit(k, XI_vcfsx, 0, 1, 1, 0);
    for (int i = 2; i < 14; i++) xk_emit(k, XI_vor, 0, i, 1, 1);
    inner = 128;
    break;
  case XTS_SMT_BUSY_MEM:
  case XTS_SMT_L2_PRESSURE:
    xk_emit(k, XI_lwz, 0, 4, TCB_K_PTR, 3);
    inner = 4;
    break;
  case XTS_SMT_BUSY_LD:
    xk_emit(k, XI_lwz, 0, 4, TCB_K_L1BUF, 3);
    inner = 128;
    break;
  case XTS_SMT_MEM_STREAM:
    xk_emit(k, XI_lwz, 0, 4, TCB_K_PTR, 3);
    xk_emit(k, XI_lwz, 0, 5, TCB_K_END, 3);
    xk_emit(k, XI_orx, 0, 6, 4, 4);
    inner = 64;               /* 64 x 512 bytes = 32 KB per outer iteration; slices are multiples of 32 KB */
    stream = 1;
    break;
  case XTS_SMT_SAME_INSN:
    if (nwords < 1) return -1;
    for (int i = 0; i < 32; i++) xk_emit(k, XI_lfd, 0, i, TCB_K_ONE, 3);   /* FPRs 1.0 (no NaN/denormal inputs) */
    for (int i = 0; i < 128; i++) xk_emit(k, XI_vxor128, 0, i, i, i);      /* every VR zero */
    for (int r = 0; r < 32; r++)
      if (r != 1 && r != 2 && r != 13 && r != 3) xk_li64(k, r, 0x100 + r);  /* small non-zero GPRs */
    xk_li64(k, 3, 0x103);
    inner = 64;
    break;
  default:
    return -1;
  }
  int outer = xk_here(k);
  xk_li64(k, 12, inner);
  int top = xk_loop_begin(k, 12);
  switch (load) {           /* body */
  case XTS_SMT_BUSY_INT:
    for (int i = 0; i < 24; i++) xk_emit(k, XI_addx, 0, 14 + i % 12, 14 + i % 12, 26);
    body = 24;
    break;
  case XTS_SMT_BUSY_FP:
    for (int i = 0; i < 24; i++) xk_emit(k, XI_fmaddx, 0, 2 + i % 12, 2 + i % 12, 1, 0);
    body = 24;
    break;
  case XTS_SMT_BUSY_VMX:
    for (int i = 0; i < 24; i++) xk_emit(k, XI_vmaddfp, 0, 2 + i % 12, 2 + i % 12, 1, 0);
    body = 24;
    break;
  case XTS_SMT_BUSY_MEM:
    for (int i = 0; i < 16; i++) xk_emit(k, XI_ld, 0, 4, 0, 4);
    body = 16;
    break;
  case XTS_SMT_L2_PRESSURE:
    for (int i = 0; i < 8; i++) {
      xk_emit(k, XI_ld, 0, 4, 0, 4);
      xk_emit(k, XI_stw, 0, 4, 64, 4);     /* dirty the line (pointer lives in bytes 0-7) */
    }
    body = 16;
    break;
  case XTS_SMT_BUSY_LD: {
    static const int regs[8] = { 5, 6, 7, 8, 9, 10, 14, 15 };
    for (int i = 0; i < 16; i++) xk_emit(k, XI_lwz, 0, regs[i & 7], (i & 7) * 128 + (i >> 3) * 4, 4);
    body = 16;
    break;
  }
  case XTS_SMT_MEM_STREAM:
    for (int i = 0; i < 4; i++) xk_emit(k, XI_lwz, 0, 7 + i, i * 128, 4);
    xk_emit(k, XI_addi, 0, 4, 4, 512);
    body = 5;
    break;
  case XTS_SMT_SAME_INSN:
    while (body < BODY_SAME_MIN)
      for (int i = 0; i < nwords; i++, body++) xk_word(k, words[i]);
    break;
  }
  xk_loop_end(k, top);
  if (stream) {             /* wrap the cursor at the end of the slice */
    int skip = xk_label(k);
    xk_emit(k, XI_cmpl, 0, 6, 0, 4, 5);
    xk_bc(k, 12, 24, skip);            /* blt cr6 */
    xk_emit(k, XI_orx, 0, 4, 6, 6);
    xk_bind(k, skip);
  }
  xk_li32(k, 11, tcb);
  xk_emit(k, XI_lwz, 0, 12, TCB_ITERS, 11);
  xk_emit(k, XI_addi, 0, 12, 12, 1);
  xk_emit(k, XI_stw, 0, 12, TCB_ITERS, 11);
  xk_emit(k, XI_lwz, 0, 12, TCB_STOP, 11);
  xk_emit(k, XI_cmpi, 0, 7, 0, 12, 0);
  xk_bc(k, 12, 30, outer);             /* beq cr7: not stopped -> next outer iteration */
  xk_exit(k);
  if (xk_finish(k) < 0) {
    xts_log("smt: %s kernel build failed: %s", xts_smt_load_name(load), k->errmsg);
    return -1;
  }
  uint32_t code = smtb_install(pir, k);
  if (!code) return -1;
  xts_smt_tcbs[pir].code = code;
  *ops_out = (uint32_t)(inner * body);
  return 0;
}

static int prep_buffers(int pir, int load, int slice, int nslices)
{
  xts_smt_tcb_t *t = &xts_smt_tcbs[pir];
  uint64_t span;
  if (load == XTS_SMT_BUSY_MEM) {
    if (!sib_mem && !(sib_mem = smtb_alloc(MEM_SIB_BYTES))) return -1;
    if (xts_prep_pure(sib_mem, smtb_addr(sib_mem), MEM_SIB_BYTES, 0, MEM_SIB_BYTES / 128, 128, 0, 1, 0x5157u, 0, &span) < 0)
      return -1;
    t->k_ptr = smtb_addr(sib_mem);
  } else if (load == XTS_SMT_L2_PRESSURE || load == XTS_SMT_MEM_STREAM) {
    if (!core_mem && !(core_mem = smtb_alloc(CORE_BUF))) return -1;
    if (load == XTS_SMT_L2_PRESSURE) {
      uint32_t per = (L2P_BYTES / (uint32_t)nslices) & ~127u, base = per * (uint32_t)slice;
      if (xts_prep_pure(core_mem, smtb_addr(core_mem), CORE_BUF, base, per / 128, 128, 0, 1, 0xC0DE + slice, 0, &span) < 0)
        return -1;
      t->k_ptr = smtb_addr(core_mem) + base;
    } else {
      t->k_ptr = smtb_addr(core_mem) + STREAM_SLICE * (uint32_t)slice;
      t->k_end = t->k_ptr + STREAM_SLICE;
    }
  }
  smt_sync();
  return 0;
}

/* ---- start / stop ------------------------------------------------------------------------------------------------------ */
static int start_ex(int pir, int load, const uint32_t *words, int nwords, int slice, int nslices)
{
  xts_smt_init();
  if (pir < 1 || pir >= XTS_SMT_NPIR) return XTS_SMT_E_PIR;
  if (load != XTS_SMT_WATCHDOG && xts_smt_reserved(pir)) return XTS_SMT_E_PIR;
  if (th[pir].st == XTS_SMT_ST_LOST) return XTS_SMT_E_LOST;
  if (th[pir].st != XTS_SMT_ST_IDLE) return XTS_SMT_E_STATE;
  if (pir >= 2 && xts_smt_cores_up() < 0) return XTS_SMT_E_CORES;
  if (!smtb_thread_alive(pir)) return XTS_SMT_E_CORES;
  if (!wait_for(c_slot_free, pir, 0, TO_SLOT)) return XTS_SMT_E_BUSY;
  xts_smt_tcb_t *t = &xts_smt_tcbs[pir];
  uint32_t ops = 0;
  t->code = 0;
  if (load != XTS_SMT_WATCHDOG) {
    if (prep_buffers(pir, load, slice, nslices) < 0) return XTS_SMT_E_BUILD;
    if (build_kernel(pir, load, words, nwords, &ops) < 0) return XTS_SMT_E_BUILD;
  }
  t->stop = 0;
  t->started = 0;
  t->done = 0;
  t->trap_vector = 0;
  t->trap_srr0 = 0;
  t->exit_why = 0;
  t->iters = 0;
  t->load = (uint32_t)load;
  uint32_t gen = ++th[pir].gen;
  smt_lwsync();
  t->gen = gen;             /* last: the worker reads gen first */
  smt_sync();
  if (smtb_launch(pir) < 0) return XTS_SMT_E_BUSY;
  if (!wait_for(c_started, pir, gen, TO_START)) {
    t->stop = 1;            /* if it starts late it leaves at once */
    smt_sync();
    if (wait_for(c_done_free, pir, gen, TO_START)) return XTS_SMT_E_START_TO;
    th[pir].st = XTS_SMT_ST_LOST;
    xts_log("smt: PIR %d did not start %s: thread marked lost", pir, xts_smt_load_name(load));
    return XTS_SMT_E_START_TO;
  }
  th[pir].st = XTS_SMT_ST_RUNNING;
  th[pir].load = load;
  th[pir].ops = ops;
  th[pir].start_tb = th[pir].last_change_tb = xts_tb();
  th[pir].last_iters = 0;
  return 0;
}

int xts_smt_start(int pir, int load, const uint32_t *words, int nwords)
{
  return start_ex(pir, load, words, nwords, 0, 1);
}

int xts_smt_stop(int pir)
{
  if (pir < 1 || pir >= XTS_SMT_NPIR) return XTS_SMT_E_PIR;
  if (th[pir].st == XTS_SMT_ST_IDLE) return 0;
  if (th[pir].st != XTS_SMT_ST_RUNNING) return th[pir].st == XTS_SMT_ST_LOST ? XTS_SMT_E_LOST : XTS_SMT_E_STATE;
  xts_smt_tcb_t *t = &xts_smt_tcbs[pir];
  t->stop = 1;
  smt_sync();
  if (!wait_for(c_done_free, pir, th[pir].gen, TO_STOP)) {
    th[pir].st = XTS_SMT_ST_LOST;
    xts_log("smt: PIR %d (%s) did not stop (done=%u gen=%u slot %s): thread marked lost", pir,
            xts_smt_load_name(th[pir].load), (unsigned)t->done, (unsigned)th[pir].gen, smtb_slot_busy(pir) ? "busy" : "free");
    return XTS_SMT_E_STOP_TO;
  }
  th[pir].st = XTS_SMT_ST_IDLE;
  if (t->trap_vector)
    xts_log("smt: PIR %d (%s) took interrupt %X at %08X inside its kernel", pir, xts_smt_load_name(th[pir].load),
            (unsigned)t->trap_vector, (unsigned)t->trap_srr0);
  return 0;
}

/* ---- park / wake the sibling (PIR 1), verified by CTRL[TE1] ------------------------------------------------------------- */
int xts_smt_park(void)
{
  xts_smt_init();
  if (th[1].st == XTS_SMT_ST_PARKED) return 0;
  if (th[1].st == XTS_SMT_ST_LOST) return XTS_SMT_E_LOST;
  if (th[1].st != XTS_SMT_ST_IDLE) return XTS_SMT_E_STATE;
  if (!smtb_thread_alive(1) || smtb_te1() == 0) return XTS_SMT_E_STATE;
  if (!wait_for(c_slot_free, 1, 0, TO_SLOT)) return XTS_SMT_E_BUSY;
  park_tscr = smtb_tscr();
  if (smtb_park1() < 0) return XTS_SMT_E_BUSY;
  if (!wait_for(c_te1, 1, 0, TO_PARK)) {
    smtb_wake1();
    if (wait_for(c_awake, 1, 0, TO_WAKE)) {
      smtb_set_tscr(park_tscr);
      return XTS_SMT_E_PARK_TO;
    }
    th[1].st = XTS_SMT_ST_LOST;
    xts_log("smt: sibling neither parked nor came back: marked lost");
    return XTS_SMT_E_PARK_TO;
  }
  th[1].st = XTS_SMT_ST_PARKED;
  return 0;
}

int xts_smt_wake(void)
{
  if (th[1].st == XTS_SMT_ST_IDLE) return 0;
  if (th[1].st != XTS_SMT_ST_PARKED) return th[1].st == XTS_SMT_ST_LOST ? XTS_SMT_E_LOST : XTS_SMT_E_STATE;
  smtb_wake1();
  if (!wait_for(c_awake, 1, 0, TO_WAKE)) {
    th[1].st = XTS_SMT_ST_LOST;
    xts_log("smt: sibling did not wake (TE1=%d, slot %s): marked lost", smtb_te1(), smtb_slot_busy(1) ? "busy" : "free");
    return XTS_SMT_E_WAKE_TO;
  }
  smtb_set_tscr(park_tscr);    /* the sleeping thread rewrote TSCR[WEXT] (libxenon prepare_sleep) */
  th[1].st = XTS_SMT_ST_IDLE;
  return 0;
}

/* other-core loads: every usable PIR of 2-5, slices of the shared core buffer */
int xts_smt_cores_start(int load, int *pirs_out)
{
  int pirs[4], n = 0;
  for (int p = 2; p < XTS_SMT_NPIR; p++)
    if (!xts_smt_reserved(p)) pirs[n++] = p;
  if (pirs_out) *pirs_out = 0;
  for (int i = 0; i < n; i++) {
    int r = start_ex(pirs[i], load, NULL, 0, i, n);
    if (r < 0) {
      for (int j = 0; j < i; j++) xts_smt_stop(pirs[j]);
      return r;
    }
    if (pirs_out) *pirs_out |= 1 << pirs[i];
  }
  return 0;
}

/* ---- same_insn word selection --------------------------------------------------------------------------------------- */
int xts_smt_same_words(const uint32_t *w, int n, uint32_t *out, int max, int *fallback)
{
  int k = 0, units[8] = { 0 };
  for (int i = 0; i < n; i++) {
    int id = xts_decode1(w[i]);
    if (id < 0) continue;
    const xts_insn_t *d = &xts_isa[id];
    if (d->unit < 8) units[d->unit]++;
    if (d->hw != XTS_HW_EXECUTES || d->priv != XTS_PRIV_USER) continue;
    if (d->unit != XTS_UNIT_INTEGER && d->unit != XTS_UNIT_FPU && d->unit != XTS_UNIT_VECTOR) continue;
    if ((d->implicit_r | d->implicit_w) & (XTS_IMP_LR | XTS_IMP_CTR | XTS_IMP_MEM)) continue;
    int bad = 0;
    for (int o = 0; o < d->nops; o++)
      if (d->ops[o].cls == XTS_OC_BRANCH || d->ops[o].cls == XTS_OC_SPR || d->ops[o].cls == XTS_OC_SR) bad = 1;
    /* or rX,rX,rX: thread-priority / no-op hints (would change the sibling's own priority) */
    if (id == XI_orx && ((w[i] >> 21) & 31) == ((w[i] >> 16) & 31) && ((w[i] >> 21) & 31) == ((w[i] >> 11) & 31)) bad = 1;
    if (bad) continue;
    if (k < max) out[k++] = w[i];
  }
  if (fallback) {
    int best = XTS_UNIT_INTEGER;
    if (units[XTS_UNIT_FPU] > units[best]) best = XTS_UNIT_FPU;
    if (units[XTS_UNIT_VECTOR] > units[best]) best = XTS_UNIT_VECTOR;
    if (units[XTS_UNIT_MEMORY] > units[best]) best = XTS_UNIT_MEMORY;
    *fallback = best == XTS_UNIT_FPU ? XTS_SMT_BUSY_FP : best == XTS_UNIT_VECTOR ? XTS_SMT_BUSY_VMX
              : best == XTS_UNIT_MEMORY ? XTS_SMT_BUSY_LD : XTS_SMT_BUSY_INT;
  }
  return k;
}

static uint64_t hash_words(const uint32_t *w, int n)
{
  uint64_t h = 0x9E37u + (uint64_t)n;
  for (int i = 0; i < n; i++) h = xts_hash_u64(h, w[i]);
  return h;
}

/* ---- exec hooks ----------------------------------------------------------------------------------------------------------- */
static int hook_h = -1;                  /* handle of the chained exec hook (shared by the sibling and other_cores axes) */
static uint32_t pre_sib, pre_cores;
static uint64_t pre_tb, raw_hash;
static int raw_valid;

static int thread_stalled(int pir)
{
  smtb_relax(pir);
  uint32_t it = xts_smt_tcbs[pir].iters;
  uint64_t now = xts_tb();
  if (it != th[pir].last_iters) {
    th[pir].last_iters = it;
    th[pir].last_change_tb = now;
    return 0;
  }
  return now - th[pir].last_change_tb > ms_ticks(TO_STALL);
}

static uint32_t cores_iters(void)
{
  uint32_t s = 0;
  for (int p = 2; p < XTS_SMT_NPIR; p++)
    if (th[p].st == XTS_SMT_ST_RUNNING && th[p].load != XTS_SMT_WATCHDOG) s += xts_smt_tcbs[p].iters;
  return s;
}

/* the run's words (x->run_word / x->run_kernel, set by the exec engine before the pre hook); kernel exits are still the
 * unpatched "b ." placeholder and are dropped (a branch is never kept anyway). Large kernels: the first `max` words. */
static int run_words(const xts_exec_t *x, uint32_t *w, int max)
{
  if (!x->run_kernel) {
    w[0] = x->run_word;
    return 1;
  }
  int n = 0;
  for (uint32_t i = 0; i < x->run_kernel_words && n < max; i++)
    if (x->run_kernel[i] != 0x48000000u) w[n++] = x->run_kernel[i];
  return n;
}

static int same_insn_pre(xts_exec_t *x, uint32_t *aux)
{
  static uint32_t raw[512], keep[48];
  int n = run_words(x, raw, 512);
  if (n <= 0) {                                       /* empty kernel: keep / start a representative kernel */
    if (th[1].st == XTS_SMT_ST_RUNNING) return 0;
    uint32_t rep = xts_enc(XI_addx, 0, 3, 4, 5);
    int r = xts_smt_start(1, XTS_SMT_SAME_INSN, &rep, 1);
    *aux |= XTS_SMT_AUX_RESTARTED;
    return r;
  }
  uint64_t rh = hash_words(raw, n);
  if (raw_valid && rh == raw_hash && th[1].st == XTS_SMT_ST_RUNNING) return 0;
  raw_hash = rh;
  raw_valid = 1;
  int fb = XTS_SMT_BUSY_INT;
  int nk = xts_smt_same_words(raw, n, keep, 48, &fb);
  uint64_t kh = nk ? hash_words(keep, nk) : xts_hash_u64(1, (uint64_t)fb);
  if (th[1].st == XTS_SMT_ST_RUNNING && kh == th[1].same_hash) return 0;
  int r = xts_smt_stop(1);
  if (r < 0) return r;
  r = nk ? xts_smt_start(1, XTS_SMT_SAME_INSN, keep, nk) : xts_smt_start(1, fb, NULL, 0);
  if (r < 0) return r;
  th[1].same_hash = kh;
  *aux |= XTS_SMT_AUX_RESTARTED;
  xts_smt_rs.restarts++;
  if (!nk) {
    *aux |= XTS_SMT_AUX_FALLBACK;
    xts_smt_rs.fallbacks++;
  }
  return 0;
}

static int smt_pre(xts_exec_t *x, void *ud)
{
  (void)ud;
  uint32_t aux = 0;
  int mode = xts_smt_hook_sibling_mode;
  if (mode == XTS_SMT_SAME_INSN) {
    int r = same_insn_pre(x, &aux);
    if (r < 0) {
      snprintf(x->err, sizeof x->err, "smt same_insn: sibling %s", xts_smt_err_name(r));
      xts_smt_rs.refused++;
      x->aux_status = aux | XTS_SMT_AUX_STALLED;
      return -1;
    }
  }
  if (mode == -1) {                                   /* parked: still asleep? */
    if (smtb_te1() != 0) {
      snprintf(x->err, sizeof x->err, "smt: sibling no longer parked (CTRL TE1 set)");
      xts_smt_rs.refused++;
      x->aux_status = XTS_SMT_AUX_STALLED;
      return -1;
    }
    aux |= XTS_SMT_AUX_PARKED;
  } else if (mode) {
    if (th[1].st != XTS_SMT_ST_RUNNING || thread_stalled(1)) {
      snprintf(x->err, sizeof x->err, "smt: sibling load not running (heartbeat stopped)");
      xts_smt_rs.stalls++;
      x->aux_status = aux | XTS_SMT_AUX_STALLED;
      return -1;
    }
    aux |= XTS_SMT_AUX_SIBLING;
  }
  if (xts_smt_hook_cores) {
    for (int p = 2; p < XTS_SMT_NPIR; p++) {
      if (xts_smt_reserved(p)) continue;
      if (th[p].st != XTS_SMT_ST_RUNNING || thread_stalled(p)) {
        snprintf(x->err, sizeof x->err, "smt: core load on PIR %d not running (heartbeat stopped)", p);
        xts_smt_rs.stalls++;
        x->aux_status = aux | XTS_SMT_AUX_STALLED;
        return -1;
      }
    }
    aux |= XTS_SMT_AUX_CORES;
  }
  x->aux_status = aux;
  xts_smt_rs.aux = aux;
  pre_sib = xts_smt_tcbs[1].iters;
  pre_cores = cores_iters();
  pre_tb = xts_tb();
  return 0;
}

static void smt_post(xts_exec_t *x, void *ud)
{
  (void)ud;
  if (x->status == XTS_EXEC_NOTRUN) return;       /* a later hook cancelled the run: nothing was measured */
  uint64_t now = xts_tb();
  smtb_relax(1);
  xts_smt_runstats_t *rs = &xts_smt_rs;
  rs->ticks = (uint32_t)(now - pre_tb);
  rs->sib_iters = (rs->aux & XTS_SMT_AUX_SIBLING) ? xts_smt_tcbs[1].iters - pre_sib : 0;
  rs->cores_iters = (rs->aux & XTS_SMT_AUX_CORES) ? cores_iters() - pre_cores : 0;
  rs->blk_runs++;
  rs->blk_ticks += rs->ticks;
  rs->blk_sib_iters += rs->sib_iters;
  rs->blk_cores_iters += rs->cores_iters;
}

int xts_smt_hooks_install(void)
{
  if (hook_h >= 0 && xts_exec_hook_count() == 0) hook_h = -1;   /* the core dropped it after a block that did not revert */
  if (hook_h >= 0) return 0;
  hook_h = xts_exec_hook_add(smt_pre, smt_post, NULL);
  raw_valid = 0;
  if (hook_h < 0) xts_log("smt: no free exec hook slot");
  return hook_h < 0 ? -1 : 0;
}

void xts_smt_hooks_remove(void)
{
  if (xts_smt_hook_sibling_mode || xts_smt_hook_cores) return;
  if (hook_h >= 0) xts_exec_hook_remove(hook_h);
  hook_h = -1;
}

/* ---- info records ------------------------------------------------------------------------------------------------------- */
int xts_smt_temps_json(xts_json_t *j, const char *key)
{
  uint16_t t[4];
  xj_key(j, key);
  if (xts_smc_temps(t) < 0) { xj_null(j); return -1; }
  xj_obj(j);
  xj_kv_double(j, "cpu", t[0] / 256.0);
  xj_kv_double(j, "gpu", t[1] / 256.0);
  xj_kv_double(j, "edram", t[2] / 256.0);
  xj_kv_double(j, "board", t[3] / 256.0);
  xj_end(j);
  return 0;
}

void xts_smt_info_begin(xts_json_t *j, const char *axis, const char *value)
{
  xj_obj(j);
  xj_kv_str(j, "axis", axis);
  xj_kv_str(j, "value", value);
  xj_kv_str(j, "backend", smtb_name());
  xj_kv_int(j, "speed_code", smtb_speed_get());
  xj_kv_bool(j, "watchdog", wd_running);
  xj_kv_str(j, "load_prio", "medium (libxenon task call, or 2,2,2)");
}

/* a per-block info record (status info, family/block/conditions of the block being run: the core knows them) */
void xts_smt_emit_info(const char *axis, double value, const char *unit, const char *detail)
{
  char cid[48];
  snprintf(cid, sizeof cid, "smt:%s", axis);
  xts_emit_block_info(NULL, cid, "info", value, unit, detail);
}

/* ---- speed ----------------------------------------------------------------------------------------------------------- */
int xts_smt_speed_get(void) { return smtb_speed_get(); }

int xts_smt_speed_set(int code, char *why, int why_size)
{
  xts_smt_init();
  if (!xts_smt_all_idle()) { snprintf(why, why_size, "sibling/other-core threads in use"); return -1; }
  for (int p = 1; p < XTS_SMT_NPIR; p++)
    if (th[p].st == XTS_SMT_ST_LOST) { snprintf(why, why_size, "PIR %d lost earlier", p); return -1; }
  uint32_t wd_ms = wd_timeout_ms;
  int had_wd = wd_running;
  if (had_wd) xts_smt_wd_stop();
  int r = smtb_speed_set(code, why, why_size);
  if (r == 0) cores_state = 1;                     /* the speed change starts and wakes every thread */
  if (had_wd) xts_smt_wd_start(wd_ms);
  return r;
}

/* ---- watchdog --------------------------------------------------------------------------------------------------------- */
int xts_smt_wd_running(void) { return wd_running; }
int xts_smt_wd_fired(void) { smtb_relax(4); return xts_smt_wd.fired != 0; }

void xts_smt_wd_set_timeout(uint32_t ms)
{
  wd_timeout_ms = ms;
  if (!wd_running) return;
  xts_smt_wd.kick = tb10();
  smt_lwsync();
  xts_smt_wd.timeout = (uint32_t)(ms_ticks(ms) >> 10) + 1;
  smt_sync();
}

int xts_smt_wd_start(uint32_t timeout_ms)
{
  xts_smt_init();
  if (wd_running) return 0;
  wd_timeout_ms = timeout_ms;
  xts_smt_wd.fired = 0;
  xts_smt_wd.kick = tb10();
  xts_smt_wd.timeout = (uint32_t)(ms_ticks(timeout_ms) >> 10) + 1;
  smt_sync();
  int r = xts_smt_start(4, XTS_SMT_WATCHDOG, NULL, 0);
  if (r < 0) {
    xts_smt_wd.timeout = 0;
    smt_sync();
    xts_log("smt: watchdog NOT started on PIR 4: %s", xts_smt_err_name(r));
    return r;
  }
  wd_running = 1;
  xts_log("smt: watchdog on PIR 4, timeout %u ms (SMC power reboot -> XeLL reloads xenon.elf -> journal resumes)",
          (unsigned)timeout_ms);
  return 0;
}

void xts_smt_wd_kick(void)
{
  if (!wd_running) return;
  xts_smt_wd.kick = tb10();
  smt_lwsync();
  smtb_relax(4);
}

void xts_smt_wd_stop(void)
{
  if (!wd_running) return;
  xts_smt_wd.timeout = 0;
  smt_sync();
  int r = xts_smt_stop(4);
  wd_running = 0;
  if (r < 0) xts_log("smt: watchdog thread did not stop: %s", xts_smt_err_name(r));
}

#ifdef XTS_WATCHDOG
/* strong definitions over src/core/watchdog.c's weak no-ops (xts/plat.h) (opt-in: CFLAGS_EXTRA=-DXTS_WATCHDOG) */
void xts_watchdog_start(uint32_t timeout_ms) { xts_smt_wd_start(timeout_ms); }
void xts_watchdog_kick(void) { xts_smt_wd_kick(); }
void xts_watchdog_stop(void) { xts_smt_wd_stop(); }
void xts_watchdog_set_timeout(uint32_t timeout_ms) { xts_smt_wd_set_timeout(timeout_ms); }   /* per-block timeout (core/main.c) */
#endif

#ifdef XTS_HOST
/* host tests: forget every thread state (the simulation is reset by smt_host.c) */
void xts_smt_sim_reset(void);
void xts_smt_test_reset(void)
{
  xts_smt_sim_reset();
  memset(th, 0, sizeof th);
  memset(xts_smt_tcbs, 0, sizeof xts_smt_tcbs);
  memset(&xts_smt_wd, 0, sizeof xts_smt_wd);
  memset(&xts_smt_rs, 0, sizeof xts_smt_rs);
  inited = cores_state = wd_running = 0;
  xts_smt_hook_sibling_mode = xts_smt_hook_cores = 0;
  xts_smt_hooks_remove();
  hook_h = -1;
  xts_smt_init();
}
#endif
