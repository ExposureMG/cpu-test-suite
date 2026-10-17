#!/usr/bin/env python3
"""check_records.py - validate XTS run output (console runs or host dry runs) against the record schema.

  python3 tools/check_records.py RUN_DIR [RUN_DIR...] [--keys-baseline FILE] [--max-records N] [--write-keys FILE]

RUN_DIR holds results.jsonl (+ summary.json). Checks:
  * every record has the schema fields with the right types (CONTRIBUTING.md "Record schema")
  * status, metric and unit are in the vocabularies (src/core/registry.c + every XTS_METRIC/XTS_UNIT registration
    in src/), and no record carries "vocab_warning"
  * measured timing records (not skip/trap/hang/crash) with a "fit" block carry every key of the unified fit schema
  * seq is unique and increasing, every record has the same condition-vector keys
  * summary.json counts agree with the records
  * --keys-baseline: the set of (family, case_id, metric) equals a snapshot written earlier with --write-keys
    (prints what was added / removed per family; any difference fails unless --allow-key-changes)
  * --max-records: the run has at most N records (e.g. the smoke profile budget)
Exit 0 = clean, 1 = problems found, 2 = usage / unreadable input.
"""
import argparse
import glob
import json
import math
import os
import re
import sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATUSES = {"pass", "fail", "trap", "skip", "noisy", "regressed", "hang", "crash", "info"}
FIELDS = {  # name: allowed types
    "run_id": (str,), "seq": (int,), "family": (str,), "case_id": (str,), "mnem": (str,), "word": (str,),
    "cond": (dict,), "block": (str,), "metric": (str,), "value": (int, float, type(None)), "unit": (str,),
    "n": (int, type(None)), "min": (int, float, type(None)), "median": (int, float, type(None)),
    "mad": (int, float, type(None)), "fit_r2": (int, float, type(None)), "status": (str,), "detail": (dict, type(None)),
}
FIT_KEYS = {"slope_ticks", "intercept_ticks", "r2", "max_resid_rel", "spread", "aba_shift", "slope_se", "depths",
            "ticks_med", "ticks_mad", "cycles_per_tick", "per_unit", "kernel_words_max"}
MAX_REPORT = 20


def load_vocab():
    """Built-in vocabularies from src/core/registry.c plus XTS_METRIC/XTS_UNIT registrations anywhere in src/."""
    metrics, units = set(), set()
    reg = open(os.path.join(ROOT, "src/core/registry.c")).read()
    for name, dest in (("metric_vocab", metrics), ("unit_vocab", units)):
        m = re.search(r"%s\[\]\s*=\s*\{(.*?)\};" % name, reg, re.S)
        if not m:
            sys.exit("check_records: cannot find %s in src/core/registry.c" % name)
        dest.update(re.findall(r'"([^"]*)"', m.group(1)))
    for path in glob.glob(os.path.join(ROOT, "src/**/*.c"), recursive=True):
        txt = open(path, errors="replace").read()
        metrics.update(re.findall(r'XTS_METRIC\(\s*"([^"]*)"\s*\)', txt))
        units.update(re.findall(r'XTS_UNIT\(\s*"([^"]*)"\s*\)', txt))
    return metrics, units


class Report:
    def __init__(self):
        self.problems = defaultdict(list)

    def add(self, kind, msg):
        self.problems[kind].append(msg)

    def print(self, label):
        n = sum(len(v) for v in self.problems.values())
        if not n:
            print("%s: OK" % label)
            return 0
        print("%s: %d problem(s)" % (label, n))
        for kind, msgs in sorted(self.problems.items()):
            print("  %s: %d" % (kind, len(msgs)))
            for m in msgs[:MAX_REPORT]:
                print("    " + m)
            if len(msgs) > MAX_REPORT:
                print("    ... %d more" % (len(msgs) - MAX_REPORT))
        return 1


def check_run(run_dir, metrics, units, rep):
    path = os.path.join(run_dir, "results.jsonl")
    if not os.path.exists(path):
        rep.add("input", "%s: no results.jsonl" % run_dir)
        return [], Counter()
    keys, status_count = [], Counter()
    by_family = defaultdict(Counter)
    last_seq, cond_keys = 0, None
    with open(path) as f:
        lines = f.readlines()
    for ln, line in enumerate(lines, 1):
        where = "%s:%d" % (os.path.basename(run_dir.rstrip("/")) or run_dir, ln)
        try:
            r = json.loads(line)
        except ValueError:
            if ln == len(lines):            # a truncated last line (power cut mid-append) is tolerated
                rep.add("truncated-tail", where)
                continue
            rep.add("json", where)
            continue
        for k, types in FIELDS.items():
            if k not in r:
                rep.add("missing-field", "%s: %s" % (where, k))
            elif not isinstance(r[k], types) or (types == (int,) and isinstance(r[k], bool)):
                rep.add("field-type", "%s: %s=%r" % (where, k, r[k]))
        st = r.get("status")
        if st not in STATUSES:
            rep.add("status", "%s: %r" % (where, st))
        if r.get("metric") not in metrics:
            rep.add("metric-vocab", "%s: %s %s metric %r" % (where, r.get("family"), r.get("case_id"), r.get("metric")))
        if r.get("unit") not in units:
            rep.add("unit-vocab", "%s: %s %s unit %r" % (where, r.get("family"), r.get("case_id"), r.get("unit")))
        det = r.get("detail") or {}
        if "vocab_warning" in det:
            rep.add("vocab-warning", "%s: %s %s" % (where, r.get("family"), r.get("case_id")))
        fit = det.get("fit") if isinstance(det, dict) else None
        if isinstance(fit, dict) and st in ("pass", "fail", "noisy", "regressed", "info"):   # measured records only
            miss = FIT_KEYS - set(fit)
            if miss:
                rep.add("fit-schema", "%s: %s %s missing %s" % (where, r.get("family"), r.get("case_id"), sorted(miss)))
        for k in ("value", "min", "median", "mad", "fit_r2"):
            v = r.get(k)
            if isinstance(v, float) and not math.isfinite(v):
                rep.add("non-finite", "%s: %s=%r" % (where, k, v))
        if st == "pass" and r.get("metric") not in ("result", "estimate", "info") and r.get("value") is None:
            rep.add("pass-without-value", "%s: %s %s %s" % (where, r.get("family"), r.get("case_id"), r.get("metric")))
        seq = r.get("seq")
        if isinstance(seq, int):
            if seq <= last_seq:
                rep.add("seq", "%s: seq %d after %d" % (where, seq, last_seq))
            last_seq = max(last_seq, seq)
        ck = tuple(sorted((r.get("cond") or {}).keys()))
        if cond_keys is None:
            cond_keys = ck
        elif ck != cond_keys:
            rep.add("cond-keys", "%s: %s" % (where, ck))
        keys.append((r.get("family", ""), r.get("case_id", ""), r.get("metric", "")))
        status_count[st] += 1
        by_family[r.get("family")][st] += 1
    summ = os.path.join(run_dir, "summary.json")
    if os.path.exists(summ):
        try:
            s = json.load(open(summ))
            if s.get("records") not in (None, len(keys)):
                rep.add("summary", "%s: records %s vs %d in results.jsonl" % (summ, s.get("records"), len(keys)))
            for st_name, cnt in (s.get("status") or {}).items():
                if cnt != status_count.get(st_name, 0):
                    rep.add("summary", "%s: status %s %s vs %d" % (summ, st_name, cnt, status_count.get(st_name, 0)))
        except ValueError:
            rep.add("summary", "%s: not JSON" % summ)
    return keys, status_count


def main():
    ap = argparse.ArgumentParser(description="validate XTS run output against the record schema")
    ap.add_argument("runs", nargs="+", help="run directories (containing results.jsonl)")
    ap.add_argument("--keys-baseline", help="file of family<TAB>case_id<TAB>metric lines to compare against")
    ap.add_argument("--allow-key-changes", action="store_true", help="report key differences without failing")
    ap.add_argument("--write-keys", help="write this run's key set (for a later --keys-baseline)")
    ap.add_argument("--max-records", type=int, help="fail if the run has more records than this")
    a = ap.parse_args()
    metrics, units = load_vocab()
    rep = Report()
    allkeys, statuses = [], Counter()
    for d in a.runs:
        k, s = check_run(d, metrics, units, rep)
        allkeys += k
        statuses += s
    if not allkeys and not rep.problems:
        print("check_records: no records in %s" % ", ".join(a.runs))
        return 2
    if a.max_records is not None and len(allkeys) > a.max_records:
        rep.add("budget", "%d records > --max-records %d" % (len(allkeys), a.max_records))
    keyset = set(allkeys)
    if a.write_keys:
        with open(a.write_keys, "w") as f:
            f.write("\n".join("\t".join(t) for t in sorted(keyset)))
    rc_keys = 0
    if a.keys_baseline:
        base = set(tuple(l.split("\t")) for l in open(a.keys_baseline).read().splitlines() if l)
        added, removed = keyset - base, base - keyset
        if added or removed:
            fam = defaultdict(lambda: [0, 0])
            for k in added:
                fam[k[0]][0] += 1
            for k in removed:
                fam[k[0]][1] += 1
            print("key set vs %s: +%d -%d" % (a.keys_baseline, len(added), len(removed)))
            for f_, (p, m) in sorted(fam.items()):
                print("  %-16s +%d -%d" % (f_, p, m))
            for k in sorted(removed)[:MAX_REPORT]:
                print("    - %s" % "\t".join(k))
            for k in sorted(added)[:MAX_REPORT]:
                print("    + %s" % "\t".join(k))
            rc_keys = 0 if a.allow_key_changes else 1
        else:
            print("key set vs %s: identical (%d keys)" % (a.keys_baseline, len(keyset)))
    print("records %d, keys %d, statuses %s" % (len(allkeys), len(keyset), dict(statuses)))
    rc = rep.print("check_records")
    return 1 if (rc or rc_keys) else 0


if __name__ == "__main__":
    sys.exit(main())
