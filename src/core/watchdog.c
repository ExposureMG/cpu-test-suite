/* Watchdog hooks: weak no-ops. The SMC-reboot watchdog is gone (there is no resume to reboot into); a hang of the test
 * core is detected by the suite core's xts_xc_call timeout and reported (HANG + END, src/core/main.c). The hooks stay so
 * existing callers (calib, the exec layer) need no change; a platform may still override them. */
#include <xts/plat.h>

__attribute__((weak)) void xts_watchdog_start(uint32_t timeout_ms) { (void)timeout_ms; }
__attribute__((weak)) void xts_watchdog_kick(void) {}
__attribute__((weak)) void xts_watchdog_stop(void) {}
__attribute__((weak)) void xts_watchdog_set_timeout(uint32_t timeout_ms) { (void)timeout_ms; }
