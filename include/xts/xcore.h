/* XTS - core roles and cross-core execution.
 *
 *   core 0  PIR 0  test executor: runs only what the suite hands it (xts_xc_call); owns the 0x600/0x700 stubs, the
 *                  SPRG1 trap context and the crash path. PIR 1 is its SMT sibling (controlled by the `sibling` axis).
 *   core 1  PIR 2  suite: scheduler, families, model, records. PIR 3 is free (other_cores load).
 *   core 2  PIR 4  transmitter: the only thread touching the NIC (xts/net.h). PIR 5 is free (other_cores load).
 *
 * All cross-core flags and mailboxes live in UNCACHED memory (xts_uc_alloc), one 128-byte line per writer, so the
 * coordination itself allocates no L1/L2 lines. Every wait has a timeout.
 *
 * Host build: no threads - xts_xc_call runs fn directly, xts_xc_measuring() is 0, xts_uc_alloc is aligned malloc.
 */
#ifndef XTS_XCORE_H
#define XTS_XCORE_H
#include <stdint.h>

enum { XTS_PIR_TEST = 0, XTS_PIR_TEST_SIBLING = 1, XTS_PIR_SUITE = 2, XTS_PIR_SUITE_SPARE = 3, XTS_PIR_NET = 4,
       XTS_PIR_NET_SPARE = 5 };

/* Start the roles. Called once from the boot thread (PIR 0) right after the screen is up: starts PIR 4 (transmitter)
 * and PIR 2 (suite main = the existing run driver), then PIR 0 enters the executor loop and never returns.
 * `suite_main` is the suite entry point. Returns only on failure (<0, reason on screen). */
int xts_xc_start_roles(int (*suite_main)(void));

/* Run fn(a, b) on the test core (PIR 0) and wait for it. Before calling fn the executor does sync; isync (so code the
 * suite just wrote and icbi'd is fetched fresh), sets the measuring flag, calls fn, clears the flag, then publishes
 * the result. Returns XTS_XC_OK, XTS_XC_TIMEOUT (no completion within timeout_us: the test core is presumed hung) or
 * XTS_XC_CRASHED (the test core's crash handler fired: see xts_xc_crash_slot(0)). After TIMEOUT/CRASHED the test core
 * is gone for the rest of the run. */
enum { XTS_XC_OK = 0, XTS_XC_TIMEOUT = -1, XTS_XC_CRASHED = -2, XTS_XC_DOWN = -3 };
int xts_xc_call(void (*fn)(void *, void *), void *a, void *b, uint32_t timeout_us);

/* 1 while the test core runs a case or kernel (the transmitter's fence). */
int xts_xc_measuring(void);

/* Heartbeat counters (incremented by each role's main loop; the suite reads them for hang reports). */
uint64_t xts_xc_heartbeat(int pir);

/* Per-PIR crash snapshot written by the crash handler (any PIR, including 0). `valid` is set last. */
typedef struct xts_xc_crash {
  volatile uint32_t valid;
  uint32_t pir, vector;
  uint64_t srr0, srr1, dar, dsisr, lr, ctr, cr, xer;
  uint64_t gpr[32];
  uint64_t tb;
} xts_xc_crash_t;
const xts_xc_crash_t *xts_xc_crash_slot(int pir);

/* Uncached memory: size rounded up to 128 bytes, 128-byte aligned, zeroed. Returns the uncached EA (console: the
 * 0xA0000000/0xB0000000 alias of heap memory). xts_uc_phys gives the physical address for DMA (ea & 0x1FFFFFFF).
 * Never freed. NULL if exhausted. */
void *xts_uc_alloc(uint32_t size);
uint32_t xts_uc_phys(const void *uncached_ea);
#endif
