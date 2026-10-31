#!/usr/bin/env python3
"""check_syms.py - build-time checks of cross-compiled benchmark objects.

  check_syms.py kernels --tc <root> --variant <id> [--header bench_lib.h] OBJ...
      The tools/kcheck.sh rules for every kernel object of one variant, plus
      the namespace rule the A/B link depends on:
        * undefined symbols: <id>_lib_<routine> (LIB kernels only, routine in
          BENCH_LIB_SYMBOLS), sqrt/sqrtf (GCC's errno fallback) and libgcc's
          _save*/_rest* helpers; nothing else (no libc / libm calls);
        * no writable data: no .data/.sdata/.bss/.sbss... section with a
          non-zero size, no common symbols;
        * every global symbol it defines starts with "<id>_" and the file
          defines at least one <id>_k_<kernel> descriptor.
      table.o may reference <id>_k_* and defines <id>_info; the generated
      missing.o placeholder only defines <id>_k_*.  Exit status 1 on any
      violation.

  check_syms.py sizes --size-a <xenon-size> --size-b <xenon-size>
                      [--label-a A] [--label-b B] DIR_A DIR_B
      Per-kernel .text bytes of variant A vs variant B.

  check_syms.py map --map <file.map> --libc <libc.a path> VARIANT:LIBOBJ...
      Checks the link map of the A/B image: every <id>_lib_<routine> comes
      from that variant's prefixed object, every <id>_k_* descriptor is
      present, and the harness's own memcpy comes from the linking
      toolchain's libc.

  check_syms.py place --map <file.map> --align N --nm <xenon-nm> IMAGE.elf
      Checks the code placement of the A/B image: every non-empty .text* and
      .rodata* input section of a placed kernel object (build/placed/,
      build/placed-s<S>/) or prefixed newlib object (lib_<id>.o,
      lib_<id>_s<S>.o) starts on an N-byte boundary, and its code starts S
      bytes past it (S = 0, or the layout shift the object was placed
      with): the first function of a kernel's .text, and the first function
      of every newlib member inside a prefixed object's .text (the first
      function of each N-byte block, except blocks that continue the code
      of a member which started in an earlier block).  N = 0 skips the
      check (packed link).

  check_syms.py isa --objdump <testxenon3 xenon-objdump> IMAGE...
      Every word of the images' code must be an instruction the Xenon PPE
      implements.  testxenon3's "objdump -M xenon" decodes only the words in
      its Xenon allow-list (the measured instruction set: no vmsum*/vmul*/
      vsum*, isel, popcntb, fre, cmpb, fri*, VSX, ISA 2.06+ except
      ldbrx/stdbrx ...) and prints anything else as ".long".  Allowed
      ".long" words: zero padding and data that no fall-through path reaches
      (at a symbol start or after an unconditional b/blr/bctr, ignoring
      nops).  Anything else would raise the illegal-instruction interrupt
      on the console (or is code test16old emitted for the Cell).
"""

import argparse
import os
import re
import subprocess
import sys

HELPER_RE = r"_(save|rest)(gpr|fpr|vr|vr128)_[0-9]+(_[a-z])?"
WRITABLE_RE = re.compile(r"^\.(s?data|s?bss)")


def run(cmd):
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           universal_newlines=True)
    except OSError as e:
        sys.exit("check_syms: cannot run %s: %s" % (cmd[0], e))
    if p.returncode != 0:
        sys.exit("check_syms: %s failed:\n%s" % (" ".join(cmd), p.stderr))
    return p.stdout


def lib_symbols(header):
    if not header:
        return None
    with open(header) as f:
        text = f.read()
    m = re.search(r"#define\s+BENCH_LIB_SYMBOLS\s*\\?\s*((?:\"[^\"]*\"\s*\\?\s*)+)",
                  text)
    if not m:
        sys.exit("check_syms: %s: BENCH_LIB_SYMBOLS not found" % header)
    return set(" ".join(re.findall(r"\"([^\"]*)\"", m.group(1))).split())


def nm_syms(nm, obj):
    """[(name, type)] of the global symbols plus undefined ones."""
    res = []
    for line in run([nm, "-P", obj]).splitlines():
        f = line.split()
        if len(f) >= 2:
            res.append((f[0], f[1]))
    return res


def section_sizes(size, obj):
    res = []
    for line in run([size, "-A", obj]).splitlines():
        f = line.split()
        if len(f) >= 2 and f[0].startswith(".") and f[1].isdigit():
            res.append((f[0], int(f[1])))
    return res


def cmd_kernels(a):
    nm = os.path.join(a.tc, "bin", "xenon-nm")
    size = os.path.join(a.tc, "bin", "xenon-size")
    libs = lib_symbols(a.header)
    v = a.variant
    allowed = re.compile(r"^(sqrtf?|%s)$" % HELPER_RE)
    lib_re = re.compile(r"^%s_lib_([A-Za-z0-9_]+)$" % re.escape(v))
    kdesc = re.compile(r"^%s_k_[a-z0-9_]+$" % re.escape(v))
    bad = 0
    checked = 0
    for obj in a.objs:
        base = os.path.basename(obj)
        stem = base[:-2] if base.endswith(".o") else base
        is_table = stem == "table"
        is_missing = stem == "missing"
        is_lib = stem.startswith("lib_")
        errs = []
        syms = nm_syms(nm, obj)
        defined = [n for n, t in syms if t not in "Uwv" and t.isupper()]
        undef = [n for n, t in syms if t in "Uwv"]
        common = [n for n, t in syms if t == "C"]
        for n in undef:
            if is_table and kdesc.match(n):
                continue
            m = lib_re.match(n)
            if m:
                if not is_lib:
                    errs.append("%s: BLIB() call outside a LIB kernel" % n)
                elif libs is not None and m.group(1) not in libs:
                    errs.append("%s: not in BENCH_LIB_SYMBOLS" % n)
                continue
            if not allowed.match(n):
                errs.append("references disallowed symbol %s" % n)
        for sec, sz in section_sizes(size, obj):
            if WRITABLE_RE.match(sec) and sz > 0:
                errs.append("writable data section %s (%d bytes)" % (sec, sz))
        for n in common:
            errs.append("common (writable) symbol %s" % n)
        for n in defined:
            if not n.startswith(v + "_"):
                errs.append("global symbol %s is not in the %s_ namespace "
                            "(make it static or use BENCH_NS)" % (n, v))
        if is_table:
            if (v + "_info") not in defined:
                errs.append("does not define %s_info" % v)
        elif not any(kdesc.match(n) for n in defined):
            errs.append("defines no %s_k_<id> descriptor" % v)
        if is_missing and undef:
            errs.append("placeholder has undefined symbols")
        checked += 1
        for e in errs:
            print("check_syms: %s (%s): %s" % (obj, v, e))
        if errs:
            bad += 1
    if bad:
        print("check_syms: %s: %d of %d objects FAILED" % (v, bad, checked))
        return 1
    print("check_syms: %s: %d objects ok" % (v, checked))
    return 0


def text_size(size, obj):
    return sum(sz for sec, sz in section_sizes(size, obj)
               if sec.startswith(".text"))


def cmd_sizes(a):
    def objs(d):
        return {f[:-2]: os.path.join(d, f) for f in os.listdir(d)
                if f.endswith(".o") and f != "table.o"} if os.path.isdir(d) else {}
    oa, ob = objs(a.dir_a), objs(a.dir_b)
    order = ["int", "br", "mem", "fp", "vec", "mix", "lib"]  # display order
    names = sorted(set(oa) | set(ob),
                   key=lambda n: (order.index(n.split("_")[0])
                                  if n.split("_")[0] in order else len(order), n))
    print("%-18s %10s %10s %8s" % ("kernel", a.label_a, a.label_b, "B/A"))
    ta = tb = 0
    for n in names:
        sa = text_size(a.size_a, oa[n]) if n in oa else None
        sb = text_size(a.size_b, ob[n]) if n in ob else None
        ratio = "%8.3f" % (sb / sa) if sa and sb is not None else "%8s" % "-"
        print("%-18s %10s %10s %s" % (n, "-" if sa is None else sa,
                                      "-" if sb is None else sb, ratio))
        ta += sa or 0
        tb += sb or 0
    print("%-18s %10d %10d %8s" % ("total", ta, tb,
                                   "%.3f" % (tb / ta) if ta else "-"))
    return 0


def parse_map(path):
    """{symbol: object file} for the symbols listed under input sections."""
    owner = {}
    cur = None
    in_memmap = False
    with open(path) as f:
        for line in f:
            if line.startswith("Linker script and memory map"):
                in_memmap = True
                continue
            if not in_memmap:
                continue
            # " .text          0x80001234      0x120 build/x/y.o"
            m = re.match(r"^ (\.\S+)?\s+0x[0-9a-fA-F]+\s+0x[0-9a-fA-F]+\s+(\S.*)$",
                         line)
            if m:
                cur = m.group(2).strip()
                continue
            # "                0x80001234                symbol"
            m = re.match(r"^\s+0x[0-9a-fA-F]+\s+([A-Za-z_.$][\w.$]*)\s*$", line)
            if m and cur:
                owner.setdefault(m.group(1), cur)
                continue
            if re.match(r"^ \.\S+\s*$", line):
                continue  # long section name; numbers on the next line
            if re.match(r"^\S", line):
                cur = None
    return owner


def cmd_map(a):
    owner = parse_map(a.map)
    errs = []
    print("check_syms: map %s: %d symbols" % (a.map, len(owner)))
    for spec in a.variants:
        vid, _, libobj = spec.partition(":")
        libsyms = [s for s in owner if s.startswith(vid + "_lib_")]
        kdescs = [s for s in owner if s.startswith(vid + "_k_")]
        for s in sorted(libsyms):
            if os.path.normpath(owner[s]) != os.path.normpath(libobj):
                errs.append("%s comes from %s, not %s" % (s, owner[s], libobj))
        print("  %s: %d kernel descriptors, %d %s_lib_* symbols from %s" %
              (vid, len(kdescs), len(libsyms), vid, libobj))
        if not kdescs:
            errs.append("no %s_k_* kernel descriptors in the image" % vid)
        if (vid + "_info") not in owner:
            errs.append("%s_info missing" % vid)
        if a.expect_lib and (vid + "_lib_" + a.expect_lib) not in owner:
            errs.append("%s_lib_%s missing" % (vid, a.expect_lib))
        elif a.expect_lib:
            print("    %s_lib_%s <- %s" % (vid, a.expect_lib,
                                         owner[vid + "_lib_" + a.expect_lib]))
    if a.libc:
        src = owner.get("memcpy")
        if not src:
            errs.append("memcpy not in the map")
        elif not os.path.normpath(src).startswith(os.path.normpath(a.libc)):
            errs.append("memcpy comes from %s, not %s" % (src, a.libc))
        else:
            print("  memcpy <- %s" % src)
    for e in errs:
        print("check_syms: map: %s" % e)
    if errs:
        return 1
    print("check_syms: map ok")
    return 0


def parse_input_sections(path):
    """[(section, address, size, object)] of the memory map's input sections."""
    res = []
    pending = None
    in_memmap = False
    with open(path) as f:
        for line in f:
            if line.startswith("Linker script and memory map"):
                in_memmap = True
                continue
            if not in_memmap:
                continue
            m = re.match(r"^ (\.\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*)$", line)
            if m:
                res.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16),
                            m.group(4).strip()))
                pending = None
                continue
            m = re.match(r"^ (\.\S+)\s*$", line)
            if m:
                pending = m.group(1)  # long name; numbers on the next line
                continue
            m = re.match(r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*)$", line)
            if m and pending:
                res.append((pending, int(m.group(1), 16), int(m.group(2), 16),
                            m.group(3).strip()))
            pending = None
    return res


PLACED_RE = re.compile(r"(?:^|/)placed(?:-s(\d+))?/")
LIBOBJ_RE = re.compile(r"(?:^|/)lib_[a-z0-9]+?(?:_s(\d+))?\.o$")
PLACED_SEC_RE = re.compile(r"^\.(text|rodata)(\..*)?$")
MERGED_SEC_RE = re.compile(r"^\.rodata\.(cst|str)")


def cmd_place(a):
    if not a.align:
        print("check_syms: place: PLACE_ALIGN=0 (packed link), nothing to check")
        return 0
    funcs = []  # (address, end), sorted
    for line in run([a.nm, "-n", "-S", "--defined-only", a.image]).splitlines():
        f = line.split()
        if len(f) == 4 and f[2] in "tTwW":
            x = int(f[0], 16)
            funcs.append((x, x + int(f[1], 16)))
        elif len(f) == 3 and f[1] in "tTwW":
            x = int(f[0], 16)
            funcs.append((x, x))
    errs = []
    stats = {}  # shift -> [sections, code blocks]
    for name, addr, size, obj in parse_input_sections(a.map):
        if not size or not PLACED_SEC_RE.match(name) or MERGED_SEC_RE.match(name):
            continue
        m = PLACED_RE.search(obj)
        lib = not m
        if lib:
            m = LIBOBJ_RE.search(obj)
        if not m:
            continue
        shift = int(m.group(1) or 0)
        st = stats.setdefault(shift, [0, 0])
        st[0] += 1
        if addr % a.align:
            errs.append("%s %s at 0x%x is not on a %d-byte boundary" %
                        (obj, name, addr, a.align, ))
            continue
        if not name.startswith(".text"):
            continue
        inside = [f for f in funcs if addr <= f[0] < addr + size]
        if not inside:
            errs.append("%s %s at 0x%x: no function symbols" % (obj, name, addr))
            continue
        starts = [inside[0][0]]  # a kernel object: its section's first function
        if lib:
            # every newlib member starts on a block; a block whose first
            # function continues the previous member's code (it runs into
            # the block, or follows it within 64 bytes) is not a member start
            reach = inside[0][1]
            for (x, e), (px, pe) in zip(inside[1:], inside):
                if (x - addr) // a.align != (px - addr) // a.align:
                    blk0 = addr + (x - addr) // a.align * a.align
                    if reach <= blk0 and x - max(reach, px) >= 64:
                        starts.append(x)
                reach = max(reach, e)
        for x in starts:
            st[1] += 1
            if (x - addr) % a.align != shift:
                errs.append("%s %s: code at 0x%x = boundary + %d, not + %d" %
                            (obj, name, x, (x - addr) % a.align, shift))
    for shift in sorted(stats):
        print("check_syms: place: shift %d: %d sections on %d-byte boundaries, "
              "%d kernels/newlib members start at boundary + %d" %
              (shift, stats[shift][0], a.align, stats[shift][1], shift))
    if not stats:
        errs.append("no placed sections found in %s" % a.map)
    for e in errs[:40]:
        print("check_syms: place: " + e)
    if errs:
        return 1
    print("check_syms: place ok")
    return 0


INSN_RE = re.compile(r"^\s*([0-9a-f]+):\t[0-9a-f ]+\t(\S+)\s*(.*)$")
FUNC_RE = re.compile(r"^([0-9a-f]+) <(.*)>:$")
# unconditional control transfers: nothing falls through to the next word
TERM_RE = re.compile(r"^(b|ba|blr|bctr|rfi|rfid|hrfid|trap|attn)$")


def cmd_isa(a):
    bad = 0
    for img in a.images:
        out = run([a.objdump, "-d", "-M", "xenon", img])
        fn, prev = "?", None
        n = pad = data = 0
        errs = []
        for line in out.splitlines():
            m = FUNC_RE.match(line)
            if m:
                fn, prev = m.group(2), None
                continue
            m = INSN_RE.match(line)
            if not m:
                continue
            addr, mn, ops = m.groups()
            n += 1
            if mn == ".long":
                word = int(ops.split()[0], 16)
                after_jump = prev is not None and TERM_RE.match(prev)
                if word == 0 and (prev is None or after_jump):
                    pad += 1  # alignment padding, libxenon's exc_base etc.
                elif after_jump:
                    data += 1  # data words between functions
                else:
                    errs.append("%s <%s>: .long 0x%08x after %s" % (addr, fn, word, prev))
                continue
            if mn != "nop":
                prev = mn
        print("check_syms: isa %s: %d words, %d zero padding, %d unreached data, "
              "%d not Xenon instructions" % (img, n, pad, data, len(errs)))
        for e in errs[:40]:
            print("  " + e)
        if len(errs) > 40:
            print("  ... and %d more" % (len(errs) - 40))
        if n == 0:
            print("check_syms: isa %s: no code disassembled" % img)
            bad = 1
        bad |= 1 if errs else 0
    if not bad:
        print("check_syms: isa ok (testxenon3 objdump -M xenon allow-list)")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd")
    k = sub.add_parser("kernels")
    k.add_argument("--tc", required=True)
    k.add_argument("--variant", required=True)
    k.add_argument("--header")
    k.add_argument("objs", nargs="+")
    s = sub.add_parser("sizes")
    s.add_argument("--size-a", required=True)
    s.add_argument("--size-b", required=True)
    s.add_argument("--label-a", default="A")
    s.add_argument("--label-b", default="B")
    s.add_argument("dir_a")
    s.add_argument("dir_b")
    m = sub.add_parser("map")
    m.add_argument("--map", required=True)
    m.add_argument("--libc", help="libc.a whose memcpy the harness must use")
    m.add_argument("--expect-lib", default="memcpy",
                   help="routine that must exist for every variant")
    m.add_argument("variants", nargs="+", help="VARIANT:PREFIXED_LIB_OBJECT")
    p = sub.add_parser("place")
    p.add_argument("--map", required=True)
    p.add_argument("--align", type=int, required=True)
    p.add_argument("--nm", required=True)
    p.add_argument("image")
    i = sub.add_parser("isa")
    i.add_argument("--objdump", required=True,
                   help="testxenon3's xenon-objdump (knows -M xenon)")
    i.add_argument("images", nargs="+")
    a = ap.parse_args()
    if a.cmd == "kernels":
        return cmd_kernels(a)
    if a.cmd == "sizes":
        return cmd_sizes(a)
    if a.cmd == "map":
        return cmd_map(a)
    if a.cmd == "place":
        return cmd_place(a)
    if a.cmd == "isa":
        return cmd_isa(a)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
