#!/usr/bin/env python3
"""xts_merge.py - merge XTS run directories (from one or more consoles) into one dataset.

Each console run writes  uda0:/xts/<console_tag>/<run_id>/  with sysinfo.json, results.jsonl, journal.txt, summary.json (and
results.csv, failures.jsonl, run.log, temps.csv).  Copy those directories to the PC (any layout) and:

    tools/xts_merge.py RUN_DIR_OR_PARENT [...] -o merged/            # parents are searched for results.jsonl
    tools/xts_merge.py usb/xts -o merged/ --allow-host                # also accept host dry-run output (flagged)
    tools/xts_merge.py --selftest

Output (in -o):
    merged.jsonl   every record once; the original fields plus "console_tag" and "console" (identity object) and "is_host"
    merged.csv     the same, flat: record columns, cond.<axis> columns, promoted detail members (producer, consumer, variant,
                   distance, unit), identity columns c_<name>, then "detail" (the JSON text)
    summary.json   per-console summary (identity, runs, record/status/family counts, duplicates, conflicts, journal state),
                   refused inputs and warnings;  the same is printed as a table

Rules:
    * records are identified by (console_tag, run_id, seq); a record seen again (a run copied twice, or a copy taken
      later) is dropped when identical and counted as a CONFLICT (first one kept) when its content differs.
    * host dry-run output (sysinfo platform "host", console_tag "host", records with backend host-model / "host dry run")
      is REFUSED unless --allow-host; with it the records carry is_host=true and the console summary says so.
    * a truncated last line (power cut while writing) is ignored and counted; unparsable lines elsewhere are counted.
    * console identity comes from sysinfo.json: console-level fields (type, PVR, CPU name, PIR, Xenos ID, DVE, RAM, SMC
      version, NAND) must agree for one console_tag - a mismatch (two consoles with the same label.txt) is a warning;
      run-level fields (profile, build id, git hash, speed code, measured TB Hz, boot count) are attached per run.

Exit status: 0 merged, 2 merged but some inputs were refused or had conflicts, 3 nothing merged, 1 usage / I/O error.
"""
import argparse
import csv
import hashlib
import json
import os
import shutil
import sys
import tempfile
from collections import Counter, OrderedDict

RECORD_COLS = ["console_tag", "run_id", "seq", "family", "case_id", "mnem", "word", "block", "metric", "value", "unit", "n",
               "min", "median", "mad", "fit_r2", "status"]
PROMOTED = ["producer", "consumer", "variant", "distance", "unit_text"]
# sysinfo.platform keys that identify the machine itself (must agree for one console_tag)
CONSOLE_KEYS = ["console_type", "console_type_code", "pvr", "cpu", "pir", "xenos_id", "dve", "dve_kind", "ram_bytes",
                "smc_version", "nand", "pci_bridge_rev"]
# run-level keys attached to every record
RUN_KEYS_TOP = ["profile", "build_id", "git_hash", "isa_sha256", "model_version", "boot"]
RUN_KEYS_PLAT = ["speed_code", "tb_hz_measured", "tb_hz_nominal", "hid0", "hid1", "hid4", "hid6"]
HOST_BACKENDS = ("host-model", "dryrun")


def jload(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def find_runs(paths):
    out, seen = [], set()
    for p in paths:
        p = os.path.abspath(p)
        if os.path.isfile(os.path.join(p, "results.jsonl")):
            cands = [p]
        else:
            cands = []
            for root, dirs, files in os.walk(p):
                if "results.jsonl" in files:
                    cands.append(root)
                    dirs[:] = []
        for c in cands:
            rp = os.path.realpath(c)
            if rp not in seen:
                seen.add(rp)
                out.append(c)
    return sorted(out)


def read_lines(path, stats):
    """yield parsed JSON objects of a .jsonl; a bad last line is a truncated tail, other bad lines are counted"""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.read().split("\n")
    except OSError:
        return
    while lines and not lines[-1].strip():
        lines.pop()
    for i, ln in enumerate(lines):
        if not ln.strip():
            continue
        try:
            yield json.loads(ln)
        except ValueError:
            if i == len(lines) - 1:
                stats["truncated_tail"] += 1
            else:
                stats["bad_lines"] += 1


def journal_state(path):
    st = {"present": False, "boots": 1, "begin": 0, "end": 0, "quarantined": 0, "done": False, "unfinished": []}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.read().split("\n")
    except OSError:
        return st
    st["present"] = True
    open_blocks = OrderedDict()
    for ln in lines:
        w = ln.split(" ", 2)
        if not w or not w[0]:
            continue
        if w[0] == "BOOT":
            st["boots"] += 1
        elif w[0] == "BEGIN" and len(w) > 1:
            st["begin"] += 1
            open_blocks[w[1]] = True
        elif w[0] == "END" and len(w) > 1:
            st["end"] += 1
            open_blocks.pop(w[1], None)
        elif w[0] == "QUAR":
            st["quarantined"] += 1
        elif w[0] == "DONE":
            st["done"] = True
    st["unfinished"] = list(open_blocks)[:5]
    return st


class Run:
    pass


def load_run(d):
    r = Run()
    r.dir = d
    r.sysinfo = jload(os.path.join(d, "sysinfo.json"))
    r.summary = jload(os.path.join(d, "summary.json"))
    r.journal = journal_state(os.path.join(d, "journal.txt"))
    plat = (r.sysinfo or {}).get("platform") or {}
    r.warnings = []
    if r.sysinfo is None:
        r.warnings.append("no sysinfo.json (identity unknown; console_tag taken from the directory layout)")
        r.tag = os.path.basename(os.path.dirname(os.path.abspath(d))) or "unknown"
        r.run_id = os.path.basename(os.path.abspath(d))
    else:
        r.tag = r.sysinfo.get("console_tag") or os.path.basename(os.path.dirname(os.path.abspath(d))) or "unknown"
        r.run_id = str(r.sysinfo.get("run_id") or os.path.basename(os.path.abspath(d)))
    r.stats = Counter()
    r.cond_keys = []
    host_recs, total, statuses = 0, 0, Counter()
    for rec in read_lines(os.path.join(d, "results.jsonl"), r.stats):
        total += 1
        det = rec.get("detail") or {}
        if det.get("backend") in HOST_BACKENDS or (isinstance(det.get("why"), str) and "host dry run" in det["why"]):
            host_recs += 1
        for k in (rec.get("cond") or {}):
            if k not in r.cond_keys:
                r.cond_keys.append(k)
        statuses[rec.get("status", "?")] += 1
    r.total = total
    r.host_records = host_recs
    reasons = []
    if plat.get("name") == "host":
        reasons.append("sysinfo platform is 'host'")
    if r.tag == "host":
        reasons.append("console_tag is 'host'")
    if r.sysinfo is None and host_recs:
        reasons.append("%d records say host dry run / host-model" % host_recs)
    elif r.sysinfo is not None and host_recs and plat.get("name") != "host":
        reasons.append("%d records say host dry run / host-model although sysinfo says console" % host_recs)
    r.host_reasons = reasons
    r.is_host = bool(reasons)
    if r.summary is not None and r.summary.get("records") is not None and r.summary["records"] != total and not r.stats["truncated_tail"]:
        r.warnings.append("summary.json counts %s records, results.jsonl has %d (incomplete copy?)" % (r.summary["records"], total))
    r.console_ident = {k: plat[k] for k in CONSOLE_KEYS if k in plat}
    run_ident = {}
    for k in RUN_KEYS_TOP:
        if r.sysinfo and k in r.sysinfo:
            run_ident[k] = r.sysinfo[k]
    for k in RUN_KEYS_PLAT:
        if k in plat:
            run_ident[k] = plat[k]
    r.run_ident = run_ident
    r.status_counts = dict(statuses)
    return r


def flat_ident(console_ident, run_ident):
    d = OrderedDict()
    for k in CONSOLE_KEYS:
        if k in console_ident:
            d["c_" + k] = console_ident[k]
    for k in RUN_KEYS_TOP + RUN_KEYS_PLAT:
        if k in run_ident:
            d["c_" + k] = run_ident[k]
    return d


def cell(v):
    if v is None:
        return ""
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (dict, list)):
        return json.dumps(v, separators=(",", ":"))
    return v


def merge(inputs, outdir, allow_host=False, log=print):
    """returns (exit_code, summary dict)"""
    run_dirs = find_runs(inputs)
    summary = OrderedDict(inputs=[os.path.abspath(p) for p in inputs], allow_host=allow_host, refused=[], warnings=[], consoles=OrderedDict())
    if not run_dirs:
        log("no results.jsonl found under: " + ", ".join(inputs))
        return 3, summary
    runs = [load_run(d) for d in run_dirs]
    accepted = []
    for r in runs:
        if r.is_host and not allow_host:
            summary["refused"].append({"dir": r.dir, "console_tag": r.tag, "run_id": r.run_id, "records": r.total,
                                       "reason": "host dry-run output (" + "; ".join(r.host_reasons) + ")"})
            log("REFUSED %s: host dry-run output (%s); use --allow-host to include it" % (r.dir, "; ".join(r.host_reasons)))
            continue
        accepted.append(r)
    if not accepted:
        log("nothing to merge")
        return 3, summary
    # the more complete copy of a run first, so its records win
    accepted.sort(key=lambda r: (r.tag, r.run_id, -r.total, r.dir))
    cond_keys = []
    for r in accepted:
        for k in r.cond_keys:
            if k not in cond_keys:
                cond_keys.append(k)
    # identity of each console, with a consistency check
    ident = OrderedDict()
    for r in accepted:
        c = ident.setdefault(r.tag, {"console": dict(r.console_ident), "is_host": r.is_host})
        for k, v in r.console_ident.items():
            if k in c["console"] and c["console"][k] != v:
                summary["warnings"].append("console_tag %s: %s differs between runs (%r vs %r) - two consoles with the same label?" % (r.tag, k, c["console"][k], v))
            c["console"].setdefault(k, v)
        c["is_host"] = c["is_host"] or r.is_host
    os.makedirs(outdir, exist_ok=True)
    id_cols = []
    for r in accepted:
        for k in flat_ident(ident[r.tag]["console"], r.run_ident):
            if k not in id_cols:
                id_cols.append(k)
    header = RECORD_COLS + ["cond." + k for k in cond_keys] + ["detail." + k for k in PROMOTED] + id_cols + ["is_host", "detail"]
    seen = {}
    conflicts = Counter()
    conflict_examples = []
    per = OrderedDict()
    fj = open(os.path.join(outdir, "merged.jsonl"), "w", encoding="utf-8")
    fc = open(os.path.join(outdir, "merged.csv"), "w", encoding="utf-8", newline="")
    w = csv.writer(fc)
    w.writerow(header)
    nout = 0
    try:
        for r in accepted:
            cs = per.setdefault(r.tag, OrderedDict(identity=ident[r.tag]["console"], is_host=ident[r.tag]["is_host"], runs=[], records=0,
                                                   duplicates_dropped=0, conflicts=0, bad_lines=0, truncated_tails=0,
                                                   statuses=Counter(), families=Counter()))
            rs = OrderedDict(run_id=r.run_id, dir=r.dir, records_in_file=r.total, records_merged=0, duplicates_dropped=0,
                             conflicts=0, host=r.is_host, run=r.run_ident, journal=r.journal, warnings=r.warnings)
            for wmsg in r.warnings:
                summary["warnings"].append("%s: %s" % (r.dir, wmsg))
            flat = flat_ident(ident[r.tag]["console"], r.run_ident)
            local = Counter()
            for rec in read_lines(os.path.join(r.dir, "results.jsonl"), local):
                rid = str(rec.get("run_id", r.run_id))
                key = (r.tag, rid, rec.get("seq"))
                h = hashlib.sha1(json.dumps(rec, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
                if key in seen:
                    if seen[key] == h:
                        rs["duplicates_dropped"] += 1
                        cs["duplicates_dropped"] += 1
                    else:
                        rs["conflicts"] += 1
                        cs["conflicts"] += 1
                        conflicts[r.tag] += 1
                        if len(conflict_examples) < 10:
                            conflict_examples.append({"console_tag": r.tag, "run_id": rid, "seq": rec.get("seq"), "dir": r.dir})
                    continue
                seen[key] = h
                det = rec.get("detail") if isinstance(rec.get("detail"), dict) else {}
                out = OrderedDict()
                out["console_tag"] = r.tag
                for k, v in rec.items():
                    out[k] = v
                out["console"] = ident[r.tag]["console"]
                out["is_host"] = r.is_host
                fj.write(json.dumps(out, separators=(",", ":")) + "\n")
                row = [cell(r.tag)]
                for k in RECORD_COLS[1:]:
                    row.append(cell(rec.get(k)))
                for k in cond_keys:
                    row.append(cell((rec.get("cond") or {}).get(k)))
                for k in PROMOTED:
                    row.append(cell(det.get("unit") if k == "unit_text" else det.get(k)))
                for k in id_cols:
                    row.append(cell(flat.get(k)))
                row.append("true" if r.is_host else "false")
                row.append(json.dumps(rec.get("detail"), separators=(",", ":")) if "detail" in rec else "")
                w.writerow(row)
                nout += 1
                rs["records_merged"] += 1
                cs["records"] += 1
                cs["statuses"][rec.get("status", "?")] += 1
                cs["families"][rec.get("family", "?")] += 1
            cs["bad_lines"] += local["bad_lines"]
            cs["truncated_tails"] += local["truncated_tail"]
            if local["truncated_tail"]:
                rs["warnings"].append("truncated last line ignored")
                summary["warnings"].append("%s: truncated last line ignored" % r.dir)
            if local["bad_lines"]:
                rs["warnings"].append("%d unparsable lines skipped" % local["bad_lines"])
                summary["warnings"].append("%s: %d unparsable lines skipped" % (r.dir, local["bad_lines"]))
            cs["runs"].append(rs)
    finally:
        fj.close()
        fc.close()
    for tag, cs in per.items():
        cs["statuses"] = dict(cs["statuses"])
        cs["families"] = dict(cs["families"])
        summary["consoles"][tag] = cs
    summary["records"] = nout
    summary["conflict_examples"] = conflict_examples
    with open(os.path.join(outdir, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1, default=str)
        f.write("\n")
    code = 2 if (summary["refused"] or sum(conflicts.values())) else 0
    return code, summary


def print_summary(summary, out=sys.stdout):
    out.write("merged %d records from %d console(s)\n" % (summary.get("records", 0), len(summary["consoles"])))
    for tag, cs in summary["consoles"].items():
        idn = cs["identity"]
        out.write("\n%s%s\n" % (tag, "  [HOST DRY RUN]" if cs["is_host"] else ""))
        out.write("  %s %s pvr=%s xenos=%s ram=%s smc=%s nand=%s\n" % (idn.get("console_type", "?"), idn.get("cpu", "?"), idn.get("pvr", "?"),
                  idn.get("xenos_id", "?"), idn.get("ram_bytes", "?"), idn.get("smc_version", "?"), idn.get("nand", "?")))
        st = " ".join("%s=%d" % kv for kv in sorted(cs["statuses"].items()))
        out.write("  records %d  (dup dropped %d, conflicts %d)  %s\n" % (cs["records"], cs["duplicates_dropped"], cs["conflicts"], st))
        for rs in cs["runs"]:
            j = rs["journal"]
            out.write("  run %-8s %6d/%-6d profile=%s build=%s speed=%s boots=%d%s%s\n" % (
                rs["run_id"], rs["records_merged"], rs["records_in_file"], rs["run"].get("profile", "?"), rs["run"].get("build_id", "?"),
                rs["run"].get("speed_code", "?"), j["boots"], " done" if j["done"] else " NOT-DONE",
                " quarantined=%d" % j["quarantined"] if j["quarantined"] else ""))
        fams = ", ".join("%s:%d" % kv for kv in sorted(cs["families"].items())[:12])
        out.write("  families %s%s\n" % (fams, " ..." if len(cs["families"]) > 12 else ""))
    for r in summary["refused"]:
        out.write("\nREFUSED %s (%d records): %s\n" % (r["dir"], r["records"], r["reason"]))
    for wmsg in summary["warnings"]:
        out.write("warning: %s\n" % wmsg)


# ------------------------------------------------------------------------------------------------------------ self test
def _write_run(base, tag, run_id, recs, host=False, ident=None, extra_line=None, truncate=False, summary_ok=True):
    d = os.path.join(base, tag, run_id)
    os.makedirs(d, exist_ok=True)
    plat = {"name": "host" if host else "console"}
    if not host:
        plat.update(ident or {})
    si = {"suite": "xts", "profile": "sched", "build_id": "b1", "git_hash": "abc", "run_id": run_id, "console_tag": tag, "boot": 1,
          "platform": plat}
    with open(os.path.join(d, "sysinfo.json"), "w") as f:
        json.dump(si, f)
    with open(os.path.join(d, "results.jsonl"), "w") as f:
        for i, r in enumerate(recs):
            line = json.dumps(r)
            if truncate and i == len(recs) - 1:
                line = line[:len(line) // 2]
            f.write(line + ("" if truncate and i == len(recs) - 1 else "\n"))
        if extra_line:
            f.write(extra_line + "\n")
    if summary_ok:
        with open(os.path.join(d, "summary.json"), "w") as f:
            json.dump({"run_id": run_id, "records": len(recs)}, f)
    with open(os.path.join(d, "journal.txt"), "w") as f:
        f.write("START sched b1\nBEGIN k1\nEND k1 seq=%d\nBOOT 2\nBEGIN k2\nDONE\n" % len(recs))
    return d


def _rec(run_id, seq, family="branch", metric="penalty", value=1.5, status="pass", backend=None, cond=None):
    d = {"variant": "v", "producer": "mtctr", "consumer": "bcctr", "distance": seq}
    if backend:
        d["backend"] = backend
    return {"run_id": run_id, "seq": seq, "family": family, "case_id": "c%d/x" % seq, "mnem": "bcctrx", "word": "4E800420",
            "cond": cond or {"sf": "64", "code_align": "0"}, "block": "r0:b:-:c0", "metric": metric, "value": value, "unit": "cycles",
            "n": 30, "min": None, "median": value, "mad": 0.01, "fit_r2": 0.999, "status": status, "detail": d}


def selftest():
    ok = True

    def check(c, msg):
        nonlocal ok
        print(("  ok   " if c else "  FAIL ") + msg)
        ok = ok and c

    tmp = tempfile.mkdtemp(prefix="xts_merge_selftest_")
    try:
        ia = {"console_type": "jasper", "pvr": "00710500", "cpu": "Waternoose", "xenos_id": "5841", "ram_bytes": 536870912}
        ib = {"console_type": "falcon", "pvr": "00710200", "cpu": "Loki", "xenos_id": "5821", "ram_bytes": 536870912}
        usb = os.path.join(tmp, "usb")
        r1 = [_rec("0001", i) for i in range(1, 6)]
        _write_run(usb, "jasper-A", "0001", r1)
        _write_run(os.path.join(tmp, "copy2"), "jasper-A", "0001", r1 + [_rec("0001", 6)])      # same run copied later, one more record
        _write_run(usb, "jasper-A", "0002", [_rec("0002", i, family="sysops") for i in range(1, 4)])
        _write_run(usb, "falcon-B", "0001", [_rec("0001", i, status="noisy") for i in range(1, 5)], truncate=True)
        host_recs = [_rec("0001", i, backend="host-model") for i in range(1, 4)]
        _write_run(os.path.join(tmp, "dry"), "host", "0001", host_recs, host=True)
        # fix identity: _write_run takes it as a parameter on console runs only
        for tag, ident in (("jasper-A", ia), ("falcon-B", ib)):
            for base in (usb, os.path.join(tmp, "copy2")):
                for rid in ("0001", "0002"):
                    p = os.path.join(base, tag, rid, "sysinfo.json")
                    if os.path.isfile(p):
                        si = json.load(open(p))
                        si["platform"].update(ident)
                        json.dump(si, open(p, "w"))
        out = os.path.join(tmp, "out")
        msgs = []
        code, summ = merge([usb, os.path.join(tmp, "copy2"), os.path.join(tmp, "dry")], out, False, log=msgs.append)
        check(code == 2, "host run refused -> exit 2 (got %d)" % code)
        check(any("REFUSED" in m for m in msgs) and len(summ["refused"]) == 1, "refusal reported")
        recs = [json.loads(l) for l in open(os.path.join(out, "merged.jsonl"))]
        keys = [(r["console_tag"], r["run_id"], r["seq"]) for r in recs]
        check(len(keys) == len(set(keys)), "no duplicate (console_tag, run_id, seq)")
        a1 = [k for k in keys if k[:2] == ("jasper-A", "0001")]
        check(len(a1) == 6, "run copied twice: union of both copies, 6 records (got %d)" % len(a1))
        b1 = [k for k in keys if k[0] == "falcon-B"]
        check(len(b1) == 3, "truncated last line ignored: 3 of 4 records (got %d)" % len(b1))
        check(not any(r["is_host"] for r in recs) and not any(r["console_tag"] == "host" for r in recs), "no host records merged")
        check(all("console" in r and r["console"].get("pvr") for r in recs), "identity object on every record")
        rows = list(csv.DictReader(open(os.path.join(out, "merged.csv"), newline="")))
        check(len(rows) == len(recs), "csv has one row per record")
        check({"console_tag", "cond.sf", "cond.code_align", "detail.variant", "c_pvr", "c_cpu", "c_profile", "is_host", "detail"} <= set(rows[0]), "csv columns")
        check(rows[0]["c_pvr"] in ("00710500", "00710200") and rows[0]["detail.producer"] == "mtctr", "csv identity + promoted detail values")
        check(summ["consoles"]["jasper-A"]["duplicates_dropped"] == 5 and len(summ["consoles"]["jasper-A"]["runs"]) == 3, "per-console summary: duplicates and runs")
        check(summ["consoles"]["falcon-B"]["identity"]["cpu"] == "Loki", "per-console identity")
        check(any("truncated" in m for m in summ["warnings"]), "truncation warned")
        check(json.load(open(os.path.join(out, "summary.json")))["records"] == len(recs), "summary.json written")
        # --allow-host
        out2 = os.path.join(tmp, "out2")
        code2, summ2 = merge([usb, os.path.join(tmp, "dry")], out2, True, log=lambda m: None)
        recs2 = [json.loads(l) for l in open(os.path.join(out2, "merged.jsonl"))]
        check(code2 == 0 and any(r["is_host"] for r in recs2) and summ2["consoles"]["host"]["is_host"], "--allow-host keeps host data, flagged")
        # only host input and no flag: nothing merged
        code3, _ = merge([os.path.join(tmp, "dry")], os.path.join(tmp, "out3"), False, log=lambda m: None)
        check(code3 == 3 and not os.path.exists(os.path.join(tmp, "out3", "merged.jsonl")), "host-only input: exit 3, no output")
        # a console run whose records say host-model is refused too
        _write_run(os.path.join(tmp, "mixed"), "jasper-A", "0009", [_rec("0009", 1, backend="host-model")], ident=ia)
        code4, s4 = merge([os.path.join(tmp, "mixed")], os.path.join(tmp, "out4"), False, log=lambda m: None)
        check(code4 == 3 and len(s4["refused"]) == 1, "console sysinfo but host-model records: refused")
        # conflict: same key, different content
        r1b = [dict(r1[0], value=9.9)] + r1[1:]
        _write_run(os.path.join(tmp, "conf"), "jasper-A", "0001", r1b)
        for rid in ("0001",):
            p = os.path.join(tmp, "conf", "jasper-A", rid, "sysinfo.json")
            si = json.load(open(p)); si["platform"].update(ia); json.dump(si, open(p, "w"))
        code5, s5 = merge([usb, os.path.join(tmp, "conf")], os.path.join(tmp, "out5"), False, log=lambda m: None)
        check(code5 == 2 and s5["consoles"]["jasper-A"]["conflicts"] == 1, "conflicting duplicate counted, first kept (exit 2)")
        # same console_tag, different hardware -> warning
        bad = dict(ia, pvr="00700000")
        _write_run(os.path.join(tmp, "clash"), "jasper-A", "0003", [_rec("0003", 1)])
        p = os.path.join(tmp, "clash", "jasper-A", "0003", "sysinfo.json")
        si = json.load(open(p)); si["platform"].update(bad); json.dump(si, open(p, "w"))
        code6, s6 = merge([usb, os.path.join(tmp, "clash")], os.path.join(tmp, "out6"), False, log=lambda m: None)
        check(any("pvr differs" in m for m in s6["warnings"]), "identity clash under one console_tag warned")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("selftest %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="merge XTS run directories into one dataset", formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("inputs", nargs="*", help="run directories, or parents that contain them (searched for results.jsonl)")
    ap.add_argument("-o", "--out", default="merged", help="output directory (default: merged)")
    ap.add_argument("--allow-host", action="store_true", help="accept host dry-run output (flagged is_host)")
    ap.add_argument("--selftest", action="store_true", help="run the built-in test on synthetic data and exit")
    ap.add_argument("-q", "--quiet", action="store_true", help="do not print the summary table")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.inputs:
        ap.error("no input directories")
    try:
        code, summ = merge(a.inputs, a.out, a.allow_host, log=lambda m: print(m, file=sys.stderr))
    except OSError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    if summ["consoles"] and not a.quiet:
        print_summary(summ)
    return code


if __name__ == "__main__":
    sys.exit(main())
