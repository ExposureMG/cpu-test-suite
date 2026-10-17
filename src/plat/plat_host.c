/* Platform layer, host build: files under a directory, monotonic clock as a fake 49.875 MHz timebase, no SMC.
 * The crash page is a file (<root>/.xts_crashpage.bin) so host tests can simulate "RAM survived the reboot". */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xts/xts.h>
#include <core/core.h>          /* crash path (self.crash simulation) */

static char root[512] = "";
static int quiet;
static uint8_t crash_page[4096];   /* >= sizeof(xts_crashpage_t) (main record + thread slots) */

static void crash_page_path(char *p, size_t n) { snprintf(p, n, "%s/.xts_crashpage.bin", root); }

int xts_plat_init(int argc, char **argv)
{
  const char *r = getenv("XTS_ROOT");
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--root") && i + 1 < argc) r = argv[++i];
    else if (!strcmp(argv[i], "--quiet")) quiet = 1;
  }
  if (getenv("XTS_QUIET")) quiet = 1;
  snprintf(root, sizeof root, "%s", r ? r : "xts-host-out");
  xts_mkdirs(root);
  char p[600];
  crash_page_path(p, sizeof p);
  FILE *f = fopen(p, "rb");
  if (f) {
    if (fread(crash_page, 1, sizeof crash_page, f) != sizeof crash_page) memset(crash_page, 0, sizeof crash_page);
    fclose(f);
  }
  return 0;
}

int xts_plat_is_host(void) { return 1; }
const char *xts_plat_root(void) { return root; }
const char *xts_plat_name(void) { return "host"; }
void xts_plat_poll(void) {}
void xts_plat_screen(const char *line) { if (!quiet) fprintf(stderr, "%s\n", line); }
void xts_plat_status(const char *line) { if (!quiet) fprintf(stderr, "[status] %s\n", line); }
void xts_plat_halt(const char *why) { fprintf(stderr, "halt: %s\n", why); exit(1); }

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
  snprintf(tmp, sizeof tmp, "%s", path);
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
  FILE *f = fopen(path, "ab");
  if (!f) return -1;
  size_t w = fwrite(data, 1, n, f);
  fflush(f);
  fsync(fileno(f));
  fclose(f);
  return w == n ? 0 : -1;
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
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  size_t n = strlen(text), w = fwrite(text, 1, n, f);
  fflush(f);
  fsync(fileno(f));
  fclose(f);
  return w == n ? 0 : -1;
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
}

void xts_plat_default_tag(char *buf, size_t size) { snprintf(buf, size, "host"); }

void *xts_plat_crash_page(size_t *size)
{
  if (size) *size = sizeof crash_page;
  return crash_page;
}

void xts_plat_crash_page_sync(void)
{
  char p[600];
  crash_page_path(p, sizeof p);
  FILE *f = fopen(p, "wb");
  if (!f) return;
  fwrite(crash_page, 1, sizeof crash_page, f);
  fclose(f);
}

uintptr_t xts_plat_heap_limit(void) { return UINTPTR_MAX; }

/* self.crash on the PC: never crashes the host process by itself. Host tests set xts_host_crash_sim in a forked boot:
 * then the crash path runs as on the console (crash page + journal CRASH line from a fake program-interrupt context) and
 * the "boot" ends like a power cycle. */
int xts_host_crash_sim;

int xts_plat_self_crash(void)
{
  if (!xts_host_crash_sim) return -1;
  uint64_t ctx[40];
  memset(ctx, 0, sizeof ctx);
  ctx[36] = (uint64_t)(uintptr_t)xts_plat_self_crash & 0xFFFFFFFFu;   /* SRR0: outside every trap range */
  ctx[37] = 0x9000000000000000ull | ((uint64_t)XTS_CAUSE_TRAP << 16);
  ctx[39] = 0;                                                       /* PIR 0: the main thread */
  xts_crash_handle(0x700, ctx);
  _exit(9);
}
