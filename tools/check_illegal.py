#!/usr/bin/env python3
"""Scan the code sections of a built ELF for instruction words the console treats as illegal.

    check_illegal.py --elf $(BUILD)/console/xts-<profile>.elf [--json isa/xenon_isa.json] [--nm xenon-nm]

Illegal = every JSON instruction with hardware.status == "illegal" (the 22 legacy VMX ops GCC/binutils still know, plus
lvsl128 lvsr128 mcrxr popcntb dcba fre frsqrtes). Only executable sections are scanned; deliberate test words live in
the data section .xts_testwords (never executable, so never scanned) and kernels are generated at run time. Words
inside libxenon's .text that are data (the page table between `pagetable` and `pagetable_end`) are skipped.
Exit status 1 if any illegal word is found.
"""
import argparse
import bisect
import json
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHF_EXECINSTR = 0x4


def sections(data):
  if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 2:
    raise SystemExit('not a 32-bit big-endian ELF')
  shoff, = struct.unpack_from('>I', data, 0x20)
  shentsize, shnum, shstrndx = struct.unpack_from('>HHH', data, 0x2E)
  hdrs = [struct.unpack_from('>IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
  stroff = hdrs[shstrndx][4]
  out = []
  for h in hdrs:
    name_off, typ, flags, addr, off, size = h[:6]
    end = data.index(b'\0', stroff + name_off)
    out.append(dict(name=data[stroff + name_off:end].decode(), type=typ, flags=flags, addr=addr, off=off, size=size))
  return out


def symbols(nm, elf):
  try:
    out = subprocess.run([nm, '-n', elf], capture_output=True, text=True, check=True).stdout
  except (OSError, subprocess.CalledProcessError):
    return [], {}
  syms, by = [], {}
  for line in out.splitlines():
    p = line.split()
    if len(p) == 3:
      a = int(p[0], 16)
      syms.append((a, p[2]))
      by[p[2]] = a
  return syms, by


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument('--elf', required=True)
  ap.add_argument('--json', default=os.path.join(ROOT, 'isa', 'xenon_isa.json'))
  ap.add_argument('--nm', default='/usr/local/xenon/bin/xenon-nm')
  a = ap.parse_args()
  isa = json.load(open(a.json))
  illegal = [(int(r['match'], 16), int(r['mask'], 16), r['mnem']) for r in isa['instructions']
             if (r.get('hardware') or {}).get('status') == 'illegal']
  data = open(a.elf, 'rb').read()
  syms, by = symbols(a.nm, a.elf)
  addrs = [s[0] for s in syms]
  skip = []
  if 'pagetable' in by and 'pagetable_end' in by:
    skip.append((by['pagetable'], by['pagetable_end']))
  if 'xts_testwords_start' in by and 'xts_testwords_end' in by:
    skip.append((by['xts_testwords_start'], by['xts_testwords_end']))
  hits, scanned, secs = [], 0, []
  for s in sections(data):
    if not (s['flags'] & SHF_EXECINSTR) or s['type'] == 8 or s['name'] == '.xts_testwords':   # 8 = NOBITS
      continue
    secs.append(s['name'])
    for o in range(0, s['size'] - 3, 4):
      addr = s['addr'] + o
      if any(lo <= addr < hi for lo, hi in skip):
        continue
      w, = struct.unpack_from('>I', data, s['off'] + o)
      scanned += 1
      for match, mask, mn in illegal:
        if (w & mask) == match:
          i = bisect.bisect_right(addrs, addr) - 1
          where = '%s+0x%X' % (syms[i][1], addr - syms[i][0]) if i >= 0 else '?'
          hits.append((addr, w, mn, s['name'], where))
  print('check_illegal: %s: %d words in %s scanned for %d hardware-illegal encodings, %d hits' %
        (os.path.basename(a.elf), scanned, ', '.join(secs), len(illegal), len(hits)))
  for addr, w, mn, sec, where in hits[:50]:
    print('  ILLEGAL %08X: %08X %-10s (%s, %s)' % (addr, w, mn, sec, where))
  return 1 if hits else 0


if __name__ == '__main__':
  sys.exit(main())
