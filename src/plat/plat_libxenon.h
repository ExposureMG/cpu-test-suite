/* libxenon symbols XTS uses that are missing from libxenon's public headers (console builds only).
 * Include as <plat/plat_libxenon.h>. Sources (libxenon tree, read-only): drivers/xenon_soc/xenon_power.c,
 * startup_from_xell.S. Keep the declarations exactly as libxenon defines them. */
#ifndef XTS_PLAT_LIBXENON_H
#define XTS_PLAT_LIBXENON_H
#ifdef XTS_CONSOLE
#include <stdint.h>

/* drivers/xenon_soc/xenon_power.c */
int xenon_get_speed(void);
void xenon_set_speed(int new_speed, int vid_delta);
void xenon_thread_sleep(void);
void wakeup_cpus(void);
void prepare_sleep(uint64_t *save);

/* drivers/usb/tinyehci/usbstorage.c (or the patched copy in third_party/libxenon-usb with USB_FIX=1): the three USB
 * mass-storage DISC_INTERFACEs libfat's fatInit uses ("uda", "udb", "udc"), and the sector size of a mounted device */
#include <diskio/disc_io.h>
extern DISC_INTERFACE usb2mass_ops_0, usb2mass_ops_1, usb2mass_ops_2;
s32 USBStorage_Get_Capacity(int device, u32 *sector_size);

/* startup_from_xell.S: per-thread task/stack slots (wait[2*pir] task, wait[2*pir+1] stack), thread start lock and the
 * alive mask */
extern volatile int xts_lx_wait[12] __asm__("wait");
extern unsigned int secondary_lock;
extern volatile int secondary_alive;
#endif
#endif
