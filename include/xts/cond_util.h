/* xts/cond_util.h - helpers for families that honour the condition axes in src/conds (implemented in src/conds/cache.c and
 * inline here). From a family:   #include <xts/cond_util.h>      (not included by <xts/xts.h>)
 * Whether an axis was applied to the current block: xts_cond_applied(ctx, "axis") (xts/kgen.h).
 *
 * cache      (xts_env.cache_state)  xts_cond_cache_prepare(buf, len) right before the timed/checked run puts the buffer in
 *                                   the requested state; xts_cond_cache_set(state, ...) forces a state; evict helpers.
 * data_align (xts_env.data_kind/offset)  xts_cond_data_offset(width): byte offset to add to a 128-byte aligned base so the
 *                                   access is aligned / misaligned / line-crossing / page-crossing for that access width.
 * unroll, chains (xts_env.unroll/chains) xts_cond_unroll(&spec) overrides the depth list of a timing spec,
 *                                   xts_cond_chains(dflt) returns the chain count to use.
 * thread_prio                       xts_cond_set_thread_prio(p): execute `or p,p,p` now (the kernel prologue is
 *                                   xk_prio_prologue); the axis revert restores medium.
 * code_align is applied entirely by xts_exec_kernel (kernel starts at kbuf + code_offset; kbuf is 128-byte aligned, the
 * nops in front never execute). Note xk_align()/spec.align are relative to the kernel start, so the absolute offset of an
 * aligned loop top is code_offset + that alignment.
 */
#ifndef XTS_COND_UTIL_H
#define XTS_COND_UTIL_H
#include <stdint.h>
#include <xts/xts.h>

/* ---- cache axis ---------------------------------------------------------------------------------------------------- */
enum { XTS_CACHE_DEFAULT = 0, XTS_CACHE_COLD = 1, XTS_CACHE_L1 = 2, XTS_CACHE_L2 = 3, XTS_CACHE_MEM = 4 };
#define XTS_L1D_BYTES (32u * 1024u)      /* 32 KB, 4-way, 128-byte lines (128 sets, set stride 8 KB) */
#define XTS_L2_BYTES  (1024u * 1024u)    /* 1 MB, 8-way, shared by the three cores */

/* Name of a cache_state ("default" "cold" "l1" "l2" "mem"). */
const char *xts_cond_cache_name(int state);
/* Make sure the private eviction buffer (4 x L1; 2 x L2 if `want_l2`) exists. 0 ok, <0 not possible (allocation, or it
 * would grow into the crash page). Host: always 0, nothing is allocated. Called by the cache axis apply for l2/mem. */
int xts_cond_evict_init(int want_l2);
/* Evict everything of L1D / (L1D and L2) by loading an eviction buffer of our own (never overlaps the big buffer). The
 * evicted data is clean; the sweep leaves other lines in L2. Host: no-op. Return 0, <0 if the buffer is unavailable. */
int xts_cond_evict_l1(void);
int xts_cond_evict_l2(void);
/* Put [addr, addr+len) in `state`:
 *   cold : dcbf (not in L1/L2, memory is up to date)
 *   l1   : loaded line by line (so in L1 and L2)           -- best effort when len > L1D
 *   l2   : loaded, then L1D evicted by a sweep (stays in L2)  -- len should be well below 1 MB
 *   mem  : dcbf, then a 2 MB sweep so L2 holds unrelated lines and the target's translation/TLB entries are displaced
 *   default (0): untouched.
 * Returns 0 or <0 (eviction buffer unavailable). The scratch image of xts_exec_t cannot be prepared this way (the engine
 * copies it right before the run); use xts_big() buffers or kernel-owned data. */
int xts_cond_cache_set(int state, void *addr, uint32_t len);
/* Same with xts_env.cache_state. */
static inline int xts_cond_cache_prepare(void *addr, uint32_t len) { return xts_cond_cache_set(xts_env.cache_state, addr, len); }

/* ---- data_align axis ----------------------------------------------------------------------------------------------- */
/* xts_env.data_kind: 0 aligned, 1 misaligned (data_offset 1 for mis1, 4 for mis4), 2 line crossing (data_offset 124), 3 4 KB
 * crossing (data_offset 4092), 4 64 KB page crossing (data_offset 65532; both are right for an 8-byte access, use xts_cond_data_offset(width) for others). Page
 * crossing needs a buffer of at least 4 KB + the access, i.e. xts_big(), not the 512-byte scratch. */
/* Byte offset to add to a base that is 128-byte (page) aligned so an access of `width` bytes (1..16) at base+offset has the
 * requested property: aligned 0; mis1 1; mis4 4; line_cross 128 - width/2 (the access straddles the 128-byte line end;
 * width 1 cannot straddle: 0); page_cross 4096 - width/2. Pass the access width; width 0 -> generic xts_env.data_offset. */
static inline uint32_t xts_cond_data_offset(unsigned width)
{
  if (width == 0) return xts_env.data_offset;
  switch (xts_env.data_kind) {
  case 1: return xts_env.data_offset ? xts_env.data_offset : 1;
  case 2: return width > 1 ? 128u - width / 2 : 0;
  case 3: return width > 1 ? 4096u - width / 2 : 0;
  case 4: return width > 1 ? 65536u - width / 2 : 0;
  default: return 0;
  }
}
/* 1 if the offset puts the access across a 128-byte line / a 4 KB page boundary (base assumed page aligned). */
static inline int xts_cond_crosses(uint32_t off, unsigned width, unsigned boundary) { return (off % boundary) + width > boundary; }

/* ---- unroll / chains axes (timing) ---------------------------------------------------------------------------------- */
/* unroll selector (xts_env.unroll): 0 family default, 1 short {2,3,4,5,6,8}, 2 long {16,24,32,48,64,96},
 * 3 pow2 {1,2,4,8,16,32}, 4 odd {3,5,7,9,11,13}. */
#define XTS_UNROLL_SETS 4
static inline int xts_cond_unroll(xts_tspec_t *s)
{
  static const int set[XTS_UNROLL_SETS][6] = { { 2, 3, 4, 5, 6, 8 }, { 16, 24, 32, 48, 64, 96 }, { 1, 2, 4, 8, 16, 32 }, { 3, 5, 7, 9, 11, 13 } };
  int u = xts_env.unroll;
  if (u < 1 || u > XTS_UNROLL_SETS) return 0;
  for (int i = 0; i < 6; i++) s->depths[i] = set[u - 1][i];
  s->ndepths = 6;
  return 1;
}
/* chain count for independent-stream bodies: xts_env.chains if the chains axis set one, else `dflt`. */
static inline int xts_cond_chains(int dflt) { return xts_env.chains > 0 ? xts_env.chains : dflt; }

/* ---- thread_prio axis ----------------------------------------------------------------------------------------------- */
/* 1 low (or 1,1,1), 2 medium (or 2,2,2), 3 high (or 3,3,3); other values ignored. No-op on the host. */
static inline void xts_cond_set_thread_prio(int p)
{
#if defined(__powerpc__) || defined(__PPC__)
  if (p == 1) __asm__ volatile("or 1,1,1" ::: "memory");
  else if (p == 2) __asm__ volatile("or 2,2,2" ::: "memory");
  else if (p == 3) __asm__ volatile("or 3,3,3" ::: "memory");
#else
  (void)p;
#endif
}
#endif
