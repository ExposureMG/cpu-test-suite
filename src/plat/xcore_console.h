/* Console core runtime: internals shared by src/plat/xcore_console.c, net_console.c, plat_console.c, main_console.c,
 * src/exec/exec_console.c and the crash path in src/exec/vec.S (which includes this file as "../plat/xcore_console.h").
 * Public interface: xts/xcore.h, xts/net.h. Console only.
 *
 * Crash slot layout (xts_xc_crash_t, xts/xcore.h): the offsets below are what vec.S's crashdump writes; xcore_console.c
 * checks them against the C struct with _Static_assert. Slots live in uncached memory, one per PIR, XC_CRASH_STRIDE
 * bytes apart; xts_xc_crash_ea[pir] holds each slot's uncached EA (0 = not armed yet: the crash path skips the slot).
 */
#ifndef XTS_XCORE_CONSOLE_H
#define XTS_XCORE_CONSOLE_H

#define XC_CRASH_VALID  0
#define XC_CRASH_PIR    4
#define XC_CRASH_VECTOR 8
#define XC_CRASH_SRR0   16
#define XC_CRASH_SRR1   24
#define XC_CRASH_DAR    32
#define XC_CRASH_DSISR  40
#define XC_CRASH_LR     48
#define XC_CRASH_CTR    56
#define XC_CRASH_CR     64
#define XC_CRASH_XER    72
#define XC_CRASH_GPR    80
#define XC_CRASH_TB     336
#define XC_CRASH_STRIDE 384
#define XC_NPIR 6

#ifndef __ASSEMBLER__
#include <stdint.h>

extern uint32_t xts_xc_crash_ea[8];     /* uncached EA of each PIR's crash slot (read by vec.S crashdump) */

/* Boot (PIR 0), before anything else that might crash or allocate: uncached pool, crash slots, the coordination lines,
 * PIR 0's HV context address. Idempotent. */
void xts_xc_init_early(void);
/* Real address of PIR 0's libxenon HV context block (its SPRG1 at boot): the 0x600/0x700 stubs record in-case traps
 * there. exec_console.c reads it through the cached 0x80000000 alias. */
uint32_t xts_xc_test_ctx_real(void);
/* Crash path (C part, emergency stack): the slot is already written by vec.S. PIR 0: clears its measuring flag so the
 * transmitter is not fenced by a dead test core. Touches nothing else. */
void xts_xc_on_crash(uint32_t pir);

/* Transmitter side of the measurement fence (Dekker style, both flags uncached):
 *   executor: measuring = 1; sync; wait (bounded) while tx_busy
 *   transmitter: tx_busy = 1; sync; if measuring -> tx_busy = 0 and wait; else hand ONE frame to the NIC, wait (bounded)
 *                until the NIC released its descriptor, tx_busy = 0.
 * xts_xc_fence_enter returns 1 when the transmitter may start a frame now (tx_busy is then 1), 0 when a measurement is
 * running (tx_busy is 0 again). The fence is ignored once the test core is down (crashed / hung: nothing to protect). */
int xts_xc_fence_enter(void);
void xts_xc_fence_leave(void);
int xts_xc_test_down(void);             /* 1 after xts_xc_call returned TIMEOUT/CRASHED (sticky) */
void xts_xc_beat(int pir);              /* heartbeat of a role (uncached store) */
int xts_xc_single_core(void);           /* 1: the profile asked for PIR 0 + transmitter only (net_hello) */

static inline uint32_t xts_xc_mypir(void)
{
  uint32_t v;
  __asm__ volatile("mfspr %0, 1023" : "=r"(v));
  return v;
}
/* thread priority nops (Power ISA: or 1,1,1 low, or 2,2,2 medium) */
static inline void xts_xc_prio_low(void) { __asm__ volatile("or 1,1,1" ::: "memory"); }
static inline void xts_xc_prio_medium(void) { __asm__ volatile("or 2,2,2" ::: "memory"); }
/* ONE 64-bit mftb (no TBU/TBL tearing, unlike libxenon's mftb(): a torn read near a TBL wrap jumps 86 s and would
 * fire every timeout at once); in 32-bit mode the GPR still holds all 64 bits */
static inline uint64_t xts_xc_tb(void)
{
  uint32_t lo, hi;
  __asm__ volatile("mftb %0\n\tsrdi %1, %0, 32" : "=r"(lo), "=r"(hi));
  return ((uint64_t)hi << 32) | lo;
}
#define XC_TB_PER_US 49u                /* 49.875 MHz timebase (PPC_TIMEBASE_FREQ): conservative for short spins */
static inline uint64_t xts_xc_us_ticks(uint64_t us) { return us * 49875u / 1000u; }
/* spin about `ticks` timebase ticks (~20 ns each) without touching memory beyond the stack */
static inline void xts_xc_spin_ticks(uint32_t ticks)
{
  uint64_t t0 = xts_xc_tb();
  while (xts_xc_tb() - t0 < ticks) ;
}

/* ---- network (net_console.c) ---- */
void xts_netc_quiesce(void);            /* stop the NIC's DMA (XeLL may have left RX/TX running); no waits */
void xts_netc_tx_main(void) __attribute__((noreturn));   /* transmitter loop, run on PIR 4 by xts_xc_start_roles */
int xts_netc_ready(void);               /* NIC up and TX ring initialised */
void xts_netc_mac(uint8_t mac[6]);
#endif
#endif
