/* Platform layer, console build (libxenon + libfat): screen, USB mass storage, timebase, SMC, identity, the crash entry
 * and raw report-file sectors. No network. Never reads the CPU key, the keyvault or fuse lines 3-6. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <console/console.h>
#include <fat.h>                    /* third_party/libfat (vendored, patched; same API as <libfat/fat.h>) */
#include <fat_xts.h>
#include <ppc/cache.h>
#include <ppc/timebase.h>
#include <time/time.h>
#include <usb/usbmain.h>
#include <xb360/xb360.h>
#include <xenon_nand/xenon_sfcx.h>
#include <xenon_smc/xenon_smc.h>
#include <xenon_soc/xenon_power.h>
#include <xenos/xenos.h>
#include <xts/xts.h>
#include <core/core.h>              /* crash path, write guard, build id */
#include <plat/plat_libxenon.h>     /* xenon_get_speed, usb2mass_ops_N */
#include <plat/disc_guard.h>        /* every libfat sector goes through the disc guard */

/* Heap bound for the big buffer and other large allocations (xts_plat_heap_limit): XTS's image + bss start at 0, the heap
 * grows above them (unbounded sbrk), the main stack sits at 0x1DE00000 and grows down, libxenon's per-thread HV contexts
 * are at 0x1DE80000-0x1DFFFFFF. Big allocations stay below EA 0x9C800000 (real 0x1C800000), ~22 MB under the stack.
 * (The RAM crash page that used to sit at 0x1D000000 is gone: crash reports go to crash.json on the stick.) */
#define HEAP_LIMIT_EA 0x9C800000u

static char root[16] = "";
static char vol_label[32] = "";
static unsigned long long vol_free = 0, vol_size = 0;
static int vol_stat_ok;
static int cached_type = -1;
static unsigned cached_dve;
static uint8_t smc_raw_12[16], smc_raw_04[16];
static int smc_have_12, smc_have_04;
static volatile int usb_poll_busy;   /* inside usb_do_poll (the crash path then leaves the stick alone) */
static char fat_name[8];

/* crash path state (src/exec/vec.S crashdump): per-PIR entry count, emergency stacks, the r2/r13 main() runs with */
volatile uint32_t xts_crash_depth[6];
uint8_t xts_crash_stacks[6][16384] __attribute__((aligned(128)));
uint32_t xts_crash_r2, xts_crash_r13;

/* ---- status row: drawn straight into the framebuffer (libxenon's console_putch flushes the whole framebuffer from the
 * cache after EVERY character, ~0.1-0.3 ms each). Same tiled layout and font as drivers/console/console.c; one cache
 * flush of the touched tile row per update. Row 0 of the text area; the console's own text starts below it. ---- */
extern const unsigned char fontdata_8x16[];
struct xts_ati_info {
  uint32_t unknown1[4];
  uint32_t base;
  uint32_t unknown2[8];
  uint32_t width;
  uint32_t height;
} __attribute__((__packed__));
static uint32_t *sfb;
static int sfb_w, sfb_offx, sfb_offy, sfb_cols;

static void status_init(void)
{
  volatile struct xts_ati_info *ai = (volatile struct xts_ati_info *)0xec806100u;
  if (!ai->base || !ai->width || !ai->height) return;
  sfb = (uint32_t *)(ai->base | 0x80000000u);
  sfb_w = (int)((ai->width + 31) >> 5) << 5;
  sfb_offx = sfb_offy = 0;
  if (xenos_is_overscan()) { sfb_offx = (int)ai->width / 28; sfb_offy = (int)ai->height / 28; }
  sfb_cols = ((int)ai->width - 2 * sfb_offx) / 8;
  if (sfb_cols > 200) sfb_cols = 200;
}

static inline void status_px(int x, int y, uint32_t c)
{
  uint32_t i = (uint32_t)((((y >> 5) * 32 * sfb_w + ((x >> 5) << 10) + (x & 3) + ((y & 1) << 2) + (((x & 31) >> 2) << 3) +
                           (((y & 31) >> 1) << 6)) ^ ((y & 8) << 2)));
  sfb[i] = c;
}

/* crash-path safe: no stdio, no malloc */
static void status_draw_row(int row, const char *s)
{
  if (!sfb) return;
  uint32_t fg = console_color[0], bg = console_color[1];   /* inverted: a bar */
  int y0 = sfb_offy + 16 * row;
  for (int col = 0; col < sfb_cols; col++) {
    unsigned char ch = *s ? (unsigned char)*s++ : ' ';
    int x0 = sfb_offx + col * 8;
    for (int ly = 0; ly < 16; ly++) {
      unsigned char bits = fontdata_8x16[ch * 16 + ly];
      for (int lx = 0; lx < 8; lx++) status_px(x0 + lx, y0 + ly, (bits >> (7 - lx)) & 1 ? fg : bg);
    }
  }
  int r0 = y0 >> 5, r1 = (y0 + 15) >> 5;
  memdcbst(sfb + r0 * 32 * sfb_w, (r1 - r0 + 1) * 32 * sfb_w * 4);
}
static void status_draw(const char *s) { status_draw_row(0, s); }

static uint32_t usb_sector_size;
static void storage_notify(const char *msg) { printf("\n%s\n", msg); }

/* Mount the first USB FAT volume ONCE, through the disc guard, as "uda:" / "udb:" / "udc:" (the names fatInitDefault
 * used). fatInitDefault is not used: called in a retry loop it re-ran every interface's startup() and probed the ATA
 * interface too, and it would mount the raw USB interface without the guard. Cache: libfat's defaults (4 pages of 64
 * sectors), as fatInitDefault had. 0 = mounted */
static int mount_usb(void)
{
  static const char *const names[3] = { "uda", "udb", "udc" };
  DISC_INTERFACE *ud[3] = { &usb2mass_ops_0, &usb2mass_ops_1, &usb2mass_ops_2 };
  for (int i = 0; i < 3; i++) {
    if (!ud[i]->isInserted()) continue;
    u32 ss = 0;
    if (USBStorage_Get_Capacity(i, &ss) <= 0 || ss < 512 || ss > XTS_DISC_MAX_SECTOR) ss = 512;
    if (xts_disc_guard_init(ud[i], ss, 1) < 0) return -1;
    xts_disc_set_notify(storage_notify);
    if (fatMount(names[i], xts_disc_guard_iface(), 0, 4, 64)) {
      usb_sector_size = ss;
      snprintf(root, sizeof root, "%s:", names[i]);
      snprintf(fat_name, sizeof fat_name, "%s", names[i]);
      return 0;
    }
  }
  return -1;
}

static int smc_query(uint8_t cmd, uint8_t reply[16]);

int xts_plat_init(int argc, char **argv)
{
  (void)argc; (void)argv;
  asm volatile("stw 2, 0(%0)\n\tstw 13, 0(%1)" : : "b"(&xts_crash_r2), "b"(&xts_crash_r13) : "memory");
  xenos_init(VIDEO_MODE_AUTO);
  console_init();
  status_init();
  console_set_cursor(0, 2);                                   /* row 0 is the status row */
  printf("XTS %s (Xenon CPU test suite), build %s\n", XTS_VERSION, xts_build_id());
  cached_type = xenon_get_console_type();                     /* SMC queries without timeout in libxenon: once */
  cached_dve = xenon_get_DVE();
  usb_init();
  root[0] = 0;
  for (int attempt = 0; attempt < 10 && !root[0]; attempt++) {
    uint64_t t0 = mftb();
    while (tb_diff_msec(mftb(), t0) < 500) xts_plat_poll();    /* let the device enumerate */
    mount_usb();
  }
  if (!root[0]) { printf("XTS: no USB FAT volume found (FAT32 on an MBR partition, or a superfloppy, is required)\n"); return -1; }
  fatGetVolumeLabel(root, vol_label);
  vol_label[sizeof vol_label - 1] = 0;
  printf("XTS: output on USB device %s, volume label \"%s\"; checking free space...\n", root, vol_label);
  char rp[24];
  snprintf(rp, sizeof rp, "%s/", root);
  struct statvfs sv;
  memset(&sv, 0, sizeof sv);   /* libfat's statvfs rescans the whole FAT (and rewrites FSInfo) if f_flag reads "SCAN" */
  if (statvfs(rp, &sv) == 0) {
    vol_stat_ok = 1;
    vol_free = (unsigned long long)sv.f_bfree * sv.f_bsize;
    vol_size = (unsigned long long)sv.f_blocks * sv.f_bsize;
    printf("XTS: %s %llu MB free of %llu MB\n", root, vol_free >> 20, vol_size >> 20);
  } else {
    printf("XTS: %s free space unknown (statvfs failed)\n", root);
  }
  return 0;
}

int xts_plat_is_host(void) { return 0; }
const char *xts_plat_root(void) { return root; }
const char *xts_plat_name(void) { return "console"; }
void xts_plat_poll(void)
{
  usb_poll_busy = 1;
  usb_do_poll();
  usb_poll_busy = 0;
}
void xts_plat_screen(const char *line) { printf("%s\n", line); }
void xts_plat_status(const char *line)
{
  if (sfb) status_draw(line);
  else printf("%s\n", line);
}

int xts_plat_storage_status(xts_storage_status_t *st)
{
  xts_storage_status_t tmp;
  if (!st) st = &tmp;
  if (xts_disc_guard_device()) xts_disc_status(st);
  else memset(st, 0, sizeof *st);
  return st->poisoned;
}

void xts_plat_halt(const char *why)
{
  const char *um = "";
  if (root[0]) {                              /* final state: everything libfat holds onto the stick, then unmount */
    int r = (xts_fat_busy || xts_cases_running) ? -1 : xts_plat_storage_unmount();
    um = r == 0 ? "\nXTS: stick unmounted: safe to power off." :
                  "\nXTS: the stick could NOT be unmounted cleanly: check it on a PC (fsck.fat -n / chkdsk) before reuse.";
  }
  printf("\nXTS: %s%s\n", why, um);
  for (;;) {
    xts_plat_poll();
    mdelay(100);
  }
}

/* ---- storage boundaries ---- */
int xts_plat_storage_flush(void)
{
  if (!root[0]) return 0;
  if (xts_disc_poisoned()) return -1;
  return fatFlush(root) ? 0 : -1;
}

int xts_plat_storage_unmount(void)
{
  if (!root[0] || xts_disc_poisoned() || xts_fat_busy) return -1;
  xts_fat_busy++;
  int r = fatFlush(root) ? 0 : -2;
  fatUnmount(fat_name);                       /* closes what is still open, flushes the cache, writes FSInfo */
  xts_fat_busy--;
  root[0] = 0;
  return r;
}

int xts_plat_storage_busy(void) { return xts_disc_busy() || usb_poll_busy; }
int xts_plat_storage_poisoned(void) { return xts_disc_poisoned(); }
uint32_t xts_plat_sector_size(void) { return usb_sector_size ? usb_sector_size : 512; }

int xts_plat_file_extent(const char *path, uint32_t *first, uint32_t *nsec)
{
  uint32_t bps = 0;
  xts_fat_busy++;
  int r = fatFileExtent(path, first, nsec, &bps);
  xts_fat_busy--;
  return (r == 0 && bps != xts_disc_sector_size()) ? -1 : r;
}

/* crash-path safe: the disc guard's raw API (no libfat, no malloc, no stdio) */
int xts_plat_raw_read(const char *path, uint32_t first, uint32_t idx, void *buf)
{
  (void)path;
  return xts_disc_read(first + idx, 1, buf);
}

int xts_plat_raw_write(const char *path, uint32_t first, uint32_t idx, const void *buf, const void *magic, uint32_t magic_len,
                       uint32_t magic_off)
{
  (void)path;
  return xts_disc_write_marked(first + idx, buf, magic, magic_len, magic_off, 0);
}

char *xts_path(char *buf, size_t size, const char *a, const char *b, const char *c, const char *d)
{
  const char *parts[4] = { a, b, c, d };
  size_t o = 0;
  buf[0] = 0;
  for (int i = 0; i < 4 && parts[i]; i++) {
    /* "uda0:" + "xts" -> "uda0:/xts" */
    int n = snprintf(buf + o, size > o ? size - o : 0, "%s%s", i ? "/" : "", parts[i]);
    if (n > 0) o += (size_t)n;
    if (o >= size) { buf[size - 1] = 0; break; }
  }
  return buf;
}

int xts_mkdirs(const char *path)
{
  char tmp[320];
  if (xts_write_refused("mkdir", path)) return -1;
  xts_fat_busy++;
  snprintf(tmp, sizeof tmp, "%s", path);
  char *start = strchr(tmp, ':');
  start = start ? start + 2 : tmp + 1;
  for (char *p = start; *p; p++)
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0777);
      *p = '/';
    }
  mkdir(tmp, 0777);
  int ok = xts_file_exists(path);
  xts_fat_busy--;
  return ok ? 0 : -1;
}

int xts_file_exists(const char *path)
{
  struct stat st;
  xts_fat_busy++;
  int r = stat(path, &st) == 0;
  xts_fat_busy--;
  return r;
}

/* every libfat use is bracketed by xts_fat_busy (the crash path never enters libfat while it is set) */
int xts_append_durable(const char *path, const void *data, size_t n)
{
  if (xts_write_refused("append", path)) return -1;
  xts_fat_busy++;
  int r = -1;
  FILE *f = fopen(path, "ab");
  if (f) {
    size_t w = fwrite(data, 1, n, f);
    int fl = fflush(f), sy = fsync(fileno(f)), cl = fclose(f);   /* fsync: newlib -> libfat _FAT_fsync_r */
    r = (w == n && fl == 0 && sy == 0 && cl == 0) ? 0 : -1;
  }
  xts_fat_busy--;
  return r;
}

int xts_read_small(const char *path, char *buf, size_t size)
{
  xts_fat_busy++;
  FILE *f = fopen(path, "rb");
  if (!f) { xts_fat_busy--; return -1; }
  size_t n = fread(buf, 1, size - 1, f);
  fclose(f);
  xts_fat_busy--;
  buf[n] = 0;
  return (int)n;
}

int xts_write_small(const char *path, const char *text)
{
  if (xts_write_refused("write", path)) return -1;
  xts_fat_busy++;
  int r = -1;
  FILE *f = fopen(path, "wb");
  if (f) {
    size_t n = strlen(text), w = fwrite(text, 1, n, f);
    int fl = fflush(f), sy = fsync(fileno(f)), cl = fclose(f);
    r = (w == n && fl == 0 && sy == 0 && cl == 0) ? 0 : -1;
  }
  xts_fat_busy--;
  return r;
}

uint64_t xts_tb(void) { return mftb(); }
uint64_t xts_tb_hz(void) { return (uint64_t)PPC_TIMEBASE_FREQ; }
void xts_delay_ms(uint32_t ms) { mdelay((int)ms); }

/* ---- SMC with timeouts (libxenon's xenon_smc_receive_response waits forever) ---- */
static int smc_query(uint8_t cmd, uint8_t reply[16])
{
  uint8_t msg[16] = { cmd };
  xenon_smc_send_message(msg);
  uint64_t t0 = mftb();
  while (tb_diff_msec(mftb(), t0) < 500) {
    if (xenon_smc_receive_message(reply) == 0) {
      if (reply[0] == 0x83) continue;   /* asynchronous bulk message (IR, tray, power): not ours */
      if (reply[0] == cmd) return 0;
    }
  }
  return -1;
}

int xts_smc_version(uint8_t v[3])
{
  uint8_t r[16];
  if (smc_query(0x12, r) < 0) return -1;
  memcpy(smc_raw_12, r, 16);
  smc_have_12 = 1;
  v[0] = r[1]; v[1] = r[2]; v[2] = r[3];
  return 0;
}

int xts_smc_temps(uint16_t t[4])
{
  uint8_t r[16];
  if (smc_query(0x07, r) < 0) return -1;
  for (int i = 0; i < 4; i++) t[i] = (uint16_t)(r[i * 2 + 1] | (r[i * 2 + 2] << 8));
  return 0;
}

int xts_smc_rtc(uint8_t raw[5])
{
  uint8_t r[16];
  if (smc_query(0x04, r) < 0) return -1;
  memcpy(smc_raw_04, r, 16);
  smc_have_04 = 1;
  memcpy(raw, r + 1, 5);
  return 0;
}

void xts_plat_reboot(void) { xenon_smc_power_reboot(); for (;;) ; }

/* ---- identity ---- */
#define MFSPR64(n)                                                                    \
  ({ uint64_t v_ __attribute__((aligned(8))); unsigned long t_;                       \
     asm volatile("mfspr %0," #n "\n\tstd %0,0(%1)" : "=&r"(t_) : "b"(&v_) : "memory"); \
     (void)t_; v_; })

uint64_t xts_mfspr64(int spr)
{
  switch (spr) {
  case 287: return MFSPR64(287);     /* PVR */
  case 1008: return MFSPR64(1008);   /* HID0 */
  case 1009: return MFSPR64(1009);   /* HID1 */
  case 1012: return MFSPR64(1012);   /* HID4 */
  case 1017: return MFSPR64(1017);   /* HID6 */
  case 1023: return MFSPR64(1023);   /* PIR */
  case 318: return MFSPR64(318);     /* LPCR */
  }
  return 0;
}

static const char *console_type_name(int t)
{
  switch (t) {
  case REV_XENON: return "xenon";
  case REV_ZEPHYR: return "zephyr";
  case REV_FALCON: return "falcon";
  case REV_JASPER: return "jasper";
  case REV_TRINITY: return "trinity";
  case REV_CORONA: return "corona";
  case REV_CORONA_PHISON: return "corona-mmc";
  case REV_WINCHESTER: return "winchester";
  case REV_WINCHESTER_MMC: return "winchester-mmc";
  }
  return "unknown";
}

static const char *cpu_name(uint32_t pvr)
{
  /* thresholds as in libxenon xenon_get_console_type() */
  if (pvr <= 0x710300) return "Waternoose (90 nm)";
  if (pvr <= 0x710500) return "Loki (65 nm)";
  if (pvr <= 0x710800) return "Vejle (45 nm CGPU)";
  if (pvr <= 0x710A00) return "Oban (45 nm)";
  return "unknown";
}

static int console_type(void)
{
  if (cached_type < 0) { cached_type = xenon_get_console_type(); cached_dve = xenon_get_DVE(); }
  return cached_type;
}

static void put_raw16(struct xts_json *j, const char *key, const uint8_t *r, int have)
{
  char s[40];
  if (!have) { xj_key(j, key); xj_null(j); return; }
  for (int i = 0; i < 16; i++) snprintf(s + 2 * i, 3, "%02X", r[i]);
  xj_kv_str(j, key, s);
}

void xts_plat_default_tag(char *buf, size_t size)
{
  snprintf(buf, size, "%s-%08X-%04X", console_type_name(console_type()), (unsigned)xenon_get_CPU_PVR(),
           xenon_get_XenosID());
}

void xts_plat_sysinfo(struct xts_json *j)
{
  int type = console_type();
  uint32_t pvr = xenon_get_CPU_PVR();
  xj_kv_str(j, "name", "console");
  xj_kv_str(j, "console_type", console_type_name(type));
  xj_kv_int(j, "console_type_code", type);
  xj_kv_hex(j, "pvr", pvr, 8);
  xj_kv_str(j, "cpu", cpu_name(pvr));
  xj_kv_uint(j, "pir", xts_mfspr64(1023));
  xj_kv_hex(j, "hid0", xts_mfspr64(1008), 16);
  xj_kv_hex(j, "hid1", xts_mfspr64(1009), 16);
  xj_kv_hex(j, "hid4", xts_mfspr64(1012), 16);
  xj_kv_hex(j, "hid6", xts_mfspr64(1017), 16);
  xj_kv_hex(j, "lpcr", xts_mfspr64(318), 16);
  uint32_t msr;
  asm volatile("mfmsr %0" : "=r"(msr));
  xj_kv_hex(j, "msr_low", msr, 8);
  unsigned pci = xenon_get_PCIBridgeRevisionID();
  xj_kv_hex(j, "pci_bridge_rev", pci, 2);
  xj_kv_hex(j, "xenos_id", xenon_get_XenosID(), 4);
  xj_kv_hex(j, "dve", cached_dve, 2);
  xj_kv_str(j, "dve_kind", cached_dve >= 0x11 ? "HANA" : "ANA");
  xj_kv_uint(j, "ram_bytes", xenon_get_ram_size());
  uint8_t v[3];
  if (xts_smc_version(v) == 0) {
    char s[16];
    snprintf(s, sizeof s, "%u.%u.%u", v[0], v[1], v[2]);
    xj_kv_str(j, "smc_version", s);
  } else {
    xj_kv_str(j, "smc_version", "no reply");
  }
  int mmc = (type == REV_CORONA_PHISON || type == REV_WINCHESTER_MMC);
  xj_kv_str(j, "nand", mmc ? "mmc (phison)" : "raw nand");
  if (pci >= 0x70) xj_kv_hex(j, "sfcx_phison_reg", (uint32_t)sfcx_readreg(SFCX_PHISON), 8);
  xj_kv_int(j, "speed_code", xenon_get_speed());
  xj_kv_uint(j, "tb_hz_nominal", (uint64_t)PPC_TIMEBASE_FREQ);
  xj_key(j, "tb_hz_measured");
  xj_null(j);                                   /* the calib family measures it */
  uint16_t t[4];
  if (xts_smc_temps(t) == 0) {
    xj_key(j, "temps_c");
    xj_arr(j);
    for (int i = 0; i < 4; i++) xj_double(j, t[i] / 256.0);
    xj_end(j);
  }
  uint8_t rtc[5];
  if (xts_smc_rtc(rtc) == 0) {
    char s[16];
    snprintf(s, sizeof s, "%02X%02X%02X%02X%02X", rtc[4], rtc[3], rtc[2], rtc[1], rtc[0]);
    xj_kv_str(j, "smc_rtc_raw", s);
  }
  put_raw16(j, "smc_reply_12_raw", smc_raw_12, smc_have_12);   /* the 16 reply bytes of query 0x12 (version) */
  put_raw16(j, "smc_reply_04_raw", smc_raw_04, smc_have_04);   /* ... and of query 0x04 (RTC) */
  xj_kv_str(j, "usb_device", root);
  xj_kv_uint(j, "usb_sector_size", usb_sector_size);
#ifdef XTS_USB_FIX
  xj_kv_str(j, "usb_driver", "third_party/libxenon-usb (patched: transfer errors -> reset + retry, bounded retries)");
#else
  xj_kv_str(j, "usb_driver", "libxenon (unpatched)");
#endif
  xj_kv_bool(j, "disc_guard", xts_disc_guard_enabled());
  xj_kv_str(j, "volume_label", vol_label);
  if (vol_stat_ok) { xj_kv_uint(j, "volume_free_bytes", vol_free); xj_kv_uint(j, "volume_size_bytes", vol_size); }
  xj_kv_hex(j, "slot", xts_exec_slot_addr(), 8);
  xj_kv_hex(j, "kernel_buffer", xts_exec_kernel_addr(), 8);
  xj_kv_hex(j, "scratch", xts_scratch_addr(), 8);
  xj_kv_str(j, "crash_reports", "crash.json + progress.bin in the run directory (raw sectors via the disc guard)");
}

uintptr_t xts_plat_heap_limit(void) { return HEAP_LIMIT_EA; }

/* crash-path safe hex formatting into a fixed buffer */
static char *crash_puts(char *o, char *end, const char *s)
{
  while (*s && o < end) *o++ = *s++;
  return o;
}
static char *crash_hex(char *o, char *end, uint64_t v, int digits)
{
  for (int i = digits - 1; i >= 0 && o < end; i--) *o++ = "0123456789ABCDEF"[(v >> (4 * i)) & 15];
  return o;
}

/* Entered from crashdump (src/exec/vec.S) after an unhandled exception, on this PIR's emergency stack with MSR[SF]
 * cleared. r3 = vector, r4 = context (libxenon's: r0-r31, LR, CTR, CR, XER, SRR0, SRR1, DAR, PIR), r5 = how many
 * crashes this PIR was already handling (1 = the handler itself faulted), r6 = DSISR as found at crashdump (meaningful
 * for vectors 0x300/0x600). No stdio, no malloc, no libfat: xts_crash_handle writes crash.json through the disc guard's
 * raw API (unless storage is busy/poisoned), and the screen lines are drawn straight into the framebuffer (rows 0-5).
 * Nothing here returns. */
void xts_crash_entry(uint32_t vector, uint64_t *ctx, uint32_t depth, uint32_t dsisr)
{
  static char line[160];
  uint32_t pir;
  asm volatile("mfspr %0, 1023" : "=r"(pir));
  if (depth >= 1) {                                   /* nested: no files; one line on the status row */
    if (pir == 0) {
      char *o = line, *end = line + sizeof line - 1;
      o = crash_puts(o, end, "XTS: crash inside the crash handler, vector ");
      o = crash_hex(o, end, vector, 4);
      o = crash_puts(o, end, " srr0 ");
      o = crash_hex(o, end, ctx ? ctx[36] : 0, 8);
      o = crash_puts(o, end, " - report may be incomplete; reset to resume");
      *o = 0;
      status_draw(line);
    }
    for (;;) ;
  }
  if (pir == 0) xts_disc_set_notify(NULL);            /* a latch during the report must not printf */
  xts_crash_handle(vector, ctx, dsisr);               /* PIR 0: crash.json (raw); other PIRs: their RAM slot */
  if (pir == 0)
    for (int i = 0; i < XTS_CRASH_LINES; i++) status_draw_row(i, xts_crash_screen[i]);
  for (;;) ;
}

/* self.crash: a trap from C code outside the slot, the kernel buffer and every extra trap range -> libxenon's program
 * vector -> crashdump. Returns only if the trap was (wrongly) handled as an in-case trap. */
int xts_plat_self_crash(void)
{
  asm volatile("tw 31, 0, 0" ::: "memory");
  return 0;
}
