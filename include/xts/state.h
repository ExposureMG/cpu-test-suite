/* XTS - machine-state image shared by the slot (asm), the exec engine, the reference model and the tests.
 *
 * Layout (byte offsets) is the one of the OpenXenos runner (hwtest.h ST_*), extended by nothing: every field is 64-bit or
 * 16-byte aligned so the generated slot.S can load/store it with ld/lfd/lvx. On the console the struct IS the image
 * (big-endian, native). On the host the same struct holds the same LOGICAL values in host byte order.
 *
 * Vector registers are four 32-bit words, w[0] = the most significant word = element 0 = the lowest address when stored.
 * Use the xts_vr_* helpers for byte/halfword elements so code works on both builds.
 */
#ifndef XTS_STATE_H
#define XTS_STATE_H

#define XTS_ST_GPR      0x000   /* 32 x u64 */
#define XTS_ST_FPR      0x100   /* 32 x u64 */
#define XTS_ST_CR       0x200   /* u64, CR in the low 32 bits */
#define XTS_ST_XER      0x208
#define XTS_ST_LR       0x210
#define XTS_ST_CTR      0x218
#define XTS_ST_FPSCR    0x220   /* u64, FPSCR in the low 32 bits (the high half of the OUT image is whatever mffs gave) */
#define XTS_ST_OUTPTR   0x228   /* IN image: address of the OUT image (becomes r30 last) */
#define XTS_ST_SAVEDMSR 0x230   /* OUT image: the caller's MSR, restored after the case */
#define XTS_ST_MODE     0x238   /* IN image: XTS_MODE_* bits */
#define XTS_ST_VSCR     0x240   /* 16 bytes as stored by mfvscr/stvx (VSCR in the last word) */
#define XTS_ST_VR       0x250   /* 128 x 16 bytes */
#define XTS_ST_MARK     0xA50   /* OUT image: 16 bytes the slot landing pads write (zeroed before every case) */
#define XTS_ST_SIZE     0xA60

#define XTS_MODE_SF     0x1     /* run the case with MSR[SF]=1 (64-bit mode) */

/* Scratch memory a case may load/store: 512 bytes, 128-byte aligned. Pointer GPRs are set relative to it. */
#define XTS_SCRATCH_BYTES 512
/* Branch landing pads, byte offsets from the slot instruction (see tools/gen_slot.py):
 *   +4, +8, +12: "b post" (fall-through and short taken branches)
 *   +16: pad 1 writes MARK word 0, +24: pad 2 writes MARK word 1, then "b post" */
#define XTS_SLOT_PAD1   16
#define XTS_SLOT_PAD2   24
/* GPRs a test must never use: r1 (stack), r2, r13 (ABI), r30 (state pointer during a case) */
#define XTS_RESERVED_GPRS ((1u << 1) | (1u << 2) | (1u << 13) | (1u << 30))

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

typedef struct xts_vr { uint32_t w[4]; } xts_vr_t;

typedef struct xts_state {
  uint64_t gpr[32];
  uint64_t fpr[32];
  uint64_t cr;          /* low 32 bits */
  uint64_t xer;
  uint64_t lr;
  uint64_t ctr;
  uint64_t fpscr;       /* low 32 bits */
  uint64_t outptr;
  uint64_t savedmsr;
  uint64_t mode;        /* XTS_MODE_* */
  xts_vr_t vscr;        /* VSCR = vscr.w[3] */
  xts_vr_t vr[128];
  uint32_t mark[4];
} __attribute__((aligned(16))) xts_state_t;

_Static_assert(offsetof(xts_state_t, fpr) == XTS_ST_FPR, "state layout");
_Static_assert(offsetof(xts_state_t, cr) == XTS_ST_CR, "state layout");
_Static_assert(offsetof(xts_state_t, fpscr) == XTS_ST_FPSCR, "state layout");
_Static_assert(offsetof(xts_state_t, mode) == XTS_ST_MODE, "state layout");
_Static_assert(offsetof(xts_state_t, vscr) == XTS_ST_VSCR, "state layout");
_Static_assert(offsetof(xts_state_t, vr) == XTS_ST_VR, "state layout");
_Static_assert(offsetof(xts_state_t, mark) == XTS_ST_MARK, "state layout");
_Static_assert(sizeof(xts_state_t) == XTS_ST_SIZE, "state layout");

/* VSCR bits (in vscr.w[3]) */
#define XTS_VSCR_NJ  0x00010000u
#define XTS_VSCR_SAT 0x00000001u
/* XER bits (low 32 bits) */
#define XTS_XER_SO 0x80000000u
#define XTS_XER_OV 0x40000000u
#define XTS_XER_CA 0x20000000u

/* ---- vector element helpers (element 0 = most significant / lowest address) ---- */
static inline uint8_t xts_vr_u8(const xts_vr_t *v, int i) { return (uint8_t)(v->w[i >> 2] >> (24 - 8 * (i & 3))); }
static inline void xts_vr_set_u8(xts_vr_t *v, int i, uint8_t b)
{
  int s = 24 - 8 * (i & 3);
  v->w[i >> 2] = (v->w[i >> 2] & ~(0xFFu << s)) | ((uint32_t)b << s);
}
static inline uint16_t xts_vr_u16(const xts_vr_t *v, int i) { return (uint16_t)(v->w[i >> 1] >> (16 - 16 * (i & 1))); }
static inline void xts_vr_set_u16(xts_vr_t *v, int i, uint16_t h)
{
  int s = 16 - 16 * (i & 1);
  v->w[i >> 1] = (v->w[i >> 1] & ~(0xFFFFu << s)) | ((uint32_t)h << s);
}
static inline xts_vr_t xts_vr4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { xts_vr_t v = { { a, b, c, d } }; return v; }
/* big-endian byte array <-> vector (memory order) */
static inline xts_vr_t xts_vr_load(const uint8_t *p)
{
  xts_vr_t v;
  for (int i = 0; i < 4; i++)
    v.w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
  return v;
}
static inline void xts_vr_store(uint8_t *p, const xts_vr_t *v)
{
  for (int i = 0; i < 16; i++) p[i] = xts_vr_u8(v, i);
}

/* ---- big-endian memory access helpers (scratch memory is architectural memory) ---- */
static inline uint64_t xts_be_load(const uint8_t *p, int n)
{
  uint64_t v = 0;
  for (int i = 0; i < n; i++) v = (v << 8) | p[i];
  return v;
}
static inline void xts_be_store(uint8_t *p, int n, uint64_t v)
{
  for (int i = n - 1; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}

/* ---- canary baseline (identical to the OpenXenos runner make_baseline(), which results4 was recorded with) ----
 * GPR n = 0xC0DE0000000000|n<<8|0x5A, FPR n = 0x7FF8DEAD000000|n (a QNaN), VR n = A5A5|n 5A5A|n C3C3|n 3C3C|n,
 * CR/XER/LR/CTR/FPSCR/VSCR = 0, MODE = SF, scratch byte i = i*13+7. */
void xts_state_baseline(xts_state_t *s);
void xts_scratch_baseline(uint8_t *mem, int n);
static inline uint64_t xts_baseline_gpr(int n) { return 0xC0DE000000000000ull | ((uint64_t)n << 8) | 0x5A; }

/* Compare two OUT states the way the suite does: reserved GPRs, OUTPTR/SAVEDMSR/MODE and the high halves of CR/FPSCR
 * are ignored. Returns the number of differing items; the first `max` are described in diffs[] (may be NULL). */
typedef struct xts_state_diff {
  char what[12];          /* "gpr5", "fpr0", "vr100", "cr", "xer", "lr", "ctr", "fpscr", "vscr", "mark", "mem32" */
  uint64_t a[2], b[2];    /* values (vectors/memory blocks: two 64-bit halves) */
} xts_state_diff_t;
int xts_state_compare(const xts_state_t *a, const xts_state_t *b, xts_state_diff_t *diffs, int max);
/* Same for scratch memory, 16-byte blocks ("mem<offset>") */
int xts_mem_compare(const uint8_t *a, const uint8_t *b, int n, xts_state_diff_t *diffs, int max);
#endif
#endif
