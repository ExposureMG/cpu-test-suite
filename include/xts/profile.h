/* XTS - profiles. One ELF per profile: `make PROFILE=<name>` links profiles/<name>.c, which defines `xts_profile`.
 *
 *   #include <xts/profile.h>
 *   XTS_PROFILE(smoke, "identity + self-test + exec.ppc subset",
 *     { .families = "self.*" },
 *     { .families = "exec.ppc", .axes = "sf=*" },
 *   );
 *
 * Row fields:
 *   families  glob over family names ("exec.*", "lat.chain", "*"); a row that matches nothing is allowed
 *             (families written later join automatically)
 *   kind      optional: only families of this kind ("timing")
 *   axes      "axis=v1,v2;axis2=*" - value lists per axis; "*" = every value. Axes the family does not list in its .axes
 *             are ignored for that family; axes not registered (yet) are ignored with a log line. Axes the family lists
 *             but the row does not mention stay at their default.
 *   repeats   run every case this many times (default 1); the repeat index is part of the seed
 *   seed      base seed (default 1)
 *   sweep     1 = skip a matching family unless at least one of the row's axes applies to it. Use it for rows that
 *             sweep one axis ("fpscr_rn=*") so families insensitive to that axis are not re-run at their defaults.
 *   max_cases cap on the cases each matching family runs in this row (0 = all). Deterministic, evenly strided over the
 *             family's enumeration order: stride = ceil(n / max_cases), cases 0, stride, 2*stride, ... (at most
 *             max_cases of them), so a cap samples the whole list (every instruction of an instruction-ordered family
 *             whose per-instruction group is at least `stride` long) instead of its first N. Blocks are cut from the
 *             sampled list (block_cases per block); case ids and seeds are unchanged.
 * The scheduler walks rows in order, each matching family in registry (name) order, the cartesian product of the
 * row's axis values in lexicographic order, then the family's cases in blocks of block_cases.
 */
#ifndef XTS_PROFILE_H
#define XTS_PROFILE_H
#include <stdint.h>

typedef struct xts_profile_row {
  const char *families;
  const char *kind;
  const char *axes;
  int repeats;
  uint64_t seed;
  int sweep;
  int max_cases;
} xts_profile_row_t;

typedef struct xts_profile {
  const char *name;
  const char *desc;
  const xts_profile_row_t *rows;
  int nrows;
} xts_profile_t;

#define XTS_PROFILE(id, description, ...)                                                    \
  static const xts_profile_row_t xts_profile_rows_[] = { __VA_ARGS__ };                       \
  const xts_profile_t xts_profile = { #id, description, xts_profile_rows_,                   \
                                      (int)(sizeof xts_profile_rows_ / sizeof xts_profile_rows_[0]) }

/* the linked profile */
extern const xts_profile_t xts_profile;
#endif
