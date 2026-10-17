#!/usr/bin/env python3
"""Read-only FAT32 checker for disk images (superfloppy or MBR-partitioned), e.g. the ones tools/fat_mkimage.py makes.

  fat_check.py IMAGE [--manifest M.json] [--json OUT.json] [--quiet] [--strict] [--list]

Checks: boot sector / BPB sanity (and the backup boot sector), FAT #1 == FAT #2 (unless the BPB disables mirroring),
media/clean-shutdown bits (FAT[1]) and the BPB dirty flag, every directory (entries, LFN checksums, "." and ".."), every
entry's cluster chain (in range, allocated, no loop, ends with end-of-chain, no cluster used twice = cross-link, length
consistent with the file size), clusters allocated in the FAT but reachable from no entry (lost chains), FSInfo
signatures and free count. With --manifest (fat_mkimage.py --manifest), every pre-existing entry must still be there
with the same 32-byte short entry, the same cluster chain and (files) the same content hash; new entries are fine.

Prints a human summary and writes JSON (--json, or to stdout with --json -). Problems have a severity: "error"
(corruption / damaged pre-existing content) or "warning" (lost clusters, FSInfo count off, dirty flags: what chkdsk
fixes silently). Exit status 1 if there is an error (or any warning with --strict), else 0. Never writes the image.
"""
import argparse
import hashlib
import json
import struct
import sys

EOC_MIN = 0x0FFFFFF8
BAD = 0x0FFFFFF7


class Img:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        self.size = self.f.tell()

    def read(self, off, n):
        self.f.seek(off)
        b = self.f.read(n)
        return b + b"\0" * (n - len(b))


class Checker:
    def __init__(self, img, manifest=None):
        self.img = img
        self.problems = []
        self.manifest = manifest
        self.paths = {}            # path -> info (for the manifest)
        self.stats = {}

    def p(self, sev, kind, msg, path=None):
        d = {"severity": sev, "kind": kind, "msg": msg}
        if path:
            d["path"] = path
        self.problems.append(d)

    # ---- layout ----
    def find_volume(self):
        s0 = self.img.read(0, 512)
        if s0[82:85] == b"FAT" or s0[54:57] == b"FAT":
            return 0, "superfloppy"
        if s0[510:512] != b"\x55\xaa":
            return None, "no boot signature in sector 0"
        for i in range(4):
            e = s0[446 + 16 * i:462 + 16 * i]
            typ, lba = e[4], struct.unpack_from("<I", e, 8)[0]
            if typ and lba:
                b = self.img.read(lba * 512, 512)
                if b[82:85] == b"FAT":
                    return lba, "mbr partition %d (type 0x%02X)" % (i + 1, typ)
        return None, "no FAT32 volume found"

    def bpb(self):
        start, how = self.find_volume()
        if start is None:
            self.p("error", "layout", how)
            return False
        self.vol = start * 512
        b = self.img.read(self.vol, 512)
        (bps, spc, res, nf, rootent, ts16, media, spf16, _, _, hidden, ts32) = struct.unpack_from("<HBHBHHBHHHII", b, 11)
        spf, ext, ver, rootc, fsinfo, bk = struct.unpack_from("<IHHIHH", b, 36)
        self.layout = how
        self.bps, self.spc, self.res, self.nf = bps, spc, res, nf
        self.spf, self.ext, self.rootc, self.fsinfo, self.bk = spf, ext, rootc, fsinfo, bk
        self.total = ts32 or ts16
        ok = True
        if b[510:512] != b"\x55\xaa":
            self.p("error", "bpb", "boot sector signature missing")
            ok = False
        if bps not in (512, 1024, 2048, 4096):
            self.p("error", "bpb", "bytes/sector %d" % bps)
            return False
        if spc == 0 or spc & (spc - 1):
            self.p("error", "bpb", "sectors/cluster %d" % spc)
            return False
        if res == 0 or nf not in (1, 2) or spf == 0 or spf16 != 0 or rootent != 0:
            self.p("error", "bpb", "reserved %d, FATs %d, FAT size %d/%d, root entries %d: not FAT32" % (res, nf, spf16, spf, rootent))
            return False
        self.data = res + nf * spf
        self.clusters = (self.total - self.data) // spc
        self.last = self.clusters + 1
        self.cbytes = bps * spc
        if self.clusters < 65525:
            self.p("error", "bpb", "%d clusters: FAT32 needs >= 65525 (drivers would treat it as FAT16)" % self.clusters)
            ok = False
        if (self.clusters + 2) * 4 > spf * bps:
            self.p("error", "bpb", "FAT too small for %d clusters" % self.clusters)
            ok = False
        if self.vol + self.total * bps > self.img.size:
            self.p("error", "bpb", "volume (%d sectors) extends past the end of the image" % self.total)
            ok = False
        if not (2 <= rootc <= self.last):
            self.p("error", "bpb", "root cluster %d out of range" % rootc)
            ok = False
        if b[0x41] & 1:
            self.p("warning", "dirty", "BPB dirty flag set (volume not cleanly unmounted)")
        if bk and bk < res:
            bb = self.img.read(self.vol + bk * bps, 512)
            if bb[:90] != b[:90] or bb[510:] != b[510:]:
                self.p("warning", "bpb", "backup boot sector (sector %d) differs from the boot sector" % bk)
        self.stats.update({"layout": how, "bytes_per_sector": bps, "sectors_per_cluster": spc, "reserved": res,
                           "fats": nf, "sectors_per_fat": spf, "clusters": self.clusters, "data_start": self.data,
                           "root_cluster": rootc, "ext_flags": ext})
        return ok

    def load_fats(self):
        n = self.clusters + 2
        self.fats = []
        for k in range(self.nf):
            raw = self.img.read(self.vol + (self.res + k * self.spf) * self.bps, n * 4)
            self.fats.append(struct.unpack("<%dI" % n, raw))
        mirrored = not (self.ext & 0x80)
        active = 0 if mirrored else (self.ext & 0x0F)
        self.fat = [v & 0x0FFFFFFF for v in self.fats[min(active, self.nf - 1)]]
        if self.fat[0] & 0xFF != 0xF8 & 0xFF and self.fat[0] & 0xFF < 0xF0:
            self.p("warning", "fat", "FAT[0] = %08X (media byte)" % self.fat[0])
        f1 = self.fats[0][1]
        if not (f1 & 0x08000000):
            self.p("warning", "dirty", "FAT[1] clean-shutdown bit clear (volume dirty)")
        if not (f1 & 0x04000000):
            self.p("warning", "dirty", "FAT[1] hard-error bit clear (I/O error recorded)")
        self.fat_diff = []
        if mirrored and self.nf > 1:
            a, b = self.fats[0], self.fats[1]
            if a != b:
                for c in range(2, n):
                    if (a[c] & 0x0FFFFFFF) != (b[c] & 0x0FFFFFFF):
                        self.fat_diff.append(c)
                if self.fat_diff:
                    sample = ", ".join("%d: %08X/%08X" % (c, a[c], b[c]) for c in self.fat_diff[:8])
                    self.p("error", "fat_mismatch", "FAT #1 and FAT #2 differ in %d entries (%s%s)" %
                           (len(self.fat_diff), sample, ", ..." if len(self.fat_diff) > 8 else ""))
        self.stats["fat_mismatch_entries"] = len(self.fat_diff)

    def csec(self, c):
        return self.data + (c - 2) * self.spc

    def read_cluster(self, c):
        return self.img.read(self.vol + self.csec(c) * self.bps, self.cbytes)

    # ---- chains ----
    def chain(self, start, path):
        """follow a chain; returns (clusters, ok)"""
        out, seen, c = [], set(), start
        while True:
            if not (2 <= c <= self.last):
                self.p("error", "chain", "cluster %d out of range (after %d clusters)" % (c, len(out)), path)
                return out, False
            if c in seen:
                self.p("error", "chain", "loop at cluster %d" % c, path)
                return out, False
            v = self.fat[c]
            if v == 0:
                if out:
                    self.p("error", "chain", "cluster %d of the chain is free in the FAT (after %d clusters)" % (c, len(out)), path)
                else:
                    self.p("error", "dangling", "the entry points to cluster %d, which is free in the FAT (the next allocation "
                           "would hand it out again: cross-link)" % c, path)
                return out, False
            if v == BAD:
                self.p("error", "chain", "cluster %d is marked bad" % c, path)
                return out, False
            seen.add(c)
            out.append(c)
            if v >= EOC_MIN:
                return out, True
            if v == 1 or v > self.last:
                self.p("error", "chain", "cluster %d links to invalid value %08X" % (c, v), path)
                return out, False
            c = v

    def claim(self, clusters, path):
        for c in clusters:
            if c in self.owner:
                self.p("error", "crosslink", "cluster %d used by %s and %s" % (c, self.owner[c], path), path)
            else:
                self.owner[c] = path

    # ---- directories ----
    def walk(self, cluster, path, parent, depth=0):
        if depth > 32:
            self.p("error", "dir", "directory nesting too deep (loop?)", path)
            return
        clusters, ok = self.chain(cluster, path)
        self.claim(clusters, path)
        data = b"".join(self.read_cluster(c) for c in clusters)
        lfn, lfn_ck, names = [], None, set()
        self.stats["dirs"] = self.stats.get("dirs", 0) + 1
        for i in range(0, len(data), 32):
            e = data[i:i + 32]
            if e[0] == 0:
                rest = data[i:]
                if any(rest):
                    self.p("warning", "dir", "non-zero bytes after the end-of-directory marker (entry %d)" % (i // 32), path)
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            attr = e[11]
            if attr == 0x0F:
                lfn_ck = e[13]
                if e[0] & 0x40:
                    lfn = []
                part = e[1:11] + e[14:26] + e[28:32]
                lfn.insert(0, part)
                continue
            name11 = e[:11]
            bad = [ch for ch in name11 if ch < 0x20 and not (ch == 0x05 and name11[0] == ch)]
            if bad or attr & 0xC0:
                self.p("error", "dir", "garbage entry %d (name %r, attr %02X): not a directory entry" % (i // 32, bytes(name11), attr),
                       path)
                lfn = []
                continue
            short = self.short_name(name11)
            longname = None
            if lfn:
                ck = 0
                for b in name11:
                    ck = (((ck & 1) << 7) + (ck >> 1) + b) & 0xFF
                if ck == lfn_ck:
                    u = b"".join(lfn)
                    s = u.decode("utf-16-le", "replace")
                    longname = s.split("\0")[0]
                else:
                    self.p("warning", "dir", "LFN checksum mismatch before %s" % short, path)
            lfn = []
            if attr & 0x08 and not attr & 0x10:
                continue                                    # volume label
            name = longname or short
            if name in (".", ".."):
                cl = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
                want = cluster if name == "." else (0 if parent == self.rootc else parent)
                if cl != want:
                    self.p("warning", "dir", "'%s' points to %d, expected %d" % (name, cl, want), path)
                continue
            if name in names:
                self.p("error", "dir", "duplicate name %s" % name, path)
            names.add(name)
            cpath = (path.rstrip("/") + "/" + name) if path != "/" else "/" + name
            self.entry(e, cpath, attr, cluster, depth)

    @staticmethod
    def short_name(n):
        base = n[:8].decode("latin-1").rstrip()
        ext = n[8:11].decode("latin-1").rstrip()
        if base and base[0] == "\x05":
            base = "\xe5" + base[1:]
        return base + ("." + ext if ext else "")

    def entry(self, e, cpath, attr, dircluster, depth):
        cl = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
        size = struct.unpack_from("<I", e, 28)[0]
        info = {"entry": e.hex(), "dir": bool(attr & 0x10), "first_cluster": cl, "size": size}
        self.paths[cpath] = info
        if attr & 0x10:
            if cl == 0:
                self.p("error", "dir", "directory without a cluster", cpath)
                return
            info["chain"] = None
            self.walk(cl, cpath, dircluster, depth + 1)
            info["chain"] = self.dir_chains.get(cl)
            return
        self.stats["files"] = self.stats.get("files", 0) + 1
        if cl == 0:
            info["chain"] = []
            if size:
                self.p("error", "size", "size %d but no cluster" % size, cpath)
            info["sha256"] = hashlib.sha256(b"").hexdigest() if not size else None
            return
        clusters, ok = self.chain(cl, cpath)
        self.claim(clusters, cpath)
        info["chain"] = clusters
        need = (size + self.cbytes - 1) // self.cbytes
        if ok and need != len(clusters):
            self.p("error", "size", "size %d needs %d cluster(s), chain has %d" % (size, need, len(clusters)), cpath)
        if ok and self.want_hash(cpath):
            h = hashlib.sha256()
            left = size
            for c in clusters:
                if left <= 0:
                    break
                blob = self.read_cluster(c)[:left]
                h.update(blob)
                left -= len(blob)
            info["sha256"] = h.hexdigest()

    def want_hash(self, path):
        return self.hash_all or (self.manifest is not None and path in self.manifest_paths)

    def check(self, hash_all=False):
        self.hash_all = hash_all
        self.manifest_paths = {e["path"] for e in self.manifest["entries"]} if self.manifest else set()
        if not self.bpb():
            return
        self.load_fats()
        self.owner = {}
        self.dir_chains = {}
        # record directory chains for the manifest (walk() computes them; recompute cheaply here)
        orig_walk = self.walk

        def walk(cluster, path, parent, depth=0):
            ch, _ = self.chain_quiet(cluster)
            self.dir_chains[cluster] = ch
            return orig_walk(cluster, path, parent, depth)
        self.walk = walk
        self.walk(self.rootc, "/", 0)
        # lost clusters
        lost = [c for c in range(2, self.last + 1) if self.fat[c] != 0 and self.fat[c] != BAD and c not in self.owner]
        if lost:
            self.p("warning", "lost", "%d allocated cluster(s) belong to no file or directory (first: %s)" %
                   (len(lost), ", ".join(map(str, lost[:8]))))
        free = sum(1 for c in range(2, self.last + 1) if self.fat[c] == 0)
        self.stats.update({"free_clusters": free, "lost_clusters": len(lost), "used_clusters": len(self.owner)})
        self.lost = lost
        self.check_fsinfo(free)
        if self.manifest:
            self.check_manifest()
            self.forensics()

    def chain_quiet(self, c):
        out, seen = [], set()
        while 2 <= c <= self.last and c not in seen and self.fat[c] not in (0, BAD):
            seen.add(c)
            out.append(c)
            if self.fat[c] >= EOC_MIN:
                break
            c = self.fat[c]
        return out, True

    def check_fsinfo(self, free):
        if not self.fsinfo or self.fsinfo >= self.res:
            self.p("warning", "fsinfo", "no FSInfo sector")
            return
        s = self.img.read(self.vol + self.fsinfo * self.bps, 512)
        if s[0:4] != b"RRaA" or s[484:488] != b"rrAa" or s[510:512] != b"\x55\xaa":
            self.p("error", "fsinfo", "FSInfo signatures damaged")
            return
        fc, nxt = struct.unpack_from("<II", s, 488)
        self.stats.update({"fsinfo_free": fc, "fsinfo_next": nxt})
        if fc != 0xFFFFFFFF and fc != free:
            self.p("warning", "fsinfo", "FSInfo free count %d, actual %d" % (fc, free))
        if nxt != 0xFFFFFFFF and not (2 <= nxt <= self.last):
            self.p("warning", "fsinfo", "FSInfo next-free hint %d out of range" % nxt)

    def forensics(self):
        """where damaged pre-existing directories got their bytes from: sectors identical to sectors of OTHER clusters
        (a cache page of another cluster written over the directory), and USB protocol blocks (CBW "USBC" / CSW "USBS")
        at the start of a sector of any pre-existing entry"""
        if not self.manifest:
            return
        pre_dirs = {self.rootc: "/"}
        pre_all = {}
        for m in self.manifest["entries"]:
            for c in m["chain"]:
                pre_all[c] = m["path"]
                if m["dir"]:
                    pre_dirs[c] = m["path"]
        pre_all[self.rootc] = "/"
        index = {}
        for c, owner in self.owner.items():
            if c in pre_dirs:
                continue
            blob = self.read_cluster(c)
            for k in range(self.spc):
                sec = blob[k * self.bps:(k + 1) * self.bps]
                if any(sec):
                    index.setdefault(sec, (c, k, owner))
        foreign = []
        for c, path in sorted(pre_dirs.items()):
            blob = self.read_cluster(c)
            hits = [(k,) + index[blob[k * self.bps:(k + 1) * self.bps]] for k in range(self.spc)
                    if blob[k * self.bps:(k + 1) * self.bps] in index]
            if hits:
                k0, oc, ok, owner = hits[0]
                foreign.append({"dir": path, "cluster": c, "sectors": [h[0] for h in hits], "from_cluster": oc, "from": owner})
                self.p("error", "foreign_page", "directory cluster %d: %d sector(s) (%d-%d) identical to sectors of cluster %d "
                       "(%s)" % (c, len(hits), hits[0][0], hits[-1][0], oc, owner), path)
        # the used part of every FAT copy: a FAT sector that equals a data sector was written from a stale cache buffer
        used_secs = (max([c for c in range(2, self.last + 1) if self.fat[c]] + [2]) * 4) // self.bps + 1
        for k in range(self.nf):
            for i in range(used_secs):
                sec = self.img.read(self.vol + (self.res + k * self.spf + i) * self.bps, self.bps)
                if any(sec) and sec in index:
                    oc, ok_, owner = index[sec]
                    foreign.append({"fat": k + 1, "sector": i, "from_cluster": oc, "from": owner})
                    self.p("error", "foreign_page", "FAT #%d sector %d is identical to sector %d of cluster %d (%s)" %
                           (k + 1, i, ok_, oc, owner))
                    continue
                vals = struct.unpack("<%dI" % (self.bps // 4), sec)
                junk = sum(1 for v in vals if (v & 0xF0000000) or (self.last < (v & 0x0FFFFFFF) < BAD) or (v & 0x0FFFFFFF) == 1)
                if junk >= 8 and not (i == 0 and junk <= 2):
                    foreign.append({"fat": k + 1, "sector": i, "junk_entries": junk})
                    self.p("error", "foreign_page", "FAT #%d sector %d holds other data (%d of %d values are not FAT entries: "
                           "a cache page of something else was written over it)" % (k + 1, i, junk, len(vals)))
        cbw = []
        for c, path in sorted(pre_all.items()):
            blob = self.read_cluster(c)
            for k in range(self.spc):
                head = blob[k * self.bps:k * self.bps + 4]
                if head in (b"USBC", b"USBS"):
                    cbw.append({"path": path, "cluster": c, "sector": k, "sig": head.decode()})
                    self.p("error", "usb_garbage", "cluster %d sector %d starts with a USB %s block" %
                           (c, k, "CBW" if head == b"USBC" else "CSW"), path)
        self.stats.update({"foreign_pages": len(foreign), "usb_garbage_sectors": len(cbw)})

    def check_manifest(self):
        missing = changed = 0
        for m in self.manifest["entries"]:
            path = m["path"]
            got = self.paths.get(path)
            if got is None:
                self.p("error", "preexisting", "pre-existing %s is gone" % ("directory" if m["dir"] else "file"), path)
                missing += 1
                continue
            why = []
            if got["entry"] != m["entry"]:
                why.append("directory entry bytes changed (%s -> %s)" % (m["entry"], got["entry"]))
            if (got.get("chain") or []) != m["chain"]:
                why.append("cluster chain changed")
            if not m["dir"] and got.get("sha256") != m["sha256"]:
                why.append("content changed")
            if why:
                changed += 1
                self.p("error", "preexisting", "; ".join(why), path)
        pre = set()
        for m in self.manifest["entries"]:
            pre.update(m["chain"])
        pre.add(self.rootc)
        bad_fat = [c for c in self.fat_diff if c in pre]
        if bad_fat:
            self.p("error", "preexisting_fat2", "FAT copies disagree on %d cluster(s) of pre-existing entries (the files "
                   "themselves are checked above)" % len(bad_fat))
        self.stats.update({"manifest_entries": len(self.manifest["entries"]), "manifest_missing": missing,
                           "manifest_changed": changed, "preexisting_fat_mismatch": len(bad_fat)})


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--manifest")
    ap.add_argument("--json", help="write the JSON report here ('-' = stdout)")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--strict", action="store_true", help="warnings fail too")
    ap.add_argument("--list", action="store_true", help="list every file with its size and hash")
    ap.add_argument("--oneline", action="store_true", help="one machine-readable summary line (host/fatreplay.c)")
    a = ap.parse_args()
    manifest = json.load(open(a.manifest)) if a.manifest else None
    ck = Checker(Img(a.image), manifest)
    ck.check(hash_all=a.list)
    errors = [p for p in ck.problems if p["severity"] == "error"]
    warnings = [p for p in ck.problems if p["severity"] == "warning"]
    kinds = {}
    for p in ck.problems:
        kinds[p["kind"]] = kinds.get(p["kind"], 0) + 1
    rep = {"image": a.image, "ok": not errors and not (a.strict and warnings), "errors": len(errors),
           "warnings": len(warnings), "kinds": kinds, "problems": ck.problems, "stats": ck.stats}
    if a.list:
        rep["files"] = {p: {k: v for k, v in i.items() if k != "entry"} for p, i in ck.paths.items()}
    if a.json == "-":
        json.dump(rep, sys.stdout, indent=1)
        print()
    elif a.json:
        with open(a.json, "w") as f:
            json.dump(rep, f, indent=1)
    if a.oneline:
        st = ck.stats
        outside = [p for p in errors if p["kind"] not in ("fat_mismatch", "preexisting_fat2") and
                   not (p.get("path", "").startswith("/xts/") or p.get("path") == "/xts")]
        print("FATCHECK ok=%d errors=%d struct_errors=%d outside_xts=%d dangling=%d warnings=%d preexisting_fat2=%d preexisting=%d fat_mismatch=%d chain=%d crosslink=%d size=%d dir=%d "
              "lost=%d fsinfo=%d foreign_page=%d usb_garbage=%d bpb=%d" %
              (1 if rep["ok"] else 0, len(errors),
               len([p for p in errors if p["kind"] not in ("fat_mismatch", "preexisting_fat2")]), len(outside),
               kinds.get("dangling", 0), len(warnings),
               kinds.get("preexisting_fat2", 0), kinds.get("preexisting", 0), kinds.get("fat_mismatch", 0),
               kinds.get("chain", 0), kinds.get("crosslink", 0), kinds.get("size", 0), kinds.get("dir", 0),
               st.get("lost_clusters", 0), kinds.get("fsinfo", 0), kinds.get("foreign_page", 0), kinds.get("usb_garbage", 0),
               kinds.get("bpb", 0) + kinds.get("layout", 0)))
    elif not a.quiet and a.json != "-":
        st = ck.stats
        print("%s: %s, %s clusters, %s files, %s dirs, %s free, %s lost: %d error(s), %d warning(s)" %
              (a.image, st.get("layout", "?"), st.get("clusters", "?"), st.get("files", 0), st.get("dirs", 0),
               st.get("free_clusters", "?"), st.get("lost_clusters", "?"), len(errors), len(warnings)))
        for p in ck.problems[:40]:
            print("  %-7s %-12s %s%s" % (p["severity"], p["kind"], (p["path"] + ": ") if "path" in p else "", p["msg"]))
        if len(ck.problems) > 40:
            print("  ... %d more" % (len(ck.problems) - 40))
    sys.exit(0 if rep["ok"] else 1)


if __name__ == "__main__":
    main()
