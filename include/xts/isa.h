/* XTS - instruction descriptors.
 *
 * The descriptor table itself is GENERATED from isa/xenon_isa.json by tools/gen_isa.py into $(BUILD)/gen/isa_table.{c,h}
 * (one entry per instruction, 484 today). isa_table.h defines XTS_ISA_COUNT and one constant per instruction:
 *
 *     XI_<mnem>      mnemonic as in the JSON "mnem" key with '.' replaced by '_':  XI_addx, XI_orx, XI_andi_, XI_vmaddfp128
 *
 * so test families can write xts_encode(XI_addx, XTS_RC, ...) and new instructions in a JSON refresh appear automatically.
 * Everything in THIS header is hand-written and stable; the field-id enum below is the single source of truth that
 * gen_isa.py reads (it fails if the JSON needs a field that is not listed here).
 */
#ifndef XTS_ISA_H
#define XTS_ISA_H
#include <stdint.h>

/* ---- operand field encodings ---------------------------------------------------------------------------------------
 * One id per distinct bit layout. A JSON operand name maps to exactly one of these per instruction form
 * (e.g. SIMM is SIMM16 in a D-form ppc instruction and SIMM5 in AltiVec; SH is SH6 in MD/XS forms; MB is MB6 in MD/MDS). */
enum xts_field_id {
  XTS_F_NONE = 0,
  XTS_F_RD, XTS_F_RS, XTS_F_RA, XTS_F_RA0, XTS_F_RB,
  XTS_F_FD, XTS_F_FS, XTS_F_FA, XTS_F_FB, XTS_F_FC,
  XTS_F_VD, XTS_F_VS, XTS_F_VA, XTS_F_VB, XTS_F_VC,
  XTS_F_VD128, XTS_F_VS128, XTS_F_VA128, XTS_F_VB128, XTS_F_VC128, XTS_F_VPERM128,
  XTS_F_SHB, XTS_F_UIMM5, XTS_F_SIMM5, XTS_F_UIMM16, XTS_F_SIMM16,
  XTS_F_VD3D0, XTS_F_VD3D1, XTS_F_VD3D2,
  XTS_F_TO, XTS_F_BO, XTS_F_BI, XTS_F_CRFD, XTS_F_CRFS, XTS_F_FPSCRD, XTS_F_L,
  XTS_F_CRBD, XTS_F_CRBA, XTS_F_CRBB,
  XTS_F_SH5, XTS_F_SH6, XTS_F_MB5, XTS_F_ME5, XTS_F_MB6, XTS_F_ME6,
  XTS_F_NB, XTS_F_CRM, XTS_F_SR, XTS_F_STRM, XTS_F_LEV, XTS_F_FM, XTS_F_IMM,
  XTS_F_D16, XTS_F_DS14, XTS_F_SPR, XTS_F_TBR, XTS_F_LI, XTS_F_BD,
  XTS_F_COUNT
};

/* how a field value is placed */
enum xts_field_kind {
  XTS_FK_UNSIGNED = 0,   /* value 0 .. 2^bits-1, scattered over the parts */
  XTS_FK_SIGNED,         /* value -2^(bits-1) .. 2^(bits-1)-1, two's complement in the parts */
  XTS_FK_SWAP10,         /* SPR/TBR: architectural number, 5-bit halves swapped in the word */
  XTS_FK_ROT6,           /* MD/MDS mb/me: word bits hold v[1:5] || v[0] */
  XTS_FK_DISP4,          /* signed byte displacement whose low 2 bits must be 0 (DS-form ds, branch LI/BD) */
};

typedef struct xts_field_part { uint8_t shift, width, dest; } xts_field_part_t;   /* shift from LSB, width, value bit */
typedef struct xts_field {
  const char *name;      /* "RD", "SIMM16", ... */
  uint8_t kind;          /* enum xts_field_kind */
  uint8_t bits;          /* value width in bits (for DISP4: total including the two zero bits) */
  uint8_t nparts;
  xts_field_part_t part[3];
} xts_field_t;
extern const xts_field_t xts_fields[XTS_F_COUNT];

/* ---- operand classes and roles -------------------------------------------------------------------------------------- */
enum xts_opclass {
  XTS_OC_GPR = 1,   /* r0-r31 */
  XTS_OC_GPR0,      /* RA0: r1-r31, or 0 meaning the literal value 0 */
  XTS_OC_FPR,
  XTS_OC_VR,        /* 5-bit AltiVec register field: v0-v31 */
  XTS_OC_VR128,     /* 7-bit VMX128 register field: v0-v127 */
  XTS_OC_VRC128,    /* VC128: v0-v7 */
  XTS_OC_CRF,       /* CR field 0-7 */
  XTS_OC_CRB,       /* CR bit 0-31 */
  XTS_OC_FPSCRB,    /* FPSCR bit 0-31 */
  XTS_OC_UIMM,      /* unsigned immediate */
  XTS_OC_SIMM,      /* signed immediate */
  XTS_OC_DISP,      /* signed memory displacement (d, D, ds) */
  XTS_OC_BRANCH,    /* branch displacement in bytes (absolute address when the AA flag is set) */
  XTS_OC_SPR,       /* SPR/TBR number */
  XTS_OC_SR,        /* segment register */
  XTS_OC_MASK,      /* CRM / FM 8-bit field masks */
};
#define XTS_ROLE_R    0x1   /* operand is read */
#define XTS_ROLE_W    0x2   /* operand is written */
#define XTS_ROLE_TIED 0x4   /* VMX128 tied operand: VS128 shares the VD128 field; encode both with the same value */

typedef struct xts_operand {
  const char *name;     /* JSON operand name: "RD", "SIMM", "VD128", ... (asm_template placeholder) */
  uint8_t field;        /* enum xts_field_id */
  uint8_t cls;          /* enum xts_opclass */
  uint8_t role;         /* XTS_ROLE_* (from the JSON reads/writes lists, which are approximate for some entries) */
  uint8_t pad;
} xts_operand_t;

/* ---- instruction descriptor ------------------------------------------------------------------------------------------ */
enum xts_isa_set { XTS_ISA_PPC = 1, XTS_ISA_ALTIVEC, XTS_ISA_VMX128, XTS_ISA_CELLVMX, XTS_ISA_XENON };
enum xts_unit { XTS_UNIT_INTEGER = 1, XTS_UNIT_MEMORY, XTS_UNIT_VECTOR, XTS_UNIT_FPU, XTS_UNIT_BRANCH, XTS_UNIT_CONTROL };
enum xts_priv { XTS_PRIV_USER = 1, XTS_PRIV_SUPERVISOR, XTS_PRIV_HYPERVISOR };
enum xts_hw {                   /* JSON hardware.status (results4 on PVR 00710500) */
  XTS_HW_UNTESTED = 0,          /* not run by the semantic plan (branches, privileged, string ops, ...) */
  XTS_HW_EXECUTES,
  XTS_HW_TRAPS,                 /* executes-with-traps: traps for some operands (tw/td, FP alignment, ...) */
  XTS_HW_ILLEGAL,               /* illegal-instruction program interrupt on every case: the expected-illegal set */
};
/* flag variants an instruction accepts (also the `flags` argument of xts_encode) */
#define XTS_RC  0x01    /* record form ('.'): bit 31, bit 21 (VC form) or bit 25 (VX128_R) */
#define XTS_OE  0x02    /* overflow enable ('o'), bit 21 */
#define XTS_LK  0x04    /* link ('l'), bit 31 */
#define XTS_AA  0x08    /* absolute address ('a'), bit 30 */
#define XTS_T   0x10    /* dst/dstst transient ('t'), bit 6 */
#define XTS_A   0x20    /* dss all ('all'), bit 6 */
/* implicit (non-operand) state an instruction reads/writes, from the JSON reads/writes lists */
#define XTS_IMP_CR    0x001
#define XTS_IMP_XER   0x002   /* SO/OV (the OE variants) */
#define XTS_IMP_CA    0x004
#define XTS_IMP_LR    0x008
#define XTS_IMP_CTR   0x010
#define XTS_IMP_FPSCR 0x020
#define XTS_IMP_VSCR  0x040
#define XTS_IMP_MEM   0x080   /* unit memory */

#define XTS_MAX_OPS 6
typedef struct xts_insn {
  const char *mnem;         /* JSON "mnem" (variant family name: "addx", "orx", "andi.") */
  const char *asm_name;     /* base assembler mnemonic without flag suffixes ("add") */
  const char *asm_template; /* JSON asm_template, e.g. "add[OE][Rc] [RD], [RA], [RB]" */
  const char *form_name;    /* "XO", "VX128_1", ... */
  uint32_t match, mask;     /* decode: (word & mask) == match */
  uint32_t extra_match;     /* bits the architecture fixes that the JSON mask leaves open (sc bit 30, stwcx./stdcx. Rc) */
  uint16_t index;           /* position in xts_isa[] == XI_<mnem> */
  uint8_t isa, unit, priv, hw;
  uint8_t flags;            /* XTS_RC | XTS_OE | ... accepted */
  uint8_t nops;
  uint16_t implicit_r, implicit_w;   /* XTS_IMP_* */
  xts_operand_t ops[XTS_MAX_OPS];    /* in asm_template order */
  float lat, issue;         /* JSON timing.latency / timing.issue in cycles, or -1 when absent */
} xts_insn_t;

extern const xts_insn_t xts_isa[];
extern const int xts_isa_count;
extern const char xts_isa_sha256[];      /* sha256 of the vendored isa/xenon_isa.json */

/* Soft timing baselines: every numeric value under an instruction's JSON "timing" object, flattened
 * ("latency", "issue", "fixed_operands.denormal", "load_use_latency", ...). */
typedef struct xts_baseline { uint16_t insn; const char *key; double value; } xts_baseline_t;
extern const xts_baseline_t xts_isa_baselines[];
extern const int xts_isa_baseline_count;

/* Encoding aliases: extended mnemonics that are a base instruction with fixed field bits the JSON table does not list
 * separately (lwsync = sync L=1, ptesync = sync L=2). They get XI_<alias> ids AFTER the table (XI_lwsync >=
 * XTS_ISA_COUNT), so loops over xts_isa[0 .. xts_isa_count) never see them. xts_enc / xts_encode / xk_emit accept them;
 * xts_insn(alias) returns the BASE descriptor (so d->nops etc. work), xts_isa_find("lwsync") returns the alias id, the
 * decoder reports the base instruction (a kernel word emitted for XI_lwsync is tagged/validated as sync), xts_disasm
 * prints the alias name. Never index xts_isa[] with an alias id; use xts_insn() / xts_insn_canonical(). */
typedef struct xts_insn_alias {
  const char *name;         /* "lwsync" */
  uint16_t base;            /* XI_ id of the base instruction */
  uint32_t set_bits;        /* bits ORed into the base encoding */
  uint32_t fix_mask;        /* bits the alias fixes (decode: (word & fix_mask) == set_bits) */
} xts_insn_alias_t;
extern const xts_insn_alias_t xts_isa_aliases[];
extern const int xts_isa_alias_count;
int xts_insn_canonical(int id);                     /* alias -> base id; table ids unchanged; -1 if invalid */
const char *xts_insn_name(int id);                  /* mnemonic (alias name for aliases), "?" if invalid */

/* lookups (src/enc/isa.c) */
int xts_isa_find(const char *mnem);                 /* JSON mnem ("addx"), asm name ("add") or alias ("lwsync"); -1 if unknown */
const xts_insn_t *xts_insn(int id);                 /* NULL if out of range; alias ids -> the base descriptor */
int xts_op_index(const xts_insn_t *d, const char *opname);   /* operand position by JSON name, -1 if absent */
int xts_isa_baseline(int insn, const char *key, double *value);   /* 1 if found */
const char *xts_isa_set_name(int isa);              /* "ppc", "altivec", "vmx128", "cell-vmx", "xenon" */
const char *xts_unit_name(int unit);
const char *xts_hw_name(int hw);                    /* "untested", "executes", "executes-with-traps", "illegal" */

#include "isa_table.h"   /* generated: XTS_ISA_COUNT, XI_* */
#endif
