/* Run journal (journal.txt): one line per event, appended durably (the file stays open for the run; every line is
 * fflush'ed + fsync'ed) at block boundaries only - BEFORE a block's cases start and after they end - so a hang, crash or
 * power cut leaves a BEGIN without END that the next boot turns into a hang/crash record. Nothing is written to it while
 * cases run or from the crash path (the case is named by crash.json / progress.bin, src/core/slot.c).
 *
 *   START <profile> <build_id>        run created
 *   BOOT <n>                          a later boot resumed the run
 *   BEGIN <key>                       block about to run
 *   END <key> seq=<n>                 block done; n = last record seq
 *   QUAR <key> case=<i> status=<s>    case i (relative to the block, -1 = whole block) quarantined after a hang/crash
 *   RETRY <key>                       the block hung/crashed without a named case: re-run once with a progress.bin
 *                                     breadcrumb before every case
 *   PART <key> k=<n> seq=<s>          the block's buffer filled: cases 0..n-1 were written at a case boundary (a re-run
 *                                     of the block starts at case n)
 *   REPORT tb=<t>                     the crash.json report with "tb":<t> has been turned into records (not again)
 *   CPT <cycles per tick>             calibration scale (survives a resume)
 *   DONE                              run finished
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"

static uint32_t hkey(const char *k)
{
  uint32_t h = 2166136261u;
  while (*k) { h ^= (uint8_t)*k++; h *= 16777619u; }
  return h;
}

static void rehash(xts_journal_t *j, int cap)
{
  int *h = malloc(sizeof(int) * cap);
  if (!h) return;
  for (int i = 0; i < cap; i++) h[i] = -1;
  for (int i = 0; i < j->n; i++) {
    uint32_t p = hkey(j->e[i].key) & (cap - 1);
    while (h[p] >= 0) p = (p + 1) & (cap - 1);
    h[p] = i;
  }
  free(j->hidx);
  j->hidx = h;
  j->hcap = cap;
}

static xts_jentry_t *get(xts_journal_t *j, const char *key, int create)
{
  if (j->hcap) {
    uint32_t p = hkey(key) & (j->hcap - 1);
    while (j->hidx[p] >= 0) {
      if (!strcmp(j->e[j->hidx[p]].key, key)) return &j->e[j->hidx[p]];
      p = (p + 1) & (j->hcap - 1);
    }
  }
  if (!create) return NULL;
  if (j->n >= j->cap) {
    int nc = j->cap ? j->cap * 2 : 256;
    xts_jentry_t *ne = realloc(j->e, sizeof *ne * nc);
    if (!ne) return NULL;
    j->e = ne;
    j->cap = nc;
  }
  xts_jentry_t *e = &j->e[j->n++];
  memset(e, 0, sizeof *e);
  snprintf(e->key, sizeof e->key, "%s", key);
  if (2 * j->n > j->hcap) rehash(j, j->hcap ? j->hcap * 2 : 512);
  else {
    uint32_t p = hkey(e->key) & (j->hcap - 1);
    while (j->hidx[p] >= 0) p = (p + 1) & (j->hcap - 1);
    j->hidx[p] = j->n - 1;
  }
  return e;
}

xts_jentry_t *xts_journal_find(xts_journal_t *j, const char *key) { return get(j, key, 0); }

static void apply_line(xts_journal_t *j, char *line)
{
  char cmd[16], key[200];
  line[strcspn(line, "\r\n")] = 0;
  if (sscanf(line, "%15s %199s", cmd, key) < 1) return;
  if (!strcmp(cmd, "DONE")) { j->done = 1; return; }
  if (!strcmp(cmd, "CPT")) {
    double v = strtod(key, NULL);
    if (v > 0) { j->cpt = v; j->has_cpt = 1; }
    return;
  }
  if (!strcmp(cmd, "REPORT")) {
    const char *t = strstr(line, "tb=");
    if (t) snprintf(j->report_tb, sizeof j->report_tb, "%.23s", t + 3);
    return;
  }
  if (!strcmp(cmd, "BEGIN")) {
    xts_jentry_t *e = get(j, key, 1);
    if (e) { e->begun++; e->open = 1; }
  } else if (!strcmp(cmd, "RETRY")) {
    xts_jentry_t *e = get(j, key, 1);
    if (e) { e->retry = 1; e->open = 0; }
  } else if (!strcmp(cmd, "PART")) {
    xts_jentry_t *e = get(j, key, 1);
    const char *k = strstr(line, " k="), *s = strstr(line, " seq=");
    if (s) {
      unsigned long long v = strtoull(s + 5, NULL, 10);
      if (v > j->max_seq) j->max_seq = v;
    }
    if (e && k) e->part_k = atoi(k + 3);
  } else if (!strcmp(cmd, "END")) {
    xts_jentry_t *e = get(j, key, 1);
    const char *s = strstr(line, "seq=");
    if (s) {
      unsigned long long v = strtoull(s + 4, NULL, 10);
      if (v > j->max_seq) j->max_seq = v;
    }
    if (e) { e->ended++; e->open = 0; e->part_k = 0; }
  } else if (!strcmp(cmd, "QUAR")) {
    xts_jentry_t *e = get(j, key, 1);
    const char *s = strstr(line, "case=");
    int c = s ? atoi(s + 5) : -1;
    if (e) {
      e->open = 0;
      if (c < 0 || e->nquar >= 16) e->quar_all = 1;
      else e->quar[e->nquar++] = c;
    }
  }
}

int xts_journal_load(xts_journal_t *j, const char *path)
{
  memset(j, 0, sizeof *j);
  snprintf(j->path, sizeof j->path, "%s", path);
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  char line[400];
  while (fgets(line, sizeof line, f)) apply_line(j, line);
  fclose(f);
  return 0;
}

int xts_journal_open(xts_journal_t *j)
{
  return xts_pf_open(&j->pf, j->path);
}

void xts_journal_close(xts_journal_t *j)
{
  xts_pf_close(&j->pf);
}

/* durable: written + fflush + fsync before the caller goes on (BEGIN before the block's cases, END after them) */
int xts_journal_raw(xts_journal_t *j, const char *line, size_t n)
{
  if (!j->pf.f && !j->pf.path[0]) snprintf(j->pf.path, sizeof j->pf.path, "%s", j->path);
  int r = xts_pf_write(&j->pf, line, n);
  if (xts_pf_sync(&j->pf) < 0) r = -1;
  return r;
}

int xts_journal_line(xts_journal_t *j, const char *line)
{
  char tmp[400];
  int n = snprintf(tmp, sizeof tmp, "%s\n", line);
  if (n <= 0 || n >= (int)sizeof tmp) return -1;
  int r = xts_journal_raw(j, tmp, (size_t)n);
  tmp[n - 1] = 0;
  apply_line(j, tmp);
  return r;
}

int xts_journal_retry(xts_journal_t *j, const char *key)
{
  char l[220];
  snprintf(l, sizeof l, "RETRY %s", key);
  return xts_journal_line(j, l);
}

int xts_journal_part(xts_journal_t *j, const char *key, int k, uint64_t seq)
{
  char l[260];
  snprintf(l, sizeof l, "PART %s k=%d seq=%llu", key, k, (unsigned long long)seq);
  return xts_journal_line(j, l);
}

int xts_journal_report(xts_journal_t *j, uint64_t tb)
{
  char l[64];
  snprintf(l, sizeof l, "REPORT tb=%llu", (unsigned long long)tb);
  return xts_journal_line(j, l);
}

int xts_journal_begin(xts_journal_t *j, const char *key)
{
  char l[220];
  snprintf(l, sizeof l, "BEGIN %s", key);
  return xts_journal_line(j, l);
}

int xts_journal_end(xts_journal_t *j, const char *key, uint64_t seq)
{
  char l[240];
  snprintf(l, sizeof l, "END %s seq=%llu", key, (unsigned long long)seq);
  return xts_journal_line(j, l);
}

int xts_journal_quar(xts_journal_t *j, const char *key, int case_rel, const char *status)
{
  char l[260];
  snprintf(l, sizeof l, "QUAR %s case=%d status=%s", key, case_rel, status);
  return xts_journal_line(j, l);
}

int xts_journal_is_quar(const xts_jentry_t *e, int case_rel)
{
  if (!e) return 0;
  if (e->quar_all) return 1;
  for (int i = 0; i < e->nquar; i++)
    if (e->quar[i] == case_rel) return 1;
  return 0;
}
