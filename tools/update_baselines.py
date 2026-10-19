#!/usr/bin/env python3
"""update_baselines.py - refresh stale soft timing baselines in isa/xenon_isa.json from XTS hardware runs.

    tools/update_baselines.py xts-runs/<tag>/<run> [more runs ...]            # report only
    tools/update_baselines.py --apply xts-runs/<tag>/<run> ...                # rewrite isa/xenon_isa.json

The timing emitter compares a pass value with the instruction's JSON baseline (timing.<key>, e.g. "issue",
"latency", "fixed_operands.normal", "load_use_latency") and marks it "regressed" beyond the tolerance (10 %). Those
records carry detail.baseline_key and detail.baseline. This tool collects them (status pass or regressed, i.e.
measurements that passed the timing gate) at the default condition of every axis (vscr_nj is a dimension: the key
names separate its bins; code_align 16/32/64 put the loop top on the same fetch-group byte as 0 and count as default),
takes the median per (instruction, key), and replaces a baseline when:
  * the median differs from it by more than --tol (default 0.10, the gate's tolerance), and
  * the measurements of that key agree with each other within --spread (default 0.05 of the median).
Each changed instruction gets timing.sources.xts naming the console, the runs and the keys taken from them. The JSON
is rewritten in its own format (indent 1, key order kept), so the diff shows only the changed values.
Results (xts-runs/) stay out of git; only the JSON changes.
"""
import argparse
import collections
import json
import os
import statistics
import sys

ISA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "isa", "xenon_isa.json")
DIMENSIONS = {"vscr_nj"}


def same_as_default(axis, v, dflt):
    """code_align values that are multiples of 16 put the loop top on the same byte of its 16-byte fetch group as 0
    (frontend.loop: only the offset mod 16 matters), so they count as the default layout"""
    if axis in DIMENSIONS or v == dflt:
        return True
    if axis == "code_align":
        try:
            return int(v) % 16 == 0 and int(dflt) % 16 == 0
        except (TypeError, ValueError):
            return False
    return False


def default_cond(run):
    """the condition vector of the run's first record (self.* families run at every axis default)"""
    with open(os.path.join(run, "results.jsonl")) as f:
        return json.loads(f.readline())["cond"]


def collect(runs):
    vals = collections.defaultdict(list)       # (mnem, key) -> [values]
    old = {}
    tags, ids = set(), []
    for run in runs:
        dflt = default_cond(run)
        recv = os.path.join(run, "receive.json")
        if os.path.exists(recv):
            r = json.load(open(recv))
            tags.add(r.get("console_tag"))
            ids.append("%s (%s)" % (r.get("run_id"), r.get("profile")))
        with open(os.path.join(run, "results.jsonl")) as f:
            for line in f:
                x = json.loads(line)
                d = x.get("detail") if isinstance(x.get("detail"), dict) else {}
                key = d.get("baseline_key")
                if not key or x.get("status") not in ("pass", "regressed") or x.get("value") is None:
                    continue
                if not all(same_as_default(a, v, dflt.get(a)) for a, v in x["cond"].items()):
                    continue
                vals[(x["mnem"], key)].append(float(x["value"]))
                old[(x["mnem"], key)] = d.get("baseline")
    return vals, old, sorted(t for t in tags if t), ids


def get_path(o, key):
    for p in key.split("."):
        if not isinstance(o, dict) or p not in o:
            return None
        o = o[p]
    return o


def set_path(o, key, v):
    parts = key.split(".")
    for p in parts[:-1]:
        o = o[p]
    o[parts[-1]] = v


def main():
    ap = argparse.ArgumentParser(description="refresh stale timing baselines in isa/xenon_isa.json from XTS runs")
    ap.add_argument("runs", nargs="+", help="receiver run directories (xts-runs/<tag>/<run>)")
    ap.add_argument("--apply", action="store_true", help="rewrite isa/xenon_isa.json (default: report only)")
    ap.add_argument("--tol", type=float, default=0.10, help="replace when |median - baseline| / baseline > tol")
    ap.add_argument("--spread", type=float, default=0.05, help="skip keys whose values spread more than this")
    ap.add_argument("--isa", default=ISA)
    a = ap.parse_args()
    raw = open(a.isa, encoding="utf-8").read()
    J = json.loads(raw, object_pairs_hook=collections.OrderedDict)
    if json.dumps(J, indent=1, ensure_ascii=False) + "\n" != raw:
        sys.exit("update_baselines: %s does not round-trip (format changed?) - refusing to rewrite it" % a.isa)
    ins = J["instructions"]
    by = {x["mnem"]: x for x in (ins if isinstance(ins, list) else ins.values())}
    vals, old, tags, ids = collect(a.runs)
    changes = collections.defaultdict(list)
    kept = spread_skip = 0
    for (mnem, key), vs in sorted(vals.items()):
        e = by.get(mnem)
        cur = get_path((e or {}).get("timing") or {}, key)
        if not isinstance(cur, (int, float)) or cur <= 0:
            continue
        med = statistics.median(vs)
        if max(vs) - min(vs) > a.spread * med:
            spread_skip += 1
            continue
        if abs(med - cur) / cur <= a.tol:
            kept += 1
            continue
        changes[mnem].append((key, cur, round(med, 3), len(vs)))
    n = sum(len(v) for v in changes.values())
    print("update_baselines: %d (instruction, key) measured at the default condition; %d within %.0f %%, %d skipped "
          "(spread > %.0f %%), %d stale in %d instructions" % (len(vals), kept, 100 * a.tol, spread_skip,
                                                                100 * a.spread, n, len(changes)))
    by_key = collections.Counter(k for v in changes.values() for k, *_ in v)
    print("  stale keys: " + ", ".join("%s %d" % kv for kv in by_key.most_common()))
    for mnem in sorted(changes)[:12]:
        print("  %-12s %s" % (mnem, "  ".join("%s %g -> %g (n=%d)" % c for c in changes[mnem])))
    if len(changes) > 12:
        print("  ... %d more instructions" % (len(changes) - 12))
    if not a.apply or not changes:
        if changes:
            print("report only: rerun with --apply to rewrite %s" % os.path.relpath(a.isa))
        return 0
    note_runs = "; ".join(ids)
    for mnem, cs in changes.items():
        t = by[mnem]["timing"]
        for key, _, new, _ in cs:
            set_path(t, key, new)
        src = t.setdefault("sources", collections.OrderedDict())
        prev = src.get("xts", "")
        keys = ", ".join(k for k, *_ in cs)
        msg = "XTS hardware (%s, runs %s; loop top at byte 12, whole-line depths): %s" % (", ".join(tags), note_runs, keys)
        src["xts"] = msg if not prev else prev + " | " + msg
    with open(a.isa, "w", encoding="utf-8") as f:
        f.write(json.dumps(J, indent=1, ensure_ascii=False) + "\n")
    print("rewrote %s: %d baselines in %d instructions" % (os.path.relpath(a.isa), n, len(changes)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
