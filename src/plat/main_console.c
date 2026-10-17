/* Console entry point: XeLL loads xenon.elf from USB, libxenon starts here. */
#include <stdio.h>
#include <xts/xts.h>
#include <core/core.h>

int main(void)
{
  if (xts_plat_init(0, 0) < 0) xts_plat_halt("no USB storage: plug in a FAT32 (MBR) USB stick and reboot");
#ifdef XTS_WATCHDOG
  const char *wd = "ON (hangs reboot the console through the SMC)";
#else
  const char *wd = "OFF (power-cycle after a hang or crash)";
#endif
  printf("XTS: profile %s - %s\nXTS: build %s, watchdog %s\n", xts_profile.name, xts_profile.desc, xts_build_id(), wd);
  xts_run(&xts_profile, 0);
  xts_plat_halt("run finished: results are on the USB stick under xts/. Power off when done.");
  return 0;
}
