/* src/smt console backend: libxenon thread facilities.
 *
 * libxenon facts this relies on (startup_from_xell.S, except.S, xenon_power.c):
 *   - each thread's wait slot is wait[2*pir] (task) / wait[2*pir+1] (stack); the `secondary` loop polls it at `or 1,1,1`,
 *     calls the task with bctrl at `or 2,2,2` (32-bit mode, IR/DR on, FP/VMX on, EE off) and clears the slot afterwards;
 *   - secondary_alive has bit pir set once a thread runs (threads 2-5 are started by xts_xc_start_roles, src/plat);
 *   - xenon_sleep_thread(1) queues xenon_thread_sleep on thread 1: prepare_sleep (TSCR[WEXT] rewrite) and `mtspr CTRLWR,0`
 *     -> CTRL[TE1] (bit 22 of CTRLRD on core 0) clears; `mtspr CTRLWR,0xC00000` from thread 0 restarts it through the
 *     0x100 vector, it returns from the task and the slot clears (exactly libxenon's own wakeup_secondary()).
 * CTRL and TSCR are per core: this code runs on the suite core (PIR 2), so every CTRL/TSCR access is executed on the
 * test core (PIR 0) through xts_core_xc_call (a lost test core does not return from it).
 * Load tasks are queued like xenon_run_thread_task, but with a sync between the stack and the entry (libxenon has none).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <ppc/timebase.h>
#include <xenon_soc/xenon_power.h>
#include <plat/plat_libxenon.h>   /* xts_lx_wait, secondary_alive, xenon_get_speed */
#include <plat/xcore_console.h>   /* xts_xc_mypir, xts_xc_tb */
#include <core/core.h>            /* xts_core_xc_call */
#include "smt_int.h"

#define CODE_WORDS 4096
#define STACK_BYTES 16384
static uint32_t code[XTS_SMT_NPIR][CODE_WORDS] __attribute__((aligned(128)));
static uint8_t stacks[XTS_SMT_NPIR][STACK_BYTES] __attribute__((aligned(128)));

static volatile uint64_t *trap_ctx(void)
{
  uint32_t sprg1;
  __asm__ volatile("mfsprg1 %0" : "=r"(sprg1));
  return (volatile uint64_t *)(sprg1 | 0x80000000u);
}

const char *smtb_name(void) { return "libxenon"; }
uint32_t smtb_addr(const void *p) { return (uint32_t)p; }
int smtb_thread_alive(int pir) { return (secondary_alive >> pir) & 1; }
int smtb_slot_busy(int pir) { return xts_lx_wait[pir * 2] != 0; }
void smtb_relax(int pir) { (void)pir; }

static int all_alive(void) { return (secondary_alive & 0x3f) == 0x3f; }

int smtb_cores_up(uint32_t timeout_ms)
{
  uint64_t t0 = xts_xc_tb(), lim = xts_xc_us_ticks((uint64_t)timeout_ms * 1000u);
  while (!all_alive())
    if (xts_xc_tb() - t0 > lim) return -1;    /* single-core bring-up: cores 1/2 are not (all) started */
  return 0;
}

/* ---- the worker: runs as a libxenon task on PIR 1, 3, 5 ---- */
static void smt_worker(void)
{
  uint32_t pir = xts_xc_mypir();
  if (pir >= XTS_SMT_NPIR) return;
  xts_smt_tcb_t *t = &xts_smt_tcbs[pir];
  uint32_t gen = t->gen;
  volatile uint64_t *ctx = trap_ctx();
  ctx[36] = 0;
  ctx[80] = 0;
  __asm__ volatile("sync" ::: "memory");
  t->started = gen;
  __asm__ volatile("sync" ::: "memory");
  if (!t->stop && t->code) xts_smt_call(t, t->code);
  t->trap_vector = (uint32_t)ctx[80];
  t->trap_srr0 = (uint32_t)ctx[36];
  t->exit_why = TCB_EXIT_OK;
  __asm__ volatile("sync" ::: "memory");
  t->done = gen;
  __asm__ volatile("sync" ::: "memory");
}

int smtb_launch(int pir)
{
  if (pir < 1 || pir >= XTS_SMT_NPIR || xts_lx_wait[pir * 2]) return -1;
  xts_lx_wait[pir * 2 + 1] = (int)(stacks[pir] + STACK_BYTES - 0x100);
  __asm__ volatile("sync" ::: "memory");
  xts_lx_wait[pir * 2] = (int)smt_worker;
  __asm__ volatile("sync" ::: "memory");
  return 0;
}

/* ---- core 0 state: CTRL (sibling park / wake) and TSCR, executed on the test core ---- */
static void te1_job(void *a, void *b)
{
  uint32_t v;
  (void)b;
  __asm__ volatile("mfspr %0, 136" : "=r"(v));        /* CTRLRD */
  *(int *)a = (int)((v >> 22) & 1);
}
static void wake1_job(void *a, void *b)
{
  (void)a; (void)b;
  __asm__ volatile("mtspr 152, %0" : : "r"(0xC00000u)); /* CTRLWR: TE0|TE1 */
}
static void tscr_get_job(void *a, void *b)
{
  uint32_t v;
  (void)b;
  __asm__ volatile("mfspr %0, 921" : "=r"(v));
  *(uint32_t *)a = v;
}
static void tscr_set_job(void *a, void *b)
{
  (void)b;
  __asm__ volatile("mtspr 921, %0" : : "r"((uint32_t)a));
}
static void on_core0(void (*fn)(void *, void *), void *a)
{
  if (xts_xc_mypir() < 2) fn(a, NULL);
  else xts_core_xc_call(fn, a, NULL);
}

int smtb_te1(void)
{
  int v = -1;
  on_core0(te1_job, &v);
  return v;
}
int smtb_park1(void)
{
  if (smtb_slot_busy(1)) return -1;
  xenon_sleep_thread(1);
  __asm__ volatile("sync" ::: "memory");
  return smtb_slot_busy(1) ? 0 : -1;
}
void smtb_wake1(void) { on_core0(wake1_job, NULL); }
uint64_t smtb_tscr(void)
{
  uint32_t v = 0;
  on_core0(tscr_get_job, &v);
  return v;
}
void smtb_set_tscr(uint64_t v) { on_core0(tscr_set_job, (void *)(uint32_t)v); }

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

/* ---- speed (read only) ---- */
int smtb_speed_get(void) { return xenon_get_speed(); }

void *smtb_alloc(uint32_t bytes)
{
  void *p = memalign(65536, bytes);
  if (!p) return NULL;
  if ((uintptr_t)p + bytes > xts_plat_heap_limit()) { free(p); return NULL; }
  return p;
}
