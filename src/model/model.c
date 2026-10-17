/* Reference model framework: dispatcher, expected-illegal set, registration check, shared helpers, SoftFloat glue.
 * Instruction semantics live in the per-family files of this directory (int.c, rot.c, fp.c, ...), registered with
 * XTS_MODEL("mnem", fn). Phase A ships only addx/orx (int.c) as examples. */
#include <stdio.h>
#include <string.h>
#include <xts/enc.h>
#include <xts/model.h>
#include "softfloat.h"

extern const xts_model_entry_t *const __start_xts_model[] __attribute__((weak));
extern const xts_model_entry_t *const __stop_xts_model[] __attribute__((weak));

static xts_model_fn handler[XTS_ISA_COUNT];
static int table_built, nhandlers, table_problems;
static char problems[8][160];

static void build_table(void)
{
  if (table_built) return;
  table_built = 1;
  if (!__start_xts_model || !__stop_xts_model) return;
  for (const xts_model_entry_t *const *p = __start_xts_model; p < __stop_xts_model; p++) {
    const xts_model_entry_t *e = *p;
    if (!e) continue;
    int id = -1;
    for (int i = 0; i < xts_isa_count; i++)
      if (!strcmp(xts_isa[i].mnem, e->mnem)) { id = i; break; }
    if (id < 0) {
      if (table_problems < 8)
        snprintf(problems[table_problems], sizeof problems[0], "model: %s registers unknown mnemonic '%s'", e->file, e->mnem);
      table_problems++;
      continue;
    }
    if (handler[id]) {
      if (table_problems < 8)
        snprintf(problems[table_problems], sizeof problems[0], "model: duplicate handler for '%s' (%s)", e->mnem, e->file);
      table_problems++;
      continue;
    }
    handler[id] = e->fn;
    nhandlers++;
  }
}

int xts_model_check(void (*print)(const char *line))
{
  build_table();
  for (int i = 0; i < table_problems && i < 8; i++)
    if (print) print(problems[i]);
  return table_problems;
}

int xts_model_handler_count(void) { build_table(); return nhandlers; }

int xts_model_expected_illegal(int insn)
{
  return insn >= 0 && insn < xts_isa_count && xts_isa[insn].hw == XTS_HW_ILLEGAL;
}

int xts_model_has(int insn)
{
  build_table();
  return insn >= 0 && insn < xts_isa_count && (handler[insn] || xts_model_expected_illegal(insn));
}

const char *xts_mres_name(xts_mres_t r)
{
  switch (r) {
  case XTS_M_OK: return "ok";
  case XTS_M_ILLEGAL: return "illegal";
  case XTS_M_TRAP: return "trap";
  case XTS_M_ALIGNMENT: return "alignment";
  case XTS_M_DSI: return "dsi";
  case XTS_M_PRIVILEGED: return "privileged";
  case XTS_M_UNIMPLEMENTED: return "unimplemented";
  }
  return "?";
}

xts_mres_t xts_model_exec(const xts_insn_t *d, uint32_t word, const xts_state_t *in, xts_mem_t *mem, xts_state_t *out)
{
  build_table();
  /* The input as the CPU holds it after the slot loaded it (see xm_state_loaded): handlers and the interrupt paths
   * start from that, exactly like the hardware. */
  xts_state_t eff = *in;
  xm_state_loaded(&eff);
  in = &eff;
  *out = eff;
  if (!d) {
    int id = xts_decode1(word);
    if (id < 0) return XTS_M_ILLEGAL;            /* the sweeps found no unknown word that executes */
    d = &xts_isa[id];
  } else if (!xts_decodes_as(word, d->index)) {
    return XTS_M_ILLEGAL;
  }
  if (d->hw == XTS_HW_ILLEGAL) return XTS_M_ILLEGAL;
  if (!handler[d->index]) return XTS_M_UNIMPLEMENTED;
  xts_mres_t r = handler[d->index](d, word, in, mem, out);
  if (r != XTS_M_OK) *out = eff;                  /* interrupts leave the architected state unchanged */
  return r;
}

uint32_t xm_fpscr_loaded(uint32_t v)
{
  /* mtfsf 0xFF: FEX and VX are not written, they summarise other bits */
  uint32_t f = v & ~(XTS_FPSCR_FEX | XTS_FPSCR_VX);
  f = xm_fpscr_merge(f & ~XTS_FPSCR_FX, 0) | (v & XTS_FPSCR_FX);
  /* Xenon: FPSCR[NI] cannot be set - every results4 case loaded with NI=1 (196 of them, incl. trapping ones) read
   * back NI=0 */
  return f & ~XTS_FPSCR_NI;
}

void xm_state_loaded(xts_state_t *s)
{
  s->fpscr = xm_fpscr_loaded((uint32_t)s->fpscr);
  s->vscr.w[3] &= XTS_VSCR_NJ | XTS_VSCR_SAT;       /* mtvscr: only NJ and SAT exist */
  s->vscr.w[0] = s->vscr.w[1] = s->vscr.w[2] = 0;   /* mfvscr returns the other words as zero */
}

/* ---- memory ---- */
void xts_mem_init(xts_mem_t *m) { memset(m, 0, sizeof *m); }

int xts_mem_add(xts_mem_t *m, uint64_t base, uint8_t *data, uint32_t size)
{
  if (m->n >= XTS_MEM_REGIONS) return -1;
  m->r[m->n].base = base;
  m->r[m->n].size = size;
  m->r[m->n].data = data;
  m->n++;
  return 0;
}

uint8_t *xts_mem_ptr(xts_mem_t *m, uint64_t ea, uint32_t n)
{
  for (int i = 0; m && i < m->n; i++)
    if (ea >= m->r[i].base && ea + n <= m->r[i].base + m->r[i].size && ea + n >= ea)
      return m->r[i].data + (ea - m->r[i].base);
  if (m) m->fault_ea = ea;
  return NULL;
}

int xm_load(xts_mem_t *m, uint64_t ea, int n, uint64_t *v)
{
  uint8_t *p = xts_mem_ptr(m, ea, (uint32_t)n);
  if (!p) return XTS_M_DSI;
  *v = xts_be_load(p, n);
  return 0;
}

int xm_store(xts_mem_t *m, uint64_t ea, int n, uint64_t v)
{
  uint8_t *p = xts_mem_ptr(m, ea, (uint32_t)n);
  if (!p) return XTS_M_DSI;
  xts_be_store(p, n, v);
  return 0;
}

/* ---- integer helpers ---- */
void xm_cr0(xts_state_t *s, uint64_t result, int sf)
{
  int64_t v = sf ? (int64_t)result : (int64_t)(int32_t)(uint32_t)result;
  uint32_t f = (v < 0 ? 8u : v > 0 ? 4u : 2u) | (xm_xer_so(s) ? 1u : 0u);
  xm_set_cr_field(s, 0, f);
}

void xm_set_ov(xts_state_t *s, int ov)
{
  if (ov) s->xer |= XTS_XER_OV | XTS_XER_SO;
  else s->xer &= ~(uint64_t)XTS_XER_OV;
}

void xm_set_ca(xts_state_t *s, int ca)
{
  if (ca) s->xer |= XTS_XER_CA;
  else s->xer &= ~(uint64_t)XTS_XER_CA;
}

int xm_add_ov(uint64_t a, uint64_t b, uint64_t r, int sf)
{
  uint64_t x = (a ^ r) & (b ^ r);
  return sf ? (int)(x >> 63) : (int)((x >> 31) & 1);
}

int xm_add_ca(uint64_t a, uint64_t b, int cin, int sf)
{
  if (!sf) return (int)((((a & 0xFFFFFFFFull) + (b & 0xFFFFFFFFull) + (uint64_t)(cin & 1)) >> 32) & 1);
  uint64_t s = a + b;
  int c = s < a;
  uint64_t s2 = s + (uint64_t)(cin & 1);
  return c | (s2 < s);
}

/* ---- SoftFloat glue ---- */
void xm_sf_begin(uint32_t fpscr)
{
  static const uint_fast8_t rm[4] = { softfloat_round_near_even, softfloat_round_minMag, softfloat_round_max,
                                      softfloat_round_min };
  softfloat_roundingMode = rm[fpscr & 3];
  softfloat_exceptionFlags = 0;
}

uint32_t xm_sf_flags_to_fpscr(void)
{
  uint32_t f = 0, e = softfloat_exceptionFlags;
  if (e & softfloat_flag_overflow) f |= XTS_FPSCR_OX;
  if (e & softfloat_flag_underflow) f |= XTS_FPSCR_UX;
  if (e & softfloat_flag_infinite) f |= XTS_FPSCR_ZX;
  if (e & softfloat_flag_inexact) f |= XTS_FPSCR_XX;
  if (e & softfloat_flag_invalid) f |= XTS_FPSCR_VX;
  return f;
}

uint32_t xm_fpscr_merge(uint32_t fpscr, uint32_t new_exc)
{
  const uint32_t exc = XTS_FPSCR_OX | XTS_FPSCR_UX | XTS_FPSCR_ZX | XTS_FPSCR_XX | XTS_FPSCR_VX_ALL;
  uint32_t n = new_exc & exc;
  uint32_t f = fpscr | n;
  if (n & ~fpscr) f |= XTS_FPSCR_FX;
  if (f & XTS_FPSCR_VX_ALL) f |= XTS_FPSCR_VX;
  else f &= ~XTS_FPSCR_VX;
  int fex = ((f & XTS_FPSCR_VX) && (f & XTS_FPSCR_VE)) || ((f & XTS_FPSCR_OX) && (f & XTS_FPSCR_OE)) ||
            ((f & XTS_FPSCR_UX) && (f & XTS_FPSCR_UE)) || ((f & XTS_FPSCR_ZX) && (f & XTS_FPSCR_ZE)) ||
            ((f & XTS_FPSCR_XX) && (f & XTS_FPSCR_XE));
  if (fex) f |= XTS_FPSCR_FEX;
  else f &= ~XTS_FPSCR_FEX;
  return f;
}

uint32_t xm_fprf_f64(uint64_t b)
{
  int neg = (int)(b >> 63);
  uint64_t e = (b >> 52) & 0x7FF, m = b & 0xFFFFFFFFFFFFFull;
  uint32_t c;
  if (e == 0x7FF) c = m ? 0x11 : (neg ? 0x09 : 0x05);
  else if (e == 0) c = m ? (neg ? 0x18 : 0x14) : (neg ? 0x12 : 0x02);
  else c = neg ? 0x08 : 0x04;
  return c << 12;
}
