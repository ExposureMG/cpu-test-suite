#!/usr/bin/env python3
"""findings_table.py - one CSV row per user-mode Xenon instruction, from a scheduling model and the ISA table.

    tools/findings_table.py <schedmodel.json> [-o docs/findings_instructions.csv] [--isa isa/xenon_isa.json]
    tools/findings_table.py --selftest

Inputs: schedmodel.json written by tools/xts_schedmodel.py (its "model.instructions" section, default condition) and
isa/xenon_isa.json. Rows: every ISA instruction with privilege "user" whose hardware status is not "illegal" (the
same set as schedmodel coverage.isa_user_instructions), in ISA order. Python 3 standard library only.

Columns (cycles at 3.2 GHz, values rounded to 2 decimals; the full statistics stay in schedmodel.json):
    mnem, isa, unit             ISA mnemonic (Rc/OE variants folded in), ISA set, ISA unit
    latency                     dependent latency per source operand slot of the base variant ("RA:2.02;RB:2.02",
                                lat.chain); empty when the instruction has no chain (loads, stores, branches, SPR moves)
    rthroughput                 reciprocal throughput of independent copies (tput.indep)
    issue_cost                  cycles one instruction occupies the issue stream (xts_schedmodel's choice: ucode, then
                                issue.pair, then lat.data, then sysops string ops at the smallest byte count)
    issue_cost_source           which of those gave issue_cost
    ucode_microcoded            yes / no / undetermined (ucode family majority verdict of the base variant)
    pipe_group                  unit.conflict pipe group of the instruction's scheduling class (see notes when the class
                                was assigned by heuristic rather than measured through its representative)
    load_use_latency_l1         loads: L1 load-to-use (ldst.lat lat.<form>, default cache state)
    notes                       ';'-separated flags: repeatable_noisy (a value accepted because >= 2 runs agree within
                                2 % although each run's fit was noisy), single_source (one run only), rthroughput
                                upper bound, disagreeing issue-cost sources, data-dependent bins, by-bytes costs of
                                string ops, L2 / misaligned load costs, FP/VR load-use method, Rc/OE variants that
                                differ from the base variant, SMT-free default condition only
"""
import argparse
import csv
import io
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
COLS = ["mnem", "isa", "unit", "latency", "rthroughput", "issue_cost", "issue_cost_source", "ucode_microcoded",
        "pipe_group", "load_use_latency_l1", "notes"]
ISSUE_SOURCES = ("ucode", "issue.pair", "lat.data", "sysops.string")
FPVR_LOAD_UNITS = ("lfd", "lfs", "lvx", "lvlx")


def val(a):
    return a.get("value") if isinstance(a, dict) else a


def fmt(x):
    if x is None:
        return ""
    return "%.2f" % x


def flags(a):
    """repeatable_noisy / single_source flags of one aggregate"""
    out = set()
    if isinstance(a, dict):
        if a.get("repeatable_noisy"):
            out.add("repeatable_noisy")
        if a.get("single_source"):
            out.add("single_source")
    return out


def user_instructions(isa_doc):
    return [i for i in isa_doc.get("instructions", [])
            if i.get("privilege") == "user" and (i.get("hardware") or {}).get("status") != "illegal"]


def by_mnem(insts):
    g = {}
    for v, o in insts.items():
        g.setdefault(o.get("mnem") or v, {})[v] = o
    return g


def variant_summary(v, o):
    lat = [val(a) for a in (o.get("latency") or {}).values() if val(a) is not None]
    parts = []
    if lat:
        parts.append("lat %s" % fmt(max(lat)))
    if val(o.get("rthroughput")) is not None:
        parts.append("tput %s" % fmt(val(o.get("rthroughput"))))
    if o.get("issue_cost") is not None:
        parts.append("issue %s" % fmt(o.get("issue_cost")))
    mc = (o.get("ucode") or {}).get("microcoded")
    if mc == "yes":
        parts.append("microcoded")
    return parts


def differs(a, b, rel=0.10):
    if a is None or b is None:
        return False
    return abs(a - b) > rel * max(abs(a), abs(b), 1e-9)


def row_for(isa_ins, variants):
    mnem = isa_ins["mnem"]
    base = variants.get(mnem) or (variants[sorted(variants)[0]] if variants else {})
    notes, fl = [], set()
    lat = base.get("latency") or {}
    lat_s = ";".join("%s:%s" % (s, fmt(val(a))) for s, a in lat.items() if val(a) is not None)
    for a in lat.values():
        fl |= flags(a)
        if isinstance(a, dict) and a.get("upper_bound"):
            notes.append("latency %s upper bound" % [s for s, b in lat.items() if b is a][0])
    rt = base.get("rthroughput")
    fl |= flags(rt)
    if isinstance(rt, dict) and rt.get("upper_bound"):
        notes.append("rthroughput upper bound (not slower than the 1-cycle filler)")
    iss = base.get("issue_cost_by_source") or {}
    src = next((s for s in ISSUE_SOURCES if val(iss.get(s)) is not None), "")
    ic = base.get("issue_cost")
    if src:
        fl |= flags(iss[src])
    others = ["%s %s" % (s, fmt(val(a))) for s, a in iss.items() if s != src and differs(val(a), ic)]
    if others:
        notes.append("other issue-cost sources: " + ", ".join(others))
    if ic is None and base:
        notes.append("no accepted issue cost (not timable, or runs disagree beyond 2 %)")
    uc = base.get("ucode") or {}
    mc = uc.get("microcoded")
    mc_s = "yes" if mc == "yes" else "no" if mc == "no" else "undetermined"
    if uc.get("not_timable"):
        notes.append("ucode not timable")
    ld = base.get("load_use_latency") or {}
    l1 = val(ld.get("l1"))
    fl |= flags(ld.get("l1"))
    if l1 is not None:
        extra = []
        for k in ("l2", "cold"):
            if val(ld.get(k)) is not None:
                extra.append("%s %s%s" % (k, fmt(val(ld.get(k))), " (single run)" if (ld.get(k) or {}).get("single_source") else ""))
        lc = val(ld.get("l1@line_cross"))
        if lc is not None:
            extra.append("line-crossing %s" % fmt(lc))
        if extra:
            notes.append("load-use " + ", ".join(extra))
        if base.get("sched_class") in FPVR_LOAD_UNITS or (base.get("isa_unit") == "memory" and base.get("reg_class") in ("fpr", "vr")) \
                or (isa_ins.get("isa") in ("altivec", "vmx128", "cell-vmx") or mnem.startswith("lf")):
            notes.append("FP/VR load-use measured through an fcmpu/vcmpequw->CR chain: upper bound (ldst.lat method)")
    ltp = val((base.get("load_throughput") or {}).get("line"))
    if ltp is not None and differs(ltp, ic):
        notes.append("load rthroughput (8 rotating destinations) %s" % fmt(ltp))
    stp = val((base.get("store_throughput") or {}).get("line"))
    if stp is not None and differs(stp, ic):
        notes.append("store rthroughput %s" % fmt(stp))
    byb = base.get("issue_cost_by_bytes") or {}
    if byb:
        notes.append("by bytes: " + ", ".join("%sB %s" % (k, fmt(val(a))) for k, a in byb.items() if val(a) is not None))
        for a in byb.values():
            fl |= flags(a)
    dd = []
    for shape, bins in (base.get("data_dependent") or {}).items():
        for b, x in bins.items():
            r, c = val(x.get("ratio")), val(x.get("cycles"))
            if r is not None and (r > 1.05 or r < 0.95):
                dd.append("%s/%s %sx (%s)" % (shape, b, fmt(r), fmt(c)))
    if dd:
        notes.append("data-dependent: " + ", ".join(dd))
    for v in sorted(variants):
        if v == mnem:
            continue
        o = variants[v]
        bl = [val(a) for a in lat.values() if val(a) is not None]
        ol = [val(a) for a in (o.get("latency") or {}).values() if val(a) is not None]
        mc_v = (o.get("ucode") or {}).get("microcoded")
        if (differs(max(ol) if ol else None, max(bl) if bl else None) or differs(o.get("issue_cost"), ic)
                or differs(val(o.get("rthroughput")), val(rt)) or (mc_v == "yes" and mc != "yes")):
            notes.append("%s: %s" % (v[len(mnem):] or v, " ".join(variant_summary(v, o))))
    if base.get("pipe_group") and base.get("pipe_group_source") != "class representative":
        notes.append("pipe_group inferred (class not measured directly)")
    if not base:
        notes.append("no measurement")
    notes = sorted(fl) + notes
    return [mnem, isa_ins.get("isa") or "", isa_ins.get("unit") or "", lat_s, fmt(val(rt)), fmt(ic), src, mc_s,
            base.get("pipe_group") or "", fmt(l1), "; ".join(notes)]


def build_rows(model_doc, isa_doc):
    insts = (model_doc.get("model") or {}).get("instructions") or {}
    g = by_mnem(insts)
    return [row_for(i, g.get(i["mnem"], {})) for i in user_instructions(isa_doc)]


def write(rows, out):
    w = csv.writer(out, lineterminator="\n")
    w.writerow(COLS)
    for r in rows:
        w.writerow(r)


def selftest():
    agg = lambda v, **k: dict(value=v, min=v, max=v, mad=0.0, n=4, runs=2, **k)  # noqa: E731
    isa = {"instructions": [
        {"mnem": "addx", "isa": "ppc", "unit": "integer", "privilege": "user", "hardware": {"status": "executes"}},
        {"mnem": "lwz", "isa": "ppc", "unit": "memory", "privilege": "user", "hardware": {"status": "executes"}},
        {"mnem": "lswi", "isa": "ppc", "unit": "memory", "privilege": "user", "hardware": {"status": "executes"}},
        {"mnem": "mtmsr", "isa": "ppc", "unit": "control", "privilege": "supervisor"},
        {"mnem": "vmulhw", "isa": "altivec", "unit": "vector", "privilege": "user", "hardware": {"status": "illegal"}}]}
    model = {"model": {"instructions": {
        "addx": {"mnem": "addx", "latency": {"RA": agg(2.0175), "RB": agg(2.0176)}, "rthroughput": agg(1.0195),
                 "issue_cost_by_source": {"ucode": agg(1.0198)}, "issue_cost": 1.0198, "ucode": {"microcoded": "no"},
                 "pipe_group": "int", "pipe_group_source": "class representative"},
        "addx.rc": {"mnem": "addx", "latency": {"RA": agg(14.0)}, "issue_cost": 14.0, "ucode": {"microcoded": "yes"}},
        "lwz": {"mnem": "lwz", "issue_cost_by_source": {"ucode": agg(1.02), "issue.pair": agg(1.5)}, "issue_cost": 1.02,
                "ucode": {"microcoded": "no"}, "load_use_latency": {"l1": agg(5.0348), "l2": agg(44.6, single_source=True)},
                "pipe_group": "ld"},
        "lswi": {"mnem": "lswi", "issue_cost_by_source": {"sysops.string": agg(15.01, repeatable_noisy=True)},
                 "issue_cost": 15.01, "issue_cost_by_bytes": {"4": agg(15.01), "32": agg(22.0)}}}}}
    rows = build_rows(model, isa)
    assert [r[0] for r in rows] == ["addx", "lwz", "lswi"], rows
    a = dict(zip(COLS, rows[0]))
    assert a["latency"] == "RA:2.02;RB:2.02" and a["rthroughput"] == "1.02" and a["issue_cost"] == "1.02", a
    assert a["issue_cost_source"] == "ucode" and a["ucode_microcoded"] == "no" and ".rc: lat 14.00 issue 14.00 microcoded" in a["notes"], a
    w = dict(zip(COLS, rows[1]))
    assert w["load_use_latency_l1"] == "5.03" and "l2 44.60 (single run)" in w["notes"] and "issue.pair 1.50" in w["notes"], w
    assert "pipe_group inferred" in w["notes"], w
    s = dict(zip(COLS, rows[2]))
    assert s["issue_cost_source"] == "sysops.string" and s["ucode_microcoded"] == "undetermined", s
    assert "by bytes: 4B 15.01, 32B 22.00" in s["notes"] and s["notes"].startswith("repeatable_noisy"), s
    buf = io.StringIO()
    write(rows, buf)
    assert buf.getvalue().splitlines()[0] == ",".join(COLS)
    print("findings_table selftest: ok")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("schedmodel", nargs="?", help="schedmodel.json from tools/xts_schedmodel.py")
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "docs", "findings_instructions.csv"),
                    help="output CSV ('-' = stdout; default docs/findings_instructions.csv)")
    ap.add_argument("--isa", default=os.path.join(ROOT, "isa", "xenon_isa.json"), help="ISA table")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.schedmodel:
        ap.error("schedmodel.json required")
    try:
        with open(a.schedmodel, encoding="utf-8") as f:
            model = json.load(f)
        with open(a.isa, encoding="utf-8") as f:
            isa = json.load(f)
    except (OSError, ValueError) as e:
        print("findings_table: %s" % e, file=sys.stderr)
        return 1
    rows = build_rows(model, isa)
    if a.out == "-":
        write(rows, sys.stdout)
    else:
        with open(a.out, "w", newline="", encoding="utf-8") as f:
            write(rows, f)
        print("findings_table: %d rows -> %s" % (len(rows), a.out), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
