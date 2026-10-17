/* XTS - instruction encoder / decoder (src/enc/enc.c), driven by the generated descriptor table (xts/isa.h).
 *
 * Operands are given in the descriptor's operand order, which is the order of the JSON asm_template, i.e. the GNU as
 * operand order:
 *
 *     xts_enc(XI_addx, XTS_OE | XTS_RC, 3, 4, 5)           addo. r3,r4,r5
 *     xts_enc(XI_lwz, 0, 3, -8, 4)                          lwz r3,-8(r4)       (template "lwz [RD], [d]([RA0])")
 *     xts_enc(XI_vmaddfp128, 0, 1, 2, 3, 1)                 vmaddfp128 v1,v2,v3,v1   (4th operand VS128 is tied to VD128)
 *     xts_enc(XI_rldiclx, 0, 3, 4, 40, 10)                  rldicl r3,r4,40,10
 *     xts_enc(XI_bcx, 0, 12, 2, 8)                          bc 12,2,.+8          (branch operands: byte displacement)
 *     xts_enc(XI_mfspr, 0, 3, 1008)                         mfspr r3,1008         (architectural SPR number)
 *
 * Values: register numbers; immediates as plain integers (signed ones may be negative); displacements in bytes.
 * The encoder range-checks every operand (and that tied operands are equal) and never silently masks.
 */
#ifndef XTS_ENC_H
#define XTS_ENC_H
#include <stdint.h>
#include <xts/isa.h>

enum xts_enc_err {
  XTS_ENC_OK = 0,
  XTS_ENC_BAD_INSN = -1,     /* id out of range */
  XTS_ENC_BAD_FLAGS = -2,    /* flag the instruction does not accept */
  XTS_ENC_RANGE = -3,        /* operand value out of range (or misaligned displacement) */
  XTS_ENC_TIED = -4,         /* VS128 != VD128 */
  XTS_ENC_NOPS = -5,         /* wrong operand count (xts_encode_n) */
};

/* Encode. ops[] has d->nops values. Returns XTS_ENC_OK and *word, or a negative error; *bad_op = failing operand index. */
int xts_encode(int insn, unsigned flags, const int64_t *ops, uint32_t *word, int *bad_op);
/* Convenience: operands as a plain argument list (any integer type; converted to int64_t by the macro, the count is
 * checked against d->nops). On error returns 0 and stores the error in xts_enc_last_error / xts_enc_last_bad_op
 * (0 is not a valid word for any table entry). Zero-operand instructions: xts_enc(XI_sync, 0). */
#define xts_enc(insn, flags, ...) \
  xts_enc_a((insn), (flags), (const int64_t[]){ __VA_ARGS__ }, (int)(sizeof((const int64_t[]){ __VA_ARGS__ }) / sizeof(int64_t)))
uint32_t xts_enc_a(int insn, unsigned flags, const int64_t *ops, int nops);
extern int xts_enc_last_error, xts_enc_last_bad_op;
const char *xts_enc_strerror(int err);

/* Decode: every descriptor whose (word & mask) == match. Returns the number of hits (up to max stored in ids[]).
 * Some reserved forms decode to more than one entry; xts_decode1 returns the first (lowest id) or -1. */
int xts_decode(uint32_t word, int *ids, int max);
int xts_decode1(uint32_t word);
/* 1 if `word` decodes to `insn` (among possibly several hits) */
int xts_decodes_as(uint32_t word, int insn);

/* Operand value back out of a word (same conventions as xts_encode: signed fields sign-extended, SPR un-swapped,
 * branch displacement in bytes). */
int64_t xts_extract(int insn, uint32_t word, int opidx);
int64_t xts_field_get(int field, uint32_t word);              /* by field id */
uint32_t xts_field_put(int field, int64_t value, uint32_t word);   /* no range check */
int xts_field_check(int field, int64_t value);                /* 1 if value is representable */
unsigned xts_flags_of(int insn, uint32_t word);               /* XTS_RC|XTS_OE|... set in the word */

/* "addo. 3,4,5" style text (numbers only, GNU operand order). Returns buf. */
char *xts_disasm(uint32_t word, char *buf, int size);
char *xts_format(int insn, uint32_t word, char *buf, int size);   /* as a specific descriptor */
#endif
