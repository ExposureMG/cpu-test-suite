/* Core roles, host build: no threads. xts_xc_call runs fn directly (measuring = 1 around it); host tests simulate a
 * crash or hang of the test core from inside fn (xts_host_xc_crash / xts_host_xc_hang: core.h), which makes xts_xc_call
 * return XTS_XC_CRASHED / XTS_XC_TIMEOUT and marks the test core down, as on the console. */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include <xts/xcore.h>
#include <core/core.h>

static jmp_buf *cur;
static int measuring, down;
static xts_xc_crash_t slots[6];
static uint64_t hb[6];

int xts_xc_start_roles(int (*suite_main)(void)) { return suite_main ? suite_main() : -1; }

int xts_xc_call(void (*fn)(void *, void *), void *a, void *b, uint32_t timeout_us)
{
  (void)timeout_us;
  if (down) return XTS_XC_DOWN;
  jmp_buf jb, *prev = cur;
  cur = &jb;
  int r = setjmp(jb);
  if (r == 0) {
    measuring = 1;
    fn(a, b);
    measuring = 0;
    cur = prev;
    hb[XTS_PIR_TEST]++;
    return XTS_XC_OK;
  }
  measuring = 0;
  cur = prev;
  down = 1;
  return r == 1 ? XTS_XC_CRASHED : XTS_XC_TIMEOUT;
}

int xts_xc_measuring(void) { return measuring; }
uint64_t xts_xc_heartbeat(int pir) { return pir >= 0 && pir < 6 ? hb[pir] : 0; }
const xts_xc_crash_t *xts_xc_crash_slot(int pir) { return pir >= 0 && pir < 6 ? &slots[pir] : NULL; }

void *xts_uc_alloc(uint32_t size)
{
  size = (size + 127u) & ~127u;
  void *p = aligned_alloc(128, size ? size : 128);
  if (p) memset(p, 0, size ? size : 128);
  return p;
}
uint32_t xts_uc_phys(const void *p) { return (uint32_t)(uintptr_t)p & 0x1FFFFFFFu; }

void xts_host_xc_crash(uint32_t vector, uint64_t srr0, const uint64_t *gpr32)
{
  xts_xc_crash_t *s = &slots[XTS_PIR_TEST];
  memset(s, 0, sizeof *s);
  s->pir = XTS_PIR_TEST;
  s->vector = vector;
  s->srr0 = srr0;
  s->srr1 = 0x9000000000000000ull | (vector == 0x700 ? 0x20000ull : 0);
  s->lr = srr0 + 4;
  if (gpr32) memcpy(s->gpr, gpr32, sizeof s->gpr);
  s->tb = xts_tb();
  s->valid = 1;
  if (!cur) abort();             /* not inside xts_xc_call: a test bug */
  longjmp(*cur, 1);
}

void xts_host_xc_hang(void)
{
  if (!cur) abort();
  longjmp(*cur, 2);
}

void xts_host_xc_set_slot(int pir, const xts_xc_crash_t *c)
{
  if (pir < 0 || pir >= 6) return;
  if (c) { slots[pir] = *c; slots[pir].pir = (uint32_t)pir; slots[pir].valid = 1; }
  else memset(&slots[pir], 0, sizeof slots[pir]);
}
