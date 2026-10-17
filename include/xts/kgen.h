/* XTS - kernel generation helpers (src/exec/kgen.c): constants into registers, and condition-vector queries that kernels
 * depend on. Part of <xts/xts.h>.
 *
 *   xk_li64(k, 5, 0xFFFFFFFF80000000ull);    // any 64-bit value, 1..5 instructions, never touches anything but r5
 *   xk_li32(k, 6, 0x80000000u);              // zero-extended: r6 = 0x0000000080000000
 *   xk_li32s(k, 7, 0x80000000u);             // sign-extended: r7 = 0xFFFFFFFF80000000
 *
 * Sequences (shortest first; only addi/addis/ori/oris/rldicr, so they behave the same with MSR[SF]=0 or 1, set no CR/XER
 * bits and never read another register; r0 as the target is fine because addi/addis use the literal 0 for RA=0):
 *   v fits signed 16 bits                    li  r,v
 *   v = sign-extended 32-bit value           lis r,hi ; [ori r,r,lo]
 *   v < 2^32 (bit 31 set)                    li r,0 ; oris r,r,hi ; [ori r,r,lo]
 *   anything else                            <high word as above> ; rldicr r,r,32,31 ; [oris r,r,..] ; [ori r,r,..]
 * Replaces the families' mu_li32 (which dropped bit 31), xe_emit_const, emit_li and emit_addr.
 */
#ifndef XTS_KGEN_H
#define XTS_KGEN_H
#include <stdint.h>
#include <xts/kernel.h>

struct xts_ctx;

/* r = v. Returns the number of words emitted (1..5) or -1 (bad register / kernel error). */
int xk_li64(xts_kbuf_t *k, int reg, uint64_t v);
/* r = (uint64_t)v (zero-extended) */
static inline int xk_li32(xts_kbuf_t *k, int reg, uint32_t v) { return xk_li64(k, reg, (uint64_t)v); }
/* r = (int64_t)(int32_t)v (sign-extended) */
static inline int xk_li32s(xts_kbuf_t *k, int reg, uint32_t v) { return xk_li64(k, reg, (uint64_t)(int64_t)(int32_t)v); }
/* number of words xk_li64 emits for v (for layout calculations) */
int xk_li64_words(uint64_t v);

/* 1 if axis `axis` was applied for the block of this ctx (profile row selected it and the family lists it), 0 if it is at
 * its default because it was not applied, -1 if no such axis is registered (or ctx has no condition vector). */
int xts_cond_applied(const struct xts_ctx *ctx, const char *axis);
/* value index of `axis` in this ctx's condition vector (0 = default when not applied), -1 if not registered */
int xts_cond_value(const struct xts_ctx *ctx, const char *axis);
#endif
