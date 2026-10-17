/* src/smt internal: thread control block layout (shared with smt_entry.S) and the backend interface that
 * smt_console.c (libxenon) and smt_host.c (simulation) implement. Not for families: use smt.h. */
#ifndef XTS_SMT_INT_H
#define XTS_SMT_INT_H

/* ---- thread control block: one per PIR, 128-byte lines so no two writers share a line ----------------------------------
 * line 0 CMD      written by thread 0 only (generation, stop flag, load, code address)
 * line 1 STARTED  written by the worker
 * line 2 DONE     written by the worker (done generation, trap vector/SRR0 seen on that thread, exit reason)
 * line 3 ITERS    written by the worker: outer-loop counter = heartbeat
 * line 4 KSTATE   worker private: constants and pointers the load kernels read (written by thread 0 before launch)
 * lines 5..9      worker private: save area of xts_smt_call (C callee-saved state around the generated kernel) */
#define TCB_GEN       0x000
#define TCB_STOP      0x004
#define TCB_LOAD      0x008
#define TCB_CODE      0x00C
#define TCB_STARTED   0x080
#define TCB_DONE      0x100
#define TCB_TRAPVEC   0x104
#define TCB_TRAPSRR0  0x108
#define TCB_EXITWHY   0x10C
#define TCB_ITERS     0x180
#define TCB_K_ONE     0x200   /* double 1.0 */
#define TCB_K_ZERO    0x208   /* double 0.0 */
#define TCB_K_PTR     0x210   /* u32 chase pointer / stream base */
#define TCB_K_END     0x214   /* u32 stream end */
#define TCB_K_L1BUF   0x218   /* u32 address of a small private buffer (busy_ld) */
#define TCB_SV_LR     0x280
#define TCB_SV_R1     0x284
#define TCB_SV_R2     0x288
#define TCB_SV_R13    0x28C
#define TCB_SV_CR     0x290
#define TCB_SV_VRSAVE 0x294
#define TCB_SV_FPSCR  0x298
#define TCB_SV_GPR    0x2A0   /* r14-r31, 8 bytes each (std: keeps the upper halves) */
#define TCB_SV_FPR    0x330   /* f14-f31 */
#define TCB_SV_VSCR   0x3C0
#define TCB_SV_VR     0x3D0   /* v20-v31 */
#define TCB_SIZE      0x500

#define XTS_SMT_NPIR 6

#ifndef __ASSEMBLER__
#include <stdint.h>
#include <xts/xts.h>
#include "smt.h"

typedef struct xts_smt_tcb {
  /* line 0 */
  volatile uint32_t gen, stop, load, code;
  uint32_t pad0[28];
  /* line 1 */
  volatile uint32_t started;
  uint32_t pad1[31];
  /* line 2 */
  volatile uint32_t done, trap_vector, trap_srr0, exit_why;
  uint32_t pad2[28];
  /* line 3 */
  volatile uint32_t iters;
  uint32_t pad3[31];
  /* line 4 */
  double k_one, k_zero;
  uint32_t k_ptr, k_end, k_l1buf;
  uint32_t pad4[25];
  /* lines 5-9 */
  uint8_t save[TCB_SIZE - 0x280];
} __attribute__((aligned(128))) xts_smt_tcb_t;

_Static_assert(sizeof(xts_smt_tcb_t) == TCB_SIZE, "tcb layout");
_Static_assert(__builtin_offsetof(xts_smt_tcb_t, started) == TCB_STARTED, "tcb started");
_Static_assert(__builtin_offsetof(xts_smt_tcb_t, done) == TCB_DONE, "tcb done");
_Static_assert(__builtin_offsetof(xts_smt_tcb_t, iters) == TCB_ITERS, "tcb iters");
_Static_assert(__builtin_offsetof(xts_smt_tcb_t, k_one) == TCB_K_ONE, "tcb k_one");
_Static_assert(__builtin_offsetof(xts_smt_tcb_t, k_l1buf) == TCB_K_L1BUF, "tcb k_l1buf");
_Static_assert(__builtin_offsetof(xts_smt_tcb_t, save) == TCB_SV_LR, "tcb save");

extern xts_smt_tcb_t xts_smt_tcbs[XTS_SMT_NPIR];
extern uint32_t xts_smt_tcbp[XTS_SMT_NPIR];      /* 32-bit address of each tcb (read by smt_entry.S by PIR) */

/* watchdog shared state (each field written by one side only, own line) */
typedef struct xts_smt_wd {
  volatile uint32_t kick;          /* thread 0: tb >> 10 at the last kick */
  uint32_t pad0[31];
  volatile uint32_t timeout;       /* thread 0: timeout in (tb >> 10) units; 0 = disarmed */
  uint32_t pad1[31];
  volatile uint32_t fired;         /* watchdog thread: 1 = timeout seen, reboot requested */
  volatile uint32_t last_check;    /* watchdog thread: tb >> 10 of its last check */
  uint32_t pad2[30];
} __attribute__((aligned(128))) xts_smt_wd_t;
extern xts_smt_wd_t xts_smt_wd;

static inline void smt_sync(void)
{
#ifdef XTS_CONSOLE
  __asm__ volatile("sync" ::: "memory");
#else
  __sync_synchronize();
#endif
}
static inline void smt_lwsync(void)
{
#ifdef XTS_CONSOLE
  __asm__ volatile("lwsync" ::: "memory");
#else
  __sync_synchronize();
#endif
}

/* ---- backend (smt_console.c / smt_host.c) ------------------------------------------------------------------------- */
const char *smtb_name(void);                  /* "libxenon" / "host-sim" */
int smtb_thread_alive(int pir);               /* libxenon's secondary_alive bit for pir */
int smtb_cores_up(uint32_t timeout_ms);       /* start threads 2-5 (libxenon xenon_thread_startup, with a timeout) */
int smtb_slot_busy(int pir);                  /* libxenon wait[] slot holds a task (task running / parked) */
int smtb_launch(int pir);                     /* queue the worker on pir (xenon_run_thread_task); <0 slot busy */
int smtb_te1(void);                           /* CTRL[TE1] of core 0 (thread 1 enabled); -1 unknown */
int smtb_park1(void);                         /* queue xenon_sleep_thread(1); 0 queued */
void smtb_wake1(void);                        /* mtspr CTRLWR 0xC00000 (TE0|TE1) */
uint64_t smtb_tscr(void);                     /* TSCR (per core 0) */
void smtb_set_tscr(uint64_t v);
/* place a finished kernel as the code for pir (exits -> xts_smt_exit); returns its address or 0 */
uint32_t smtb_install(int pir, xts_kbuf_t *k);
void smtb_code_range(int pir, uint64_t *lo, uint64_t *hi);
void smtb_relax(int pir);                     /* host: advance the simulated worker; console: nothing */
uint32_t smtb_addr(const void *p);            /* address as the console code sees it (host: low 32 bits) */
uint32_t smtb_tb10(void);                     /* (timebase >> 10) from ONE 64-bit mftb (no TBU/TBL tearing); watchdog unit */
int smtb_speed_get(void);                     /* libxenon xenon_get_speed() code, 1 = full; host simulated */
int smtb_speed_set(int code, char *why, int why_size);   /* everything must be idle (checked by the caller) */
void *smtb_alloc(uint32_t bytes);              /* 64 KB aligned, below the heap limit; NULL if not possible */
#ifdef XTS_CONSOLE
void xts_smt_call(xts_smt_tcb_t *t, uint32_t code);   /* smt_entry.S: run a generated kernel on the worker's stack */
extern char xts_smt_exit[];                           /* smt_entry.S: where a kernel's exit branches go */
#endif
#endif
#endif
