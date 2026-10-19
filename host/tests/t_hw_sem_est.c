/* Hardware semantic replay for the estimate family: the 28 failure records of the console run on Jasper (PVR 0x00710500)
 * that exposed two hardware rules the estimate checker did not know, replayed through xts_estimate_verdict.
 *   vexptefp / vexptefp128  every denormal RESULT is flushed to +0 although VSCR[NJ]=0 (inputs -127 .. -152, integral and
 *                           not); results down to 2^-126 are exact. The checker used to expect the exact denormal.
 *   fresx                   FPSCR[OE]=1 (MSR[FE0/FE1]=0, no interrupt) and a reciprocal beyond the single range: the
 *                           result is the 8-bit table value times 2^-192 (enabled-overflow exponent adjust), not +-inf.
 * Besides replaying every record this checks that clearly wrong outputs are still rejected. Records are data from the
 * console; only the input/output bits are used (all had VSCR[NJ]=0, FPSCR as listed for fres). */
#include <string.h>
#include <xts/xts.h>
#include "htest.h"

typedef struct {
  int insn;
  uint32_t word, fpscr;
  const char *id;
  uint64_t in[4], out[4];        /* vector: 4 lanes of single bits; fres: [0] = double bits of FB / FD */
} rec_t;

static const rec_t R[] = {
  { XI_vexptefp, 0x1060218A, 0, "vexptefp.exp#304 sf64", { 0xC3010000ull, 0xC30307FFull, 0xC3050400ull, 0xC3070000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp, 0x1060218A, 0, "vexptefp.exp#305 sf64", { 0xC30907FFull, 0xC30B0400ull, 0xC30D0000ull, 0xC30F07FFull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp, 0x1060218A, 0, "vexptefp.exp#306 sf64", { 0xC3110400ull, 0xC3130000ull, 0xC31507FFull, 0xC3170400ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp, 0x1060218A, 0, "vexptefp.int#2 sf64", { 0xC3180000ull, 0xC3170000ull, 0xC3160000ull, 0xC3150000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp, 0x1060218A, 0, "vexptefp.int#3 sf64", { 0xC3140000ull, 0xC3130000ull, 0xC3120000ull, 0xC3110000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp, 0x1060218A, 0, "vexptefp.int#4 sf64", { 0xC3100000ull, 0xC30F0000ull, 0xC30E0000ull, 0xC30D0000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp, 0x1060218A, 0, "vexptefp.int#5 sf64", { 0xC30C0000ull, 0xC30B0000ull, 0xC30A0000ull, 0xC3090000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp, 0x1060218A, 0, "vexptefp.int#6 sf64", { 0xC3080000ull, 0xC3070000ull, 0xC3060000ull, 0xC3050000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp, 0x1060218A, 0, "vexptefp.int#7 sf64", { 0xC3040000ull, 0xC3030000ull, 0xC3020000ull, 0xC3010000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp, 0x1060218A, 0, "vexptefp.int#8 sf64", { 0xC3000000ull, 0xC2FE0000ull, 0xC2FC0000ull, 0xC2FA0000ull }, { 0x00000000ull, 0x00000000ull, 0x00800000ull, 0x01000000ull } },
  { XI_vexptefp, 0x1060218A, 0, "vexptefp.edge#9 sf64", { 0xC2FE0000ull, 0xC3000000ull, 0xC3150000ull, 0xC3158000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp128, 0x19006EB6, 0, "vexptefp128.int#2 sf64", { 0xC3180000ull, 0xC3170000ull, 0xC3160000ull, 0xC3150000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp128, 0x19006EB6, 0, "vexptefp128.int#3 sf64", { 0xC3140000ull, 0xC3130000ull, 0xC3120000ull, 0xC3110000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp128, 0x19006EB6, 0, "vexptefp128.int#4 sf64", { 0xC3100000ull, 0xC30F0000ull, 0xC30E0000ull, 0xC30D0000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp128, 0x19006EB6, 0, "vexptefp128.int#5 sf64", { 0xC30C0000ull, 0xC30B0000ull, 0xC30A0000ull, 0xC3090000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp128, 0x19006EB6, 0, "vexptefp128.int#6 sf64", { 0xC3080000ull, 0xC3070000ull, 0xC3060000ull, 0xC3050000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp128, 0x19006EB6, 0, "vexptefp128.int#7 sf64", { 0xC3040000ull, 0xC3030000ull, 0xC3020000ull, 0xC3010000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp128, 0x19006EB6, 0, "vexptefp128.int#8 sf64", { 0xC3000000ull, 0xC2FE0000ull, 0xC2FC0000ull, 0xC2FA0000ull }, { 0x00000000ull, 0x00000000ull, 0x00800000ull, 0x01000000ull } },
  { XI_vexptefp128, 0x19006EB6, 0, "vexptefp128.edge#9 sf64", { 0xC2FE0000ull, 0xC3000000ull, 0xC3150000ull, 0xC3158000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp, 0x1060218A, 0, "vexptefp.exp#304 sf32", { 0xC3010000ull, 0xC30307FFull, 0xC3050400ull, 0xC3070000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp, 0x1060218A, 0, "vexptefp.int#8 sf32", { 0xC3000000ull, 0xC2FE0000ull, 0xC2FC0000ull, 0xC2FA0000ull }, { 0x00000000ull, 0x00000000ull, 0x00800000ull, 0x01000000ull } },
  { XI_vexptefp, 0x1060218A, 0, "vexptefp.edge#9 sf32", { 0xC2FE0000ull, 0xC3000000ull, 0xC3150000ull, 0xC3158000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_vexptefp128, 0x19006EB6, 0, "vexptefp128.int#8 sf32", { 0xC3000000ull, 0xC2FE0000ull, 0xC2FC0000ull, 0xC2FA0000ull }, { 0x00000000ull, 0x00000000ull, 0x00800000ull, 0x01000000ull } },
  { XI_vexptefp128, 0x19006EB6, 0, "vexptefp128.edge#9 sf32", { 0xC2FE0000ull, 0xC3000000ull, 0xC3150000ull, 0xC3158000ull }, { 0x00000000ull, 0x00000000ull, 0x00000000ull, 0x00000000ull } },
  { XI_fresx, 0xEFA00031, 0x00000040, "fresx.rc#13 sf64", { 0x800E7FB0D1011973ull }, { 0xF3D1B00000000000ull } },
  { XI_fresx, 0xEEE08830, 0x00000040, "fresx#14 sf64", { 0x01A56E1FC2F8F359ull }, { 0x7237E00000000000ull } },
  { XI_fresx, 0xEE208031, 0x00000040, "fresx.rc#23 sf64", { 0xB1179DF1705276C3ull }, { 0xC2C5C00000000000ull } },
  { XI_fresx, 0xEDC0E031, 0x000000F8, "fresx.rc#11 sf64", { 0x0000000000000001ull }, { 0x770FE00000000000ull } },
};
#define NR ((int)(sizeof R / sizeof R[0]))

static int is_fres(const rec_t *r) { return r->insn == XI_fresx; }

static int reg(const rec_t *r, const char *a, const char *b)
{
  const xts_insn_t *d = xts_insn(r->insn);
  int i = xts_op_index(d, a);
  if (i < 0) i = xts_op_index(d, b);
  return i < 0 ? -1 : (int)xts_extract(d->index, r->word, i);
}

static xts_state_t IN, OUT;

/* load record r into IN/OUT; out[] overrides the recorded CPU result when given */
static void load(const rec_t *r, const uint64_t *out, uint32_t vscr_nj, uint32_t fpscr)
{
  xts_state_baseline(&IN);
  if (is_fres(r)) {
    IN.fpr[reg(r, "FB", "FB") & 31] = r->in[0];
    IN.fpscr = fpscr;
  } else {
    IN.vscr = xts_vr4(0, 0, 0, vscr_nj ? XTS_VSCR_NJ : 0);
    IN.vr[reg(r, "VB", "VB128") & 127] = xts_vr4((uint32_t)r->in[0], (uint32_t)r->in[1], (uint32_t)r->in[2], (uint32_t)r->in[3]);
  }
  OUT = IN;
  xm_state_loaded(&OUT);
  const uint64_t *o = out ? out : r->out;
  if (is_fres(r)) {
    OUT.fpr[reg(r, "FD", "FD") & 31] = o[0];
  } else {
    OUT.vr[reg(r, "VD", "VD128") & 127] = xts_vr4((uint32_t)o[0], (uint32_t)o[1], (uint32_t)o[2], (uint32_t)o[3]);
  }
}

static int verdict(const rec_t *r, const uint64_t *out, uint32_t vscr_nj, uint32_t fpscr, char *js, size_t cap)
{
  load(r, out, vscr_nj, fpscr);
  xts_json_t j;
  if (js) {
    xj_init(&j, js, cap);
    xj_obj(&j);
  }
  int st = xts_estimate_verdict(r->insn, r->word, &IN, &OUT, js ? &j : NULL);
  if (js) xj_finish(&j);
  return st;
}

/* every one of the 28 console records is a pass now */
HTEST(hw_est_replay_all)
{
  int n[3] = { 0, 0, 0 }, ok[3] = { 0, 0, 0 };
  for (int i = 0; i < NR; i++) {
    int k = R[i].insn == XI_vexptefp ? 0 : R[i].insn == XI_vexptefp128 ? 1 : 2;
    int st = verdict(&R[i], NULL, 0, R[i].fpscr, NULL, 0);
    n[k]++;
    ok[k] += st == XTS_ST_PASS;
    if (st != XTS_ST_PASS) printf("      %s -> status %d\n", R[i].id, st);
  }
  printf("      matched/total: vexptefp %d/%d  vexptefp128 %d/%d  fresx %d/%d\n", ok[0], n[0], ok[1], n[1], ok[2], n[2]);
  HT_EQ_U64(n[0], 14);
  HT_EQ_U64(n[1], 10);
  HT_EQ_U64(n[2], 4);
  HT_EQ_U64(ok[0], 14);
  HT_EQ_U64(ok[1], 10);
  HT_EQ_U64(ok[2], 4);
}

/* vexptefp: every lane is now an exact verdict (denormal results and the former "unknown" non-integral lanes alike) */
HTEST(hw_est_vexptefp_lanes_exact)
{
  char js[2048];
  for (int i = 0; i < NR; i++) {
    if (is_fres(&R[i])) continue;
    HT_EQ_U64(verdict(&R[i], NULL, 0, 0, js, sizeof js), XTS_ST_PASS);
    HT_EQ_U64(ht_count_substr(js, "\"verdict\":\"exact\""), 4);
    HT_EQ_U64(ht_count_substr(js, "\"ok\":false"), 0);
  }
  /* the rule does not depend on NJ in the checker (denormal results are flushed either way; NJ=1 is the architected case) */
  for (int i = 0; i < NR; i++)
    if (!is_fres(&R[i])) HT_EQ_U64(verdict(&R[i], NULL, 1, 0, NULL, 0), XTS_ST_PASS);
}

/* vexptefp: an output that keeps the denormal (or anything but +0 below 2^-126) is rejected, lane by lane */
HTEST(hw_est_vexptefp_wrong_rejected)
{
  for (int i = 0; i < NR; i++) {
    if (is_fres(&R[i])) continue;
    for (int l = 0; l < 4; l++) {
      static const uint64_t bad[] = { 0x00000001, 0x00400000, 0x007FFFFF, 0x00800001, 0x3F800000, 0x7F800000, 0x80000000 };
      for (unsigned b = 0; b < sizeof bad / sizeof bad[0]; b++) {
        uint64_t o[4];
        memcpy(o, R[i].out, sizeof o);
        if (o[l] == bad[b]) continue;
        o[l] = bad[b];
        int st = verdict(&R[i], o, 0, 0, NULL, 0);
        if (st != XTS_ST_FAIL) printf("      %s lane %d out %llX -> %d\n", R[i].id, l, (unsigned long long)bad[b], st);
        HT_EQ_U64(st, XTS_ST_FAIL);
      }
    }
  }
  /* the two lanes that are normal results (2^-126, 2^-125) stay bit exact: one ulp off or flushed is a failure */
  const rec_t *r = NULL;
  for (int i = 0; i < NR; i++)
    if (!strcmp(R[i].id, "vexptefp.int#8 sf64")) r = &R[i];
  HT_CHECK(r != NULL);
  if (!r) return;
  uint64_t o[4];
  memcpy(o, r->out, sizeof o);
  o[3] = 0x01000001;
  HT_EQ_U64(verdict(r, o, 0, 0, NULL, 0), XTS_ST_FAIL);
  o[3] = 0;
  HT_EQ_U64(verdict(r, o, 0, 0, NULL, 0), XTS_ST_FAIL);
  o[3] = 0x01000000;
  o[2] = 0x00800001;
  HT_EQ_U64(verdict(r, o, 0, 0, NULL, 0), XTS_ST_FAIL);
  o[2] = 0x00800000;
  HT_EQ_U64(verdict(r, o, 0, 0, NULL, 0), XTS_ST_PASS);
  /* other state changing is still caught */
  load(r, NULL, 0, 0);
  OUT.gpr[5] ^= 1;
  HT_EQ_U64(xts_estimate_verdict(r->insn, r->word, &IN, &OUT, NULL), XTS_ST_FAIL);
}

/* vexptefp: the neighbourhood of the flush boundary keeps its exact / unknown verdicts */
HTEST(hw_est_vexptefp_boundary)
{
  static const rec_t b = { XI_vexptefp, 0x1060218A, 0, "boundary", { 0xC2FC0000ull, 0xC2FE0000ull, 0xC2FDFFFFull, 0x43000000ull },
                           { 0x00800000ull, 0, 0, 0x7F800000ull } };
  char js[2048];
  /* -126 -> 2^-126 exact, -127 -> flushed (exact), -126.99999 -> not decided by the model (unknown), 128 -> +inf */
  HT_EQ_U64(verdict(&b, NULL, 0, 0, js, sizeof js), XTS_ST_PASS);
  HT_EQ_U64(ht_count_substr(js, "\"verdict\":\"exact\""), 3);
  HT_EQ_U64(ht_count_substr(js, "\"verdict\":\"unknown\""), 1);
  /* the unknown lane carries no verdict: whatever the hardware answers there is not a failure */
  uint64_t o[4] = { 0x00800000ull, 0, 0x00400000ull, 0x7F800000ull };
  HT_EQ_U64(verdict(&b, o, 0, 0, NULL, 0), XTS_ST_PASS);
  o[1] = 0x00400000ull;                          /* -127 must flush */
  HT_EQ_U64(verdict(&b, o, 0, 0, NULL, 0), XTS_ST_FAIL);
}

static const rec_t *find(const char *id)
{
  for (int i = 0; i < NR; i++)
    if (!strcmp(R[i].id, id)) return &R[i];
  return NULL;
}

/* fres with FPSCR[OE]: estimate * 2^-192, strict about the table bound and the scale */
HTEST(hw_est_fres_oe_scaled)
{
  char js[1024];
  for (int i = 0; i < NR; i++) {
    if (!is_fres(&R[i])) continue;
    int st = verdict(&R[i], NULL, 0, R[i].fpscr, js, sizeof js);
    HT_EQ_U64(st, XTS_ST_PASS);
    HT_CHECK(strstr(js, "\"verdict\":\"bound\"") && strstr(js, "\"exp_adjust\":-192") && strstr(js, "\"ok\":true"));
    const uint64_t c = R[i].out[0], e52 = 1ull << 52;
    uint64_t o[1];
    const uint64_t bad[] = {
      c + 192 * e52,                              /* unscaled reciprocal estimate (what the checker used to expect) */
      c + 1 * e52, c - 1 * e52,                   /* scale off by 2^1 */
      c + 64 * e52, c - 64 * e52,
      c ^ (1ull << 63),                           /* wrong sign */
      c + (1ull << 46),                           /* 2^-6 relative error in the table value */
      0, 1ull << 63,                              /* zero */
      0x7FF0000000000000ull, 0xFFF0000000000000ull, /* +-inf (overflow without OE) */
      0x47EFFFFFE0000000ull,                      /* max single */
      0x7FF8000000000000ull,
    };
    for (unsigned b = 0; b < sizeof bad / sizeof bad[0]; b++) {
      o[0] = bad[b];
      int s2 = verdict(&R[i], o, 0, R[i].fpscr, NULL, 0);
      if (s2 != XTS_ST_FAIL) printf("      %s out %llX -> %d\n", R[i].id, (unsigned long long)bad[b], s2);
      HT_EQ_U64(s2, XTS_ST_FAIL);
    }
    /* without OE the same output is no longer explained */
    HT_EQ_U64(verdict(&R[i], NULL, 0, R[i].fpscr & ~XTS_FPSCR_OE, NULL, 0), XTS_ST_FAIL);
  }
  /* x = 2^-1074 with all enables: table value 255/256 * 2 * 2^881 */
  const rec_t *r = find("fresx.rc#11 sf64");
  HT_CHECK(r != NULL);
  if (r) HT_EQ_U64(r->out[0], 0x770FE00000000000ull);
}

/* fres with OE set but an ordinary operand stays a strict 2^-8 check; the boundary zone accepts either form */
HTEST(hw_est_fres_oe_normal_strict)
{
  static const rec_t t = { XI_fresx, 0xEE208031, XTS_FPSCR_OE, "fres 3.0", { 0x4008000000000000ull },
                           { 0x3FD5555555555555ull } };
  HT_EQ_U64(verdict(&t, NULL, 0, XTS_FPSCR_OE, NULL, 0), XTS_ST_PASS);
  uint64_t o[1] = { 0x3FD5555555555555ull + (1ull << 46) };          /* 2^-6 off */
  HT_EQ_U64(verdict(&t, o, 0, XTS_FPSCR_OE, NULL, 0), XTS_ST_FAIL);
  o[0] = 0x3FD5555555555555ull + (192ull << 52);                    /* scaled result of an in-range operand */
  HT_EQ_U64(verdict(&t, o, 0, XTS_FPSCR_OE, NULL, 0), XTS_ST_FAIL);
  /* x = 2^-128: reciprocal 2^128 is beyond the single range or not depending on the table; both forms are accepted */
  static const rec_t z = { XI_fresx, 0xEE208031, XTS_FPSCR_OE, "fres 2^-128", { 0x37F0000000000000ull }, { 0 } };
  o[0] = 0x47EFE00000000000ull;                                      /* ~2^128 (255/256): unscaled */
  HT_EQ_U64(verdict(&z, o, 0, XTS_FPSCR_OE, NULL, 0), XTS_ST_PASS);
  o[0] = 0x47EFE00000000000ull - (192ull << 52);                     /* the same scaled by 2^-192 */
  HT_EQ_U64(verdict(&z, o, 0, XTS_FPSCR_OE, NULL, 0), XTS_ST_PASS);
  o[0] = 0x47EFE00000000000ull - (100ull << 52);
  HT_EQ_U64(verdict(&z, o, 0, XTS_FPSCR_OE, NULL, 0), XTS_ST_FAIL);
}
