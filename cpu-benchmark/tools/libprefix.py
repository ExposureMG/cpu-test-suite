#!/usr/bin/env python3
"""libprefix.py - extract newlib routines from one toolchain, prefixed.

A combined A/B benchmark image links exactly one libc (the linking
toolchain's).  To benchmark *each* toolchain's own newlib build, this tool
pulls the archive members that define the requested routines out of a
toolchain's 32-bit libc.a / libm.a, together with everything they need from
inside those two archives (the closure), links them into one relocatable
object with "ld -m elf32ppc -r", and renames every symbol to <prefix><name>
with "objcopy --prefix-symbols".  Symbols that are still undefined after the
closure (__errno, libgcc helpers, ...) are renamed back to their real names,
so the final link binds them to the linking toolchain's runtime.  With
--align N every member's code and tables start on an N-byte boundary inside
the object (tools/place.py), as the kernels' do in the images; --shift S
moves every member's code S bytes past that boundary (layout shifts, make
SHIFTS=...), its tables stay on it.

    tools/libprefix.py --tc <toolchain root> --prefix xenon3_lib_ \\
        --header include/bench_lib.h -o build/xenon3/lib_xenon3.o

The routine list comes from BENCH_LIB_SYMBOLS in the header (--header), from
the command line, or both.  The tool fails unless every requested
<prefix><sym> is defined in the output and no unprefixed global definition is
left (a global "memcpy" in the output would clash with the real one).  It
prints the closure (members pulled and why) and the externals left.

Only the Python standard library is used.  Archives are parsed directly (GNU
ar format), so duplicate member names cannot overwrite each other.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

import place

# Never pulled from the archive: they belong to the C runtime of the final
# image (one errno, one reentrancy structure).  They stay undefined and keep
# their real names.
DEFAULT_EXTERNALS = ("__errno", "_impure_ptr", "_global_impure_ptr")

# Search order: the benchmark links "-lxenon -lm" and gcc appends -lc, so
# libm is searched before libc, as the real link does.
ARCHIVES = ("libm.a", "libc.a")

DEFINED_TYPES = set("TDBRSGVWCAIiu")  # nm types of global/weak definitions
UNDEF_TYPES = set("Uwv")              # undefined (strong or weak)


def die(msg):
    sys.stderr.write("libprefix: error: %s\n" % msg)
    sys.exit(1)


def run(cmd, cwd=None):
    """Run a tool, return stdout; die with its stderr on failure."""
    try:
        p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, universal_newlines=True)
    except OSError as e:
        die("cannot run %s: %s" % (cmd[0], e))
    if p.returncode != 0:
        die("%s failed (exit %d):\n%s%s" % (" ".join(cmd), p.returncode,
                                            p.stdout, p.stderr))
    for line in p.stderr.splitlines():  # warnings of a successful run
        sys.stderr.write("libprefix: %s: %s\n" % (os.path.basename(cmd[0]), line))
    return p.stdout


# ---------------------------------------------------------------------------
# GNU ar reader

def read_archive(path):
    """Yield (member name, bytes) for every regular member of a GNU ar file."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"!<arch>\n"):
        die("%s: not an ar archive" % path)
    pos = 8
    longnames = b""
    members = []
    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        if hdr[58:60] != b"`\n":
            die("%s: bad member header at offset %d" % (path, pos))
        name = hdr[0:16].decode("ascii", "replace").rstrip(" ")
        try:
            size = int(hdr[48:58].decode("ascii").strip())
        except ValueError:
            die("%s: bad member size at offset %d" % (path, pos))
        body = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if name in ("/", "/SYM64/", "__.SYMDEF", "__.SYMDEF SORTED"):
            continue  # symbol index
        if name == "//":
            longnames = body
            continue
        if name.startswith("/") and name[1:].isdigit():
            off = int(name[1:])
            end = longnames.find(b"/\n", off)
            if end < 0:
                end = longnames.find(b"\n", off)
            name = longnames[off:end].decode("ascii", "replace")
        elif name.endswith("/"):
            name = name[:-1]
        members.append((name, body))
    return members


# ---------------------------------------------------------------------------
# Symbols

class Member(object):
    def __init__(self, archive, index, name, path):
        self.archive = archive  # "libc.a" / "libm.a"
        self.index = index      # position in the archive
        self.name = name        # member name inside the archive
        self.path = path        # extracted file
        self.defs = set()       # global / weak symbols defined
        self.strong = set()     # the subset that is not weak
        self.undefs = set()     # strong undefined references
        self.wundefs = set()    # weak undefined references


def nm_symbols(nm, paths):
    """Return {path: [(name, type)]} from nm -P -A (POSIX format)."""
    out = {}
    chunk = 200
    for i in range(0, len(paths), chunk):
        part = paths[i:i + chunk]
        text = run([nm, "-P", "-A"] + part)
        for line in text.splitlines():
            # "<file>: <name> <type> [<value> [<size>]]"
            head, sep, rest = line.partition(": ")
            if not sep:
                continue
            fields = rest.split()
            if len(fields) < 2:
                continue
            out.setdefault(head, []).append((fields[0], fields[1]))
    return out


def load_members(tc, tools, work):
    members = []
    for arname in ARCHIVES:
        apath = os.path.join(tc, "xenon", "lib", "32", arname)
        if not os.path.isfile(apath):
            die("archive not found: %s" % apath)
        sub = os.path.join(work, arname.replace(".", "_"))
        os.makedirs(sub)
        for idx, (name, body) in enumerate(read_archive(apath)):
            if not name.endswith(".o"):
                continue
            path = os.path.join(sub, "%04d_%s" % (idx, name))
            with open(path, "wb") as f:
                f.write(body)
            members.append(Member(arname, idx, name, path))
    syms = nm_symbols(tools["nm"], [m.path for m in members])
    for m in members:
        for name, t in syms.get(m.path, []):
            if t in UNDEF_TYPES:
                (m.undefs if t == "U" else m.wundefs).add(name)
            elif t in DEFINED_TYPES:
                m.defs.add(name)
                if t not in "VWvw":
                    m.strong.add(name)
    return members


def closure(members, wanted, externals):
    """Pull members like the linker does; return (pulled, why, missing, ext)."""
    providers = {}
    for m in members:  # members are in search order (libm, then libc)
        for s in m.defs:
            providers.setdefault(s, []).append(m)
    pulled = []
    why = {}
    defined = set()
    strong_owner = {}
    queue = list(wanted)
    seen = set()
    missing = []
    while queue:
        s = queue.pop(0)
        if s in seen:
            continue
        seen.add(s)
        if s in defined or s in externals:
            continue
        cands = providers.get(s)
        if not cands:
            missing.append(s)
            continue
        # Prefer a strong definition, in search order.
        strong = [m for m in cands if s in m.strong]
        m = (strong or cands)[0]
        for d in sorted(m.strong):
            if d in strong_owner and strong_owner[d] is not m:
                die("symbol %s is defined by both %s and %s" %
                    (d, strong_owner[d].name, m.name))
            strong_owner[d] = m
        pulled.append(m)
        why[m] = s
        defined |= m.defs
        queue.extend(sorted(m.undefs))
    ext = set()
    for m in pulled:
        ext |= (m.undefs | m.wundefs)
    ext -= defined
    return pulled, why, missing, ext


# ---------------------------------------------------------------------------

def symbols_from_header(path):
    with open(path) as f:
        text = f.read()
    m = re.search(r"#define\s+BENCH_LIB_SYMBOLS\s*\\?\s*((?:\"[^\"]*\"\s*\\?\s*)+)",
                  text)
    if not m:
        die("%s: BENCH_LIB_SYMBOLS not found" % path)
    words = " ".join(re.findall(r"\"([^\"]*)\"", m.group(1))).split()
    if not words:
        die("%s: BENCH_LIB_SYMBOLS is empty" % path)
    return words


def parse_nm_output(text):
    res = []
    for line in text.splitlines():
        f = line.split()
        if len(f) >= 2:
            res.append((f[0], f[1]))
    return res


def main():
    ap = argparse.ArgumentParser(
        description="Extract newlib routines from a toolchain, prefixed.")
    ap.add_argument("--tc", required=True, help="toolchain root")
    ap.add_argument("--prefix", required=True,
                    help="symbol prefix, e.g. xenon3_lib_")
    ap.add_argument("--header", help="read BENCH_LIB_SYMBOLS from this file")
    ap.add_argument("-o", "--output", required=True,
                    help="output relocatable object")
    ap.add_argument("--work", help="work directory (default: <output>.d)")
    ap.add_argument("--extern", action="append", default=[],
                    help="extra symbol never to pull (repeatable)")
    ap.add_argument("--align", type=int, default=0,
                    help="start each member's .text/.rodata on this boundary")
    ap.add_argument("--shift", type=int, default=0,
                    help="start each member's code this many bytes after the "
                         "--align boundary (tools/place.py --shift)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the work directory")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="print only the summary")
    ap.add_argument("symbols", nargs="*", help="routines to provide")
    a = ap.parse_args()

    if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", a.prefix):
        die("bad prefix %r" % a.prefix)
    wanted = []
    if a.header:
        wanted += symbols_from_header(a.header)
    wanted += a.symbols
    wanted = list(dict.fromkeys(wanted))  # dedupe, keep order
    if not wanted:
        die("no symbols requested (use --header or list them)")
    externals = set(DEFAULT_EXTERNALS) | set(a.extern)
    for s in wanted:
        if s in externals:
            die("%s is requested but treated as an external" % s)

    tools = {}
    for t in ("nm", "ld", "objcopy", "objdump", "size"):
        p = os.path.join(a.tc, "bin", "xenon-" + t)
        if not os.access(p, os.X_OK):
            die("tool not found: %s" % p)
        tools[t] = p

    out = a.output
    work = a.work or out + ".d"
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)

    members = load_members(a.tc, tools, work)
    pulled, why, missing, ext = closure(members, wanted, externals)
    if missing:
        die("not defined in %s libc/libm: %s" % (a.tc, " ".join(missing)))

    label = os.path.basename(os.path.normpath(a.tc))
    nlibc = sum(1 for m in pulled if m.archive == "libc.a")
    print("libprefix: %s -> %s (prefix %s)" % (label, out, a.prefix))
    print("  requested (%d): %s" % (len(wanted), " ".join(wanted)))
    print("  closure: %d members (libc %d, libm %d)" %
          (len(pulled), nlibc, len(pulled) - nlibc))
    if not a.quiet:
        for m in pulled:
            print("    %-6s %-28s for %-22s defines %s" %
                  (m.archive, m.name, why[m], " ".join(sorted(m.defs))))

    # 1. one relocatable object, each member on an --align boundary
    paths = []
    for i, m in enumerate(pulled):
        dst = os.path.join(work, "placed-%03d.o" % i)
        place.place(a.align, m.path, dst, a.shift)
        paths.append(dst)
    combined = os.path.join(work, "combined.o")
    run([tools["ld"], "-m", "elf32ppc", "-r", "-o", combined] + paths)
    # 2. prefix everything (defined and undefined)
    prefixed = os.path.join(work, "prefixed.o")
    run([tools["objcopy"], "--prefix-symbols=" + a.prefix, combined, prefixed])
    # 3. rename back what is still undefined
    und = []
    for line in run([tools["nm"], "-u", prefixed]).splitlines():
        f = line.split()
        if f:
            und.append(f[-1])
    redefs = []
    for s in und:
        if not s.startswith(a.prefix):
            die("undefined symbol %s was not prefixed" % s)
        redefs.append((s, s[len(a.prefix):]))
    tmp = out + ".tmp"
    if redefs:
        mapfile = os.path.join(work, "redefine.txt")
        with open(mapfile, "w") as f:
            for old, new in redefs:
                f.write("%s %s\n" % (old, new))
        run([tools["objcopy"], "--redefine-syms=" + mapfile, prefixed, tmp])
    else:
        shutil.copyfile(prefixed, tmp)

    # 4. verify the output
    nm_out = parse_nm_output(run([tools["nm"], "-P", tmp]))
    defined = {}
    undefined = set()
    for name, t in nm_out:
        if t in UNDEF_TYPES:
            undefined.add(name)
        elif t in DEFINED_TYPES:
            defined[name] = t
    errors = []
    for s in wanted:
        p = a.prefix + s
        if p not in defined:
            errors.append("%s is not defined in the output" % p)
        elif defined[p] not in "TW":
            errors.append("%s is defined as type %s, not code" % (p, defined[p]))
    for name in sorted(defined):
        if not name.startswith(a.prefix):
            errors.append("unprefixed global definition %s leaks" % name)
    for name in sorted(undefined):
        if name.startswith(a.prefix):
            errors.append("undefined prefixed symbol %s" % name)
    expected_ext = set(ext)
    if undefined != expected_ext:
        errors.append("externals differ from the closure: %s vs %s" %
                      (" ".join(sorted(undefined)), " ".join(sorted(expected_ext))))
    # Relocations that would need a shared small-data base are not expected
    # (newlib is built without -msdata); refuse them rather than guess.
    relocs = run([tools["objdump"], "-r", tmp])
    sda = sorted(set(re.findall(r"\bR_PPC_(?:EMB_)?SDA\w*", relocs)))
    if sda:
        errors.append("small-data relocations present: %s" % " ".join(sda))
    if errors:
        for e in errors:
            sys.stderr.write("libprefix: error: %s\n" % e)
        os.remove(tmp)
        sys.exit(1)

    text = 0
    for line in run([tools["size"], "-A", tmp]).splitlines():
        f = line.split()
        if len(f) >= 2 and f[0].startswith(".text") and f[1].isdigit():
            text += int(f[1])
    os.replace(tmp, out)
    print("  provides (%d): %s" % (len(wanted),
                                    " ".join(a.prefix + s for s in wanted)))
    print("  externals left (%d): %s" % (len(undefined),
                                         " ".join(sorted(undefined)) or "-"))
    print("  output: %s, %d global symbols, .text %d bytes" %
          (out, len(defined), text))
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
