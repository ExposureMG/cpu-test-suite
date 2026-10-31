/*
 * qrunner.c - big-endian check runner for the benchmark kernels.
 *
 * The same kernel sources, compiled by both Xenon toolchains with 32-bit-only
 * flags (xenon3_* and old16_* namespaces), are linked into one qemu-ppc
 * executable.  For every kernel index this runner:
 *
 *   1. checks that both tables hold the same id and identical descriptors,
 *      and that the descriptor is valid (the ktest.c rules);
 *   2. poisons the kernel's arena_bytes with 0xA5 and writes a 4 KB guard
 *      pattern right after them;
 *   3. runs xenon3's init once, then run() 3 times per variant in the real
 *      harness' alternating order (AB, BA, AB);
 *   4. checks that each variant returns bit-identical values every time
 *      (UNSTABLE-A / UNSTABLE-B), that A and B agree (EXACT: bit-exact,
 *      APPROX: |a-b| <= tolerance * max(|a|,|b|), else MISMATCH), that EXACT
 *      results are uint32 values and APPROX results finite (RESULT), and that
 *      the guard is intact (OVERRUN);
 *   5. repeats init + one run per variant on an arena poisoned with 0x5A: a
 *      different result means init/run read bytes init never wrote (UNINIT);
 *   6. measures the deepest stack use of the run() calls (stack painted
 *      below the runner's frame) and warns above DESIGN.md's 16 KB.
 *
 * One line per kernel, then a summary.  Exit status: 0 all ok, 1 a kernel
 * failed, 2 usage error, 3 crash (printed by the runtime's signal handler).
 *
 * Host build (-DQR_SINGLE_VARIANT, qrt_host.c): the same runner over
 * host-compiled kernels (little-endian, scalar fallbacks of the AltiVec
 * kernels); "-c <qemu output>" then compares the qemu results with the host
 * results (the BE/LE cross-check, informational).
 */
#include "bench.h"
#include "qrt.h"

extern const bench_variant_info xenon3_info;
#ifdef QR_SINGLE_VARIANT
#define QR_INFO_A (&xenon3_info)
#define QR_INFO_B (&xenon3_info)
#else
extern const bench_variant_info old16_info;
#define QR_INFO_A (&xenon3_info)
#define QR_INFO_B (&old16_info)
#endif

#define QR_GUARD_BYTES 4096u
#define QR_STACK_LIMIT (16u * 1024u) /* DESIGN.md rule 9: run() stack use */
#define QR_MAX_RUNS 8u
#define QR_MAX_SEL 64
#define QR_MAX_KERNELS 128u

enum {
	QF_NAME = 1u << 0,       /* A and B tables disagree on the id at this index */
	QF_DESC = 1u << 1,       /* invalid descriptor, or A/B descriptors differ */
	QF_UNSTABLE_A = 1u << 2, /* A's run() is not idempotent */
	QF_UNSTABLE_B = 1u << 3,
	QF_MISMATCH = 1u << 4,   /* A and B disagree */
	QF_RESULT = 1u << 5,     /* EXACT not a uint32 / APPROX not finite */
	QF_OVERRUN = 1u << 6,    /* the guard after arena_bytes was written */
	QF_UNINIT = 1u << 7,     /* result depends on bytes init did not write */
	QF_SKIPPED = 1u << 8,    /* not run (descriptor unusable) */
};

static const char *const flag_names[] = {
	"NAME", "DESC", "UNSTABLE-A", "UNSTABLE-B", "MISMATCH",
	"RESULT", "OVERRUN", "UNINIT", "SKIPPED",
};

static const char *const cat_names[BENCH_CAT_COUNT] = {
	"INT", "BR", "MEM", "FP", "VEC", "MIX", "LIB",
};

typedef uint32_t __attribute__((may_alias)) qr_w32;

struct opts {
	int verbose;
	int list;
	int machine;
	int single_pass;
	uint32_t runs;
	int nsel;
	const char *sel[QR_MAX_SEL];
	const char *compare;
};

struct kres {
	double a[QR_MAX_RUNS], b[QR_MAX_RUNS];
	double a2, b2;
	double rel;
	uint32_t flags;
	uint32_t stack;           /* deepest stack use of run(), bytes (0: unknown) */
	const char *why;
};

/* ---- small helpers ---------------------------------------------------- */

static double qr_fabs(double x)
{
	return x < 0 ? -x : x;
}

static int is_finite(double d)
{
	return ((uint32_t)(qrt_dbits(d) >> 52) & 0x7ffu) != 0x7ffu;
}

static int is_u32_value(double d)
{
	if (qrt_dbits(d) >> 63)
		return 0;
	if (!(d >= 0.0 && d <= 4294967295.0))
		return 0;
	return d == (double)(uint32_t)d;
}

static int same_str(const char *a, const char *b)
{
	if (!a || !b)
		return a == b;
	return qrt_streq(a, b);
}

static uint32_t count_kernels(const bench_variant_info *v)
{
	uint32_t n = 0;

	while (v->kernels[n] && n < QR_MAX_KERNELS)
		n++;
	return n;
}

static unsigned char guard_byte(uint32_t i)
{
	return (unsigned char)(0xC3u ^ (i * 37u) ^ (i >> 7));
}

static void fill_guard(unsigned char *g)
{
	uint32_t i;

	for (i = 0; i < QR_GUARD_BYTES; i++)
		g[i] = guard_byte(i);
}

static int guard_ok(const unsigned char *g)
{
	uint32_t i;

	for (i = 0; i < QR_GUARD_BYTES; i++)
		if (g[i] != guard_byte(i))
			return 0;
	return 1;
}

static void poison(unsigned char *p, uint32_t n, unsigned char v)
{
	uint32_t w = v * 0x01010101u, i = 0;

	for (; i + 4u <= n; i += 4u)
		*(qr_w32 *)(void *)(p + i) = w;
	for (; i < n; i++)
		p[i] = v;
}

static int big_endian(void)
{
	union {
		uint32_t u;
		unsigned char b[4];
	} c;

	c.u = 0x01020304u;
	return c.b[0] == 1;
}

static void put_flags(uint32_t f)
{
	unsigned i, first = 1;

	for (i = 0; i < sizeof flag_names / sizeof flag_names[0]; i++)
		if (f & (1u << i)) {
			if (!first)
				qrt_putc(' ');
			qrt_puts(flag_names[i]);
			first = 0;
		}
}

static void put_value(const bench_kernel *k, double v)
{
	if (k->flags == BENCH_F_EXACT && is_u32_value(v)) {
		qrt_puts("0x");
		qrt_put_hex((uint32_t)v, 8);
	} else {
		qrt_put_double(v, 12);
		qrt_puts(" (");
		qrt_put_hex64(qrt_dbits(v));
		qrt_putc(')');
	}
}

/* ---- descriptor checks ------------------------------------------------ */

static const char *desc_problem(const bench_kernel *k)
{
	const char *p;

	if (!k->name || !*k->name || qrt_strlen(k->name) > 8)
		return "name";
	for (p = k->name; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))
			return "name charset";
	if (!k->desc || qrt_strlen(k->desc) > 32)
		return "desc";
	if (!k->unit || qrt_strlen(k->unit) > 4)
		return "unit";
	if (k->category >= BENCH_CAT_COUNT)
		return "category";
	if (k->flags > BENCH_F_APPROX)
		return "flags";
	if (k->flags == BENCH_F_APPROX && !(k->tolerance > 0.0 && k->tolerance < 1.0))
		return "tolerance";
	if (!k->units)
		return "units";
	if (!k->arena_bytes || k->arena_bytes > BENCH_ARENA_BYTES)
		return "arena_bytes";
	if (!k->init || !k->run)
		return "init/run";
	return 0;
}

static const char *desc_differs(const bench_kernel *a, const bench_kernel *b)
{
	if (!same_str(a->desc, b->desc))
		return "desc differs";
	if (!same_str(a->unit, b->unit))
		return "unit differs";
	if (a->category != b->category)
		return "category differs";
	if (a->flags != b->flags)
		return "flags differ";
	if (qrt_dbits(a->tolerance) != qrt_dbits(b->tolerance))
		return "tolerance differs";
	if (a->units != b->units)
		return "units differ";
	if (a->arena_bytes != b->arena_bytes)
		return "arena_bytes differ";
	return 0;
}

/* ---- one kernel ---------------------------------------------------------- */

static void run_kernel(const bench_kernel *ka, const bench_kernel *kb,
		       const struct opts *o, struct kres *r)
{
	unsigned char *arena = qrt_arena();
	unsigned char *guard;
	uint64_t a0, b0;
	uint32_t i;

	r->flags = 0;
	r->why = 0;
	r->rel = 0.0;
	r->stack = 0;

	if (!same_str(ka->name, kb->name)) {
		r->flags |= QF_NAME | QF_SKIPPED;
		r->why = "ids differ";
		return;
	}
	r->why = desc_problem(ka);
	if (!r->why)
		r->why = desc_problem(kb);
	if (!r->why)
		r->why = desc_differs(ka, kb);
	if (r->why)
		r->flags |= QF_DESC;
	if (!ka->init || !ka->run || !kb->run || !ka->arena_bytes ||
	    ka->arena_bytes > BENCH_ARENA_BYTES || ka->arena_bytes != kb->arena_bytes) {
		r->flags |= QF_SKIPPED;
		return;
	}

	guard = arena + ka->arena_bytes;
	poison(arena, ka->arena_bytes, 0xA5);
	fill_guard(guard);

	qrt_set_context(ka->name, "xenon3 init");
	qrt_flush();
	ka->init(arena);
	qrt_stack_paint();
	for (i = 0; i < o->runs; i++) {
		if (i & 1u) { /* BA */
			qrt_set_context(ka->name, "old16 run");
			r->b[i] = kb->run(arena);
			qrt_set_context(ka->name, "xenon3 run");
			r->a[i] = ka->run(arena);
		} else {      /* AB */
			qrt_set_context(ka->name, "xenon3 run");
			r->a[i] = ka->run(arena);
			qrt_set_context(ka->name, "old16 run");
			r->b[i] = kb->run(arena);
		}
	}
	r->stack = qrt_stack_used();
	if (!guard_ok(guard))
		r->flags |= QF_OVERRUN;

	a0 = qrt_dbits(r->a[0]);
	b0 = qrt_dbits(r->b[0]);
	for (i = 1; i < o->runs; i++) {
		if (qrt_dbits(r->a[i]) != a0)
			r->flags |= QF_UNSTABLE_A;
		if (qrt_dbits(r->b[i]) != b0)
			r->flags |= QF_UNSTABLE_B;
	}

	if (ka->flags == BENCH_F_EXACT) {
		if (!is_u32_value(r->a[0]) || !is_u32_value(r->b[0]))
			r->flags |= QF_RESULT;
		if (a0 != b0)
			r->flags |= QF_MISMATCH;
	} else {
		if (!is_finite(r->a[0]) || !is_finite(r->b[0])) {
			r->flags |= QF_RESULT;
			if (a0 != b0)
				r->flags |= QF_MISMATCH;
		} else if (a0 != b0) {
			double d = qr_fabs(r->a[0] - r->b[0]);
			double m = qr_fabs(r->a[0]) > qr_fabs(r->b[0]) ? qr_fabs(r->a[0]) : qr_fabs(r->b[0]);

			r->rel = d / m;
			if (!(d <= ka->tolerance * m))
				r->flags |= QF_MISMATCH;
		}
	}

	if (!o->single_pass) {
		poison(arena, ka->arena_bytes, 0x5A);
		fill_guard(guard);
		qrt_set_context(ka->name, "xenon3 init (pass 2)");
		qrt_flush();
		ka->init(arena);
		qrt_set_context(ka->name, "xenon3 run (pass 2)");
		r->a2 = ka->run(arena);
		qrt_set_context(ka->name, "old16 run (pass 2)");
		r->b2 = kb->run(arena);
		if (!guard_ok(guard))
			r->flags |= QF_OVERRUN;
		if (!(r->flags & (QF_UNSTABLE_A | QF_UNSTABLE_B)) &&
		    (qrt_dbits(r->a2) != a0 || qrt_dbits(r->b2) != b0))
			r->flags |= QF_UNINIT;
	}
	qrt_set_context(ka->name, "checks");
}

/* ---- options ---------------------------------------------------------------- */

static int usage(void)
{
	qrt_puts("usage: bench-qemu.elf [-v] [-m] [-l] [-s] [-r N] [-k id]...\n"
		 "  -k id   run only this kernel (repeatable; also --only id)\n"
		 "  -v      verbose: descriptors and the bits of every result\n"
		 "  -m      machine-readable result lines (@R ...)\n"
		 "  -l      list the kernel ids and exit\n"
		 "  -s      skip the second (0x5A-poisoned) init-determinism pass\n"
		 "  -r N    run() calls per variant, 1-8 (default 3)\n"
#ifdef QR_SINGLE_VARIANT
		 "  -c file compare with a qemu run's -m output (BE/LE cross-check)\n"
#endif
		 );
	qrt_flush();
	return 2;
}

static int parse_args(int argc, char **argv, struct opts *o)
{
	int i;

	o->verbose = o->list = o->machine = o->single_pass = 0;
	o->runs = 3;
	o->nsel = 0;
	o->compare = 0;
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (qrt_streq(a, "-v") || qrt_streq(a, "--verbose")) {
			o->verbose = 1;
		} else if (qrt_streq(a, "-m")) {
			o->machine = 1;
		} else if (qrt_streq(a, "-l") || qrt_streq(a, "--list")) {
			o->list = 1;
		} else if (qrt_streq(a, "-s")) {
			o->single_pass = 1;
		} else if ((qrt_streq(a, "-k") || qrt_streq(a, "--only")) && i + 1 < argc) {
			if (o->nsel == QR_MAX_SEL)
				return 0;
			o->sel[o->nsel++] = argv[++i];
		} else if (qrt_streq(a, "-r") && i + 1 < argc) {
			if (!qrt_parse_u32(argv[++i], &o->runs) || o->runs < 1 || o->runs > QR_MAX_RUNS)
				return 0;
#ifdef QR_SINGLE_VARIANT
		} else if (qrt_streq(a, "-c") && i + 1 < argc) {
			o->compare = argv[++i];
#endif
		} else {
			return 0;
		}
	}
	return 1;
}

static int selected(const struct opts *o, const char *name)
{
	int i;

	if (!o->nsel)
		return 1;
	for (i = 0; i < o->nsel; i++)
		if (same_str(o->sel[i], name))
			return 1;
	return 0;
}

/* ---- BE/LE cross-check (host build) --------------------------------------- */

#ifdef QR_SINGLE_VARIANT

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Parses one whitespace-separated token; returns the position after it. */
static const char *token(const char *p, char *out, uint32_t max)
{
	uint32_t n = 0;

	while (*p == ' ')
		p++;
	while (*p && *p != ' ' && *p != '\n') {
		if (n + 1 < max)
			out[n++] = *p;
		p++;
	}
	out[n] = 0;
	return p;
}

static int parse_hex64(const char *s, uint64_t *v)
{
	uint64_t x = 0;
	int n = 0;

	for (; *s; s++, n++) {
		int h = hexval(*s);

		if (h < 0 || n >= 16)
			return 0;
		x = (x << 4) | (uint64_t)h;
	}
	*v = x;
	return n > 0;
}

struct xrow {
	char id[16];
	uint64_t a, b;
	int found;
};

static int cross_check(const char *path, const bench_kernel *const *ks, const struct kres *res,
		       const int *ran, uint32_t n)
{
	static struct xrow rows[QR_MAX_KERNELS];
	char *text = qrt_read_file(path), *line;
	uint32_t nrows = 0, i, same = 0, close = 0, diff = 0, missing = 0;

	if (!text) {
		qrt_puts("BE/LE cross-check: cannot read ");
		qrt_puts(path);
		qrt_puts("\n");
		return 1;
	}
	for (line = text; *line && nrows < QR_MAX_KERNELS;) {
		char *next = line;

		while (*next && *next != '\n')
			next++;
		if (line[0] == '@' && line[1] == 'R' && line[2] == ' ') {
			char tok[32];
			const char *p = line + 3;
			struct xrow *r = &rows[nrows];

			p = token(p, r->id, sizeof r->id);
			p = token(p, tok, sizeof tok);      /* EXACT/APPROX */
			p = token(p, tok, sizeof tok);      /* tolerance bits */
			p = token(p, tok, sizeof tok);
			if (parse_hex64(tok, &r->a)) {
				p = token(p, tok, sizeof tok);
				if (parse_hex64(tok, &r->b))
					nrows++;
			}
		}
		line = *next ? next + 1 : next;
	}

	qrt_puts("\nBE/LE cross-check: qemu-ppc results (big-endian, AltiVec paths, qemu stand-ins "
		 "for BLIB) vs host results (little-endian, scalar fallbacks, host libc/libm)\n");
	for (i = 0; i < n; i++) {
		const bench_kernel *k = ks[i];
		const struct xrow *r = 0;
		uint32_t j;
		double h, q;

		if (!ran[i])
			continue;
		for (j = 0; j < nrows; j++)
			if (same_str(rows[j].id, k->name)) {
				r = &rows[j];
				break;
			}
		qrt_puts("  ");
		qrt_put_padded(k->name, 9);
		qrt_put_padded(k->flags == BENCH_F_EXACT ? "EXACT" : "APPROX", 7);
		if (!r) {
			qrt_puts("not in the qemu output\n");
			missing++;
			continue;
		}
		h = res[i].a[0];
		q = qrt_bitsd(r->a);
		if (r->a != r->b) {
			qrt_puts("(qemu A != B) ");
		}
		if (qrt_dbits(h) == r->a) {
			qrt_puts("same\n");
			same++;
			continue;
		}
		if (k->flags == BENCH_F_APPROX && is_finite(h) && is_finite(q)) {
			double d = qr_fabs(h - q), m = qr_fabs(h) > qr_fabs(q) ? qr_fabs(h) : qr_fabs(q);

			if (d <= k->tolerance * m) {
				qrt_puts("close (rel ");
				qrt_put_double(d / m, 2);
				qrt_puts(")\n");
				close++;
				continue;
			}
			qrt_puts("DIFFERENT rel ");
			qrt_put_double(d / m, 2);
			qrt_puts("  ");
		} else {
			qrt_puts("DIFFERENT  ");
		}
		qrt_puts("qemu ");
		put_value(k, q);
		qrt_puts("  host ");
		put_value(k, h);
		qrt_puts("\n");
		diff++;
	}
	qrt_puts("BE/LE summary: ");
	qrt_put_u32(same);
	qrt_puts(" same, ");
	qrt_put_u32(close);
	qrt_puts(" within tolerance, ");
	qrt_put_u32(diff);
	qrt_puts(" different");
	if (missing) {
		qrt_puts(", ");
		qrt_put_u32(missing);
		qrt_puts(" missing");
	}
	qrt_puts(" (informational: endian-dependent results are legitimate where DESIGN.md says "
		 "data is native)\n");
	return 0;
}

#endif /* QR_SINGLE_VARIANT */

/* ---- main ---------------------------------------------------------------- */

static void put_variant(const char *tag, const bench_variant_info *v, uint32_t n)
{
	qrt_puts("  ");
	qrt_puts(tag);
	qrt_puts(": ");
	qrt_put_padded(v->id ? v->id : "?", 7);
	qrt_put_padded(v->label ? v->label : "?", 11);
	qrt_puts(v->compiler ? v->compiler : "?");
	qrt_puts(", cpu ");
	qrt_puts(v->cpu ? v->cpu : "?");
	qrt_puts(", cflags \"");
	qrt_puts(v->cflags ? v->cflags : "?");
	qrt_puts("\", ");
	qrt_put_u32(n);
	qrt_puts(" kernels\n");
}

int qrt_main(int argc, char **argv);

int qrt_main(int argc, char **argv)
{
	static struct kres res[QR_MAX_KERNELS];
	static int ran[QR_MAX_KERNELS];
	const bench_variant_info *va = QR_INFO_A, *vb = QR_INFO_B;
	const bench_kernel *const *ka = va->kernels, *const *kb = vb->kernels;
	struct opts o;
	uint32_t na, nb, n, i, nrun = 0, nfail = 0, nident = 0, nclose = 0, table_bad = 0;
	uint32_t fcount[9] = { 0 };
	uint32_t max_stack = 0;
	const char *max_stack_id = "-";
	int s;

	qrt_install_crash_handlers();
	if (!parse_args(argc, argv, &o))
		return usage();

	na = count_kernels(va);
	nb = count_kernels(vb);
	n = na < nb ? na : nb;

	if (o.list) {
		for (i = 0; i < na; i++) {
			qrt_puts(ka[i]->name ? ka[i]->name : "?");
			qrt_putc('\n');
		}
		qrt_flush();
		return 0;
	}
	for (s = 0; s < o.nsel; s++) {
		for (i = 0; i < n; i++)
			if (same_str(o.sel[s], ka[i]->name))
				break;
		if (i == n) {
			qrt_puts("unknown kernel id: ");
			qrt_puts(o.sel[s]);
			qrt_puts(" (-l lists them)\n");
			qrt_flush();
			return 2;
		}
	}

	qrt_puts("qemu-ppc big-endian check runner, bench " BENCH_VERSION "\n");
	put_variant("A", va, na);
	put_variant("B", vb, nb);
	qrt_puts("  byte order ");
	qrt_puts(big_endian() ? "big-endian" : "little-endian");
	qrt_puts(", VSCR 0x");
	qrt_put_hex(qrt_vscr(), 8);
	qrt_puts(", arena 0x");
	qrt_put_hex((uint32_t)(uintptr_t)qrt_arena(), 8);
	qrt_puts(" (");
	qrt_put_u32(qrt_arena_size() >> 10);
	qrt_puts(" KB), guard ");
	qrt_put_u32(QR_GUARD_BYTES);
	qrt_puts(" B, ");
	qrt_put_u32(o.runs);
	qrt_puts(" runs per variant (AB BA AB ...), ");
	qrt_puts(o.single_pass ? "single pass\n" : "init-determinism pass on (poison A5 then 5A)\n");

	if (na != nb) {
		qrt_puts("TABLE: A has ");
		qrt_put_u32(na);
		qrt_puts(" kernels, B has ");
		qrt_put_u32(nb);
		qrt_puts("\n");
		table_bad = 1;
	}
	for (i = 0; i < n; i++)
		if (!same_str(ka[i]->name, kb[i]->name)) {
			qrt_puts("TABLE: index ");
			qrt_put_u32(i);
			qrt_puts(": A '");
			qrt_puts(ka[i]->name ? ka[i]->name : "(null)");
			qrt_puts("' vs B '");
			qrt_puts(kb[i]->name ? kb[i]->name : "(null)");
			qrt_puts("'\n");
			table_bad = 1;
		}

	for (i = 0; i < n; i++) {
		const bench_kernel *a = ka[i], *b = kb[i];
		struct kres *r = &res[i];
		uint32_t f;

		if (!selected(&o, a->name))
			continue;
		ran[i] = 1;
		nrun++;
		qrt_putc('[');
		if (i + 1 < 10)
			qrt_putc(' ');
		qrt_put_u32(i + 1);
		qrt_putc('/');
		qrt_put_u32(n);
		qrt_puts("] ");
		qrt_put_padded(a->name ? a->name : "?", 9);
		qrt_put_padded(a->category < BENCH_CAT_COUNT ? cat_names[a->category] : "?", 4);
		qrt_put_padded(a->flags == BENCH_F_EXACT ? "EXACT" : "APPROX", 7);
		qrt_flush();

		run_kernel(a, b, &o, r);
		f = r->flags;

		if (!(f & QF_SKIPPED)) {
			qrt_puts("A ");
			put_value(a, r->a[0]);
			qrt_puts("  B ");
			put_value(a, r->b[0]);
			if (a->flags == BENCH_F_APPROX) {
				if (qrt_dbits(r->a[0]) == qrt_dbits(r->b[0])) {
					qrt_puts("  identical");
				} else {
					qrt_puts("  rel ");
					qrt_put_double(r->rel, 2);
					qrt_puts(" (tol ");
					qrt_put_double(a->tolerance, 1);
					qrt_puts(")");
				}
			}
		}
		if (r->stack > QR_STACK_LIMIT) {
			qrt_puts("  WARN stack ");
			qrt_put_u32(r->stack);
			qrt_puts(" B > 16 KB");
		}
		if (f) {
			qrt_puts("  FAIL ");
			put_flags(f);
			if (r->why) {
				qrt_puts(" (");
				qrt_puts(r->why);
				qrt_puts(")");
			}
			nfail++;
		} else {
			qrt_puts("  ok");
		}
		qrt_putc('\n');
		if (r->stack > max_stack) {
			max_stack = r->stack;
			max_stack_id = a->name;
		}
		if (!(f & QF_SKIPPED) && qrt_dbits(r->a[0]) == qrt_dbits(r->b[0]))
			nident++;
		else if (!(f & QF_SKIPPED) && !(f & QF_MISMATCH))
			nclose++;
		{
			uint32_t bit;

			for (bit = 0; bit < 9; bit++)
				if (f & (1u << bit))
					fcount[bit]++;
		}

		if (o.verbose) {
			uint32_t j;

			qrt_puts("        desc \"");
			qrt_puts(a->desc ? a->desc : "?");
			qrt_puts("\" unit ");
			qrt_puts(a->unit ? a->unit : "?");
			qrt_puts(" units ");
			qrt_put_u32(a->units);
			qrt_puts(" arena_bytes ");
			qrt_put_u32(a->arena_bytes);
			qrt_puts(" tolerance ");
			qrt_put_double(a->tolerance, 3);
			if (r->stack) {
				qrt_puts(" run() stack ");
				qrt_put_u32(r->stack);
				qrt_puts(" B");
			}
			qrt_putc('\n');
			if (!(f & QF_SKIPPED)) {
				for (j = 0; j < o.runs; j++) {
					qrt_puts("        run ");
					qrt_put_u32(j + 1);
					qrt_puts(": A ");
					qrt_put_dbits(r->a[j]);
					qrt_puts("  B ");
					qrt_put_dbits(r->b[j]);
					qrt_putc('\n');
				}
				if (!o.single_pass) {
					qrt_puts("        pass 2 (poison 5A): A ");
					qrt_put_dbits(r->a2);
					qrt_puts("  B ");
					qrt_put_dbits(r->b2);
					qrt_putc('\n');
				}
			}
		}
		if (o.machine && !(f & QF_SKIPPED)) {
			qrt_puts("@R ");
			qrt_puts(a->name);
			qrt_puts(a->flags == BENCH_F_EXACT ? " EXACT " : " APPROX ");
			qrt_put_hex64(qrt_dbits(a->tolerance));
			qrt_putc(' ');
			qrt_put_hex64(qrt_dbits(r->a[0]));
			qrt_putc(' ');
			qrt_put_hex64(qrt_dbits(r->b[0]));
			qrt_putc(' ');
			qrt_put_hex(f, 4);
			qrt_putc('\n');
		}
		qrt_flush();
	}
	qrt_set_context("-", "summary");

	qrt_puts("summary: ");
	qrt_put_u32(nrun);
	qrt_puts(" kernels run, ");
	qrt_put_u32(nrun - nfail);
	qrt_puts(" ok, ");
	qrt_put_u32(nfail);
	qrt_puts(nfail ? " FAILED" : " failed");
	if (nfail) {
		uint32_t bit;

		qrt_puts(" (");
		for (bit = 0, s = 0; bit < 9; bit++)
			if (fcount[bit]) {
				if (s++)
					qrt_puts(", ");
				qrt_puts(flag_names[bit]);
				qrt_putc(' ');
				qrt_put_u32(fcount[bit]);
			}
		qrt_puts(")");
	}
	qrt_puts("; A/B bit-identical ");
	qrt_put_u32(nident);
	qrt_puts(", within tolerance ");
	qrt_put_u32(nclose);
	if (table_bad)
		qrt_puts("; TABLES DIFFER");
	if (max_stack) {
		qrt_puts("; deepest run() stack ");
		qrt_put_u32(max_stack);
		qrt_puts(" B (");
		qrt_puts(max_stack_id);
		qrt_puts(max_stack > QR_STACK_LIMIT ? ", OVER 16 KB)" : ")");
	}
	qrt_putc('\n');
	if (nfail) {
		qrt_puts("failed:");
		for (i = 0; i < n; i++)
			if (ran[i] && res[i].flags) {
				qrt_putc(' ');
				qrt_puts(ka[i]->name);
			}
		qrt_putc('\n');
	}
	qrt_flush();

#ifdef QR_SINGLE_VARIANT
	if (o.compare) {
		cross_check(o.compare, ka, res, ran, n);
		qrt_flush();
		return 0;
	}
#endif
	return (nfail || table_bad) ? 1 : 0;
}
