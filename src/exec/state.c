/* State image helpers: canary baseline, comparison. */
#include <stdio.h>
#include <string.h>
#include <xts/state.h>

void xts_state_baseline(xts_state_t *s)
{
  memset(s, 0, sizeof *s);
  for (int n = 0; n < 32; n++) {
    s->gpr[n] = xts_baseline_gpr(n);
    s->fpr[n] = 0x7FF8DEAD00000000ull | (uint64_t)n;
  }
  for (int n = 0; n < 128; n++)
    s->vr[n] = xts_vr4(0xA5A50000u | n, 0x5A5A0000u | n, 0xC3C30000u | n, 0x3C3C0000u | n);
  s->mode = XTS_MODE_SF;
}

void xts_scratch_baseline(uint8_t *mem, int n)
{
  for (int i = 0; i < n; i++) mem[i] = (uint8_t)(i * 13 + 7);
}

static void add(xts_state_diff_t *d, int *n, int max, const char *what, uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1)
{
  if (d && *n < max) {
    snprintf(d[*n].what, sizeof d[*n].what, "%s", what);
    d[*n].a[0] = a0; d[*n].a[1] = a1;
    d[*n].b[0] = b0; d[*n].b[1] = b1;
  }
  (*n)++;
}

static uint64_t hi64(const xts_vr_t *v) { return ((uint64_t)v->w[0] << 32) | v->w[1]; }
static uint64_t lo64(const xts_vr_t *v) { return ((uint64_t)v->w[2] << 32) | v->w[3]; }

int xts_state_compare(const xts_state_t *a, const xts_state_t *b, xts_state_diff_t *d, int max)
{
  int n = 0;
  char w[12];
  for (int r = 0; r < 32; r++) {
    if (XTS_RESERVED_GPRS & (1u << r)) continue;
    if (a->gpr[r] != b->gpr[r]) { snprintf(w, sizeof w, "gpr%d", r); add(d, &n, max, w, a->gpr[r], 0, b->gpr[r], 0); }
  }
  for (int r = 0; r < 32; r++)
    if (a->fpr[r] != b->fpr[r]) { snprintf(w, sizeof w, "fpr%d", r); add(d, &n, max, w, a->fpr[r], 0, b->fpr[r], 0); }
  for (int r = 0; r < 128; r++)
    if (memcmp(&a->vr[r], &b->vr[r], sizeof a->vr[r])) {
      snprintf(w, sizeof w, "vr%d", r);
      add(d, &n, max, w, hi64(&a->vr[r]), lo64(&a->vr[r]), hi64(&b->vr[r]), lo64(&b->vr[r]));
    }
  if ((uint32_t)a->cr != (uint32_t)b->cr) add(d, &n, max, "cr", (uint32_t)a->cr, 0, (uint32_t)b->cr, 0);
  if (a->xer != b->xer) add(d, &n, max, "xer", a->xer, 0, b->xer, 0);
  if (a->lr != b->lr) add(d, &n, max, "lr", a->lr, 0, b->lr, 0);
  if (a->ctr != b->ctr) add(d, &n, max, "ctr", a->ctr, 0, b->ctr, 0);
  if ((uint32_t)a->fpscr != (uint32_t)b->fpscr) add(d, &n, max, "fpscr", (uint32_t)a->fpscr, 0, (uint32_t)b->fpscr, 0);
  if (a->vscr.w[3] != b->vscr.w[3]) add(d, &n, max, "vscr", a->vscr.w[3], 0, b->vscr.w[3], 0);
  if (memcmp(a->mark, b->mark, sizeof a->mark))
    add(d, &n, max, "mark", ((uint64_t)a->mark[0] << 32) | a->mark[1], 0, ((uint64_t)b->mark[0] << 32) | b->mark[1], 0);
  return n;
}

int xts_mem_compare(const uint8_t *a, const uint8_t *b, int len, xts_state_diff_t *d, int max)
{
  int n = 0;
  char w[16];
  for (int o = 0; o < len; o += 16) {
    int l = len - o < 16 ? len - o : 16;
    if (memcmp(a + o, b + o, l)) {
      snprintf(w, sizeof w, "mem%d", o & 0xFFFF);
      uint64_t a0 = xts_be_load(a + o, l < 8 ? l : 8), b0 = xts_be_load(b + o, l < 8 ? l : 8);
      uint64_t a1 = l > 8 ? xts_be_load(a + o + 8, l - 8) : 0, b1 = l > 8 ? xts_be_load(b + o + 8, l - 8) : 0;
      add(d, &n, max, w, a0, a1, b0, b1);
    }
  }
  return n;
}
