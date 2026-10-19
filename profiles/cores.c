/* cores: hardware bring-up step 2 - the core runtime. All roles run (PIR 0 test executor, PIR 2 suite, PIR 4
 * transmitter); self.cores ping-pongs 10,000 remote no-op calls (latency min/median/max), checks the role PIRs, the
 * heartbeats, cross-core code sync (code written on the suite core, executed on the test core), cross-core data sync
 * (data_sync_*: whether data handed between the cores arrives fresh, plain and after an xdcbt; the staged variants must
 * be 0) and the SMT load
 * entry/exit self-check on PIR 1, 3 and 5; self.net exercises fragmentation and the transmitter statistics.
 * Expected on the PC (tools/xts_recv.py): complete run, 0 missing seq, every self.cores / self.net record pass
 * (the unstaged data_sync variants are info: hardware facts). */
#include <xts/profile.h>

XTS_PROFILE(cores, "core runtime bring-up: remote calls, heartbeats, code and data sync, SMT exit self-check, transport",
  { .families = "self.cores" },
  { .families = "self.net" },
);
