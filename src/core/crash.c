/* Crash page: a reserved RAM page holding a breadcrumb (which case is running) and, after an unhandled exception, the
 * crash state. Written only with plain stores + cache flush (safe from exception context); the next boot validates it
 * (magic + checksum) and turns it into a hang/crash record. If the page does not survive the reboot, the journal's
 * BEGIN-without-END still marks the block. */
#include <string.h>
#include "core.h"

#define CRASH_VERSION 2
_Static_assert(sizeof(xts_crashpage_t) <= 4096, "crash page record must fit the smallest platform page (host: 4 KB)");

static xts_crashpage_t *page(void)
{
  size_t sz = 0;
  void *p = xts_plat_crash_page(&sz);
  return (p && sz >= sizeof(xts_crashpage_t)) ? (xts_crashpage_t *)p : NULL;
}

static uint32_t fnv_skip(const uint8_t *b, size_t n, size_t skip_at)
{
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    if (i >= skip_at && i < skip_at + 4) continue;
    h ^= b[i];
    h *= 16777619u;
  }
  return h;
}

/* main record only (the thread slots have their own checksums) */
uint32_t xts_crash_checksum(const xts_crashpage_t *p)
{
  return fnv_skip((const uint8_t *)p, XTS_CRASH_MAIN_BYTES, offsetof(xts_crashpage_t, checksum));
}

uint32_t xts_crash_thr_checksum(const xts_crashthr_t *t)
{
  return fnv_skip((const uint8_t *)t, sizeof *t, offsetof(xts_crashthr_t, checksum));
}

static int thr_valid(const xts_crashthr_t *t)
{
  return t->magic == XTS_CRASH_THR_MAGIC && t->checksum == xts_crash_thr_checksum(t);
}

static void seal(xts_crashpage_t *p)
{
  p->magic = XTS_CRASH_MAGIC;
  p->version = CRASH_VERSION;
  p->checksum = xts_crash_checksum(p);
  xts_plat_crash_page_sync();
}

void xts_crash_breadcrumb(const char *run_id, const char *block_key, int case_index, int repeat, const char *case_id,
                          uint64_t seq)
{
  xts_crashpage_t *p = page();
  if (!p) return;
  if (p->magic != XTS_CRASH_MAGIC || strncmp(p->run_id, run_id, sizeof p->run_id) || strncmp(p->block_key, block_key, sizeof p->block_key)) {
    uint32_t boot = p->magic == XTS_CRASH_MAGIC ? p->boot : 0;
    memset(p, 0, XTS_CRASH_MAIN_BYTES);           /* the thread slots are not the breadcrumb's */
    p->boot = boot;
    strncpy(p->run_id, run_id, sizeof p->run_id - 1);
    strncpy(p->block_key, block_key, sizeof p->block_key - 1);
  }
  p->case_index = case_index;
  p->repeat = repeat;
  memset(p->case_id, 0, sizeof p->case_id);
  if (case_id) strncpy(p->case_id, case_id, sizeof p->case_id - 1);
  p->seq = seq;
  p->crashed = 0;
  seal(p);
}

void xts_crash_clear(void)
{
  xts_crashpage_t *p = page();
  if (!p) return;
  memset(p, 0, XTS_CRASH_MAIN_BYTES);
  xts_plat_crash_page_sync();
}

int xts_crash_collect(xts_crashpage_t *out)
{
  xts_crashpage_t *p = page();
  memset(out, 0, sizeof *out);
  if (!p) return 0;
  int main_ok = p->magic == XTS_CRASH_MAGIC && p->version == CRASH_VERSION && p->checksum == xts_crash_checksum(p);
  int any = main_ok;
  *out = *p;
  if (!main_ok) memset(out, 0, XTS_CRASH_MAIN_BYTES);
  for (int i = 0; i < XTS_CRASH_THREADS; i++) {
    if (thr_valid(&p->thr[i])) {
      any = 1;
      out->thr[i].block_key[sizeof out->thr[i].block_key - 1] = 0;
    } else {
      memset(&out->thr[i], 0, sizeof out->thr[i]);
    }
  }
  out->block_key[sizeof out->block_key - 1] = 0;
  out->run_id[sizeof out->run_id - 1] = 0;
  out->case_id[sizeof out->case_id - 1] = 0;
  memset(p, 0, sizeof *p);          /* collected once (main record and thread slots) */
  xts_plat_crash_page_sync();
  return any;
}

/* ctx40: libxenon's context block (r0-r31, LR, CTR, CR, XER, SRR0, SRR1, DAR, PIR) or NULL */
void xts_crash_record(uint32_t vector, const uint64_t *ctx40)
{
  xts_crashpage_t *p = page();
  if (!p) return;
  uint64_t pir = ctx40 ? ctx40[39] : 0;
  if (pir >= 1 && pir <= XTS_CRASH_THREADS) {   /* another hardware thread: its own slot, the main record is untouched */
    xts_crashthr_t *t = &p->thr[pir - 1];
    if (thr_valid(t)) {                         /* keep the first crash, count the rest */
      t->count++;
    } else {
      memset(t, 0, sizeof *t);
      t->count = 1;
      t->vector = vector;
      t->pir = (uint32_t)pir;
      t->srr0 = ctx40[36];
      t->srr1 = ctx40[37];
      t->dar = ctx40[38];
      t->lr = ctx40[32];
      t->r1 = ctx40[1];
      t->r3 = ctx40[3];
      t->case_index = -1;
      if (p->magic == XTS_CRASH_MAGIC) {
        t->case_index = p->case_index;
        t->seq = p->seq;
        memcpy(t->block_key, p->block_key, sizeof t->block_key - 1);
      }
    }
    t->magic = XTS_CRASH_THR_MAGIC;
    t->checksum = xts_crash_thr_checksum(t);
    xts_plat_crash_page_sync();
    return;
  }
  if (p->magic != XTS_CRASH_MAGIC) {      /* crash outside any case: still record it */
    memset(p, 0, XTS_CRASH_MAIN_BYTES);
    p->case_index = -1;
  }
  p->crashed = 1;
  p->vector = vector;
  if (ctx40) {
    for (int i = 0; i < 32; i++) p->gpr[i] = ctx40[i];
    p->lr = ctx40[32];
    p->srr0 = ctx40[36];
    p->srr1 = ctx40[37];
    p->dar = ctx40[38];
    p->pir = ctx40[39];
  }
  seal(p);
}

/* The core part of the crash path (console: vec.S crashdump -> emergency stack -> xts_crash_entry -> here; host tests:
 * xts_plat_self_crash). No malloc, no printf, small stack. The crash page first (RAM: survives only if the reboot keeps
 * it), then on PIR 0 the journal CRASH line (USB: survives the reboot, written only while a case runs, when the core is
 * not inside libfat). Other threads only fill their crash page slot: the main thread owns the files. */
void xts_crash_handle(uint32_t vector, const uint64_t *ctx40)
{
  xts_crash_record(vector, ctx40);
  uint64_t pir = ctx40 ? ctx40[39] : 0;
  if (pir == 0) xts_core_crash_journal(vector, ctx40 ? ctx40[36] : 0, ctx40 ? ctx40[38] : 0);
}
