/* XTS - platform layer: src/plat/plat_console.c (libxenon: USB mass storage + libfat, screen, timebase, SMC, caches) and
 * src/plat/plat_host.c (stdio files under a directory, fake timebase). Core, scheduler, sinks and journal only use this
 * interface plus stdio, so they run unchanged in the host tests.
 *
 * Files: paths are "<root>/..." where xts_plat_root() is "uda0:" on the console (the first USB FAT volume libfat mounted)
 * or the host directory. Use fopen/fwrite/fclose/mkdir on xts_path() results.
 */
#ifndef XTS_PLAT_H
#define XTS_PLAT_H
#include <stddef.h>
#include <stdint.h>

int xts_plat_init(int argc, char **argv);   /* screen, USB, mount. 0 = storage available */
int xts_plat_is_host(void);
const char *xts_plat_root(void);            /* "uda0:" / host dir; "" if no storage */
const char *xts_plat_name(void);            /* "console" / "host" */
void xts_plat_poll(void);                   /* keep USB alive (called between blocks) */
void xts_plat_screen(const char *line);     /* one message line on screen (console) / stderr (host): errors, halts */
void xts_plat_status(const char *line);     /* the status line: console redraws one fixed screen row (no scrolling,
                                               one cache flush), host stderr; callers rate-limit (xts_status) */
void xts_plat_halt(const char *why);        /* final state: message on screen, spin (console) / exit (host) */

/* "<root>/<a>/<b>" into buf (NULL components end the list). Returns buf. */
char *xts_path(char *buf, size_t size, const char *a, const char *b, const char *c, const char *d);
int xts_mkdirs(const char *path);           /* mkdir -p; 0 ok */
int xts_file_exists(const char *path);
/* durable append: open "a", write, fflush + fsync, fclose. 0 ok (the core keeps its run files open instead) */
int xts_append_durable(const char *path, const void *data, size_t n);
/* read a whole small file into buf (NUL-terminated); returns length or -1 */
int xts_read_small(const char *path, char *buf, size_t size);
int xts_write_small(const char *path, const char *text);   /* replace a small file, durable */

/* time */
uint64_t xts_tb(void);                      /* timebase */
uint64_t xts_tb_hz(void);                   /* nominal timebase frequency (49.875 MHz on the console) */
void xts_delay_ms(uint32_t ms);

/* SMC (console only; host returns -1). Never blocks forever. */
int xts_smc_version(uint8_t v[3]);          /* query 0x12 */
int xts_smc_temps(uint16_t t[4]);           /* query 0x07: CPU, GPU, eDRAM, board in 1/256 degC units (raw) */
int xts_smc_rtc(uint8_t raw[5]);            /* query 0x04: 5 raw bytes (little-endian counter) */
void xts_plat_reboot(void);                 /* SMC reboot (watchdog), host: exit(77) */

/* identity (sysinfo.json): writes platform-specific members into an open JSON object */
struct xts_json;
void xts_plat_sysinfo(struct xts_json *j);
/* "<type>-<pvr>-<xenosid>" e.g. "jasper-00710500-5841"; host: "host" */
void xts_plat_default_tag(char *buf, size_t size);
/* 64-bit SPR read done right on the 32-bit ABI (std of the full register). Console only, host returns 0. */
uint64_t xts_mfspr64(int spr);

/* Reserved RAM page for the crash record (never FAT from exception context). Host: a file-backed buffer. */
void *xts_plat_crash_page(size_t *size);
void xts_plat_crash_page_sync(void);        /* host: write the page file; console: dcbf */
/* Lowest address the heap may grow to before reaching the crash page (big-buffer bound check). */
uintptr_t xts_plat_heap_limit(void);

/* watchdog hook (src/smt implements it with -DXTS_WATCHDOG; src/core/watchdog.c has weak no-op defaults). The core kicks
 * after every case and the console exec backend after every execution (timing measurements included); the timeout
 * (time allowed between kicks) is set per block from the family kind: 120 s, mem/smt 600 s (xts_watchdog_set_timeout). */
void xts_watchdog_start(uint32_t timeout_ms);
void xts_watchdog_kick(void);
void xts_watchdog_stop(void);
void xts_watchdog_set_timeout(uint32_t timeout_ms);
#endif
