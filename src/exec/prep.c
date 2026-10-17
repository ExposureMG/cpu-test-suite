/* Big-buffer preparation, pure part: port of the OpenXenos runner's hp_prep() (runner/hwprep.h). The cache work
 * (dcbf / touch) is done by xts_prep() in exec_common.c. Host-tested (host/tests). */
#include <string.h>
#include <xts/exec.h>

static uint32_t xs32(uint32_t *s)
{
  uint32_t x = *s;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return *s = x;
}

int xts_prep_pure(uint8_t *buf, uint32_t buf_addr, uint32_t buf_bytes, uint32_t base, uint32_t n, uint32_t stride,
                  int mode, int k, uint32_t seed, int slot8, uint64_t *span_out)
{
  int op = mode & 15;
  uint64_t span = (uint64_t)n * stride;
  if (!n || !stride || (base & 7) || (uint64_t)base + span > buf_bytes || (mode & 0x30) || op > 5) return -1;
  uint8_t *b = buf + base;
  uint32_t b_addr = buf_addr + base;
  if (op == 0 || op == 1) {
    uint32_t off = 8u * (uint32_t)slot8;
    if ((stride & 7) || off + 8 > stride || k < 1 || (uint32_t)k > n) return -1;
#define HP_IDX(i) (*(uint32_t *)(b + (uint64_t)(i) * stride + off))
    for (uint32_t i = 0; i < n; i++) HP_IDX(i) = (op == 0) ? i : (i + k < n ? i + k : i % k);
    if (op == 0) {
      uint32_t s = seed * 2654435761u + 0x9E3779B9u;
      if (!s) s = 1;
      for (uint32_t c = 0; c < (uint32_t)k; c++) {
        uint32_t m = (n - c + k - 1) / k;            /* elements in cycle c */
        for (uint32_t t = m - 1; t >= 1 && m > 1; t--) {   /* Sattolo: j < t, so the result is one cycle */
          uint32_t j = xs32(&s) % t;
          uint32_t pa = c + t * k, pb = c + j * k;
          uint32_t tmp = HP_IDX(pa); HP_IDX(pa) = HP_IDX(pb); HP_IDX(pb) = tmp;
        }
      }
    }
    for (uint32_t i = 0; i < n; i++) {
      uint32_t nx = HP_IDX(i);
      uint64_t ptr = (uint64_t)(uint32_t)(b_addr + nx * stride + off);
      xts_be_store(b + (uint64_t)i * stride + off, 8, ptr);    /* big-endian, as the CPU reads it */
    }
#undef HP_IDX
  } else if (op == 2) {
    memset(b, 0, (size_t)span);
  } else if (op == 5) {
    uint32_t s = seed | 1;
    for (uint64_t o = 0; o + 4 <= span; o += 4) xts_be_store(b + o, 4, xs32(&s));
  }
  if (span_out) *span_out = span;
  return mode;
}
