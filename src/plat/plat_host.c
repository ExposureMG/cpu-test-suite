/* Platform layer, host build: files under a directory, monotonic clock as a fake 49.875 MHz timebase, no SMC.
 * Report files (crash.json, progress.bin): in a host directory the "raw" sector API reads/writes the file itself at
 * idx * 512 with the same magic check; in image mode it is the disc guard's raw API on the image, as on the console.
 *
 * Image mode (XTS_HOST_STORAGE=image:<file> or --image <file>): the console's storage stack on the PC. <file> is a FAT
 * disk image (tools/fat_mkimage.py); it is opened as a disc (src/plat/disc_image_host.c, optional injected faults from
 * XTS_FAULTS, see plat/disc_host.h), wrapped by the disc guard (XTS_DISC_GUARD=0 turns it into a pass-through) and
 * mounted with libfat as "img:" exactly as the console mounts "uda:"; the core's stdio calls reach libfat through
 * src/plat/fatio_host.c. Host tests can hook every device call (xts_host_disc_hook, core.h), e.g. to crash inside one. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <fat.h>
#include <fat_xts.h>
#pragma weak fatFlush            /* absent from upstream libfat (host-fatreplay links both) */
#pragma weak fatFileExtent
#include <xts/xts.h>
#include <core/core.h>          /* crash path (self.crash simulation) */
#include <plat/disc_guard.h>
#include <plat/disc_host.h>

static char root[512] = "";
static char image[512] = "";       /* image mode: the image file */
static int quiet;
static char fat_name[16] = "img";

static void storage_notify(const char *msg) { fprintf(stderr, "%s\n", msg); }

/* image mode: every device call passes here first (host tests: xts_host_disc_hook) */
void (*xts_host_disc_hook)(int write, uint32_t sector, uint32_t count);
static const DISC_INTERFACE *hk_dev;
static bool hk_startup(void) { return hk_dev->startup(); }
static bool hk_inserted(void) { return hk_dev->isInserted(); }
static bool hk_clear(void) { return hk_dev->clearStatus(); }
static bool hk_shutdown(void) { return hk_dev->shutdown(); }
static s32 hk_sectors(void) { return hk_dev->sectors ? hk_dev->sectors() : 0; }
static bool hk_read(sec_t s, sec_t n, void *b)
{
  if (xts_host_disc_hook) xts_host_disc_hook(0, (uint32_t)s, (uint32_t)n);
  return hk_dev->readSectors(s, n, b);
}
static bool hk_write(sec_t s, sec_t n, const void *b)
{
  if (xts_host_disc_hook) xts_host_disc_hook(1, (uint32_t)s, (uint32_t)n);
  return hk_dev->writeSectors(s, n, b);
}
static DISC_INTERFACE hk_iface = {
  .startup = hk_startup, .isInserted = hk_inserted, .readSectors = hk_read, .writeSectors = hk_write,
  .clearStatus = hk_clear, .shutdown = hk_shutdown, .sectors = hk_sectors,
};

/* image mode: 0 ok */
static int image_mount(const char *path)
{
  xts_fault_cfg_t fc;
  const char *spec = getenv("XTS_FAULTS");
  if (xts_fault_parse(spec, &fc) < 0) { fprintf(stderr, "XTS_FAULTS: cannot parse \"%s\"\n", spec); return -1; }
  if (xts_image_disc_open(path, &fc, 0) < 0) { fprintf(stderr, "image %s: cannot open\n", path); return -1; }
  const char *g = getenv("XTS_DISC_GUARD");
  int guard = !(g && !strcmp(g, "0"));
  hk_dev = xts_image_disc();
  hk_iface.ioType = hk_dev->ioType;
  hk_iface.features = hk_dev->features;
  if (xts_disc_guard_init(&hk_iface, 512, guard) < 0) return -1;
  xts_disc_set_notify(storage_notify);
  const char *pg = getenv("XTS_FAT_PAGES");
  int pages = pg ? atoi(pg) : 4;              /* libfat's default on the console (DEFAULT_CACHE_PAGES) */
  if (!fatMount("img", xts_disc_guard_iface(), 0, (uint32_t)(pages > 2 ? pages : 2), 64)) {
    fprintf(stderr, "image %s: no FAT volume (libfat fatMount failed)\n", path);
    return -1;
  }
  snprintf(image, sizeof image, "%s", path);
  snprintf(root, sizeof root, "%s:", fat_name);
  return 0;
}

int xts_plat_init(int argc, char **argv)
{
  const char *r = getenv("XTS_ROOT"), *img = NULL, *hs = getenv("XTS_HOST_STORAGE");
  if (hs && !strncmp(hs, "image:", 6)) img = hs + 6;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--root") && i + 1 < argc) r = argv[++i];
    else if (!strcmp(argv[i], "--image") && i + 1 < argc) img = argv[++i];
    else if (!strcmp(argv[i], "--quiet")) quiet = 1;
  }
  if (getenv("XTS_QUIET")) quiet = 1;
  image[0] = 0;
  if (img) {
    if (image_mount(img) < 0) { root[0] = 0; return -1; }
  } else {
    snprintf(root, sizeof root, "%s", r ? r : "xts-host-out");
    xts_mkdirs(root);
  }
  return 0;
}

int xts_plat_is_host(void) { return 1; }
const char *xts_plat_root(void) { return root; }
const char *xts_plat_name(void) { return "host"; }
void xts_plat_poll(void) {}
void xts_plat_screen(const char *line) { if (!quiet) fprintf(stderr, "%s\n", line); }
void xts_plat_status(const char *line) { if (!quiet) fprintf(stderr, "[status] %s\n", line); }
void xts_plat_halt(const char *why)
{
  const char *um = "";
  if (image[0] && root[0]) {                   /* as the console does before it spins */
    int r = (xts_fat_busy || xts_cases_running) ? -1 : xts_plat_storage_unmount();
    um = r == 0 ? " (stick unmounted: safe to power off)" : " (stick NOT unmounted: check it)";
  }
  fprintf(stderr, "halt: %s%s\n", why, um);
  exit(1);
}

/* ---- storage boundaries ---- */
int xts_plat_storage_flush(void)
{
  if (!image[0] || !root[0]) return 0;
  if (xts_disc_poisoned()) return -1;
  return (!fatFlush || fatFlush(root)) ? 0 : -1;
}

int xts_plat_storage_unmount(void)
{
  if (!image[0]) return 0;                     /* host directory: nothing mounted */
  if (!root[0] || xts_disc_poisoned() || xts_fat_busy) return -1;
  int r = xts_plat_storage_flush();
  fatUnmount(fat_name);
  root[0] = 0;
  return r ? -2 : 0;
}

int xts_plat_storage_busy(void) { return image[0] ? xts_disc_busy() : 0; }
int xts_plat_storage_poisoned(void) { return image[0] ? xts_disc_poisoned() : 0; }
uint32_t xts_plat_sector_size(void) { return image[0] ? xts_disc_sector_size() : 512; }

int xts_plat_file_extent(const char *path, uint32_t *first, uint32_t *nsec)
{
  if (image[0]) {
    uint32_t bps = 0;
    if (!fatFileExtent) return -1;
    int r = fatFileExtent(path, first, nsec, &bps);
    return (r == 0 && bps != xts_disc_sector_size()) ? -1 : r;
  }
  struct stat st;
  if (stat(path, &st) != 0 || st.st_size <= 0) return -1;
  *first = 0;
  *nsec = (uint32_t)((st.st_size + 511) / 512);
  return 0;
}

int xts_plat_raw_read(const char *path, uint32_t first, uint32_t idx, void *buf)
{
  if (image[0]) return xts_disc_read(first + idx, 1, buf);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return XTS_DISC_EINIT;
  ssize_t r = pread(fd, buf, 512, (off_t)idx * 512);
  close(fd);
  return r == 512 ? 0 : XTS_DISC_EREAD;
}

int xts_plat_raw_write(const char *path, uint32_t first, uint32_t idx, const void *buf, const void *magic, uint32_t magic_len,
                       uint32_t magic_off)
{
  if (image[0]) return xts_disc_write_marked(first + idx, buf, magic, magic_len, magic_off, 0);
  if (magic_off + magic_len > 512 || memcmp((const uint8_t *)buf + magic_off, magic, magic_len)) return XTS_DISC_EINVAL;
  uint8_t cur[512];
  int fd = open(path, O_RDWR);
  if (fd < 0) return XTS_DISC_EINIT;
  int r = 0;
  if (pread(fd, cur, 512, (off_t)idx * 512) != 512) r = XTS_DISC_EREAD;
  else if (memcmp(cur + magic_off, magic, magic_len)) r = XTS_DISC_ENOMAGIC;
  else if (pwrite(fd, buf, 512, (off_t)idx * 512) != 512) r = XTS_DISC_EWRITE;   /* page cache: survives _exit */
  else if (pread(fd, cur, 512, (off_t)idx * 512) != 512 || memcmp(cur, buf, 512)) r = XTS_DISC_EVERIFY;
  close(fd);
  return r;
}

int xts_plat_storage_status(xts_storage_status_t *st)
{
  xts_storage_status_t tmp;
  if (!st) st = &tmp;
  if (image[0]) xts_disc_status(st);
  else memset(st, 0, sizeof *st);
  return st->poisoned;
}

char *xts_path(char *buf, size_t size, const char *a, const char *b, const char *c, const char *d)
{
  const char *parts[4] = { a, b, c, d };
  size_t o = 0;
  buf[0] = 0;
  for (int i = 0; i < 4 && parts[i]; i++) {
    int n = snprintf(buf + o, size > o ? size - o : 0, "%s%s", i ? "/" : "", parts[i]);
    if (n > 0) o += (size_t)n;
    if (o >= size) { buf[size - 1] = 0; break; }
  }
  return buf;
}

int xts_mkdirs(const char *path)
{
  char tmp[600];
  if (xts_write_refused("mkdir", path)) return -1;
  snprintf(tmp, sizeof tmp, "%s", path);
  char *dev = strchr(tmp, ':');
  if (dev) {                                   /* image mode: "img:/a/b" as on the console */
    for (char *p = dev + 2; *p; p++)
      if (*p == '/') {
        *p = 0;
        mkdir(tmp, 0777);
        *p = '/';
      }
    mkdir(tmp, 0777);
    return xts_file_exists(path) ? 0 : -1;
  }
  for (char *p = tmp + 1; *p; p++)
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0777);
      *p = '/';
    }
  return (mkdir(tmp, 0777) == 0 || errno == EEXIST) ? 0 : -1;
}

int xts_file_exists(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

int xts_append_durable(const char *path, const void *data, size_t n)
{
  if (xts_write_refused("append", path)) return -1;
  FILE *f = fopen(path, "ab");
  if (!f) return -1;
  size_t w = fwrite(data, 1, n, f);
  int fl = fflush(f), sy = fsync(fileno(f)), cl = fclose(f);
  return (w == n && fl == 0 && sy == 0 && cl == 0) ? 0 : -1;
}

int xts_read_small(const char *path, char *buf, size_t size)
{
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  size_t n = fread(buf, 1, size - 1, f);
  fclose(f);
  buf[n] = 0;
  return (int)n;
}

int xts_write_small(const char *path, const char *text)
{
  if (xts_write_refused("write", path)) return -1;
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  size_t n = strlen(text), w = fwrite(text, 1, n, f);
  int fl = fflush(f), sy = fsync(fileno(f)), cl = fclose(f);
  return (w == n && fl == 0 && sy == 0 && cl == 0) ? 0 : -1;
}

uint64_t xts_tb(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 49875000ull + (uint64_t)ts.tv_nsec * 49875ull / 1000000ull;
}
uint64_t xts_tb_hz(void) { return 49875000ull; }
void xts_delay_ms(uint32_t ms) { usleep(ms * 1000u); }

int xts_smc_version(uint8_t v[3]) { (void)v; return -1; }
int xts_smc_temps(uint16_t t[4]) { (void)t; return -1; }
int xts_smc_rtc(uint8_t raw[5]) { (void)raw; return -1; }
void xts_plat_reboot(void) { exit(77); }
uint64_t xts_mfspr64(int spr) { (void)spr; return 0; }

void xts_plat_sysinfo(struct xts_json *j)
{
  xj_kv_str(j, "name", "host");
  xj_kv_str(j, "note", "PC dry run: nothing executed on a Xenon CPU");
  if (image[0]) {
    xj_kv_str(j, "storage", "image (libfat + disc guard, as on the console)");
    xj_kv_bool(j, "disc_guard", xts_disc_guard_enabled());
    struct statvfs sv;
    memset(&sv, 0, sizeof sv);                  /* libfat rescans the whole FAT if f_flag happens to read "SCAN" */
    if (xts_fatio_statvfs(root, &sv) == 0) {
      xj_kv_uint(j, "volume_free_bytes", (uint64_t)sv.f_bfree * sv.f_bsize);
      xj_kv_uint(j, "volume_size_bytes", (uint64_t)sv.f_blocks * sv.f_bsize);
    }
  }
}

void xts_plat_default_tag(char *buf, size_t size) { snprintf(buf, size, "host"); }

uintptr_t xts_plat_heap_limit(void) { return UINTPTR_MAX; }

/* self.crash on the PC: never crashes the host process by itself. Host tests set xts_host_crash_sim in a forked boot:
 * then the crash path runs as on the console (xts_crash_handle: crash.json, from a fake program-interrupt context), its
 * screen lines go to stderr, and the "boot" ends like a reset (_exit(9)). */
int xts_host_crash_sim;

void xts_host_crash_screen(void)
{
  for (int i = 0; i < XTS_CRASH_LINES; i++)
    if (xts_crash_screen[i][0]) fprintf(stderr, "%s\n", xts_crash_screen[i]);
}

int xts_plat_self_crash(void)
{
  if (!xts_host_crash_sim) return -1;
  uint64_t ctx[40];
  memset(ctx, 0, sizeof ctx);
  ctx[36] = (uint64_t)(uintptr_t)xts_plat_self_crash & 0xFFFFFFFFu;   /* SRR0: outside every trap range */
  ctx[37] = 0x9000000000000000ull | ((uint64_t)XTS_CAUSE_TRAP << 16);
  ctx[39] = 0;                                                       /* PIR 0: the main thread */
  xts_crash_handle(0x700, ctx, 0);
  xts_host_crash_screen();
  _exit(9);
}
