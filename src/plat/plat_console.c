/* Platform layer, console build (libxenon): screen, status row, timebase, SMC, identity and the crash entry. No storage
 * at all (USB is used only by XeLL to load xenon.elf): results leave through the UDP transport (net_console.c).
 * Never reads the CPU key, the keyvault or fuse lines 3-6. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <console/console.h>
#include <ppc/cache.h>
#include <ppc/timebase.h>
#include <time/time.h>
#include <xb360/xb360.h>
#include <xenon_nand/xenon_sfcx.h>
#include <xenon_smc/xenon_smc.h>
#include <xenon_soc/xenon_power.h>
#include <xenos/xenos.h>
#include <xts/xts.h>
#include <xts/net.h>
#include <xts/xcore.h>
#include <core/core.h>              /* build id, xts_core_xc_call (self.crash) */
#include <plat/plat_libxenon.h>     /* xenon_get_speed */
#include <plat/xcore_console.h>     /* crash slots, PIR, priority */

/* Heap bound for the big buffer and other large allocations (xts_plat_heap_limit): XTS's image + bss start at 0, the heap
 * grows above them (unbounded sbrk), the main stack sits at 0x1DE00000 and grows down, libxenon's per-thread HV contexts
 * are at 0x1DE80000-0x1DFFFFFF. Big allocations stay below EA 0x9C800000 (real 0x1C800000), ~22 MB under the stack. */
#define HEAP_LIMIT_EA 0x9C800000u

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

static int smc_query(uint8_t cmd, uint8_t reply[16]);

/* USB host controllers: halted, interrupts off (exactly libxenon's usb_shutdown, drivers/usb/usbmain.c). Called at boot
 * (XeLL loaded xenon.elf through them and may have left schedules running) and by libxenon's exit path
 * (xenon_syscalls.c shutdown_drivers). Defining it here keeps libxenon's whole USB stack out of the link: XTS never
 * initialises USB, so nothing can talk to a stick. */
static inline void wr32le(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = __builtin_bswap32(v); }
void usb_shutdown(void)
{
  wr32le(0xEA003028, 0);              /* EHCI: interrupts off */
  wr32le(0xEA005028, 0);
  wr32le(0xEA003020, 0);              /* EHCI: halt */
  wr32le(0xEA005020, 0);
  wr32le(0xEA002014, 1u << 31);       /* OHCI: interrupts off */
  wr32le(0xEA004014, 1u << 31);
  wr32le(0xEA002004, 0);              /* OHCI: halt (read back as libxenon does) */
  (void)*(volatile uint32_t *)0xEA002004;
  wr32le(0xEA004004, 0);
  (void)*(volatile uint32_t *)0xEA004004;
  asm volatile("sync" ::: "memory");
}

int xts_plat_init(int argc, char **argv)
{
  (void)argc; (void)argv;
  asm volatile("stw 2, 0(%0)\n\tstw 13, 0(%1)" : : "b"(&xts_crash_r2), "b"(&xts_crash_r13) : "memory");
  xenos_init(VIDEO_MODE_AUTO);
  console_init();
  status_init();
  console_set_cursor(0, 2);                                   /* row 0 is the status row, row 1 the crash row */
  cached_type = xenon_get_console_type();                     /* SMC queries without timeout in libxenon: once */
  cached_dve = xenon_get_DVE();
  return 0;
}

int xts_plat_is_host(void) { return 0; }
const char *xts_plat_name(void) { return "console"; }
void xts_plat_screen(const char *line) { printf("%s\n", line); }
void xts_plat_status(const char *line)
{
  if (sfb) status_draw(line);
  else printf("%s\n", line);
}

void xts_plat_halt(const char *why)
{
  printf("\nXTS: %s\nXTS: nothing is stored on the console: power off when done.\n", why ? why : "halted");
  for (;;) {
    xts_xc_prio_low();
    mdelay(100);
  }
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
  uint8_t m[6];
  xts_netc_mac(m);
  char ms[24];
  snprintf(ms, sizeof ms, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  xj_kv_str(j, "mac", ms);
  snprintf(ms, sizeof ms, "169.254.%u.%u", m[4], m[5]);
  xj_kv_str(j, "ip", ms);
  xj_kv_str(j, "transport", "udp: libxenon DHCP + lwIP (default) or NET_DRIVER=own; transmitter on PIR 4");
  xj_kv_hex(j, "run_nonce", xts_net_run_nonce(), 16);
  xj_kv_str(j, "storage", "none (USB is used only by XeLL to load xenon.elf)");
  xj_kv_hex(j, "slot", xts_exec_slot_addr(), 8);
  xj_kv_hex(j, "kernel_buffer", xts_exec_kernel_addr(), 8);
  xj_kv_hex(j, "scratch", xts_scratch_addr(), 8);
  xj_kv_str(j, "crash_reports", "CRASH message from the per-PIR uncached crash slots (xts/xcore.h)");
  xj_kv_bool(j, "single_core", xts_xc_single_core());
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
 * cleared, after vec.S filled this PIR's uncached crash slot. r3 = vector, r4 = context (libxenon's: r0-r31, LR, CTR,
 * CR, XER, SRR0, SRR1, DAR, PIR), r5 = how many crashes this PIR was already handling (1 = the handler itself faulted),
 * r6 = DSISR. Touches no shared state but its own measuring flag (PIR 0, xts_xc_on_crash) and one framebuffer row
 * (row 1 + PIR; sfb/font/colours are read-only after init). No stdio, no malloc. The report itself is sent by the
 * suite core (PIR 0 crashed: xts_xc_call returns XTS_XC_CRASHED) or by the transmitter (the suite core crashed).
 * Nothing here returns. */
void xts_crash_entry(uint32_t vector, uint64_t *ctx, uint32_t depth, uint32_t dsisr)
{
  static char line[XC_NPIR][160];
  uint32_t pir = xts_xc_mypir();
  (void)dsisr;
  if (depth == 0) {
    xts_xc_on_crash(pir);
    if (pir < XC_NPIR) {
      char *o = line[pir], *end = line[pir] + sizeof line[pir] - 1;
      o = crash_puts(o, end, "XTS CRASH PIR ");
      o = crash_hex(o, end, pir, 1);
      o = crash_puts(o, end, pir == 0 ? " (test core)" : pir == 2 ? " (suite)" : pir == 4 ? " (transmitter)" : " (load)");
      o = crash_puts(o, end, " vector ");
      o = crash_hex(o, end, vector, 4);
      o = crash_puts(o, end, " srr0 ");
      o = crash_hex(o, end, ctx ? ctx[36] : 0, 8);
      o = crash_puts(o, end, " lr ");
      o = crash_hex(o, end, ctx ? ctx[32] : 0, 8);
      o = crash_puts(o, end, " dar ");
      o = crash_hex(o, end, ctx ? ctx[38] : 0, 8);
      *o = 0;
      status_draw_row(1 + (int)pir, line[pir]);
    }
  }
  for (;;) xts_xc_prio_low();
}

/* self.crash: tw 31,0,0 on the TEST core, from C, outside the slot, the kernel buffer and every extra trap range ->
 * libxenon's program vector -> crashdump: PIR 0's crash slot, xts_xc_call returns XTS_XC_CRASHED and
 * xts_core_xc_call reports it (CRASH + END "crashed") and halts. Returns 0 only if the trap was (wrongly) handled. */
static void crash_job(void *a, void *b)
{
  (void)a; (void)b;
  asm volatile("tw 31, 0, 0" ::: "memory");
}

int xts_plat_self_crash(void)
{
  xts_core_xc_call(crash_job, NULL, NULL);
  return 0;
}
