#!/usr/bin/env python3
"""Replay OpenXenos results4 (10,776 hardware cases, PVR 00710500) through the host-built reference model.

    check_model.py --replay $(BUILD)/host/xts-model-replay --work $(BUILD)/check-model [--openxenos ~/Projects/OpenXenos]
                   [--results isa/hwtest/results4] [--show 3] [--mnem addx,orx]

results4 stores each case's instruction, its operand inputs and every state item that CHANGED, but not the CR/XER/
FPSCR/VSCR/scratch inputs: those come from the deterministic plan, regenerated here by importing OpenXenos
isa/hwtest/host/plans.py (read-only; no bytecode is written there) exactly as host/analyze.py regen_inputs() does.
Every regenerated case must reproduce the recorded instruction word, which proves the regeneration.

Writes <work>/results4_cases.txt (format: host/model_replay.c), runs the replay, prints per mnemonic:
matched / mismatched (with diffs) / unimplemented. Exit status 1 only if some case mismatches (or the regeneration fails).
"""
import argparse
import collections
import json
import os
import subprocess
import sys

sys.dont_write_bytecode = True

# results4 was recorded with semantic plan version 2. plans.py PLAN_VERSION 3 changed only the latency plan
# ("semantic cases are unchanged from 2", plans.py); the per-case word check below verifies that claim.
SEMANTIC_COMPATIBLE = {2: (2, 3)}


def load_plans(openxenos):
  host = os.path.join(openxenos, 'isa', 'hwtest', 'host')
  sys.path.insert(0, host)
  import plans  # noqa: E402  (imports proto + isaenc from OpenXenos, reads its xenon_isa.json)
  import proto  # noqa: E402
  return plans, proto


def regenerate(plans, proto, args):
  ver = args.get('plan_version', 1)
  ok = SEMANTIC_COMPATIBLE.get(ver, (ver,))
  if plans.PLAN_VERSION not in ok:
    raise SystemExit('plans.py PLAN_VERSION %d cannot regenerate semantic plan version %d' % (plans.PLAN_VERSION, ver))
  mn = set(args['mnem'].split(',')) if args.get('mnem') else None
  jobs, _ = plans.semantic_jobs(mn, per_insn=args['per_insn'], per_job=args['batch'])
  return {j.id: j for j in jobs}


def infer_scratch(records):
  """Console address of the scratch buffer: update-form loads write EA = scratch + 128 + d into RA."""
  seen = collections.Counter()
  for rec in records:
    for c in rec['results']:
      if c['mnem'] in ('lwzu', 'lbzu', 'lhzu', 'ldu', 'stwu', 'stbu', 'sthu', 'stdu') and not c['trapped']:
        ra = str(c['ops']['RA'])
        v = c['changes'].get('gpr', {}).get(ra)
        d = c['ops'].get('d', c['ops'].get('ds'))
        if v is not None and d is not None:
          seen[int(v, 16) - 128 - d] += 1
  if len(seen) != 1:
    raise SystemExit('cannot infer the scratch address (candidates %s)' % dict(seen))
  return next(iter(seen))


def write_cases(path, records, jobs, proto, scratch, only):
  K = proto
  n = 0
  bad_regen = []
  with open(path, 'w') as f:
    f.write('SCRATCH %X\n' % scratch)
    for rec in records:
      job = jobs.get(rec['job'])
      for i, c in enumerate(rec['results']):
        if only and c['mnem'] not in only:
          continue
        if job is None or i >= len(job.raw) or (job.raw[i]['insn'] & 0xFFFFFFFF) != int(c['insn'], 16):
          bad_regen.append((rec['job'], c['mnem'], c['insn']))
          continue
        raw = job.raw[i]
        f.write('C %d %s %s\n' % (n, c['mnem'], c['insn']))
        for kind, reg, val in proto.parse_entries(b''.join(raw['entries']), len(raw['entries'])):
          if kind == K.K_GPR: f.write('I gpr %d %016X\n' % (reg, int.from_bytes(val[:8], 'big')))
          elif kind == K.K_FPR: f.write('I fpr %d %016X\n' % (reg, int.from_bytes(val[:8], 'big')))
          elif kind == K.K_VR: f.write('I vr %d %s\n' % (reg, val.hex().upper()))
          elif kind == K.K_CR: f.write('I cr %08X\n' % int.from_bytes(val[:4], 'big'))
          elif kind == K.K_XER: f.write('I xer %016X\n' % int.from_bytes(val[:8], 'big'))
          elif kind == K.K_FPSCR: f.write('I fpscr %08X\n' % int.from_bytes(val[:4], 'big'))
          elif kind == K.K_VSCR: f.write('I vscr %s\n' % val.hex().upper())
          elif kind == K.K_LR: f.write('I lr %016X\n' % int.from_bytes(val[:8], 'big'))
          elif kind == K.K_CTR: f.write('I ctr %016X\n' % int.from_bytes(val[:8], 'big'))
          elif kind == K.K_MEM: f.write('I mem %d %s\n' % (reg, val.hex().upper()))
          elif kind == K.K_GPR_PTR: f.write('P %d %d\n' % (reg, int.from_bytes(val[:8], 'big')))
          elif kind == K.K_MODE: f.write('I mode %X\n' % int.from_bytes(val[:8], 'big'))
          else:
            raise SystemExit('unexpected setup kind %d in job %d' % (kind, rec['job']))
        if c['trapped']:
          f.write('T %s\n' % c['srr1'])
        ch = c['changes']
        for k in ('gpr', 'fpr', 'vr', 'mem'):
          for reg, v in ch.get(k, {}).items():
            f.write('O %s %s %s\n' % (k, reg, v))
        for k in ('cr', 'xer', 'fpscr', 'vscr', 'lr', 'ctr'):
          if k in ch:
            f.write('O %s %s\n' % (k, ch[k]))
        f.write('E\n')
        n += 1
  return n, bad_regen


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument('--replay', required=True)
  ap.add_argument('--work', required=True)
  ap.add_argument('--openxenos', default=os.path.expanduser('~/Projects/OpenXenos'))
  ap.add_argument('--results', default=None, help='default: <openxenos>/isa/hwtest/results4')
  ap.add_argument('--show', type=int, default=3, help='mismatching cases shown per mnemonic')
  ap.add_argument('--mnem', default='', help='only these mnemonics')
  a = ap.parse_args()
  res = a.results or os.path.join(a.openxenos, 'isa', 'hwtest', 'results4')
  records = [json.loads(l) for l in open(os.path.join(res, 'semantic.jsonl')) if l.strip()]
  args = json.load(open(os.path.join(res, 'semantic.args.json')))
  plans, proto = load_plans(a.openxenos)
  jobs = regenerate(plans, proto, args)
  scratch = infer_scratch(records)
  os.makedirs(a.work, exist_ok=True)
  path = os.path.join(a.work, 'results4_cases.txt')
  only = set(a.mnem.split(',')) if a.mnem else None
  n, bad_regen = write_cases(path, records, jobs, proto, scratch, only)
  print('check_model: %d results4 cases (scratch at %08X, plan v%d regenerated with plans.py v%d) -> %s' %
        (n, scratch, args.get('plan_version', 1), plans.PLAN_VERSION, path))
  if bad_regen:
    print('  REGENERATION FAILED for %d cases, e.g. %s' % (len(bad_regen), bad_regen[:5]))
  out = subprocess.run([a.replay, path], capture_output=True, text=True)
  if out.returncode != 0:
    print(out.stderr)
    raise SystemExit('replay failed')
  per = collections.defaultdict(lambda: collections.Counter())
  examples = collections.defaultdict(list)
  for line in out.stdout.splitlines():
    p = line.split(' ', 5)
    if len(p) < 5 or p[0] != 'R':
      continue
    mn, verdict = p[2], p[3]
    per[mn][verdict] += 1
    if verdict in ('mismatch', 'badinput') and len(examples[mn]) < a.show:
      examples[mn].append(line)
  tot = collections.Counter()
  for mn in per:
    tot.update(per[mn])
  implemented = sorted(m for m in per if per[m]['match'] + per[m]['mismatch'])
  print('  model verdicts: %d match, %d mismatch, %d unimplemented, %d bad input; %d mnemonics have a handler or are '
        'expected-illegal' % (tot['match'], tot['mismatch'], tot['unimpl'], tot['badinput'], len(implemented)))
  # flag-variant coverage of the implemented mnemonics (Rc/OE)
  variants = collections.defaultdict(collections.Counter)
  for rec in records:
    for c in rec['results']:
      if c['mnem'] in implemented:
        variants[c['mnem']]['+'.join(c['flags']) or 'plain'] += 1
  for mn in implemented:
    v = per[mn]
    flags = ', '.join('%s %d' % kv for kv in sorted(variants[mn].items()))
    print('    %-12s %3d match %3d mismatch   variants: %s' % (mn, v['match'], v['mismatch'], flags))
  unimpl = sorted(m for m in per if per[m]['unimpl'])
  print('  unimplemented (%d mnemonics, %d cases): %s' % (len(unimpl), tot['unimpl'], ' '.join(unimpl)))
  for mn in sorted(examples):
    print('  MISMATCH %s (%d cases):' % (mn, per[mn]['mismatch'] + per[mn]['badinput']))
    for line in examples[mn]:
      print('    ' + line[:600])
  bad = tot['mismatch'] + tot['badinput'] + len(bad_regen)
  print('check_model: %s' % ('PASS' if not bad else 'FAIL (%d)' % bad))
  return 1 if bad else 0


if __name__ == '__main__':
  sys.exit(main())
