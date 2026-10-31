#!/usr/bin/env python3
"""place.py - copy an object file with its code and constant sections aligned.

  place.py --align 16384 in.o out.o

Sets the alignment of every allocated, non-empty .text* and .rodata* section
of in.o (mergeable constant/string pools excepted) to --align bytes and writes
the result to out.o.  The images link the kernel objects (and libprefix.py the
newlib members) through this, so each kernel's code and tables start on an
--align boundary in both variants.  The linker then gives identical code the
same instruction-cache sets, branch-history entries and fetch alignment in A
and B, whatever the size of the objects linked before it: the A/B speedups
measure code generation, not link order (DESIGN.md section 1).  The compiler's
own alignment inside each section is kept, since --align is a multiple of it.

Mergeable pools (SHF_MERGE, .rodata.cst*/.rodata.str*) are left alone: the
linker merges them across all objects, so A and B share those constants.

Only the sh_addralign field of those section headers changes, which is all
objcopy --set-section-alignment does for a relocatable object; patching it
here works for any ELF32/ELF64 object of either byte order and does not
depend on the toolchain's binutils (test16old's 2.32 objcopy lacks that
option).
"""
import argparse
import re
import struct
import sys

SHF_ALLOC = 0x2
SHF_MERGE = 0x10
SHT_NOBITS = 8
SHN_XINDEX = 0xFFFF
NAME = re.compile(r"^\.(text|rodata)(\..*)?$")


def die(msg):
    sys.exit("place.py: " + msg)


def place(align, src, dst):
    """Write src to dst with its sections aligned (also used by libprefix)."""
    if align < 0 or align & (align - 1):
        die("--align %d is not a power of two" % align)
    with open(src, "rb") as f:
        data = bytearray(f.read())
    if align:
        set_alignment(data, align, src)
    with open(dst, "wb") as f:
        f.write(data)


def set_alignment(data, align, path):
    if data[:4] != b"\x7fELF" or data[4] not in (1, 2) or data[5] not in (1, 2):
        die("%s is not an ELF object" % path)
    elf64 = data[4] == 2
    e = "<" if data[5] == 1 else ">"
    if elf64:
        shoff, = struct.unpack_from(e + "Q", data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from(e + "HHH", data, 0x3A)
        shfmt, alfield, alfmt = e + "IIQQQQIIQQ", 48, e + "Q"
    else:
        shoff, = struct.unpack_from(e + "I", data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(e + "HHH", data, 0x2E)
        shfmt, alfield, alfmt = e + "IIIIIIIIII", 32, e + "I"
    if not shoff or not shnum or shstrndx == SHN_XINDEX:
        die("%s: no section headers, or extended section numbering" % path)
    hdrs = [struct.unpack_from(shfmt, data, shoff + i * shentsize)
            for i in range(shnum)]
    strtab = hdrs[shstrndx][4]
    for i, h in enumerate(hdrs):
        name_off, typ, flags, size, cur = h[0], h[1], h[2], h[5], h[8]
        end = data.index(b"\0", strtab + name_off)
        name = data[strtab + name_off:end].decode("ascii", "replace")
        if not (flags & SHF_ALLOC) or flags & SHF_MERGE or not size:
            continue
        if typ == SHT_NOBITS or not NAME.match(name):
            continue
        if align % max(cur, 1):
            die("%s: %s is aligned to %d, which does not divide %d" %
                (path, name, cur, align))
        struct.pack_into(alfmt, data, shoff + i * shentsize + alfield, align)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--align", type=int, required=True,
                    help="bytes, a power of two (0 copies unchanged)")
    ap.add_argument("input")
    ap.add_argument("output")
    a = ap.parse_args()
    place(a.align, a.input, a.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
