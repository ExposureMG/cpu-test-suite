#!/usr/bin/env python3
"""Generate the C instruction table from isa/xenon_isa.json.

    gen_isa.py --json isa/xenon_isa.json --header include/xts/isa.h --out $(BUILD)/gen

Writes isa_table.h (XTS_ISA_COUNT, XI_<mnem> ids) and isa_table.c (field layouts, one xts_insn_t per instruction,
soft timing baselines). Field ids come from the enum in include/xts/isa.h (single source of truth); this script fails if
the JSON needs an operand layout that enum does not list, or if the expected-illegal set is not what the hardware
findings say (22 dropped legacy VMX ops + lvsl128 lvsr128 mcrxr popcntb dcba fre frsqrtes).

Field placement rules are the ones isa/hwtest/isaenc.py (OpenXenos) verified against GNU as; tools/check_encoder.py
re-verifies the C encoder against xenon-as for every instruction.
"""
import argparse
import hashlib
import json
import os
import re
import sys


def ibm(a, b):
  """IBM bits a..b (bit 0 = MSB) -> (shift_from_lsb, width)."""
  return (31 - b, b - a + 1)


# Layouts of fields the JSON 'fields' table does not define (IBM bit numbers, as in isaenc.py SCALAR_FIELDS).
# name -> (kind, value bits, [(shift, width, dest), ...])
U, S, SW, ROT, D4 = 'XTS_FK_UNSIGNED', 'XTS_FK_SIGNED', 'XTS_FK_SWAP10', 'XTS_FK_ROT6', 'XTS_FK_DISP4'
SCALAR = {
    'TO': (U, 5, [ibm(6, 10) + (0,)]),
    'BO': (U, 5, [ibm(6, 10) + (0,)]),
    'BI': (U, 5, [ibm(11, 15) + (0,)]),
    'CRFD': (U, 3, [ibm(6, 8) + (0,)]),
    'CRFS': (U, 3, [ibm(11, 13) + (0,)]),
    'FPSCRD': (U, 5, [ibm(6, 10) + (0,)]),
    'L': (U, 1, [ibm(10, 10) + (0,)]),
    'CRBD': (U, 5, [ibm(6, 10) + (0,)]),
    'CRBA': (U, 5, [ibm(11, 15) + (0,)]),
    'CRBB': (U, 5, [ibm(16, 20) + (0,)]),
    'SH5': (U, 5, [ibm(16, 20) + (0,)]),
    'SH6': (U, 6, [(11, 5, 0), (1, 1, 5)]),            # sh[0:4] at IBM 16-20, sh5 at IBM 30
    'MB5': (U, 5, [ibm(21, 25) + (0,)]),
    'ME5': (U, 5, [ibm(26, 30) + (0,)]),
    'MB6': (ROT, 6, [(5, 6, 0)]),                       # IBM 21-26 hold mb[1:5] || mb[0]
    'ME6': (ROT, 6, [(5, 6, 0)]),
    'FD': (U, 5, [ibm(6, 10) + (0,)]),
    'FS': (U, 5, [ibm(6, 10) + (0,)]),
    'FA': (U, 5, [ibm(11, 15) + (0,)]),
    'FB': (U, 5, [ibm(16, 20) + (0,)]),
    'FC': (U, 5, [ibm(21, 25) + (0,)]),
    'VS': (U, 5, [ibm(6, 10) + (0,)]),
    'NB': (U, 5, [ibm(16, 20) + (0,)]),
    'CRM': (U, 8, [ibm(12, 19) + (0,)]),
    'SR': (U, 4, [ibm(12, 15) + (0,)]),
    'STRM': (U, 2, [ibm(9, 10) + (0,)]),
    'LEV': (U, 7, [ibm(20, 26) + (0,)]),
    'FM': (U, 8, [ibm(7, 14) + (0,)]),
    'IMM': (U, 4, [ibm(16, 19) + (0,)]),
    'D16': (S, 16, [ibm(16, 31) + (0,)]),
    'DS14': (D4, 16, [(2, 14, 2)]),                     # DS-form: byte displacement, low two bits are opcode
    'UIMM16': (U, 16, [(0, 16, 0)]),
    'SIMM16': (S, 16, [(0, 16, 0)]),
    'SPR': (SW, 10, [(11, 10, 0)]),
    'TBR': (SW, 10, [(11, 10, 0)]),
    'LI': (D4, 26, [(2, 24, 2)]),                       # I-form branch: signed byte displacement
    'BD': (D4, 16, [(2, 14, 2)]),                       # B-form branch
}
# JSON field name -> field id for the ones taken from d['fields'] (RA0 shares RA's layout)
FROM_JSON = {'RD': 'RD', 'RS': 'RS', 'RA': 'RA', 'RA0': 'RA', 'RB': 'RB', 'VD': 'VD', 'VA': 'VA', 'VB': 'VB', 'VC': 'VC',
             'VD128': 'VD128', 'VA128': 'VA128', 'VB128': 'VB128', 'VC128': 'VC128', 'VPERM128': 'VPERM128',
             'SHB': 'SHB', 'UIMM5': 'UIMM', 'SIMM5': 'SIMM', 'VS128': 'VS128', 'VD3D0': 'VD3D0', 'VD3D1': 'VD3D1',
             'VD3D2': 'VD3D2'}

VECTOR_ISAS = ('altivec', 'vmx128', 'cell-vmx')
ISA_ENUM = {'ppc': 'XTS_ISA_PPC', 'altivec': 'XTS_ISA_ALTIVEC', 'vmx128': 'XTS_ISA_VMX128', 'cell-vmx': 'XTS_ISA_CELLVMX',
            'xenon': 'XTS_ISA_XENON'}
UNIT_ENUM = {'integer': 'XTS_UNIT_INTEGER', 'memory': 'XTS_UNIT_MEMORY', 'vector': 'XTS_UNIT_VECTOR', 'fpu': 'XTS_UNIT_FPU',
             'branch': 'XTS_UNIT_BRANCH', 'control': 'XTS_UNIT_CONTROL'}
PRIV_ENUM = {'user': 'XTS_PRIV_USER', 'supervisor': 'XTS_PRIV_SUPERVISOR', 'hypervisor': 'XTS_PRIV_HYPERVISOR'}
HW_ENUM = {None: 'XTS_HW_UNTESTED', 'executes': 'XTS_HW_EXECUTES', 'executes-with-traps': 'XTS_HW_TRAPS',
           'illegal': 'XTS_HW_ILLEGAL'}
FLAG_ENUM = {'rc': 'XTS_RC', 'oe': 'XTS_OE', 'lk': 'XTS_LK', 'aa': 'XTS_AA', 't': 'XTS_T', 'a': 'XTS_A'}
# Table imprecision found by the GNU cross-check (isaenc.py EXTRA_MATCH): bits the architecture fixes that the JSON match
# leaves clear. sc: PPC 2.02 pins instruction bit 30 = 1. stwcx./stdcx.: Rc is always 1.
EXTRA_MATCH = {'sc': 0x2, 'stwcx': 0x1, 'stdcx': 0x1}
EXPECTED_ILLEGAL_EXTRA = {'lvsl128', 'lvsr128', 'mcrxr', 'popcntb', 'dcba', 'fre', 'frsqrtes'}
# Encoding aliases (extended mnemonics the JSON does not list): name -> (base mnem, bits ORed in, bits fixed).
# They get XI_ ids after the table (xts/isa.h); tools/check_encoder.py cross-checks them against xenon-as.
ALIASES = [
    ('lwsync', 'sync', 1 << 21, 3 << 21),     # sync L=1 (IBM bits 9-10)
    ('ptesync', 'sync', 2 << 21, 3 << 21),    # sync L=2
]
# Operand classes the JSON gets wrong: (mnem, operand) -> class. mffs writes an FPR (its JSON operand is named RD, the
# field layout is the same as FD; "writes" already says FD).
OPCLASS_OVERRIDE = {('mffsx', 'RD'): 'XTS_OC_FPR'}

GPR = {'RD', 'RS', 'RA', 'RB'}
FPR = {'FD', 'FS', 'FA', 'FB', 'FC'}
VR5 = {'VD', 'VS', 'VA', 'VB', 'VC'}
VR7 = {'VD128', 'VA128', 'VB128', 'VS128'}
DEST_DEFAULT = {'RD', 'FD', 'VD', 'VD128', 'CRFD', 'CRBD', 'FPSCRD'}


def field_for(r, op):
  """JSON operand name of instruction r -> field id name (key into the layout tables)."""
  form, scalar = r['form'], r['isa'] not in VECTOR_ISAS
  if op == 'SIMM':
    return 'SIMM16' if scalar else 'SIMM5'
  if op == 'UIMM':
    return 'UIMM16' if scalar else 'UIMM5'
  if op == 'SH':
    return 'SH6' if form in ('MD', 'XS') else 'SH5'
  if op == 'MB':
    return 'MB6' if form in ('MD', 'MDS') else 'MB5'
  if op == 'ME':
    return 'ME6' if form in ('MD', 'MDS') else 'ME5'
  if op in ('d', 'D'):
    return 'D16'
  if op == 'ds':
    return 'DS14'
  if op == 'ADDR':
    if form == 'I':
      return 'LI'
    if form == 'B':
      return 'BD'
    raise SystemExit('ADDR operand in form %s (%s)' % (form, r['mnem']))
  return op


def opclass(r, op, fid):
  if (r['mnem'], op) in OPCLASS_OVERRIDE:
    return OPCLASS_OVERRIDE[(r['mnem'], op)]
  if op == 'RA0':
    return 'XTS_OC_GPR0'
  if op in GPR:
    return 'XTS_OC_GPR'
  if op in FPR:
    return 'XTS_OC_FPR'
  if op in VR5:
    return 'XTS_OC_VR'
  if op in VR7:
    return 'XTS_OC_VR128'
  if op == 'VC128':
    return 'XTS_OC_VRC128'
  if op in ('CRFD', 'CRFS'):
    return 'XTS_OC_CRF'
  if op in ('CRBD', 'CRBA', 'CRBB', 'BI'):
    return 'XTS_OC_CRB'
  if op == 'FPSCRD':
    return 'XTS_OC_FPSCRB'
  if op in ('d', 'D', 'ds'):
    return 'XTS_OC_DISP'
  if op == 'ADDR':
    return 'XTS_OC_BRANCH'
  if op in ('SPR', 'TBR'):
    return 'XTS_OC_SPR'
  if op == 'SR':
    return 'XTS_OC_SR'
  if op in ('CRM', 'FM'):
    return 'XTS_OC_MASK'
  if fid in ('SIMM16', 'SIMM5'):
    return 'XTS_OC_SIMM'
  return 'XTS_OC_UIMM'


def c_ident(m):
  return 'XI_' + re.sub(r'[^A-Za-z0-9_]', '_', m)


def asm_name(t):
  return re.split(r'[\s\[]', t, maxsplit=1)[0]


def cstr(s):
  return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def flatten(prefix, v, out):
  if isinstance(v, bool):
    return
  if isinstance(v, (int, float)):
    out.append((prefix, float(v)))
  elif isinstance(v, dict):
    for k in sorted(v):
      flatten(prefix + '.' + k if prefix else k, v[k], out)


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument('--json', required=True)
  ap.add_argument('--header', required=True, help='include/xts/isa.h (field id enum)')
  ap.add_argument('--out', required=True)
  a = ap.parse_args()
  raw = open(a.json, 'rb').read()
  sha = hashlib.sha256(raw).hexdigest()
  d = json.loads(raw)
  ins = d['instructions']

  enum = re.findall(r'XTS_F_(\w+)', open(a.header).read())
  fids = [e for e in enum if e not in ('NONE', 'COUNT')]

  # field layouts
  layouts = {}
  for fid, jname in FROM_JSON.items():
    f = d['fields'][jname]
    parts = [(p['shift_from_lsb'], p['width'], p['dest_shift']) for p in f['parts']]
    bits = max(dst + w for _, w, dst in parts)
    layouts[fid] = (S if f.get('signed') else U, bits, parts)
  layouts.update(SCALAR)
  missing = [f for f in fids if f not in layouts]
  unknown = [f for f in layouts if f not in fids]
  if missing or unknown:
    raise SystemExit('field id mismatch with %s: missing layout %s, not in enum %s' % (a.header, missing, unknown))

  # expected-illegal check
  illegal = {r['mnem'] for r in ins if (r.get('hardware') or {}).get('status') == 'illegal'}
  dropped = set(d['legacy_vmx_not_implemented']['unsupported'])
  if illegal != dropped | EXPECTED_ILLEGAL_EXTRA:
    raise SystemExit('expected-illegal set changed: hardware illegal %s vs dropped+extra %s' %
                     (sorted(illegal), sorted(dropped | EXPECTED_ILLEGAL_EXTRA)))

  names = [r['mnem'] for r in ins]
  if len(set(names)) != len(names):
    raise SystemExit('duplicate mnemonics in the JSON')
  idents = [c_ident(m) for m in names]
  if len(set(idents)) != len(idents):
    raise SystemExit('two mnemonics map to the same C identifier')

  os.makedirs(a.out, exist_ok=True)
  h = ['/* GENERATED by tools/gen_isa.py from isa/xenon_isa.json - do not edit. */',
       '#ifndef XTS_ISA_TABLE_H', '#define XTS_ISA_TABLE_H',
       '#define XTS_ISA_COUNT %d' % len(ins),
       '#define XTS_ISA_SHA256 "%s"' % sha,
       'enum xts_insn_id {']
  for i, m in enumerate(names):
    h.append('  %s = %d,' % (idents[i], i))
  for k, (an, base, setb, fixm) in enumerate(ALIASES):
    if base not in names or an in names:
      raise SystemExit('alias %s: base %s missing or alias name taken' % (an, base))
    h.append('  %s = %d,   /* alias: %s */' % (c_ident(an), len(ins) + k, base))
  h += ['};', '#define XTS_ISA_ALIAS_COUNT %d' % len(ALIASES), '#endif', '']

  c = ['/* GENERATED by tools/gen_isa.py from isa/xenon_isa.json - do not edit. */',
       '#include <xts/isa.h>', '',
       'const char xts_isa_sha256[] = "%s";' % sha,
       'const int xts_isa_count = %d;' % len(ins), '',
       'const xts_field_t xts_fields[XTS_F_COUNT] = {']
  for fid in fids:
    kind, bits, parts = layouts[fid]
    ps = ', '.join('{%d, %d, %d}' % p for p in parts)
    c.append('  [XTS_F_%s] = { "%s", %s, %d, %d, { %s } },' % (fid, fid, kind, bits, len(parts), ps))
  c += ['};', '', 'const xts_insn_t xts_isa[%d] = {' % len(ins)]
  baselines = []
  for i, r in enumerate(ins):
    m = r['mnem']
    reads, writes = set(r.get('reads', [])), set(r.get('writes', []))
    tied = set(r.get('tied_operands') or [])
    ops = []
    if len(r['operands']) > 6:
      raise SystemExit('%s: more than 6 operands' % m)
    for op in r['operands']:
      fid = field_for(r, op)
      if fid not in layouts:
        raise SystemExit('%s: operand %s -> field %s has no layout' % (m, op, fid))
      rd, wr = op in reads, op in writes
      if not rd and not wr:
        wr, rd = op in DEST_DEFAULT, op not in DEST_DEFAULT
      if op == 'VS128':
        rd = True
      is_tied = op in tied or (op == 'VS128' and 'VD128' in r['operands'])
      if op in tied:
        rd = wr = True
      role = (['XTS_ROLE_R'] if rd else []) + (['XTS_ROLE_W'] if wr else []) + (['XTS_ROLE_TIED'] if is_tied else [])
      ops.append('{ "%s", XTS_F_%s, %s, %s, 0 }' % (op, fid, opclass(r, op, fid), '|'.join(role) or '0'))
    imp_r, imp_w = [], []
    for nm, bit in (('CR', 'XTS_IMP_CR'), ('LR', 'XTS_IMP_LR'), ('CTR', 'XTS_IMP_CTR'), ('FPSCR', 'XTS_IMP_FPSCR'),
                    ('VSCR', 'XTS_IMP_VSCR'), ('CA', 'XTS_IMP_CA'), ('XER', 'XTS_IMP_XER')):
      if nm in reads and nm not in r['operands']:
        imp_r.append(bit)
      if nm in writes and nm not in r['operands']:
        imp_w.append(bit)
    if 'OE' in writes:
      imp_w.append('XTS_IMP_XER')
    if r['unit'] == 'memory':
      imp_r.append('XTS_IMP_MEM')
      imp_w.append('XTS_IMP_MEM')
    flags = [FLAG_ENUM[f] for f in r['flags'] if f in FLAG_ENUM]
    unknown_flags = [f for f in r['flags'] if f not in FLAG_ENUM]
    if unknown_flags:
      raise SystemExit('%s: unknown flags %s' % (m, unknown_flags))
    t = r.get('timing') or {}
    lat = t.get('latency') if isinstance(t.get('latency'), (int, float)) else -1
    iss = t.get('issue') if isinstance(t.get('issue'), (int, float)) else -1
    fl = []
    flatten('', t, fl)
    for k, v in fl:
      baselines.append((i, k, v))
    hw = (r.get('hardware') or {}).get('status')
    c.append('  [%s] = { %s, %s, %s, %s, 0x%08Xu, 0x%08Xu, 0x%Xu, %d, %s, %s, %s, %s, %s, %d, %s, %s,\n'
             '    { %s }, %sf, %sf },' % (
                 idents[i], cstr(m), cstr(asm_name(r['asm_template'])), cstr(r['asm_template']), cstr(r['form']),
                 int(r['match'], 16), int(r['mask'], 16), EXTRA_MATCH.get(m, 0), i, ISA_ENUM[r['isa']],
                 UNIT_ENUM[r['unit']], PRIV_ENUM[r['privilege']], HW_ENUM[hw], '|'.join(flags) or '0', len(ops),
                 '|'.join(sorted(set(imp_r))) or '0', '|'.join(sorted(set(imp_w))) or '0',
                 ', '.join(ops) if ops else '{ 0, 0, 0, 0, 0 }', repr(float(lat)), repr(float(iss))))
  c += ['};', '', 'const xts_baseline_t xts_isa_baselines[%d] = {' % max(1, len(baselines))]
  for i, k, v in baselines:
    c.append('  { %d, %s, %r },' % (i, cstr(k), v))
  if not baselines:
    c.append('  { 0, "", 0 },')
  c += ['};', 'const int xts_isa_baseline_count = %d;' % len(baselines), '']
  c += ['const xts_insn_alias_t xts_isa_aliases[%d] = {' % max(1, len(ALIASES))]
  for an, base, setb, fixm in ALIASES:
    c.append('  { %s, %s, 0x%08Xu, 0x%08Xu },' % (cstr(an), c_ident(base), setb, fixm))
  if not ALIASES:
    c.append('  { "", 0, 0, 0 },')
  c += ['};', 'const int xts_isa_alias_count = %d;' % len(ALIASES), '']

  def write_if_changed(path, text):
    if os.path.exists(path) and open(path).read() == text:
      return
    open(path, 'w').write(text)
  write_if_changed(os.path.join(a.out, 'isa_table.h'), '\n'.join(h))
  write_if_changed(os.path.join(a.out, 'isa_table.c'), '\n'.join(c))
  print('gen_isa: %d instructions, %d baselines, %d illegal, sha256 %s' % (len(ins), len(baselines), len(illegal), sha[:16]))


if __name__ == '__main__':
  main()
