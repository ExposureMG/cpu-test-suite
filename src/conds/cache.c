/* cache - cache state of the data a family is about to touch: default (as it falls), cold (dcbf), l1 (resident in L1D),
 * l2 (in L2, not in L1D), mem (flushed, L2 holding other data). The axis sets xts_env.cache_state (= the value index);
 * families call xts_cond_cache_prepare() (cond_util.h) on their buffer before the timed or checked run. The helpers live
 * here: the eviction buffer is private (never the big buffer the families use). */
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include "cond_util.h"

static const char *const cache_values[] = { "default", "cold", "l1", "l2", "mem", NULL };

const char *xts_cond_cache_name(int state)
{
  return state >= 0 && state <= XTS_CACHE_MEM ? cache_values[state] : "?";
}

/* eviction buffer: 4 x L1D covers every set of a 4-way PLRU L1D with 16 lines per set; 2 x L2 for the memory state */
#define EV_L1_BYTES (4u * XTS_L1D_BYTES)
#define EV_L2_BYTES (2u * XTS_L2_BYTES)
static void *ev_raw;
static uint8_t *ev_buf;
static uint32_t ev_bytes;

int xts_cond_evict_init(int want_l2)
{
  uint32_t need = want_l2 ? EV_L2_BYTES : EV_L1_BYTES;
  if (xts_plat_is_host()) return 0;
  if (ev_buf && ev_bytes >= need) return 0;
  void *raw = malloc((size_t)need + 256);
  if (!raw) return -1;
  uint8_t *b = (uint8_t *)(((uintptr_t)raw + 127) & ~(uintptr_t)127);
  if ((uintptr_t)b + need > xts_plat_heap_limit()) { free(raw); return -1; }   /* never grow past the heap limit (stacks above) */
  for (uint32_t i = 0; i < need; i += 128) b[i] = (uint8_t)(i >> 7);            /* fault pages in; contents irrelevant */
  free(ev_raw);
  ev_raw = raw;
  ev_buf = b;
  ev_bytes = need;
  return 0;
}

int xts_cond_evict_l1(void)
{
  if (xts_plat_is_host()) return 0;
  if (xts_cond_evict_init(0) < 0) return -1;
  xts_touch_range(ev_buf, EV_L1_BYTES);
  return 0;
}

int xts_cond_evict_l2(void)
{
  if (xts_plat_is_host()) return 0;
  if (xts_cond_evict_init(1) < 0) return -1;
  xts_touch_range(ev_buf, EV_L2_BYTES);
  return 0;
}

int xts_cond_cache_set(int state, void *addr, uint32_t len)
{
  switch (state) {
  case XTS_CACHE_COLD:
    xts_dcbf_range(addr, len);
    return 0;
  case XTS_CACHE_L1:
    xts_touch_range(addr, len);
    return 0;
  case XTS_CACHE_L2:
    xts_touch_range(addr, len);
    return xts_cond_evict_l1();
  case XTS_CACHE_MEM:
    xts_dcbf_range(addr, len);
    return xts_cond_evict_l2();
  default:
    return 0;
  }
}

static int cache_apply(int v)
{
  if (v == XTS_CACHE_L2 && xts_cond_evict_init(0) < 0) return -1;
  if (v == XTS_CACHE_MEM && xts_cond_evict_init(1) < 0) return -1;
  xts_env.cache_state = v;
  return 0;
}

static void cache_revert(int v)
{
  (void)v;
  xts_env.cache_state = 0;
}

/* data caches matter for anything that loads or stores; not for the harness self-test or the calibration */
static int cache_applies(const char *fam, const char *kind)
{
  (void)fam;
  return kind && (!strcmp(kind, "timing") || !strcmp(kind, "sched") || !strcmp(kind, "mem") || !strcmp(kind, "smt"));
}

XTS_AXIS(cache, .values = cache_values, .apply = cache_apply, .revert = cache_revert, .applies_to = cache_applies, .order = 20,
         .desc = "cache state of the data under test: default, cold (dcbf), l1, l2 (evicted from L1D only), mem (dcbf + L2 sweep)");
