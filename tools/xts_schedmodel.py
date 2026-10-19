#!/usr/bin/env python3
"""xts_schedmodel.py - turn XTS measurements into a machine model a scheduling compiler can use.

Input: merged.jsonl from tools/xts_merge.py (records carry console_tag / console identity), a directory that holds one,
or raw run directories (results.jsonl + sysinfo.json; parents are searched like xts_merge does). Several inputs mix.

    tools/xts_schedmodel.py merged/ -o model/                       # schedmodel.json + schedmodel.csv
    tools/xts_schedmodel.py usb/xts -o model/ --td                  # + PPCScheduleXenon.td (LLVM SchedMachineModel sketch)
    tools/xts_schedmodel.py merged/ --console jasper-A --include-noisy
    tools/xts_schedmodel.py --selftest

Families read (metric names / detail keys as their emitters write them):
    lat.chain (latency per source slot)  tput.indep (rthroughput + N curve, depth)  lat.data (issue_cost / latency / ratio
    per operand bin)  ucode (issue_cost, extra = dispatch cycles, microcoded flags)  issue.pair (alone issue_cost, ordered
    pair cycles + verdict)  unit.conflict (alone rthroughput, overlap -> pipe graph, busy penalty)  bypass.matrix (penalty /
    extra per producer x consumer x slot, distance sweeps, mtctr>bctr / mtlr>blr)  branch  sysops  calib  ldst.lat
    mem.hier  and the SMT info records (metric smt_*) of the sibling / other_cores / speed axes.

Rules:
    * records are deduplicated on (console_tag, run_id, seq) like xts_merge (identical copy dropped, differing copy = conflict,
      first kept); host dry-run data is refused unless --allow-host (it carries no measurements anyway).
    * status filter: pass, nonlinear (repeatable, but the cost per unit depends on the depth: the value is the
      least-squares average, fit.marginal_cycles has the steps), regressed (timing gate passed, only the JSON baseline
      disagrees) and info (kept, flagged: e.g. front-end bound = upper bound) by default; --include-noisy adds noisy;
      --statuses sets the list explicitly. A noisy measurement that at least two runs reproduce within --repeat-tol
      (default 2 %) of their median is accepted too and tagged repeatable_noisy (coverage.noisy_promoted counts them).
    * condition sections: "default" = every axis at its default value (values[0] of src/conds), except the axes a family
      measures along (ldst.lat: cache, data_align; lat.data: vscr_nj; calib: speed), which become keys inside the model.
      Every other condition vector gets its own section under "conditions", and ratios against default under
      "condition_effects" (SMT axes sibling / other_cores / thread_prio also under "smt").
    * every value is {value (median), min, max, mad, n (records), runs}; single_source when one run gave it; with several
      consoles by_console medians + console_spread, and a per-console model under "consoles".

Exit status: 0 model written, 3 inputs had no usable measurement (files are still written), 1 usage / I/O error.
"""
import argparse
import csv
import glob
import contextlib
import hashlib
import io
import json
import math
import os
import re
import shutil
import statistics
import sys
import tempfile
from collections import Counter, OrderedDict, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import xts_merge as XM  # noqa: E402  (same directory: find_runs, read_lines, load_run, host detection)

TOOL_VERSION = 1
DEFAULT_STATUSES = ("pass", "nonlinear", "regressed", "info")
MEASURE_FAMILIES = ("lat.chain", "tput.indep", "lat.data", "ucode", "issue.pair", "unit.conflict", "bypass.matrix", "branch",
                    "sysops", "calib", "ldst.lat", "mem.hier")
# axes a family measures ALONG (their value is a key inside the model, not a separate condition section)
DIM_AXES = {"ldst.lat": ("cache", "data_align"), "lat.data": ("vscr_nj",), "calib": ("speed",)}
SMT_AXES = ("sibling", "other_cores", "thread_prio")
RATIO_METRICS = ("latency", "rthroughput", "issue_cost", "cycles", "bytes_per_cycle")
FALLBACK_DEFAULTS = {"sf": "64", "fpscr_rn": "rn", "fpscr_en": "none", "vscr_nj": "0", "cache": "default", "code_align": "0",
                     "data_align": "aligned", "thread_prio": "default", "unroll": "default", "chains": "default",
                     "speed": "default", "other_cores": "idle", "sibling": "idle"}
# src/tests/sched_classes.c: class -> (representative JSON mnemonic, variant suffix)
CLASS_REP = {
    "filler": ("addi", ""), "int": ("addx", ""), "int_rc": ("addx", ".rc"), "logic": ("orx", ""), "rotimm": ("rlwinmx", ""),
    "cmp": ("cmpi", ""), "carry": ("addex", ""), "recimm": ("andi.", ""), "mulli": ("mulli", ""), "mul": ("mullwx", ""),
    "mul64": ("mulldx", ""), "div": ("divwx", ""), "div64": ("divdx", ""), "varshift": ("slwx", ""), "lwz": ("lwz", ""),
    "ld": ("ld", ""), "lbz": ("lbz", ""), "lha": ("lha", ""), "lwzx": ("lwzx", ""), "lwzu": ("lwzu", ""), "lfd": ("lfd", ""),
    "lfs": ("lfs", ""), "lvx": ("lvx128", ""), "lvlx": ("lvlx", ""), "stw": ("stw", ""), "std": ("std", ""),
    "stwu": ("stwu", ""), "stfd": ("stfd", ""), "stfiwx": ("stfiwx", ""), "stvx": ("stvx128", ""), "fp": ("faddx", ""),
    "fma": ("fmaddx", ""), "fconv": ("fctiwzx", ""), "fcmp": ("fcmpu", ""), "fdiv": ("fdivx", ""), "fsqrt": ("fsqrtx", ""),
    "vlogic": ("vand128", ""), "vint": ("vadduwm", ""), "vperm": ("vperm128", ""), "vsplt": ("vspltw128", ""),
    "vfp": ("vaddfp128", ""), "vfma": ("vmaddfp128", ""), "vest": ("vrefp128", ""), "vdot": ("vmsum4fp128", ""),
    "vconv": ("vcfpsxws128", ""), "vcmp": ("vcmpeqfp128", ""), "vcmp_rc": ("vcmpequw128", ".rc"), "crlogic": ("creqv", ""),
    "mcrf": ("mcrf", ""), "mfcr": ("mfcr", ""), "mtcrf": ("mtcrf", ""), "mflr": ("mfspr", ""), "mtlr": ("mtspr", ""),
    "br_t": ("bcx", ""), "br_nt": ("bcx", ""), "b": ("bx", ""),
}
# class without its own unit.conflict measurement -> the measured class it most likely shares a pipe with
CLASS_FALLBACK = {"logic": "int", "rotimm": "int", "carry": "int", "int_rc": "int", "mulli": "mul", "mul64": "mul",
                  "div64": "div", "vint": "vlogic", "vsplt": "vperm", "vfma": "vfp", "vdot": "vest", "vconv": "vfp",
                  "vcmp": "vfp", "vcmp_rc": "vfp", "fma": "fp", "fconv": "fp", "fcmp": "fp", "lbz": "lwz", "lha": "lwz",
                  "lwzx": "lwz", "lwzu": "lwz", "ld": "lwz", "lfs": "lfd", "lvlx": "lvx", "std": "stw", "stwu": "stw",
                  "stfiwx": "stfd", "mcrf": "crlogic", "mfcr": "int", "mtcrf": "int", "br_nt": "br_t", "b": "br_t",
                  "mflr": "int", "mtlr": "int", "filler": "int"}
# without unit.conflict data: one assumed pipe per ISA unit (marked unverified)
ASSUMED_GROUPS = OrderedDict([("FXU", ["int", "cmp", "recimm", "mul", "div", "varshift", "crlogic"]),
                              ("LSU", ["lwz", "lfd", "lvx", "stw", "stfd", "stvx"]), ("FPU", ["fp", "fdiv", "fsqrt"]),
                              ("VMX", ["vlogic", "vperm", "vfp", "vest"]), ("BRU", ["br_t"])])


# ----------------------------------------------------------------------------------------------------------- helpers
def fnum(v):
    if v is None or isinstance(v, bool):
        return None
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    return f if math.isfinite(f) else None


def rnd(v, nd=4):
    return None if v is None else round(v, nd)


def median(vals):
    return statistics.median(vals) if vals else None


def pct(vals, q):
    if not vals:
        return None
    s = sorted(vals)
    k = (len(s) - 1) * q
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def majority(items):
    c = Counter(x for x in items if x is not None)
    if not c:
        return None
    return c.most_common(1)[0][0]


def prune(o):
    """drop empty dicts/lists recursively (keeps 0, False, None-valued leaves only when meaningful)"""
    if isinstance(o, dict):
        out = OrderedDict()
        for k, v in o.items():
            v = prune(v)
            if v is None or v == {} or v == []:
                continue
            out[k] = v
        return out
    if isinstance(o, list):
        return [prune(x) for x in o]
    return o


def axis_defaults(root=ROOT):
    d = dict(FALLBACK_DEFAULTS)
    for f in sorted(glob.glob(os.path.join(root, "src", "conds", "*.c"))):
        try:
            txt = open(f, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        arrays = {m.group(1): re.findall(r'"([^"]*)"', m.group(2))
                  for m in re.finditer(r"static\s+const\s+char\s*\*\s*const\s+(\w+)\s*\[\]\s*=\s*\{([^}]*)\}", txt)}
        for m in re.finditer(r"XTS_AXIS\(\s*(\w+)\s*,\s*\.values\s*=\s*(\w+)", txt):
            vals = arrays.get(m.group(2))
            if vals:
                d[m.group(1)] = vals[0]
    return d


def load_isa(path):
    try:
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError):
        return {}
    out = {}
    for i in d.get("instructions", []):
        out[i.get("mnem")] = i
    return out


def is_info(r):
    """block/SMT info records: metric "info" (v2: case smt:<axis>, dist.summary, ...) or the v1 smt_* metrics"""
    return r.metric == "info" or r.metric.startswith("smt_")


def is_smt_info(r):
    return r.metric.startswith("smt_") or (r.metric == "info" and r.case.startswith("smt:"))


class Rec(object):
    __slots__ = ("tag", "run", "family", "case", "mnem", "metric", "value", "unit", "status", "det", "cond", "label", "ok",
                 "rep")


# ----------------------------------------------------------------------------------------------------------- loading
def load_inputs(inputs, allow_host, consoles, log):
    prov = OrderedDict(inputs=[os.path.abspath(p) for p in inputs], runs=OrderedDict(), refused=[], consoles=OrderedDict(),
                       records_read=0, duplicates_dropped=0, conflicts=0, bad_lines=0, truncated_tails=0,
                       filtered_console=0, warnings=[])
    seen = {}
    recs = []
    strip = ("console_tag", "console", "is_host")

    def add_run(tag, run_id, src, info, is_host):
        k = "%s/%s" % (tag, run_id)
        r = prov["runs"].setdefault(k, OrderedDict(console_tag=tag, run_id=run_id, sources=[], is_host=is_host, records=0))
        if src not in r["sources"]:
            r["sources"].append(src)
        for kk, vv in (info or {}).items():
            r.setdefault(kk, vv)
        return r

    def ingest(rec, tag, src, info, is_host):
        prov["records_read"] += 1
        if consoles and tag not in consoles:
            prov["filtered_console"] += 1
            return
        rid = str(rec.get("run_id", ""))
        key = (tag, rid, rec.get("seq"))
        h = hashlib.sha1(json.dumps({k: v for k, v in rec.items() if k not in strip}, sort_keys=True,
                                    separators=(",", ":")).encode()).hexdigest()
        if key in seen:
            if seen[key] == h:
                prov["duplicates_dropped"] += 1
            else:
                prov["conflicts"] += 1
            return
        seen[key] = h
        run = add_run(tag, rid, src, info, is_host)
        run["records"] += 1
        if isinstance(rec.get("console"), dict) and rec["console"]:
            prov["consoles"].setdefault(tag, rec["console"])
        recs.append((tag, rid, rec))

    merged_files, run_dirs = [], []
    for p in inputs:
        if os.path.isfile(p):
            if os.path.basename(p) == "results.jsonl":
                run_dirs.append(os.path.dirname(os.path.abspath(p)))
            else:
                merged_files.append(p)
        elif os.path.isdir(p):
            if os.path.isfile(os.path.join(p, "merged.jsonl")) and not os.path.isfile(os.path.join(p, "results.jsonl")):
                merged_files.append(os.path.join(p, "merged.jsonl"))
            else:
                run_dirs.extend(XM.find_runs([p]))
        else:
            prov["warnings"].append("input not found: %s" % p)
    for mf in merged_files:
        summ = XM.jload(os.path.join(os.path.dirname(os.path.abspath(mf)), "summary.json")) or {}
        runinfo = {}
        for tag, cs in (summ.get("consoles") or {}).items():
            if isinstance(cs.get("identity"), dict):
                prov["consoles"].setdefault(tag, cs["identity"])
            for rs in cs.get("runs") or []:
                runinfo[(tag, str(rs.get("run_id")))] = dict(rs.get("run") or {})
        st = Counter()
        refused = Counter()
        for rec in XM.read_lines(mf, st):
            tag = rec.get("console_tag") or "unknown"
            if rec.get("is_host") and not allow_host:
                refused[tag] += 1
                continue
            ingest(rec, tag, mf, runinfo.get((tag, str(rec.get("run_id")))), bool(rec.get("is_host")))
        prov["bad_lines"] += st["bad_lines"]
        prov["truncated_tails"] += st["truncated_tail"]
        for tag, n in refused.items():
            prov["refused"].append({"source": mf, "console_tag": tag, "records": n, "reason": "host dry-run records (is_host)"})
            log("REFUSED %d host dry-run records of %s in %s; use --allow-host to include them" % (n, tag, mf))
    for d in list(OrderedDict.fromkeys(run_dirs)):       # input order: on a conflict the first input wins
        r = XM.load_run(d)
        if r.is_host and not allow_host:
            prov["refused"].append({"source": d, "console_tag": r.tag, "records": r.total,
                                    "reason": "host dry-run output (" + "; ".join(r.host_reasons) + ")"})
            log("REFUSED %s: host dry-run output (%s); use --allow-host to include it" % (d, "; ".join(r.host_reasons)))
            continue
        info = dict(r.run_ident)
        if r.sysinfo and "cycles_per_tick" in r.sysinfo:
            info["cycles_per_tick"] = r.sysinfo["cycles_per_tick"]
        info["journal_done"] = r.journal.get("done")
        if r.console_ident:
            prov["consoles"].setdefault(r.tag, r.console_ident)
        for w in r.warnings:
            prov["warnings"].append("%s: %s" % (d, w))
        st = Counter()
        for rec in XM.read_lines(os.path.join(d, "results.jsonl"), st):
            ingest(rec, rec.get("console_tag") or r.tag, d, info, r.is_host)
        prov["bad_lines"] += st["bad_lines"]
        prov["truncated_tails"] += st["truncated_tail"]
    return recs, prov


def normalise(raw, defaults, accept):
    out = []
    for tag, rid, rec in raw:
        r = Rec()
        r.tag, r.run = tag, "%s/%s" % (tag, rid)
        r.family = rec.get("family") or ""
        r.case = rec.get("case_id") or ""
        r.mnem = rec.get("mnem") or ""
        r.metric = rec.get("metric") or ""
        r.value = fnum(rec.get("value"))
        r.unit = rec.get("unit") or ""
        r.status = rec.get("status") or "?"
        r.det = rec.get("detail") if isinstance(rec.get("detail"), dict) else {}
        r.cond = rec.get("cond") if isinstance(rec.get("cond"), dict) else {}
        dims = DIM_AXES.get(r.family, ())
        nd = sorted("%s=%s" % (k, v) for k, v in r.cond.items() if k not in dims and v != defaults.get(k, v))
        r.label = ";".join(nd) if nd else "default"
        r.ok = r.status in accept
        r.rep = False
        out.append(r)
    return out


def promote_repeatable(recs, tol):
    """noisy records of one measurement (family, case, metric, mnem, condition label) from at least two runs that agree
    within tol of their median are accepted and tagged (r.rep): the gate flagged each run, but the value repeats. Returns
    the number of records promoted."""
    if not tol or tol <= 0:
        return 0
    groups = defaultdict(list)
    for r in recs:
        if r.status == "noisy" and not r.ok and r.value is not None:
            groups[(r.tag, r.family, r.case, r.metric, r.mnem, r.label)].append(r)
    n = 0
    for g in groups.values():
        runs = set(r.run for r in g)
        if len(runs) < 2:
            continue
        vs = [r.value for r in g]
        med = statistics.median(vs)
        if med == 0 or (max(vs) - min(vs)) > tol * abs(med):
            continue
        for r in g:
            r.ok, r.rep = True, True
            n += 1
    return n


# ----------------------------------------------------------------------------------------------------------- aggregation
class Ctx(object):
    def __init__(self, isa, count=False):
        self.isa = isa
        self.count = count
        self.values = 0
        self.single = 0
        self.single_keys = []


def agg(recs, ctx=None, vals=None, where=None):
    """recs: accepted records; vals: optional parallel values (default r.value). None when nothing numeric."""
    items = []
    rep = False
    for i, r in enumerate(recs):
        v = vals[i] if vals is not None else r.value
        if v is not None:
            items.append((v, r.tag, r.run))
            rep = rep or bool(getattr(r, "rep", False))
    if not items:
        return None
    vs = [x[0] for x in items]
    med = statistics.median(vs)
    out = OrderedDict(value=rnd(med), min=rnd(min(vs)), max=rnd(max(vs)), mad=rnd(statistics.median([abs(v - med) for v in vs])),
                      n=len(vs), runs=len(set(x[2] for x in items)))
    tags = sorted(set(x[1] for x in items))
    if len(tags) > 1:
        bc = OrderedDict((t, rnd(statistics.median([x[0] for x in items if x[1] == t]))) for t in tags)
        out["by_console"] = bc
        out["console_spread"] = rnd(max(bc.values()) - min(bc.values()))
    if out["runs"] == 1:
        out["single_source"] = True
    if rep:
        out["repeatable_noisy"] = True      # some inputs were noisy runs accepted because the runs agree (promote_repeatable)
    if ctx is not None and ctx.count:
        ctx.values += 1
        if out["runs"] == 1:
            ctx.single += 1
            if where and len(ctx.single_keys) < 40:
                ctx.single_keys.append(where)
    return out


def val(a):
    return a.get("value") if isinstance(a, dict) else None


def split_case(case):
    a, _, b = case.partition("/")
    return a, b


def size_of(tok):
    """'4k' -> 4096, '256' -> 256, '2m' -> 2097152; None if not a size"""
    m = re.match(r"^(\d+)([kKmM]?)$", str(tok))
    if not m:
        return None
    return int(m.group(1)) * {"": 1, "k": 1024, "K": 1024, "m": 1 << 20, "M": 1 << 20}[m.group(2)]


# ----------------------------------------------------------------------------------------------------------- model builder
class Builder(object):
    def __init__(self, ctx):
        self.ctx = ctx
        self.I = OrderedDict()           # variant -> dict of lists
        self.cls = defaultdict(lambda: defaultdict(list))
        self.L = defaultdict(list)        # generic keyed lists of records: key tuple -> [rec]
        self.V = defaultdict(list)        # keyed lists of (rec, value-from-detail)
        self.verd = defaultdict(list)     # keyed verdict strings
        self.graphs = []
        self.class_mnem = {}

    def ins(self, variant, mnem):
        e = self.I.get(variant)
        if e is None:
            e = self.I[variant] = {"mnem": mnem, "lat": defaultdict(list), "lat_kind": {}, "lat_upper": set(), "rc": None,
                                   "rt": [], "rt_upper": [], "depth": [], "curve": defaultdict(list), "nsat": [],
                                   "pipelined": [], "issue": defaultdict(list), "U": [], "mc": [], "nb": [], "unpip": [],
                                   "bdi": [], "bins": defaultdict(list), "binc": defaultdict(list), "load": defaultdict(list),
                                   "use": [], "ltp": defaultdict(list), "stp": defaultdict(list), "mis": defaultdict(list),
                                   "other": defaultdict(list), "ucode_refused": None, "strc": defaultdict(list)}
        if not e["mnem"] and mnem:
            e["mnem"] = mnem
        return e

    # -- per family
    def add(self, r):
        f = r.family
        if is_info(r):
            return
        if f == "ucode" and r.metric == "extra" and r.status == "skip" and r.det.get("why") and "host" not in r.det.get("why", ""):
            self.ins(r.case, r.mnem)["ucode_refused"] = r.det.get("why")
        if not r.ok:
            return
        h = getattr(self, "f_" + f.replace(".", "_"), None)
        if h:
            h(r)

    def f_lat_chain(self, r):
        if r.metric != "latency":
            return
        e = self.ins(r.det.get("variant") or r.mnem, r.mnem)
        slot = r.det.get("slot") or "?"
        e["lat"][slot].append(r)
        e["lat_kind"][slot] = r.det.get("kind")
        e["rc"] = r.det.get("class") or e["rc"]
        if r.status == "info":
            e["lat_upper"].add(slot)

    def f_tput_indep(self, r):
        e = self.ins(r.det.get("variant") or r.mnem, r.mnem)
        if r.metric == "rthroughput":
            e["rt"].append(r)
            if r.status == "info":
                e["rt_upper"].append(r)
            e["rc"] = r.det.get("class") or e["rc"]
            for p in r.det.get("curve") or []:
                if isinstance(p, dict) and p.get("status") in DEFAULT_STATUSES and fnum(p.get("cycles")) is not None:
                    e["curve"][int(p.get("n", 0))].append((r, fnum(p.get("cycles"))))
            if r.det.get("n_sat") not in (None, -1):
                e["nsat"].append(r.det.get("n_sat"))
            if r.det.get("pipelined") is not None:
                e["pipelined"].append(bool(r.det.get("pipelined")))
        elif r.metric == "depth":
            e["depth"].append(r)

    def f_lat_data(self, r):
        e = self.ins(r.mnem, r.mnem)
        b = r.det.get("bin") or "?"
        if r.det.get("class") == "vr":
            b = "%s.nj%s" % (b, r.det.get("vscr_nj", 0))
        shape = r.det.get("shape") or "?"
        if r.metric == "ratio":
            e["bins"][(shape, b)].append(r)
        elif r.metric in ("issue_cost", "latency"):
            e["binc"][(shape, b)].append(r)
            if r.det.get("bin") in ("normal", "small") and str(r.det.get("vscr_nj", 0)) == "0":
                if r.metric == "issue_cost":
                    e["issue"]["lat.data"].append(r)
                else:
                    e["other"]["lat.data:chain_normal_latency"].append(r)

    def f_ucode(self, r):
        if r.case == "control.filler":
            self.L[("filler", "ucode")].append(r)
            return
        e = self.ins(r.case, r.mnem)
        if r.metric == "issue_cost":
            e["issue"]["ucode"].append(r)
        elif r.metric == "extra":
            e["U"].append(r)
            e["mc"].append(r.det.get("microcoded"))
            e["nb"].append(r.det.get("neighbour"))
            if r.det.get("unpipelined") is not None:
                e["unpip"].append(bool(r.det.get("unpipelined")))
            if r.det.get("blocks_dual_issue") is not None:
                e["bdi"].append(bool(r.det.get("blocks_dual_issue")))

    def _cls_variant(self, c, mnem=None):
        rep = CLASS_REP.get(c)
        if rep:
            return rep[0] + rep[1], rep[0]
        return (mnem or c), (mnem or c)

    def f_issue_pair(self, r):
        if r.case == "control.filler":
            self.L[("filler", "issue.pair")].append(r)
            return
        if r.metric == "issue_cost" and r.case.startswith("alone."):
            c = r.det.get("class") or r.case[6:]
            self.cls[c]["issue"].append(r)
            v, m = self._cls_variant(c, r.mnem)
            self.ins(v, m)["issue"]["issue.pair"].append(r)
        elif r.metric == "cycles" and r.case.startswith("pair."):
            cl = r.det.get("classes") or []
            if len(cl) != 2:
                return
            k = ("pair", cl[0], cl[1])
            self.L[k].append(r)
            self.V[k].append(fnum(r.det.get("overlap")))
            self.verd[k].append(r.det.get("verdict"))
            od = r.det.get("order_dependent")
            self.verd[k + ("od",)].append(od if isinstance(od, bool) else None)
            for c, mn in zip(cl, (r.det.get("first"), r.det.get("second"))):
                if mn:
                    self.class_mnem.setdefault(c, mn)

    def f_unit_conflict(self, r):
        if r.metric == "rthroughput" and r.case.startswith("alone."):
            c = r.det.get("class") or r.case[6:]
            self.cls[c]["rt"].append(r)
            v, m = self._cls_variant(c, r.mnem)
            self.ins(v, m)["other"]["unit.conflict:rthroughput"].append(r)
        elif r.metric == "overlap":
            cl = r.det.get("classes") or []
            if len(cl) == 2:
                k = ("edge",) + tuple(sorted(cl))
                self.L[k].append(r)
                self.verd[k].append(r.det.get("verdict"))
        elif r.metric == "penalty" and r.case.startswith("busy."):
            p, s = r.det.get("producer"), r.det.get("stream")
            k = ("busy", p, s)
            self.L[k].append(r)
            self.V[k].append(fnum(r.det.get("busy_cycles")) if fnum(r.det.get("busy_cycles")) is not None else r.value)
            self.verd[k].append(r.det.get("verdict"))
        elif r.metric == "count" and r.case == "graph":
            if r.det.get("shared_groups"):
                self.graphs.append(r.det.get("shared_groups"))

    def f_bypass_matrix(self, r):
        d = r.det
        if r.case.startswith(("mtctr>bctr", "mtlr>blr")):
            k = ("regbr", "bypass.matrix", "%s>%s" % (d.get("producer"), d.get("consumer")), int(d.get("distance", 0)))
            self.L[k].append(r)
            return
        if r.case.startswith("dist."):
            name = r.case[5:].rsplit(".d", 1)[0]
            if r.metric == "penalty":
                self.L[("bpdist", name, int(d.get("distance", 0)))].append(r)
            elif r.metric == "latency":
                self.L[("bphide", name)].append(r)
            return
        key = (d.get("producer"), d.get("consumer"), d.get("slot"))
        if None in key:
            return
        for c, mn in ((d.get("producer"), d.get("producer_mnem")), (d.get("consumer"), d.get("consumer_mnem"))):
            if c and mn:
                self.class_mnem.setdefault(c, mn)
        if r.metric == "penalty":
            self.L[("bp_pen",) + key].append(r)
            self.verd[("bp_pen",) + key].append(d.get("verdict"))
        elif r.metric == "extra":
            self.L[("bp_ext",) + key].append(r)
            self.verd[("bp_ext",) + key].append(d.get("verdict"))
            self.V[("bp_meta",) + key].append((d.get("slot_role"), d.get("reference"), d.get("reference_slot"),
                                                d.get("producer_mnem"), d.get("consumer_mnem")))

    def f_branch(self, r):
        c, sub = split_case(r.case)
        self.L[("branch", c, sub, r.metric)].append(r)
        if c.startswith(("bctr.d", "blr.d")) and sub == "penalty":
            nm = "mtctr>bctr" if c.startswith("bctr") else "mtlr>blr"
            self.L[("regbr", "branch", nm, int(r.det.get("distance", c.split(".d")[-1])))].append(r)
        if c == "linkstack" and sub == "depth":
            self.V[("linkstack",)].append(r.det)

    def f_sysops(self, r):
        c, sub = split_case(r.case)
        self.L[("sysops", c, sub, r.metric)].append(r)
        if c.startswith("str.") and r.metric == "issue_cost" and r.mnem:   # string / multiple: cost per byte count
            nb = r.det.get("bytes") or size_of(sub[1:]) if sub.startswith("n") else r.det.get("bytes")
            if nb:
                self.ins(r.mnem, r.mnem)["strc"][int(nb)].append(r)
        if r.mnem and r.metric in ("latency", "rthroughput", "issue_cost"):
            self.ins(r.mnem, r.mnem)["other"]["sysops:%s/%s:%s" % (c, sub, r.metric)].append(r)

    def f_calib(self, r):
        c, sub = split_case(r.case)
        sp = r.cond.get("speed", "default")
        self.L[("calib", c, sub, r.metric, sp)].append(r)

    def f_ldst_lat(self, r):
        d, case = r.det, r.case
        pre = case.split(".", 1)[0]
        align = r.cond.get("data_align", "aligned")
        if pre == "lat" and r.metric == "latency":
            lv = d.get("level") or {"default": "l1"}.get(r.cond.get("cache", "default"), r.cond.get("cache"))
            key = lv if align == "aligned" else "%s@%s" % (lv, align)
            self.ins(r.mnem, r.mnem)["load"][key].append(r)
        elif pre == "use" and r.metric == "extra":
            self.ins(r.mnem, r.mnem)["use"].append(r)
        elif pre == "ltp" and r.metric == "rthroughput":
            self.ins(r.mnem, r.mnem)["ltp"][d.get("pattern") or "?"].append(r)
        elif pre == "stp" and r.metric == "rthroughput":
            self.ins(r.mnem, r.mnem)["stp"][d.get("pattern") or "?"].append(r)
        elif pre == "mis" and r.metric == "penalty":
            parts = case.split(".")
            if len(parts) >= 4:
                form, kind, what = ".".join(parts[1:-2]), parts[-2], parts[-1]
                self.L[("mis", form, kind, what)].append(r)
                self.ins(r.mnem or form, r.mnem or form)["mis"]["%s.%s" % (kind, what)].append(r)
        elif pre in ("lhs", "xfer") and r.metric == "penalty":
            self.L[(pre, d.get("store"), d.get("load"), d.get("relation"))].append(r)
        elif pre == "rt" and r.metric == "latency":
            self.L[("rt", d.get("route") or case[3:])].append(r)
        elif pre == "resv":
            self.L[("resv", case[5:])].append(r)

    def f_mem_hier(self, r):
        d, case = r.det, r.case
        parts = case.split(".")
        if case.startswith("chase.rand.") and r.metric == "latency":
            self.L[("rand", size_of(parts[2]) or parts[2])].append(r)
        elif case == "chase.levels":
            if r.metric == "latency" and d.get("level"):
                self.L[("level", d.get("level"))].append(r)
            elif r.metric == "count" and d.get("what") == "capacity":
                self.L[("capacity", d.get("level"))].append(r)
        elif case.startswith("chase.seq.") and r.metric == "latency":
            k = size_of(parts[2]) or parts[2]
            self.L[("seq", k)].append(r)
            if fnum(d.get("ratio_to_random")) is not None:
                self.V[("seqratio", k)].append((r, fnum(d.get("ratio_to_random"))))
        elif case.startswith("stride.") and r.metric == "latency":
            self.L[("stride", size_of(parts[1]) or parts[1])].append(r)
        elif case.startswith("assoc.") and r.metric == "count":
            if d.get("what") == "ways":
                self.L[("ways", d.get("level"))].append(r)
                self.V[("ways_meta", d.get("level"))].append((d.get("set_stride"), d.get("verdict")))
            elif d.get("what") == "lines_that_fit":
                self.L[("fit", d.get("level"), d.get("stride"))].append(r)
        elif case.startswith("stream.") and r.metric == "bytes_per_cycle":
            self.L[("stream", ".".join(parts[1:]))].append(r)
        elif case.startswith("pfstream.") and r.metric == "bytes_per_cycle":
            self.L[("pfstream", parts[1])].append(r)
        elif case.startswith("cop.") and r.metric == "issue_cost":
            self.L[("cop", parts[1], ".".join(parts[2:]))].append(r)
        elif case == "prefetch.xdcbt2":
            if r.metric == "latency" and d.get("verdict"):
                self.L[("xdcbt2", r.mnem or "?")].append(r)
                self.verd[("xdcbt2", r.mnem or "?")].append(d.get("verdict"))
                self.V[("xdcbt2_meta", r.mnem or "?")].append((d.get("level"), d.get("delivered_to")))
            elif r.metric == "cycles":
                self.L[("xdcbt2_cyc", d.get("prefetch"), bool(d.get("evict")))].append(r)
        elif case.startswith("tlb.") and case != "tlb.knees" and r.metric == "penalty":
            self.L[("tlb", int(d.get("pages") or parts[1]))].append(r)
        elif case == "tlb.knees" and r.metric == "count":
            self.L[("tlbknee", d.get("what"))].append(r)

    # -- results
    def sub(self, prefix):
        return [(k, v) for k, v in self.L.items() if k[0] == prefix]

    def a(self, recs, where=None, vals=None):
        return agg(recs, self.ctx, vals, where)

    def finish(self):
        ctx = self.ctx
        M = OrderedDict()
        # ---- classes and pipes
        classes = OrderedDict()
        names = set(self.cls) | set(c for k in self.L if k[0] == "edge" for c in k[1:]) | \
            set(c for k in self.L if k[0] == "pair" for c in k[1:3])
        for c in sorted(names):
            rep = CLASS_REP.get(c)
            classes[c] = OrderedDict(rep=(rep[0] + rep[1]) if rep else self.class_mnem.get(c),
                                     issue_cost=self.a(self.cls[c]["issue"], "class %s issue_cost" % c) if c in self.cls else None,
                                     rthroughput=self.a(self.cls[c]["rt"], "class %s rthroughput" % c) if c in self.cls else None)
        edges = []
        parent = {}

        def find(x):
            while parent.setdefault(x, x) != x:
                x = parent[x]
            return x
        # classes that block issue for many cycles (unit.conflict busy verdict "blocking": div, fdiv, mul, ...) serialise
        # with whatever they are mixed with, so their "shared" edges say nothing about a common pipe and would bridge
        # unrelated groups (int + fp + loads in one). Group without them; each stays a group of its own, with the groups
        # it serialises with as candidates (a vote over its shared edges only counted which mixes were run).
        blocking = OrderedDict()
        for k, recs in sorted(self.sub("busy"), key=lambda kv: (str(kv[0][1]), str(kv[0][2]))):
            if majority(self.verd[k]) == "blocking":
                blocking.setdefault(k[1], []).append(k[2])
        shared = []
        for k, recs in sorted(self.sub("edge")):
            v = majority(self.verd[k])
            edges.append(OrderedDict(a=k[1], b=k[2], verdict=v, overlap=self.a(recs, "edge %s+%s" % k[1:]),
                                     votes=dict(Counter(x for x in self.verd[k] if x))))
            for c in k[1:3]:
                if c not in blocking:
                    find(c)
            if v == "shared":
                shared.append((k[1], k[2]))
                if k[1] in blocking or k[2] in blocking:
                    continue
                ra, rb = find(k[1]), find(k[2])
                if ra != rb:
                    parent[rb] = ra
        placement = OrderedDict()
        for c in blocking:
            parent[c] = c
            placement[c] = sorted(set(find(o) for x, y in shared for o in ((y,) if x == c else (x,) if y == c else ())
                                      if o not in blocking))
        groups = []
        if parent:
            comp = defaultdict(list)
            for c in sorted(parent):
                comp[find(c)].append(c)
            for members in sorted(comp.values(), key=lambda m: m[0]):
                groups.append(OrderedDict(name=group_name(members), classes=members, source="unit.conflict",
                                          issue_blocking=[c for c in members if c in blocking]))
        elif self.graphs:
            g = majority([json.dumps(x) for x in self.graphs])
            for members in json.loads(g):
                groups.append(OrderedDict(name=group_name(members), classes=members, source="unit.conflict graph record"))
        group_of = {}
        for g in groups:
            for c in g["classes"]:
                group_of[c] = g["name"]
            rts = [val(classes.get(c, {}).get("rthroughput")) for c in g["classes"]]
            rts = [x for x in rts if x]
            g["units"] = max(1, int(round(1.0 / min(rts)))) if rts and min(rts) < 0.75 else 1
        for c in classes:
            classes[c]["pipe_group"] = group_of.get(c) or group_of.get(CLASS_FALLBACK.get(c))
        busy = OrderedDict()
        for k, recs in sorted(self.sub("busy"), key=lambda kv: (str(kv[0][1]), str(kv[0][2]))):
            busy["%s/%s" % (k[1], k[2])] = OrderedDict(busy_cycles=self.a(recs, "busy %s/%s" % k[1:], self.V[k]),
                                                       verdict=majority(self.verd[k]))
        M["pipes"] = OrderedDict(groups=groups, edges=edges, busy=busy,
                                 issue_blocking=OrderedDict((c, OrderedDict(blocks=v, group=group_of.get(c),
                                                                            candidates=placement.get(c, [])))
                                                            for c, v in blocking.items()),
                                 assumed=None if groups else "no unit.conflict data: compiler should assume one pipe per unit")
        M["classes"] = classes
        # ---- dual issue
        pairing = OrderedDict()
        for k, recs in sorted(self.sub("pair")):
            ov = [x for x in self.V[k]]
            pairing["%s>%s" % (k[1], k[2])] = OrderedDict(
                first=k[1], second=k[2], verdict=majority(self.verd[k]), votes=dict(Counter(x for x in self.verd[k] if x)),
                overlap=self.a(recs, None, ov), cycles=self.a(recs, "pair %s>%s" % k[1:]),
                order_dependent=majority(self.verd[k + ("od",)]))
        M["pairing"] = pairing
        filler = self.L.get(("filler", "issue.pair"), []) + self.L.get(("filler", "ucode"), [])
        # ---- bypass
        bp = OrderedDict()
        for k, recs in sorted(self.sub("bp_pen")):
            key = k[1:]
            meta = majority([json.dumps(m) for m in self.V.get(("bp_meta",) + key, [])])
            meta = json.loads(meta) if meta else [None] * 5
            ext = self.L.get(("bp_ext",) + key, [])
            bp["%s>%s.%s" % key] = OrderedDict(
                producer=key[0], consumer=key[1], slot=key[2], slot_role=meta[0], producer_mnem=meta[3], consumer_mnem=meta[4],
                penalty=self.a(recs, "bypass %s>%s.%s penalty" % key), verdict=majority(self.verd[k]),
                extra=self.a(ext, "bypass %s>%s.%s extra" % key), extra_verdict=majority(self.verd.get(("bp_ext",) + key, [])),
                reference="%s.%s" % (meta[1], meta[2]) if meta[1] and meta[1] != "none" else None)
        for k, recs in sorted(self.sub("bp_ext")):          # extra without penalty record (partial runs)
            key = k[1:]
            if "%s>%s.%s" % key not in bp:
                bp["%s>%s.%s" % key] = OrderedDict(producer=key[0], consumer=key[1], slot=key[2],
                                                   extra=self.a(recs, "bypass extra"), extra_verdict=majority(self.verd[k]))
        matrix = OrderedDict()
        for e in bp.values():
            x = val(e.get("extra"))
            if x is None:
                x = val(e.get("penalty"))
            if x is not None:
                matrix.setdefault(e["producer"], OrderedDict()).setdefault(e["consumer"], OrderedDict())[e["slot"]] = x
        sweeps = OrderedDict()
        for k, recs in sorted(self.sub("bpdist")):
            sweeps.setdefault(k[1], OrderedDict(penalty_by_distance=OrderedDict()))["penalty_by_distance"][str(k[2])] = self.a(recs)
        for k, recs in self.sub("bphide"):
            sweeps.setdefault(k[1], OrderedDict())["fillers_to_hide"] = self.a(recs, "bypass sweep %s" % k[1])
        M["bypass"] = OrderedDict(entries=bp, extra_matrix=matrix, distance_sweeps=sweeps,
                                  meaning="extra = stall beyond the producer's ordinary same-file forwarding (penalty minus "
                                          "the reference consumer's); penalty = stall of an adjacent P->C pair")
        # ---- register branches
        rb = OrderedDict()
        for k, recs in sorted(self.sub("regbr")):
            rb.setdefault(k[2], OrderedDict()).setdefault(k[1], OrderedDict())[str(k[3])] = self.a(recs, "regbr %s %s d%d" % k[1:])
        for nm, srcs in rb.items():
            for src, curve in list(srcs.items()):
                srcs[src] = OrderedDict(penalty_by_distance=curve, hide_distance=hide_distance(curve))
            hs = [s["hide_distance"] for s in srcs.values() if s["hide_distance"] is not None]
            srcs["hide_distance"] = max(hs) if hs else None
        M["register_branch"] = rb
        # ---- instructions (needs groups)
        M["instructions"] = self.finish_insns(group_of)
        # ---- globals
        M["global"] = self.finish_global(pairing, filler)
        return M

    def finish_insns(self, group_of):
        out = OrderedDict()
        for v in sorted(self.I):
            e = self.I[v]
            mnem = e["mnem"] or v
            isa = self.ctx.isa.get(mnem) or {}
            ops = isa.get("operands") or []
            o = OrderedDict(mnem=mnem, variant=v, isa_unit=isa.get("unit"), isa=isa.get("isa"), reg_class=e["rc"])
            lat = OrderedDict()
            for slot in sorted(e["lat"], key=lambda s: (ops.index(s) if s in ops else (99 if s == "acc" else 50), s)):
                a = self.a(e["lat"][slot], "%s latency %s" % (v, slot))
                if a is None:
                    continue
                a["kind"] = e["lat_kind"].get(slot)
                if slot in e["lat_upper"]:
                    a["upper_bound"] = True
                lat[slot] = a
            o["latency"] = lat
            lv = [val(a) for a in lat.values() if val(a) is not None]
            o["latency_max"] = rnd(max(lv)) if lv else None
            o["latency_first_slot"] = val(next(iter(lat.values()))) if lat else None
            rt = self.a(e["rt"], "%s rthroughput" % v)
            if rt is not None and e["rt_upper"] and len(e["rt_upper"]) * 2 >= len(e["rt"]):
                rt["upper_bound"] = True
            o["rthroughput"] = rt
            o["pipelining"] = OrderedDict(
                depth=self.a(e["depth"], "%s depth" % v),
                pipelined=majority(e["pipelined"]), n_sat=majority(e["nsat"]),
                curve=OrderedDict((str(n), rnd(statistics.median([x[1] for x in e["curve"][n]]))) for n in sorted(e["curve"])))
            iss = OrderedDict()
            for src in ("ucode", "issue.pair", "lat.data"):
                if e["issue"].get(src):
                    iss[src] = self.a(e["issue"][src], "%s issue_cost %s" % (v, src))
            if e["strc"]:                     # sysops str.*: issue cost per byte count; the smallest count is the issue cost
                byb = OrderedDict((str(nb), self.a(e["strc"][nb], "%s issue_cost %d bytes" % (v, nb))) for nb in sorted(e["strc"]))
                o["issue_cost_by_bytes"] = byb
                first = next((a for a in byb.values() if val(a) is not None), None)
                if first is not None:
                    iss["sysops.string"] = first
            o["issue_cost_by_source"] = iss
            o["issue_cost"] = next((val(a) for a in iss.values() if val(a) is not None), None)
            U = self.a(e["U"], "%s ucode extra" % v)
            mc = majority(e["mc"])
            if U is not None or mc or e["ucode_refused"]:
                o["ucode"] = OrderedDict(
                    microcoded=mc, cracked_or_microcoded=mc, dispatch_cycles=U,
                    extra_dispatch_cycles=rnd(max(0.0, val(U) - 1.0)) if val(U) is not None else None,
                    neighbour=majority(e["nb"]), unpipelined=majority(e["unpip"]), blocks_dual_issue=majority(e["bdi"]),
                    votes=dict(Counter(x for x in e["mc"] if x)), not_timable=e["ucode_refused"])
            bins = OrderedDict()
            for (shape, b), recs in sorted(e["bins"].items()):
                bins.setdefault(shape, OrderedDict())[b] = OrderedDict(ratio=self.a(recs, "%s bin %s %s" % (v, shape, b)),
                                                                       cycles=self.a(e["binc"].get((shape, b), [])))
            for (shape, b), recs in sorted(e["binc"].items()):
                if b not in bins.get(shape, {}):
                    bins.setdefault(shape, OrderedDict())[b] = OrderedDict(cycles=self.a(recs), normal=True)
            o["data_dependent"] = bins
            o["load_use_latency"] = OrderedDict((k, self.a(e["load"][k], "%s load %s" % (v, k))) for k in sorted(e["load"]))
            o["use_stall"] = self.a(e["use"], "%s use stall" % v)
            o["load_throughput"] = OrderedDict((k, self.a(e["ltp"][k])) for k in sorted(e["ltp"]))
            o["store_throughput"] = OrderedDict((k, self.a(e["stp"][k], "%s store tput %s" % (v, k))) for k in sorted(e["stp"]))
            o["misaligned_penalty"] = OrderedDict((k, self.a(e["mis"][k])) for k in sorted(e["mis"]))
            o["other"] = OrderedDict((k, self.a(e["other"][k])) for k in sorted(e["other"]))
            # busy time of an unpipelined unit (unit.conflict busy, producer class of this instruction)
            bt = []
            for k, recs in self.sub("busy"):
                rep = CLASS_REP.get(k[1])
                if rep and rep[0] + rep[1] == v:
                    a = self.a(recs, None, self.V[k])
                    if a:
                        bt.append(OrderedDict(stream=k[2], busy_cycles=a, verdict=majority(self.verd[k])))
            o["unit_busy"] = bt
            cls = class_for(v, mnem, isa)
            o["sched_class"] = cls
            o["pipe_group"] = group_of.get(cls) or group_of.get(CLASS_FALLBACK.get(cls))
            o["pipe_group_source"] = ("class representative" if any(r[0] + r[1] == v for r in CLASS_REP.values()) else
                                      "isa unit / mnemonic heuristic") if o["pipe_group"] else None
            o["baseline_delta"] = baseline_delta(o, isa)
            out[v] = o
        return out

    def finish_global(self, pairing, filler):
        G = OrderedDict()
        yes = [k for k, p in pairing.items() if p.get("verdict") == "yes"]
        G["issue"] = OrderedDict(
            issue_width=2 if yes else None, issue_width_source=("issue.pair: %d ordered pairs co-issue" % len(yes)) if yes else
            "no co-issuing pair measured (design value 2 per thread is assumed in the .td)",
            filler_issue_cost=self.a(filler, "filler issue cost"))
        # branch
        B = OrderedDict()

        def bget(case, sub, metric):
            return self.a(self.L.get(("branch", case, sub, metric), []), "branch %s/%s" % (case, sub))
        B["taken_next"] = bget("pred", "taken_next", "rthroughput")
        B["not_taken"] = bget("pred", "not_taken", "rthroughput")
        B["uncond_next"] = bget("pred", "uncond_next", "rthroughput")
        B["taken_bubble"] = bget("pred", "taken_extra", "extra")
        B["taken_cond_minus_uncond"] = bget("pred", "cond_vs_uncond", "extra")
        B["taken_skip_extra"] = bget("pred", "skip_extra", "extra")
        B["mispredict_penalty"] = bget("mispredict", "penalty", "penalty")
        B["taken_by_distance"] = OrderedDict()
        for k, recs in sorted((k, v) for k, v in self.L.items() if k[0] == "branch" and k[1].startswith(("tk_bc.d", "tk_b.d"))):
            kind, d = k[1].split(".d")
            B["taken_by_distance"].setdefault("bc" if kind == "tk_bc" else "b", OrderedDict())[d] = self.a(recs)
        for kind in B["taken_by_distance"]:
            B["taken_by_distance"][kind] = OrderedDict(sorted(B["taken_by_distance"][kind].items(), key=lambda kv: int(kv[0])))
        B["pattern"] = OrderedDict()
        for k, recs in sorted(((k, v) for k, v in self.L.items() if k[0] == "branch" and k[1].startswith("pattern.p")),
                              key=lambda kv: (int(kv[0][1][9:]), kv[0][2])):
            if k[2] in ("extra", "rate"):
                B["pattern"].setdefault(k[1][9:], OrderedDict())["mispredict_rate" if k[2] == "rate" else "extra"] = self.a(recs)
        ls = bget("linkstack", "depth", "depth")
        lsd = self.V.get(("linkstack",), [])
        B["link_stack"] = OrderedDict(depth=ls, per_level_cycles=rnd(median([fnum(d.get("per_level_cycles")) for d in lsd
                                                                               if fnum(d.get("per_level_cycles")) is not None])),
                                      overflow_cycles_per_level=rnd(median([fnum(d.get("overflow_cycles_per_level")) for d in lsd
                                                                            if fnum(d.get("overflow_cycles_per_level")) is not None])))
        B["loop"] = OrderedDict(overhead_cycles=bget("loop", "overhead", "cycles"), body_insn_cycles=bget("loop", "body_tput", "rthroughput"))
        al = OrderedDict()
        for k, recs in self.L.items():
            if k[0] == "branch" and k[1].startswith("align."):
                al[k[1][6:]] = self.a(recs)
        if al:
            vs = [(val(a), n) for n, a in al.items() if val(a) is not None]
            vs.sort()
            B["alignment"] = OrderedDict(min=vs[0][0] if vs else None, max=vs[-1][0] if vs else None,
                                         worst=[n for x, n in vs[-5:]][::-1], best=[n for x, n in vs[:3]],
                                         by_case=OrderedDict(sorted(al.items(), key=lambda kv: natkey(kv[0]))))
        bht = OrderedDict()
        for k, recs in sorted(((k, v) for k, v in self.L.items() if k[0] == "branch" and k[1].startswith("bht.k") and k[2] == "extra"),
                              key=lambda kv: int(kv[0][1][5:])):
            bht[str(1 << int(k[1][5:]))] = self.a(recs)
        B["bht_aliasing_extra_by_distance"] = bht
        G["branch"] = B
        # memory
        Mm = OrderedDict()
        lv = OrderedDict((k[1], self.a(v, "mem level %s" % k[1])) for k, v in sorted(self.sub("level")))
        Mm["levels"] = lv
        Mm["capacity_bytes"] = OrderedDict((k[1], self.a(v, "capacity %s" % k[1])) for k, v in sorted(self.sub("capacity")))
        ways = OrderedDict()
        for k, v in sorted(self.sub("ways")):
            meta = self.V.get(("ways_meta", k[1]), [])
            ways[k[1]] = OrderedDict(ways=self.a(v, "ways %s" % k[1]), set_stride=majority([m[0] for m in meta]),
                                     verdict=majority([m[1] for m in meta]))
        Mm["associativity"] = ways
        Mm["line_size"] = OrderedDict(value=128, source="design value (mem_util MU_LINE; 128 B lines [AB06]); not measured")
        curve = OrderedDict((str(k[1]), self.a(v)) for k, v in sorted(self.sub("rand"), key=lambda kv: natkey(str(kv[0][1]))))
        Mm["random_chase_curve"] = curve
        Mm["knees_bytes"] = knees(curve)
        Mm["sequential_chase"] = OrderedDict((str(k[1]), OrderedDict(latency=self.a(v), ratio_to_random=self.a(
            [x[0] for x in self.V.get(("seqratio", k[1]), [])], None, [x[1] for x in self.V.get(("seqratio", k[1]), [])])))
            for k, v in sorted(self.sub("seq"), key=lambda kv: natkey(str(kv[0][1]))))
        Mm["stride"] = OrderedDict((str(k[1]), self.a(v)) for k, v in sorted(self.sub("stride"), key=lambda kv: natkey(str(kv[0][1]))))
        Mm["streams_bytes_per_cycle"] = OrderedDict((k[1], self.a(v)) for k, v in sorted(self.sub("stream")))
        pf = OrderedDict((k[1], self.a(v)) for k, v in sorted(self.sub("pfstream")))
        base = val(pf.get("none"))
        Mm["prefetch_stream"] = OrderedDict((k, OrderedDict(bytes_per_cycle=a, speedup_vs_none=rnd(val(a) / base) if base and val(a)
                                                            else None)) for k, a in pf.items())
        xd = OrderedDict()
        for k, v in sorted(self.sub("xdcbt2")):
            meta = self.V.get(("xdcbt2_meta", k[1]), [])
            xd[k[1]] = OrderedDict(verdict=majority(self.verd[k]), level_after_eviction=majority([m[0] for m in meta]),
                                   delivered_to=majority([m[1] for m in meta]), reload_cycles=self.a(v))
        for k, v in sorted(self.sub("xdcbt2_cyc"), key=lambda kv: (str(kv[0][1]), kv[0][2])):
            xd.setdefault("cycles", OrderedDict())["%s%s" % (k[1], "+evict" if k[2] else "")] = self.a(v)
        Mm["prefetch_placement"] = xd
        Mm["cache_ops_issue_cost"] = OrderedDict(("%s.%s" % (k[1], k[2]), self.a(v)) for k, v in sorted(self.sub("cop")))
        Mm["tlb_penalty_by_pages"] = OrderedDict((str(k[1]), self.a(v)) for k, v in sorted(self.sub("tlb")))
        Mm["tlb_reach_pages"] = OrderedDict((k[1], self.a(v)) for k, v in sorted(self.sub("tlbknee")))
        lhs = OrderedDict()
        for pre in ("lhs", "xfer"):
            for k, v in sorted(self.sub(pre), key=lambda kv: tuple(str(x) for x in kv[0])):
                lhs.setdefault(pre, OrderedDict()).setdefault(k[1], OrderedDict()).setdefault(k[2], OrderedDict())[k[3]] = \
                    self.a(v, "%s %s>%s %s" % k)
        Mm["store_to_load"] = lhs
        Mm["memory_round_trips"] = OrderedDict((k[1], self.a(v)) for k, v in sorted(self.sub("rt")))
        mis = OrderedDict()
        for k, v in sorted(self.sub("mis")):
            mis.setdefault(k[2], OrderedDict())["%s.%s" % (k[1], k[3])] = self.a(v, "misaligned %s %s %s" % k[1:])
        Mm["misaligned_penalty"] = mis
        Mm["reservation"] = OrderedDict((k[1], self.a(v)) for k, v in sorted(self.sub("resv")))
        # load latency per level summarised over GPR loads (lwz preferred)
        l1 = None
        lw = self.I.get("lwz")
        if lw and lw["load"].get("l1"):
            l1 = self.a(lw["load"]["l1"])
        Mm["load_use_l1_lwz"] = l1
        G["memory"] = Mm
        # barriers / sysops
        bar = OrderedDict()
        sysops = OrderedDict()
        for k, v in sorted(self.sub("sysops")):
            a = self.a(v, "sysops %s/%s" % (k[1], k[2]))
            sysops.setdefault(k[1], OrderedDict())[k[2]] = OrderedDict(metric=k[3], **(a or {}))
            if k[1].startswith("barrier."):
                bar.setdefault(k[1][8:], OrderedDict())[k[2]] = a
        G["barriers"] = bar
        G["sysops"] = sysops
        # clock
        cal = OrderedDict()
        for k, v in sorted(self.sub("calib"), key=lambda kv: tuple(str(x) for x in kv[0])):
            key = "%s/%s" % (k[1], k[2]) + ("" if k[4] == "default" else "@speed=%s" % k[4])
            cal[key] = self.a(v)
        cpt = OrderedDict((k.split("@")[1] if "@" in k else "default", a) for k, a in cal.items() if k.startswith("cpt/cycles_per_tick"))
        G["clock"] = OrderedDict(cycles_per_tick=cpt, tb_hz=OrderedDict((k.split("@")[1] if "@" in k else "default", a)
                                                                      for k, a in cal.items() if k.startswith("tb_rtc/tb_hz")),
                                 calib=cal)
        return G


GROUP_PRIORITY = ("int", "mul", "div", "fp", "fdiv", "fsqrt", "vfp", "vlogic", "vperm", "vest", "lwz", "lfd", "lvx", "stw",
                  "stfd", "stvx", "br_t", "cmp", "crlogic")


def group_name(members):
    """short identifier for a pipe group: the member itself, a_b / a_b_c, or <first two>_n<count>"""
    ms = sorted(members, key=lambda c: (GROUP_PRIORITY.index(c) if c in GROUP_PRIORITY else 99, c))
    if len(ms) <= 3:
        return "_".join(ms)
    return "%s_%s_n%d" % (ms[0], ms[1], len(ms))


def natkey(s):
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", str(s))]


def hide_distance(curve, thr=0.5):
    pts = sorted((int(d), val(a)) for d, a in curve.items() if val(a) is not None)
    hide = None
    for d, p in reversed(pts):
        if p <= thr:
            hide = d
        else:
            break
    return hide


def knees(curve):
    pts = sorted((int(k), val(a)) for k, a in curve.items() if val(a) is not None and str(k).isdigit())
    out = []
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        if y0 > 0 and y1 / y0 >= 1.3:
            out.append(OrderedDict(below=x0, above=x1, from_cycles=rnd(y0), to_cycles=rnd(y1)))
    return out


def class_for(variant, mnem, isa):
    for c, (m, suf) in CLASS_REP.items():
        if m + suf == variant and c not in ("br_nt", "filler", "mflr", "mtlr"):
            return c
    unit = (isa or {}).get("unit")
    m = mnem
    if m.startswith(("divw", "divd")):
        return "div"
    if m.startswith(("mull", "mulh")):
        return "mul"
    if m.startswith("fdiv"):
        return "fdiv"
    if m.startswith("fsqrt"):
        return "fsqrt"
    if unit == "fpu":
        return "fp"
    if unit == "vector":
        if m.startswith(("vre", "vrsqrte", "vexpte", "vlogefp")):
            return "vest"
        if m.startswith(("vmsum3fp", "vmsum4fp", "vdot")):
            return "vest"
        if m.startswith(("vperm", "vsldoi", "vmrg", "vpk", "vupk", "vsplt", "vrlimi", "vpermwi")):
            return "vperm"
        if "fp" in m or m.startswith(("vcfu", "vcfs", "vctu", "vcts", "vcsx", "vcux", "vmadd", "vnmsub")):
            return "vfp"
        return "vlogic"
    if unit == "memory":
        if m.startswith(("stf",)):
            return "stfd"
        if m.startswith(("stv",)):
            return "stvx"
        if m.startswith("st"):
            return "stw"
        if m.startswith("lf"):
            return "lfd"
        if m.startswith("lv"):
            return "lvx"
        if m.startswith("l"):
            return "lwz"
        return None
    if unit == "branch":
        return "br_t"
    if unit == "integer":
        return "int"
    if m.startswith("cr") or m in ("mcrf",):
        return "crlogic"
    return None


def baseline_delta(o, isa):
    t = (isa or {}).get("timing") or {}
    out = OrderedDict()
    base_variant = o["variant"] == o["mnem"]

    def put(key, b, m):
        b = fnum(b)
        if b is None or m is None:
            return
        out[key] = OrderedDict(baseline=b, measured=m, delta=rnd(m - b), rel=rnd((m - b) / b) if b else None)
    if base_variant:
        put("latency", t.get("latency"), o.get("latency_first_slot"))
        put("issue_vs_rthroughput", t.get("issue"), val(o.get("rthroughput")))
        put("issue_vs_issue_cost", t.get("issue"), o.get("issue_cost"))
        put("load_use_latency", t.get("load_use_latency"), val((o.get("load_use_latency") or {}).get("l1")))
        fo = t.get("fixed_operands") or {}
        dd = (o.get("data_dependent") or {}).get("indep") or {}
        for b, key in (("denormal", "denormal"), ("denormal.nj0", "denormal"), ("denormal.nj1", "denormal_nj"), ("wide", "wide")):
            if b in dd and key in fo:
                put("fixed_operands." + key, fo[key], val(dd[b].get("cycles")))
    return out


# ----------------------------------------------------------------------------------------------------------- effects / SMT
def eff_key(r):
    d = r.det
    return (r.family, r.case, r.metric) + tuple(str(d.get(k)) for k in ("order", "shape", "pattern", "level", "k", "prefetch", "evict"))


def condition_effects(by_label):
    """ratios of every (family, case, metric, discriminator) median in each non-default section against the default section,
    computed per console (so console-to-console offsets cancel) and then combined (median over consoles)"""
    def meds(recs):
        g = defaultdict(list)
        meta = {}
        for r in recs:
            if r.ok and r.value is not None and r.metric in RATIO_METRICS:
                k = (r.tag,) + eff_key(r)
                g[k].append(r.value)
                meta[k[1:]] = r
        return {k: statistics.median(v) for k, v in g.items()}, meta
    dm, _ = meds(by_label.get("default", []))
    out = OrderedDict()
    for label in sorted(by_label):
        if label == "default" or not any(r.ok and r.value is not None for r in by_label[label]):
            continue
        sm, meta = meds(by_label[label])
        per_key = defaultdict(list)
        for k, v in sm.items():
            b = dm.get(k)
            if b is not None and b > 0.05:
                per_key[k[1:]].append(v / b)
        ratios, fam = [], defaultdict(list)
        per_insn = OrderedDict()
        for k, qs in sorted(per_key.items(), key=lambda kv: tuple(str(x) for x in kv[0])):
            q = statistics.median(qs)
            ratios.append(q)
            fam["%s/%s" % (k[0], k[2])].append(q)
            r = meta[k]
            if r.family == "lat.chain":
                per_insn.setdefault(r.det.get("variant") or r.mnem, OrderedDict()).setdefault("latency", OrderedDict())[
                    r.det.get("slot") or "?"] = rnd(q)
            elif r.family == "tput.indep" and r.metric == "rthroughput":
                per_insn.setdefault(r.det.get("variant") or r.mnem, OrderedDict())["rthroughput"] = rnd(q)
        if not ratios:
            out[label] = OrderedDict(matched=0, note="no metric measured both here and at the default condition")
            continue
        out[label] = OrderedDict(
            matched=len(ratios), median_ratio=rnd(statistics.median(ratios)), p10=rnd(pct(ratios, 0.1)), p90=rnd(pct(ratios, 0.9)),
            min=rnd(min(ratios)), max=rnd(max(ratios)),
            by_family_metric=OrderedDict((k, OrderedDict(median_ratio=rnd(statistics.median(v)), n=len(v),
                                                         min=rnd(min(v)), max=rnd(max(v)))) for k, v in sorted(fam.items())),
            instructions=per_insn)
    return out


def smt_section(effects, info_recs):
    out = OrderedDict()
    for label, e in effects.items():
        parts = label.split(";")
        if len(parts) != 1:
            continue
        ax, _, v = parts[0].partition("=")
        if ax in SMT_AXES + ("speed",):
            out.setdefault(ax, OrderedDict())[v] = OrderedDict(factors=e)
    loads = defaultdict(list)
    for r in info_recs:
        ax, v = r.det.get("axis"), r.det.get("value")
        if ax and v:
            loads[(ax, v)].append(r.det)
    for (ax, v), ds in sorted(loads.items()):
        def med(key):
            xs = [fnum(d.get(key)) for d in ds if fnum(d.get(key)) is not None]
            return rnd(median(xs)) if xs else None
        li = OrderedDict(blocks=len(ds), backend=majority([d.get("backend") for d in ds]),
                         restore_failures=sum(1 for d in ds if d.get("restore") not in (None, "ok")),
                         ops_per_tick=med("sibling_ops_per_tick") if ax == "sibling" else med("ops_per_tick"),
                         stalls=sum(int(fnum(d.get("stalls")) or 0) for d in ds),
                         refused_runs=sum(int(fnum(d.get("refused_runs")) or 0) for d in ds),
                         speed_code=majority([d.get("speed_code") for d in ds]))
        out.setdefault(ax, OrderedDict()).setdefault(v, OrderedDict())["load_info"] = li
    return out


# ----------------------------------------------------------------------------------------------------------- CSV
CSV_COLS = ["mnem", "variant", "isa_unit", "reg_class", "latency_first_slot", "latency_max", "latency_slots", "rthroughput",
            "rthroughput_upper_bound", "depth", "pipelined", "issue_cost", "dispatch_cycles", "microcoded", "unpipelined",
            "unit_busy_cycles", "sched_class", "pipe_group", "load_l1", "load_l2", "load_mem", "load_cold", "use_stall",
            "store_tput_line", "data_bins_max_ratio", "baseline_latency", "delta_latency", "baseline_issue", "delta_issue",
            "runs", "single_source", "consoles"]


def csv_rows(insts):
    rows = []
    for v, o in insts.items():
        lat = o.get("latency") or {}
        allaggs = [a for a in lat.values()] + [o.get("rthroughput")] + list((o.get("issue_cost_by_source") or {}).values())
        allaggs = [a for a in allaggs if isinstance(a, dict)]
        runs = max([a.get("runs", 0) for a in allaggs] or [0])
        cons = sorted(set(c for a in allaggs for c in (a.get("by_console") or {})))
        uc = o.get("ucode") or {}
        ld = o.get("load_use_latency") or {}
        ratios = [val(b.get("ratio")) for sh in (o.get("data_dependent") or {}).values() for b in sh.values()]
        ratios = [x for x in ratios if x is not None]
        bd = o.get("baseline_delta") or {}
        busy = [val(b.get("busy_cycles")) for b in o.get("unit_busy") or [] if val(b.get("busy_cycles")) is not None]
        rt = o.get("rthroughput") or {}
        rows.append([o.get("mnem"), v, o.get("isa_unit"), o.get("reg_class"), o.get("latency_first_slot"), o.get("latency_max"),
                     ";".join("%s=%s" % (s, val(a)) for s, a in lat.items()), val(rt), bool(rt.get("upper_bound")) if rt else "",
                     val((o.get("pipelining") or {}).get("depth")), (o.get("pipelining") or {}).get("pipelined"),
                     o.get("issue_cost"), val(uc.get("dispatch_cycles")), uc.get("microcoded"), uc.get("unpipelined"),
                     max(busy) if busy else None, o.get("sched_class"), o.get("pipe_group"), val(ld.get("l1")), val(ld.get("l2")),
                     val(ld.get("mem")), val(ld.get("cold")), val(o.get("use_stall")),
                     val((o.get("store_throughput") or {}).get("line")), max(ratios) if ratios else None,
                     (bd.get("latency") or {}).get("baseline"), (bd.get("latency") or {}).get("delta"),
                     (bd.get("issue_vs_rthroughput") or {}).get("baseline"), (bd.get("issue_vs_rthroughput") or {}).get("delta"),
                     runs, runs == 1, ";".join(cons)])
    return rows


def write_csv(path, insts):
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(CSV_COLS)
        for row in csv_rows(insts):
            w.writerow(["" if x is None else x for x in row])


# ----------------------------------------------------------------------------------------------------------- LLVM .td
def llvm_ppc_dir(p):
    for c in ([p] if p else []) + [os.environ.get("XENON_LLVM"), os.path.join(os.path.dirname(ROOT), "xenon-llvm")]:
        if not c:
            continue
        for d in (c, os.path.join(c, "llvm", "lib", "Target", "PowerPC")):
            if os.path.isfile(os.path.join(d, "PPCInstrInfo.td")):
                return d
    return None


def scan_llvm(ppc_dir):
    """asm mnemonic -> [LLVM record names] for the PowerPC backend: plain defs by their asm string, defms expanded with the
    suffixes their multiclass defines (NAME, _rec, O, O_rec ...)."""
    if not ppc_dir:
        return None
    files = sorted(glob.glob(os.path.join(ppc_dir, "PPCInstr*.td")))
    texts = []
    for f in files:
        try:
            texts.append(open(f, encoding="utf-8", errors="replace").read())
        except OSError:
            pass
    mc_suffix = {}
    stripped = []
    for txt in texts:
        txt = re.sub(r"//[^\n]*", "", txt)
        out, pos = [], 0
        for m in re.finditer(r"\bmulticlass\s+(\w+)", txt):
            if m.start() < pos:
                continue
            b = txt.find("{", m.end())
            if b < 0:
                continue
            depth, i = 0, b
            while i < len(txt):
                if txt[i] == "{":
                    depth += 1
                elif txt[i] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = txt[b:i]
            sfx = []
            for d in re.finditer(r"\bdef\s+(\w*)\s*:", body):
                s = d.group(1)
                sfx.append("" if s in ("", "NAME") else s)
            mc_suffix[m.group(1)] = sfx
            out.append(txt[pos:m.start()])
            pos = i + 1
        out.append(txt[pos:])
        stripped.append("".join(out))
    amap = defaultdict(list)
    for txt in stripped:
        for m in re.finditer(r"\b(def|defm)\s+(\w+)\s*:\s*(\w+)", txt):
            kind, name, base = m.groups()
            seg = txt[m.end():m.end() + 700]
            semi = seg.find(";")
            if semi >= 0:
                seg = seg[:semi]
            s = re.search(r'"([^"]*)"', seg)
            if not s:
                continue
            asm = s.group(1).strip().split(" ")[0].split("\t")[0]
            if not asm or not re.match(r"^[a-z][a-z0-9_.]*$", asm):
                continue
            if kind == "def":
                amap[asm].append(name)
            else:
                for sfx in mc_suffix.get(base, [""]):
                    tail = {"": "", "_rec": ".", "O": "o", "O_rec": "o."}.get(sfx)
                    if tail is None:
                        continue
                    amap[asm + tail].append(name + sfx)
    return amap


# the backend spells the L-field compares by their extended mnemonics
LLVM_ASM_ALIAS = {"cmpi": ("cmpwi", "cmpdi"), "cmp": ("cmpw", "cmpd"), "cmpli": ("cmplwi", "cmpldi"), "cmpl": ("cmplw", "cmpld")}


def llvm_names(amap, variant, mnem, isa):
    if amap is None:
        return []
    asm = (isa or {}).get("asm") or mnem
    suffix = variant[len(mnem):] if variant.startswith(mnem) else ""
    tail = {"": "", ".rc": ".", ".oe": "o", ".oe.rc": "o."}.get(suffix)
    if tail is None:
        return []
    names = []
    for a in LLVM_ASM_ALIAS.get(asm, (asm,)):
        names += amap.get(a + tail, [])
    seen, out = set(), []
    for n in names:
        if n not in seen and "TLS" not in n and not n.startswith("SPE"):
            seen.add(n)
            out.append(n)
    return out


def ident(s):
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


def gen_td(model, prov, amap, ppc_dir):
    insts = model.get("instructions") or {}
    G = model.get("global") or {}
    pipes = model.get("pipes") or {}
    groups = list(pipes.get("groups") or [])
    assumed = not groups
    if assumed:
        groups = [OrderedDict(name=n, classes=c, units=1, source="ASSUMED") for n, c in ASSUMED_GROUPS.items()]
    gof = {}
    for g in groups:
        for c in g["classes"]:
            gof[c] = g["name"]

    def group_for(cls):
        """measured group of the class (or of its fallback class); else an UNVERIFIED per-unit pipe added on demand"""
        g = gof.get(cls) or gof.get(CLASS_FALLBACK.get(cls or ""))
        if g or not cls:
            return g
        base = cls if any(cls in m for m in ASSUMED_GROUPS.values()) else CLASS_FALLBACK.get(cls, cls)
        unit = next((n for n, m in ASSUMED_GROUPS.items() if base in m), "MISC")
        name = "U_" + unit
        if not any(x["name"] == name for x in groups):
            groups.append(OrderedDict(name=name, classes=[], units=1, source="UNVERIFIED (class not in unit.conflict data)"))
        grp = next(x for x in groups if x["name"] == name)
        if cls not in grp["classes"]:
            grp["classes"].append(cls)
        gof[cls] = name
        return name
    L = []
    w = L.append
    w("//===-- PPCScheduleXenon.td - Xbox 360 Xenon (PPE-class in-order) model --*- tablegen -*-===//")
    w("//")
    w("// GENERATED by cpu-test-suite tools/xts_schedmodel.py (v%d) from XTS measurements. UNVERIFIED SKETCH:" % TOOL_VERSION)
    w("// review before use; regenerate instead of editing. Values without a measurement are marked UNVERIFIED.")
    w("// Inputs: %s" % ", ".join(sorted(prov.get("runs", {}).keys()))[:400])
    w("// Consoles: %s" % (", ".join("%s (pvr %s)" % (t, (i or {}).get("pvr", "?")) for t, i in prov.get("consoles", {}).items()) or "?"))
    w("// LLVM names: %s" % (ppc_dir or "PowerPC backend not found: InstRW entries are mnemonic comments only"))
    w("//===----------------------------------------------------------------------===//")
    w("")
    lw = val((G.get("memory") or {}).get("load_use_l1_lwz")) or val(((G.get("memory") or {}).get("levels") or {}).get("l1"))
    mp = val((G.get("branch") or {}).get("mispredict_penalty"))
    iw = (G.get("issue") or {}).get("issue_width")
    w("def XenonModel : SchedMachineModel {")
    w("  let IssueWidth = 2;          // %s" % ("measured: " + G["issue"]["issue_width_source"] if iw else
                                             "UNVERIFIED (design: dual issue per thread)"))
    w("  let MicroOpBufferSize = 0;   // in-order")
    w("  let LoadLatency = %d;         // %s" % (int(round(lw)) if lw else 5, "ldst.lat lwz L1 load-to-use / mem.hier L1" if lw else
                                              "UNVERIFIED (no load latency measured)"))
    w("  let MispredictPenalty = %d;  // %s" % (int(round(mp)) if mp else 20, "branch mispredict (xorshift 50% method)" if mp else
                                              "UNVERIFIED (no mispredict measurement)"))
    w("  let CompleteModel = 0;       // only measured instructions are described")
    w("  let PostRAScheduler = 1;")
    w("}")
    w("")
    w("let SchedModel = XenonModel in {")
    w("")
    # writes (computed first: unmeasured classes add UNVERIFIED pipes)
    writes = OrderedDict()
    assign = []
    for v, o in insts.items():
        lat = o.get("latency_max")
        src = "lat.chain"
        ld = val((o.get("load_use_latency") or {}).get("l1"))
        if ld is not None:
            lat, src = ld, "ldst.lat l1"
        rt = val(o.get("rthroughput")) or o.get("issue_cost")
        cls = o.get("sched_class")
        if lat is None and rt is None:
            continue
        grp = o.get("pipe_group") or group_for(cls)
        if grp is None:
            continue
        uc = o.get("ucode") or {}
        uops = int(math.ceil(val(uc.get("dispatch_cycles")) - 0.25)) if uc.get("microcoded") == "yes" and val(uc.get("dispatch_cycles")) else 1
        L_ = int(round(lat)) if lat is not None else None
        R_ = max(1, int(round(rt))) if rt is not None else 1
        key = (grp, L_, R_, max(1, uops))
        nm = "XeW_%s_L%s_R%d%s" % (ident(grp), L_ if L_ is not None else "X", R_, "_U%d" % uops if uops > 1 else "")
        writes[key] = nm
        assign.append((v, o, nm, lat, rt, src))
    w("// ---- pipes (%s)" % ("ASSUMED one per unit: no unit.conflict data" if assumed else
                             "connected components of 'shared' edges of the unit.conflict graph (issue-blocking classes "
                             "are groups of their own); U_* = UNVERIFIED"))
    for g in groups:
        w("def XePipe_%s : ProcResource<%d> { let BufferSize = 0; }   // %s: %s" % (
            ident(g["name"]), g.get("units", 1), g.get("source", ""), " ".join(g["classes"])))
    w("")
    w("// ---- writes: latency (cycles to a dependent consumer) + pipe occupancy (from rthroughput)")
    for (grp, L_, R_, U_), nm in writes.items():
        extra = " let NumMicroOps = %d;" % U_ if U_ > 1 else ""
        w("def %s : SchedWriteRes<[XePipe_%s]> { let Latency = %s; let ReleaseAtCycles = [%d];%s }%s" % (
            nm, ident(grp), L_ if L_ is not None else R_, R_, extra, "   // UNVERIFIED latency (= occupancy)" if L_ is None else ""))
    w("")
    # bypass read advances
    w("// ---- forwarding: extra cycles beyond the producer's normal latency (bypass.matrix 'extra', >= 1 cycle).")
    w("// Negative ReadAdvance delays the consumer; attach each to the consumer operand by hand (InstRW read list).")
    wr_of_variant = {v: nm for v, o, nm, *_ in assign}
    n_adv = 0
    for k, e in ((model.get("bypass") or {}).get("entries") or {}).items():
        x = val(e.get("extra"))
        if x is None or x < 1.0 or e.get("extra_verdict") not in ("extra", None):
            continue
        rep = CLASS_REP.get(e["producer"])
        pw = wr_of_variant.get(rep[0] + rep[1]) if rep else None
        if not pw:
            w("// %s: +%.2f cycles (producer write unknown)" % (k, x))
            continue
        w("def XeRd_%s : SchedReadAdvance<-%d, [%s]>;   // %s: +%.2f cycles" % (ident(k), int(round(x)), pw, k, x))
        n_adv += 1
    if not n_adv:
        w("// (none measured)")
    w("")
    w("// ---- instruction mapping (InstRW by PowerPC backend record name)")
    used = set()
    for v, o, nm, lat, rt, src in assign:
        names = [n for n in llvm_names(amap, v, o["mnem"], (o.get("_isa") or {})) if n not in used]
        note = "%s: lat %s (%s) rthroughput %s%s" % (v, rnd(lat, 2), src, rnd(rt, 2), " UNVERIFIED-latency" if lat is None else "")
        if names:
            used.update(names)
            w("def : InstRW<[%s], (instrs %s)>;   // %s" % (nm, ", ".join(names), note))
        else:
            w("// no backend record: %s -> %s" % (note, nm))
    w("")
    w("} // SchedModel = XenonModel")
    return "\n".join(L) + "\n"


# ----------------------------------------------------------------------------------------------------------- main build
def build_all(recs, prov, isa):
    by_label = defaultdict(list)
    info_recs = []
    for r in recs:
        if is_smt_info(r):
            info_recs.append(r)
            continue
        if is_info(r):
            continue
        by_label[r.label].append(r)
    ctx = Ctx(isa, count=True)
    b = Builder(ctx)
    for r in by_label.get("default", []):
        b.add(r)
    model = b.finish()
    for v, o in model["instructions"].items():
        o["_isa"] = isa.get(o["mnem"]) or {}
    conds = OrderedDict()
    for label in sorted(by_label):
        if label == "default" or not any(r.ok and r.value is not None for r in by_label[label]):
            continue
        cb = Builder(Ctx(isa))
        for r in by_label[label]:
            cb.add(r)
        conds[label] = prune(cb.finish())
    effects = condition_effects(by_label)
    tags = sorted(set(r.tag for r in recs if r.ok and r.value is not None))
    consoles = OrderedDict()
    if len(tags) > 1:
        for t in tags:
            cb = Builder(Ctx(isa))
            for r in by_label.get("default", []):
                if r.tag == t:
                    cb.add(r)
            consoles[t] = OrderedDict(identity=prov["consoles"].get(t), model=prune(cb.finish()))
    return model, conds, effects, smt_section(effects, info_recs), consoles, ctx, by_label


def coverage(recs, model, isa, ctx):
    fam = Counter(r.family for r in recs)
    meas = [r for r in recs if r.family in MEASURE_FAMILIES and not is_info(r)]
    st = Counter(r.status for r in meas)
    usable = [r for r in meas if r.ok and r.value is not None]
    insts = model.get("instructions") or {}
    user = [m for m, i in isa.items() if i.get("privilege") == "user" and (i.get("hardware") or {}).get("status") != "illegal"]
    mn = lambda pred: len(set(o["mnem"] for o in insts.values() if pred(o)))  # noqa: E731
    cov = OrderedDict(
        records=len(recs), measurement_records=len(meas), usable_measurements=len(usable),
        statuses=dict(st), noisy_fraction=rnd(st.get("noisy", 0) / float(len(meas))) if meas else None,
        nonlinear_fraction=rnd(st.get("nonlinear", 0) / float(len(meas))) if meas else None,
        skip_fraction=rnd(st.get("skip", 0) / float(len(meas))) if meas else None,
        families_present=OrderedDict((f, fam[f]) for f in sorted(fam)),
        families_missing=[f for f in MEASURE_FAMILIES if not any(r.family == f for r in recs)],
        families_without_usable=[f for f in MEASURE_FAMILIES if fam.get(f) and not any(r.family == f for r in usable)],
        isa_user_instructions=len(user),
        instructions_with_latency=mn(lambda o: o.get("latency")),
        instructions_with_rthroughput=mn(lambda o: val(o.get("rthroughput")) is not None),
        instructions_with_issue_cost=mn(lambda o: o.get("issue_cost") is not None),
        instructions_with_ucode_verdict=mn(lambda o: (o.get("ucode") or {}).get("microcoded") in ("yes", "no")),
        microcoded=sorted(v for v, o in insts.items() if (o.get("ucode") or {}).get("microcoded") == "yes"),
        instructions_with_data_bins=mn(lambda o: o.get("data_dependent")),
        loads_with_latency=mn(lambda o: o.get("load_use_latency")),
        values=ctx.values, single_source_values=ctx.single, single_source_examples=ctx.single_keys[:20])
    return cov


def run(inputs, out, allow_host=False, consoles=None, statuses=None, include_noisy=False, td=False, llvm=None, isa_path=None,
        log=None, quiet=False, repeat_tol=0.02):
    log = log or (lambda m: print(m, file=sys.stderr))
    accept = tuple(statuses or DEFAULT_STATUSES) + (("noisy",) if include_noisy else ())
    raw, prov = load_inputs(inputs, allow_host, consoles, log)
    defaults = axis_defaults()
    isa = load_isa(isa_path or os.path.join(ROOT, "isa", "xenon_isa.json"))
    recs = normalise(raw, defaults, accept)
    promoted = promote_repeatable(recs, repeat_tol) if "noisy" not in accept else 0
    model, conds, effects, smt, cons, ctx, by_label = build_all(recs, prov, isa)
    cov = coverage(recs, model, isa, ctx)
    cov["noisy_promoted"] = promoted
    cov["repeat_tol"] = repeat_tol
    os.makedirs(out, exist_ok=True)
    insts = model["instructions"]
    amap, ppc = None, None
    if td:
        ppc = llvm_ppc_dir(llvm)
        amap = scan_llvm(ppc)
        with open(os.path.join(out, "PPCScheduleXenon.td"), "w", encoding="utf-8") as f:
            f.write(gen_td(model, prov, amap, ppc))
    for v, o in insts.items():
        o.pop("_isa", None)
        if amap is not None:
            o["llvm_records"] = llvm_names(amap, v, o["mnem"], isa.get(o["mnem"]))
    doc = OrderedDict(
        generator=OrderedDict(tool="tools/xts_schedmodel.py", version=TOOL_VERSION, statuses_accepted=list(accept),
                              axis_defaults=defaults, dimension_axes={k: list(v) for k, v in DIM_AXES.items()},
                              value_format="{value: median, min, max, mad, n: records, runs, single_source, by_console, "
                                           "console_spread}"),
        provenance=prov, coverage=cov, model=prune(model), conditions=conds, condition_effects=effects, smt=prune(smt),
        consoles=cons)
    with open(os.path.join(out, "schedmodel.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1, default=str)
        f.write("\n")
    write_csv(os.path.join(out, "schedmodel.csv"), insts)
    if not quiet:
        print_summary(doc, out, td, ppc)
    return (0 if cov["usable_measurements"] else 3), doc


def print_summary(doc, out, td, ppc):
    cov, prov = doc["coverage"], doc["provenance"]
    p = print
    p("xts_schedmodel: %d records from %d run(s), %d console(s); %d duplicates dropped, %d conflicts, %d refused input(s)" % (
        cov["records"], len(prov["runs"]), len(set(r["console_tag"] for r in prov["runs"].values())), prov["duplicates_dropped"],
        prov["conflicts"], len(prov["refused"])))
    if not cov["usable_measurements"]:
        p("no measurements: %d measurement records, none usable (statuses %s) - host dry run or nothing executed" % (
            cov["measurement_records"], " ".join("%s=%d" % kv for kv in sorted(cov["statuses"].items())) or "-"))
    else:
        p("usable measurements %d of %d (nonlinear %.1f %%, noisy %.1f %%, skip %.1f %%)" % (
            cov["usable_measurements"], cov["measurement_records"], 100 * (cov["nonlinear_fraction"] or 0),
            100 * (cov["noisy_fraction"] or 0), 100 * (cov["skip_fraction"] or 0)))
    p("instructions (of %d user ISA): latency %d, rthroughput %d, issue cost %d, ucode verdict %d (%d microcoded), data bins %d, "
      "loads %d" % (cov["isa_user_instructions"], cov["instructions_with_latency"], cov["instructions_with_rthroughput"],
                    cov["instructions_with_issue_cost"], cov["instructions_with_ucode_verdict"], len(cov["microcoded"]),
                    cov["instructions_with_data_bins"], cov["loads_with_latency"]))
    m = doc["model"]
    G = m.get("global") or {}
    p("pipes: %d group(s) %s; pairs %d; bypass entries %d" % (
        len((m.get("pipes") or {}).get("groups") or []), " ".join("[%s]" % " ".join(g["classes"]) for g in
                                                                  ((m.get("pipes") or {}).get("groups") or [])[:6]),
        len(m.get("pairing") or {}), len(((m.get("bypass") or {}).get("entries")) or {})))
    br, mem = G.get("branch") or {}, G.get("memory") or {}
    p("branch: mispredict %s, taken bubble %s, link stack %s; memory: L1 %s L2 %s mem %s; cycles/tick %s" % (
        val(br.get("mispredict_penalty")), val(br.get("taken_bubble")), val((br.get("link_stack") or {}).get("depth")),
        val((mem.get("levels") or {}).get("l1")), val((mem.get("levels") or {}).get("l2")), val((mem.get("levels") or {}).get("mem")),
        val(((G.get("clock") or {}).get("cycles_per_tick") or {}).get("default"))))
    if cov["families_missing"]:
        p("families missing: %s" % " ".join(cov["families_missing"]))
    if cov["families_without_usable"]:
        p("families without usable values: %s" % " ".join(cov["families_without_usable"]))
    p("condition sections: %d (%s); single-source values %d of %d" % (
        len(doc["conditions"]), " ".join(list(doc["conditions"])[:6]) + (" ..." if len(doc["conditions"]) > 6 else ""),
        cov["single_source_values"], cov["values"]))
    p("wrote %s/schedmodel.json, schedmodel.csv%s" % (out, (", PPCScheduleXenon.td (LLVM names: %s)" % ("yes" if ppc else "no")) if td else ""))
    for wmsg in prov["warnings"][:5]:
        p("warning: %s" % wmsg)


# ----------------------------------------------------------------------------------------------------------- self test
COND0 = dict(FALLBACK_DEFAULTS)


DIFF_METRICS = ("penalty", "extra", "ratio")          # emitted with xts_time_emit_diff / _value: "fit" + "ref", stats null
NOSTAT_METRICS = DIFF_METRICS + ("overlap", "count", "depth", "info", "result")


def _fit(cycles, per=1.0, cpt=64.0):
    """the v2 fit schema (xts_time_fit_json) for a measurement of `cycles` per event"""
    cpu = None if cycles is None else cycles * per
    sl = None if cpu is None else cpu / cpt
    depths = [8, 16, 24, 32, 48, 64]
    return OrderedDict(unit="", iters=256, status="pass", cpu=cpu, slope_ticks=sl, intercept_ticks=0.5, r2=0.9995,
                       max_resid_rel=0.002, spread=0.004, aba_shift=0.001, slope_se=None if sl is None else sl * 1e-3,
                       depths=depths, ticks_med=[None if sl is None else 0.5 + sl * d for d in depths],
                       ticks_mad=[0.001] * len(depths), cycles_per_tick=cpt, per_unit=per, kernel_words_max=80)


def _rec(seq, family, case, metric, value, status="pass", mnem="", detail=None, cond=None, unit="cycles", run_id="0001",
         per=1.0):
    """one record as the v2 emitters write it (src/timing/timing.c emit_core): detail = family members + "fit" (+ "ref" for
    diffs, + baseline/baseline_key); min/median/mad only when value is the per-unit fit figure"""
    c = dict(COND0)
    c.update(cond or {})
    d = OrderedDict(detail or {})
    timed = metric not in ("count", "info", "result", "depth") and family not in ("exec.ppc",)
    if timed and "fit" not in d:
        d["fit"] = _fit(value, per)
        if metric in DIFF_METRICS:
            d["ref"] = _fit(1.0, per)
    stats = value is not None and metric not in NOSTAT_METRICS
    fin = case in ("chase.levels", "tlb.knees", "graph") or (family == "bypass.matrix" and metric == "latency")
    return {"run_id": run_id, "seq": seq, "family": family, "case_id": case, "mnem": mnem, "word": "", "cond": c,
            "block": "r0:%s:%s" % (family, "finish" if fin else "-:c0"), "metric": metric, "value": value, "unit": unit,
            "n": (60 if metric in DIFF_METRICS else 30) if timed else None, "min": value if stats else None,
            "median": value if stats else None, "mad": 0.01 if stats else None, "fit_r2": 0.999 if timed else None,
            "status": status, "detail": d}


def _fixture(scale=1.0, sib=None, v1_info=False):
    """records in the emitters' formats with planted values; scale multiplies the addx latency (console B differs)"""
    R = []
    s = [0]

    def add(*a, **k):
        s[0] += 1
        R.append(_rec(s[0], *a, **k))
    for rep in range(3):
        add("lat.chain", "addx:RA", "latency", 2.0 * scale + 0.01 * rep, mnem="addx",
            detail={"variant": "addx", "slot": "RA", "kind": "chain", "class": "gpr", "cycles_per_tick": 64})
        add("lat.chain", "addx:RB", "latency", 2.0 * scale, mnem="addx", detail={"variant": "addx", "slot": "RB", "kind": "chain", "class": "gpr"})
        add("lat.chain", "addx.rc:RA", "latency", 2.0, mnem="addx", detail={"variant": "addx.rc", "slot": "RA", "kind": "chain", "class": "gpr"})
        add("lat.chain", "fdivx:FA", "latency", 74.0, mnem="fdivx", detail={"variant": "fdivx", "slot": "FA", "kind": "chain", "class": "fpr"})
        add("lat.chain", "faddx:FA", "latency", 10.0, mnem="faddx", detail={"variant": "faddx", "slot": "FA", "kind": "chain", "class": "fpr"})
        add("lat.chain", "vaddfp128:VA128", "latency", 12.0, mnem="vaddfp128",
            detail={"variant": "vaddfp128", "slot": "VA128", "kind": "chain", "class": "vr"})
        add("lat.chain", "lwz:x", "latency", 99.0, status="noisy", mnem="addx", detail={"variant": "addx", "slot": "RA", "kind": "chain"})
        add("tput.indep", "addx", "rthroughput", 1.0, mnem="addx", per=2.0,
            detail={"variant": "addx", "class": "gpr", "slot": "RA", "n_sat": 2, "pipelined": True, "latency_n1": 2.0,
                    "curve": [{"n": 1, "cycles": 2.0, "status": "pass", "r2": 0.999, "spread": 0.003, "filler": 1.0,
                               "discriminates": True},
                              {"n": 2, "cycles": 1.0, "status": "pass", "r2": 0.999, "spread": 0.004, "filler": 1.0,
                               "discriminates": True},
                              {"n": 4, "cycles": 1.0, "status": "noisy", "why": "spread 3 %", "filler": 1.0}],
                    "points_executed": 3})
        add("tput.indep", "addx", "depth", 2.0, mnem="addx", unit="count", detail={"variant": "addx", "n_sat": 2})
        add("tput.indep", "fdivx", "rthroughput", 74.0, mnem="fdivx",
            detail={"variant": "fdivx", "class": "fpr", "pipelined": False, "curve": [{"n": 1, "cycles": 74.0, "status": "pass"}]})
        add("tput.indep", "faddx", "rthroughput", 1.0, mnem="faddx", detail={"variant": "faddx", "pipelined": True})
        add("tput.indep", "vaddfp128", "rthroughput", 1.0, mnem="vaddfp128", detail={"variant": "vaddfp128", "pipelined": True})
        add("lat.data", "faddx:normal", "issue_cost", 1.0, mnem="faddx", detail={"bin": "normal", "shape": "indep", "class": "fpr"})
        add("lat.data", "faddx:denormal_in", "issue_cost", 9.0, mnem="faddx", detail={"bin": "denormal_in", "shape": "indep", "class": "fpr"})
        add("lat.data", "faddx:denormal_in", "ratio", 9.0, mnem="faddx", unit="ratio",
            detail={"bin": "denormal_in", "shape": "indep", "class": "fpr", "cycles": 9.0, "normal_cycles": 1.0})
        add("lat.data", "vaddfp128:denormal:nj1", "ratio", 1.0, mnem="vaddfp128", unit="ratio",
            detail={"bin": "denormal", "shape": "indep", "class": "vr", "vscr_nj": 1}, cond={"vscr_nj": "1"})
        add("ucode", "mtcrf", "issue_cost", 6.0, mnem="mtcrf", detail={"shape": "one X per unit, independent"})
        add("ucode", "mtcrf", "extra", 6.0, mnem="mtcrf",
            detail={"unit": "integer", "variant": "base", "microcoded": "yes", "neighbour": "blocks", "unpipelined": False,
                    "blocks_dual_issue": True})
        add("ucode", "addx", "issue_cost", 1.0, mnem="addx")
        add("ucode", "addx", "extra", 0.1, mnem="addx", detail={"microcoded": "no", "neighbour": "hidden", "unpipelined": False})
        add("ucode", "fdivx", "extra", 1.0, mnem="fdivx", detail={"microcoded": "no", "neighbour": "one_slot", "unpipelined": True})
        add("issue.pair", "control.filler", "issue_cost", 1.0, mnem="addi")
        for c, v in (("int", 1.0), ("fp", 1.0), ("mul", 2.0)):
            add("issue.pair", "alone." + c, "issue_cost", v, mnem=CLASS_REP[c][0], detail={"class": c})
        add("issue.pair", "pair.int+fp", "cycles", 1.05, detail={"order": "A>B", "classes": ["int", "fp"], "overlap": 0.95,
                                                                 "verdict": "yes", "order_dependent": False, "first": "addx", "second": "faddx"})
        add("issue.pair", "pair.int+fp", "cycles", 2.0, detail={"order": "B>A", "classes": ["fp", "int"], "overlap": 0.0,
                                                                "verdict": "no", "order_dependent": True})
        for c, v in (("int", 1.0), ("mul", 2.0), ("div", 18.0), ("fp", 1.0)):
            add("unit.conflict", "alone." + c, "rthroughput", v, mnem=CLASS_REP[c][0], detail={"class": c})
        add("unit.conflict", "mix.mul+div", "overlap", 0.05, unit="ratio", detail={"classes": ["mul", "div"], "verdict": "shared",
                                                                                    "overlap_err": 0.02, "overlap_by_ratio": [0.05, 0.1, 0.0]})
        add("unit.conflict", "mix.int+fp", "overlap", 0.97, unit="ratio", detail={"classes": ["int", "fp"], "verdict": "separate"})
        add("unit.conflict", "mix.int+mul", "overlap", 0.9, unit="ratio", detail={"classes": ["int", "mul"], "verdict": "separate"})
        add("unit.conflict", "busy.div/int", "penalty", 1.0, mnem="divwx",
            detail={"producer": "div", "stream": "int", "busy_cycles": 1.0, "verdict": "non-blocking"})
        # an issue-blocking class serialises with everything: its shared edges must not merge int and fp
        add("unit.conflict", "mix.fdiv+fp", "overlap", 0.02, unit="ratio", detail={"classes": ["fdiv", "fp"], "verdict": "shared"})
        add("unit.conflict", "mix.fdiv+int", "overlap", 0.03, unit="ratio", detail={"classes": ["fdiv", "int"], "verdict": "shared"})
        add("unit.conflict", "busy.fdiv/fp", "penalty", 80.0, mnem="fdivx",
            detail={"producer": "fdiv", "stream": "fp", "busy_cycles": 80.0, "verdict": "blocking"})
        add("bypass.matrix", "lwz>int.RA", "penalty", 4.9, mnem="addx",
            detail={"producer": "lwz", "consumer": "int", "producer_mnem": "lwz", "consumer_mnem": "addx", "slot": "RA",
                    "slot_role": "source", "distance": 0, "verdict": "stall"})
        add("bypass.matrix", "lwz>int.RA", "extra", 1.9, mnem="addx",
            detail={"producer": "lwz", "consumer": "int", "producer_mnem": "lwz", "consumer_mnem": "addx", "slot": "RA",
                    "slot_role": "source", "distance": 0, "reference": "int", "reference_slot": "RA", "verdict": "extra"})
        for d, pen in zip((0, 4, 8, 16, 24, 32), (22.0, 20.0, 18.0, 10.0, 0.1, 0.0)):
            add("bypass.matrix", "mtctr>bctr.d%d" % d, "penalty", pen, mnem="bcctrx",
                detail={"producer": "mtctr", "consumer": "bctr", "slot": "CTR", "distance": d})
        add("bypass.matrix", "dist.int>lwz.RA0.d16", "latency", 4.0, mnem="lwz",
            detail={"producer": "int", "consumer": "lwz", "slot": "RA0", "distance": 16, "verdict": "hidden"})
        add("branch", "pred/taken_extra", "extra", 0.5, mnem="bcx")
        add("branch", "mispredict/penalty", "penalty", 24.7, mnem="bcx")
        add("branch", "linkstack/depth", "depth", 4.0, mnem="bclrx", unit="entries", detail={"jump": True, "per_level_cycles": 12.0})
        add("branch", "loop/overhead", "cycles", 7.2, mnem="bcx")
        add("branch", "tk_bc.d4/tput", "rthroughput", 5.5, mnem="bcx", detail={"distance": 4})
        add("branch", "align.b.m0.p12/tput", "rthroughput", 9.0, mnem="bx")
        add("branch", "align.b.m0.p0/tput", "rthroughput", 8.0, mnem="bx")
        add("branch", "bctr.d8/penalty", "penalty", 21.6, mnem="bcctrx", detail={"distance": 8})
        add("sysops", "barrier.sync/alone", "issue_cost", 165.0, mnem="sync", detail={"producer": "sync", "variant": "alone"})
        add("sysops", "spr.lr/roundtrip", "latency", 5.2, mnem="mtspr")
        add("calib", "cpt/cycles_per_tick", "ratio", 64.0, mnem="addx", unit="ratio",
            detail={"producer": "addx", "consumer": "addx", "variant": "cycles_per_tick_from_add_chains", "consistent": True})
        add("calib", "tb_rtc/tb_hz", "count", 49875000.0, mnem="mftb", unit="hz", detail={"producer": "smc_rtc", "consumer": "mftb"})
        for lv, v in (("default", 5.0), ("l2", 45.0), ("mem", 637.0)):
            add("ldst.lat", "lat.lwz", "latency", v, mnem="lwz", detail={"level": "l1" if lv == "default" else lv, "form": "g"},
                cond={"cache": lv})
        add("ldst.lat", "lat.lwz", "latency", 7.0, mnem="lwz", detail={"level": "l1"}, cond={"data_align": "line_cross"})
        add("ldst.lat", "stp.stw", "rthroughput", 1.0, mnem="stw", detail={"pattern": "line"})
        add("ldst.lat", "lhs.stw.lwz.exact", "penalty", 49.0, mnem="lwz", detail={"store": "stw", "load": "lwz", "relation": "exact"})
        add("ldst.lat", "mis.lwz.line.lat", "penalty", 2.0, mnem="lwz", detail={"kind": "line"})
        for lv, v in (("l1", 5.01), ("l2", 45.2), ("mem", 637.7)):
            add("mem.hier", "chase.levels", "latency", v, mnem="ld", detail={"level": lv})
        add("mem.hier", "chase.levels", "count", 32768.0, unit="bytes", detail={"what": "capacity", "level": "l1"})
        add("mem.hier", "assoc.l1", "count", 4.0, unit="", detail={"what": "ways", "level": "l1", "set_stride": 8192, "verdict": "4-way"})
        for kb, v in ((16, 5.0), (32, 5.1), (64, 45.0), (2048, 637.0)):
            add("mem.hier", "chase.rand.%dk" % kb, "latency", v, mnem="ld", detail={"size": kb * 1024})
        add("mem.hier", "pfstream.none", "bytes_per_cycle", 0.2, unit="bytes_per_cycle", detail={"kind": "ld_line_prefetch", "prefetch": "none"})
        add("mem.hier", "pfstream.dcbt", "bytes_per_cycle", 0.6, unit="bytes_per_cycle", mnem="dcbt",
            detail={"kind": "ld_line_prefetch", "prefetch": "dcbt"})
        add("mem.hier", "tlb.knees", "count", 64.0, unit="", detail={"what": "erat_reach_pages"})
        # non-default conditions
        add("lat.chain", "addx:RA", "latency", 2.0 * scale * (sib or 2.0), mnem="addx", cond={"sibling": "busy_int"},
            detail={"variant": "addx", "slot": "RA", "kind": "chain", "class": "gpr"})
        add("lat.chain", "addx:RA", "latency", 2.5, mnem="addx", cond={"code_align": "4"},
            detail={"variant": "addx", "slot": "RA", "kind": "chain", "class": "gpr"})
    s[0] += 1
    R.append(_rec(s[0], "lat.chain", "smt:sibling", "smt_sibling" if v1_info else "info", None if v1_info else 4096.0,
                  status="info", cond={"sibling": "busy_int"}, unit="" if v1_info else "count",
                  detail={"axis": "sibling", "value": "busy_int", "backend": "console", "restore": "ok", "sibling_ops_per_tick": 50.0}))
    s[0] += 1
    R.append(_rec(s[0], "bypass.matrix", "dist.summary", "info", 3.0, status="info", unit="count",
                  detail={"why": "scan table full: records dropped"}))
    s[0] += 1
    R.append(_rec(s[0], "exec.ppc", "addx#1", "result", None, mnem="addx"))
    return R


def _write_run(base, tag, run_id, recs, host=False, ident=None, extra=None):
    d = os.path.join(base, tag, run_id)
    os.makedirs(d, exist_ok=True)
    plat = {"name": "host"} if host else dict({"name": "console", "speed_code": 1}, **(ident or {}))
    si = {"suite": "xts", "profile": "full", "build_id": "b1", "git_hash": "abc", "run_id": run_id, "console_tag": tag, "boot": 1,
          "cycles_per_tick": 64, "platform": plat}
    with open(os.path.join(d, "sysinfo.json"), "w") as f:
        json.dump(si, f)
    with open(os.path.join(d, "results.jsonl"), "w") as f:
        for r in recs:
            r = dict(r, run_id=run_id)
            f.write(json.dumps(r) + "\n")
        if extra:
            f.write(extra)
    with open(os.path.join(d, "journal.txt"), "w") as f:
        f.write("START full b1\nDONE\n")
    return d


TD_FIXTURE = r'''
// minimal PowerPC backend excerpt for the self test
multiclass XOForm_1rx<bits<6> opcode, bits<9> xo, bit oe, dag OOL, dag IOL, string asmbase, string asmstr, InstrItinClass itin,
                      list<dag> pattern> {
  let BaseName = asmbase in {
    def NAME : XOForm_1<opcode, xo, oe, OOL, IOL, !strconcat(asmbase, !strconcat(" ", asmstr)), itin, pattern>, RecFormRel;
    def _rec : XOForm_1<opcode, xo, oe, OOL, IOL, !strconcat(asmbase, !strconcat(". ", asmstr)), itin, []>, isRecordForm;
  }
  let BaseName = !strconcat(asmbase, "O") in {
    def O    : XOForm_1<opcode, xo, 1, OOL, IOL, !strconcat(asmbase, !strconcat("o ", asmstr)), itin, []>, RecFormRel;
    def O_rec : XOForm_1<opcode, xo, 1, OOL, IOL, !strconcat(asmbase, !strconcat("o. ", asmstr)), itin, []>, isRecordForm;
  }
}
defm ADD4  : XOForm_1rx<31, 266, 0, (outs gprc:$RT), (ins gprc:$RA, gprc:$RB),
                        "add", "$RT, $RA, $RB", IIC_IntSimple, [(set i32:$RT, (add i32:$RA, i32:$RB))]>;
def LWZ : DForm_1<32, (outs gprc:$RST), (ins (memri $D, $RA):$addr), "lwz $RST, $addr", IIC_LdStLoad, []>;
def FDIV : AForm_2<63, 18, (outs f8rc:$FRT), (ins f8rc:$FRA, f8rc:$FRB), "fdiv $FRT, $FRA, $FRB", IIC_FPDivD, []>;
'''


def selftest():
    ok = True

    def check(c, msg):
        nonlocal ok
        print(("  ok   " if c else "  FAIL ") + msg)
        ok = ok and bool(c)

    tmp = tempfile.mkdtemp(prefix="xts_schedmodel_selftest_")
    try:
        ia = {"console_type": "jasper", "pvr": "00710500", "cpu": "Waternoose"}
        ib = {"console_type": "falcon", "pvr": "00710200", "cpu": "Loki"}
        usb = os.path.join(tmp, "usb")
        ra = _fixture(1.0)
        _write_run(usb, "jasper-A", "0001", ra, ident=ia)
        _write_run(os.path.join(tmp, "copy2"), "jasper-A", "0001", ra, ident=ia)              # same run copied twice
        conf = [dict(r) for r in ra]
        conf[0]["value"] = 77.0                                                               # same key, other content
        _write_run(os.path.join(tmp, "conf"), "jasper-A", "0001", conf, ident=ia)
        _write_run(usb, "falcon-B", "0001", _fixture(1.1, sib=1.5, v1_info=True), ident=ib, extra='{"run_id": "0001", "seq": 99')   # truncated tail
        _write_run(os.path.join(tmp, "dry"), "host", "0001", [_rec(1, "lat.chain", "addx:RA", "latency", None, status="skip",
                                                                   detail={"backend": "host-model"})], host=True)
        ppc = os.path.join(tmp, "llvm", "llvm", "lib", "Target", "PowerPC")
        os.makedirs(ppc)
        with open(os.path.join(ppc, "PPCInstrInfo.td"), "w") as f:
            f.write(TD_FIXTURE)
        out = os.path.join(tmp, "out")
        msgs = []
        code, doc = run([usb, os.path.join(tmp, "copy2"), os.path.join(tmp, "conf"), os.path.join(tmp, "dry")], out, td=True,
                        llvm=os.path.join(tmp, "llvm"), log=msgs.append, quiet=True)
        J = json.load(open(os.path.join(out, "schedmodel.json")))
        M = J["model"]
        I = M["instructions"]
        P = J["provenance"]
        check(code == 0, "exit 0 with measurements (got %d)" % code)
        check(any("REFUSED" in m for m in msgs) and len(P["refused"]) == 1, "host dry-run input refused")
        check(P["duplicates_dropped"] == 2 * len(ra) - 1 and P["conflicts"] == 1, "duplicates dropped (%d) and conflict counted (%d)" % (
            P["duplicates_dropped"], P["conflicts"]))
        check(P["truncated_tails"] == 1, "truncated last line ignored")
        a = I["addx"]["latency"]["RA"]
        check(abs(a["value"] - 2.11) < 0.01 and a["n"] == 6 and a["runs"] == 2, "addx RA latency median over 2 runs (%s)" % a)
        check(set(a.get("by_console", {})) == {"jasper-A", "falcon-B"} and abs(a["console_spread"] - 0.2) < 0.03,
              "per-console medians + spread")
        check(I["addx"]["latency"]["RA"]["max"] < 50, "noisy record excluded by default")
        check(I["addx.rc"]["latency"]["RA"]["value"] == 2.0, "flag variant addx.rc kept separate")
        check(I["fdivx"]["latency_max"] == 74.0 and I["fdivx"]["rthroughput"]["value"] == 74.0 and
              I["fdivx"]["pipelining"]["pipelined"] is False, "fdivx latency/rthroughput/unpipelined")
        check(I["addx"]["pipelining"]["curve"] == {"1": 2.0, "2": 1.0} and I["addx"]["pipelining"]["depth"]["value"] == 2.0,
              "throughput curve (noisy point dropped) + depth")
        check(I["mtcrf"]["ucode"]["microcoded"] == "yes" and I["mtcrf"]["ucode"]["extra_dispatch_cycles"] == 5.0,
              "ucode: mtcrf microcoded, 5 extra dispatch cycles")
        check(I["addx"]["issue_cost_by_source"]["ucode"]["value"] == 1.0 and I["addx"]["issue_cost"] == 1.0, "issue cost sources")
        dd = I["faddx"]["data_dependent"]["indep"]
        check(dd["denormal_in"]["ratio"]["value"] == 9.0 and "normal" in dd, "data-dependent bin ratio faddx denormal_in = 9")
        check(I["vaddfp128"]["data_dependent"]["indep"]["denormal.nj1"]["ratio"]["value"] == 1.0, "VR bin keyed by NJ (dimension axis)")
        ll = I["lwz"]["load_use_latency"]
        check(ll["l1"]["value"] == 5.0 and ll["l2"]["value"] == 45.0 and ll["mem"]["value"] == 637.0 and "l1@line_cross" in ll,
              "load-use latency per cache level (cache axis as dimension) + misaligned key")
        check(I["stw"]["store_throughput"]["line"]["value"] == 1.0, "store throughput")
        groups = [sorted(g["classes"]) for g in M["pipes"]["groups"]]
        check(["div", "mul"] in groups and ["int"] in groups, "pipe groups from shared edges: %s" % groups)
        ib = M["pipes"]["issue_blocking"]
        check(["fdiv"] in groups and ["fp"] in groups and ib["fdiv"]["candidates"] == ["fp", "int"] and ib["fdiv"]["blocks"] == ["fp"],
              "issue-blocking class does not bridge pipe groups and stays alone: %s %s" % (groups, dict(ib)))
        check(I["divwx"]["pipe_group"] == "mul_div" and I["divwx"]["pipe_group_source"] == "class representative",
              "pipe group on class representative")
        pr = M["pairing"]
        check(pr["int>fp"]["verdict"] == "yes" and pr["fp>int"]["verdict"] == "no" and pr["int>fp"]["overlap"]["value"] == 0.95,
              "ordered dual-issue table")
        bp = M["bypass"]["entries"]["lwz>int.RA"]
        check(bp["extra"]["value"] == 1.9 and bp["penalty"]["value"] == 4.9 and M["bypass"]["extra_matrix"]["lwz"]["int"]["RA"] == 1.9,
              "bypass matrix producer x consumer x slot")
        rb = M["register_branch"]["mtctr>bctr"]
        check(rb["bypass.matrix"]["hide_distance"] == 24 and rb["branch"]["penalty_by_distance"]["8"]["value"] == 21.6,
              "register-branch hide distance 24")
        G = M["global"]
        check(G["branch"]["mispredict_penalty"]["value"] == 24.7 and G["branch"]["link_stack"]["depth"]["value"] == 4.0 and
              G["branch"]["taken_bubble"]["value"] == 0.5 and G["branch"]["loop"]["overhead_cycles"]["value"] == 7.2,
              "branch costs")
        check(G["branch"]["alignment"]["worst"][0] == "b.m0.p12", "alignment effects")
        mem = G["memory"]
        check(mem["levels"]["l2"]["value"] == 45.2 and mem["capacity_bytes"]["l1"]["value"] == 32768 and
              mem["associativity"]["l1"]["ways"]["value"] == 4, "memory levels/capacity/associativity")
        check(mem["knees_bytes"] and mem["knees_bytes"][0]["below"] == 32768, "knee found at 32K")
        check(mem["store_to_load"]["lhs"]["stw"]["lwz"]["exact"]["value"] == 49.0, "store->load penalty matrix")
        check(mem["misaligned_penalty"]["line"]["lwz.lat"]["value"] == 2.0, "misaligned penalty")
        check(abs(mem["prefetch_stream"]["dcbt"]["speedup_vs_none"] - 3.0) < 1e-6, "prefetch effectiveness")
        check(G["barriers"]["sync"]["alone"]["value"] == 165.0, "barrier cost")
        check(G["clock"]["cycles_per_tick"]["default"]["value"] == 64.0, "cycles per tick from calib")
        check(G["issue"]["issue_width"] == 2, "issue width from co-issuing pairs")
        check("code_align=4" in J["conditions"] and "sibling=busy_int" in J["conditions"], "non-default condition sections")
        sm = J["smt"]["sibling"]["busy_int"]
        check(abs(sm["factors"]["instructions"]["addx"]["latency"]["RA"] - 1.74) < 0.02 and sm["load_info"]["ops_per_tick"] == 50.0 and
              sm["load_info"]["blocks"] == 2, "SMT factor (median of 2.0x and 1.5x consoles) + load info")
        check("jasper-A" in J["consoles"] and "falcon-B" in J["consoles"], "per-console sections")
        bd = I["addx"].get("baseline_delta", {})
        check(J["coverage"]["instructions_with_latency"] >= 4 and "noisy_fraction" in J["coverage"], "coverage block")
        check(isinstance(bd, dict), "baseline delta present (ISA JSON %s)" % ("found" if load_isa(os.path.join(ROOT, "isa", "xenon_isa.json")) else "absent"))
        rows = list(csv.DictReader(open(os.path.join(out, "schedmodel.csv"))))
        check(len(rows) == len(I) and {"latency_slots", "rthroughput", "microcoded", "pipe_group"} <= set(rows[0]), "csv table")
        td = open(os.path.join(out, "PPCScheduleXenon.td")).read()
        check("MicroOpBufferSize = 0" in td and "IssueWidth = 2" in td and "LoadLatency = 5;" in td and "MispredictPenalty = 25;" in td,
              "td: model header")
        check(re.search(r"InstRW<\[XeW_\w+_L2_R1\], \(instrs ADD4\)>", td) and "instrs ADD4_rec" in td and "instrs FDIV" in td,
              "td: InstRW by backend names (ADD4, ADD4_rec, FDIV)")
        check("SchedReadAdvance<-2" in td and "GENERATED" in td and "UNVERIFIED" in td, "td: bypass ReadAdvance + markings")
        check("no backend record: vaddfp128" in td, "td: mnemonic comment where the backend has no record")
        # include-noisy changes the addx RA set
        code2, doc2 = run([usb], os.path.join(tmp, "out2"), include_noisy=True, quiet=True, log=lambda m: None)
        check(doc2["model"]["instructions"]["addx"]["latency"]["RA"]["max"] == 99.0, "--include-noisy includes the noisy record")
        # console filter
        code3, doc3 = run([usb], os.path.join(tmp, "out3"), consoles=["falcon-B"], quiet=True, log=lambda m: None)
        check(doc3["model"]["instructions"]["addx"]["latency"]["RA"]["value"] == 2.21 and not doc3["consoles"], "console filter")
        # merged.jsonl input (as xts_merge writes it)
        # string / multiple ops from sysops str.*: per byte count, the smallest count is the issue cost
        sdir = os.path.join(tmp, "strops")
        _write_run(sdir, "jasper-A", "0004", [_rec(1, "sysops", "str.lswi/n4", "issue_cost", 6.0, mnem="lswi", detail={"bytes": 4}),
                                              _rec(2, "sysops", "str.lswi/n32", "issue_cost", 20.0, mnem="lswi", detail={"bytes": 32})],
                   ident=ia)
        code7, doc7 = run([sdir], os.path.join(tmp, "out7"), quiet=True, log=lambda m: None)
        L7 = ((doc7.get("model") or {}).get("instructions") or {}).get("lswi") or {}
        check(L7.get("issue_cost") == 6.0 and set((L7.get("issue_cost_by_bytes") or {}).keys()) == {"4", "32"},
              "string op issue cost by byte count (smallest count = issue cost)")
        # repeatable noisy: two runs that agree (stwx 2.90 / 2.93) are accepted and tagged; stb disagrees (2.9 / 3.5)
        rdir = os.path.join(tmp, "repeat")
        for rid, sv, bv in (("0002", 2.90, 2.9), ("0003", 2.93, 3.5)):
            _write_run(rdir, "jasper-A", rid, [_rec(1, "ucode", "stwx", "issue_cost", sv, status="noisy", mnem="stwx"),
                                               _rec(2, "ucode", "stb", "issue_cost", bv, status="noisy", mnem="stb")], ident=ia)
        code5, doc5 = run([rdir], os.path.join(tmp, "out5"), quiet=True, log=lambda m: None)
        I5 = doc5["model"]["instructions"]
        sw = (I5.get("stwx") or {}).get("issue_cost_by_source", {}).get("ucode") or {}
        check(sw.get("repeatable_noisy") is True and abs((sw.get("value") or 0) - 2.915) < 1e-6 and
              (I5.get("stb") or {}).get("issue_cost") is None and doc5["coverage"]["noisy_promoted"] == 2,
              "repeatable noisy values accepted and tagged, disagreeing ones not")
        code6, doc6 = run([rdir], os.path.join(tmp, "out6"), quiet=True, log=lambda m: None, repeat_tol=0)
        check(((doc6.get("model") or {}).get("instructions") or {}).get("stwx", {}).get("issue_cost") is None and
              doc6["coverage"]["noisy_promoted"] == 0, "--repeat-tol 0 turns it off")
        mdir = os.path.join(tmp, "merged")
        mc, _ = XM.merge([usb], mdir, False, log=lambda m: None)
        code4, doc4 = run([mdir], os.path.join(tmp, "out4"), quiet=True, log=lambda m: None)
        check(doc4["model"]["instructions"]["fdivx"]["latency_max"] == 74.0 and doc4["provenance"]["consoles"].get("jasper-A", {}).get("pvr")
              == "00710500", "merged.jsonl input with console identity")
        # host-only: no measurements, still writes the files
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code5, doc5 = run([os.path.join(tmp, "dry")], os.path.join(tmp, "out5"), allow_host=True, td=True, log=lambda m: None)
        check(code5 == 3 and "no measurements" in buf.getvalue() and os.path.isfile(os.path.join(tmp, "out5", "schedmodel.json")),
              "host-only input: 'no measurements', exit 3, files written")
        # partial run: only branch records
        _write_run(os.path.join(tmp, "part"), "jasper-A", "0002", [r for r in ra if r["family"] == "branch"], ident=ia)
        code6, doc6 = run([os.path.join(tmp, "part")], os.path.join(tmp, "out6"), td=True, quiet=True, log=lambda m: None)
        check(code6 == 0 and "lat.chain" in doc6["coverage"]["families_missing"] and
              "ASSUMED" in open(os.path.join(tmp, "out6", "PPCScheduleXenon.td")).read(), "partial run: missing families, assumed pipes")
    finally:
        if os.environ.get("XTS_KEEP_SELFTEST"):
            print("kept %s" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    print("selftest %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="build a scheduling machine model from XTS results",
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("inputs", nargs="*", help="merged.jsonl, a directory holding it, or run directories / their parents")
    ap.add_argument("-o", "--out", default="schedmodel", help="output directory (default: schedmodel)")
    ap.add_argument("--console", action="append", help="only this console_tag (repeatable)")
    ap.add_argument("--include-noisy", action="store_true", help="also use records with status noisy")
    ap.add_argument("--repeat-tol", type=float, default=0.02,
                    help="accept noisy records of one measurement when >= 2 runs agree within this fraction of their median "
                         "(tagged repeatable_noisy; default 0.02, 0 = off)")
    ap.add_argument("--statuses", help="comma list of accepted statuses (default: %s)" % ",".join(DEFAULT_STATUSES))
    ap.add_argument("--allow-host", action="store_true", help="accept host dry-run output (it has no measurements)")
    ap.add_argument("--td", action="store_true", help="also write PPCScheduleXenon.td (LLVM SchedMachineModel sketch)")
    ap.add_argument("--llvm", help="xenon-llvm checkout (or its llvm/lib/Target/PowerPC) for record names; default ../xenon-llvm")
    ap.add_argument("--isa", help="ISA table (default isa/xenon_isa.json)")
    ap.add_argument("--selftest", action="store_true", help="run the built-in test on synthetic data and exit")
    ap.add_argument("-q", "--quiet", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.inputs:
        ap.error("no inputs")
    try:
        code, _ = run(a.inputs, a.out, a.allow_host, a.console, a.statuses.split(",") if a.statuses else None, a.include_noisy,
                      a.td, a.llvm, a.isa, quiet=a.quiet, repeat_tol=a.repeat_tol)
    except OSError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return code


if __name__ == "__main__":
    sys.exit(main())
