#!/usr/bin/env python3
"""xts_recv.py - receive XTS runs (UDP messages, docs/PROTOCOL.md) and write the run directory.

  python3 tools/xts_recv.py [--out xts-runs] [--port 36360] [--bind 0.0.0.0] [--keep-listening] [--timeout SEC]
                            [--quarantine-out quarantine.txt] [--quiet]
  python3 tools/xts_recv.py --once --port 0 --run-dir DIR --exec CMD [ARGS...]   (host dry runs: XTS_NET_HOST is set
                                                                                  for CMD; exit status CMD's or ours)
  python3 tools/xts_recv.py --replay DIR/capture.bin [--run-dir DIR2]       (rebuild a run directory offline)
  python3 tools/xts_recv.py --selftest

Start it BEFORE booting the console. It listens on UDP 36360 (any interface, broadcast), de-duplicates frames on
(run_nonce, seq), checks every CRC, reassembles messages and, when the run's END arrives (or on Ctrl-C / --timeout of
silence), writes <out>/<console_tag>/<run_nonce>/ (the suite's run_id is the nonce):
    results.jsonl  results.csv  failures.jsonl  run.log  sysinfo.json  summary.json  temps.csv
    crash.json     (only when a CRASH or HANG report arrived)
    receive.json   (loss accounting: complete, missing_seq ranges, crc_errors, duplicates, incomplete_messages, ...)
    capture.bin    (every unique valid frame as received: --replay rebuilds the directory from it)
This is the layout the USB sink used to write, so xts_merge.py, xts_schedmodel.py and check_records.py work unchanged.

A run is COMPLETE only if HELLO and END arrived, no seq in 1..END.last_seq is missing, the per-type message counts
match END and the recomputed rec_hash matches END. The verdict is one line; exit status 0 = every run complete with
END status "complete", 1 = otherwise, 2 = usage. Several consoles (run nonces) can send at the same time.

--quarantine-out FILE appends a suggested quarantine.txt line for every CRASH/HANG report (the case the test core was
running), unless the file already lists it. Rebuild the ELF afterwards (make), the case is then skipped.

Multi-pass sessions (a profile with passes > 1, or make PASSES=<n>): every pass is a run of its own whose HELLO and END
carry "session": {"id", "pass", "passes"}. The receiver files each pass as usual and keeps listening until every pass of
the session has ended (even in the default one-run mode), then writes <out>/<console_tag>/session-<id>/session.json
(per pass: run directory, verdict, status, records, seed; the session verdict) and merges the complete passes with
tools/xts_merge.py into session-<id>/merged/ (--no-merge skips that). The session ends early when a pass reports a
crash or hang (the console halted), or after --session-wait SEC (default 600) without a new pass. With --run-dir DIR,
pass k goes to DIR/pass<k>/ and session.json to DIR. Exit status 0 needs every pass complete.
Only the standard library is used.
"""
import argparse
import bisect
import json
import os
import random
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from collections import Counter, OrderedDict

PORT = 36360
MAGIC = b"XTS1"
VERSION = 1
HDR = struct.Struct(">4sBBBBQIIHHHHI")          # 36 bytes, docs/PROTOCOL.md
assert HDR.size == 36
MAX_PAYLOAD = 1400
MAX_MESSAGE = 32768
TYPES = {1: "HELLO", 2: "REC", 3: "LOG", 4: "CRUMB", 5: "CRASH", 6: "HANG", 7: "BLOCK", 8: "END"}
TYPE_ID = {v: k for k, v in TYPES.items()}
STATUSES = ["pass", "fail", "trap", "skip", "noisy", "nonlinear", "regressed", "hang", "crash", "info"]
CSV_HEADER = "run_id,seq,family,case_id,mnem,word,cond,block,metric,value,unit,n,min,median,mad,fit_r2,status\n"
CAP_MAGIC = b"XTSCAP1\n"
FNV_INIT, FNV_PRIME = 0xcbf29ce484222325, 0x100000001b3
LINGER = 1.0                                     # seconds to keep listening after a run's first END
SESSION_WAIT = 600.0                             # default --session-wait: seconds without a new pass
SO_RXQ_OVFL = getattr(socket, "SO_RXQ_OVFL", 40)  # Linux: datagrams the kernel dropped on this socket (full buffer)


def fnv1a64(h, data):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def ranges(seqs):
    """sorted ints -> [[a, b], ...] inclusive"""
    out = []
    for s in seqs:
        if out and s == out[-1][1] + 1:
            out[-1][1] = s
        else:
            out.append([s, s])
    return out


def sanitize(s):
    return "".join(c if c.isalnum() or c in "-_." else "_" for c in str(s)) or "unknown"


# ------------------------------------------------------------------------------------------------ frame encoding
def encode_frame(nonce, seq, msg, typ, idx, cnt, payload, copy=0):
    """one UDP datagram body (XTS header + payload); the reference encoder for the selftest"""
    flags = 1 if copy else 0
    head = HDR.pack(MAGIC, VERSION, typ, flags, copy, nonce, seq, msg, idx, cnt, len(payload), 0, 0)[:32]
    crc = zlib.crc32(head + payload) & 0xFFFFFFFF
    return head + struct.pack(">I", crc) + payload


def encode_message(nonce, first_seq, msg, typ, data, copies=1):
    """frames of one message: list of (seq, [copy datagrams])"""
    n = max(1, (len(data) + MAX_PAYLOAD - 1) // MAX_PAYLOAD)
    out = []
    for i in range(n):
        p = data[i * MAX_PAYLOAD:(i + 1) * MAX_PAYLOAD]
        out.append((first_seq + i, [encode_frame(nonce, first_seq + i, msg, typ, i, n, p, c) for c in range(copies)]))
    return out


# ------------------------------------------------------------------------------------------------ CSV / summary
def csv_field(s):
    s = "" if s is None else str(s)
    if not any(c in s for c in ',"\n'):
        return s
    return '"' + s.replace('"', '""').replace("\n", " ") + '"'


def num(v):
    if v is None or isinstance(v, bool):
        return "null"
    try:
        f = float(v)
    except (TypeError, ValueError):
        return "null"
    if f != f or f in (float("inf"), float("-inf")):
        return "null"
    return "%.10g" % v


def cond_text(cond):
    return ";".join("%s=%s" % (k, v) for k, v in (cond or {}).items())


def csv_row(r):
    return ",".join([str(r.get("run_id", "")), str(r.get("seq", "")), csv_field(r.get("family", "")),
                     csv_field(r.get("case_id", "")), str(r.get("mnem", "")), str(r.get("word", "")),
                     csv_field(cond_text(r.get("cond"))), csv_field(r.get("block", "")),
                     csv_field(r.get("metric", "result")), num(r.get("value")), csv_field(r.get("unit", "")),
                     num(r.get("n")), num(r.get("min")), num(r.get("median")), num(r.get("mad")), num(r.get("fit_r2")),
                     str(r.get("status", ""))]) + "\n"


def summarize(records, run_id, profile, transport):
    total = Counter()
    by_family, by_axis = OrderedDict(), OrderedDict()
    for r in records:
        st = r.get("status")
        if st not in STATUSES:
            continue
        total[st] += 1
        by_family.setdefault(r.get("family", ""), Counter())[st] += 1
        for k, v in (r.get("cond") or {}).items():
            by_axis.setdefault("%s=%s" % (k, v), Counter())[st] += 1
    return OrderedDict([
        ("run_id", run_id), ("profile", profile), ("records", sum(total.values())),
        ("status", OrderedDict((s, total[s]) for s in STATUSES)),
        ("by_family", OrderedDict((f, OrderedDict((s, c[s]) for s in STATUSES if c[s])) for f, c in by_family.items())),
        ("by_axis_value", OrderedDict((a, OrderedDict((s, c[s]) for s in STATUSES if c[s])) for a, c in by_axis.items())),
        ("transport", transport), ("truncated", False)])


# ------------------------------------------------------------------------------------------------ one run
class Run:
    def __init__(self, nonce, now):
        self.nonce = nonce
        self.frames = {}              # seq -> (msg, typ, idx, cnt)
        self.arrival = {}             # seq -> time its first copy arrived
        self.timed = True             # arrival times are real (not a --replay)
        self.kernel_drops = None      # datagrams the PC's kernel dropped on the socket during the run (None: unknown)
        self.parts = {}               # msg -> {"typ", "cnt", "frags": {idx: bytes}}
        self.messages = {}            # msg -> (typ, bytes) complete
        self.received = self.duplicates = self.crc_errors = self.bad = self.late = 0
        self.max_seq = 0
        self.hello = self.end = None
        self.hello_msgs = 0
        self.last_crumb = None
        self.first = self.last = now
        self.end_time = None
        self.finalized = False
        self.capture = []             # raw datagrams (unique, valid), kept until the directory is known
        self.capture_file = None
        self.done_blocks = 0
        self.last_block = ""

    def feed(self, h, data, now=0.0):
        _, _, typ, _, _, _, seq, msg, idx, cnt, plen, _, _ = h
        self.received += 1
        if seq in self.frames:
            self.duplicates += 1
            return
        if typ not in TYPES or idx >= cnt or seq == 0 or msg == 0:
            self.bad += 1
            return
        self.frames[seq] = (msg, typ, idx, cnt)
        self.arrival[seq] = now
        self.max_seq = max(self.max_seq, seq)
        if self.capture_file:
            self.capture_file.write(struct.pack(">I", len(data)) + data)
        else:
            self.capture.append(data)
        if msg in self.messages:
            return
        p = self.parts.setdefault(msg, {"typ": typ, "cnt": cnt, "frags": {}})
        p["frags"][idx] = data[36:36 + plen]
        if len(p["frags"]) == p["cnt"]:
            payload = b"".join(p["frags"][i] for i in range(p["cnt"]))
            del self.parts[msg]
            self.messages[msg] = (typ, payload)
            self.on_message(typ, payload)

    def on_message(self, typ, payload):
        name = TYPES[typ]
        if name in ("HELLO", "END", "CRUMB", "BLOCK"):
            try:
                obj = json.loads(payload.decode("utf-8", "replace"))
            except ValueError:
                obj = None
            if name == "HELLO" and obj is not None:
                self.hello_msgs += 1
                if self.hello is None:
                    self.hello = obj
            elif name == "END" and obj is not None and self.end is None:
                self.end = obj
            elif name == "CRUMB" and obj is not None:
                self.last_crumb = obj
            elif name == "BLOCK" and obj is not None:
                self.done_blocks += 1
                self.last_block = obj.get("block", "")

    # identity
    def tag(self):
        h = self.hello or {}
        return sanitize(h.get("console_tag") or (h.get("sysinfo") or {}).get("console_tag") or "unknown")

    def run_id(self):
        h = self.hello or {}
        return str(h.get("run_id") or "%016x" % self.nonce)

    def session(self):
        """(id, pass, passes) of a multi-pass session, from HELLO (or END), else None"""
        for src in (self.hello, self.end):
            ss = (src or {}).get("session")
            if isinstance(ss, dict) and ss.get("id") and isinstance(ss.get("passes"), int) and ss["passes"] > 1:
                try:
                    return str(ss["id"]), int(ss.get("pass") or 0), int(ss["passes"])
                except (TypeError, ValueError):
                    return None
        return None

    def counts(self):
        c = Counter()
        for typ, _ in self.messages.values():
            c[TYPES[typ]] += 1
        return c

    def progress(self):
        c = self.counts()
        plan = (self.hello or {}).get("plan") or {}
        gaps = self.max_seq - len(self.frames)
        ss = self.session()
        return ("[%s %s%s] %s blocks %d/%s rec %d log %d | frames %d dup %d crc %d gaps %d%s" % (
            self.tag(), "%016x" % self.nonce, (" pass %d/%d" % (ss[1], ss[2])) if ss else "",
            "HELLO" if self.hello else "no-HELLO", self.done_blocks,
            plan.get("blocks", "?"), c["REC"], c["LOG"], len(self.frames), self.duplicates, self.crc_errors, gaps,
            (" | END " + str(self.end.get("status"))) if self.end else (" | " + self.last_block[:40])))

    # ---- the run directory ----
    def finalize(self, out_dir, run_dir=None, extra_crc=0, quiet=False):
        self.finalized = True
        d = run_dir or os.path.join(out_dir, self.tag(), "%016x" % self.nonce)
        os.makedirs(d, exist_ok=True)
        if self.capture_file is None:
            with open(os.path.join(d, "capture.bin"), "wb") as f:
                f.write(CAP_MAGIC)
                for data in self.capture:
                    f.write(struct.pack(">I", len(data)) + data)
        else:
            self.capture_file.close()
        self.capture = []
        order = sorted(self.messages)
        recs_raw, logs, reports, blocks = [], [], [], []
        h = FNV_INIT
        for m in order:
            typ, payload = self.messages[m]
            name = TYPES[typ]
            if name == "REC":
                recs_raw.append(payload)
                h = fnv1a64(h, payload)
            elif name == "LOG":
                logs.append(payload)
            elif name in ("CRASH", "HANG"):
                try:
                    reports.append((name, json.loads(payload.decode("utf-8", "replace"))))
                except ValueError:
                    reports.append((name, {"unparsable": payload.decode("utf-8", "replace")}))
            elif name == "BLOCK":
                try:
                    blocks.append(json.loads(payload.decode("utf-8", "replace")))
                except ValueError:
                    pass
        records = []
        for p in recs_raw:
            try:
                records.append(json.loads(p.decode("utf-8", "replace")))
            except ValueError:
                records.append({})
        # ---- loss accounting ----
        counts = self.counts()
        end = self.end or {}
        last_seq = end.get("last_seq") if isinstance(end.get("last_seq"), int) else None
        top = max(self.max_seq, last_seq or 0)
        missing = [s for s in range(1, top + 1) if s not in self.frames]
        missing_in_end = [s for s in missing if last_seq is not None and s <= last_seq]
        incomplete = [{"msg": m, "type": TYPES.get(p["typ"], "?"), "have": sorted(p["frags"]), "frag_cnt": p["cnt"]}
                      for m, p in sorted(self.parts.items())]
        want = end.get("messages") or {}
        mismatch = OrderedDict()
        for name in TYPES.values():
            if name == "END" or name not in want:
                continue
            if counts[name] != want[name]:
                mismatch[name] = [counts[name], want[name]]
        rec_hash = "%016x" % h
        context = self.loss_context(ranges(missing)) if self.timed else None
        reasons = []
        if self.hello is None:
            reasons.append("no HELLO")
        if self.end is None:
            reasons.append("no END")
        if missing_in_end:
            reasons.append("%d frame(s) missing in 1..last_seq" % len(missing_in_end))
        if mismatch:
            reasons.append("message counts differ from END: %s" % ", ".join(
                "%s %d/%d" % (k, v[0], v[1]) for k, v in mismatch.items()))
        if self.end is not None and end.get("rec_hash") != rec_hash:
            reasons.append("rec_hash %s != END %s" % (rec_hash, end.get("rec_hash")))
        if self.end is not None and end.get("send_failures"):
            reasons.append("%s message(s) could not be queued on the console" % end.get("send_failures"))
        complete = not reasons
        status = end.get("status", "no END")
        block_mismatch = []
        per_block = Counter(r.get("block") for r in records)
        for b in blocks:
            if b.get("block") is not None and isinstance(b.get("records"), int) and not b.get("aborted") \
                    and per_block[b["block"]] < b["records"]:
                block_mismatch.append([b["block"], per_block[b["block"]], b["records"]])
        rcv = OrderedDict([
            ("complete", complete), ("status", status), ("reasons", reasons),
            ("run_nonce", "%016x" % self.nonce), ("console_tag", self.tag()), ("run_id", self.run_id()),
            ("profile", (self.hello or {}).get("profile")), ("build_id", (self.hello or {}).get("build_id")),
            ("seed", (self.hello or {}).get("seed", 1)),
            ("session", (self.hello or {}).get("session") or end.get("session")),
            ("hello_seen", self.hello is not None), ("hello_messages", self.hello_msgs), ("end_seen", self.end is not None),
            ("last_seq", last_seq), ("max_seq_seen", self.max_seq),
            ("frames_received", self.received), ("frames_unique", len(self.frames)), ("duplicates", self.duplicates),
            ("crc_errors", self.crc_errors + extra_crc), ("bad_frames", self.bad), ("late_frames", self.late),
            ("missing_frames", len(missing)), ("missing_seq", ranges(missing)),
            ("missing_in_end_range", len(missing_in_end)), ("missing_context", context),
            ("kernel_drops", self.kernel_drops),
            ("incomplete_messages", incomplete),
            ("messages", OrderedDict((n, counts[n]) for n in TYPES.values() if counts[n])),
            ("end_messages", want), ("count_mismatch", mismatch),
            ("rec_hash", rec_hash), ("end_rec_hash", end.get("rec_hash")),
            ("hash_ok", self.end is not None and end.get("rec_hash") == rec_hash),
            ("send_failures", end.get("send_failures")),
            ("records", len(records)), ("blocks", len(blocks)), ("block_record_mismatch", block_mismatch[:50]),
            ("last_crumb", self.last_crumb),
            ("first_frame", time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(self.first))),
            ("duration_s", round(self.last - self.first, 3)),
            ("net", end.get("net")),
            ("receiver", "tools/xts_recv.py (protocol %d)" % VERSION)])
        # ---- files ----
        run_id = self.run_id()
        profile = (self.hello or {}).get("profile") or ""

        def wr(name, text):
            tmp = os.path.join(d, name + ".tmp")
            with open(tmp, "w", encoding="utf-8", newline="") as f:
                f.write(text)
            os.replace(tmp, os.path.join(d, name))

        wr("results.jsonl", "".join(p.decode("utf-8", "replace") + "\n" for p in recs_raw))
        wr("results.csv", CSV_HEADER + "".join(csv_row(r) for r in records))
        wr("failures.jsonl", "".join(p.decode("utf-8", "replace") + "\n" for p, r in zip(recs_raw, records)
                                     if r.get("status") in ("fail", "hang", "crash")))
        wr("run.log", "".join(p.decode("utf-8", "replace") + "\n" for p in logs))
        if self.hello is not None:
            wr("sysinfo.json", json.dumps(self.hello.get("sysinfo") or {}, indent=1) + "\n")
        transport = OrderedDict((k, rcv[k]) for k in ("complete", "status", "reasons", "missing_frames", "crc_errors",
                                                      "duplicates", "incomplete_messages"))
        transport["incomplete_messages"] = len(incomplete)
        wr("summary.json", json.dumps(summarize(records, run_id, profile, transport), indent=1) + "\n")
        temps = ["block,tb,cpu,gpu,edram,board\n"]
        for b in blocks:
            t = b.get("temps")
            if isinstance(t, dict):
                temps.append("%s,%s,%s,%s,%s,%s\n" % (b.get("block"), b.get("seq", ""), t.get("cpu"), t.get("gpu"),
                                                      t.get("edram"), t.get("board")))
        wr("temps.csv", "".join(temps))
        if reports:
            kind, rep = reports[0]
            obj = OrderedDict([("kind", kind.lower()), ("run_nonce", "%016x" % self.nonce)])
            obj.update(rep)
            if len(reports) > 1:
                obj["more_reports"] = [dict(r, kind=k.lower()) for k, r in reports[1:]]
            wr("crash.json", json.dumps(obj, indent=1) + "\n")
        wr("receive.json", json.dumps(rcv, indent=1) + "\n")
        self.reports = reports
        self.dir = d
        self.receive = rcv
        ss = self.session()
        verdict = "%s: run %s (%s, profile %s%s): %s, END status %s | records %d, frames %d unique, %d missing, %d dup, " \
                  "%d crc errors -> %s" % ("xts_recv", run_id, self.tag(), profile or "?",
                                           (", pass %d/%d" % (ss[1], ss[2])) if ss else "",
                                           "COMPLETE" if complete else "INCOMPLETE (" + "; ".join(reasons) + ")",
                                           status, len(records), len(self.frames), len(missing), self.duplicates,
                                           self.crc_errors + extra_crc, d)
        if self.kernel_drops:
            verdict += " | %d datagram(s) dropped by this PC's socket buffer" % self.kernel_drops
        if not quiet or not complete:
            print(verdict, flush=True)
        for kind, rep in reports:
            c = rep.get("crumb") or {}
            if not quiet:
                print("  %s report: family %s case %s block %s%s%s" % (
                    kind, c.get("family"), c.get("case_id"), c.get("block"),
                    (" vector %s srr0 %s" % (rep.get("vector"), rep.get("srr0"))) if kind == "CRASH" else "",
                    (" timeout %s us" % rep.get("timeout_us")) if kind == "HANG" else ""), flush=True)
        return complete and status == "complete"

    def loss_context(self, missing_ranges):
        """per missing range (first 50): when the frames around it arrived, relative to the run's first frame, and the
        longest silence in the second before it. Loss right after a silence of >= ~100 ms points at a receiver on Wi-Fi
        in power save (its access point drops what it cannot buffer while the receiver dozes); loss with kernel_drops
        > 0 at the PC's socket buffer."""
        if not self.arrival:
            return []
        order = sorted(self.arrival.items(), key=lambda kv: kv[1])
        times = [t for _, t in order]
        t0 = times[0]
        out = []
        for lo, hi in missing_ranges[:50]:
            before, after = self.arrival.get(lo - 1), self.arrival.get(hi + 1)
            quiet = 0.0
            if before is not None:
                i = bisect.bisect_right(times, before)
                j = bisect.bisect_left(times, before - 1.0)
                for a, b in zip(times[j:i], times[j + 1:i]):
                    quiet = max(quiet, b - a)
            out.append(OrderedDict([
                ("seq", [lo, hi]),
                ("t_ms", None if before is None else round((before - t0) * 1e3, 1)),
                ("hole_ms", None if before is None or after is None else round((after - before) * 1e3, 1)),
                ("quiet_before_ms", round(quiet * 1e3, 1))]))
        return out

    def quarantine_lines(self):
        out = []
        for kind, rep in getattr(self, "reports", []):
            c = rep.get("crumb") or {}
            fam, cid = c.get("family"), c.get("case_id")
            if not fam or not cid:
                continue
            what = "crash vector %s srr0 %s" % (rep.get("vector"), rep.get("srr0")) if kind == "CRASH" else \
                "hang (timeout %s us)" % rep.get("timeout_us")
            out.append(("%s %s" % (fam, cid), "%s  # %s, run %016x (%s), %s" % (
                "%s %s" % (fam, cid), what, self.nonce, self.tag(), time.strftime("%Y-%m-%d"))))
        return out


# ------------------------------------------------------------------------------------------------ receiver
class Receiver:
    def __init__(self, a):
        self.a = a
        self.runs = OrderedDict()
        self.unattributed_crc = 0
        self.bad_magic = 0
        self.results = []           # (nonce, ok)
        self.last_progress = 0
        self.timed = True           # False for --replay: arrival times are not real
        self.kernel_drops = None    # SO_RXQ_OVFL counter of the socket (None: not available)
        self.sessions = OrderedDict()   # session id -> Session (multi-pass profiles)

    def datagram(self, data, now):
        if len(data) < 36 or data[:4] != MAGIC:
            self.bad_magic += 1
            return
        h = HDR.unpack_from(data)
        if h[1] != VERSION:
            self.bad_magic += 1
            return
        nonce, plen = h[5], h[10]
        ok = plen <= MAX_PAYLOAD and 36 + plen <= len(data) and \
            (zlib.crc32(data[:32] + data[36:36 + plen]) & 0xFFFFFFFF) == h[12]
        run = self.runs.get(nonce)
        if not ok:
            if run is not None:
                run.crc_errors += 1
            else:
                self.unattributed_crc += 1
            return
        if run is None:
            run = self.runs[nonce] = Run(nonce, now)
            run.timed = self.timed
            run.kdrops0 = self.kernel_drops
            if self.a.run_dir is None and not self.a.quiet:
                print("xts_recv: new run %016x" % nonce, file=sys.stderr, flush=True)
        if run.finalized:
            run.late += 1
            return
        run.last = now
        run.feed(h, data[:36 + plen], now)
        if run.end is not None and run.end_time is None:
            run.end_time = now

    def due(self, now, force=False, idle=None):
        """finalize runs whose END lingered, that were idle too long, or all (force)"""
        for nonce, run in list(self.runs.items()):
            if run.finalized:
                continue
            if force or (run.end_time is not None and now - run.end_time >= LINGER) or \
                    (idle and now - run.last >= idle):
                self.finish(run, now)

    def finish(self, run, now=None):
        if not self.a.quiet:
            sys.stderr.write("\n")
        extra = self.unattributed_crc
        self.unattributed_crc = 0
        if self.kernel_drops is not None and getattr(run, "kdrops0", None) is not None:
            run.kernel_drops = self.kernel_drops - run.kdrops0
        ss = None if getattr(self, "single", False) else run.session()
        run_dir = self.a.run_dir
        if ss and run_dir:
            run_dir = os.path.join(run_dir, "pass%d" % ss[1])
        ok = run.finalize(self.a.out, run_dir, extra, self.a.quiet)
        self.results.append((run.nonce, ok))
        if ss:
            sess = self.sessions.get(ss[0])
            if sess is None:
                sess = self.sessions[ss[0]] = Session(ss[0], ss[2], run.tag())
            sess.add(ss[1], run, ok, now if now is not None else time.time())
        lines = run.quarantine_lines()
        if lines:
            print("suggested quarantine.txt line(s):")
            for _, l in lines:
                print("  " + l)
            if self.a.quarantine_out:
                have = set()
                try:
                    for l in open(self.a.quarantine_out, encoding="utf-8"):
                        t = l.split("#", 1)[0].split()
                        if len(t) == 2:
                            have.add(" ".join(t))
                except FileNotFoundError:
                    pass
                new = [l for k, l in lines if k not in have]
                if new:
                    with open(self.a.quarantine_out, "a", encoding="utf-8") as f:
                        f.write("".join(l + "\n" for l in new))
                print("%d line(s) appended to %s" % (len(new), self.a.quarantine_out))

    def all_done(self):
        return bool(self.runs) and all(r.finalized for r in self.runs.values())

    def sessions_due(self, now, force=False):
        """end the sessions that have every pass, stopped (crash/hang), or waited too long for the next pass"""
        wait = getattr(self.a, "session_wait", SESSION_WAIT)
        unattributed = any(not r.finalized and r.session() is None for r in self.runs.values())
        for sess in self.sessions.values():
            if sess.done:
                continue
            live = any(not r.finalized and (r.session() or ("",))[0] == sess.id for r in self.runs.values())
            why = sess.finished()
            if why is None and not live and not unattributed and wait and now - sess.last >= wait:
                why = "no new pass for %d s" % wait
            if why is None and force:
                why = "receiver stopped"
            if why is not None:
                sess.close(self.a, why)

    def sessions_settled(self):
        return all(sess.done for sess in self.sessions.values())

    def show_progress(self, now):
        if self.a.quiet or now - self.last_progress < (0.5 if sys.stderr.isatty() else 10):
            return
        self.last_progress = now
        live = [r for r in self.runs.values() if not r.finalized]
        if not live:
            return
        line = " || ".join(r.progress() for r in live)
        if sys.stderr.isatty():
            sys.stderr.write("\r" + line[:200].ljust(120))
        else:
            sys.stderr.write(line + "\n")
        sys.stderr.flush()

    def exit_code(self):
        if not self.results:
            return 1
        if any(not sess.ok for sess in self.sessions.values()):
            return 1
        return 0 if all(ok for _, ok in self.results) else 1


# ------------------------------------------------------------------------------------------------ multi-pass session
class Session:
    """the passes of one multi-pass boot (HELLO/END "session": {"id", "pass", "passes"})"""

    def __init__(self, sid, passes, tag):
        self.id, self.passes, self.tag = sid, passes, tag
        self.pass_runs = {}           # pass (1-based) -> (Run, ok)
        self.last = 0.0
        self.done = self.ok = False
        self.dir = None

    def add(self, k, run, ok, now):
        self.pass_runs[k] = (run, ok)
        self.last = now

    def finished(self):
        """why the session is over (every pass, or a pass that halted the console), or None"""
        if all(k in self.pass_runs for k in range(1, self.passes + 1)):
            return "all %d passes received" % self.passes
        for k, (run, ok) in sorted(self.pass_runs.items()):
            st = (run.end or {}).get("status")
            if st in ("crashed", "hang"):
                return "pass %d ended with status %s: the console halted" % (k, st)
        return None

    def close(self, a, why):
        self.done = True
        got = sorted(self.pass_runs)
        complete = [k for k in got if self.pass_runs[k][1]]
        self.ok = len(complete) == self.passes
        base = a.run_dir if getattr(a, "run_dir", None) else os.path.join(a.out, self.tag, "session-%s" % self.id)
        os.makedirs(base, exist_ok=True)
        self.dir = base
        passes = []
        for k in range(1, self.passes + 1):
            if k not in self.pass_runs:
                passes.append(OrderedDict([("pass", k), ("received", False)]))
                continue
            run, ok = self.pass_runs[k]
            rcv = getattr(run, "receive", {}) or {}
            passes.append(OrderedDict([
                ("pass", k), ("received", True), ("run_id", run.run_id()), ("dir", getattr(run, "dir", None)),
                ("complete", rcv.get("complete")), ("status", rcv.get("status")), ("ok", ok),
                ("records", rcv.get("records")), ("seed", rcv.get("seed")), ("missing_frames", rcv.get("missing_frames")),
                ("duration_s", rcv.get("duration_s")), ("reasons", rcv.get("reasons"))]))
        merged = None
        if complete and not getattr(a, "no_merge", False):
            merged = self.merge(base, [self.pass_runs[k][0] for k in complete])
        first = self.pass_runs[got[0]][0] if got else None
        obj = OrderedDict([
            ("session", self.id), ("console_tag", self.tag),
            ("profile", ((first.hello or {}).get("profile")) if first else None),
            ("build_id", ((first.hello or {}).get("build_id")) if first else None),
            ("passes", self.passes), ("received", len(got)), ("complete", len(complete)), ("ok", self.ok),
            ("ended_because", why), ("pass_runs", passes), ("merged", merged),
            ("next", "python3 tools/xts_schedmodel.py %s -o model/" % " ".join(
                p["dir"] for p in passes if p.get("ok") and p.get("dir")) if complete else None),
            ("receiver", "tools/xts_recv.py (protocol %d)" % VERSION)])
        tmp = os.path.join(base, "session.json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(json.dumps(obj, indent=1) + "\n")
        os.replace(tmp, os.path.join(base, "session.json"))
        print("xts_recv: session %s (%s, %s passes): %d received, %d complete -> %s (%s)%s" % (
            self.id, self.tag, self.passes, len(got), len(complete), "COMPLETE" if self.ok else "INCOMPLETE", why,
            (" | merged -> %s" % merged["dir"]) if merged and merged.get("dir") else ""), flush=True)
        print("  session summary: %s" % os.path.join(base, "session.json"), flush=True)

    def merge(self, base, runs):
        tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xts_merge.py")
        out = os.path.join(base, "merged")
        if not os.path.exists(tool):
            return OrderedDict([("dir", None), ("error", "tools/xts_merge.py not found")])
        cmd = [sys.executable, "-B", tool] + [r.dir for r in runs] + ["-o", out]
        if any(((r.hello or {}).get("sysinfo") or {}).get("platform") == "host" or r.tag() == "host" for r in runs):
            cmd.append("--allow-host")
        try:
            p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1800)
        except (OSError, subprocess.SubprocessError) as e:
            return OrderedDict([("dir", None), ("error", str(e))])
        tail = p.stdout.decode("utf-8", "replace").strip().splitlines()[-3:]
        return OrderedDict([("dir", out if p.returncode in (0, 2) else None), ("exit", p.returncode),
                            ("runs", len(runs)), ("output_tail", tail)])


def open_socket(bind, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    for size in (64 << 20, 16 << 20, 4 << 20, 1 << 20):
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, size)
            break
        except OSError:
            continue
    s.bind((bind, port))
    s.settimeout(0.1)
    return s


def enable_drop_count(sock):
    """ask the kernel to attach the socket's drop counter to received datagrams (Linux SO_RXQ_OVFL)"""
    try:
        sock.setsockopt(socket.SOL_SOCKET, SO_RXQ_OVFL, 1)
        return hasattr(sock, "recvmsg")
    except OSError:
        return False


def recv_one(sock, rx):
    if rx.kernel_drops is None:
        return sock.recv(65535)
    data, anc, _, _ = sock.recvmsg(65535, socket.CMSG_SPACE(4))
    for level, typ, val in anc:             # present only once the counter is non-zero
        if level == socket.SOL_SOCKET and typ == SO_RXQ_OVFL and len(val) >= 4:
            rx.kernel_drops = struct.unpack("=I", val[:4])[0]
    return data


def serve(a, sock, rx, child=None):
    stop = {"flag": False}

    def on_signal(signum, frame):
        stop["flag"] = True
    old = {sig: signal.signal(sig, on_signal) for sig in (signal.SIGINT, signal.SIGTERM)}
    child_exit = None
    if enable_drop_count(sock):
        rx.kernel_drops = 0
    try:
        while not stop["flag"]:
            now = time.time()
            try:
                data = recv_one(sock, rx)
                rx.datagram(data, time.time())
                # drain whatever is queued without the timeout path
                sock.setblocking(False)
                try:
                    for _ in range(4096):
                        rx.datagram(recv_one(sock, rx), time.time())
                except (BlockingIOError, InterruptedError):
                    pass
                finally:
                    sock.settimeout(0.1)
            except socket.timeout:
                pass
            except InterruptedError:
                continue
            now = time.time()
            if child is not None and child_exit is None and child.poll() is not None:
                child_exit = now
            idle = a.timeout if a.timeout else None
            if child_exit is not None:
                idle = min(idle or 2.0, 2.0)
            rx.due(now, idle=idle)
            rx.sessions_due(now)
            rx.show_progress(now)
            if a.once and rx.all_done() and rx.sessions_settled():
                break
            if child_exit is not None and (rx.all_done() or not rx.runs) and now - child_exit > 2.0:
                break
    finally:
        for sig, h in old.items():
            signal.signal(sig, h)
    rx.due(time.time(), force=True)
    rx.sessions_due(time.time(), force=True)


def run_exec(a):
    sock = open_socket("127.0.0.1", a.port)
    port = sock.getsockname()[1]
    env = dict(os.environ, XTS_NET_HOST="127.0.0.1:%d" % port)
    child = subprocess.Popen(a.exec, env=env)
    rx = Receiver(a)
    serve(a, sock, rx, child)
    rc = child.wait()
    if not rx.runs:
        print("xts_recv: no run received from %s" % " ".join(a.exec), file=sys.stderr)
        return rc or 1
    return rc if rc else rx.exit_code()


def replay(a, path):
    rx = Receiver(a)
    rx.timed = False
    rx.single = True                # one pass replayed: its own directory, no session summary
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(CAP_MAGIC):
        print("xts_recv: %s is not a capture file" % path, file=sys.stderr)
        return 2
    o, now = len(CAP_MAGIC), time.time()
    while o + 4 <= len(data):
        n = struct.unpack_from(">I", data, o)[0]
        rx.datagram(data[o + 4:o + 4 + n], now)
        o += 4 + n
    rx.due(now, force=True)
    return rx.exit_code()


# ------------------------------------------------------------------------------------------------ selftest
def _synthetic_run(nonce, nrec=40, big=(5, 17), crash=False, copies=1, session=None):
    """the messages of a small run and its datagrams: (expected record lines, log lines, [(seq, [copies])]);
    session = (id, pass, passes) for a pass of a multi-pass session"""
    msgs, recs, logs = [], [], []
    hobj = {"sysinfo": {"suite": "xts", "console_tag": "selftest", "run_id": "%016x" % nonce},
            "profile": "selftest", "plan": {"blocks": 2, "cases": nrec}, "cores": {"test": 0},
            "build_id": "b", "run_id": "%016x" % nonce, "console_tag": "selftest"}
    if session:
        hobj["session"] = {"id": session[0], "pass": session[1], "passes": session[2]}
    hello = json.dumps(hobj).encode()
    msgs += [(1, hello)] * 3
    h = FNV_INIT
    for i in range(nrec):
        det = {"i": i}
        if i in big:
            det["pad"] = "x" * (3 * MAX_PAYLOAD + 123)      # fragmented over 4 frames
        r = OrderedDict([("run_id", "%016x" % nonce), ("seq", i + 1), ("family", "self.test"), ("case_id", "c%d" % i),
                         ("mnem", "add"), ("word", "7C642A14"), ("cond", OrderedDict([("sf", "64"), ("x", "a,b")])),
                         ("block", "r0:self.test:-:c%d" % (i // 20)), ("metric", "result"),
                         ("value", 1.5 if i % 3 else None), ("unit", ""), ("n", 3), ("min", None), ("median", None),
                         ("mad", None), ("fit_r2", None), ("status", "fail" if i == 7 else "pass"), ("detail", det)])
        line = json.dumps(r, separators=(",", ":")).encode()
        recs.append(line)
        h = fnv1a64(h, line)
        msgs.append((2, line))
        if i % 10 == 0:
            msgs.append((4, json.dumps({"block": r["block"], "case_index": i, "case_id": r["case_id"],
                                        "family": "self.test"}).encode()))
            log = ("[%10.3f] case %d" % (i, i)).encode()
            logs.append(log)
            msgs.append((3, log))
        if i % 20 == 19:
            msgs.append((7, json.dumps({"block": r["block"], "records": 20, "temps": {"cpu": 50.5, "gpu": 1,
                                                                                         "edram": 2, "board": 3}}).encode()))
    status = "complete"
    if crash:
        msgs.append((5, json.dumps({"pir": 0, "vector": "0700", "srr0": "0000000080001234",
                                    "crumb": {"family": "self.test", "case_id": "c39", "block": "r0:self.test:-:c1"},
                                    "log_tail": ["a"]}).encode()))
        status = "crashed"
    frames, seq = [], 1
    counts = Counter(TYPES[t] for t, _ in msgs)
    for m, (t, d) in enumerate(msgs, 1):
        fr = encode_message(nonce, seq, m, t, d, copies)
        frames += fr
        seq += len(fr)
    last_seq = seq - 1
    eobj = {"status": status, "last_seq": last_seq, "messages": dict(counts), "rec_hash": "%016x" % h,
            "send_failures": 0}
    if session:
        eobj["session"] = hobj["session"]
    end = json.dumps(eobj).encode()
    for k in range(3):
        fr = encode_message(nonce, seq, len(msgs) + 1 + k, 8, end, copies)
        frames += fr
        seq += len(fr)
    return recs, logs, frames, last_seq


def selftest():
    import io
    import contextlib
    tmp = tempfile.mkdtemp(prefix="xts_recv_selftest_")
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)

    def process(datagrams, quarantine_out=None):
        a = argparse.Namespace(out=os.path.join(tmp, "out"), run_dir=None, quiet=True, quarantine_out=quarantine_out,
                               once=True, timeout=0)
        rx = Receiver(a)
        with contextlib.redirect_stdout(io.StringIO()):
            for d in datagrams:
                rx.datagram(d, time.time())
            rx.due(time.time(), force=True)
        return rx

    def one(rx):
        return list(rx.runs.values())[0]

    def read(run, name):
        with open(os.path.join(run.dir, name), encoding="utf-8") as f:
            return f.read()

    # 1. clean
    recs, logs, frames, last_seq = _synthetic_run(0x1111)
    rx = process([c for _, cs in frames for c in cs])
    r = one(rx)
    check(r.receive["complete"] and r.receive["hash_ok"], "clean: complete + hash")
    check(read(r, "results.jsonl") == "".join(x.decode() + "\n" for x in recs), "clean: results.jsonl == stream")
    check(read(r, "run.log") == "".join(x.decode() + "\n" for x in logs), "clean: run.log")
    csv = read(r, "results.csv").splitlines()
    check(csv[0] + "\n" == CSV_HEADER and len(csv) == len(recs) + 1, "clean: csv rows")
    check(csv[2] == '0000000000001111,2,self.test,c1,add,7C642A14,"sf=64;x=a,b",r0:self.test:-:c0,result,1.5,,3,null,'
                    'null,null,null,pass', "clean: csv row format: " + csv[2])
    s = json.loads(read(r, "summary.json"))
    check(s["records"] == len(recs) and s["status"]["fail"] == 1 and s["by_family"]["self.test"]["pass"] == len(recs) - 1
          and s["by_axis_value"]["x=a,b"]["fail"] == 1, "clean: summary")
    check(read(r, "failures.jsonl").count("\n") == 1, "clean: failures.jsonl")
    check(json.loads(read(r, "sysinfo.json"))["console_tag"] == "selftest", "clean: sysinfo")
    check(read(r, "temps.csv").count("\n") == 3, "clean: temps.csv")
    check(r.dir.endswith(os.path.join("selftest", "0000000000001111")), "clean: run dir layout " + r.dir)
    check(rx.exit_code() == 0, "clean: exit code")
    # replay of the capture rebuilds the same results
    a = argparse.Namespace(out=os.path.join(tmp, "replay"), run_dir=os.path.join(tmp, "replayed"), quiet=True,
                           quarantine_out=None, once=True, timeout=0)
    with contextlib.redirect_stdout(io.StringIO()):
        rc = replay(a, os.path.join(r.dir, "capture.bin"))
    check(rc == 0 and open(os.path.join(tmp, "replayed", "results.jsonl")).read() == read(r, "results.jsonl"),
          "replay: same results")
    check(json.load(open(os.path.join(tmp, "replayed", "receive.json")))["missing_context"] is None,
          "replay: no arrival times, no loss context")

    # 2. duplicates (3 copies), reorder, corruption of copy 0 of every 7th frame
    recs, logs, frames, last_seq = _synthetic_run(0x2222, copies=3)
    rng = random.Random(5)
    dgs, ncorrupt = [], 0
    for k, (seq, cs) in enumerate(frames):
        for c, d in enumerate(cs):
            if c == 0 and k % 7 == 0:
                d = bytearray(d)
                d[-1] ^= 0x41
                d = bytes(d)
                ncorrupt += 1
            dgs.append(d)
    rng.shuffle(dgs)
    r = one(process(dgs))
    check(r.receive["complete"], "dup/reorder/corrupt: complete " + str(r.receive["reasons"]))
    check(r.receive["crc_errors"] == ncorrupt, "corrupt: crc_errors %d != %d" % (r.receive["crc_errors"], ncorrupt))
    check(r.receive["duplicates"] == 3 * len(frames) - ncorrupt - len(frames), "dup: duplicates %d" % r.receive["duplicates"])
    check(read(r, "results.jsonl") == "".join(x.decode() + "\n" for x in recs), "reorder: results in msg order")

    # 3. loss: drop some frames, including one fragment of a big record and one END message
    recs, logs, frames, last_seq = _synthetic_run(0x3333)
    big_seq = [seq for seq, cs in frames if HDR.unpack_from(cs[0])[9] > 1]
    drop = {3, 4, 10, big_seq[1], last_seq + 1}
    r = one(process([c for seq, cs in frames if seq not in drop for c in cs]))
    rc = r.receive
    check(not rc["complete"], "loss: incomplete")
    check([s for a_, b_ in rc["missing_seq"] for s in range(a_, b_ + 1)] == sorted(drop),
          "loss: missing_seq %s != %s" % (rc["missing_seq"], sorted(drop)))
    check(rc["missing_in_end_range"] == len(drop) - 1, "loss: missing within last_seq")
    check(len(rc["incomplete_messages"]) == 1 and rc["incomplete_messages"][0]["type"] == "REC", "loss: incomplete msg")
    check(rc["end_seen"] and not rc["hash_ok"], "loss: END from the 2nd copy, hash differs")
    # 3b. loss context from arrival times: 1 ms apart, a 250 ms silence before seq 20, seq 30..34 lost (a 60 ms hole)
    recs, logs, frames, last_seq = _synthetic_run(0x3434)
    a3 = argparse.Namespace(out=os.path.join(tmp, "out"), run_dir=None, quiet=True, quarantine_out=None, once=True,
                            timeout=0)
    rx3, t = Receiver(a3), 1000.0
    with contextlib.redirect_stdout(io.StringIO()):
        for seq, cs in frames:
            t += 0.25 if seq == 20 else 0.06 if seq == 35 else 0.001
            if not 30 <= seq <= 34:
                for c in cs:
                    rx3.datagram(c, t)
        rx3.due(t, force=True)
    ctx = one(rx3).receive["missing_context"]
    check(len(ctx) == 1 and ctx[0]["seq"] == [30, 34] and ctx[0]["quiet_before_ms"] == 250.0 and
          ctx[0]["hole_ms"] == 65.0 and ctx[0]["t_ms"] == 277.0, "loss context: %s" % ctx)
    # 4. no END at all
    recs, logs, frames, last_seq = _synthetic_run(0x4444)
    r = one(process([c for seq, cs in frames if seq <= last_seq for c in cs]))
    check(not r.receive["complete"] and "no END" in r.receive["reasons"], "no END: incomplete")
    # 5. crash report + quarantine suggestion
    recs, logs, frames, last_seq = _synthetic_run(0x5555, crash=True)
    qf = os.path.join(tmp, "quarantine.txt")
    with open(qf, "w") as f:
        f.write("# list\nexec.ppc lwz#1\n")
    rx = process([c for _, cs in frames for c in cs], quarantine_out=qf)
    r = one(rx)
    check(r.receive["complete"] and r.receive["status"] == "crashed", "crash: complete, status crashed")
    cj = json.loads(read(r, "crash.json"))
    check(cj["kind"] == "crash" and cj["vector"] == "0700", "crash: crash.json")
    q = open(qf).read()
    check("self.test c39  # crash vector 0700" in q and "exec.ppc lwz#1" in q, "crash: quarantine line appended")
    check(rx.exit_code() == 1, "crash: exit code 1 (status not complete)")
    with contextlib.redirect_stdout(io.StringIO()):
        process([c for _, cs in frames for c in cs], quarantine_out=qf)
    check(open(qf).read().count("self.test c39") == 1, "crash: quarantine line not duplicated")
    # 6. two interleaved runs (two consoles) + a real localhost socket
    r1 = _synthetic_run(0x6666)
    r2 = _synthetic_run(0x7777, nrec=10)
    dgs = [c for _, cs in r1[2] for c in cs]
    dgs2 = [c for _, cs in r2[2] for c in cs]
    mixed = []
    while dgs or dgs2:
        if dgs:
            mixed.append(dgs.pop(0))
        if dgs2:
            mixed.append(dgs2.pop(0))
    a = argparse.Namespace(out=os.path.join(tmp, "sock"), run_dir=None, quiet=True, quarantine_out=None, once=True,
                           timeout=0, port=0, bind="127.0.0.1")
    sock = open_socket("127.0.0.1", 0)
    port = sock.getsockname()[1]
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for k, d in enumerate(mixed):
        tx.sendto(d, ("127.0.0.1", port))
        if k % 200 == 199:
            time.sleep(0.002)
    rx = Receiver(a)
    with contextlib.redirect_stdout(io.StringIO()):
        serve(a, sock, rx)
    check(len(rx.runs) == 2 and all(rr.receive["complete"] for rr in rx.runs.values()), "socket: two complete runs")
    check(not sys.platform.startswith("linux") or all(rr.receive["kernel_drops"] == 0 for rr in rx.runs.values()),
          "socket: kernel drop counter read (0)")
    # 7. multi-pass session: 3 passes -> 3 run dirs, session.json, merge attempted; the receiver in one-run mode
    #    keeps listening between passes (over a real socket, the passes 1.5 s apart)
    sid = "00000000000a0001"
    passes = [_synthetic_run(0xA0001 + k, nrec=12, big=(), session=(sid, k + 1, 3)) for k in range(3)]
    a7 = argparse.Namespace(out=os.path.join(tmp, "sess"), run_dir=None, quiet=True, quarantine_out=None, once=True,
                            timeout=0, port=0, bind="127.0.0.1", session_wait=30, no_merge=False)
    sock = open_socket("127.0.0.1", 0)
    port = sock.getsockname()[1]

    def send_passes():
        tx7 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for k, pr in enumerate(passes):
            if k:
                time.sleep(1.5)               # longer than LINGER: pass k is finalized before pass k+1 starts
            for _, cs in pr[2]:
                for c in cs:
                    tx7.sendto(c, ("127.0.0.1", port))
    import threading
    th = threading.Thread(target=send_passes)
    th.start()
    rx = Receiver(a7)
    with contextlib.redirect_stdout(io.StringIO()):
        serve(a7, sock, rx)
    th.join()
    sj = os.path.join(tmp, "sess", "selftest", "session-" + sid, "session.json")
    check(len(rx.runs) == 3 and all(rr.receive["complete"] for rr in rx.runs.values()), "session: 3 complete passes")
    check(os.path.exists(sj), "session: session.json written")
    if os.path.exists(sj):
        so = json.load(open(sj))
        check(so["ok"] and so["received"] == 3 and [p["pass"] for p in so["pass_runs"]] == [1, 2, 3]
              and so["merged"] is not None, "session: session.json content %s" % so.get("ended_because"))
    check(rx.exit_code() == 0, "session: exit code 0")
    # 7b. a pass never arrives: the session ends after --session-wait, INCOMPLETE, exit code 1
    a7b = argparse.Namespace(out=os.path.join(tmp, "sess2"), run_dir=None, quiet=True, quarantine_out=None, once=True,
                             timeout=0, session_wait=5, no_merge=True)
    rx = Receiver(a7b)
    t = 2000.0
    with contextlib.redirect_stdout(io.StringIO()):
        for pr in passes[:2]:
            for _, cs in pr[2]:
                for c in cs:
                    rx.datagram(c, t)
            t += 2.0
            rx.due(t)
            rx.sessions_due(t)
        settled_early = rx.sessions_settled()
        rx.sessions_due(t + 6.0)
    check(not settled_early and rx.sessions_settled(), "session: waits, then ends after --session-wait")
    check(rx.exit_code() == 1, "session: missing pass -> exit code 1")
    # 7c. --run-dir: pass k in DIR/pass<k>, session.json in DIR; a crashed pass ends the session at once
    crashed = _synthetic_run(0xB0001, nrec=12, big=(), crash=True, session=("00000000000b0001", 1, 3))
    a7c = argparse.Namespace(out=os.path.join(tmp, "x"), run_dir=os.path.join(tmp, "rd"), quiet=True,
                             quarantine_out=None, once=True, timeout=0, session_wait=600, no_merge=True)
    rx = Receiver(a7c)
    with contextlib.redirect_stdout(io.StringIO()):
        for _, cs in crashed[2]:
            for c in cs:
                rx.datagram(c, 3000.0)
        rx.due(3002.0)
        rx.sessions_due(3002.0)
    check(rx.sessions_settled() and os.path.exists(os.path.join(tmp, "rd", "pass1", "results.jsonl"))
          and os.path.exists(os.path.join(tmp, "rd", "session.json")), "session: --run-dir layout, crash ends it")
    if fails:
        print("xts_recv selftest: FAIL\n  " + "\n  ".join(fails))
        return 1
    print("xts_recv selftest: PASS (clean, replay, dup/reorder/corrupt, loss + context, no END, crash + quarantine, 2 runs over a socket, multi-pass sessions)")
    import shutil
    shutil.rmtree(tmp, ignore_errors=True)
    return 0


# ------------------------------------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(description="receive XTS runs (UDP broadcast) and write run directories",
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__.split("\n\n", 1)[1])
    ap.add_argument("--out", default="xts-runs", help="output root: <out>/<console_tag>/<run_nonce>/ (default xts-runs)")
    ap.add_argument("--port", type=int, default=PORT, help="UDP port (default 36360; 0 = ephemeral, see --port-file)")
    ap.add_argument("--bind", default="0.0.0.0", help="address to listen on (default all)")
    ap.add_argument("--once", action="store_true", help="exit after the first run is written (the default)")
    ap.add_argument("--keep-listening", action="store_true", help="keep receiving runs until Ctrl-C")
    ap.add_argument("--run-dir", help="write the (single) run into exactly this directory")
    ap.add_argument("--port-file", help="write the bound port number into this file once listening")
    ap.add_argument("--timeout", type=float, default=0, help="finish a run after SEC seconds without frames (0 = never)")
    ap.add_argument("--session-wait", type=float, default=SESSION_WAIT,
                    help="multi-pass session: end it after SEC seconds without a new pass (default %d)" % SESSION_WAIT)
    ap.add_argument("--no-merge", action="store_true", help="multi-pass session: do not run xts_merge.py on the passes")
    ap.add_argument("--quarantine-out", help="append suggested quarantine.txt lines for CRASH/HANG reports to this file")
    ap.add_argument("--quiet", action="store_true", help="no progress line; only the verdict of incomplete runs")
    ap.add_argument("--replay", help="rebuild a run directory from a capture.bin")
    ap.add_argument("--selftest", action="store_true", help="run the built-in tests and exit")
    ap.add_argument("--exec", nargs=argparse.REMAINDER, help="run a command with XTS_NET_HOST pointing at this receiver")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    a.once = not a.keep_listening
    if a.replay:
        return replay(a, a.replay)
    if a.exec:
        return run_exec(a)
    try:
        sock = open_socket(a.bind, a.port)
    except OSError as e:
        print("xts_recv: cannot listen on %s:%d: %s" % (a.bind, a.port, e), file=sys.stderr)
        return 2
    port = sock.getsockname()[1]
    if a.port_file:
        with open(a.port_file + ".tmp", "w") as f:
            f.write("%d\n" % port)
        os.replace(a.port_file + ".tmp", a.port_file)
    if not a.quiet:
        print("xts_recv: listening on %s:%d (UDP), writing runs below %s%s" % (
            a.bind, port, a.run_dir or a.out, "" if a.once else " (until Ctrl-C)"), file=sys.stderr, flush=True)
    rx = Receiver(a)
    serve(a, sock, rx)
    if not rx.runs:
        print("xts_recv: no run received", file=sys.stderr)
        return 1
    return rx.exit_code()


if __name__ == "__main__":
    sys.exit(main())
