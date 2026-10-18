/* Console entry point: XeLL loads xenon.elf from USB, libxenon starts here on PIR 0.
 *
 * Boot: stop the NIC's and the USB controllers' DMA (XeLL may have left them running), arm the crash slots, screen up
 * (build id + profile), bring the transport up (NIC reset, PHY link), then xts_xc_start_roles: PIR 4 transmitter, PIR 2
 * suite (xts_run), PIR 0 becomes the test executor and never comes back here.
 *
 * Single-core bring-up profiles (profiles/net_hello.c) define xts_profile_single_core = 1 and their own entry
 * xts_profile_main: only PIR 0 (running that entry) and the transmitter on PIR 4 are started. */
#include <stdio.h>
#include <xts/xts.h>
#include <xts/net.h>
#include <xts/xcore.h>
#include <core/core.h>
#include <plat/xcore_console.h>

extern int xts_profile_main(void) __attribute__((weak));
void usb_shutdown(void);             /* plat_console.c */

static int suite_main(void)
{
  int r = xts_run(&xts_profile, NULL);
  int d = xts_net_drain(20u * 1000u * 1000u);
  char msg[160];
  snprintf(msg, sizeof msg, "%s (xts_run %d)%s",
           r == 0 ? "run finished: results were broadcast (see tools/xts_recv.py on the PC)" : "run stopped early", r,
           d < 0 ? "; WARNING: the transmitter did not drain its queue in 20 s" : "");
  xts_plat_halt(msg);
}

int main(void)
{
  xts_netc_quiesce();                 /* XeLL's NIC and USB controllers may still be doing DMA: stop them first */
  usb_shutdown();
  xts_xc_init_early();
  if (xts_plat_init(0, 0) < 0) xts_plat_halt("screen init failed");
  char line[200];
  snprintf(line, sizeof line, "XTS %s (Xenon CPU test suite)  build %s", XTS_VERSION, xts_build_id());
  xts_plat_screen(line);
  xts_plat_status(line);
  snprintf(line, sizeof line, "XTS: profile %s - %s", xts_profile.name, xts_profile.desc);
  xts_plat_screen(line);
  xts_plat_screen("XTS: no storage: results leave as UDP broadcasts; run tools/xts_recv.py on a PC on the same link");
  int r = xts_net_init();
  if (r < 0) {
    snprintf(line, sizeof line, "network not available (%d): nothing can be reported. Check the cable and reboot.", r);
    xts_plat_halt(line);
  }
  r = xts_xc_start_roles(xts_profile_main ? xts_profile_main : suite_main);
  int d = xts_net_drain(20u * 1000u * 1000u);
  snprintf(line, sizeof line, "%s (%d)%s", r < 0 ? "core roles could not be started" : "finished", r,
           d < 0 ? "; the transmitter did not drain its queue in 20 s" : "");
  xts_plat_halt(line);
}
