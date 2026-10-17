/* Host-only storage pieces for replaying the console's storage stack on the PC (include as <plat/disc_host.h>):
 *   - an image-file DISC_INTERFACE with fault injection that models the failure modes of the USB mass-storage driver;
 *   - the stdio bridge: with libfat mounted on the image, fopen/remove/mkdir/stat/fsync/fileno on "<dev>:/..." paths go
 *     to libfat (the host links with -Wl,--wrap=<those>), so the core's unchanged stdio code runs on libfat.
 * Used by plat_host.c (XTS_HOST_STORAGE=image:<file>), host/fatreplay.c and host/tests/t_storage_*.c. */
#ifndef XTS_PLAT_DISC_HOST_H
#define XTS_PLAT_DISC_HOST_H
#ifdef XTS_HOST
#include <stdint.h>
#include <sys/statvfs.h>
#include <diskio/disc_io.h>

/* Fault model. Probabilities are per device call of that kind (after `after` calls), drawn from a seeded PRNG.
 * "Unpatched driver" modes (the call reports SUCCESS):
 *   short_read  only the first k of n sectors are transferred, the rest of the buffer keeps its old contents
 *   cbw_read    a USB CSW (13 bytes "USBS"...) lands at the start of one sector of the buffer
 *   torn_write  only the first k of n sectors reach the medium
 *   cbw_write   a 31-byte CBW ("USBC" + WRITE(10)) is taken as data at the start of sector j: the data from there on
 *               is shifted by 31 bytes (the incident's "sector 4 starts with a CBW")
 *   misdirect   the data goes to the LBA of the previous write command instead (data/command desync)
 * "Patched driver" modes (the call reports FAILURE; partial transfers may have happened):
 *   read_fail   the first k sectors are transferred, then the call fails
 *   write_fail  the first k sectors reach the medium, then the call fails */
typedef struct xts_fault_cfg {
  uint64_t seed;
  double short_read, cbw_read, torn_write, cbw_write, misdirect, read_fail, write_fail;
  uint32_t at;             /* >0: exactly one fault, at device call number `at` (or the first later call of a kind
                              that has an enabled mode); the mode is drawn among the enabled ones (rate > 0) */
  uint32_t after;          /* no faults during the first `after` device calls (mount, probe) */
  uint32_t max;            /* stop after this many injected faults (0 = no limit) */
  int verbose;             /* print every injected fault to stderr */
} xts_fault_cfg_t;

typedef struct xts_fault_stats {
  uint64_t calls, reads, writes, injected;
  uint64_t n_short_read, n_cbw_read, n_torn_write, n_cbw_write, n_misdirect, n_read_fail, n_write_fail;
} xts_fault_stats_t;

/* "seed=3,short_read=0.01,torn_write=0.005,after=200,max=1,at=0,verbose=1" or a preset name followed by overrides:
 * "unpatched:0.01" (short_read+cbw_read+torn_write+cbw_write at that rate each), "patched:0.01" (read_fail+write_fail),
 * "none". 0 ok, -1 parse error. */
int xts_fault_parse(const char *spec, xts_fault_cfg_t *c);

/* open an image file as a disc (512-byte sectors); faults may be NULL; readonly: writes fail and the interface does
 * not advertise FEATURE_MEDIUM_CANWRITE (libfat then never writes, not even FSInfo). 0 ok */
int xts_image_disc_open(const char *path, const xts_fault_cfg_t *faults, int readonly);
void xts_image_disc_close(void);
const DISC_INTERFACE *xts_image_disc(void);
void xts_image_fault_stats(xts_fault_stats_t *st);
void xts_image_fault_set(const xts_fault_cfg_t *faults);   /* change the fault model of the open image */

/* stdio bridge state: 1 while a libfat device is registered */
int xts_fatio_active(void);
int xts_fatio_statvfs(const char *path, struct statvfs *sv);
#endif
#endif
