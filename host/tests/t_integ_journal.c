/* Integration: the calibrated cycles-per-tick scale survives a reboot through the journal ("CPT" lines). */
#include <xts/xts.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "htest.h"
#include <core/core.h>

HTEST(journal_cpt_roundtrip)
{
  char dir[] = "/tmp/xts-cpt-XXXXXX";
  HT_CHECK(mkdtemp(dir) != NULL);
  char path[128];
  snprintf(path, sizeof path, "%s/journal.txt", dir);
  xts_journal_t j;
  HT_EQ_U64(xts_journal_load(&j, path), 0);
  HT_CHECK(!j.has_cpt);
  xts_journal_line(&j, "CPT 64");
  xts_journal_begin(&j, "r0:x:-:c0");
  xts_journal_line(&j, "CPT 63.75");
  xts_journal_end(&j, "r0:x:-:c0", 5);
  xts_journal_t k;
  HT_EQ_U64(xts_journal_load(&k, path), 0);          /* a new boot reads the last CPT line */
  HT_CHECK(k.has_cpt && k.cpt == 63.75);
  HT_EQ_U64(k.max_seq, 5);
  unlink(path);
  rmdir(dir);
}
