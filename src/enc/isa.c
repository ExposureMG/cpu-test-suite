/* Lookups over the generated descriptor table. */
#include <string.h>
#include <xts/isa.h>

int xts_insn_canonical(int id)
{
  if (id >= 0 && id < xts_isa_count) return id;
  if (id >= xts_isa_count && id < xts_isa_count + xts_isa_alias_count) return xts_isa_aliases[id - xts_isa_count].base;
  return -1;
}

const xts_insn_t *xts_insn(int id)
{
  id = xts_insn_canonical(id);
  return id >= 0 ? &xts_isa[id] : NULL;
}

const char *xts_insn_name(int id)
{
  if (id >= 0 && id < xts_isa_count) return xts_isa[id].mnem;
  if (id >= xts_isa_count && id < xts_isa_count + xts_isa_alias_count) return xts_isa_aliases[id - xts_isa_count].name;
  return "?";
}

int xts_isa_find(const char *mnem)
{
  if (!mnem) return -1;
  for (int i = 0; i < xts_isa_count; i++)
    if (!strcmp(xts_isa[i].mnem, mnem)) return i;
  for (int i = 0; i < xts_isa_count; i++)
    if (!strcmp(xts_isa[i].asm_name, mnem)) return i;
  for (int i = 0; i < xts_isa_alias_count; i++)
    if (!strcmp(xts_isa_aliases[i].name, mnem)) return xts_isa_count + i;
  return -1;
}

int xts_op_index(const xts_insn_t *d, const char *opname)
{
  for (int i = 0; d && i < d->nops; i++)
    if (!strcmp(d->ops[i].name, opname)) return i;
  return -1;
}

int xts_isa_baseline(int insn, const char *key, double *value)
{
  for (int i = 0; i < xts_isa_baseline_count; i++)
    if (xts_isa_baselines[i].insn == insn && !strcmp(xts_isa_baselines[i].key, key)) {
      if (value) *value = xts_isa_baselines[i].value;
      return 1;
    }
  return 0;
}

const char *xts_isa_set_name(int isa)
{
  switch (isa) {
  case XTS_ISA_PPC: return "ppc";
  case XTS_ISA_ALTIVEC: return "altivec";
  case XTS_ISA_VMX128: return "vmx128";
  case XTS_ISA_CELLVMX: return "cell-vmx";
  case XTS_ISA_XENON: return "xenon";
  }
  return "?";
}

const char *xts_unit_name(int unit)
{
  static const char *const n[] = { "?", "integer", "memory", "vector", "fpu", "branch", "control" };
  return (unit >= 0 && unit <= XTS_UNIT_CONTROL) ? n[unit] : "?";
}

const char *xts_hw_name(int hw)
{
  static const char *const n[] = { "untested", "executes", "executes-with-traps", "illegal" };
  return (hw >= 0 && hw <= XTS_HW_ILLEGAL) ? n[hw] : "?";
}
