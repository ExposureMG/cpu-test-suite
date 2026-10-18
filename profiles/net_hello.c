/* net_hello: hardware bring-up step 1 - the NIC TX path and the PC receiver, nothing else.
 *
 * Console: only PIR 0 and the transmitter (PIR 4) run (xts_profile_single_core; cores 1's threads are never started).
 * xts_profile_main replaces the suite: it sends HELLO x3, 100 LOG lines and END x3 straight through xts_net_send (no
 * scheduler, no families, no exec), waits until everything is on the wire and halts with the transport statistics on
 * screen. Expected on the PC (tools/xts_recv.py): one run, HELLO and END seen, 0 missing seq, LOG 100, REC 0.
 * Host dry run: the profile row runs self.net through the normal suite (xts_profile_main is not used there). */
#include <stdio.h>
#include <string.h>
#include <xts/profile.h>
#include <xts/xts.h>
#include <xts/json.h>
#include <xts/net.h>
#include <core/core.h>

XTS_PROFILE(net_hello, "network bring-up: HELLO, 100 LOG lines, END (console: PIR 0 + transmitter only)",
  { .families = "self.net" },
);

const int xts_profile_single_core = 1;

#define NH_LOGS 100

static int send3(int type, const char *s, uint32_t *last)
{
  int ok = 0;
  for (int i = 0; i < 3; i++) {
    uint32_t q = xts_net_send(type, s, (uint32_t)strlen(s));
    if (q) { *last = q; ok++; }
  }
  return ok;
}

int xts_profile_main(void)
{
  static char hello[8192], line[160], tag[64];
  uint32_t last = 0;
  int hellos, logs = 0, ends;
  xts_plat_default_tag(tag, sizeof tag);
  xts_json_t j;
  xj_init(&j, hello, sizeof hello);
  xj_obj(&j);
  xj_key(&j, "sysinfo");
  xj_obj(&j);
  xj_kv_str(&j, "suite", "xts");
  xj_kv_str(&j, "version", XTS_VERSION);
  xj_kv_str(&j, "build_id", xts_build_id());
  xj_kv_str(&j, "git_hash", xts_git_hash());
  xj_kv_str(&j, "profile", xts_profile.name);
  xj_kv_str(&j, "run_id", "net_hello");
  snprintf(line, sizeof line, "%016llx", (unsigned long long)xts_net_run_nonce());
  xj_kv_str(&j, "run_nonce", line);
  xj_kv_str(&j, "console_tag", tag);
  xj_key(&j, "platform");
  xj_obj(&j);
  xts_plat_sysinfo(&j);
  xj_end(&j);
  xj_end(&j);
  xj_kv_str(&j, "profile", xts_profile.name);
  xj_key(&j, "plan");
  xj_obj(&j);
  xj_kv_int(&j, "blocks", 0);
  xj_kv_int(&j, "cases", 0);
  xj_end(&j);
  xj_key(&j, "cores");
  xj_obj(&j);
  xj_kv_int(&j, "test", 0);
  xj_kv_int(&j, "suite", 0);
  xj_kv_int(&j, "net", 4);
  xj_kv_bool(&j, "single_core", 1);
  xj_end(&j);
  xj_kv_str(&j, "build_id", xts_build_id());
  xj_end(&j);
  hellos = send3(XTS_PK_HELLO, xj_finish(&j), &last);
  uint64_t t0 = xts_tb();
  for (int i = 1; i <= NH_LOGS; i++) {
    snprintf(line, sizeof line, "net_hello: line %d/%d tb=%llu", i, NH_LOGS, (unsigned long long)(xts_tb() - t0));
    uint32_t q = xts_net_send(XTS_PK_LOG, line, (uint32_t)strlen(line));
    if (q) { last = q; logs++; }
  }
  char end[256];
  snprintf(end, sizeof end,
           "{\"status\":\"%s\",\"last_seq\":%u,\"messages\":{\"HELLO\":%d,\"LOG\":%d},\"rec_hash\":\"cbf29ce484222325\"}",
           hellos == 3 && logs == NH_LOGS ? "complete" : "aborted", (unsigned)last, hellos, logs);
  ends = send3(XTS_PK_END, end, &last);
  int d = xts_net_drain(10u * 1000u * 1000u);
  xts_net_stats_t st;
  xts_net_stats(&st);
  snprintf(line, sizeof line, "net_hello: queued HELLO %d LOG %d END %d; frames %llu copies %llu tx_timeouts %llu failures %llu%s",
           hellos, logs, ends, (unsigned long long)st.frames, (unsigned long long)st.frame_copies,
           (unsigned long long)st.tx_timeouts, (unsigned long long)st.send_failures, d < 0 ? " (DRAIN TIMEOUT)" : "");
  xts_plat_screen(line);
  return hellos == 3 && logs == NH_LOGS && ends == 3 && d == 0 ? 0 : -1;
}
