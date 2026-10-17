/* Platform layer, console build (libxenon + libfat): screen, USB mass storage, timebase, SMC, identity, crash page.
 * No network. Never reads the CPU key, the keyvault or fuse lines 3-6. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <console/console.h>
#include <libfat/fat.h>
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
#include <core/core.h>              /* crash page record, build id */
#include <plat/plat_libxenon.h>     /* xenon_get_speed */

/* Reserved crash page: EA 0x9D000000 (real 0x1D000000), 64 KB. Real-memory map at the next boot (what may overwrite it):
 *   XeLL stage 1 (xell-reloaded source/lv1/xell-*.lds): code 0x1C000000 (the NAND image is copied there), bss with the
 *     1 MB stage-2 ELF buffer from 0x1C080000, stacks 0x1DFA0000-0x1E000000 (64 KB per thread below 0x1E000000);
 *   XeLL stage 2 (a libxenon app): image + bss 0x00000000-0x00701600, heap above it (xenon.elf file buffer), ELF loader
 *     buffers 0x05FE0000-0x14000000 (libxenon drivers/elf/elf.c), main stack 0x1DE00000 down, per-thread HV contexts
 *     0x1DE80000-0x1DFFFFFF, framebuffer 0x1E000000;
 *   XTS itself: image + bss from 0, heap above (unbounded sbrk; the big buffer and the other large allocations check
 *     xts_plat_heap_limit), main stack 0x1DE00000 down (~14 MB above the page), HV contexts 0x1DE80000-0x1DFFFFFF.
 * The old place, 0x1C000000, is XeLL stage 1's own code: overwritten on every boot. Whether 0x1D000000 survives an SMC
 * reboot / the CB/CD boot stages is NOT verified: sysinfo.json "crashpage_survived" reports it; the journal (CRASH and
 * CASE lines) is the fallback. */
#define CRASH_PAGE_EA 0x9D000000u
#define CRASH_PAGE_SIZE 0x10000u
#define HEAP_LIMIT_EA 0x9C800000u     /* big allocations stay 8 MB below the page (room for small ones after them) */

static char root[16] = "";
static char vol_label[32] = "";
static unsigned long long vol_free = 0, vol_size = 0;
static int vol_stat_ok;
static int cached_type = -1;
static unsigned cached_dve;
static uint8_t smc_raw_12[16], smc_raw_04[16];
static int smc_have_12, smc_have_04;

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
static void status_draw(const char *s)
{
  if (!sfb) return;
  uint32_t fg = console_color[0], bg = console_color[1];   /* inverted: a bar */
  int y0 = sfb_offy;
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

static void find_root(void)
{
  root[0] = 0;
  for (int i = 0; i < STD_MAX; i++) {
    const devoptab_t *d = devoptab_list[i];
    if (d && d->name && !strncmp(d->name, "ud", 2)) {
      snprintf(root, sizeof root, "%s:", d->name);
      return;
    }
  }
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
  for (int attempt = 0; attempt < 10 && !root[0]; attempt++) {
    uint64_t t0 = mftb();
    while (tb_diff_msec(mftb(), t0) < 500) usb_do_poll();      /* let the device enumerate */
    fatInitDefault();
    find_root();
  }
  if (!root[0]) { printf("XTS: no USB FAT volume found (FAT32 on an MBR partition, or a superfloppy, is required)\n"); return -1; }
  fatGetVolumeLabel(root, vol_label);
  vol_label[sizeof vol_label - 1] = 0;
  printf("XTS: output on USB device %s, volume label \"%s\"; checking free space...\n", root, vol_label);
  char rp[24];
  snprintf(rp, sizeof rp, "%s/", root);
  struct statvfs sv;
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
void xts_plat_poll(void) { usb_do_poll(); }
void xts_plat_screen(const char *line) { printf("%s\n", line); }
void xts_plat_status(const char *line)
{
  if (sfb) status_draw(line);
  else printf("%s\n", line);
}

void xts_plat_halt(const char *why)
{
  printf("\nXTS: %s\n", why);
  for (;;) {
    usb_do_poll();
    mdelay(100);
  }
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
  xj_kv_str(j, "volume_label", vol_label);
  if (vol_stat_ok) { xj_kv_uint(j, "volume_free_bytes", vol_free); xj_kv_uint(j, "volume_size_bytes", vol_size); }
  xj_kv_hex(j, "slot", xts_exec_slot_addr(), 8);
  xj_kv_hex(j, "kernel_buffer", xts_exec_kernel_addr(), 8);
  xj_kv_hex(j, "scratch", xts_scratch_addr(), 8);
  xj_kv_hex(j, "crash_page", CRASH_PAGE_EA, 8);
}

/* ---- crash page ---- */
void *xts_plat_crash_page(size_t *size)
{
  if (size) *size = CRASH_PAGE_SIZE;
  return (void *)CRASH_PAGE_EA;
}

void xts_plat_crash_page_sync(void)
{
  asm volatile("sync" ::: "memory");
  memdcbf((void *)CRASH_PAGE_EA, sizeof(xts_crashpage_t));
  asm volatile("sync" ::: "memory");
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
 * crashes this PIR was already handling (1 = the handler itself faulted). Nothing here returns. */
void xts_crash_entry(uint32_t vector, uint64_t *ctx, uint32_t depth)
{
  static char line[160];
  uint32_t pir;
  asm volatile("mfspr %0, 1023" : "=r"(pir));
  if (depth >= 1) {                                   /* nested: no stdio, no files; one line on the status row */
    if (pir == 0) {
      char *o = line, *end = line + sizeof line - 1;
      o = crash_puts(o, end, "XTS: crash inside the crash handler, vector ");
      o = crash_hex(o, end, vector, 4);
      o = crash_puts(o, end, " srr0 ");
      o = crash_hex(o, end, ctx ? ctx[36] : 0, 8);
      o = crash_puts(o, end, " - power-cycle to resume");
      *o = 0;
      status_draw(line);
    }
    for (;;) ;
  }
  xts_crash_handle(vector, ctx);                      /* crash page; PIR 0 in a case: journal CRASH line (fsync'd) */
  if (pir == 0) {
    char *o = line, *end = line + sizeof line - 1;
    o = crash_puts(o, end, "XTS CRASH vector ");
    o = crash_hex(o, end, vector, 4);
    o = crash_puts(o, end, " srr0 ");
    o = crash_hex(o, end, ctx[36], 8);
    o = crash_puts(o, end, " dar ");
    o = crash_hex(o, end, ctx[38], 8);
    o = crash_puts(o, end, " - recorded; reboot to resume");
    *o = 0;
    status_draw(line);
    printf("\n\nXTS CRASH: vector %X srr0 %08X srr1 %016llX dar %08X lr %08X\n", (unsigned)vector, (unsigned)ctx[36],
           (unsigned long long)ctx[37], (unsigned)ctx[38], (unsigned)ctx[32]);
    printf("Recorded in the RAM crash page and (inside a case) as a CRASH line in journal.txt.\n"
           "SMC reboot / power-cycle (or let the watchdog reboot) and boot the same ELF to resume the run.\n");
  }
  for (;;) ;
}

/* self.crash: a trap from C code outside the slot, the kernel buffer and every extra trap range -> libxenon's program
 * vector -> crashdump. Returns only if the trap was (wrongly) handled as an in-case trap. */
int xts_plat_self_crash(void)
{
  asm volatile("tw 31, 0, 0" ::: "memory");
  return 0;
}
