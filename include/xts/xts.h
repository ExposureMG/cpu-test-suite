/* XTS - Xenon CPU test suite: common definitions. Include <xts/xts.h> to get every public header.
 *
 * Public headers (stable interface; Phase B code must not edit include/ or src/core):
 *   xts/isa.h      generated instruction descriptors (XI_* ids)        xts/enc.h      encoder / decoder
 *   xts/state.h    machine-state image                                 xts/exec.h     running cases on the CPU
 *   xts/kernel.h   JIT kernel builder                                  xts/model.h    reference model interface
 *   xts/test.h     test families (XTS_FAMILY)                          xts/cond.h     condition axes (XTS_AXIS), env
 *   xts/record.h   result records                                      xts/json.h     JSON text builder
 *   xts/timing.h   measurement engine                                  xts/profile.h  profiles
 *   xts/rline.h    results.jsonl line reader (finish hooks)
 *   xts/plat.h     platform layer (console / host)                     xts/kgen.h     constants (xk_li64), cond queries
 *   xts/net.h      result transport over UDP (framework)              xts/xcore.h    core roles, cross-core execution
 *   xts/cond_util.h  helpers for the condition axes (explicit include; not part of xts.h)
 * Framework-private headers (framework code only, via -Isrc): <core/core.h>, <exec/exec_priv.h>, <model/model_priv.h>,
 * <plat/plat_libxenon.h>.
 */
#ifndef XTS_XTS_H
#define XTS_XTS_H
#include <stdint.h>
#include <stddef.h>

#define XTS_VERSION "0.1.0"
#define XTS_MODEL_VERSION 1          /* bump when model semantics change (recorded in sysinfo.json) */

/* ---- deterministic random numbers (xorshift64*, splitmix64 seeding) ------------------------------------------------- */
typedef struct xts_rng { uint64_t s; } xts_rng_t;
static inline uint64_t xts_mix64(uint64_t z)
{
  z += 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
static inline void xts_rng_seed(xts_rng_t *r, uint64_t seed) { r->s = xts_mix64(seed) | 1; }
static inline uint64_t xts_rng_next(xts_rng_t *r)
{
  uint64_t x = r->s;
  x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
  r->s = x;
  return x * 0x2545F4914F6CDD1Dull;
}
static inline uint32_t xts_rng_below(xts_rng_t *r, uint32_t n) { return n ? (uint32_t)((xts_rng_next(r) >> 32) % n) : 0; }
static inline int64_t xts_rng_range(xts_rng_t *r, int64_t lo, int64_t hi)    /* inclusive */
{
  uint64_t span = (uint64_t)(hi - lo) + 1;
  return span ? lo + (int64_t)(xts_rng_next(r) % span) : (int64_t)xts_rng_next(r);
}
/* FNV-1a over a string, folded into h (for deterministic seeds from names) */
static inline uint64_t xts_hash_str(uint64_t h, const char *s)
{
  h ^= 0xCBF29CE484222325ull;
  while (s && *s) { h ^= (uint8_t)*s++; h *= 0x100000001B3ull; }
  return xts_mix64(h);
}
static inline uint64_t xts_hash_u64(uint64_t h, uint64_t v) { return xts_mix64(h ^ xts_mix64(v)); }

/* ---- logging: run.log (one LOG message per line) + screen status -------------------------------------------------------- */
void xts_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void xts_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* screen status line only */

/* glob match with * and ? (used for profile family selection) */
int xts_glob(const char *pattern, const char *s);

#include <xts/isa.h>
#include <xts/enc.h>
#include <xts/state.h>
#include <xts/json.h>
#include <xts/record.h>
#include <xts/cond.h>
#include <xts/test.h>
#include <xts/rline.h>
#include <xts/exec.h>
#include <xts/kernel.h>
#include <xts/kgen.h>
#include <xts/model.h>
#include <xts/timing.h>
#include <xts/profile.h>
#include <xts/plat.h>
#endif
