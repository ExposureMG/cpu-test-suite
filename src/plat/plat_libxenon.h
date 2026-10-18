/* libxenon symbols XTS uses that are missing from libxenon's public headers (console builds only).
 * Include as <plat/plat_libxenon.h>. Sources (libxenon tree, read-only): drivers/xenon_soc/xenon_power.c,
 * startup_from_xell.S, drivers/ppc/except.S. Keep the declarations exactly as libxenon defines them. */
#ifndef XTS_PLAT_LIBXENON_H
#define XTS_PLAT_LIBXENON_H
#ifdef XTS_CONSOLE
#include <stdint.h>

/* drivers/xenon_soc/xenon_power.c */
int xenon_get_speed(void);

/* startup_from_xell.S: per-thread task/stack slots (wait[2*pir] task, wait[2*pir+1] stack), polled by the `secondary`
 * loop of PIR 1-5; except.S: the thread start lock (threads 2-5 wait at _start+0x60 for their bit) and the alive mask */
extern volatile int xts_lx_wait[12] __asm__("wait");
extern unsigned int secondary_lock;
extern volatile int secondary_alive;
#endif
#endif
