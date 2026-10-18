/* Core roles and cross-core execution, console (libxenon). Interface: xts/xcore.h; internals: plat/xcore_console.h.
 *
 *   PIR 0  test executor  boot thread; after xts_xc_start_roles it only runs what xts_xc_call hands it
 *   PIR 2  suite          started here as a libxenon thread task (2 MB stack below), runs `suite_main`
 *   PIR 4  transmitter    started here as a libxenon thread task (64 KB stack), runs xts_netc_tx_main (net_console.c)
 * PIR 1/3/5 stay in libxenon's `secondary` wait loop until src/smt gives them a load.
 *
 * Starting threads 2-5: they wait at _start+0x60 until their bit in libxenon's secondary_lock is set; each core's
 * primary (PIR 2, 4) sets its sibling's bit itself (startup_from_xell.S). xenon_thread_startup() waits forever and sets
 * both bits; here each bit is set separately and every wait is bounded. A task is queued by writing libxenon's wait[]
 * slot (stack first, then the entry, with a sync in between: xenon_run_thread_task has no barrier).
 *
 * Uncached memory: one static 8 MB pool in .bss (UC_POOL_BYTES), flushed from the caches once (dcbf) and then used ONLY
 * through the 0xA0000000 alias (libxenon's segment 10/11 map: EA 0xA0000000-0xBFFFFFFF = real 0-0x1FFFFFFF, WIMG
 * cache-inhibited + guarded). Physical = EA & 0x1FFFFFFF (libxenon enet.c's & 0x7FFFFFFF is wrong for this alias).
 * Never dcbz/lwarx/stwcx./unaligned accesses there (alignment interrupt on cache-inhibited storage): only aligned
 * word/doubleword loads and stores through volatile pointers. The pool holds the coordination lines and crash slots
 * (~4 KB), the transmitter's message ring (4 MB) and NIC descriptors/buffers (~50 KB); the rest is spare. Allocations
 * are bump-only and happen on PIR 0 before the roles start or on the suite thread afterwards (never concurrently).
 *
 * Every cross-core flag is its own 128-byte line with one writer. Every wait has a timeout.
 */
#include <stdio.h>
#include <string.h>
#include <ppc/atomic.h>
#include <ppc/cache.h>
#include <xts/xts.h>
#include <xts/xcore.h>
#include <xts/net.h>
#include <plat/plat_libxenon.h>
#include <plat/xcore_console.h>

#define UC_POOL_BYTES (8u << 20)
#define SUITE_STACK_BYTES (2u << 20)
#define NET_STACK_BYTES (64u << 10)
#define START_TIMEOUT_MS 3000
#define TASK_TIMEOUT_MS 1000
#define FENCE_TX_WAIT_US 5000           /* executor: longest wait for the transmitter to finish its frame */

_Static_assert(__builtin_offsetof(xts_xc_crash_t, valid) == XC_CRASH_VALID, "crash slot layout");
_Static_assert(__builtin_offsetof(xts_xc_crash_t, pir) == XC_CRASH_PIR, "crash slot layout");
_Static_assert(__builtin_offsetof(xts_xc_crash_t, vector) == XC_CRASH_VECTOR, "crash slot layout");
_Static_assert(__builtin_offsetof(xts_xc_crash_t, srr0) == XC_CRASH_SRR0, "crash slot layout");
_Static_assert(__builtin_offsetof(xts_xc_crash_t, srr1) == XC_CRASH_SRR1, "crash slot layout");
_Static_assert(__builtin_offsetof(xts_xc_crash_t, dar) == XC_CRASH_DAR, "crash slot layout");
_Static_assert(__builtin_offsetof(xts_xc_crash_t, dsisr) == XC_CRASH_DSISR, "crash slot layout");
_Static_assert(__builtin_offsetof(xts_xc_crash_t, lr) == XC_CRASH_LR, "crash slot layout");
_Static_assert(__builtin_offsetof(xts_xc_crash_t, ctr) == XC_CRASH_CTR, "crash slot layout");
_Static_assert(__builtin_offsetof(xts_xc_crash_t, cr) == XC_CRASH_CR, "crash slot layout");
_Static_assert(__builtin_offsetof(xts_xc_crash_t, xer) == XC_CRASH_XER, "crash slot layout");
_Static_assert(__builtin_offsetof(xts_xc_crash_t, gpr) == XC_CRASH_GPR, "crash slot layout");
_Static_assert(__builtin_offsetof(xts_xc_crash_t, tb) == XC_CRASH_TB, "crash slot layout");
_Static_assert(sizeof(xts_xc_crash_t) <= XC_CRASH_STRIDE, "crash slot size");

/* ---- uncached pool ---- */
static uint8_t uc_pool[UC_POOL_BYTES] __attribute__((aligned(4096)));
static uint32_t uc_used;
static int uc_ready;

static void uc_init(void)
{
  if (uc_ready) return;
  /* the pool was zeroed through the cache by the C runtime: push those lines out and drop them, so no stale or dirty
   * cached copy can ever be written back over what the uncached alias stores later */
  asm volatile("sync" ::: "memory");
  memdcbf(uc_pool, (int)sizeof uc_pool);
  asm volatile("sync" ::: "memory");
  uc_ready = 1;
}

static inline void *uc_alias(void *cached) { return (void *)(((uint32_t)cached & 0x1FFFFFFFu) | 0xA0000000u); }

void *xts_uc_alloc(uint32_t size)
{
  uc_init();
  uint32_t n = (size + 127u) & ~127u;
  if (!n || n > UC_POOL_BYTES - uc_used) return NULL;
  volatile uint64_t *p = uc_alias(uc_pool + uc_used);
  uc_used += n;
  for (uint32_t i = 0; i < n / 8; i++) p[i] = 0;     /* aligned doubleword stores only (no memset: it may dcbz) */
  asm volatile("sync" ::: "memory");
  return (void *)p;
}

uint32_t xts_uc_phys(const void *uncached_ea) { return (uint32_t)uncached_ea & 0x1FFFFFFFu; }

/* ---- coordination lines (each written by one thread only) ---- */
typedef struct xc_line {
  volatile uint32_t v[32];
} xc_line_t;

typedef struct xc_shared {
  xc_line_t req;          /* suite: v[0] seq, v[1] fn, v[2] a, v[3] b */
  xc_line_t done;         /* executor: v[0] seq of the last finished call, v[1] fence_late count, v[2] calls */
  xc_line_t measuring;    /* executor (and PIR 0's crash path): v[0] 1 while a case/kernel runs */
  xc_line_t tx_busy;      /* transmitter: v[0] 1 while a frame is handed to the NIC */
  xc_line_t test_down;    /* suite: v[0] 0 alive / XTS_XC_TIMEOUT / XTS_XC_CRASHED (sticky) */
  xc_line_t suite;        /* suite task: v[0] started, v[1] finished, v[2] return value */
  xc_line_t hb[XC_NPIR];  /* each role: v[0] started, v[2]/v[3] heartbeat counter (u64, big-endian halves) */
} xc_shared_t;

static xc_shared_t *xs;
static uint8_t *crash_base;
uint32_t xts_xc_crash_ea[8];
static uint32_t ctx0_real;
static int roles_started;           /* suite side: the executor loop runs on PIR 0 */
static uint32_t req_seq;            /* suite side */
static int test_status;             /* suite side mirror of xs->test_down */

/* single-core bring-up profiles (net_hello) define a strong xts_profile_single_core = 1 */
extern const int xts_profile_single_core __attribute__((weak));
int xts_xc_single_core(void) { return &xts_profile_single_core && xts_profile_single_core; }

void xts_xc_init_early(void)
{
  if (xs) return;
  uc_init();
  xs = xts_uc_alloc(sizeof *xs);
  crash_base = xts_uc_alloc(XC_CRASH_STRIDE * XC_NPIR);
  if (!xs || !crash_base) return;   /* cannot happen with an 8 MB pool; the API then degrades to direct calls */
  if (xts_xc_mypir() == 0) {
    uint32_t sprg1;
    asm volatile("mfsprg1 %0" : "=r"(sprg1));
    ctx0_real = sprg1;
  }
  for (int p = 0; p < XC_NPIR; p++) xts_xc_crash_ea[p] = (uint32_t)(crash_base + p * XC_CRASH_STRIDE);
  asm volatile("sync" ::: "memory");
}

/* fallback (not on PIR 0 at init): libxenon gives each PIR 0x40000 bytes of HV context below __libc_stack_end,
 * PIR 0's at the top: ctx(pir) = ctx(self) + (self - pir) * 0x40000 (startup_from_xell.S) */
uint32_t xts_xc_test_ctx_real(void)
{
  if (ctx0_real) return ctx0_real;
  uint32_t sprg1;
  asm volatile("mfsprg1 %0" : "=r"(sprg1));
  return sprg1 + xts_xc_mypir() * 0x40000u;
}

const xts_xc_crash_t *xts_xc_crash_slot(int pir)
{
  static const xts_xc_crash_t none;
  if (!crash_base || pir < 0 || pir >= XC_NPIR) return &none;
  return (const xts_xc_crash_t *)(crash_base + pir * XC_CRASH_STRIDE);
}

void xts_xc_on_crash(uint32_t pir)
{
  if (pir == 0 && xs) {
    xs->measuring.v[0] = 0;
    asm volatile("sync" ::: "memory");
  }
}

void xts_xc_beat(int pir)
{
  if (!xs || pir < 0 || pir >= XC_NPIR) return;
  volatile uint32_t *h = xs->hb[pir].v;
  uint32_t lo = h[3] + 1;
  if (!lo) h[2] = h[2] + 1;
  h[3] = lo;
}

uint64_t xts_xc_heartbeat(int pir)
{
  if (!xs || pir < 0 || pir >= XC_NPIR) return 0;
  volatile uint32_t *h = xs->hb[pir].v;
  uint32_t hi, lo;
  do { hi = h[2]; lo = h[3]; } while (hi != h[2]);
  return ((uint64_t)hi << 32) | lo;
}

int xts_xc_measuring(void) { return xs ? (int)xs->measuring.v[0] : 0; }
int xts_xc_test_down(void) { return xs ? xs->test_down.v[0] != 0 : 0; }

int xts_xc_fence_enter(void)
{
  if (!xs) return 1;
  xs->tx_busy.v[0] = 1;
  asm volatile("sync" ::: "memory");
  if (!xs->measuring.v[0] || xs->test_down.v[0]) return 1;
  xs->tx_busy.v[0] = 0;
  asm volatile("sync" ::: "memory");
  return 0;
}

void xts_xc_fence_leave(void)
{
  if (!xs) return;
  asm volatile("sync" ::: "memory");
  xs->tx_busy.v[0] = 0;
  asm volatile("sync" ::: "memory");
}

/* ---- the executor (PIR 0) ---- */
static void run_measured(void (*fn)(void *, void *), void *a, void *b)
{
  xs->measuring.v[0] = 1;
  asm volatile("sync" ::: "memory");
  if (xs->tx_busy.v[0]) {                                   /* a frame is being handed to the NIC: let it finish */
    uint64_t t0 = xts_xc_tb(), lim = xts_xc_us_ticks(FENCE_TX_WAIT_US);
    while (xs->tx_busy.v[0])
      if (xts_xc_tb() - t0 > lim) { xs->done.v[1] = xs->done.v[1] + 1; break; }   /* fence_late (recorded) */
  }
  asm volatile("sync\n\tisync" ::: "memory");             /* code the suite wrote (dcbst + icbi there) is fetched fresh */
  fn(a, b);
  asm volatile("sync" ::: "memory");
  xs->measuring.v[0] = 0;
}

static void executor_loop(void) __attribute__((noreturn));
static void executor_loop(void)
{
  uint32_t last = xs->req.v[0];
  uint64_t beat_tb = 0;
  xts_xc_beat(0);
  for (;;) {
    xts_xc_prio_low();                                       /* idle: give the sibling (PIR 1) the core */
    uint32_t seq;
    while ((seq = xs->req.v[0]) == last) {
      xts_xc_spin_ticks(16);                                 /* ~0.3 us between uncached polls */
      uint64_t now = xts_xc_tb();
      if (now - beat_tb > xts_xc_us_ticks(1000)) { beat_tb = now; xts_xc_beat(0); }
    }
    asm volatile("sync" ::: "memory");                       /* req fields were written before req.seq */
    void (*fn)(void *, void *) = (void (*)(void *, void *))xs->req.v[1];
    void *a = (void *)xs->req.v[2], *b = (void *)xs->req.v[3];
    xts_xc_prio_medium();                                    /* cases run at medium priority (the boot default) */
    run_measured(fn, a, b);
    xs->done.v[2] = xs->done.v[2] + 1;
    xs->done.v[0] = seq;
    asm volatile("sync" ::: "memory");
    last = seq;
    xts_xc_beat(0);
  }
}

int xts_xc_call(void (*fn)(void *, void *), void *a, void *b, uint32_t timeout_us)
{
  if (!fn) return XTS_XC_OK;
  if (!xs || !roles_started || xts_xc_mypir() == 0) {       /* before the roles / single-core mode: run it here */
    if (!xs) { fn(a, b); return XTS_XC_OK; }
    xts_xc_prio_medium();
    run_measured(fn, a, b);
    return XTS_XC_OK;
  }
  if (test_status) return XTS_XC_DOWN;
  xts_xc_beat(XTS_PIR_SUITE);
  uint32_t seq = ++req_seq;
  xs->req.v[1] = (uint32_t)fn;
  xs->req.v[2] = (uint32_t)a;
  xs->req.v[3] = (uint32_t)b;
  asm volatile("sync" ::: "memory");                       /* everything the suite wrote (data, code) before the seq */
  xs->req.v[0] = seq;
  asm volatile("sync" ::: "memory");
  const volatile uint32_t *crash0 = (const volatile uint32_t *)(crash_base + XC_CRASH_VALID);
  uint64_t t0 = xts_xc_tb(), lim = xts_xc_us_ticks(timeout_us), fast = xts_xc_us_ticks(20);
  int r = XTS_XC_OK;
  xts_xc_prio_low();                                         /* the suite is idle while the test core works */
  for (uint32_t n = 0;; n++) {
    if (xs->done.v[0] == seq) break;
    uint64_t dt = xts_xc_tb() - t0;
    if (dt > lim) { r = XTS_XC_TIMEOUT; break; }
    if ((n & 15) == 15 && *crash0) { r = XTS_XC_CRASHED; break; }
    /* first 20 us: poll every ~0.3 us (no-op calls return fast); then every ~5 us so the uncached reads add almost
     * no bus traffic to a long measurement */
    xts_xc_spin_ticks(dt < fast ? 16 : 250);
  }
  xts_xc_prio_medium();
  if (r != XTS_XC_OK && xs->done.v[0] == seq && !*crash0) r = XTS_XC_OK;   /* finished right at the deadline */
  if (r != XTS_XC_OK) {
    if (*crash0) r = XTS_XC_CRASHED;
    test_status = r;
    xs->test_down.v[0] = (uint32_t)r;                        /* the transmitter stops honouring the fence */
  }
  asm volatile("sync" ::: "memory");                         /* results written by fn are visible after done.seq */
  return r;
}

/* ---- starting threads 2-5 and the role tasks ---- */
static uint8_t suite_stack[SUITE_STACK_BYTES] __attribute__((aligned(128)));
static uint8_t net_stack[NET_STACK_BYTES] __attribute__((aligned(128)));
static int (*suite_entry)(void);

static int wait_ms(int (*cond)(uint32_t), uint32_t arg, uint32_t ms)
{
  uint64_t t0 = xts_xc_tb(), lim = xts_xc_us_ticks((uint64_t)ms * 1000u);
  for (;;) {
    if (cond(arg)) return 1;
    if (xts_xc_tb() - t0 > lim) return cond(arg);
    xts_xc_spin_ticks(500);
  }
}
static int c_alive(uint32_t mask) { return ((uint32_t)secondary_alive & mask) == mask; }
static int c_slot_free(uint32_t pir) { return xts_lx_wait[pir * 2] == 0; }
static int c_started(uint32_t pir) { return xs->hb[pir].v[0] != 0; }

static int run_task(int pir, uint8_t *stack, uint32_t stack_bytes, void (*task)(void))
{
  if (!wait_ms(c_slot_free, (uint32_t)pir, TASK_TIMEOUT_MS)) return -1;
  xts_lx_wait[pir * 2 + 1] = (int)(stack + stack_bytes - 0x100);
  asm volatile("sync" ::: "memory");
  xts_lx_wait[pir * 2] = (int)task;
  asm volatile("sync" ::: "memory");
  return wait_ms(c_started, (uint32_t)pir, TASK_TIMEOUT_MS) ? 0 : -2;
}

static void net_task(void)
{
  xs->hb[XTS_PIR_NET].v[0] = 1;
  asm volatile("sync" ::: "memory");
  xts_netc_tx_main();
}

static void suite_task(void)
{
  xs->hb[XTS_PIR_SUITE].v[0] = 1;
  xs->suite.v[0] = 1;
  asm volatile("sync" ::: "memory");
  int r = suite_entry();
  xs->suite.v[2] = (uint32_t)r;
  xs->suite.v[1] = 1;
  asm volatile("sync" ::: "memory");
  char msg[96];
  snprintf(msg, sizeof msg, "XTS: suite finished (%d); power off when done", r);
  xts_plat_screen(msg);
  for (;;) {                                                 /* never back into libxenon's loop: nothing more to run */
    xts_xc_prio_low();
    xts_xc_spin_ticks(50000);
    xts_xc_beat(XTS_PIR_SUITE);
  }
}

static void say(const char *fmt, int a, int b)
{
  char m[128];
  snprintf(m, sizeof m, fmt, a, b);
  xts_plat_screen(m);
}

int xts_xc_start_roles(int (*suite_main)(void))
{
  xts_xc_init_early();
  if (!xs) { xts_plat_screen("XTS: no uncached memory for the core runtime"); return -1; }
  if (xts_xc_mypir() != 0) { xts_plat_screen("XTS: xts_xc_start_roles must run on PIR 0"); return -1; }
  int single = xts_xc_single_core();
  /* threads 2-5: core 2 (transmitter) always, core 1 (suite) unless single-core; each primary wakes its sibling */
  uint32_t want = single ? 0x33u : 0x3Fu;
  if (!c_alive(want)) {
    atomic_clearset(&secondary_lock, 0, single ? 16u : (4u | 16u));
    if (!wait_ms(c_alive, want, START_TIMEOUT_MS)) {
      say("XTS: hardware threads did not start (alive mask %02X, wanted %02X)", secondary_alive & 0x3F, (int)want);
      return -2;
    }
  }
  int r = run_task(XTS_PIR_NET, net_stack, sizeof net_stack, net_task);
  if (r < 0) {
    say("XTS: transmitter thread (PIR %d) did not start (%d)", XTS_PIR_NET, r);
    return -3;
  }
  xts_plat_screen(single ? "XTS: roles: PIR 0 test+suite, PIR 4 transmitter (single-core bring-up)"
                         : "XTS: roles: PIR 0 test executor, PIR 2 suite, PIR 4 transmitter");
  if (single) {
    xts_xc_beat(0);
    return suite_main();
  }
  suite_entry = suite_main;
  roles_started = 1;
  asm volatile("sync" ::: "memory");
  r = run_task(XTS_PIR_SUITE, suite_stack, sizeof suite_stack, suite_task);
  if (r < 0) {
    roles_started = 0;
    say("XTS: suite thread (PIR %d) did not start (%d)", XTS_PIR_SUITE, r);
    return -4;
  }
  executor_loop();
}
