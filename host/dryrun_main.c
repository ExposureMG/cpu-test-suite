/* Host dry run of a profile: `make host-dryrun PROFILE=<p>` builds this with profiles/<p>.c and runs it under the
 * receiver:  tools/xts_recv.py --once --port 0 --run-dir $(BUILD)/dryrun-<p>/run --exec xts-dryrun-<p>
 * Every block and case of the profile runs through the real scheduler and sink; single words are simulated by the model
 * where it can, every kernel is built with the encoder and every word decoded back. The results leave as UDP datagrams
 * to XTS_NET_HOST (set by xts_recv.py --exec; default 127.0.0.1:36360), exactly the messages the console sends.
 * Exit status 2 if any kernel word does not decode to the instruction it was built for. */
#include <stdio.h>
#include <xts/xts.h>
#include <xts/net.h>
#include <core/core.h>

int main(int argc, char **argv)
{
  xts_plat_init(argc, argv);
  if (xts_net_init() < 0) {
    fprintf(stderr, "host-dryrun: transport init failed (XTS_NET_HOST)\n");
    return 1;
  }
  xts_run_opts_t o = { 0 };
  o.dryrun = 1;
  int r = xts_run(&xts_profile, &o);
  printf("host-dryrun %s: %s (run %s sent to the receiver)\n", xts_profile.name, r == 0 ? "OK" : "PROBLEMS",
         xts_current_run_id ? xts_current_run_id : "?");
  return r < 0 ? 1 : r;
}
