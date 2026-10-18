/* In-RAM results store: the REC lines of the current run, kept per family for xts_results_foreach (family .finish hooks
 * summarise their own records). Nothing is written anywhere; the store lives until the next xts_results_reset.
 *
 * Bounded: XTS_RESULTS_CAP bytes in total (line text + 12 bytes per line). When the total reaches the cap, the lines of
 * families WITHOUT a .finish hook are freed and their later lines are not stored (only xts_results_foreach(NULL, ...)
 * would read them); lines of families WITH a .finish hook are always kept up to the cap counted over them alone, then
 * dropped (counted; one run.log line). xts_results_foreach(family) of a family that lost lines logs that its view is
 * incomplete. Chunks of 256 KB per family, lines stored as [u64 seq][u32 len][text][NUL]. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"

#define CHUNK (256u << 10)
#define MAXFAM 256

typedef struct chunk { struct chunk *next; uint32_t used; uint8_t data[]; } chunk_t;
typedef struct fam {
  char name[48];
  int finish;                     /* the family has a .finish hook (its lines are always kept) */
  chunk_t *head, *tail;
  uint64_t bytes, lines, dropped;
  int evicted;
} fam_t;

static fam_t fams[MAXFAM];
static int nfam, last;
static uint64_t total_bytes, finish_bytes, total_lines, dropped_lines;
static int evicted_others, told;
static uint32_t cap = XTS_RESULTS_CAP;

void xts_results_set_cap(uint32_t c) { cap = c ? c : XTS_RESULTS_CAP; }

static void free_fam(fam_t *f)
{
  for (chunk_t *c = f->head, *n; c; c = n) {
    n = c->next;
    free(c);
  }
  f->head = f->tail = NULL;
}

void xts_results_reset(void)
{
  for (int i = 0; i < nfam; i++) free_fam(&fams[i]);
  memset(fams, 0, sizeof fams);
  nfam = last = 0;
  total_bytes = finish_bytes = total_lines = dropped_lines = 0;
  evicted_others = told = 0;
}

static fam_t *find(const char *name, int add)
{
  if (last < nfam && !strcmp(fams[last].name, name)) return &fams[last];
  for (int i = 0; i < nfam; i++)
    if (!strcmp(fams[i].name, name)) { last = i; return &fams[i]; }
  if (!add || nfam >= MAXFAM) return NULL;
  fam_t *f = &fams[nfam];
  memset(f, 0, sizeof *f);
  snprintf(f->name, sizeof f->name, "%s", name);
  const xts_family_t *xf = xts_family_find(name);
  f->finish = xf && xf->finish;
  last = nfam++;
  return f;
}

static void evict_others(void)
{
  for (int i = 0; i < nfam; i++) {
    fam_t *f = &fams[i];
    if (f->finish || f->evicted) continue;
    total_bytes -= f->bytes;
    free_fam(f);
    f->bytes = 0;
    f->evicted = 1;
  }
  evicted_others = 1;
  xts_log("results store: %u MB reached: lines of families without a finish hook are no longer kept in RAM", cap >> 20);
}

static void drop(fam_t *f)
{
  dropped_lines++;
  if (f) { f->dropped++; f->evicted |= !f->finish; }
  if (f && f->finish && !told) {
    told = 1;
    xts_log("results store: %u MB of finish-hook family lines reached: further lines are dropped (family %s)", cap >> 20,
            f->name);
  }
}

void xts_results_add(const char *family, uint64_t seq, const char *line, uint32_t len)
{
  fam_t *f = find(family ? family : "", 1);
  uint32_t need = 12 + len + 1;
  if (!f || need > CHUNK - sizeof(chunk_t)) { drop(f); return; }
  if (!f->finish && evicted_others) { drop(f); return; }
  if (total_bytes + need > cap && !evicted_others) evict_others();
  if (!f->finish && evicted_others) { drop(f); return; }
  if (finish_bytes + need > cap) { drop(f); return; }
  if (!f->tail || f->tail->used + need > CHUNK - sizeof(chunk_t)) {
    chunk_t *c = malloc(CHUNK);
    if (!c) { drop(f); return; }
    c->next = NULL;
    c->used = 0;
    if (f->tail) f->tail->next = c;
    else f->head = c;
    f->tail = c;
  }
  uint8_t *p = f->tail->data + f->tail->used;
  memcpy(p, &seq, 8);
  memcpy(p + 8, &len, 4);
  memcpy(p + 12, line, len);
  p[12 + len] = 0;
  f->tail->used += need;
  f->bytes += need;
  f->lines++;
  total_bytes += need;
  if (f->finish) finish_bytes += need;
  total_lines++;
}

void xts_results_stats(uint64_t *bytes, uint64_t *lines, uint64_t *dropped, int *evicted)
{
  if (bytes) *bytes = total_bytes;
  if (lines) *lines = total_lines;
  if (dropped) *dropped = dropped_lines;
  if (evicted) *evicted = evicted_others;
}

/* cursor over one family's lines */
typedef struct cur { chunk_t *c; uint32_t off; } cur_t;
static int cur_peek(cur_t *k, uint64_t *seq, const char **line)
{
  while (k->c && k->off >= k->c->used) { k->c = k->c->next; k->off = 0; }
  if (!k->c) return 0;
  uint32_t len;
  memcpy(seq, k->c->data + k->off, 8);
  memcpy(&len, k->c->data + k->off + 8, 4);
  *line = (const char *)k->c->data + k->off + 12;
  return 1;
}
static void cur_next(cur_t *k)
{
  uint32_t len;
  memcpy(&len, k->c->data + k->off + 8, 4);
  k->off += 12 + len + 1;
}

int xts_results_foreach(const char *family, int (*cb)(const char *json_line, void *ud), void *ud)
{
  if (!xts_sink_is_open()) return -1;
  int n = 0;
  if (family) {
    fam_t *f = find(family, 0);
    if (!f) return 0;
    if (f->dropped || f->evicted)
      xts_log("results store: family %s: %llu line(s) were not kept in RAM: xts_results_foreach sees only %llu", f->name,
              (unsigned long long)f->dropped, (unsigned long long)f->lines);
    cur_t k = { f->head, 0 };
    uint64_t s;
    const char *line;
    while (cur_peek(&k, &s, &line)) {
      n++;
      if (cb(line, ud)) break;
      cur_next(&k);
    }
    return n;
  }
  /* every family, merged back into emission (seq) order */
  static cur_t ks[MAXFAM];
  int nf = nfam;
  for (int i = 0; i < nf; i++) { ks[i].c = fams[i].head; ks[i].off = 0; }
  for (;;) {
    int best = -1;
    uint64_t bs = 0, s;
    const char *bl = NULL, *line;
    for (int i = 0; i < nf; i++)
      if (cur_peek(&ks[i], &s, &line) && (best < 0 || s < bs)) { best = i; bs = s; bl = line; }
    if (best < 0) break;
    n++;
    if (cb(bl, ud)) break;
    cur_next(&ks[best]);
  }
  return n;
}
