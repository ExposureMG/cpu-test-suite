/* memory guard: loads and stores are range-checked too, whatever the model says. A wild load can hit MMIO in libxenon's map
 * (0xC flash, 0xD/0xE PCI registers incl. the USB controller), so a load the model simulates is not executed either when its
 * EA window leaves scratch / the big buffer. Windows: the access size at the EA (1/2/4/8 bytes, scalar and FP), the aligned 16 bytes (AltiVec),
 * 128 bytes for lmw/stmw/lswi/lswx. Scratch and big-buffer accesses stay allowed, to the last byte. */
#include <string.h>
#include <xts/xts.h>
#include <exec/exec_priv.h>
#include <core/core.h>
#include "htest.h"

static xts_exec_t x;
static char cap_line[8192];
static int cap_n;
static void tap(const char *family, const char *case_id, const xts_record_t *r, const char *json)
{
  (void)family; (void)case_id; (void)r;
  cap_n++;
  snprintf(cap_line, sizeof cap_line, "%s", json);
}
static const xts_family_t gfam = { .name = "guard_load", .kind = "semantic" };
static xts_case_t gcase = { .id = "g#0", .insn = -1 };
static xts_ctx_t gctx = { .fam = &gfam, .c = &gcase, .block_key = "r0:guard.load:-:c0" };

typedef struct { const char *name; uint32_t w; int window; int align; } lw_t;   /* base register is r3, index r4 = 0 */

static const lw_t words[] = {
  { "lbz", 0, 1, 1 }, { "lwz", 0, 4, 4 }, { "ld", 0, 8, 8 }, { "lwa", 0, 4, 4 }, { "lfd", 0, 8, 8 }, { "lfs", 0, 4, 4 },
  { "lwzu", 0, 4, 4 }, { "stw", 0, 4, 4 }, { "std", 0, 8, 8 }, { "stfd", 0, 8, 8 }, { "lwzx", 0, 4, 4 }, { "ldx", 0, 8, 8 },
  { "lwarx", 0, 4, 4 }, { "ldarx", 0, 8, 8 }, { "stwcx", 0, 4, 4 }, { "lvx", 0, 16, 16 }, { "lvx128", 0, 16, 16 },
  { "stvx", 0, 16, 16 }, { "stvewx", 0, 16, 4 }, { "lvlx", 0, 16, 1 }, { "lwbrx", 0, 4, 4 }, { "eciwx", 0, 4, 4 },
  { "lmw", 0, 128, 4 }, { "stmw", 0, 128, 4 }, { "lswi", 0, 128, 1 }, { "lswx", 0, 128, 1 },
};
#define NW ((int)(sizeof words / sizeof words[0]))

/* build each word with RA = r3 (displacement 0 / RB = r4) */
static uint32_t build(int i)
{
  const char *n = words[i].name;
  if (!strcmp(n, "lbz")) return xts_enc(XI_lbz, 0, 5, 0, 3);
  if (!strcmp(n, "lwz")) return xts_enc(XI_lwz, 0, 5, 0, 3);
  if (!strcmp(n, "ld")) return xts_enc(XI_ld, 0, 5, 0, 3);
  if (!strcmp(n, "lwa")) return xts_enc(XI_lwa, 0, 5, 0, 3);
  if (!strcmp(n, "lfd")) return xts_enc(XI_lfd, 0, 5, 0, 3);
  if (!strcmp(n, "lfs")) return xts_enc(XI_lfs, 0, 5, 0, 3);
  if (!strcmp(n, "lwzu")) return xts_enc(XI_lwzu, 0, 5, 0, 3);
  if (!strcmp(n, "stw")) return xts_enc(XI_stw, 0, 5, 0, 3);
  if (!strcmp(n, "std")) return xts_enc(XI_std, 0, 5, 0, 3);
  if (!strcmp(n, "stfd")) return xts_enc(XI_stfd, 0, 5, 0, 3);
  if (!strcmp(n, "lwzx")) return xts_enc(XI_lwzx, 0, 5, 3, 4);
  if (!strcmp(n, "ldx")) return xts_enc(XI_ldx, 0, 5, 3, 4);
  if (!strcmp(n, "lwarx")) return xts_enc(XI_lwarx, 0, 5, 3, 4);
  if (!strcmp(n, "ldarx")) return xts_enc(XI_ldarx, 0, 5, 3, 4);
  if (!strcmp(n, "stwcx")) return xts_enc(XI_stwcx, 0, 5, 3, 4);
  if (!strcmp(n, "lvx")) return xts_enc(XI_lvx, 0, 5, 3, 4);
  if (!strcmp(n, "lvx128")) return xts_enc(XI_lvx128, 0, 5, 3, 4);
  if (!strcmp(n, "stvx")) return xts_enc(XI_stvx, 0, 5, 3, 4);
  if (!strcmp(n, "stvewx")) return xts_enc(XI_stvewx, 0, 5, 3, 4);
  if (!strcmp(n, "lvlx")) return xts_enc(XI_lvlx, 0, 5, 3, 4);
  if (!strcmp(n, "lwbrx")) return xts_enc(XI_lwbrx, 0, 5, 3, 4);
  if (!strcmp(n, "eciwx")) return xts_enc(XI_eciwx, 0, 5, 3, 4);
  if (!strcmp(n, "lmw")) return xts_enc(XI_lmw, 0, 5, 0, 3);
  if (!strcmp(n, "stmw")) return xts_enc(XI_stmw, 0, 5, 0, 3);
  if (!strcmp(n, "lswi")) return xts_enc(XI_lswi, 0, 5, 3, 8);
  if (!strcmp(n, "lswx")) return xts_enc(XI_lswx, 0, 5, 3, 4);
  return 0;
}

static int guard(uint32_t w, int mres, char *why) { return xts_exec_mem_guard(xts_decode1(w), w, &x.in, mres, 0, why, 256); }

static const int mres_list[] = { XTS_M_OK, XTS_M_UNIMPLEMENTED, XTS_M_ILLEGAL, XTS_M_ALIGNMENT };

HTEST(guard_load_wild_ea_blocked)
{
  const uint64_t sc = xts_scratch_addr();
  /* MMIO of libxenon's map, the flash, the PCI / USB register windows, low memory, the r0 canary, wrapped values */
  const uint64_t wild[] = { 0xC0000000ull, 0xC8000000ull, 0xD0000000ull, 0xD0010000ull, 0xEA000000ull, 0xE0000000ull, 0, 8,
                            0x1000, 0x80000000ull, 0xC0DEC0DEC0DEC0DEull, ~0ull, ~7ull, sc - 16, sc - 1, sc + 512 };
  char why[256];
  for (int i = 0; i < NW; i++) {
    uint32_t w = build(i);
    HT_CHECK(w != 0);
    if (!w) { printf("      cannot build %s\n", words[i].name); continue; }
    for (unsigned k = 0; k < sizeof wild / sizeof *wild; k++)
      for (unsigned m = 0; m < sizeof mres_list / sizeof *mres_list; m++) {
        xts_case_init(&x);
        xts_set_sf(&x, 1);
        xts_set_gpr(&x, 3, wild[k]);
        xts_set_gpr(&x, 4, 0);
        why[0] = 0;
        int b = guard(w, mres_list[m], why);
        if (!b) printf("      %s base %llX model %d not blocked\n", words[i].name, (unsigned long long)wild[k], mres_list[m]);
        HT_CHECK(b && strstr(why, "EA outside test memory"));
      }
    /* the offset can be the wild part: scratch base plus a huge index register */
    if (strchr("x", words[i].name[strlen(words[i].name) - 1]) || !strcmp(words[i].name, "lswi")) {
      xts_case_init(&x);
      xts_set_sf(&x, 1);
      xts_set_ptr(&x, 3, 128);
      xts_set_gpr(&x, 4, 0x7FFFFFFFFFFFF000ull);
      if (build(i) && strcmp(words[i].name, "lswi")) HT_CHECK(guard(build(i), XTS_M_OK, why));
    }
  }
}

HTEST(guard_load_window_edges)
{
  const uint64_t sc = xts_scratch_addr();
  char why[256];
  for (int i = 0; i < NW; i++) {
    uint32_t w = build(i);
    if (!w) continue;
    int win = words[i].window;
    /* last byte offsets that still fit: scalar [ea, ea+size) <= 512; AltiVec: any byte of the aligned block; multiples and
     * strings: window 128 */
    int vec = win == 16;
    int last_ok = vec ? 511 : XTS_SCRATCH_BYTES - win;
    xts_case_init(&x);
    xts_set_sf(&x, 1);
    xts_set_gpr(&x, 4, 0);
    xts_set_gpr(&x, 3, sc + (uint64_t)last_ok);
    if (!guard(w, XTS_M_OK, why)) HT_CHECK(1); else { printf("      %s at +%d blocked\n", words[i].name, last_ok); HT_CHECK(0); }
    if (!vec) {
      xts_set_gpr(&x, 3, sc + (uint64_t)last_ok + 1);
      if (!guard(w, XTS_M_OK, why)) { printf("      %s at +%d not blocked\n", words[i].name, last_ok + 1); HT_CHECK(0); }
      else HT_CHECK(1);
    } else {
      xts_set_gpr(&x, 3, sc + 512);
      HT_CHECK(guard(w, XTS_M_OK, why));
    }
    xts_set_gpr(&x, 3, sc);
    HT_CHECK(!guard(w, XTS_M_OK, why));
    xts_set_gpr(&x, 3, sc - 1);
    HT_CHECK(guard(w, XTS_M_OK, why));
  }
  /* the big buffer */
  uint64_t ba = 0;
  void *big = xts_big(1u << 20, &ba);
  HT_CHECK(big != NULL);
  if (big) {
    uint64_t end = ba + xts_big_size();
    uint32_t lwz = xts_enc(XI_lwz, 0, 5, 0, 3);
    xts_case_init(&x);
    xts_set_sf(&x, 1);
    xts_set_gpr(&x, 3, ba);
    HT_CHECK(!guard(lwz, XTS_M_OK, why));
    xts_set_gpr(&x, 3, end - 4);
    HT_CHECK(!guard(lwz, XTS_M_OK, why));
    xts_set_gpr(&x, 3, end - 3);
    HT_CHECK(guard(lwz, XTS_M_OK, why));
    xts_set_gpr(&x, 3, ba - 4);
    HT_CHECK(guard(lwz, XTS_M_OK, why));
    xts_set_gpr(&x, 3, 0xC0000000ull);                 /* MMIO is blocked while a big buffer exists */
    HT_CHECK(guard(lwz, XTS_M_OK, why));
  }
}

HTEST(guard_load_displacement_and_sf)
{
  const uint64_t sc = xts_scratch_addr();
  char why[256];
  xts_case_init(&x);
  xts_set_sf(&x, 1);
  xts_set_ptr(&x, 3, 128);
  /* D-form / DS-form displacements are part of the EA, signed */
  HT_CHECK(!guard(xts_enc(XI_lwz, 0, 5, 64, 3), XTS_M_OK, why));
  HT_CHECK(!guard(xts_enc(XI_lwz, 0, 5, -128, 3), XTS_M_OK, why));
  HT_CHECK(guard(xts_enc(XI_lwz, 0, 5, -132, 3), XTS_M_OK, why));
  HT_CHECK(!guard(xts_enc(XI_ld, 0, 5, 376, 3), XTS_M_OK, why));          /* 128 + 376 = 504: the last doubleword */
  HT_CHECK(guard(xts_enc(XI_ld, 0, 5, 380, 3), XTS_M_OK, why));           /* 508 + 8 > 512 */
  HT_CHECK(guard(xts_enc(XI_ld, 0, 5, 384, 3), XTS_M_OK, why));
  HT_CHECK(guard(xts_enc(XI_ld, 0, 5, -132, 3), XTS_M_OK, why));
  /* 32-bit mode: the EA wraps at 2^32, so a pointer with junk in the upper half is still scratch; 64-bit mode sees the junk */
  xts_set_gpr(&x, 3, 0xFFFFFFFF00000000ull | (sc + 128));
  xts_set_sf(&x, 0);
  HT_CHECK(!guard(xts_enc(XI_lwz, 0, 5, 0, 3), XTS_M_OK, why));
  xts_set_sf(&x, 1);
  HT_CHECK(guard(xts_enc(XI_lwz, 0, 5, 0, 3), XTS_M_OK, why));
  /* 32-bit wrap of the sum: 0xFFFFFFF0 + 0x20 wraps to 0x10, which is low memory */
  xts_set_gpr(&x, 3, 0xFFFFFFF0ull);
  xts_set_sf(&x, 0);
  HT_CHECK(guard(xts_enc(XI_lwz, 0, 5, 0x20, 3), XTS_M_OK, why));
}

HTEST(guard_load_semantic_check_and_backend)
{
  xts_sink_tap = tap;
  for (int i = 0; i < NW; i++) {
    uint32_t w = build(i);
    if (!w) continue;
    xts_case_init(&x);
    xts_set_sf(&x, 1);
    xts_set_gpr(&x, 3, 0xD0010000ull);                  /* the USB controller's register window */
    xts_set_gpr(&x, 4, 0);
    cap_n = 0;
    cap_line[0] = 0;
    int st = xts_semantic_check(&gctx, &x, w, NULL);
    HT_EQ_U64(st, XTS_ST_SKIP);
    HT_EQ_U64(x.status, XTS_EXEC_NOTRUN);
    HT_EQ_U64(cap_n, 1);
    HT_CHECK(strstr(cap_line, "EA outside test memory") != NULL || strstr(cap_line, "model predicts DSI") != NULL);
    HT_CHECK(strstr(cap_line, "\"status\":\"skip\"") != NULL);
    /* a family calling xts_exec_word itself is stopped by the backend assertion */
    xts_case_init(&x);
    xts_set_sf(&x, 1);
    xts_set_gpr(&x, 3, 0xD0010000ull);
    xts_set_gpr(&x, 4, 0);
    HT_EQ_U64(xts_exec_word(&x, w), XTS_EXEC_NOTRUN);
    HT_CHECK(strstr(x.err, "EA outside test memory") != NULL);
    /* the public helper agrees */
    char why[256];
    HT_CHECK(xts_exec_ea_outside(w, &x.in, why, sizeof why) && strstr(why, "EA outside test memory"));
    /* legitimate: scratch + 128 runs (simulated / dry-run), no guard text */
    xts_case_init(&x);
    xts_set_sf(&x, 1);
    xts_set_ptr(&x, 3, 128);
    xts_set_gpr(&x, 4, 0);
    cap_n = 0;
    cap_line[0] = 0;
    xts_semantic_check(&gctx, &x, w, NULL);
    HT_EQ_U64(cap_n, 1);
    HT_CHECK(strstr(cap_line, "EA outside test memory") == NULL);
    HT_CHECK(x.status != XTS_EXEC_NOTRUN);
    HT_CHECK(!xts_exec_ea_outside(w, &x.in, why, sizeof why));
  }
  /* a non-memory word has no window */
  xts_case_init(&x);
  char why[256];
  HT_CHECK(!xts_exec_ea_outside(xts_enc(XI_addx, 0, 3, 4, 5), &x.in, why, sizeof why));
  HT_CHECK(!xts_exec_ea_outside(0, &x.in, why, sizeof why));
  xts_sink_tap = NULL;
  xts_sink_discard();
}
