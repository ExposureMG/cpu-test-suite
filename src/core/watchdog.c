/* Watchdog hook. These are WEAK defaults (no watchdog); src/smt/ provides the real implementation (heartbeat thread on
 * core 2 calling xenon_smc_power_reboot() after a timeout, opt-in WATCHDOG=1) by defining the same functions
 * (xts_watchdog_set_timeout: the per-block timeout from the family kind, see src/core/main.c). */
#include <xts/plat.h>

__attribute__((weak)) void xts_watchdog_start(uint32_t timeout_ms) { (void)timeout_ms; }
__attribute__((weak)) void xts_watchdog_kick(void) {}
__attribute__((weak)) void xts_watchdog_stop(void) {}
__attribute__((weak)) void xts_watchdog_set_timeout(uint32_t timeout_ms) { (void)timeout_ms; }
