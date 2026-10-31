#!/usr/bin/env python3
"""mkrefs.py - generate src/harness/refs.h, the reference results the images
check every kernel against (`make refs`), or verify that it is current.

  mkrefs.py --be build/qemu/qemu-run.txt --host build/refs/host.log \\
            --known tools/qemu/known-failures.txt -o src/harness/refs.h
  mkrefs.py --verify src/harness/refs.h

Sources:
  --be     the qemu-ppc big-endian run of BOTH toolchains' kernels
           (qemu-check; its "@R id flag tol A B status" lines).  This is the
           reference: big-endian, like the console, and A and B agree.
  --host   the CSV log of a host bench-host run: the kernel descriptors
           (EXACT/APPROX, tolerance, units, arena bytes; recorded as a
           fingerprint, so the harness ignores a reference recorded for a
           different descriptor) and the little-endian results (a result that
           differs there is marked big-endian-only and not checked on a
           little-endian host; by design that is bswap).
  --known  qemu-check's known toolchain defects.  For a kernel listed there,
           the variant whose big-endian result equals the host's is taken.

Each entry also records a hash of the kernel's source and of the shared
headers; --verify (part of `make check`) fails when one of them changed after
refs.h was generated, without needing qemu.
"""
import argparse
import hashlib
import math
import os
import re
import struct
import sys

HEADERS = ["include/bench.h", "include/bench_kernel.h", "include/bench_lib.h"]
LINE = re.compile(r'^\t\{ "([a-z0-9_]+)",.*/\* (\S+) ([0-9a-f]{16}) \*/$')


def die(msg):
    sys.exit("mkrefs: " + msg)


def bits_to_double(b):
    return struct.unpack(">d", struct.pack(">Q", b))[0]


def double_to_bits(d):
    return struct.unpack(">Q", struct.pack(">d", d))[0]


def kernel_source(root, kid):
    for cat in ("int", "br", "mem", "fp", "vec", "mix", "lib"):
        p = os.path.join(root, "src", "kernels", "%s_%s.c" % (cat, kid))
        if os.path.exists(p):
            return p
    return None


def source_hash(root, kid):
    """sha1 of the kernel's source and the shared headers (16 hex digits)."""
    src = kernel_source(root, kid)
    if not src:
        return None, None
    h = hashlib.sha1()
    for p in [src] + [os.path.join(root, x) for x in HEADERS]:
        with open(p, "rb") as f:
            h.update(f.read())
        h.update(b"\0")
    return os.path.basename(src), h.hexdigest()[:16]


def read_be(path):
    """id -> (exact, tol_bits, a_bits, b_bits, status) in run order."""
    out = {}
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) == 7 and p[0] == "@R":
                out[p[1]] = (p[2] == "EXACT", int(p[3], 16), int(p[4], 16),
                             int(p[5], 16), int(p[6], 16))
    if not out:
        die("%s has no @R lines (run make qemu-check)" % path)
    return out


def read_host(path):
    """id -> descriptor and A result from a bench-host CSV log."""
    out = {}
    with open(path) as f:
        for line in f:
            p = line.rstrip("\n").split(",")
            if p[0] != "kernel" or len(p) < 28 or p[3] != "A":
                continue
            name, check, tol, units, arena, result = p[1], p[6], p[7], p[8], p[10], p[20]
            if not result:
                die("%s: no host result for %s" % (path, name))
            out[name] = {"exact": check == "EXACT", "tol": float(tol),
                         "units": int(units), "arena": int(arena),
                         "result": float(result)}
    if not out:
        die("%s has no kernel lines" % path)
    return out


def read_known(path):
    out = {}
    if not path:
        return out
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) >= 4 and not line.startswith("#"):
                out[p[0]] = (int(p[1], 16), int(p[2], 16), int(p[3], 16))
    return out


def same(exact, tol, a, b):
    if exact:
        return double_to_bits(a) == double_to_bits(b)
    if a == b:
        return True
    if not (math.isfinite(a) and math.isfinite(b)):
        return False
    return abs(a - b) <= tol * max(abs(a), abs(b))


def generate(a):
    root = os.getcwd()
    be = read_be(a.be)
    host = read_host(a.host)
    known = read_known(a.known)
    rows, notes = [], []
    for kid, (exact, tolb, ab, bb, status) in be.items():
        d = host.get(kid)
        if not d:
            die("%s is in %s but not in %s" % (kid, a.be, a.host))
        tol = bits_to_double(tolb)
        if d["exact"] != exact or (not exact and abs(d["tol"] - tol) > 1e-6 * tol):
            die("%s: descriptor of the qemu run and the host build differ" % kid)
        if status == 0:
            ref = ab
        elif known.get(kid) == (status, ab, bb):
            # known toolchain defect: the variant that agrees with the host
            if same(exact, tol, bits_to_double(ab), d["result"]):
                ref, who = ab, "A"
            elif same(exact, tol, bits_to_double(bb), d["result"]):
                ref, who = bb, "B"
            else:
                notes.append("%s: no reference (known defect, neither variant "
                             "matches the host)" % kid)
                continue
            notes.append("%s: known defect %s, reference from %s" %
                         (kid, "%04x" % status, who))
        else:
            die("%s failed in the qemu run (status %04x) and is not a known "
                "defect: fix it before recording references" % (kid, status))
        be_only = not same(exact, tol, bits_to_double(ref), d["result"])
        if be_only:
            notes.append("%s: little-endian result differs, big-endian only" % kid)
        src, h = source_hash(root, kid)
        if not src:
            die("no source file for %s" % kid)
        rows.append((kid, ref, exact, be_only, tolb, d["units"], d["arena"], src, h))

    out = []
    out.append("/*")
    out.append(" * refs.h - reference results of every kernel, checked by runner.c.")
    out.append(" *")
    out.append(" * GENERATED by tools/mkrefs.py (`make refs`): do not edit.  The values")
    out.append(" * are the IEEE double bits of run()'s result in the qemu-ppc big-endian")
    out.append(" * run of both toolchains' kernels (make qemu-check, 32-bit G4 flags), where")
    out.append(" * A and B agree; the fingerprint (flags, tolerance, units, arena bytes) is")
    out.append(" * the host build's descriptor.  A result that differs on a little-endian")
    out.append(" * host is BENCH_REF_BE_ONLY.  The comment holds a hash of the kernel source")
    out.append(" * and the shared headers for `make check` (tools/mkrefs.py --verify).")
    for n in notes:
        out.append(" *   %s" % n)
    out.append(" */")
    out.append("#ifndef BENCH_REFS_H")
    out.append("#define BENCH_REFS_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("#define BENCH_REF_EXACT 1u   /* EXACT kernel: compare the bits         */")
    out.append("#define BENCH_REF_BE_ONLY 2u /* the result depends on the byte order    */")
    out.append("")
    out.append("typedef struct bench_ref {")
    out.append("\tconst char *name;")
    out.append("\tuint64_t bits;      /* run()'s result, IEEE double bits       */")
    out.append("\tuint32_t flags;     /* BENCH_REF_*                             */")
    out.append("\tuint64_t tol_bits;  /* fingerprint: tolerance (double bits),   */")
    out.append("\tuint32_t units;     /*   units and arena_bytes of the kernel   */")
    out.append("\tuint32_t arena_bytes;")
    out.append("} bench_ref;")
    out.append("")
    out.append("static const bench_ref bench_refs[] = {")
    for kid, ref, exact, be_only, tolb, units, arena, src, h in rows:
        fl = " | ".join(x for x, on in (("BENCH_REF_EXACT", exact),
                                         ("BENCH_REF_BE_ONLY", be_only)) if on) or "0"
        out.append('\t{ "%s", 0x%016xull, %s, 0x%016xull, %du, %du }, /* %s %s */' %
                   (kid, ref, fl, tolb, units, arena, src, h))
    out.append("};")
    out.append("")
    out.append("#endif /* BENCH_REFS_H */")
    text = "\n".join(out) + "\n"
    tmp = a.output + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, a.output)
    print("mkrefs: %s: %d references" % (a.output, len(rows)))
    for n in notes:
        print("  " + n)


def verify(path):
    root = os.getcwd()
    seen, bad = 0, []
    with open(path) as f:
        for line in f:
            m = LINE.match(line.rstrip("\n"))
            if not m:
                continue
            seen += 1
            kid, src, h = m.groups()
            s2, h2 = source_hash(root, kid)
            if s2 != src or h2 != h:
                bad.append(kid)
    if not seen:
        die("%s holds no references" % path)
    if bad:
        die("%s is older than the sources of: %s\n"
            "  run `make refs` (needs qemu-ppc) to record new reference results"
            % (path, " ".join(bad)))
    print("mkrefs: %s: %d references, sources unchanged" % (path, seen))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--be")
    ap.add_argument("--host")
    ap.add_argument("--known")
    ap.add_argument("-o", "--output")
    ap.add_argument("--verify", metavar="REFS_H")
    a = ap.parse_args()
    if a.verify:
        verify(a.verify)
    elif a.be and a.host and a.output:
        generate(a)
    else:
        ap.error("need --be, --host and -o, or --verify")
    return 0


if __name__ == "__main__":
    sys.exit(main())
