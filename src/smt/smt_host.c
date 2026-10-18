/* src/smt host backend: a simulation of libxenon's thread slots, CTRL[TE1] and the (read-only) speed code, driven from smt.c's bounded
 * wait loops (smtb_relax). Load kernels are built exactly as on the console and every word is decoded back (counted in the
 * dry-run statistics; a bad word makes host-dryrun fail). Nothing executes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "smt_int.h"
#include <core/core.h>        /* xts_dry_stats: no public call exists for the dry-run kernel statistics */

#define CODE_WORDS 4096
static uint32_t code[XTS_SMT_NPIR][CODE_WORDS];
static int alive = 0x3, busy[XTS_SMT_NPIR], launched[XTS_SMT_NPIR], parking, te1 = 1, speed = 1;

/* test hooks (host/tests): make a simulated thread unresponsive */
int xts_smt_sim_dead[XTS_SMT_NPIR];
int xts_smt_sim_cores_fail;

const char *smtb_name(void) { return "host-sim"; }
uint32_t smtb_addr(const void *p) { return (uint32_t)(uintptr_t)p; }
int smtb_thread_alive(int pir) { return (alive >> pir) & 1; }
int smtb_slot_busy(int pir) { return busy[pir]; }

int smtb_cores_up(uint32_t timeout_ms)
{
  (void)timeout_ms;
  if (xts_smt_sim_cores_fail) return -1;
  alive = 0x3f;
  return 0;
}

void smtb_relax(int pir)
{
  if (pir < 1 || pir >= XTS_SMT_NPIR || xts_smt_sim_dead[pir]) return;
  xts_smt_tcb_t *t = &xts_smt_tcbs[pir];
  if (pir == 1 && parking) { parking = 0; te1 = 0; return; }
  if (!launched[pir]) return;
  if (t->started != t->gen) { t->started = t->gen; return; }
  if (t->stop) {
    t->exit_why = TCB_EXIT_OK;
    t->done = t->gen;
    launched[pir] = busy[pir] = 0;
    return;
  }
  t->iters++;
}

int smtb_launch(int pir)
{
  if (busy[pir]) return -1;
  busy[pir] = launched[pir] = 1;
  return 0;
}

int smtb_te1(void) { return te1; }
int smtb_park1(void)
{
  if (busy[1]) return -1;
  busy[1] = 1;
  parking = 1;
  return 0;
}
void smtb_wake1(void)
{
  if (xts_smt_sim_dead[1]) return;
  te1 = 1;
  busy[1] = 0;
  parking = 0;
}
uint64_t smtb_tscr(void) { return 0x100000; }
void smtb_set_tscr(uint64_t v) { (void)v; }

uint32_t smtb_install(int pir, xts_kbuf_t *k)
{
  if (pir < 1 || pir >= XTS_SMT_NPIR || k->n > CODE_WORDS) return 0;
  int ni = 0, nr = 0;
  char why[96];
  int bad = xk_validate(k, &ni, &nr, why, sizeof why);
  xts_dry_stats.kernels++;
  xts_dry_stats.kernel_words += k->n;
  xts_dry_stats.kernel_insn_words += (uint64_t)ni;
  xts_dry_stats.kernel_raw_words += (uint64_t)nr;
  if (bad) {
    xts_dry_stats.bad_words += (uint64_t)bad;
    xts_log("smt: load kernel validation failed: %s", why);
    return 0;
  }
  memcpy(code[pir], k->w, k->n * 4);
  return smtb_addr(code[pir]);
}

void smtb_code_range(int pir, uint64_t *lo, uint64_t *hi)
{
  *lo = smtb_addr(code[pir]);
  *hi = *lo + sizeof code[pir];
}

int smtb_speed_get(void) { return speed; }

void *smtb_alloc(uint32_t bytes) { return aligned_alloc(65536, (bytes + 65535u) & ~65535u); }

void xts_smt_sim_reset(void)
{
  alive = 0x3;
  parking = 0;
  te1 = 1;
  speed = 1;
  xts_smt_sim_cores_fail = 0;
  memset(busy, 0, sizeof busy);
  memset(launched, 0, sizeof launched);
  memset(xts_smt_sim_dead, 0, sizeof xts_smt_sim_dead);
}
