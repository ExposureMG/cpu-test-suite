#!/usr/bin/env python3
"""place.py - copy an object file with its code and constant sections aligned.

  place.py --align 16384 in.o out.o
  place.py --align 16384 --shift 4 --rename xenon3_=xenon3_s4_ in.o out.o

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

Without --shift/--rename only the sh_addralign field of those section headers
changes, which is all objcopy --set-section-alignment does for a relocatable
object; patching it here works for any ELF32/ELF64 object of either byte
order and does not depend on the toolchain's binutils (test16old's 2.32
objcopy lacks that option).

Layout shifts (make SHIFTS=..., DESIGN.md section 1):

  --shift N   also puts N bytes of padding (no-ops) in front of the contents
              of every placed .text* section, so the code starts at an
              --align boundary + N bytes.  Symbols defined in the section
              move up by N, relocations applied to it move with their
              targets, and every relocation that refers to the section
              through its section symbol gets N added to its addend (code,
              .eh_frame, debug info).  .rodata* stays on the boundary: tables
              may need their alignment (lvx ignores the low address bits).
              N is a multiple of 4 below --align.  An object without code
              (table.o) is only renamed.
  --rename OLD=NEW
              renames every symbol whose name starts with OLD (or contains
              ".OLD", as ASan's __odr_asan.<name> does) to start with NEW, so
              several shifted copies of one kernel object can be linked into
              one image (xenon3_k_crc32 -> xenon3_s4_k_crc32, and its
              reference xenon3_lib_memcpy -> xenon3_s4_lib_memcpy).

The new section contents and string table are appended to the file and
their headers repointed, so all other offsets stay valid.  Only RELA
relocation sections are supported (PowerPC and x86-64 use nothing else).
"""
import argparse
import re
import struct
import sys

SHF_ALLOC = 0x2
SHF_MERGE = 0x10
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9
STT_SECTION = 3
SHN_XINDEX = 0xFFFF
NAME = re.compile(r"^\.(text|rodata)(\..*)?$")
CODE = re.compile(r"^\.text(\..*)?$")

EM_386, EM_PPC, EM_PPC64, EM_X86_64 = 3, 20, 21, 62


def die(msg):
    sys.exit("place.py: " + msg)


def place(align, src, dst, shift=0, rename=None):
    """Write src to dst with its sections aligned (also used by libprefix).

    shift: bytes of padding in front of every placed code section.
    rename: (old, new) symbol prefix pair, or None."""
    if align < 0 or align & (align - 1):
        die("--align %d is not a power of two" % align)
    if shift and (not align or shift % 4 or not 0 < shift < align):
        die("--shift %d must be a multiple of 4 below --align %d" % (shift, align))
    with open(src, "rb") as f:
        data = bytearray(f.read())
    if align or rename:
        data = Elf(data, src).edit(align, shift, rename)
    with open(dst, "wb") as f:
        f.write(data)


class Elf(object):
    def __init__(self, data, path):
        if data[:4] != b"\x7fELF" or data[4] not in (1, 2) or data[5] not in (1, 2):
            die("%s is not an ELF object" % path)
        self.data, self.path = data, path
        self.elf64 = data[4] == 2
        e = self.e = "<" if data[5] == 1 else ">"
        self.machine, = struct.unpack_from(e + "H", data, 0x12)
        if self.elf64:
            self.shoff, = struct.unpack_from(e + "Q", data, 0x28)
            self.shentsize, shnum, shstrndx = struct.unpack_from(e + "HHH", data, 0x3A)
            self.shfmt = e + "IIQQQQIIQQ"
        else:
            self.shoff, = struct.unpack_from(e + "I", data, 0x20)
            self.shentsize, shnum, shstrndx = struct.unpack_from(e + "HHH", data, 0x2E)
            self.shfmt = e + "IIIIIIIIII"
        if not self.shoff or not shnum or shstrndx == SHN_XINDEX:
            die("%s: no section headers, or extended section numbering" % path)
        # name, type, flags, addr, offset, size, link, info, addralign, entsize
        self.sh = [list(struct.unpack_from(self.shfmt, data, self.shoff + i * self.shentsize))
                   for i in range(shnum)]
        strtab = self.sh[shstrndx][4]
        self.names = []
        for h in self.sh:
            end = data.index(b"\0", strtab + h[0])
            self.names.append(data[strtab + h[0]:end].decode("ascii", "replace"))

    def put_sh(self, i):
        struct.pack_into(self.shfmt, self.data, self.shoff + i * self.shentsize, *self.sh[i])

    def append(self, blob):
        """Append blob (16-byte aligned) to the file; return its offset."""
        while len(self.data) % 16:
            self.data.append(0)
        off = len(self.data)
        self.data += blob
        return off

    def nop(self):
        if self.machine in (EM_PPC, EM_PPC64):
            return struct.pack(self.e + "I", 0x60000000)  # nop = ori 0,0,0
        if self.machine in (EM_386, EM_X86_64):
            return b"\x90"
        return b"\0"

    def edit(self, align, shift, rename):
        shifted = set()
        for i, h in enumerate(self.sh):
            typ, flags, size, cur = h[1], h[2], h[5], h[8]
            if not align or not (flags & SHF_ALLOC) or flags & SHF_MERGE or not size:
                continue
            if typ == SHT_NOBITS or not NAME.match(self.names[i]):
                continue
            if align % max(cur, 1):
                die("%s: %s is aligned to %d, which does not divide %d" %
                    (self.path, self.names[i], cur, align))
            h[8] = align
            if shift and CODE.match(self.names[i]):
                nop = self.nop()
                pad = nop * (shift // len(nop))
                h[4] = self.append(pad + self.data[h[4]:h[4] + size])
                h[5] = size + shift
                shifted.add(i)
            self.put_sh(i)
        if shifted or rename:
            self.symbols(shifted, shift, rename)
        return self.data

    def symbols(self, shifted, shift, rename):
        e, d = self.e, self.data
        symtabs = [i for i, h in enumerate(self.sh) if h[1] == SHT_SYMTAB]
        if len(symtabs) != 1:
            die("%s: %d symbol tables" % (self.path, len(symtabs)))
        st = symtabs[0]
        sym_off, sym_size, strndx = self.sh[st][4], self.sh[st][5], self.sh[st][6]
        if self.elf64:
            symfmt, entsz, vi = e + "IBBHQQ", 24, 4   # name info other shndx value size
        else:
            symfmt, entsz, vi = e + "IIIBBH", 16, 1   # name value size info other shndx
        str_off, str_size = self.sh[strndx][4], self.sh[strndx][5]
        newstr = bytearray(d[str_off:str_off + str_size])
        section_syms = set()  # STT_SECTION symbols of the shifted sections
        renamed = 0
        for n in range(sym_size // entsz):
            o = sym_off + n * entsz
            s = list(struct.unpack_from(symfmt, d, o))
            name = s[0]
            info, shndx = (s[1], s[3]) if self.elf64 else (s[3], s[5])
            if shndx in shifted:
                if info & 0xF == STT_SECTION:
                    section_syms.add(n)
                else:
                    s[vi] += shift
            if rename and name:
                end = d.index(b"\0", str_off + name)
                old = d[str_off + name:end].decode("ascii", "replace")
                new = rename_symbol(old, rename)
                if new != old:
                    s[0] = len(newstr)
                    newstr += new.encode("ascii") + b"\0"
                    renamed += 1
            struct.pack_into(symfmt, d, o, *s)
        if rename:
            if not renamed:
                die("%s: no symbol starts with %s" % (self.path, rename[0]))
            self.sh[strndx][4] = self.append(bytes(newstr))
            self.sh[strndx][5] = len(newstr)
            self.put_sh(strndx)
        if not shifted:
            return
        if self.elf64:
            rfmt, rsz, symshift = e + "QQq", 24, 32
        else:
            rfmt, rsz, symshift = e + "IIi", 12, 8
        for i, h in enumerate(self.sh):
            if h[1] == SHT_REL:
                die("%s: %s: REL relocations are not supported" % (self.path, self.names[i]))
            if h[1] != SHT_RELA:
                continue
            if h[6] != st:
                die("%s: %s does not use the symbol table" % (self.path, self.names[i]))
            target = h[7]
            for k in range(h[5] // rsz):
                o = h[4] + k * rsz
                off, inf, add = struct.unpack_from(rfmt, d, o)
                if target in shifted:
                    off += shift
                if (inf >> symshift) in section_syms:
                    add += shift
                struct.pack_into(rfmt, d, o, off, inf, add)


def rename_symbol(name, rename):
    old, new = rename
    if name.startswith(old):
        return new + name[len(old):]
    k = name.find("." + old)
    if k >= 0:
        return name[:k + 1] + new + name[k + 1 + len(old):]
    return name


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--align", type=int, required=True,
                    help="bytes, a power of two (0 copies unchanged)")
    ap.add_argument("--shift", type=int, default=0,
                    help="bytes of no-op padding before each code section "
                         "(a multiple of 4 below --align)")
    ap.add_argument("--rename", metavar="OLD=NEW",
                    help="rename symbols starting with OLD to start with NEW")
    ap.add_argument("input")
    ap.add_argument("output")
    a = ap.parse_args()
    rename = None
    if a.rename:
        old, sep, new = a.rename.partition("=")
        if not sep or not old or not new:
            die("--rename wants OLD=NEW")
        rename = (old, new)
    place(a.align, a.input, a.output, a.shift, rename)
    return 0


if __name__ == "__main__":
    sys.exit(main())
