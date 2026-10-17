/* Host dry run of a profile: `make host-dryrun PROFILE=<p>` builds this with profiles/<p>.c and runs
 *   xts-dryrun-<p> --root $(BUILD)/dryrun-<p> [--quiet]
 * Every block and case of the profile runs through the real scheduler, sinks and journal; single words are simulated by
 * the model where it can, every kernel is built with the encoder and every word decoded back. Exit status 2 if any
 * kernel word does not decode to the instruction it was built for. */
#include <stdio.h>
#include <xts/xts.h>
#include <core/core.h>

int main(int argc, char **argv)
{
  xts_plat_init(argc, argv);
  xts_run_opts_t o = { 0 };
  o.dryrun = 1;
  int r = xts_run(&xts_profile, &o);
  printf("host-dryrun %s: %s (results in %s/xts/host/%s)\n", xts_profile.name, r == 0 ? "OK" : "PROBLEMS", xts_plat_root(),
         xts_current_run_id ? xts_current_run_id : "?");
  return r;
}
