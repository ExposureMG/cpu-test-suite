/* Platform layer, host build: stderr for the screen, a monotonic clock as a fake 49.875 MHz timebase, no SMC, no storage.
 * The host transport is src/plat/net_host.c (UDP to XTS_NET_HOST), the host core runtime src/plat/xcore_host.c. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xts/xts.h>
#include <core/core.h>          /* self.crash simulation */

static int quiet;

int xts_plat_init(int argc, char **argv)
{
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], "--quiet")) quiet = 1;
  if (getenv("XTS_QUIET")) quiet = 1;
  return 0;
}

int xts_plat_is_host(void) { return 1; }
const char *xts_plat_name(void) { return "host"; }
void xts_plat_screen(const char *line) { if (!quiet) fprintf(stderr, "%s\n", line); }
void xts_plat_status(const char *line) { if (!quiet) fprintf(stderr, "[status] %s\n", line); }
void xts_plat_halt(const char *why)
{
  fprintf(stderr, "halt: %s\n", why);
  fflush(NULL);
  exit(1);
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
uint64_t xts_mfspr64(int spr) { (void)spr; return 0; }

void xts_plat_sysinfo(struct xts_json *j)
{
  xj_kv_str(j, "name", "host");
  xj_kv_str(j, "note", "PC dry run: nothing executed on a Xenon CPU");
}

void xts_plat_default_tag(char *buf, size_t size) { snprintf(buf, size, "host"); }

uintptr_t xts_plat_heap_limit(void) { return UINTPTR_MAX; }

/* self.crash on the PC: never crashes the host process. Host tests set xts_host_crash_sim in a forked child: then the
 * host test core "crashes" inside xts_xc_call (xcore_host.c), and xts_core_xc_call reports the loss exactly as the
 * console's exec layer does (crash record, CRASH, END crashed, halt = exit(1)). */
static void crash_fn(void *a, void *b)
{
  (void)a;
  (void)b;
  uint64_t gpr[32];
  for (int r = 0; r < 32; r++) gpr[r] = 0x1000u * (uint64_t)r;
  xts_host_xc_crash(0x700, (uint64_t)(uintptr_t)crash_fn & 0xFFFFFFFFu, gpr);
}

int xts_plat_self_crash(void)
{
  if (!xts_host_crash_sim) return -1;
  xts_core_xc_call(crash_fn, NULL, NULL);   /* does not return: the loss is reported and the process exits */
  return 0;
}
