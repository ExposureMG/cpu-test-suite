/* XTS - JIT kernel builder (src/exec/kbuf.c). Families build kernels ON THE CONSOLE with the C encoder; on the PC the same
 * code runs in the host dry run, where every word is decoded back and must name the instruction it was built for.
 *
 *   xts_kbuf_t *k = xk_new(4096);                 // capacity in words (kept for the whole run; reuse with xk_reset)
 *   xk_reset(k);
 *   xk_emit(k, XI_addi, 0, 9, 0, 1000);           // li r9,1000   (operands in descriptor order, see xts/enc.h)
 *   int top = xk_loop_begin(k, 9);                // mtctr r9 ; top:
 *   xk_emit(k, XI_addx, 0, 3, 3, 4);
 *   xk_loop_end(k, top);                          // bdnz top
 *   xk_exit(k);                                   // branch back to the state-store epilogue (patched at run time)
 *   if (xk_finish(k) < 0) ... k->err ...
 *   xts_exec_kernel(&x, k);
 *
 * Kernels must not touch r1, r2, r13 or r30 (r30 holds the OUT image pointer until the exit). Every kernel needs at least
 * one xk_exit(). Branch displacements to labels are resolved by xk_finish(). Code is position independent; addresses of
 * kernel locations are passed in registers with xts_set_code_ptr().
 */
#ifndef XTS_KERNEL_H
#define XTS_KERNEL_H
#include <stdint.h>

/* per-word tags (k->tag[i]): an instruction id (XI_*), or one of these */
#define XK_TAG_RAW   0xFFFF   /* raw word from xk_word(): not decode-checked */
#define XK_TAG_EXIT  0xFFFE   /* exit placeholder, patched to "b post" */
#define XK_TAG_DATA  0xFFFD   /* deliberate non-instruction / illegal word (xk_data): never decode-checked */

#define XK_MAX_LABELS 256
#define XK_MAX_FIXUPS 1024

typedef struct xts_kbuf {
  uint32_t *w;            /* words */
  uint16_t *tag;          /* per word */
  uint32_t cap, n;
  int nlabels;
  int32_t label_pos[XK_MAX_LABELS];      /* word index, -1 = unbound */
  int nfix;
  struct { uint32_t at; int label; int kind; } fix[XK_MAX_FIXUPS];   /* kind 0: I-form (LI), 1: B-form (BD) */
  int nexits;
  int err;                /* first error (0 = ok) */
  char errmsg[96];
  int finished;
  uint32_t loop_iters_hint;   /* informational (dry-run report) */
} xts_kbuf_t;

xts_kbuf_t *xk_new(uint32_t cap_words);        /* NULL on allocation failure */
void xk_free(xts_kbuf_t *k);
void xk_reset(xts_kbuf_t *k);

/* emit; return the word index or -1 (error recorded in k->err / k->errmsg, later calls become no-ops) */
#define xk_emit(k, insn, flags, ...) \
  xk_emit_a((k), (insn), (flags), (const int64_t[]){ __VA_ARGS__ }, (int)(sizeof((const int64_t[]){ __VA_ARGS__ }) / sizeof(int64_t)))
int xk_emit_a(xts_kbuf_t *k, int insn, unsigned flags, const int64_t *ops, int nops);
int xk_word(xts_kbuf_t *k, uint32_t word);     /* raw word (tag RAW) */
int xk_data(xts_kbuf_t *k, uint32_t word);     /* deliberate data/illegal word (tag DATA) */
int xk_nop(xts_kbuf_t *k);                     /* ori 0,0,0 */
int xk_align(xts_kbuf_t *k, uint32_t bytes);   /* pad with nops to a multiple of bytes (relative to kernel start) */
int xk_exit(xts_kbuf_t *k);

/* labels and branches */
int xk_label(xts_kbuf_t *k);                   /* new unbound label id */
void xk_bind(xts_kbuf_t *k, int label);        /* bind to the next word */
int xk_here(xts_kbuf_t *k);                    /* new label bound here */
int xk_b(xts_kbuf_t *k, int label);            /* b label */
int xk_bl(xts_kbuf_t *k, int label);           /* bl label */
int xk_bc(xts_kbuf_t *k, int bo, int bi, int label);   /* bc BO,BI,label */
int xk_bdnz(xts_kbuf_t *k, int label);         /* bc 16,0,label */

/* CTR loop framing: mtctr count_reg; [align]; top:  ...  bdnz top */
int xk_loop_begin(xts_kbuf_t *k, int count_reg);
int xk_loop_end(xts_kbuf_t *k, int top_label);

/* Loop modes (also xts_tspec_t.loop) and a loop closure with long reach:
 *   XTS_LOOP_CTR: bdnz top                                   (CTR counts; count_reg/crf unused)
 *   XTS_LOOP_GPR: addi rC,rC,-1 ; cmpwi crF,rC,0 ; bne crF,top   (GPR rC counts down to 0; CTR is free; crF is clobbered)
 * If `top` is further back than a bc can reach (+-32 KB), the conditional branch jumps over an unconditional one:
 *   CTR: bdz out ; b top ; out:        GPR: ... ; beq crF,out ; b top ; out:
 * The loop body runs `count` times for a count >= 1 in either mode. crf 0..7 (0 = cr0). Returns the index of the last
 * word emitted, or -1 (error recorded in k). */
#define XTS_LOOP_CTR 0
#define XTS_LOOP_GPR 1
int xk_loop_close(xts_kbuf_t *k, int top_label, int mode, int count_reg, int crf);

/* thread priority prologue from xts_env.thread_prio (or 1,1,1 / or 2,2,2 / or 3,3,3), no-op when 0 */
int xk_prio_prologue(xts_kbuf_t *k);

/* resolve labels; checks exits exist and no error. 0 or <0 */
int xk_finish(xts_kbuf_t *k);

/* Dry-run validation: every tagged instruction word decodes to its tag. Returns number of bad words (0 = ok);
 * *n_insn / *n_raw count the checked / unchecked words. Used by the host dry run and host tests. */
int xk_validate(const xts_kbuf_t *k, int *n_insn, int *n_raw, char *why, int why_size);
#endif
