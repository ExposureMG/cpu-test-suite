#!/usr/bin/env python3
"""Create a FAT32 disk image (sparse file) like a USB stick, optionally pre-populated with deterministic files.

  fat_mkimage.py OUT.img [--size 2200M] [--cluster 32768] [--layout superfloppy|mbr] [--populate stick] [--seed N]
                         [--manifest OUT.json] [--label NAME] [--fats 2] [--reserved N | --align SECTORS]

Layouts: "superfloppy" (no partition table, the boot sector is sector 0 - like the user's stick) or "mbr" (one FAT32 LBA
partition, type 0x0C, starting at --part-start, default 2048). FAT32 needs at least 65525 clusters, so with 32 KB
clusters the image must be about 2.1 GB or more; the file is sparse (only metadata and file data take disk space).

By default the data region starts on a 32 MB boundary from the start of the volume, as on the user's stick (reserved
sectors fill the gap); --align / --reserved change that.

--populate stick creates a deterministic tree (names, sizes and contents depend only on --seed): ~40 root entries
(8.3 and long names, a volume label), nested directories, files of 0..300 KB, one fragmented file, and free holes in the
allocation so new clusters land between existing data (as cluster 13 did on the stick). --manifest writes every
pre-existing entry (path, raw 32-byte short entry, first cluster, cluster chain, size, sha256) for fat_check.py
--manifest, which verifies they are byte-identical after a run.

Only regular files are written; never point OUT at a block device (refused).
"""
import argparse
import hashlib
import json
import os
import random
import stat
import struct
import sys

SECTOR = 512
EOC = 0x0FFFFFFF


def parse_size(s):
    s = s.strip().upper()
    mult = 1
    for suf, m in (("K", 1 << 10), ("M", 1 << 20), ("G", 1 << 30)):
        if s.endswith(suf):
            mult, s = m, s[:-1]
    return int(float(s) * mult)


def short_name_ok(name):
    if name in (".", ".."):
        return True
    if name != name.upper():
        return False
    base, _, ext = name.partition(".")
    ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&'()@^`{}")
    return 0 < len(base) <= 8 and len(ext) <= 3 and "." not in ext and set(base) <= ok and set(ext) <= ok


def lfn_checksum(short11):
    s = 0
    for b in short11:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


class Volume:
    def __init__(self, a):
        self.a = a
        self.bps = SECTOR
        self.spc = a.cluster // SECTOR
        if self.spc < 1 or self.spc > 128 or self.spc & (self.spc - 1):
            sys.exit("cluster size must be 512 B .. 64 KB, a power of two")
        size = parse_size(a.size)
        self.part_start = a.part_start if a.layout == "mbr" else 0
        self.total = size // SECTOR - self.part_start          # sectors in the volume
        self.nfats = a.fats
        # FAT size: iterate (clusters depend on the FAT size)
        reserved = a.reserved if a.reserved else 32
        spf = 1
        for _ in range(20):
            if not a.reserved:
                reserved = 32
            data_start = reserved + self.nfats * spf
            if not a.reserved:
                align = a.align
                data_start = ((data_start + align - 1) // align) * align
                reserved = data_start - self.nfats * spf
            clusters = (self.total - data_start) // self.spc
            need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            if need <= spf:
                break
            spf = need
        self.reserved, self.spf = reserved, spf
        self.data_start = reserved + self.nfats * spf
        self.clusters = (self.total - self.data_start) // self.spc
        self.last = self.clusters + 1
        if self.clusters < 65525:
            sys.exit("%d clusters: too few for FAT32 (need >= 65525; use a bigger --size or smaller --cluster)" % self.clusters)
        self.fat = {}            # cluster -> value (only non-free)
        self.data = {}           # absolute volume sector -> bytes (512)
        self.next_free = 2
        self.rng = random.Random(a.seed)
        self.manifest = []

    # ---- allocation ----
    def alloc_run(self, n, hole_after=0):
        c0 = self.next_free
        for i in range(n):
            self.fat[c0 + i] = c0 + i + 1 if i < n - 1 else EOC
        self.next_free = c0 + n + hole_after
        return list(range(c0, c0 + n))

    def alloc_fragmented(self, n):
        chain = []
        while len(chain) < n:
            k = min(n - len(chain), self.rng.randint(1, 3))
            run = list(range(self.next_free, self.next_free + k))
            self.next_free += k + self.rng.randint(1, 2)          # leave a hole
            chain += run
        for i, c in enumerate(chain):
            self.fat[c] = chain[i + 1] if i < len(chain) - 1 else EOC
        return chain

    def csec(self, c):
        return self.data_start + (c - 2) * self.spc

    def write_chain(self, chain, payload):
        cb = self.spc * SECTOR
        payload = payload + b"\0" * (len(chain) * cb - len(payload))
        for i, c in enumerate(chain):
            blob = payload[i * cb:(i + 1) * cb]
            for s in range(self.spc):
                sec = blob[s * SECTOR:(s + 1) * SECTOR]
                if any(sec):
                    self.data[self.csec(c) + s] = sec

    # ---- directory entries ----
    def short_for(self, name, used):
        if short_name_ok(name):
            base, _, ext = name.partition(".")
            return (base.ljust(8) + ext.ljust(3)).encode()
        up = "".join(ch for ch in name.upper() if ch.isalnum())
        base, ext = (up[:6] or "FILE"), ""
        if "." in name:
            ext = "".join(ch for ch in name.rsplit(".", 1)[1].upper() if ch.isalnum())[:3]
        for n in range(1, 1000):
            tail = "~%d" % n
            cand = (base[:8 - len(tail)] + tail).ljust(8) + ext.ljust(3)
            if cand not in used:
                return cand.encode()
        sys.exit("no short name for " + name)

    def entries_for(self, name, attr, cluster, size, used, stamp):
        short = self.short_for(name, used)
        used.add(short.decode())
        t, d = stamp
        ent = struct.pack("<11sBBBHHHHHHHI", short, attr, 0, 0, t, d, d, cluster >> 16, t, d, cluster & 0xFFFF, size)
        out = []
        if not short_name_ok(name):
            u = name.encode("utf-16-le")
            chars = [u[i:i + 2] for i in range(0, len(u), 2)]
            if len(chars) % 13:
                chars.append(b"\0\0")
            while len(chars) % 13:
                chars.append(b"\xff\xff")
            parts = [chars[i:i + 13] for i in range(0, len(chars), 13)]
            ck = lfn_checksum(short)
            for seq in range(len(parts), 0, -1):
                p = parts[seq - 1]
                order = seq | (0x40 if seq == len(parts) else 0)
                out.append(struct.pack("<B10sBBB12sH4s", order, b"".join(p[0:5]), 0x0F, 0, ck, b"".join(p[5:11]), 0,
                                       b"".join(p[11:13])))
        out.append(ent)
        return out, ent

    def stamp(self):
        # 2024-03-05 12:34:56 + jitter (deterministic)
        y, mo, d = 2024, 3, 1 + self.rng.randint(0, 27)
        h, mi, s = self.rng.randint(0, 23), self.rng.randint(0, 59), self.rng.randint(0, 29)
        return (h << 11) | (mi << 5) | s, ((y - 1980) << 9) | (mo << 5) | d

    def content(self, path, size):
        h = hashlib.sha256(("%d:%s" % (self.a.seed, path)).encode()).digest()
        out = bytearray()
        i = 0
        while len(out) < size:
            out += hashlib.sha256(h + struct.pack("<I", i)).digest()
            i += 1
        return bytes(out[:size])

    def build_dir(self, path, chain, parent_cluster, children, is_root=False, label=None):
        """children: list of (name, kind, arg) kind 'file' (size) / 'frag' (size) / 'dir' (children)"""
        used, ents = set(), []
        if is_root and label:
            ents.append(struct.pack("<11sB20s", label.upper().ljust(11)[:11].encode(), 0x08, b"\0" * 20))
        if not is_root:
            t, d = self.stamp()
            ents.append(struct.pack("<11sBBBHHHHHHHI", b".          ", 0x10, 0, 0, t, d, d, chain[0] >> 16, t, d,
                                    chain[0] & 0xFFFF, 0))
            pc = 0 if parent_cluster == 2 else parent_cluster
            ents.append(struct.pack("<11sBBBHHHHHHHI", b"..         ", 0x10, 0, 0, t, d, d, pc >> 16, t, d, pc & 0xFFFF, 0))
        sub = []
        for name, kind, arg in children:
            cpath = path + "/" + name if path != "/" else "/" + name
            if kind == "dir":
                n = arg[1] if isinstance(arg, tuple) else 1
                c = self.alloc_run(n, hole_after=self.rng.randint(0, 1))
                es, short = self.entries_for(name, 0x10, c[0], 0, used, self.stamp())
                ents += es
                sub.append((cpath, c, arg[0] if isinstance(arg, tuple) else arg))
                self.manifest.append({"path": cpath, "dir": True, "entry": short.hex(), "chain": c})
            else:
                size = arg
                if size:
                    n = (size + self.spc * SECTOR - 1) // (self.spc * SECTOR)
                    c = self.alloc_fragmented(n) if kind == "frag" else self.alloc_run(n, hole_after=self.rng.randint(0, 2))
                    data = self.content(cpath, size)
                    self.write_chain(c, data)
                else:
                    c, data = [], b""
                es, short = self.entries_for(name, 0x20, c[0] if c else 0, size, used, self.stamp())
                ents += es
                self.manifest.append({"path": cpath, "dir": False, "entry": short.hex(), "chain": c, "size": size,
                                      "sha256": hashlib.sha256(data).hexdigest()})
        blob = b"".join(ents)
        cap = len(chain) * self.spc * SECTOR
        if len(blob) > cap:
            sys.exit("directory %s needs more clusters (%d bytes of entries)" % (path, len(blob)))
        self.write_chain(chain, blob)
        for cpath, c, kids in sub:
            self.build_dir(cpath, c, chain[0], kids)

    def populate_stick(self):
        r = self.rng
        root = []
        for i in range(12):
            root.append(("FILE%02d.BIN" % i, "file", r.choice([0, 100, 511, 512, 513, 4096, 32768, 32769, 70000])))
        for i in range(10):
            root.append(("Long file name number %d with spaces.txt" % i, "file", r.randint(1, 50000)))
        root.append(("xenon.elf", "file", 300000))
        root.append(("fragmented.dat", "frag", 400000))
        photos = [("IMG_%04d.JPG" % i, "file", r.randint(20000, 120000)) for i in range(20)]
        deep = [("level3", "dir", [("deep file.txt", "file", 1234), ("EMPTY.TXT", "file", 0)])]
        root.append(("DCIM", "dir", [("100PHOTO", "dir", photos)]))
        root.append(("Documents and Settings", "dir", [("level2 dir", "dir", deep), ("notes.txt", "file", 777)]))
        root.append(("MUSIC", "dir", ([("Track %02d.mp3" % i, "file", r.randint(50000, 200000)) for i in range(30)], 2)))
        for i in range(6):
            root.append(("SMALL%d.TXT" % i, "file", r.randint(1, 3000)))
        return root


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--size", default="2200M")
    ap.add_argument("--cluster", type=int, default=32768)
    ap.add_argument("--layout", choices=("superfloppy", "mbr"), default="superfloppy")
    ap.add_argument("--part-start", type=int, default=2048)
    ap.add_argument("--fats", type=int, default=2)
    ap.add_argument("--reserved", type=int, default=0)
    ap.add_argument("--align", type=int, default=65536, help="align the data region to this many sectors (default 32 MB)")
    ap.add_argument("--label", default="XTSTEST")
    ap.add_argument("--populate", choices=("none", "stick"), default="none")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--manifest")
    a = ap.parse_args()

    if os.path.exists(a.out) and not stat.S_ISREG(os.stat(a.out).st_mode):
        sys.exit("refusing to write %s: not a regular file" % a.out)
    v = Volume(a)
    root_chain = v.alloc_run(1)          # cluster 2
    assert root_chain == [2]
    v.next_free = 3
    kids = v.populate_stick() if a.populate == "stick" else []
    v.build_dir("/", root_chain, 0, kids, is_root=True, label=a.label)

    used = len(v.fat)
    free = v.clusters - used
    with open(a.out, "wb") as f:
        f.truncate(parse_size(a.size))
        base = v.part_start * SECTOR

        def wsec(sec, data):
            f.seek(base + sec * SECTOR)
            f.write(data)

        if a.layout == "mbr":
            mbr = bytearray(SECTOR)
            ent = struct.pack("<B3sB3sII", 0x00, b"\xfe\xff\xff", 0x0C, b"\xfe\xff\xff", v.part_start, v.total)
            mbr[0x1BE:0x1BE + 16] = ent
            mbr[0x1B8:0x1BC] = struct.pack("<I", 0x58545331)
            mbr[510:512] = b"\x55\xaa"
            f.seek(0)
            f.write(mbr)
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = b"MSWIN4.1"
        struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, v.spc, v.reserved, v.nfats, 0, 0, 0xF8, 0, 63, 255,
                         v.part_start, v.total)
        struct.pack_into("<IHHIHH", bs, 36, v.spf, 0, 0, 2, 1, 6)
        struct.pack_into("<BBBI11s8s", bs, 64, 0x80, 0, 0x29, 0x1234ABCD ^ a.seed, a.label.upper().ljust(11)[:11].encode(),
                         b"FAT32   ")
        bs[510:512] = b"\x55\xaa"
        fsi = bytearray(SECTOR)
        fsi[0:4] = b"RRaA"
        fsi[484:488] = b"rrAa"
        struct.pack_into("<II", fsi, 488, free, max(v.fat) if v.fat else 2)
        fsi[510:512] = b"\x55\xaa"
        for off in (0, 6):
            wsec(off, bytes(bs))
            wsec(off + 1, bytes(fsi))
            wsec(off + 2, b"\0" * 510 + b"\x55\xaa")
        # FATs
        fat = {0: 0x0FFFFFF8, 1: 0x0FFFFFFF}
        fat.update(v.fat)
        maxc = max(fat)
        table = bytearray((maxc + 1) * 4)
        for c, val in fat.items():
            struct.pack_into("<I", table, c * 4, val)
        for k in range(v.nfats):
            f.seek(base + (v.reserved + k * v.spf) * SECTOR)
            f.write(table)
        for sec, data in v.data.items():
            wsec(sec, data)
    if a.manifest:
        with open(a.manifest, "w") as m:
            json.dump({"image": a.out, "layout": a.layout, "part_start": v.part_start, "cluster": a.cluster,
                       "entries": v.manifest}, m, indent=1)
    print("%s: %s, %d clusters of %d B, reserved %d, FAT %d sectors x %d, data at %d, %d used, %d free, %d entries" %
          (a.out, a.layout, v.clusters, a.cluster, v.reserved, v.spf, v.nfats, v.data_start, used, free, len(v.manifest)))


if __name__ == "__main__":
    main()
