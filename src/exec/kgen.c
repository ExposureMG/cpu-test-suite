/* Kernel generation helpers (xts/kgen.h): 64-bit constants into a GPR. Portable (console + host dry run). */
#include <xts/xts.h>

/* value of a sign-extended 32-bit load: lis/ori (or li) */
static int fits_s16(int64_t v) { return v >= -32768 && v <= 32767; }
static int fits_s32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

/* emit r = sext32(v32) or r = zext32(v32): returns words */
static int load32(xts_kbuf_t *k, int r, uint32_t v, int zext, int emit)
{
  int64_t s = (int64_t)(int32_t)v;
  int n = 0;
  if (!zext || !(v & 0x80000000u)) {
    if (fits_s16(s)) {
      if (emit) xk_emit(k, XI_addi, 0, r, 0, s);
      return 1;
    }
    if (emit) xk_emit(k, XI_addis, 0, r, 0, (int64_t)(int16_t)(v >> 16));
    n = 1;
    if (v & 0xFFFF) {
      if (emit) xk_emit(k, XI_ori, 0, r, r, (int64_t)(v & 0xFFFF));
      n++;
    }
    return n;
  }
  /* zero-extended with bit 31 set: li r,0 ; oris ; [ori] (addis would sign-extend into the high word) */
  if (emit) {
    xk_emit(k, XI_addi, 0, r, 0, 0);
    xk_emit(k, XI_oris, 0, r, r, (int64_t)(v >> 16));
  }
  n = 2;
  if (v & 0xFFFF) {
    if (emit) xk_emit(k, XI_ori, 0, r, r, (int64_t)(v & 0xFFFF));
    n++;
  }
  return n;
}

static int li64(xts_kbuf_t *k, int r, uint64_t v, int emit)
{
  int64_t s = (int64_t)v;
  if (fits_s32(s)) return load32(k, r, (uint32_t)v, 0, emit);
  if ((v >> 32) == 0) return load32(k, r, (uint32_t)v, 1, emit);
  uint32_t hi = (uint32_t)(v >> 32), lo = (uint32_t)v;
  int n = load32(k, r, hi, 0, emit);          /* r = sext32(hi): the upper half is shifted out below */
  if (emit) xk_emit(k, XI_rldicrx, 0, r, r, 32, 31);   /* sldi r,r,32 */
  n++;
  if (lo >> 16) {
    if (emit) xk_emit(k, XI_oris, 0, r, r, (int64_t)(lo >> 16));
    n++;
  }
  if (lo & 0xFFFF) {
    if (emit) xk_emit(k, XI_ori, 0, r, r, (int64_t)(lo & 0xFFFF));
    n++;
  }
  return n;
}

int xk_li64(xts_kbuf_t *k, int reg, uint64_t v)
{
  if (!k || k->err) return -1;
  if (reg < 0 || reg > 31) return -1;
  uint32_t before = k->n;
  int n = li64(k, reg, v, 1);
  return (k->err || k->n - before != (uint32_t)n) ? -1 : n;
}

int xk_li64_words(uint64_t v) { return li64(NULL, 0, v, 0); }
