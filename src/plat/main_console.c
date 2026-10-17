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
  int r = xts_run(&xts_profile, 0);
  /* xts_run unmounted the stick after the last file was closed (r == 0); xts_plat_halt reports the unmount state and
   * unmounts itself if a run ended early with storage still mounted */
  if (xts_plat_storage_status(NULL))
    xts_plat_halt("STORAGE ERROR: the run was stopped and nothing more was written. Power off and stop using this stick "
                  "until it has been checked on a PC (Windows: chkdsk /f, Linux: fsck.fat -n).");
  if (r == 0 || r == 2) xts_plat_halt("run finished: results are on the USB stick under xts/. Stick unmounted: safe to power off.");
  if (r == -4) xts_plat_halt("run finished, but the final flush/unmount reported an error: check the stick on a PC before reuse.");
  xts_plat_halt("run stopped early (see the messages above and run.log).");
  return 0;
}
