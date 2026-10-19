/* tlb_bisect: find the page count at which mem.hier's TLB case freezes the console (tlb.2048 did: whole-SoC lockup, no
 * report). calib first (cycles per tick), then probe.tlb in ascending page counts with a breadcrumb confirmed before
 * every case. Expected on the PC: either a complete run (no lockup up to 2048) or a run that stops; the last breadcrumb
 * in receive.json (last_crumb) then names the first page count that froze it - power-cycle the console. */
#include <xts/profile.h>

XTS_PROFILE(tlb_bisect, "TLB lockup bisect: probe.tlb at 1600-2048 pages, breadcrumb before every case",
  { .families = "calib" },
  { .families = "probe.tlb" },
);
