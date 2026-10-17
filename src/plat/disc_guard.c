/* Disc guard between libfat and the block device: verified reads (double read), verified writes (read-back), a latch
 * that refuses every write after the first storage error, statistics, and a raw-sector API. See disc_guard.h. */
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <plat/disc_guard.h>

#define PAT_A 0xA5   /* fill of the caller's buffer before a read */
#define PAT_B 0x5A   /* fill of the verification buffer */

static const DISC_INTERFACE *dev;
static uint32_t ss = 512;
static int enabled;
static uint8_t *scr;          /* XTS_DISC_CHUNK sectors: second read / read-back */
static uint8_t *sec1;         /* one sector: raw API */
static volatile int busy;
static void (*notify)(const char *msg);

static struct {
  int poisoned;
  char op[8];
  uint32_t sector, count;
  char why[112];
  uint64_t sectors_read, sectors_written, device_reads, device_writes, read_retries, writes_refused, ticks;
  uint64_t repairs, repaired;
} g;

/* ---- latch ---- */
void xts_disc_set_notify(void (*fn)(const char *message)) { notify = fn; }
int xts_disc_poisoned(void) { return g.poisoned; }
int xts_disc_busy(void) { return busy; }
const DISC_INTERFACE *xts_disc_guard_device(void) { return dev; }
int xts_disc_guard_enabled(void) { return enabled; }
uint32_t xts_disc_sector_size(void) { return ss; }

void xts_disc_message(char *buf, size_t size)
{
  if (!g.poisoned) { if (size) buf[0] = 0; return; }
  snprintf(buf, size, "STORAGE ERROR: %s of sectors %lu-%lu failed (%s). No further writes. Stop using this stick until "
           "it has been checked (Windows: chkdsk /f, Linux: fsck.fat -n).", g.op, (unsigned long)g.sector,
           (unsigned long)(g.sector + (g.count ? g.count - 1 : 0)), g.why);
}

void xts_disc_poison(const char *op, uint32_t sector, uint32_t count, const char *why)
{
  if (g.poisoned) return;
  g.poisoned = 1;
  snprintf(g.op, sizeof g.op, "%s", op);
  g.sector = sector;
  g.count = count;
  snprintf(g.why, sizeof g.why, "%s", why);
  if (notify) {
    char m[300];
    xts_disc_message(m, sizeof m);
    notify(m);
  }
}

void xts_disc_status(xts_storage_status_t *st)
{
  memset(st, 0, sizeof *st);
  st->guard = dev && enabled;
  st->poisoned = g.poisoned;
  snprintf(st->op, sizeof st->op, "%s", g.op);
  st->sector = g.sector;
  st->count = g.count;
  snprintf(st->why, sizeof st->why, "%s", g.why);
  st->sectors_read = g.sectors_read;
  st->sectors_written = g.sectors_written;
  st->device_reads = g.device_reads;
  st->device_writes = g.device_writes;
  st->read_retries = g.read_retries;
  st->writes_refused = g.writes_refused;
  st->write_repairs = g.repairs;
  st->write_repaired = g.repaired;
  st->guard_ms = (double)g.ticks * 1000.0 / (double)xts_tb_hz();
}

/* ---- verified primitives (k <= XTS_DISC_CHUNK) ---- */
static int dev_read(uint32_t s, uint32_t k, void *b)
{
  g.device_reads++;
  return dev->readSectors(s, k, b) ? 1 : 0;
}

static int dev_write(uint32_t s, uint32_t k, const void *b)
{
  g.device_writes++;
  return dev->writeSectors(s, k, b) ? 1 : 0;
}

/* double read: 0 ok, else XTS_DISC_EREAD with *why set */
static int vread(uint32_t s, uint32_t k, uint8_t *buf, const char **why)
{
  size_t bytes = (size_t)k * ss;
  for (int attempt = 0; attempt < 2; attempt++) {
    memset(buf, PAT_A, bytes);
    memset(scr, PAT_B, bytes);
    if (!dev_read(s, k, buf)) { *why = "device read error"; continue; }
    if (!dev_read(s, k, scr)) { *why = "device read error (second read)"; continue; }
    if (memcmp(buf, scr, bytes)) { *why = "the two reads of the same sectors differ (short or stale transfer)"; continue; }
    if (attempt) g.read_retries++;
    return 0;
  }
  return XTS_DISC_EREAD;
}

/* read back and compare (one re-read): 1 = the range holds src */
static int readback(uint32_t s, uint32_t k, const uint8_t *src, const char **why)
{
  size_t bytes = (size_t)k * ss;
  for (int attempt = 0; attempt < 2; attempt++) {
    memset(scr, PAT_B, bytes);
    if (!dev_read(s, k, scr)) { *why = "device read error while reading the written sectors back"; continue; }
    if (memcmp(scr, src, bytes)) { *why = "the sectors read back differ from the data written"; continue; }
    return 1;
  }
  return 0;
}

/* write + read-back: 0 ok, XTS_DISC_EWRITE / XTS_DISC_EVERIFY with *why set. A failed write (device error, or a
 * read-back that differs) is written ONCE more and read back again, so the range ends up holding what the caller meant
 * (whole, not torn) whenever the device allows it; the call fails and the guard latches either way. */
static int vwrite(uint32_t s, uint32_t k, const uint8_t *src, const char **why)
{
  int r;
  if (!dev_write(s, k, src)) { *why = "device write error"; r = XTS_DISC_EWRITE; }
  else if (readback(s, k, src, why)) return 0;
  else r = XTS_DISC_EVERIFY;
  g.repairs++;
  if (dev_write(s, k, src) && readback(s, k, src, why)) {
    g.repaired++;
    *why = r == XTS_DISC_EWRITE ? "device write error; rewritten once and verified" :
                                  "read-back differed from the data written; rewritten once and verified";
  } else {
    *why = r == XTS_DISC_EWRITE ? "device write error; the rewrite failed too" :
                                  "read-back differed from the data written; the rewrite failed too";
  }
  return r;
}

/* ---- the DISC_INTERFACE libfat gets ---- */
static bool g_startup(void) { return dev->startup(); }
static bool g_inserted(void) { return dev->isInserted(); }
static bool g_clear(void) { return dev->clearStatus(); }
static bool g_shutdown(void) { return dev->shutdown(); }
static s32 g_sectors(void) { return dev->sectors ? dev->sectors() : 0; }

static bool g_read(sec_t sector, sec_t n, void *buffer)
{
  uint64_t t0 = xts_tb();
  bool ok = true;
  busy++;
  if (!enabled) {
    ok = dev_read(sector, n, buffer);
    if (ok) g.sectors_read += n;
  } else {
    uint8_t *b = buffer;
    for (sec_t done = 0; done < n && ok;) {
      uint32_t k = n - done > XTS_DISC_CHUNK ? XTS_DISC_CHUNK : n - done;
      const char *why = "";
      if (vread(sector + done, k, b + (size_t)done * ss, &why)) {
        xts_disc_poison("read", sector + done, k, why);
        ok = false;
        break;
      }
      g.sectors_read += k;
      done += k;
    }
  }
  busy--;
  g.ticks += xts_tb() - t0;
  return ok;
}

static bool g_write(sec_t sector, sec_t n, const void *buffer)
{
  uint64_t t0 = xts_tb();
  bool ok = true;
  busy++;
  if (!enabled) {
    ok = dev_write(sector, n, buffer);
    if (ok) g.sectors_written += n;
  } else if (g.poisoned) {
    g.writes_refused++;
    ok = false;
  } else {
    const uint8_t *b = buffer;
    for (sec_t done = 0; done < n;) {
      uint32_t k = n - done > XTS_DISC_CHUNK ? XTS_DISC_CHUNK : n - done;
      const char *why = "";
      int r = vwrite(sector + done, k, b + (size_t)done * ss, &why);
      if (r) {
        xts_disc_poison(r == XTS_DISC_EWRITE ? "write" : "verify", sector + done, k, why);
        ok = false;
        break;
      }
      g.sectors_written += k;
      done += k;
    }
  }
  busy--;
  g.ticks += xts_tb() - t0;
  return ok;
}

static DISC_INTERFACE iface = {
  .startup = g_startup,
  .isInserted = g_inserted,
  .readSectors = g_read,
  .writeSectors = g_write,
  .clearStatus = g_clear,
  .shutdown = g_shutdown,
  .sectors = g_sectors,
};

const DISC_INTERFACE *xts_disc_guard_iface(void) { return dev ? &iface : NULL; }

int xts_disc_guard_init(const DISC_INTERFACE *d, uint32_t sector_size, int en)
{
  if (!d || sector_size < 512 || sector_size > XTS_DISC_MAX_SECTOR || (sector_size & (sector_size - 1))) return -1;
  if (!scr) scr = memalign(128, (size_t)XTS_DISC_CHUNK * XTS_DISC_MAX_SECTOR);
  if (!sec1) sec1 = memalign(128, XTS_DISC_MAX_SECTOR);
  if (!scr || !sec1) return -1;
  dev = d;
  ss = sector_size;
  enabled = en;
  memset(&g, 0, sizeof g);
  iface.ioType = d->ioType;
  iface.features = d->features;
  return 0;
}

/* ---- raw-sector API ---- */
int xts_disc_read(uint32_t sector, uint32_t n, void *buf)
{
  if (!dev) return XTS_DISC_EINIT;
  if (busy) return XTS_DISC_EBUSY;
  busy++;
  int r = 0;
  uint8_t *b = buf;
  for (uint32_t done = 0; done < n && !r;) {
    uint32_t k = n - done > XTS_DISC_CHUNK ? XTS_DISC_CHUNK : n - done;
    const char *why = "";
    r = vread(sector + done, k, b + (size_t)done * ss, &why);
    if (r) xts_disc_poison("read", sector + done, k, why);
    done += k;
  }
  busy--;
  return r;
}

int xts_disc_write_marked(uint32_t sector, const void *buf, const void *magic, uint32_t magic_len, uint32_t magic_off,
                          unsigned flags)
{
  if (!dev) return XTS_DISC_EINIT;
  if (!buf || !magic || !magic_len || magic_off > ss || magic_len > ss - magic_off) return XTS_DISC_EINVAL;
  if (memcmp((const uint8_t *)buf + magic_off, magic, magic_len)) return XTS_DISC_EINVAL;
  if (g.poisoned && !(flags & XTS_DISC_WM_ALLOW_POISONED)) { g.writes_refused++; return XTS_DISC_EPOISONED; }
  if (busy) return XTS_DISC_EBUSY;
  busy++;
  const char *why = "";
  int r = vread(sector, 1, sec1, &why);
  if (r) xts_disc_poison("read", sector, 1, why);
  else if (memcmp(sec1 + magic_off, magic, magic_len)) r = XTS_DISC_ENOMAGIC;
  else {
    r = vwrite(sector, 1, buf, &why);
    if (r) xts_disc_poison(r == XTS_DISC_EWRITE ? "write" : "verify", sector, 1, why);
    else g.sectors_written++;
  }
  busy--;
  return r;
}
