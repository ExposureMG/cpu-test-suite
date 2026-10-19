#!/usr/bin/env python3
"""check_draws.py - find RNG draws whose order C leaves unspecified.

Case inputs come from the per-case RNG (xts_rng_t), so a case is reproducible only if every build draws in the same order.
C does not fix the order in which the operands of one expression are evaluated: function arguments, the two sides of a
binary operator, and the members of an initialiser list. The compilers do differ: xenon-gcc (PowerPC) evaluates call
arguments left to right, host gcc (x86-64) right to left, so `f(draw(r), draw(r))` makes the host dry run generate other
inputs than the console for the same case id. Every such expression must take its draws into named temporaries first.

The check parses the C sources with libclang (host flags), finds the functions that consume the RNG (xts_rng_next or
xts_rng_seed, directly or through other functions; calls through function pointers count when an argument carries an
xts_rng_t or xts_ctx_t), and reports every call, binary operator or initialiser list with two or more operands that
consume it. Exit status 1 when it finds one.

  tools/check_draws.py --gen build/gen src/*/*.c
  tools/check_draws.py --selftest
"""
import argparse
import os
import subprocess
import sys

try:
    import clang.cindex as ci
except ImportError:
    sys.stderr.write("check_draws: needs the libclang Python bindings (python module 'clang', e.g. the clang package)\n")
    sys.exit(2)

K = ci.CursorKind
BASE = {"xts_rng_next", "xts_rng_seed"}      # the functions that change an xts_rng_t
RNG_TYPES = ("xts_rng", "xts_ctx")          # an argument of these types (or pointers to them) can reach the RNG
SEQUENCED_BINOPS = {"&&", "||", ","}
OPS = {"Add": "+", "Sub": "-", "Mul": "*", "Div": "/", "Rem": "%", "Shl": "<<", "Shr": ">>", "And": "&", "Or": "|",
       "Xor": "^", "LAnd": "&&", "LOr": "||", "Comma": ",", "Assign": "=", "LT": "<", "GT": ">", "LE": "<=", "GE": ">=",
       "EQ": "==", "NE": "!="}


def binop_spelling(cur):
    """The operator of a BINARY_OPERATOR / COMPOUND_ASSIGNMENT_OPERATOR cursor (libclang >= 17 has binary_operator)."""
    try:
        op = cur.binary_operator
        if op is not None and op.name not in ("Invalid", "INVALID"):
            return OPS.get(op.name, op.name)
    except (AttributeError, ValueError):
        pass
    toks = [t.spelling for t in cur.get_tokens()]
    lhs = next(cur.get_children(), None)
    n = len(list(lhs.get_tokens())) if lhs is not None else 0
    return toks[n] if n < len(toks) else "?"


def in_project(cur, roots):
    f = cur.location.file
    return f is not None and os.path.realpath(f.name).startswith(roots)


def callee_name(call):
    ref = call.referenced
    if ref is not None and ref.kind == K.FUNCTION_DECL:
        return ref.spelling
    return None


def carries_rng(expr):
    s = expr.type.spelling
    return any(n in s for n in RNG_TYPES)


def parse_all(files, args):
    idx = ci.Index.create()
    tus = []
    for f in files:
        tu = idx.parse(f, args=args)
        errs = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
        if errs:
            for d in errs[:5]:
                sys.stderr.write(f"check_draws: {d}\n")
            sys.exit(2)
        tus.append(tu)
    return tus


def project_functions(tu, roots):
    """the function definitions of this TU that come from the project's own files (C has no nested functions)"""
    return [c for c in tu.cursor.get_children()
            if c.kind == K.FUNCTION_DECL and c.is_definition() and in_project(c, roots)]


def function_calls(fns):
    """name -> set of called names (direct calls), plus names that make an RNG-carrying indirect call."""
    calls, indirect = {}, set()
    for fn in fns:
        s = calls.setdefault(fn.spelling, set())
        for c in fn.walk_preorder():
            if c.kind != K.CALL_EXPR:
                continue
            n = callee_name(c)
            if n:
                s.add(n)
            elif any(carries_rng(a) for a in c.get_arguments()):
                indirect.add(fn.spelling)
    return calls, indirect


def consumers(calls, indirect):
    drawing = set(BASE) | indirect
    changed = True
    while changed:
        changed = False
        for fn, callees in calls.items():
            if fn not in drawing and callees & drawing:
                drawing.add(fn)
                changed = True
    return drawing


def unsequenced(cur):
    """are the evaluations of cur's operands unsequenced (or indeterminately sequenced) against each other?"""
    if cur.kind in (K.CALL_EXPR, K.INIT_LIST_EXPR, K.ARRAY_SUBSCRIPT_EXPR):   # callee + arguments / members / base + index
        return True
    if cur.kind in (K.BINARY_OPERATOR, K.COMPOUND_ASSIGNMENT_OPERATOR):
        return binop_spelling(cur) not in SEQUENCED_BINOPS
    return False


def scan(fn, drawing, found, root):
    """one bottom-up pass over a function body: record every node with two or more operands that draw"""
    def visit(cur):
        res = [visit(k) for k in cur.get_children()]
        if sum(res) >= 2 and unsequenced(cur):
            loc = cur.location
            found[(os.path.relpath(os.path.realpath(loc.file.name), root), loc.line, loc.column)] = describe(cur)
        d = any(res)
        if not d and cur.kind == K.CALL_EXPR:
            n = callee_name(cur)
            d = n in drawing if n else any(carries_rng(a) for a in cur.get_arguments())
        return d
    visit(fn)


def describe(cur):
    if cur.kind == K.CALL_EXPR:
        return f"arguments of {cur.spelling or 'an indirect call'}()"
    if cur.kind == K.INIT_LIST_EXPR:
        return "initialiser list"
    if cur.kind == K.ARRAY_SUBSCRIPT_EXPR:
        return "array subscript"
    return f"operands of '{binop_spelling(cur)}'"


def clang_args(incs):
    args = ["-std=gnu11", "-DXTS_HOST"] + ["-I" + i for i in incs]
    try:      # libclang does not find its own builtin headers (stddef.h, ...) by itself
        res = subprocess.run(["clang", "-print-resource-dir"], capture_output=True, text=True).stdout.strip()
        if res:
            args += ["-isystem", os.path.join(res, "include")]
    except OSError:
        pass
    return args


def check(files, args, root, subdirs):
    """-> ({(file, line, col): what}, number of drawing functions) for the functions defined under root/<subdirs>"""
    roots = tuple(os.path.join(root, d) + os.sep for d in subdirs)
    fns = [fn for tu in parse_all(files, args) for fn in project_functions(tu, roots)]
    calls, indirect = function_calls(fns)
    drawing = consumers(calls, indirect)
    found = {}
    for fn in fns:
        scan(fn, drawing, found, root)
    return found, len(drawing - BASE)


SELFTEST = r"""
#include <stdint.h>
typedef struct xts_rng { uint64_t s; } xts_rng_t;
typedef struct xts_ctx { int x; xts_rng_t rng; } xts_ctx_t;
static inline uint64_t xts_rng_next(xts_rng_t *r) { return r->s = r->s * 6364136223846793005ull + 1; }
static inline uint32_t below(xts_rng_t *r, uint32_t n) { return n ? (uint32_t)(xts_rng_next(r) >> 32) % n : 0; }
static uint32_t bit(xts_rng_t *r) { return below(r, 2); }             /* draws through two levels */
static uint32_t pure(uint32_t a, uint32_t b) { return a + b; }
static uint32_t ctxdraw(xts_ctx_t *c) { return bit(&c->rng); }
struct v { uint32_t a, b; };
uint32_t (*fp)(xts_rng_t *);
static const uint32_t tab[4] = { 1, 2, 3, 4 };
void bad(xts_rng_t *r, xts_ctx_t *c, uint32_t *o)
{
  o[0] = pure(bit(r), below(r, 3));                  /* BAD:call */
  o[1] = bit(r) ^ bit(r);                            /* BAD:binop */
  struct v s = { bit(r), 1 + bit(r) };               /* BAD:init */
  o[2] = s.a;
  o[3] = pure(1, (uint32_t)xts_rng_next(r) | tab[below(r, 4)]);   /* BAD:nested */
  o[4] = pure(ctxdraw(c), fp(r));                    /* BAD:ctx-and-pointer */
  o[bit(r)] = bit(r);                                /* BAD:assign */
  o[5] += bit(r) + (bit(r) ? 1 : 0);                 /* BAD:compound */
}
void good(xts_rng_t *r, uint32_t *o)
{
  uint32_t a = bit(r), b = below(r, 3);              /* separate declarators are sequenced */
  o[0] = pure(a, b);
  o[1] = bit(r) && bit(r);
  o[2] = bit(r) ? bit(r) : below(r, 5);
  o[3] = (bit(r), bit(r));
  o[4] = pure(bit(r), 7) + pure(1, 2);
  o[5] |= bit(r);
  o[6] = tab[below(r, 4)];
}
"""


def selftest():
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        root = os.path.realpath(d)
        os.mkdir(os.path.join(root, "src"))
        f = os.path.join(root, "src", "t.c")
        with open(f, "w") as fh:
            fh.write(SELFTEST)
        found, _ = check([f], clang_args([]), root, ("src",))
    want = {i + 1 for i, line in enumerate(SELFTEST.splitlines()) if "/* BAD:" in line}
    got = {line for (_, line, _) in found}
    if got != want:
        print(f"check_draws selftest: FAILED: missed lines {sorted(want - got)}, wrong lines {sorted(got - want)}")
        for k, v in sorted(found.items()):
            print("  ", k, v)
        return 1
    print(f"check_draws selftest: OK ({len(want)} unsequenced draws found, no false positives)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen", default="build/gen", help="generated headers (make's $(GEN))")
    ap.add_argument("-I", dest="incs", action="append", default=[], help="extra include directory")
    ap.add_argument("--selftest", action="store_true", help="check the checker on built-in examples")
    ap.add_argument("files", nargs="*")
    a = ap.parse_args()
    sys.setrecursionlimit(20000)
    if a.selftest:
        return selftest()
    if not a.files:
        ap.error("no files")
    root = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    incs = ["include", "src", a.gen, "third_party/softfloat-3e/source/include", "third_party/softfloat-3e/platform/host"]
    files = [f for f in a.files if not f.endswith("_console.c")]
    found, nfun = check(files, clang_args(incs + a.incs), root, ("src", "include", "profiles", "host"))
    for (f, line, col), what in sorted(found.items()):
        print(f"{f}:{line}:{col}: RNG draws in unspecified order: {what}")
    if found:
        print(f"check_draws: {len(found)} expression(s) draw from the RNG more than once in an unspecified order; "
              "take each draw into a temporary first (console order: left to right)", file=sys.stderr)
        return 1
    print(f"check_draws: {len(files)} files, {nfun} drawing functions, every draw sequenced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
