/* memory guard: cache-block and prefetch ops (dcbf dcbst dcbt dcbtst icbi xdcbt dcbz dcbz128 dcba dst dstst) are range-checked
 * against their 128-byte line / base address whatever the model says (the model has them as no-ops, or dcbz via DSI), so a
 * wild EA - MMIO, flash, the PCI/USB registers, the r0 canary - is never executed; scratch and the big buffer still are. */
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
static const xts_family_t gfam = { .name = "guard_cache", .kind = "semantic" };
static xts_case_t gcase = { .id = "g#0", .insn = -1 };
static xts_ctx_t gctx = { .fam = &gfam, .c = &gcase, .block_key = "r0:guard.cache:-:c0" };

/* word for cache op `insn` with RA field 0 (RA|0 = 0) and RB = r4, or RA = r3 and RB = r4 for the ops whose RA is not RA|0 */
static uint32_t cw(int insn, int ra) { return insn == XI_dst || insn == XI_dstst ? xts_enc(insn, 0, ra, 4, 0) : xts_enc(insn, 0, ra, 4); }

static const int cache_ops[] = { XI_dcbf, XI_dcbst, XI_dcbt, XI_dcbtst, XI_icbi, XI_xdcbt, XI_dcbz, XI_dcbz128, XI_dcba };
static const int model_results[] = { XTS_M_OK, XTS_M_UNIMPLEMENTED, XTS_M_ILLEGAL, XTS_M_ALIGNMENT, XTS_M_TRAP };

static int guard(uint32_t w, int mres, char *why)
{
  return xts_exec_mem_guard(xts_decode1(w), w, &x.in, mres, 0, why, 256);
}

HTEST(guard_cache_wild_ea_blocked)
{
  const uint64_t sc = xts_scratch_addr();
  const uint64_t wild[] = { 0xC0DEC0DEC0DEC0DEull, 0, 0xC0000000ull, 0xD0010000ull, 0xEA000000ull, 0x80000000ull, ~0ull,
                            sc - 128, sc - 1, sc + XTS_SCRATCH_BYTES, sc + XTS_SCRATCH_BYTES + 127, 0x100000000ull + sc };
  char why[256];
  for (unsigned i = 0; i < sizeof cache_ops / sizeof *cache_ops; i++)
    for (unsigned k = 0; k < sizeof wild / sizeof *wild; k++)
      for (unsigned m = 0; m < sizeof model_results / sizeof *model_results; m++) {
        xts_case_init(&x);
        xts_set_sf(&x, 1);
        xts_set_gpr(&x, 3, wild[k]);       /* base register */
        xts_set_gpr(&x, 4, 0);
        why[0] = 0;
        int b = guard(cw(cache_ops[i], 3), model_results[m], why);
        if (!b) printf("      %s base %llX model %d not blocked\n", xts_isa[cache_ops[i]].mnem, (unsigned long long)wild[k], model_results[m]);
        HT_CHECK(b);
        HT_CHECK(strstr(why, "EA outside test memory") != NULL);
        /* the same address through RB with RA = 0 (RA|0) */
        xts_set_gpr(&x, 0, 0);             /* icbi/dst: the descriptor does not mark RA as RA|0, so RA = 0 is also read as GPR[0] */
        xts_set_gpr(&x, 3, 0x1000);
        xts_set_gpr(&x, 4, wild[k]);
        HT_CHECK(guard(cw(cache_ops[i], 0), model_results[m], why));
      }
  /* a DSI prediction still blocks, with its own reason */
  xts_case_init(&x);
  xts_set_ptr(&x, 3, 128);
  xts_set_gpr(&x, 4, 0);
  HT_CHECK(guard(cw(XI_dcbz, 3), XTS_M_DSI, why) && strstr(why, "DSI"));
}

HTEST(guard_cache_inside_runs)
{
  const uint64_t sc = xts_scratch_addr();
  char why[256];
  for (unsigned i = 0; i < sizeof cache_ops / sizeof *cache_ops; i++)
    for (unsigned m = 0; m < sizeof model_results / sizeof *model_results; m++) {
      xts_case_init(&x);
      xts_set_sf(&x, 1);
      static const int offs[] = { 0, 1, 31, 127, 128, 255, 256, 384, 511 };     /* any byte of the 4 lines of scratch */
      for (unsigned k = 0; k < sizeof offs / sizeof *offs; k++) {
        xts_set_ptr(&x, 3, offs[k]);
        xts_set_gpr(&x, 4, 0);
        why[0] = 0;
        HT_CHECK(!guard(cw(cache_ops[i], 3), model_results[m], why));
        xts_set_gpr(&x, 0, 0);
        xts_set_gpr(&x, 3, 0);
        xts_set_ptr(&x, 4, offs[k]);
        HT_CHECK(!guard(cw(cache_ops[i], 0), model_results[m], why));
      }
    }
  /* 32-bit mode ignores the upper half: 2^32 + scratch is scratch there and wild in 64-bit mode */
  xts_case_init(&x);
  xts_set_gpr(&x, 3, 0x100000000ull + sc + 128);
  xts_set_gpr(&x, 4, 0);
  xts_set_sf(&x, 0);
  HT_CHECK(!guard(cw(XI_dcbz, 3), XTS_M_OK, why));
  xts_set_sf(&x, 1);
  HT_CHECK(guard(cw(XI_dcbz, 3), XTS_M_OK, why));
  /* the big buffer is test memory too, with its own 128-byte lines */
  uint64_t ba = 0;
  void *big = xts_big(1u << 20, &ba);
  HT_CHECK(big != NULL);
  if (big) {
    uint64_t end = ba + xts_big_size();
    xts_set_sf(&x, 1);
    xts_set_gpr(&x, 3, ba + 4096);
    HT_CHECK(!guard(cw(XI_xdcbt, 3), XTS_M_OK, why));
    xts_set_gpr(&x, 3, end - 1);
    HT_CHECK(!guard(cw(XI_dcbz128, 3), XTS_M_OK, why));
    xts_set_gpr(&x, 3, end);
    HT_CHECK(guard(cw(XI_xdcbt, 3), XTS_M_OK, why));
    xts_set_gpr(&x, 3, ba - 1);
    HT_CHECK(guard(cw(XI_dcbz128, 3), XTS_M_OK, why));
  }
}

HTEST(guard_cache_stream_prefetch)
{
  /* dst/dstst: RB is the stream control word, not an address; only the base RA must be in range */
  char why[256];
  const int ops[] = { XI_dst, XI_dstst };
  for (int i = 0; i < 2; i++) {
    xts_case_init(&x);
    xts_set_sf(&x, 1);
    xts_set_ptr(&x, 3, 128);
    xts_set_gpr(&x, 4, 0xC0DEC0DEC0DEC0DEull);                 /* control word: block size / count / stride */
    HT_CHECK(!guard(cw(ops[i], 3), XTS_M_OK, why));
    HT_CHECK(!guard(cw(ops[i], 3), XTS_M_UNIMPLEMENTED, why));
    xts_set_gpr(&x, 3, 0xC0000000ull);                          /* base in MMIO */
    xts_set_gpr(&x, 4, 0);
    HT_CHECK(guard(cw(ops[i], 3), XTS_M_OK, why) && strstr(why, "EA outside test memory"));
    HT_CHECK(guard(cw(ops[i], 3), XTS_M_UNIMPLEMENTED, why));
    xts_set_gpr(&x, 3, 0xD0000000ull);
    HT_CHECK(guard(cw(ops[i], 3), XTS_M_ILLEGAL, why));
  }
  /* dss has no address at all */
  xts_case_init(&x);
  uint32_t dss = xts_enc(XI_dss, 0, 0);
  HT_CHECK(dss && !guard(dss, XTS_M_OK, why));
}

HTEST(guard_cache_semantic_check_skips)
{
  xts_sink_tap = tap;
  const uint64_t sc = xts_scratch_addr();
  for (unsigned i = 0; i < sizeof cache_ops / sizeof *cache_ops; i++) {
    uint32_t w = cw(cache_ops[i], 3);
    /* wild: never executed, a skip record with the reason, x untouched */
    xts_case_init(&x);
    xts_set_sf(&x, 1);
    xts_set_gpr(&x, 3, 0xC0DE000000000000ull);
    xts_set_gpr(&x, 4, 0);
    cap_n = 0;
    cap_line[0] = 0;
    int st = xts_semantic_check(&gctx, &x, w, NULL);
    HT_EQ_U64(st, XTS_ST_SKIP);
    HT_EQ_U64(x.status, XTS_EXEC_NOTRUN);
    HT_EQ_U64(cap_n, 1);
    /* dcbz/dcbz128 are modelled as a store: the model's own DSI verdict comes first; every other op is a model no-op */
    HT_CHECK(strstr(cap_line, "EA outside test memory") != NULL ||
             (strstr(cap_line, "model predicts DSI") != NULL && (cache_ops[i] == XI_dcbz || cache_ops[i] == XI_dcbz128)));
    HT_CHECK(strstr(cap_line, "\"status\":\"skip\"") != NULL);
    /* the backend assertion refuses the same word when a family calls xts_exec_word directly */
    xts_case_init(&x);
    xts_set_sf(&x, 1);
    xts_set_gpr(&x, 3, 0xC0DE000000000000ull);
    xts_set_gpr(&x, 4, 0);
    HT_EQ_U64(xts_exec_word(&x, w), XTS_EXEC_NOTRUN);
    HT_CHECK(strstr(x.err, "EA outside test memory") != NULL);
    /* inside scratch: runs (the host simulates or dry-runs it) */
    xts_case_init(&x);
    xts_set_sf(&x, 1);
    xts_set_ptr(&x, 3, 128);
    xts_set_gpr(&x, 4, 0);
    cap_n = 0;
    cap_line[0] = 0;
    xts_semantic_check(&gctx, &x, w, NULL);
    HT_EQ_U64(cap_n, 1);
    HT_CHECK(strstr(cap_line, "EA outside test memory") == NULL);
    HT_CHECK(x.status == XTS_EXEC_SIMULATED || x.status == XTS_EXEC_DRYRUN || x.status == XTS_EXEC_OK);
  }
  (void)sc;
  /* dcbz really zeroes its line in the host model when it is inside scratch */
  xts_case_init(&x);
  xts_set_sf(&x, 1);
  xts_set_ptr(&x, 3, 128 + 5);
  xts_set_gpr(&x, 4, 0);
  HT_CHECK(x.mem_in[130] != 0);
  int st = xts_exec_word(&x, cw(XI_dcbz128, 3));
  HT_CHECK(st == XTS_EXEC_SIMULATED || st == XTS_EXEC_DRYRUN);
  if (st == XTS_EXEC_SIMULATED) HT_EQ_U64(x.mem_out[130], 0);
  xts_sink_tap = NULL;
  xts_sink_discard();
}
