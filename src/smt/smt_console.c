/* src/smt console backend: libxenon thread facilities.
 *
 * libxenon facts this relies on (startup_from_xell.S, except.S, xenon_power.c):
 *   - each thread's wait slot is wait[2*pir] (task) / wait[2*pir+1] (stack); the `secondary` loop polls it at `or 1,1,1`,
 *     calls the task with bctrl at `or 2,2,2` (32-bit mode, IR/DR on, FP/VMX on, EE off) and clears the slot afterwards;
 *   - xenon_run_thread_task() fails (-1) while the slot is busy and never waits;
 *   - secondary_alive has bit pir set once a thread runs; threads 2-5 start when secondary_lock gets bits 2 and 4
 *     (xenon_thread_startup, which waits forever: reimplemented here with a timeout);
 *   - xenon_sleep_thread(1) queues xenon_thread_sleep on thread 1: prepare_sleep (TSCR[WEXT] rewrite) and `mtspr CTRLWR,0`
 *     -> CTRL[TE1] (bit 22 of CTRLRD on core 0) clears; `mtspr CTRLWR,0xC00000` from thread 0 restarts it through the
 *     0x100 vector, it returns from the task and the slot clears (exactly libxenon's own wakeup_secondary()).
 * The SMT runner code in OpenXenos (sib_park/sib_wake/sib_start) followed the same plan but never ran on hardware: every
 * step here is bounded and verified, and a thread that does not answer is marked lost by smt.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <ppc/atomic.h>
#include <ppc/timebase.h>
#include <time/time.h>
#include <xenon_smc/xenon_smc.h>
#include <xenon_soc/xenon_power.h>
#include <xenon_nand/xenon_config.h>
#include <plat/plat_libxenon.h>   /* xts_lx_wait, secondary_lock/alive, xenon_thread_sleep, xenon_get/set_speed, wakeup_cpus, prepare_sleep */
#include "smt_int.h"

#define CODE_WORDS 4096
#define STACK_BYTES 16384
static uint32_t code[XTS_SMT_NPIR][CODE_WORDS] __attribute__((aligned(128)));
static uint8_t stacks[XTS_SMT_NPIR][STACK_BYTES] __attribute__((aligned(128)));
static volatile uint32_t sleep_mark[XTS_SMT_NPIR][32] __attribute__((aligned(128)));

static inline uint32_t mfpir(void)
{
  uint32_t v;
  __asm__ volatile("mfspr %0, 1023" : "=r"(v));
  return v;
}

static volatile uint64_t *trap_ctx(void)
{
  uint32_t sprg1;
  __asm__ volatile("mfsprg1 %0" : "=r"(sprg1));
  return (volatile uint64_t *)(sprg1 | 0x80000000u);
}

const char *smtb_name(void) { return "libxenon"; }
uint32_t smtb_addr(const void *p) { return (uint32_t)p; }
uint32_t smtb_tb10(void)
{
  uint32_t v;
  __asm__ volatile("mftb %0\n\tsrdi %0, %0, 10" : "=r"(v));   /* mftb reads all 64 bits even in 32-bit mode */
  return v;
}
int smtb_thread_alive(int pir) { return (secondary_alive >> pir) & 1; }
int smtb_slot_busy(int pir) { return xts_lx_wait[pir * 2] != 0; }
void smtb_relax(int pir) { (void)pir; }

static int all_alive(void) { return (secondary_alive & 0x3f) == 0x3f; }

int smtb_cores_up(uint32_t timeout_ms)
{
  if (all_alive()) return 0;
  atomic_clearset(&secondary_lock, 0, 4 | 16);      /* start the primary threads of cores 1 and 2; they wake their siblings */
  uint64_t t0 = mftb();
  while (!all_alive())
    if (tb_diff_msec(mftb(), t0) >= timeout_ms) return -1;
  return 0;
}

/* ---- the worker: runs as a libxenon task on PIR 1-5 ---- */
static void wd_reboot(void)
{
  /* libxenon's SMC send spins until the FIFO is free; if the hung main thread owns it mid-message nothing else helps */
  xenon_smc_power_reboot();
  uint64_t t0 = mftb();
  while (tb_diff_msec(mftb(), t0) < 5000) ;
  xenon_smc_power_reboot();
  for (;;) ;
}

static void wd_loop(xts_smt_tcb_t *t)
{
  __asm__ volatile("or 1,1,1");                      /* low priority: the watchdog must not load core 2 */
  while (!t->stop) {
    t->iters++;
    uint32_t now = smtb_tb10();
    xts_smt_wd.last_check = now;
    uint32_t to = xts_smt_wd.timeout;
    if (to && (uint32_t)(now - xts_smt_wd.kick) > to) {
      xts_smt_wd.fired = 1;
      __asm__ volatile("sync" ::: "memory");
      __asm__ volatile("or 2,2,2");
      wd_reboot();
    }
    uint64_t t0 = mftb();
    while (mftb() - t0 < PPC_TIMEBASE_FREQ / 100 && !t->stop) ;   /* ~10 ms */
  }
  __asm__ volatile("or 2,2,2");
}

static void smt_worker(void)
{
  uint32_t pir = mfpir();
  if (pir >= XTS_SMT_NPIR) return;
  xts_smt_tcb_t *t = &xts_smt_tcbs[pir];
  uint32_t gen = t->gen;
  volatile uint64_t *ctx = trap_ctx();
  ctx[36] = 0;
  ctx[80] = 0;
  __asm__ volatile("sync" ::: "memory");
  t->started = gen;
  __asm__ volatile("sync" ::: "memory");
  if (!t->stop) {
    if (t->load == XTS_SMT_WATCHDOG) wd_loop(t);
    else if (t->code) xts_smt_call(t, t->code);
  }
  t->trap_vector = (uint32_t)ctx[80];
  t->trap_srr0 = (uint32_t)ctx[36];
  t->exit_why = 1;
  __asm__ volatile("sync" ::: "memory");
  t->done = gen;
  __asm__ volatile("sync" ::: "memory");
}

int smtb_launch(int pir)
{
  return xenon_run_thread_task(pir, stacks[pir] + STACK_BYTES - 0x100, (void *)smt_worker) < 0 ? -1 : 0;
}

/* ---- sibling park / wake (core 0 CTRL) ---- */
int smtb_te1(void)
{
  uint32_t v;
  __asm__ volatile("mfspr %0, 136" : "=r"(v));        /* CTRLRD */
  return (v >> 22) & 1;
}
int smtb_park1(void)
{
  if (smtb_slot_busy(1)) return -1;
  xenon_sleep_thread(1);
  return smtb_slot_busy(1) ? 0 : -1;
}
void smtb_wake1(void) { __asm__ volatile("mtspr 152, %0" : : "r"(0xC00000u)); }   /* CTRLWR: TE0|TE1 */
uint64_t smtb_tscr(void)
{
  uint32_t v;
  __asm__ volatile("mfspr %0, 921" : "=r"(v));
  return v;
}
void smtb_set_tscr(uint64_t v) { __asm__ volatile("mtspr 921, %0" : : "r"((uint32_t)v)); }

/* ---- code ---- */
uint32_t smtb_install(int pir, xts_kbuf_t *k)
{
  if (pir < 1 || pir >= XTS_SMT_NPIR || k->n > CODE_WORDS) return 0;
  uint32_t *dst = code[pir];
  for (uint32_t i = 0; i < k->n; i++)
    dst[i] = k->tag[i] == XK_TAG_EXIT ? xts_branch_word((uint32_t)&dst[i], (uint32_t)xts_smt_exit) : k->w[i];
  xts_sync_code(dst, k->n * 4);
  return (uint32_t)dst;
}

void smtb_code_range(int pir, uint64_t *lo, uint64_t *hi)
{
  *lo = (uint32_t)code[pir];
  *hi = (uint32_t)code[pir] + sizeof code[pir];
}

/* ---- speed ---- */
int smtb_speed_get(void) { return xenon_get_speed(); }

static void sleep_task(void)
{
  uint32_t pir = mfpir();
  sleep_mark[pir][0] = 1;
  __asm__ volatile("sync" ::: "memory");
  xenon_thread_sleep();
  sleep_mark[pir][0] = 0;
  __asm__ volatile("sync" ::: "memory");
}

static int marks(int want)
{
  for (int p = 1; p < XTS_SMT_NPIR; p++)
    if ((int)sleep_mark[p][0] != want || (!want && smtb_slot_busy(p))) return 0;
  return 1;
}

static int wait_marks(int want, uint32_t ms)
{
  uint64_t t0 = mftb();
  while (!marks(want))
    if (tb_diff_msec(mftb(), t0) >= ms) return 0;
  return 1;
}

/* xenon_make_it_faster() with every wait bounded (the hardware polls inside xenon_set_speed are libxenon's) */
int smtb_speed_set(int code_, char *why, int why_size)
{
  if (code_ < 1 || code_ > 4) { snprintf(why, why_size, "bad speed code %d", code_); return -1; }
  if (smtb_cores_up(2000) < 0) { snprintf(why, why_size, "threads 2-5 not up"); return -1; }
  for (int p = 1; p < XTS_SMT_NPIR; p++)
    if (smtb_slot_busy(p)) { snprintf(why, why_size, "PIR %d busy", p); return -1; }
  xenon_config_init();
  int delta = xenon_config_get_vid_delta();
  if (delta < 0) { snprintf(why, why_size, "no VID delta in the NAND config"); return -1; }
  uint32_t tscr0 = (uint32_t)smtb_tscr();
  for (int p = 1; p < XTS_SMT_NPIR; p++) {
    sleep_mark[p][0] = 0;
    if (xenon_run_thread_task(p, stacks[p] + STACK_BYTES - 0x100, (void *)sleep_task) < 0) {
      snprintf(why, why_size, "could not queue the sleep task on PIR %d", p);
      wakeup_cpus();
      wait_marks(0, 1000);
      smtb_set_tscr(tscr0);
      return -1;
    }
  }
  uint64_t t0 = mftb();
  int ok = wait_marks(1, 500);
  while (ok && smtb_te1())
    if (tb_diff_msec(mftb(), t0) >= 700) ok = 0;
  if (!ok) {
    snprintf(why, why_size, "threads did not all go to sleep");
    wakeup_cpus();
    wait_marks(0, 1000);
    smtb_set_tscr(tscr0);
    return -1;
  }
  mdelay(2);                                          /* cores 1/2 primaries: past thread_state=1, into cpusleep */
  uint64_t save[6];
  prepare_sleep(save);
  xenon_smc_set_fan_algorithm(1);
  xenon_set_speed(code_, delta);
  wakeup_cpus();
  int back = wait_marks(0, 2000);
  smtb_set_tscr(tscr0);
  if (!back) { snprintf(why, why_size, "threads did not all come back after the speed change"); return -1; }
  if (xenon_get_speed() != code_) { snprintf(why, why_size, "speed code reads %d after setting %d", xenon_get_speed(), code_); return -1; }
  return 0;
}

void *smtb_alloc(uint32_t bytes)
{
  void *p = memalign(65536, bytes);
  if (!p) return NULL;
  if ((uintptr_t)p + bytes > xts_plat_heap_limit()) { free(p); return NULL; }
  return p;
}
