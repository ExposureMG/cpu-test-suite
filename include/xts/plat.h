/* XTS - platform layer: src/plat/plat_console.c (libxenon: screen, timebase, SMC, caches) and src/plat/plat_host.c
 * (stderr, monotonic clock as a fake timebase). There is NO storage: the console never opens, writes or mounts anything.
 * Results leave as UDP messages (xts/net.h, docs/PROTOCOL.md); USB is used only by XeLL to load xenon.elf.
 * Core, scheduler and sink use this interface plus xts/net.h and xts/xcore.h, so they run unchanged in the host tests.
 */
#ifndef XTS_PLAT_H
#define XTS_PLAT_H
#include <stddef.h>
#include <stdint.h>

int xts_plat_init(int argc, char **argv);   /* screen (console) / options (host: --quiet). 0 ok */
int xts_plat_is_host(void);
const char *xts_plat_name(void);            /* "console" / "host" */
void xts_plat_screen(const char *line);     /* one message line on screen (console) / stderr (host): errors, halts */
void xts_plat_status(const char *line);     /* the status line: console redraws one fixed screen row (no scrolling,
                                               one cache flush), host stderr; callers rate-limit (xts_status) */
/* final state: the message on screen, then spin (console) / exit(1) (host). Nothing is flushed or unmounted: there is
 * nothing to flush. The caller drains the transport first (xts_net_drain) if it wants the last messages out. */
void xts_plat_halt(const char *why) __attribute__((noreturn));

/* time */
uint64_t xts_tb(void);                      /* timebase */
uint64_t xts_tb_hz(void);                   /* nominal timebase frequency (49.875 MHz on the console) */
void xts_delay_ms(uint32_t ms);

/* SMC (console only; host returns -1). Never blocks forever. */
int xts_smc_version(uint8_t v[3]);          /* query 0x12 */
int xts_smc_temps(uint16_t t[4]);           /* query 0x07: CPU, GPU, eDRAM, board in 1/256 degC units (raw) */
int xts_smc_rtc(uint8_t raw[5]);            /* query 0x04: 5 raw bytes (little-endian counter) */

/* identity (sysinfo.json, carried by the HELLO message): writes platform-specific members into an open JSON object */
struct xts_json;
void xts_plat_sysinfo(struct xts_json *j);
/* "<type>-<pvr>-<xenosid>" e.g. "jasper-00710500-5841"; host: "host". The receiver files runs under this tag. */
void xts_plat_default_tag(char *buf, size_t size);
/* 64-bit SPR read done right on the 32-bit ABI (std of the full register). Console only, host returns 0. */
uint64_t xts_mfspr64(int spr);
/* Lowest address the heap may grow to (big-buffer bound check): keeps big allocations clear of the stacks above. */
uintptr_t xts_plat_heap_limit(void);

/* Watchdog hooks: weak no-ops in src/core/watchdog.c (the SMC-reboot watchdog is gone; a hang of the test core is
 * detected by the suite core's xts_xc_call timeout instead). Kept so existing callers (calib, exec) need no change. */
void xts_watchdog_start(uint32_t timeout_ms);
void xts_watchdog_kick(void);
void xts_watchdog_stop(void);
void xts_watchdog_set_timeout(uint32_t timeout_ms);
#endif
