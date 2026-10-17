/* XTS - reference model interface (src/model). Pure C, no host/console dependencies, independent FP through SoftFloat.
 *
 *   xts_mres_t xts_model_exec(const xts_insn_t *d, uint32_t word, const xts_state_t *in, xts_mem_t *mem, xts_state_t *out);
 *
 * The dispatcher (src/model/model.c) decodes the word if d is NULL, normalises *in to what the CPU really holds after
 * the slot loaded it (xm_state_loaded: FPSCR[NI] cannot be set on Xenon, FEX/VX are summaries, VSCR keeps NJ/SAT),
 * copies that to *out (handlers receive the normalised input), returns XTS_M_ILLEGAL for the
 * expected-illegal set (hardware.status == illegal: the 22 dropped VMX ops + lvsl128 lvsr128 mcrxr popcntb dcba fre
 * frsqrtes) and for words no descriptor matches, and otherwise calls the handler registered for the mnemonic, or returns
 * XTS_M_UNIMPLEMENTED. A handler only writes what the instruction changes into *out (it starts as a copy of *in).
 *
 * Adding handlers = adding src/model/<family>.c:
 *
 *   static xts_mres_t m_subfx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out) {
 *     int rd = XM_RD(w), ra = XM_RA(w), rb = XM_RB(w);
 *     uint64_t r = in->gpr[rb] - in->gpr[ra];
 *     out->gpr[rd] = r;
 *     if (w & 0x400) xm_xer_ov(out, xm_ov_sub(...), xm_sf(in));     // OE
 *     if (w & 1) xm_cr0(out, r, xm_sf(in));                           // Rc
 *     return XTS_M_OK;
 *   }
 *   XTS_MODEL("subfx", m_subfx);
 *
 * Memory: case addresses are resolved through xts_mem_t regions; an access outside every region is XTS_M_DSI, an access
 * the console rejects for alignment is XTS_M_ALIGNMENT (rules per family, e.g. FP needs EA%4==0 on hardware).
 * Traps (tw/td family) return XTS_M_TRAP and leave *out == *in, like the hardware stub (which resumes at SRR0+4).
 */
#ifndef XTS_MODEL_H
#define XTS_MODEL_H
#include <stdint.h>
#include <xts/isa.h>
#include <xts/state.h>

typedef enum xts_mres {
  XTS_M_OK = 0,
  XTS_M_ILLEGAL,          /* program interrupt, illegal instruction (cause 0x08) */
  XTS_M_TRAP,             /* program interrupt, trap (cause 0x02) */
  XTS_M_ALIGNMENT,        /* alignment interrupt (vector 0x600) */
  XTS_M_DSI,              /* data storage: address outside the mapped regions */
  XTS_M_PRIVILEGED,       /* privileged instruction in problem state (not expected in this suite: tests run in HV) */
  XTS_M_UNIMPLEMENTED,    /* no handler (yet) */
} xts_mres_t;
/* Never returned by xts_model_exec: xts_semantic_check's verdict class for an estimate instruction the model could not
 * answer exactly (XTS_M_UNIMPLEMENTED); the CPU result is checked against the architected bound instead
 * (xts_estimate_verdict, xts/exec.h). A macro, not an enumerator, so existing switches over xts_mres_t stay complete. */
#define XTS_M_ESTIMATE ((xts_mres_t)(XTS_M_UNIMPLEMENTED + 1))
const char *xts_mres_name(xts_mres_t r);

#define XTS_MEM_REGIONS 4
typedef struct xts_mem {
  struct { uint64_t base; uint32_t size; uint8_t *data; } r[XTS_MEM_REGIONS];   /* data is big-endian memory */
  int n;
  uint64_t fault_ea;      /* set on DSI/ALIGNMENT */
} xts_mem_t;
void xts_mem_init(xts_mem_t *m);
int xts_mem_add(xts_mem_t *m, uint64_t base, uint8_t *data, uint32_t size);
/* pointer to `n` bytes at EA (all inside one region) or NULL */
uint8_t *xts_mem_ptr(xts_mem_t *m, uint64_t ea, uint32_t n);
/* big-endian loads/stores; return 0 or XTS_M_DSI */
int xm_load(xts_mem_t *m, uint64_t ea, int n, uint64_t *v);
int xm_store(xts_mem_t *m, uint64_t ea, int n, uint64_t v);

typedef xts_mres_t (*xts_model_fn)(const xts_insn_t *d, uint32_t word, const xts_state_t *in, xts_mem_t *mem,
                                   xts_state_t *out);
typedef struct xts_model_entry { const char *mnem; xts_model_fn fn; const char *file; } xts_model_entry_t;
/* Registration: one pointer per handler in linker section "xts_model". Two handlers for one mnemonic is an error
 * (xts_model_check() reports it; host-test fails). */
#define XTS_MODEL(mnem_str, fn_)                                                                                 \
  static const xts_model_entry_t XTS_MODEL_CAT(xts_model_e_, __LINE__) = { mnem_str, fn_, __FILE__ };            \
  static const xts_model_entry_t *const XTS_MODEL_CAT(xts_model_p_, __LINE__)                                    \
      __attribute__((used, section("xts_model"), aligned(sizeof(void *)))) = &XTS_MODEL_CAT(xts_model_e_, __LINE__)
#define XTS_MODEL_CAT2(a, b) a##b
#define XTS_MODEL_CAT(a, b) XTS_MODEL_CAT2(a, b)

xts_mres_t xts_model_exec(const xts_insn_t *d, uint32_t word, const xts_state_t *in, xts_mem_t *mem, xts_state_t *out);
int xts_model_has(int insn);            /* 1 if a handler is registered (or the insn is expected-illegal) */
int xts_model_expected_illegal(int insn);
/* Consistency check of the registrations: unknown mnemonics, duplicates. Returns number of problems, prints them. */
int xts_model_check(void (*print)(const char *line));
int xts_model_handler_count(void);

/* ---- field helpers (fixed positions; use xts_extract(insn, word, opidx) for the generic way) ---- */
#define XM_BITS(w, sh, n) (((uint32_t)(w) >> (sh)) & ((1u << (n)) - 1))
#define XM_RD(w)  XM_BITS(w, 21, 5)      /* also RS, FD, FS, VD, TO, BO, CRBD */
#define XM_RS(w)  XM_BITS(w, 21, 5)
#define XM_RA(w)  XM_BITS(w, 16, 5)
#define XM_RB(w)  XM_BITS(w, 11, 5)
#define XM_RC_BIT(w) ((w) & 1u)          /* Rc for X/XO/A/M/MD forms */
#define XM_OE_BIT(w) (((w) >> 10) & 1u)  /* OE for XO forms */
#define XM_SIMM16(w) ((int64_t)(int16_t)((w) & 0xFFFF))
#define XM_UIMM16(w) ((uint64_t)((w) & 0xFFFF))

static inline int xm_sf(const xts_state_t *s) { return (int)(s->mode & XTS_MODE_SF); }
static inline uint64_t xm_exts32(uint64_t v) { return (uint64_t)(int64_t)(int32_t)(uint32_t)v; }

/* CR helpers: field 0 = CR bits 0-3 = the most significant nibble */
static inline uint32_t xm_cr_field(const xts_state_t *s, int f) { return ((uint32_t)s->cr >> (28 - 4 * f)) & 0xF; }
static inline void xm_set_cr_field(xts_state_t *s, int f, uint32_t v)
{
  uint32_t sh = 28 - 4 * f;
  s->cr = ((uint32_t)s->cr & ~(0xFu << sh)) | ((v & 0xF) << sh);
}
/* CR0 from a result (Rc=1): LT/GT/EQ from the signed 64-bit result (SF=1) or the low 32 bits sign-extended (SF=0),
 * SO copied from XER[SO] of *s (call it after updating XER). */
void xm_cr0(xts_state_t *s, uint64_t result, int sf);
/* XER helpers */
static inline int xm_xer_so(const xts_state_t *s) { return (s->xer & XTS_XER_SO) != 0; }
static inline int xm_xer_ca(const xts_state_t *s) { return (s->xer & XTS_XER_CA) != 0; }
void xm_set_ov(xts_state_t *s, int ov);    /* OV := ov, SO |= ov */
void xm_set_ca(xts_state_t *s, int ca);
/* overflow / carry of a+b(+cin) in the current mode (SF=1: 64-bit, SF=0: low 32 bits) */
int xm_add_ov(uint64_t a, uint64_t b, uint64_t r, int sf);
int xm_add_ca(uint64_t a, uint64_t b, int cin, int sf);

/* ---- FP glue (SoftFloat). FPSCR bit masks (low 32 bits of the FPSCR image, bit 0 = MSB in IBM numbering) ---- */
#define XTS_FPSCR_FX     0x80000000u
#define XTS_FPSCR_FEX    0x40000000u
#define XTS_FPSCR_VX     0x20000000u
#define XTS_FPSCR_OX     0x10000000u
#define XTS_FPSCR_UX     0x08000000u
#define XTS_FPSCR_ZX     0x04000000u
#define XTS_FPSCR_XX     0x02000000u
#define XTS_FPSCR_VXSNAN 0x01000000u
#define XTS_FPSCR_VXISI  0x00800000u
#define XTS_FPSCR_VXIDI  0x00400000u
#define XTS_FPSCR_VXZDZ  0x00200000u
#define XTS_FPSCR_VXIMZ  0x00100000u
#define XTS_FPSCR_VXVC   0x00080000u
#define XTS_FPSCR_FR     0x00040000u
#define XTS_FPSCR_FI     0x00020000u
#define XTS_FPSCR_FPRF   0x0001F000u
#define XTS_FPSCR_VXSOFT 0x00000400u
#define XTS_FPSCR_VXSQRT 0x00000200u
#define XTS_FPSCR_VXCVI  0x00000100u
#define XTS_FPSCR_VE     0x00000080u
#define XTS_FPSCR_OE     0x00000040u
#define XTS_FPSCR_UE     0x00000020u
#define XTS_FPSCR_ZE     0x00000010u
#define XTS_FPSCR_XE     0x00000008u
#define XTS_FPSCR_NI     0x00000004u
#define XTS_FPSCR_RN     0x00000003u
#define XTS_FPSCR_VX_ALL (XTS_FPSCR_VXSNAN | XTS_FPSCR_VXISI | XTS_FPSCR_VXIDI | XTS_FPSCR_VXZDZ | XTS_FPSCR_VXIMZ | \
                          XTS_FPSCR_VXVC | XTS_FPSCR_VXSOFT | XTS_FPSCR_VXSQRT | XTS_FPSCR_VXCVI)

/* Set SoftFloat's rounding mode from FPSCR[RN] and clear its exception flags. */
void xm_sf_begin(uint32_t fpscr);
/* SoftFloat flags raised since xm_sf_begin, mapped to FPSCR exception bits (OX UX ZX XX; invalid -> VX only:
 * the caller ORs the right VX subtype since SoftFloat does not know it). */
uint32_t xm_sf_flags_to_fpscr(void);
/* Merge newly raised exception bits into an FPSCR the PowerPC way: sticky bits, FX if any bit went 0->1, VX summary of
 * the VX* bits, FEX from enabled exceptions. Returns the new FPSCR. FR/FI/FPRF are the caller's (instruction-specific). */
uint32_t xm_fpscr_merge(uint32_t fpscr, uint32_t new_exc);
/* FPRF class codes for a double result (C,<,>,=,? bits as placed in FPSCR[FPRF]) */
uint32_t xm_fprf_f64(uint64_t bits);

/* The state the CPU actually holds after the slot loaded an input image (mtfsf 0xFF: FEX/VX recomputed; Xenon cannot
 * set FPSCR[NI]; mtvscr keeps only NJ/SAT). xts_model_exec applies this to its input before anything else. */
uint32_t xm_fpscr_loaded(uint32_t fpscr);
void xm_state_loaded(xts_state_t *s);

/* default PowerPC NaNs */
#define XM_DNAN64 0x7FF8000000000000ull
#define XM_DNAN32 0x7FC00000u
#endif
