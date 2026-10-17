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
/* final state: if storage is mounted, healthy and no file operation is in progress: fatFlush + fatUnmount, then the
 * message and "stick unmounted: safe to power off" (or why not) on screen; spin (console) / exit(1) (host) */
void xts_plat_halt(const char *why);

/* "<root>/<a>/<b>" into buf (NULL components end the list). Returns buf. */
char *xts_path(char *buf, size_t size, const char *a, const char *b, const char *c, const char *d);
int xts_mkdirs(const char *path);           /* mkdir -p; 0 ok */
int xts_file_exists(const char *path);
/* durable append: open "a", write, fflush + fsync, fclose. 0 ok (the core keeps its run files open instead) */
int xts_append_durable(const char *path, const void *data, size_t n);
/* read a whole small file into buf (NUL-terminated); returns length or -1 */
int xts_read_small(const char *path, char *buf, size_t size);
int xts_write_small(const char *path, const char *text);   /* replace a small file, durable */

/* Storage health. On the console (and the host in image mode, XTS_HOST_STORAGE=image:<file>) libfat reaches the
 * device through the disc guard (src/plat/disc_guard.c): every read is done twice and compared, every write is read
 * back and compared. The first failure latches "poisoned": no further write reaches the device (reads still work).
 * The core then stops writing (sinks, journal) and stops the run; the screen says STORAGE ERROR. */
typedef struct xts_storage_status {
  int guard;                                /* 1 = disc guard verifies libfat's I/O; 0 = pass-through / host directory */
  int poisoned;                             /* 1 = a storage error was latched: writes are refused from now on */
  char op[8];                               /* first error: "read", "write" or "verify" */
  uint32_t sector, count;                   /* its sector range (device sectors) */
  char why[112];                            /* what went wrong */
  uint64_t sectors_read, sectors_written;   /* sectors libfat read / wrote through the guard */
  uint64_t device_reads, device_writes;     /* device calls (verification included) */
  uint64_t read_retries;                    /* reads whose first double read disagreed but the retry agreed */
  uint64_t writes_refused;                  /* writes refused after the latch */
  uint64_t write_repairs, write_repaired;   /* failed writes rewritten once (before latching) / of those, verified */
  double guard_ms;                          /* time spent in the guard, device time included */
} xts_storage_status_t;
int xts_plat_storage_status(xts_storage_status_t *st);   /* fills st (may be NULL); returns poisoned (0/1) */

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

/* Storage boundaries (src/core/main.c): the stick is consistent whenever test code runs. */
int xts_plat_storage_flush(void);           /* fatFlush: every open file, the cache (FAT copies, directories, data),
                                               FSInfo. 0 ok; host directory: 0 */
/* Final: fatFlush + fatUnmount (no file I/O afterwards; xts_plat_root() becomes ""). 0 = unmounted cleanly, -1 = not
 * mounted / refused (poisoned, a file operation in progress), -2 = the flush reported an error (unmounted anyway). */
int xts_plat_storage_unmount(void);
/* Crash-path safe (no stdio, no malloc): */
int xts_plat_storage_busy(void);            /* 1 = the USB driver / disc guard is inside an operation (do not touch) */
int xts_plat_storage_poisoned(void);        /* the disc guard latched an error (no writes) */
uint32_t xts_plat_sector_size(void);        /* device sector size (512..4096); host directory: 512 */

/* Raw sector access to pre-allocated report files (crash.json, progress.bin; src/core/slot.c). `first` is the file's
 * first device sector from xts_plat_file_extent (console / host image: absolute device sector, written through the disc
 * guard's verified raw API; host directory: unused, the file at `path` is read/written at idx * 512 instead).
 *   xts_plat_file_extent  0 = one contiguous run, *first / *nsec set; -2 = not contiguous; -1 = unknown file / no API.
 *                         Only while healthy, after xts_plat_storage_flush, with the file closed.
 *   xts_plat_raw_read     one sector, double-read verified. 0 or a negative XTS_DISC_* code.
 *   xts_plat_raw_write    one sector, only if it currently holds `magic` at `magic_off` and `buf` does too; read back.
 *                         0 or a negative XTS_DISC_* code (src/plat/disc_guard.h). Crash-path safe on the console. */
int xts_plat_file_extent(const char *path, uint32_t *first, uint32_t *nsec);
int xts_plat_raw_read(const char *path, uint32_t first, uint32_t idx, void *buf);
int xts_plat_raw_write(const char *path, uint32_t first, uint32_t idx, const void *buf, const void *magic, uint32_t magic_len,
                       uint32_t magic_off);
/* Lowest address the heap may grow to (big-buffer bound check): keeps big allocations clear of the stacks above. */
uintptr_t xts_plat_heap_limit(void);

/* watchdog hook (src/smt implements it with -DXTS_WATCHDOG; src/core/watchdog.c has weak no-op defaults). The core kicks
 * after every case and the console exec backend after every execution (timing measurements included); the timeout
 * (time allowed between kicks) is set per block from the family kind: 120 s, mem/smt 600 s (xts_watchdog_set_timeout). */
void xts_watchdog_start(uint32_t timeout_ms);
void xts_watchdog_kick(void);
void xts_watchdog_stop(void);
void xts_watchdog_set_timeout(uint32_t timeout_ms);
#endif
