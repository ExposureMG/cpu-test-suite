#!/usr/bin/env python3
"""Cross-check the C encoder (host build) against xenon-as for EVERY instruction in isa/xenon_isa.json.

    check_encoder.py --cli $(BUILD)/host/xts-enc-cli [--as /usr/local/xenon/bin/xenon-as] [--cases 8] [--seed 1] [-v]

For each instruction: several random operand sets (random flag variants), plus, for instructions with 7-bit VMX128
register fields, 128 extra sets that walk every register number 0-127 through every such operand. The assembler text
is derived from the JSON asm_template (binutils' 4-operand syntax for the tied VMX128 ops falls out of the template,
which lists VS128 explicitly). Every word is also decoded back by the C decoder and must name the instruction.

Instructions xenon-as does not know by name are checked through an alias (ALIASES below). An instruction for which no
operand set assembles is reported as NOT CHECKED. Exit status: 0 only if there are no mismatches and nothing is left
unchecked except the documented exceptions.
"""
import argparse
import json
import os
import random
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import gen_isa  # noqa: E402

ROOT = os.path.dirname(HERE)
# instructions binutils 2.32 (+vmx128 patch) does not know by this name: same encoding under another spelling
ALIASES = {
    'dcbz128': 'dcbzl [RA0], [RB]',          # Cell name of dcbz with L=1 (128-byte line)
    'xdcbt': 'dcbt [RA0], [RB], 16',         # dcbt with TH=0b10000 (embedded-form operand order under -mcell)
}
# instructions whose GNU spelling depends on the -m cpu: under -mcell/-mvmx128 binutils assembles `mftb` as the
# mfspr form (XO 339, "mftb is mfspr 268 on POWER4+"); the table (and -many/-mppc64) use the classic XO 371 encoding.
# Both execute on the console. These are cross-checked with the generic dialect instead.
DIALECT = {'mftb': ['-many']}
# operand domains narrower than the field (GNU refuses the rest; the hardware field is wider)
DOMAIN = {
    ('vspltb', 'UIMM'): (0, 15, False), ('vsplth', 'UIMM'): (0, 7, False), ('vspltw', 'UIMM'): (0, 3, False),
    ('mfocrf', 'CRM'): [1 << i for i in range(8)], ('mtocrf', 'CRM'): [1 << i for i in range(8)],
}
# Genuine table/assembler disagreements. Printed on every run; they do not fail the check, any other mismatch does.
KNOWN_DISAGREEMENTS = {
    'lvsl128': 'JSON (xenia) VX128_1 xop 0x003 (match 0x10000003); binutils vmx128 patch xop 1 (0x10000001). The table '
               'encoding traps (illegal) on hardware on every results4 case, and the primary-4 sweep found no unknown word '
               'that executes, so the binutils spelling traps too: lvsl128 does not exist on the console either way.',
    'lvsr128': 'JSON (xenia) xop 0x043 (match 0x10000043); binutils xop 65 (0x10000041). Same situation as lvsl128.',
}
VALID_BO = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 24, 25, 26, 27]
FLAG_TXT = (('[OE]', 'oe', 'o'), ('[Rc]', 'rc', '.'), ('[T]', 't', 't'), ('[LK]', 'lk', 'l'), ('[AA]', 'aa', 'a'),
            ('[A]', 'a', 'all'))
FLAG_BIT = {'rc': 1, 'oe': 2, 'lk': 4, 'aa': 8, 't': 16, 'a': 32}
VR7 = {'VD128', 'VA128', 'VB128', 'VS128'}


def layouts(d):
  lay = {}
  for fid, jname in gen_isa.FROM_JSON.items():
    f = d['fields'][jname]
    parts = [(p['shift_from_lsb'], p['width'], p['dest_shift']) for p in f['parts']]
    lay[fid] = (gen_isa.S if f.get('signed') else gen_isa.U, max(dst + w for _, w, dst in parts))
  for k, (kind, bits, _) in gen_isa.SCALAR.items():
    lay[k] = (kind, bits)
  return lay


def domain(r, op, lay):
  if (r['mnem'], op) in DOMAIN:
    return DOMAIN[(r['mnem'], op)]
  if op == 'BO':
    return [b for b in VALID_BO if r['mnem'] != 'bcctrx' or b & 4]
  fid = gen_isa.field_for(r, op)
  kind, bits = lay[fid]
  if op == 'TBR':
    return [268, 269]
  if kind in (gen_isa.S, gen_isa.D4):
    lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
  else:
    lo, hi = 0, (1 << bits) - 1
  return (lo, hi, kind == gen_isa.D4)


def rand_value(rnd, dom):
  if isinstance(dom, list):
    return rnd.choice(dom)
  lo, hi, al4 = dom
  # bias towards the edges sometimes
  v = rnd.choice([lo, hi, 0, 1, -1]) if rnd.random() < 0.15 else rnd.randint(lo, hi)
  v = max(lo, min(hi, v))
  if al4:
    v &= ~3
  return v


def asm_text(r, ops, flags, alias=None):
  txt = alias or r['asm_template']
  for ph, f, on in FLAG_TXT:
    txt = txt.replace(ph, on if f in flags else '')
  for n in r['operands']:
    v = ops[n]
    if n == 'ADDR':
      v = ('%d' % v) if 'aa' in flags else ('.+%d' % v if v >= 0 else '.%d' % v)
    elif n == 'NB' and v == 0:
      v = 32
    txt = txt.replace('[%s]' % n, str(v))
  if r['mnem'] == 'dss' and 'a' in flags:
    txt = 'dssall'
  return txt


def fix_forms(r, ops, rnd):
  """GNU as refuses invalid forms the hardware traps on (update with RA=0 or RA=RT, lmw/lswi loading RA): avoid them."""
  m = r['mnem']
  upd = r['unit'] == 'memory' and 'RA' in r['operands'] and 'RA' in r.get('writes', [])
  if upd:
    t = ops.get('RD', ops.get('FD', ops.get('RS', ops.get('FS'))))
    while ops['RA'] == 0 or (ops['RA'] == t and ('RD' in ops)):
      ops['RA'] = rnd.randrange(1, 32)
  if m == 'lmw':
    ops['RA0'] = rnd.randrange(0, ops['RD']) if ops['RD'] > 0 else 0
  if m in ('lswi',):
    n = ops['NB'] or 32
    regs = {(ops['RD'] + k) % 32 for k in range((n + 3) // 4)}
    while ops['RA0'] in regs:
      ops['RA0'] = rnd.randrange(0, 32)


def gen_cases(d, r, lay, rnd, n):
  m = r['mnem']
  out = []
  regs7 = [o for o in r['operands'] if o in VR7 and o != 'VS128']
  total = n + (128 if regs7 else 0)
  for k in range(total):
    ops = {}
    for o in r['operands']:
      ops[o] = rand_value(rnd, domain(r, o, lay))
    if k >= n:                                  # walk every 7-bit register number through every register operand
      i = k - n
      for j, o in enumerate(regs7):
        ops[o] = (i + 37 * j) % 128
    if 'VS128' in ops and 'VD128' in ops:
      ops['VS128'] = ops['VD128']
    flags = [f for f in r['flags'] if rnd.random() < 0.5]
    if 'aa' in flags and 'ADDR' in ops:
      ops['ADDR'] = abs(ops['ADDR']) & ((1 << (25 if r['form'] == 'I' else 15)) - 4)
    if m == 'dss' and 'a' in flags:
      ops['STRM'] = 0
    fix_forms(r, ops, rnd)
    out.append((ops, flags))
  return out


def assemble(asm, lines, tmp, extra_flags):
  """Assemble; drop lines the assembler rejects (iteratively). Returns {line index: word}."""
  live = list(range(len(lines)))
  rejected = {}
  for _ in range(50):
    src = os.path.join(tmp, 'c.s')
    with open(src, 'w') as f:
      for i in live:
        f.write(' %s\n' % lines[i])
    obj = os.path.join(tmp, 'c.o')
    p = subprocess.run([asm] + extra_flags + [src, '-o', obj], capture_output=True, text=True)
    bad = set()
    for ln in p.stderr.splitlines():
      mm = re.match(r'.*c\.s:(\d+): Error: (.*)', ln)
      if mm:
        idx = live[int(mm.group(1)) - 1]
        bad.add(idx)
        rejected.setdefault(idx, mm.group(2))
    if p.returncode == 0 and not bad:
      break
    if not bad:
      raise SystemExit('assembler failed without line errors:\n' + p.stderr[:2000])
    live = [i for i in live if i not in bad]
  od = subprocess.run([asm.replace('-as', '-objdump'), '-d', '-j', '.text', obj], capture_output=True, text=True).stdout
  words = []
  for line in od.splitlines():
    parts = line.split('\t')
    if len(parts) >= 2 and parts[0].strip().endswith(':') and len(parts[1].split()) == 4:
      words.append(int(''.join(parts[1].split()), 16))
  if len(words) != len(live):
    raise SystemExit('objdump returned %d words for %d lines' % (len(words), len(live)))
  return dict(zip(live, words)), rejected


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument('--cli', required=True)
  ap.add_argument('--json', default=os.path.join(ROOT, 'isa', 'xenon_isa.json'))
  ap.add_argument('--as', dest='asm', default='/usr/local/xenon/bin/xenon-as')
  ap.add_argument('--cases', type=int, default=8)
  ap.add_argument('--seed', type=int, default=1)
  ap.add_argument('--work', default=None, help='scratch directory for the assembler files (default: a temp dir)')
  ap.add_argument('-v', action='store_true')
  a = ap.parse_args()
  d = json.load(open(a.json))
  ins = d['instructions']
  lay = layouts(d)
  rnd = random.Random(a.seed)
  table = subprocess.run([a.cli, '--table'], capture_output=True, text=True, check=True).stdout.split('\n')
  ids = {ln.split()[1]: int(ln.split()[0]) for ln in table if ln.strip()}
  assert len(ids) == len(ins), 'C table has %d entries, JSON %d' % (len(ids), len(ins))

  cases = []          # (mnem, ops, flags, asm text)
  for r in ins:
    alias = ALIASES.get(r['mnem'])
    for ops, flags in gen_cases(d, r, lay, rnd, a.cases):
      cases.append((r, ops, flags, asm_text(r, ops, flags, alias)))

  # C encoder
  req = []
  for r, ops, flags, _ in cases:
    fl = sum(FLAG_BIT[f] for f in flags)
    req.append('E %d %d %s' % (ids[r['mnem']], fl, ' '.join(str(ops[o]) for o in r['operands'])))
  enc = subprocess.run([a.cli], input='\n'.join(req) + '\n', capture_output=True, text=True, check=True).stdout.split('\n')
  mine = enc[:len(cases)]
  dec = subprocess.run([a.cli], input=''.join('D %s\n' % w for w in mine if not w.startswith('ERR')),
                       capture_output=True, text=True, check=True).stdout.split('\n')
  dec_it = iter(dec)

  if a.work:
    os.makedirs(a.work, exist_ok=True)
    tmp = a.work
  else:
    tmp = tempfile.mkdtemp(prefix='xts_enc_')
  main_idx = [i for i, c in enumerate(cases) if c[0]['mnem'] not in DIALECT]
  gnu, rejected = {}, {}
  g, rj = assemble(a.asm, [cases[i][3] for i in main_idx], tmp, ['-mvmx128', '-mcell', '-many'])
  gnu.update({main_idx[k]: w for k, w in g.items()})
  rejected.update({main_idx[k]: e for k, e in rj.items()})
  for mn, fl in DIALECT.items():
    idx = [i for i, c in enumerate(cases) if c[0]['mnem'] == mn]
    g, rj = assemble(a.asm, [cases[i][3] for i in idx], tmp, fl)
    gnu.update({idx[k]: w for k, w in g.items()})
    rejected.update({idx[k]: e for k, e in rj.items()})

  per = {}
  mismatches = []
  enc_errors = []
  decode_bad = []
  for i, (r, ops, flags, txt) in enumerate(cases):
    m = r['mnem']
    st = per.setdefault(m, dict(checked=0, rejected=0, alias=m in ALIASES))
    w = mine[i]
    if w.startswith('ERR'):
      enc_errors.append((m, ops, flags, w))
    else:
      hits = next(dec_it).split()
      if str(ids[m]) not in hits:
        decode_bad.append((m, w, hits))
    if i not in gnu:
      st['rejected'] += 1
      continue
    st['checked'] += 1
    if w.startswith('ERR') or int(w, 16) != gnu[i]:
      mismatches.append((m, txt, w, '%08X' % gnu[i]))

  print('check_encoder: %d instructions, %d operand sets, %d assembled by xenon-as (%d rejected by it)' %
        (len(ins), len(cases), len(gnu), len(rejected)))
  unchecked = sorted(m for m, s in per.items() if s['checked'] == 0)
  aliased = sorted(m for m, s in per.items() if s['alias'] and s['checked'])
  print('  checked with another assembler dialect: %s' % ', '.join('%s (%s)' % (m, ' '.join(f)) for m, f in DIALECT.items()))
  print('  checked through an alias spelling: %s' % (', '.join('%s (%s)' % (m, ALIASES[m].split()[0]) for m in aliased) or 'none'))
  print('  instructions with no assembled operand set (NOT CHECKED): %s' % (', '.join(unchecked) or 'none'))
  if enc_errors:
    print('  C encoder rejected %d generated operand sets, e.g. %s' % (len(enc_errors), enc_errors[:3]))
  if decode_bad:
    print('  decode-back failures: %d, e.g. %s' % (len(decode_bad), decode_bad[:5]))
  seen = {}
  for m, t, x, g in mismatches:
    seen.setdefault(m, []).append((t, x, g))
  known = {m: v for m, v in seen.items() if m in KNOWN_DISAGREEMENTS}
  real = {m: v for m, v in seen.items() if m not in KNOWN_DISAGREEMENTS}
  print('  mismatches: %d words over %d instructions (%d known table/assembler disagreements)' %
        (len(mismatches), len(seen), len(known)))
  for m, lst in sorted(seen.items()):
    t, x, g = lst[0]
    print('    %s %-12s %d words, e.g. "%s"  ours=%s xenon-as=%s' % ('KNOWN   ' if m in known else 'MISMATCH', m, len(lst), t, x, g))
    if m in known:
      print('             %s' % KNOWN_DISAGREEMENTS[m])
  for m in sorted(set(KNOWN_DISAGREEMENTS) - set(known)):
    print('    NOTE: known disagreement for %s no longer reproduces; remove it from KNOWN_DISAGREEMENTS' % m)
  if a.v:
    for i, msg in sorted(rejected.items())[:40]:
      print('    rejected: %-40s %s' % (cases[i][3], msg))
  partial = sorted((m, s['rejected']) for m, s in per.items() if s['rejected'] and s['checked'])
  if partial and a.v:
    print('  partially rejected (assembler refused some operand sets):', partial)
  # encoding aliases (gen_isa.ALIASES: XI_lwsync, XI_ptesync): C encoding == xenon-as, decodes back as the base
  alias_bad = 0
  al = subprocess.run([a.cli, '--aliases'], capture_output=True, text=True, check=True).stdout.split('\n')
  al_ids = {ln.split()[1]: int(ln.split()[0]) for ln in al if ln.strip()}
  names = [an for an, _, _, _ in gen_isa.ALIASES]
  if sorted(al_ids) != sorted(names):
    print('  aliases: C table has %s, gen_isa.ALIASES %s' % (sorted(al_ids), sorted(names)))
    alias_bad += 1
  else:
    out = subprocess.run([a.cli], input=''.join('E %d 0\n' % al_ids[n] for n in names), capture_output=True, text=True,
                         check=True).stdout.split('\n')[:len(names)]
    g, rj = assemble(a.asm, names, tmp, ['-mvmx128', '-mcell', '-many'])
    for k, (an, base, setb, fixm) in enumerate(gen_isa.ALIASES):
      hits = subprocess.run([a.cli], input='D %s\n' % out[k], capture_output=True, text=True, check=True).stdout.split()
      okw = k in g and not out[k].startswith('ERR') and int(out[k], 16) == g[k]
      okd = str(ids[base]) in hits
      if not (okw and okd):
        alias_bad += 1
      print('  alias %-8s ours=%s xenon-as=%s decodes as %s: %s' % (an, out[k], '%08X' % g[k] if k in g else 'rejected',
                                                                     base, 'ok' if okw and okd else 'MISMATCH'))
  bad = sum(len(v) for v in real.values()) + len(enc_errors) + len(decode_bad) + len(unchecked) + alias_bad
  print('check_encoder: %s' % ('PASS' if not bad else 'FAIL (%d problems)' % bad))
  return 1 if bad else 0


if __name__ == '__main__':
  sys.exit(main())
