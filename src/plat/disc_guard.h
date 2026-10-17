/* Disc guard: a DISC_INTERFACE between libfat and the block device (console: libxenon's USB mass storage; host: an
 * image file, optionally with injected faults). Include as <plat/disc_guard.h>; framework code only (src/plat, src/core,
 * host tools and tests).
 *
 * libfat side (xts_disc_guard_iface()):
 *   readSectors   reads the range twice into two buffers pre-filled with different patterns and compares them. On a
 *                 mismatch or a device error the pair is repeated once; a second failure fails the call and latches
 *                 "poisoned". A short or stale transfer that the driver reports as success therefore cannot reach libfat.
 *   writeSectors  refused (false) once poisoned. Otherwise writes, reads the range back (pattern-filled buffer) and
 *                 compares; a device error or a read-back that differs (after one re-read) fails the call and latches
 *                 "poisoned" - after ONE rewrite + read-back of the same data, so that the range holds the intended
 *                 contents (not a torn or shifted copy) whenever the device still allows it.
 *   Requests are processed in chunks of XTS_DISC_CHUNK sectors (libfat asks for one cache page, 64 sectors, at most).
 *   With the guard disabled (xts_disc_guard_init(..., 0)) the interface is a pass-through that only counts.
 *
 * Raw-sector API (verified like the libfat side, independent of the enabled flag; no malloc, no stdio, no libfat):
 *   xts_disc_read          double-read verified read of n sectors.
 *   xts_disc_write_marked  one-sector write that only happens if the sector CURRENTLY holds `magic` at `magic_off`
 *                          (checked with a verified read), and the new contents carry the same magic at the same place
 *                          (so the slot stays claimable). Read back and compared after the write. Refused while poisoned
 *                          unless XTS_DISC_WM_ALLOW_POISONED is passed. Intended for slots reserved on the stick (e.g. a
 *                          pre-allocated file whose sectors fatFileExtent() located while libfat was healthy).
 *   Never call these while xts_disc_busy() (a guard operation was interrupted, e.g. by a crash inside the USB driver).
 */
#ifndef XTS_PLAT_DISC_GUARD_H
#define XTS_PLAT_DISC_GUARD_H
#include <stddef.h>
#include <stdint.h>
#include <diskio/disc_io.h>
#include <xts/plat.h>

#define XTS_DISC_CHUNK 64            /* sectors per verified device call */
#define XTS_DISC_MAX_SECTOR 4096     /* largest sector size supported */

/* error codes of the raw API */
#define XTS_DISC_OK 0
#define XTS_DISC_EINIT -1            /* no device */
#define XTS_DISC_EPOISONED -2        /* refused: storage poisoned */
#define XTS_DISC_EREAD -3            /* read error or the two reads disagree (latches poisoned) */
#define XTS_DISC_ENOMAGIC -4         /* the sector does not hold the magic: nothing written */
#define XTS_DISC_EWRITE -5           /* the device reported a write error (latches poisoned) */
#define XTS_DISC_EVERIFY -6          /* read-back differs from what was written (latches poisoned) */
#define XTS_DISC_EINVAL -7           /* bad arguments (magic outside the sector, buf without the magic, ...) */
#define XTS_DISC_EBUSY -8            /* a guard operation is in progress (re-entry) */

#define XTS_DISC_WM_ALLOW_POISONED 1u

/* Set up the guard over `dev` (sector_size 512..4096). enabled = 0: libfat side is a pass-through (counts only).
 * Buffers are allocated here, once. Returns 0, or -1 (no memory / bad sector size). Calling it again resets the
 * statistics and the latch and re-targets the guard. */
int xts_disc_guard_init(const DISC_INTERFACE *dev, uint32_t sector_size, int enabled);
const DISC_INTERFACE *xts_disc_guard_iface(void);   /* what fatMount gets */
const DISC_INTERFACE *xts_disc_guard_device(void);  /* the wrapped device (NULL before init) */
int xts_disc_guard_enabled(void);

int xts_disc_poisoned(void);
int xts_disc_busy(void);
/* latch "poisoned" from outside (e.g. a higher layer found the volume inconsistent); the first latch wins */
void xts_disc_poison(const char *op, uint32_t sector, uint32_t count, const char *why);
void xts_disc_status(xts_storage_status_t *st);
/* called once, at the moment the latch is set (console: the screen); may be NULL */
void xts_disc_set_notify(void (*fn)(const char *message));
/* "STORAGE ERROR: ... stop using this stick until it has been checked" for the latched error ("" if none) */
void xts_disc_message(char *buf, size_t size);

uint32_t xts_disc_sector_size(void);
int xts_disc_read(uint32_t sector, uint32_t n, void *buf);
int xts_disc_write_marked(uint32_t sector, const void *buf, const void *magic, uint32_t magic_len, uint32_t magic_off,
                          unsigned flags);
#endif
