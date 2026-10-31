/*
 * report.c - progress lines, the final screen and the CSV log.
 *
 * The final screen is a grid of cols x rows cells (plat_screen_size).  The
 * last column is never written and no text contains a newline, so the
 * libxenon console can neither wrap nor scroll (see platform.h).  Layouts,
 * most informative first; the first one that fits is drawn:
 *
 *   wide     one kernel per line: name, category, description, A and B
 *            us/run, cycles/unit, speedup (+ a bar), noise, check
 *   compact  two kernels per line: name, [category], A us, B us, speedup
 *            (or the failure word)
 *   minimal  anything smaller: overall result, then "name speedup" cells
 *
 * Header: title, one line per variant, environment.  Footer: per-category
 * geomeans, overall geomean, failure count and legend.  Every table cell is
 * formatted to its column width, so text never runs past the screen at the
 * supported sizes (74x27 and larger); the host build verifies this.
 *
 * Layout shifts (r->nshift > 1, make SHIFTS=...): the speedup shown is the
 * median over the shifts and the times are the medians of each shift's
 * minimum; wide adds the spread (max/min - 1 of the per-shift speedups),
 * a '*' for layout-sensitive kernels ('!' when the winner flips) and, when
 * there is room, one column per shift (else a min-max column); compact and
 * minimal add the marker; the footer adds the geomean at each shift.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "report.h"

#define BUF 256          /* longer than any line on any supported screen */
#define MAX_ITEMS 32
#define DESC_MIN 16      /* narrower than this: drop the description      */
#define BAR_MIN 15       /* narrower than this: no speedup bar            */
#define BAR_MAX 41
#define NOISE_WARN 3.0   /* noise above this (percent) is shown in yellow  */
#define SSP_W 5          /* per-shift speedup column: "1.023"              */

typedef struct scr {
	int cols, rows; /* as reported by the platform */
	int w;          /* usable width: never the last column */
} scr;

static const char *layout_name = "none";

const char *report_layout(void)
{
	return layout_name;
}

/* ---- formatting ------------------------------------------------------- */

static int is_fin(double x)
{
	return x == x && x - x == 0.0;
}

/* About 4 significant digits, at most maxdec decimals, at most width chars
 * (0 = any width).  1234.5 -> "1234", 812.34 -> "812.3", 0.0123 -> "0.012" */
static void fmt_sig(char *b, size_t n, double v, int maxdec, int width)
{
	static const char sfx[] = "kMGT";
	double a = fabs(v);
	int dec, len, i;

	if (!is_fin(v)) {
		snprintf(b, n, "%s", v != v ? "nan" : "inf");
		return;
	}
	dec = a >= 999.95 ? 0 : a >= 99.995 ? 1 : a >= 9.9995 ? 2 : a >= 0.99995 ? 3 : 4;
	if (dec > maxdec)
		dec = maxdec;
	for (;;) {
		len = snprintf(b, n, "%.*f", dec, v);
		if (width <= 0 || len <= width || dec == 0)
			break;
		dec--;
	}
	for (i = 0; width > 0 && len > width && i < 4; i++) {
		v /= 1000.0;
		len = snprintf(b, n, "%.0f%c", v, sfx[i]);
	}
	if (width > 0 && len > width)
		snprintf(b, n, "%.*s", width, "################");
}

/* Fixed decimals (a column shares them so the decimal points line up);
 * falls back to fmt_sig when the value does not fit. */
static void fmt_fixed(char *b, size_t n, double v, int dec, int width)
{
	if (is_fin(v) && snprintf(b, n, "%.*f", dec, v) <= width)
		return;
	fmt_sig(b, n, v, 0, width);
}

static void fmt_speedup(char *b, size_t n, double s)
{
	if (!(s > 0.0) || !is_fin(s))
		snprintf(b, n, "-");
	else if (s < 9.9995)
		snprintf(b, n, "%.3fx", s);
	else if (s < 99.995)
		snprintf(b, n, "%.2fx", s);
	else if (s < 999.95)
		snprintf(b, n, "%.1fx", s);
	else
		snprintf(b, n, ">999x");
}

/* A speedup without the 'x', at most 5 chars: "1.023", "12.34". */
static void fmt_ratio(char *b, size_t n, double s)
{
	if (!(s > 0.0) || !is_fin(s))
		snprintf(b, n, "-");
	else if (s < 9.9995)
		snprintf(b, n, "%.3f", s);
	else if (s < 99.995)
		snprintf(b, n, "%.2f", s);
	else if (s < 999.95)
		snprintf(b, n, "%.1f", s);
	else
		snprintf(b, n, ">999");
}

static void fmt_pct(char *b, size_t n, double p)
{
	if (!is_fin(p) || p < 0.0)
		snprintf(b, n, "-");
	else if (p < 99.95)
		snprintf(b, n, "%.1f%%", p);
	else if (p < 999.5)
		snprintf(b, n, "%.0f%%", p);
	else
		snprintf(b, n, ">999%%");
}

static int speed_color(double s)
{
	if (!(s > 0.0))
		return PLAT_DIM;
	if (s > 1.01)
		return PLAT_GOOD;
	if (s < 0.99)
		return PLAT_BAD;
	return PLAT_TEXT;
}

static const char *str_or(const char *s, const char *dflt)
{
	return s ? s : dflt;
}

static int has_timing(const bench_kres *e)
{
	return e->samples > 0 && e->tshow[0] > 0.0;
}

static int shifted(const bench_results *r)
{
	return r->nshift > 1;
}

/* Layout marker of a kernel that passed: '!' the faster toolchain depends
 * on the shift, '*' layout-sensitive, ' ' neither. */
static const char *layout_mark(const bench_results *r, const bench_kres *e)
{
	if (!shifted(r) || r->nvar != 2 || e->status || !has_timing(e))
		return " ";
	return e->flip ? "!" : e->layout ? "*" : " ";
}

/* An EXACT result as hex when it is a uint32, else %.9g. */
static void fmt_result(char *b, size_t n, const bench_kernel *k, double v)
{
	if (k && k->flags == BENCH_F_EXACT && v >= 0.0 && v <= 4294967295.0 &&
	    v == (double)(uint32_t)v)
		snprintf(b, n, "0x%08x", (unsigned)(uint32_t)v);
	else
		snprintf(b, n, "%.9g", v);
}

/* ---- drawing primitives ----------------------------------------------- */

static void put(const scr *s, int col, int row, int color, const char *t)
{
	(void)s;
	if (t && *t)
		plat_screen_put(col, row, color, t);
}

/* Text cut or padded to exactly width chars. */
static void cell(const scr *s, int col, int row, int width, int right,
		 int color, const char *t)
{
	char b[BUF];

	if (width <= 0)
		return;
	if (width >= BUF)
		width = BUF - 1;
	if (right)
		snprintf(b, sizeof(b), "%*.*s", width, width, str_or(t, ""));
	else
		snprintf(b, sizeof(b), "%-*.*s", width, width, str_or(t, ""));
	put(s, col, row, color, b);
}

typedef struct item {
	const char *text;
	int color;
	int glue; /* attach to the previous item: no separator, no wrap */
} item;

/* Lays items out from (col0, row), separated by sep blanks.  An item that
 * does not fit starts a new line at column indent (at most maxlines lines);
 * items that do not fit within maxlines are dropped (a later, shorter one
 * may still fit), an item longer than a line is cut.  Draws only when draw
 * is set.  Returns the number of lines used (0 if nothing was placed). */
static int flow(const scr *s, int row, int col0, int indent, int sep,
		const item *it, int n, int maxlines, int draw)
{
	char b[BUF];
	int i, col = col0, lines = 1, fresh = 1, placed = 0, prev_ok = 0;

	for (i = 0; i < n; i++) {
		int len = it[i].text ? (int)strlen(it[i].text) : 0;
		int start, grp, k;

		if (!len)
			continue;
		if (it[i].glue) {
			if (!prev_ok)
				continue;
			start = col;
			if (start + len > s->w) {
				/* only when the whole group is wider than a line */
				len = s->w - start;
				if (len <= 0)
					continue;
			}
		} else {
			/* a head and its glued tail move together */
			grp = len;
			for (k = i + 1; k < n && it[k].glue; k++)
				grp += it[k].text ? (int)strlen(it[k].text) : 0;
			start = fresh ? col : col + sep;
			if (start + grp > s->w && !fresh) {
				if (lines >= maxlines) {
					prev_ok = 0;
					continue;
				}
				lines++;
				row++;
				col = indent;
				fresh = 1;
				start = col;
			}
			if (start + len > s->w)
				len = s->w - start;
			if (len <= 0) {
				prev_ok = 0;
				continue;
			}
		}
		if (draw && len < BUF) {
			snprintf(b, sizeof(b), "%.*s", len, it[i].text);
			put(s, start, row, it[i].color, b);
		}
		col = start + len;
		fresh = 0;
		placed = 1;
		prev_ok = 1;
	}
	return placed ? lines : 0;
}

/* ---- header and footer ------------------------------------------------ */

typedef struct texts {
	char title[64], vs[96], runinfo[64], runtime[32], cpu[128], tb[48];
	char var[BENCH_MAX_VARIANTS][5][96];
	char geo[BENCH_CAT_COUNT][24], geoall[160], geoof[64], status[96];
	char refnote[64];
	char shifts[48];                        /* "4 layout shifts +0..+12" */
	char slabel[BENCH_MAX_SHIFTS][8];       /* "+12"                     */
	char sgeo[BENCH_MAX_SHIFTS][16];        /* "1.012x"                  */
	char laynote[48], spleg[48];
} texts;

static texts T; /* large: keep it off the stack */

static void build_texts(const bench_results *r)
{
	int j, c;
	double secs = r->tb_hz ? (double)r->total_ticks / (double)r->tb_hz : 0.0;

	memset(&T, 0, sizeof(T));
	snprintf(T.title, sizeof(T.title), "LibXenon CPU benchmark %s", BENCH_VERSION);
	if (r->nvar == 2)
		snprintf(T.vs, sizeof(T.vs), "%s vs %s",
			 str_or(r->var[0]->label, "A"), str_or(r->var[1]->label, "B"));
	else if (r->nvar == 1)
		snprintf(T.vs, sizeof(T.vs), "%s only", str_or(r->var[0]->label, "A"));
	snprintf(T.runinfo, sizeof(T.runinfo), "%d kernel%s, %d x %d ms%s", r->nk,
		 r->nk == 1 ? "" : "s", r->samples, r->sample_ms,
		 r->quick ? " (quick)" : "");
	snprintf(T.runtime, sizeof(T.runtime), "run %.1f s", secs);
	for (j = 0; j < r->nshift; j++) {
		snprintf(T.slabel[j], sizeof(T.slabel[j]), "+%d", r->shift[j]);
		if (r->ngeo_all)
			fmt_speedup(T.sgeo[j], sizeof(T.sgeo[j]), r->geo_shift[j]);
		else
			snprintf(T.sgeo[j], sizeof(T.sgeo[j]), "-");
	}
	if (shifted(r)) {
		snprintf(T.shifts, sizeof(T.shifts), "%d layout shifts %s..%s", r->nshift,
			 T.slabel[0], T.slabel[r->nshift - 1]);
		snprintf(T.laynote, sizeof(T.laynote), "%d layout-sensitive%s", r->nlayout,
			 r->nflip ? "" : " (*)");
		if (r->nflip)
			snprintf(T.laynote + strlen(T.laynote), sizeof(T.laynote) - strlen(T.laynote),
				 " (*), %d flip (!)", r->nflip);
		snprintf(T.spleg, sizeof(T.spleg), "speedup = median of B/A over shifts");
	}
	snprintf(T.cpu, sizeof(T.cpu), "%s %s", plat_name(), plat_cpu_desc());
	if (r->cyc_per_tick > 0.0)
		snprintf(T.tb, sizeof(T.tb), "TB %g MHz = %g cycles/tick",
			 (double)r->tb_hz / 1e6, r->cyc_per_tick);
	else
		snprintf(T.tb, sizeof(T.tb), "timer %g MHz", (double)r->tb_hz / 1e6);
	for (j = 0; j < r->nvar; j++) {
		const bench_variant_info *v = r->var[j];

		snprintf(T.var[j][0], 96, "%s", r->nvar == 2 ? (j ? "B" : "A") : "=");
		snprintf(T.var[j][1], 96, "%-10s", str_or(v->label, "?"));
		snprintf(T.var[j][2], 96, "%s", str_or(v->compiler, "?"));
		snprintf(T.var[j][3], 96, "cpu %s", str_or(v->cpu, "?"));
		snprintf(T.var[j][4], 96, "cflags %s", str_or(v->cflags, "?"));
	}
	for (c = 0; c < BENCH_CAT_COUNT; c++) {
		char sp[16];

		if (r->ngeo[c])
			fmt_speedup(sp, sizeof(sp), r->geo[c]);
		else
			snprintf(sp, sizeof(sp), "-");
		snprintf(T.geo[c], sizeof(T.geo[c]), "%s", sp);
	}
	if (r->nvar == 2 && r->ngeo_all) {
		char sp[16];
		const char *a = str_or(r->var[0]->label, "A");
		const char *b = str_or(r->var[1]->label, "B");
		const char *ks = shifted(r) ? (r->ngeo_all == 1 ? " median" : " medians")
					    : r->ngeo_all == 1 ? "" : "s";

		fmt_speedup(sp, sizeof(sp), r->geo_all);
		if (fabs(r->geo_all - 1.0) < 0.0005) {
			snprintf(T.geoall, sizeof(T.geoall), "%s and %s are equally fast", a, b);
			snprintf(T.geoof, sizeof(T.geoof), "(geomean %s of %d kernel%s)", sp,
				 r->ngeo_all, ks);
		} else if (r->geo_all >= 1.0) {
			snprintf(T.geoall, sizeof(T.geoall), "%s is %s faster than %s", a, sp, b);
			snprintf(T.geoof, sizeof(T.geoof), "(geomean of %d kernel%s)",
				 r->ngeo_all, ks);
		} else {
			snprintf(T.geoof, sizeof(T.geoof), "(geomean speedup %s, %d kernel%s)",
				 sp, r->ngeo_all, ks);
			fmt_speedup(sp, sizeof(sp), 1.0 / r->geo_all);
			snprintf(T.geoall, sizeof(T.geoall), "%s is %s faster than %s", b, sp, a);
		}
	} else if (r->nvar == 2) {
		snprintf(T.geoall, sizeof(T.geoall), "no kernel passed: no overall result");
	}
	{
		int nmiss = 0, nfail, i;
		const char *ex = r->nvar == 2 ? " (red, not in the geomeans)" : " (red)";

		for (i = 0; i < r->nk; i++)
			if (r->k[i].status & BST_MISSING)
				nmiss++;
		nfail = r->nfailed - nmiss;
		if (!r->nk)
			snprintf(T.status, sizeof(T.status), "no kernels were run");
		else if (!r->nfailed)
			snprintf(T.status, sizeof(T.status), "all %d kernel%s passed", r->nk,
				 r->nk == 1 ? "" : "s");
		else if (!nmiss)
			snprintf(T.status, sizeof(T.status), "%d of %d kernels failed%s",
				 nfail, r->nk, ex);
		else if (!nfail)
			snprintf(T.status, sizeof(T.status), "%d of %d kernels missing%s",
				 nmiss, r->nk, ex);
		else
			snprintf(T.status, sizeof(T.status), "%d failed, %d missing of %d%s",
				 nfail, nmiss, r->nk, ex);
	}
	/* what "ok" in the check column vouches for */
	if (r->nvar == 2 && r->nref == r->nk)
		snprintf(T.refnote, sizeof(T.refnote), "ok = A, B and reference agree");
	else if (r->nvar == 2 && r->nref)
		snprintf(T.refnote, sizeof(T.refnote), "ok = A, B agree (+ reference: %d of %d)",
			 r->nref, r->nk);
	else if (r->nvar == 2)
		snprintf(T.refnote, sizeof(T.refnote), "ok = A and B agree");
	else if (r->nref == r->nk)
		snprintf(T.refnote, sizeof(T.refnote), "ok = result matches the reference");
	else if (r->nref)
		snprintf(T.refnote, sizeof(T.refnote), "ok = matches the reference (%d of %d)",
			 r->nref, r->nk);
}

/* Title + environment items (one flow). */
static int title_items(const bench_results *r, item *it)
{
	int n = 0;

	it[n++] = (item){ T.title, PLAT_HEAD, 0 };
	it[n++] = (item){ T.vs, PLAT_BRIGHT, 0 };
	it[n++] = (item){ T.runinfo, PLAT_TEXT, 0 };
	if (shifted(r))
		it[n++] = (item){ T.shifts, PLAT_TEXT, 0 };
	it[n++] = (item){ T.runtime, PLAT_TEXT, 0 };
	it[n++] = (item){ T.cpu, plat_cpu_warning() ? PLAT_WARN : PLAT_DIM, 0 };
	it[n++] = (item){ T.tb, PLAT_DIM, 0 };
	return n;
}

static int variant_items(const bench_results *r, int j, item *it)
{
	int n = 0, color = j ? PLAT_VAR_B : PLAT_VAR_A;

	if (r->nvar == 2)
		it[n++] = (item){ T.var[j][0], color, 0 };
	it[n++] = (item){ T.var[j][1], color, 0 };
	it[n++] = (item){ T.var[j][2], PLAT_TEXT, 0 };
	it[n++] = (item){ T.var[j][3], PLAT_TEXT, 0 };
	it[n++] = (item){ T.var[j][4], PLAT_TEXT, 0 };
	return n;
}

/* Title and environment on at most tlines lines (items that do not fit are
 * dropped, least important last), then one line per variant.  Returns the
 * lines used; draws if draw. */
static int draw_header(const bench_results *r, const scr *s, int row, int tlines,
		       int draw)
{
	item it[MAX_ITEMS];
	int n, j, lines;

	n = title_items(r, it);
	lines = flow(s, row, 0, 0, 3, it, n, tlines, draw);
	for (j = 0; j < r->nvar; j++) {
		n = variant_items(r, j, it);
		lines += flow(s, row + lines, 0, 0, 2, it, n, 1, draw);
	}
	return lines;
}

static int geo_items(const bench_results *r, item *it)
{
	int n = 0, c;

	it[n++] = (item){ "geomean", PLAT_HEAD, 0 };
	for (c = 0; c < BENCH_CAT_COUNT; c++) {
		int count = 0, i;

		for (i = 0; i < r->nk; i++)
			if (r->k[i].k && r->k[i].k->category == (uint32_t)c)
				count++;
		if (!count)
			continue; /* category not run (--only) */
		it[n++] = (item){ bench_cat_name((uint32_t)c), PLAT_DIM, 0 };
		it[n++] = (item){ " ", PLAT_TEXT, 1 };
		it[n++] = (item){ T.geo[c], r->ngeo[c] ? speed_color(r->geo[c]) : PLAT_DIM, 1 };
	}
	return n;
}

static int legend_items(const bench_results *r, item *it, int wide)
{
	int n = 0;
	int cyc = r->cyc_per_tick > 0.0;

	it[n++] = (item){ T.status, (r->nfailed || !r->nk) ? PLAT_BAD : PLAT_GOOD, 0 };
	if (r->nvar == 2 && shifted(r)) {
		it[n++] = (item){ T.spleg, PLAT_DIM, 0 };
		it[n++] = (item){ "* = layout-sensitive", PLAT_WARN, 0 };
		if (wide)
			it[n++] = (item){ "spread = max/min - 1", PLAT_DIM, 0 };
	} else if (r->nvar == 2) {
		it[n++] = (item){ "speedup = B time / A time", PLAT_DIM, 0 };
	}
	if (T.refnote[0])
		it[n++] = (item){ T.refnote, PLAT_DIM, 0 };
	it[n++] = (item){ "us = microseconds per run()", PLAT_DIM, 0 };
	if (cyc && wide)
		it[n++] = (item){ "cyc/u = CPU cycles per unit", PLAT_DIM, 0 };
	if (r->nvar == 1)
		it[n++] = (item){ "Mu/s = million units per second", PLAT_DIM, 0 };
	if (wide)
		it[n++] = (item){ "noise = (median - min) / min", PLAT_DIM, 0 };
	return n;
}

static int draw_footer(const bench_results *r, const scr *s, int row, int wide,
		       int draw)
{
	item it[MAX_ITEMS];
	int n, lines = 0;

	if (r->nvar == 2) {
		n = geo_items(r, it);
		lines += flow(s, row + lines, 0, 9, 2, it, n, 2, draw);
		if (shifted(r)) {
			int j;

			n = 0;
			it[n++] = (item){ "by shift", PLAT_HEAD, 0 };
			for (j = 0; j < r->nshift; j++) {
				it[n++] = (item){ T.slabel[j], PLAT_DIM, 0 };
				it[n++] = (item){ " ", PLAT_TEXT, 1 };
				it[n++] = (item){ T.sgeo[j], r->ngeo_all ? speed_color(r->geo_shift[j])
									  : PLAT_DIM, 1 };
			}
			it[n++] = (item){ T.laynote, r->nlayout ? PLAT_WARN : PLAT_GOOD, 0 };
			lines += flow(s, row + lines, 0, 9, 2, it, n, 2, draw);
		}
		it[0] = (item){ T.geoall, r->ngeo_all ? speed_color(r->geo_all) : PLAT_BAD, 0 };
		it[1] = (item){ T.geoof, PLAT_TEXT, 0 };
		lines += flow(s, row + lines, 0, 0, 1, it, 2, 1, draw);
	}
	n = legend_items(r, it, wide);
	lines += flow(s, row + lines, 0, 0, 3, it, n, 1, draw);
	if (r->error) {
		it[0] = (item){ "error:", PLAT_BAD, 0 };
		it[1] = (item){ r->error, PLAT_BAD, 0 };
		lines += flow(s, row + lines, 0, 0, 1, it, 2, 1, draw);
	}
	return lines;
}

/* ---- tables ----------------------------------------------------------- */

enum {
	C_NAME, C_CAT, C_DESC, C_US_A, C_US_B, C_CYC_A, C_CYC_B, C_UNIT,
	C_SPEED, C_NOISE, C_CHECK, C_BAR, C_SLOT, C_MUS, C_RESULT,
	C_SPREAD, C_MARK, C_SSPEED, C_RANGE
};

typedef struct colspec {
	int id;
	const char *head;
	int width;
	int right;
	int gap; /* blanks before the column */
	int x;   /* computed: offset from the table origin */
	int arg; /* C_SSPEED: the shift index */
} colspec;

enum { D_US, D_CYC, D_MUS, D_COUNT }; /* shared decimals per quantity */

typedef struct table {
	colspec c[16 + BENCH_MAX_SHIFTS];
	int n;
	int width; /* total */
	double bar_scale;
	int dec[D_COUNT];
} table;

static void add_arg(table *t, int id, const char *head, int width, int right,
		    int gap, int arg)
{
	colspec *c = &t->c[t->n++];

	c->id = id;
	c->head = head;
	c->width = width;
	c->right = right;
	c->gap = t->n == 1 ? 0 : gap;
	c->x = t->width + c->gap;
	c->arg = arg;
	t->width = c->x + width;
}

static void add(table *t, int id, const char *head, int width, int right, int gap)
{
	add_arg(t, id, head, width, right, gap, 0);
}

/* Width of the per-shift speedup columns (all of them, with their gaps). */
static int sspeed_width(const bench_results *r)
{
	int j, w = 0;

	for (j = 0; j < r->nshift; j++) {
		int l = (int)strlen(T.slabel[j]);

		w += (j ? 1 : 2) + (l > SSP_W ? l : SSP_W);
	}
	return w;
}

static int max_desc_len(const bench_results *r)
{
	int i, m = 0;

	for (i = 0; i < r->nk; i++) {
		const char *d = r->k[i].k ? r->k[i].k->desc : NULL;
		int l = d ? (int)strlen(d) : 0;

		if (l > m)
			m = l;
	}
	return m > 32 ? 32 : m;
}

static double bar_scale(const bench_results *r)
{
	static const double nice[] = { 1.1, 1.25, 1.5, 2.0, 3.0, 4.0, 8.0, 16.0 };
	double m = 0.0;
	int i;

	for (i = 0; i < r->nk; i++) {
		const bench_kres *e = &r->k[i];
		double l;

		if (e->status || !(e->speedup > 0.0) || !is_fin(e->speedup))
			continue;
		l = fabs(log(e->speedup));
		if (l > m)
			m = l;
	}
	for (i = 0; i < (int)(sizeof(nice) / sizeof(nice[0])); i++)
		if (log(nice[i]) >= m * (1.0 - 1e-9))
			return nice[i];
	return 16.0;
}

/* Value of quantity q (D_*) for variant j of kernel e, 0 if none. */
static double quantity(const bench_results *r, const bench_kres *e, int q, int j)
{
	if (!has_timing(e) || j >= r->nvar || !(e->tshow[j] > 0.0))
		return 0.0;
	if (q == D_US)
		return bench_us(r, e->tshow[j]);
	if (q == D_CYC)
		return bench_cyc_per_unit(r, e->kv[0][j], e->tshow[j]);
	return bench_munits_per_s(r, e->kv[0][j], e->tshow[j]);
}

/* One number of decimals per quantity: three significant digits for the
 * smallest value, as long as the largest still fits its column. */
static void table_decimals(const bench_results *r, table *t)
{
	int q, i, j, width;

	for (q = 0; q < D_COUNT; q++) {
		double lo = 0.0, hi = 0.0;
		char b[64];
		int dec;

		width = 0;
		for (i = 0; i < t->n; i++) {
			int id = t->c[i].id;

			if ((q == D_US && (id == C_US_A || id == C_US_B)) ||
			    (q == D_CYC && (id == C_CYC_A || id == C_CYC_B)) ||
			    (q == D_MUS && (id == C_MUS || id == C_SLOT)))
				width = t->c[i].width;
		}
		for (i = 0; i < r->nk; i++)
			for (j = 0; j < r->nvar; j++) {
				double v = quantity(r, &r->k[i], q, j);

				if (!(v > 0.0) || !is_fin(v))
					continue;
				if (lo == 0.0 || v < lo)
					lo = v;
				if (v > hi)
					hi = v;
			}
		dec = lo >= 99.95 ? 0 : lo >= 9.995 ? 1 : lo >= 0.9995 ? 2 : 3;
		while (dec > 0 && width > 0 && snprintf(b, sizeof(b), "%.*f", dec, hi) > width)
			dec--;
		t->dec[q] = dec;
	}
}

/* Wide table for w columns; returns 0 if even the narrowest form is too
 * wide. */
static int wide_table(const bench_results *r, int w, table *t)
{
	int cyc = r->cyc_per_tick > 0.0, desc, room, ssw;
	table base;

	memset(&base, 0, sizeof(base));
	add(&base, C_NAME, "kernel", 8, 0, 0);
	add(&base, C_CAT, "cat", 3, 0, 2);
	/* the description goes here when there is room (see below) */
	if (r->nvar == 2) {
		add(&base, C_US_A, "A us/run", 8, 1, 2);
		add(&base, C_US_B, "B us/run", 8, 1, 2);
		if (cyc) {
			add(&base, C_CYC_A, "A cyc/u", 7, 1, 2);
			add(&base, C_CYC_B, "B cyc/u", 7, 1, 2);
			add(&base, C_UNIT, "unit", 4, 0, 1);
		}
		add(&base, C_SPEED, "speedup", 7, 1, 2);
		if (shifted(r)) {
			/* (the per-shift or min-max columns go here) */
			add(&base, C_SPREAD, "spread", 6, 1, 2);
			add(&base, C_MARK, "", 1, 0, 0);
		}
		add(&base, C_NOISE, "noise", 5, 1, 2);
		add(&base, C_CHECK, "check", 8, 0, 2);
	} else {
		add(&base, C_US_A, "us/run", 8, 1, 2);
		if (cyc)
			add(&base, C_CYC_A, "cyc/u", 7, 1, 2);
		add(&base, C_MUS, "Mu/s", 8, 1, 2);
		add(&base, C_UNIT, "unit", 4, 0, 1);
		add(&base, C_NOISE, "noise", 5, 1, 2);
		add(&base, C_RESULT, "result", 17, 0, 2);
		add(&base, C_CHECK, "check", 8, 0, 2);
	}
	if (base.width > w)
		return 0;
	/* layout shifts: one speedup column per shift, else min-max */
	ssw = 0;
	if (r->nvar == 2 && shifted(r)) {
		ssw = sspeed_width(r);
		if (base.width + ssw > w)
			ssw = base.width + 13 <= w ? -1 : 0;
	}
	room = w - base.width - (ssw > 0 ? ssw : ssw < 0 ? 13 : 0) - 2;
	desc = max_desc_len(r);
	if (desc > room)
		desc = room;
	if (desc < DESC_MIN)
		desc = 0;

	/* rebuild in display order with the description and shift columns */
	memset(t, 0, sizeof(*t));
	{
		int i, j;

		for (i = 0; i < base.n; i++) {
			const colspec *c = &base.c[i];

			add(t, c->id, c->head, c->width, c->right, c->gap);
			if (c->id == C_CAT && desc)
				add(t, C_DESC, "description", desc, 0, 2);
			if (c->id == C_SPEED && ssw > 0)
				for (j = 0; j < r->nshift; j++) {
					int l = (int)strlen(T.slabel[j]);

					add_arg(t, C_SSPEED, T.slabel[j], l > SSP_W ? l : SSP_W, 1,
						j ? 1 : 2, j);
				}
			if (c->id == C_SPEED && ssw < 0)
				add(t, C_RANGE, "min-max", 11, 1, 2);
		}
	}
	room = w - t->width - 2;
	if (r->nvar == 2 && room >= BAR_MIN) {
		int bw = room > BAR_MAX ? BAR_MAX : room;

		if (!(bw & 1))
			bw--;
		add(t, C_BAR, "", bw, 0, 2);
		t->bar_scale = bar_scale(r);
	}
	table_decimals(r, t);
	return 1;
}

/* One half of a compact line. */
static void compact_table(const bench_results *r, int with_cat, table *t)
{
	int cyc = r->cyc_per_tick > 0.0;

	memset(t, 0, sizeof(*t));
	add(t, C_NAME, "kernel", 8, 0, 0);
	if (with_cat)
		add(t, C_CAT, "cat", 3, 0, 1);
	if (r->nvar == 2) {
		add(t, C_US_A, "A us", 7, 1, 1);
		add(t, C_US_B, "B us", 7, 1, 1);
		add(t, C_SLOT, "speedup", 8, 1, 1);
		if (shifted(r))
			add(t, C_MARK, "", 1, 0, 0);
	} else {
		add(t, C_US_A, "us/run", 7, 1, 1);
		if (cyc)
			add(t, C_CYC_A, "cyc/u", 7, 1, 1);
		add(t, C_SLOT, "Mu/s", 8, 1, 1);
	}
	table_decimals(r, t);
}

static void cell_text(const bench_results *r, const table *t, const bench_kres *e,
		      int id, int arg, int width, char *b, size_t n, int *color)
{
	const bench_kernel *k = e->k;
	int fail = e->status != 0, timed = has_timing(e);
	int base = fail ? PLAT_BAD : PLAT_TEXT;

	b[0] = 0;
	*color = base;
	switch (id) {
	case C_NAME:
		snprintf(b, n, "%s", k ? str_or(k->name, "?") : "?");
		*color = fail ? PLAT_BAD : PLAT_BRIGHT;
		break;
	case C_CAT:
		snprintf(b, n, "%s", k ? bench_cat_name(k->category) : "?");
		*color = fail ? PLAT_BAD : PLAT_DIM;
		break;
	case C_DESC:
		snprintf(b, n, "%s", k ? str_or(k->desc, "") : "");
		*color = fail ? PLAT_BAD : PLAT_DIM;
		break;
	case C_US_A:
	case C_US_B: {
		int j = id == C_US_B ? 1 : 0;

		if (timed && j < r->nvar && e->tshow[j] > 0.0)
			fmt_fixed(b, n, bench_us(r, e->tshow[j]), t->dec[D_US], width);
		else
			snprintf(b, n, "-");
		break;
	}
	case C_CYC_A:
	case C_CYC_B: {
		int j = id == C_CYC_B ? 1 : 0;
		double c = j < r->nvar ? bench_cyc_per_unit(r, e->kv[0][j], e->tshow[j]) : 0.0;

		if (timed && c > 0.0)
			fmt_fixed(b, n, c, t->dec[D_CYC], width);
		else
			snprintf(b, n, "-");
		break;
	}
	case C_UNIT:
		snprintf(b, n, "%.4s", k ? str_or(k->unit, "?") : "?");
		*color = fail ? PLAT_BAD : PLAT_DIM;
		break;
	case C_SPEED:
		fmt_speedup(b, n, timed ? e->speedup : 0.0);
		*color = fail ? PLAT_BAD : speed_color(e->speedup);
		break;
	case C_NOISE:
		if (timed) {
			double p = e->noise_pct;

			fmt_pct(b, n, p);
			if (!fail && p > NOISE_WARN)
				*color = PLAT_WARN;
		} else {
			snprintf(b, n, "-");
		}
		break;
	case C_CHECK:
		snprintf(b, n, "%s", bench_status_word(e->status, r->nvar));
		*color = fail ? PLAT_BAD : PLAT_GOOD;
		break;
	case C_MUS:
		if (timed)
			fmt_fixed(b, n, bench_munits_per_s(r, k, e->tshow[0]), t->dec[D_MUS], width);
		else
			snprintf(b, n, "-");
		break;
	case C_RESULT:
		if (timed || e->v[0][0].calls)
			fmt_result(b, n, k, e->v[0][0].ref);
		else
			snprintf(b, n, "-");
		break;
	case C_SLOT:
		if (fail) {
			snprintf(b, n, "%s", bench_status_word(e->status, r->nvar));
			*color = PLAT_BAD;
		} else if (r->nvar == 2) {
			fmt_speedup(b, n, e->speedup);
			*color = speed_color(e->speedup);
		} else {
			fmt_fixed(b, n, bench_munits_per_s(r, k, e->tshow[0]), t->dec[D_MUS], width);
		}
		break;
	case C_SPREAD:
		if (timed && e->speedup > 0.0) {
			fmt_pct(b, n, e->spread_pct);
			if (!fail)
				*color = e->layout ? PLAT_WARN : PLAT_DIM;
		} else {
			snprintf(b, n, "-");
		}
		break;
	case C_MARK:
		snprintf(b, n, "%s", layout_mark(r, e));
		*color = PLAT_WARN;
		break;
	case C_SSPEED:
		if (timed && arg < r->nshift && e->sspeed[arg] > 0.0) {
			fmt_ratio(b, n, e->sspeed[arg]);
			if (!fail)
				*color = speed_color(e->sspeed[arg]);
		} else {
			snprintf(b, n, "-");
		}
		break;
	case C_RANGE:
		if (timed && e->speedup > 0.0) {
			char lo[16], hi[16];

			fmt_ratio(lo, sizeof(lo), e->sp_min);
			fmt_ratio(hi, sizeof(hi), e->sp_max);
			snprintf(b, n, "%s-%s", lo, hi);
			if (!fail)
				*color = e->layout ? PLAT_WARN : PLAT_TEXT;
		} else {
			snprintf(b, n, "-");
		}
		break;
	default:
		break;
	}
}

static void draw_bar_head(const scr *s, int x, int row, const colspec *c,
			  double scale)
{
	char l[16], rr[16];
	int half = c->width / 2;

	snprintf(l, sizeof(l), "%.2gx", 1.0 / scale);
	snprintf(rr, sizeof(rr), "%.3gx", scale);
	put(s, x + half, row, PLAT_HEAD, "1");
	if ((int)strlen(l) < half - 1 && (int)strlen(rr) < half - 1) {
		put(s, x, row, PLAT_HEAD, l);
		put(s, x + c->width - (int)strlen(rr), row, PLAT_HEAD, rr);
	}
}

static void draw_bar(const scr *s, int x, int row, const colspec *c,
		     double scale, const bench_kres *e)
{
	char b[BAR_MAX + 1];
	int half = c->width / 2, nb, over;
	double f;

	put(s, x + half, row, PLAT_DIM, "|");
	if (e->status || !(e->speedup > 0.0) || !is_fin(e->speedup) || scale <= 1.0)
		return;
	f = log(e->speedup) / log(scale);
	nb = (int)floor(fabs(f) * half + 0.5);
	over = nb > half;
	if (over)
		nb = half;
	if (nb <= 0)
		return;
	memset(b, '=', (size_t)nb);
	b[nb] = 0;
	if (over)
		b[f > 0 ? nb - 1 : 0] = f > 0 ? '>' : '<';
	if (f > 0)
		put(s, x + half + 1, row, PLAT_GOOD, b);
	else
		put(s, x + half - nb, row, PLAT_BAD, b);
}

static void draw_table_head(const scr *s, const table *t, int x0, int row)
{
	int i;

	for (i = 0; i < t->n; i++) {
		const colspec *c = &t->c[i];

		if (c->id == C_BAR)
			draw_bar_head(s, x0 + c->x, row, c, t->bar_scale);
		else
			cell(s, x0 + c->x, row, c->width, c->right, PLAT_HEAD, c->head);
	}
}

static void draw_table_row(const bench_results *r, const scr *s, const table *t,
			   int x0, int row, const bench_kres *e)
{
	char b[BUF];
	int i, color;

	for (i = 0; i < t->n; i++) {
		const colspec *c = &t->c[i];

		if (c->id == C_BAR) {
			draw_bar(s, x0 + c->x, row, c, t->bar_scale, e);
			continue;
		}
		cell_text(r, t, e, c->id, c->arg, c->width, b, sizeof(b), &color);
		cell(s, x0 + c->x, row, c->width, c->right, color, b);
	}
}

/* ---- minimal layout --------------------------------------------------- */

static int draw_minimal(const bench_results *r, const scr *s)
{
	char b[BUF], v[32];
	int ncol, nrow, cap, shown, i, cw = 16, gap = 2;

	if (r->nvar == 2 && r->ngeo_all) {
		fmt_speedup(v, sizeof(v), r->geo_all);
		snprintf(b, sizeof(b), "B/A %s, %d failed", v, r->nfailed);
		if (shifted(r))
			snprintf(b + strlen(b), sizeof(b) - strlen(b), ", %d layout*", r->nlayout);
	} else {
		snprintf(b, sizeof(b), "%d kernels, %d failed", r->nk, r->nfailed);
	}
	if ((int)strlen(b) > s->w)
		b[s->w > 0 ? s->w : 0] = 0;
	put(s, 0, 0, r->nfailed ? PLAT_BAD : PLAT_HEAD, b);

	ncol = (s->w + gap) / (cw + gap);
	if (ncol < 1)
		ncol = 1;
	nrow = s->rows - 1;
	cap = nrow > 0 ? ncol * nrow : 0;
	shown = r->nk <= cap ? r->nk : (cap > 0 ? cap - 1 : 0);
	for (i = 0; i < shown; i++) {
		const bench_kres *e = &r->k[i];
		int x = (i / nrow) * (cw + gap), y = 1 + i % nrow, color;

		if (e->status) {
			snprintf(v, sizeof(v), "FAIL");
			color = PLAT_BAD;
		} else if (r->nvar == 2) {
			fmt_speedup(v, sizeof(v), e->speedup);
			color = speed_color(e->speedup);
		} else {
			fmt_sig(v, sizeof(v), bench_us(r, e->tshow[0]), 3, 7);
			color = PLAT_TEXT;
		}
		snprintf(b, sizeof(b), "%-8.8s", e->k ? str_or(e->k->name, "?") : "?");
		cell(s, x, y, 8 < s->w - x ? 8 : s->w - x, 0, e->status ? PLAT_BAD : PLAT_BRIGHT, b);
		if (shifted(r) && r->nvar == 2) {
			/* "name     1.114x*": the marker in the cell's last column */
			if (x + 9 < s->w)
				cell(s, x + 9, y, 6 < s->w - x - 9 ? 6 : s->w - x - 9, 1, color, v);
			if (x + 15 < s->w)
				cell(s, x + 15, y, 1, 0, PLAT_WARN, layout_mark(r, e));
		} else if (x + 9 < s->w) {
			cell(s, x + 9, y, 7 < s->w - x - 9 ? 7 : s->w - x - 9, 1, color, v);
		}
	}
	if (shown < r->nk && cap > 0) {
		int x = (shown / nrow) * (cw + gap), y = 1 + shown % nrow;

		snprintf(b, sizeof(b), "+%d more", r->nk - shown);
		if (x < s->w)
			cell(s, x, y, cw < s->w - x ? cw : s->w - x, 0, PLAT_WARN, b);
	}
	return r->nk - shown;
}

/* ---- the screen ------------------------------------------------------- */

/* Rows needed by a layout whose table takes trows rows (column heading
 * included).  The title/environment block may shrink from 3 lines to 1;
 * returns the largest number of title lines that fits, 0 if none does. */
static int fit(const bench_results *r, const scr *s, int trows, int wide,
	       int *need)
{
	int tl, ftr = draw_footer(r, s, 0, wide, 0);

	for (tl = 3; tl >= 1; tl--) {
		*need = draw_header(r, s, 0, tl, 0) + trows + ftr;
		if (*need <= s->rows)
			return tl;
	}
	return 0;
}

int report_draw(const bench_results *r)
{
	scr s;
	table t;
	int tl = 0, need = 0, spare, row, i, half, wide = 0, compact = 0;

	plat_screen_size(&s.cols, &s.rows);
	s.w = s.cols - 1;
	plat_screen_clear();
	if (s.w < 1 || s.rows < 1) {
		layout_name = "none";
		return r->nk;
	}
	build_texts(r);
	half = (r->nk + 1) / 2;

	/* wide: one kernel per line */
	if (wide_table(r, s.w, &t)) {
		tl = fit(r, &s, 1 + r->nk, 1, &need);
		wide = tl > 0;
	}
	/* compact: two kernels per line, with the category if it fits */
	if (!wide) {
		compact_table(r, 1, &t);
		if (2 * t.width + 3 > s.w)
			compact_table(r, 0, &t);
		if (2 * t.width + 3 <= s.w) {
			tl = fit(r, &s, 1 + half, 0, &need);
			compact = tl > 0;
		}
	}
	if (!wide && !compact) {
		layout_name = "minimal";
		return draw_minimal(r, &s);
	}

	spare = s.rows - need;
	row = draw_header(r, &s, 0, tl, 1);
	if (spare >= 2) {
		row++; /* blank line after the header */
		spare--;
	}
	if (wide) {
		layout_name = "wide";
		draw_table_head(&s, &t, 0, row++);
		for (i = 0; i < r->nk; i++)
			draw_table_row(r, &s, &t, 0, row++, &r->k[i]);
	} else {
		int x1 = t.width + 3;

		layout_name = "compact";
		draw_table_head(&s, &t, 0, row);
		if (r->nk > 1)
			draw_table_head(&s, &t, x1, row);
		row++;
		for (i = 0; i < half; i++) {
			draw_table_row(r, &s, &t, 0, row + i, &r->k[i]);
			if (i + half < r->nk)
				draw_table_row(r, &s, &t, x1, row + i, &r->k[i + half]);
		}
		row += half;
	}
	if (spare >= 1)
		row++; /* blank line before the footer */
	draw_footer(r, &s, row, wide, 1);
	return 0;
}

/* ---- progress --------------------------------------------------------- */

void report_progress(const bench_results *r, int i, int phase)
{
	const bench_kres *e = &r->k[i];
	const bench_kernel *k = e->k;
	char a[32], b[32], sp[32];

	if (phase == 0) {
		plat_status(PLAT_TEXT, "[%2d/%2d] %-8s %-3s ", i + 1, r->nk,
			    k ? str_or(k->name, "?") : "?",
			    k ? bench_cat_name(k->category) : "?");
		return;
	}
	if (e->status & (BST_MISSING | BST_BADDESC | BST_NOTIME)) {
		plat_status(PLAT_BAD, " %s%s%s\n", bench_status_word(e->status, r->nvar),
			    e->note ? ": " : "", e->note ? e->note : "");
		return;
	}
	fmt_sig(a, sizeof(a), bench_us(r, e->tshow[0]), 3, 0);
	if (r->nvar == 2) {
		fmt_sig(b, sizeof(b), bench_us(r, e->tshow[1]), 3, 0);
		fmt_speedup(sp, sizeof(sp), e->speedup);
		plat_status(PLAT_TEXT, " A %7s us  B %7s us  ", a, b);
		plat_status(e->status ? PLAT_BAD : speed_color(e->speedup), shifted(r) ? "%6s" : "%7s",
			    sp);
		if (shifted(r)) {
			/* median over the shifts, then the spread of the per-shift
			 * speedups: the line still fits 74 columns */
			fmt_pct(b, sizeof(b), e->speedup > 0.0 ? e->spread_pct : -1.0);
			plat_status(e->layout ? PLAT_WARN : PLAT_DIM, " %5s%s", b, layout_mark(r, e));
		}
	} else {
		double c = bench_cyc_per_unit(r, k, e->tshow[0]);

		plat_status(PLAT_TEXT, " %7s us", a);
		if (c > 0.0) {
			fmt_sig(b, sizeof(b), c, 3, 0);
			plat_status(PLAT_TEXT, "  %7s cyc/%.4s", b, str_or(k->unit, "?"));
		}
	}
	plat_status(e->status ? PLAT_BAD : PLAT_GOOD, "  %s\n",
		    bench_status_word(e->status, r->nvar));

	/* one line of detail for a failure */
	if (e->status & (BST_MISMATCH | BST_UNSTABLE | BST_OVERRUN | BST_WRONG_A | BST_WRONG_B)) {
		char x[40], y[40], z[64];

		if (e->status & BST_OVERRUN) {
			plat_status(PLAT_BAD, "        guard around arena[0..%u) damaged\n",
				    (unsigned)k->arena_bytes);
		}
		if (e->status & BST_UNSTABLE) {
			int j;

			int sh;

			for (sh = 0; sh < r->nshift; sh++) {
				for (j = 0; j < r->nvar; j++) {
					const bench_vres *v = &e->v[sh][j];
					char at[16];

					if (!v->bad_calls)
						continue;
					at[0] = 0;
					if (shifted(r))
						snprintf(at, sizeof(at), " at +%d", r->shift[sh]);
					fmt_result(x, sizeof(x), k, v->ref);
					fmt_result(y, sizeof(y), k, v->bad_value);
					plat_status(PLAT_BAD, "        %s%s: %u of %u calls != %s (%s)\n",
						    r->nvar == 2 ? (j ? "B" : "A") : "run", at,
						    (unsigned)v->bad_calls, (unsigned)v->calls, x, y);
				}
			}
		}
		if (e->status & (BST_MISMATCH | BST_WRONG_A | BST_WRONG_B)) {
			fmt_result(x, sizeof(x), k, e->v[0][0].ref);
			z[0] = 0;
			if (e->ref == REF_CHECKED) {
				fmt_result(y, sizeof(y), k, e->ref_value);
				snprintf(z, sizeof(z), "  reference %s", y);
			}
			if (r->nvar == 2) {
				fmt_result(y, sizeof(y), k, e->v[0][1].ref);
				if (k->flags == BENCH_F_EXACT || !(e->status & BST_MISMATCH))
					plat_status(PLAT_BAD, "        A %s  B %s%s\n", x, y, z);
				else
					plat_status(PLAT_BAD, "        A %s  B %s  rel %.2g > %.2g%s\n",
						    x, y, e->rel_diff, k->tolerance, z);
			} else {
				plat_status(PLAT_BAD, "        result %s%s\n", x, z);
			}
		}
	}
}

/* ---- CSV log ---------------------------------------------------------- */

/* Free text for a CSV field: commas, quotes and control characters become
 * blanks or semicolons. */
static const char *csv_text(char *b, size_t n, const char *s)
{
	size_t i;

	snprintf(b, n, "%s", str_or(s, ""));
	for (i = 0; b[i]; i++) {
		if (b[i] == ',')
			b[i] = ';';
		else if (b[i] == '"' || (unsigned char)b[i] < 32 || (unsigned char)b[i] > 126)
			b[i] = ' ';
	}
	return b;
}

static void status_bits(char *b, size_t n, uint32_t s)
{
	static const struct {
		uint32_t bit;
		const char *word;
	} w[] = {
		{ BST_MISMATCH, "MISMATCH" }, { BST_UNSTABLE, "UNSTABLE" },
		{ BST_OVERRUN, "OVERRUN" },   { BST_BADDESC, "BADDESC" },
		{ BST_MISSING, "MISSING" },   { BST_NOTIME, "NOTIMER" },
		{ BST_WRONG_A, "WRONG_A" },   { BST_WRONG_B, "WRONG_B" },
	};
	size_t i, len = 0;

	b[0] = 0;
	if (!s) {
		snprintf(b, n, "ok");
		return;
	}
	for (i = 0; i < sizeof(w) / sizeof(w[0]); i++) {
		if (!(s & w[i].bit))
			continue;
		len += (size_t)snprintf(b + len, len < n ? n - len : 0, "%s%s",
					len ? "|" : "", w[i].word);
		if (len >= n)
			break;
	}
}

void report_log(const bench_results *r)
{
	char t1[160], t2[160], t3[160], t4[160], st[96], res[40], rv[40], refv[40];
	char smp[BENCH_MAX_SAMPLES * 16];
	double secs = r->tb_hz ? (double)r->total_ticks / (double)r->tb_hz : 0.0;
	int i, j, s, c, sh, multi = shifted(r);

	plat_log("#bench,version,mode,platform,cpu,tb_hz,cycles_per_tick,samples,"
		 "sample_ms,quick,kernels,failed,total_s,error");
	plat_log("bench,%s,%s,%s,%s,%lu,%g,%d,%d,%d,%d,%d,%.3f,%s", BENCH_VERSION,
		 r->nvar == 2 ? "ab" : "single", plat_name(),
		 csv_text(t1, sizeof(t1), plat_cpu_desc()),
		 (unsigned long)r->tb_hz, r->cyc_per_tick, r->samples,
		 r->sample_ms, r->quick, r->nk, r->nfailed, secs,
		 csv_text(t2, sizeof(t2), r->error));
	{
		int cols = 0, rows = 0;

		plat_screen_size(&cols, &rows);
		plat_log("#screen,layout,cols,rows");
		plat_log("screen,%s,%d,%d", layout_name, cols, rows);
	}
	plat_log("#variant,slot,id,label,compiler,cpu,cflags");
	for (j = 0; j < r->nvar; j++) {
		const bench_variant_info *v = r->var[j];

		plat_log("variant,%s,%s,%s,%s,%s,%s", r->nvar == 2 ? (j ? "B" : "A") : "A",
			 csv_text(t1, sizeof(t1), v->id), csv_text(t2, sizeof(t2), v->label),
			 csv_text(t3, sizeof(t3), v->compiler), csv_text(t4, sizeof(t4), v->cpu),
			 csv_text(st, sizeof(st), v->cflags));
	}
	/* layout shifts only: the shifts, then one more field per kernel line
	 * (the shift it measured) and the layout / geomean_shift lines */
	if (multi) {
		size_t len = 0;

		for (sh = 0; sh < r->nshift && len < sizeof(smp); sh++)
			len += (size_t)snprintf(smp + len, sizeof(smp) - len, "%s%d",
						sh ? ";" : "", r->shift[sh]);
		plat_log("#shifts,count,bytes,layout_sensitive,flips,layout_pct");
		plat_log("shifts,%d,%s,%d,%d,%g", r->nshift, smp, r->nlayout, r->nflip,
			 (double)BENCH_LAYOUT_PCT);
	}
	plat_log("#kernel,name,category,slot,variant,status,check,tolerance,units,"
		 "unit,arena_bytes,reps,samples,tmin_ticks,tmed_ticks,tmax_ticks,"
		 "us_per_run,cycles_per_unit,munits_per_s,noise_pct,result,"
		 "result_hex,bad_calls,calls,speedup,rel_diff,kernel_ms,sample_ticks,"
		 "ref,ref_value%s", multi ? ",shift" : "");
	for (i = 0; i < r->nk; i++) {
		const bench_kres *e = &r->k[i];
		const bench_kernel *k = e->k;

		status_bits(st, sizeof(st), e->status);
		/* per shift (in order), A then B */
		for (c = 0; c < r->nshift * r->nvar; c++) {
			const bench_vres *v;
			char shf[16];
			size_t len = 0;

			sh = c / r->nvar;
			j = c % r->nvar;
			v = &e->v[sh][j];

			shf[0] = 0;
			if (multi)
				snprintf(shf, sizeof(shf), ",%d", r->shift[sh]);

			smp[0] = 0;
			for (s = 0; s < e->samples && len < sizeof(smp); s++)
				len += (size_t)snprintf(smp + len, sizeof(smp) - len, "%s%.1f",
							s ? ";" : "", v->sample[s]);
			/* result and its hex form (EXACT); empty if run() never ran */
			if (!v->calls)
				rv[0] = res[0] = 0;
			else
				snprintf(rv, sizeof(rv), "%.17g", v->ref);
			if (v->calls && k && k->flags == BENCH_F_EXACT && v->ref >= 0.0 &&
			    v->ref <= 4294967295.0 && v->ref == (double)(uint32_t)v->ref)
				snprintf(res, sizeof(res), "0x%08x", (unsigned)(uint32_t)v->ref);
			else
				res[0] = 0;
			/* reference check of this variant: ok / WRONG, or why not */
			if (e->ref == REF_CHECKED)
				snprintf(refv, sizeof(refv), "%.17g", e->ref_value);
			else
				refv[0] = 0;
			plat_log("kernel,%s,%s,%s,%s,%s,%s,%g,%u,%s,%u,%u,%d,%.2f,%.2f,%.2f,"
				 "%.4f,%.4f,%.4f,%.3f,%s,%s,%u,%u,%.6f,%.3e,%.1f,%s,%s,%s%s",
				 csv_text(t1, sizeof(t1), k ? k->name : "?"),
				 k ? bench_cat_name(k->category) : "?",
				 r->nvar == 2 ? (j ? "B" : "A") : "A",
				 csv_text(t2, sizeof(t2), r->var[j]->id), st,
				 k && k->flags == BENCH_F_APPROX ? "APPROX" : "EXACT",
				 k ? k->tolerance : 0.0, k ? (unsigned)k->units : 0u,
				 csv_text(t3, sizeof(t3), k ? k->unit : ""),
				 k ? (unsigned)k->arena_bytes : 0u, (unsigned)e->reps,
				 e->samples, v->tmin, v->tmed, v->tmax, bench_us(r, v->tmin),
				 bench_cyc_per_unit(r, e->kv[sh][j], v->tmin),
				 bench_munits_per_s(r, k, v->tmin), bench_noise_pct(v),
				 rv, res, (unsigned)v->bad_calls, (unsigned)v->calls,
				 r->nvar != 2 ? 0.0 : multi ? e->sspeed[sh] : e->speedup,
				 e->rel_diff,
				 r->tb_hz ? (double)e->ticks * 1e3 / (double)r->tb_hz : 0.0,
				 smp,
				 e->ref != REF_CHECKED ? bench_ref_word(e->ref)
				 : (e->status & (j ? BST_WRONG_B : BST_WRONG_A)) ? "WRONG" : "ok",
				 refv, shf);
		}
	}
	if (multi && r->nvar == 2) {
		plat_log("#layout,name,category,status,shifts,speedup_median,speedup_min,"
			 "speedup_max,spread_pct,noise_pct,band_pct,flip,sensitive,"
			 "a_ticks,b_ticks,speedups");
		for (i = 0; i < r->nk; i++) {
			const bench_kres *e = &r->k[i];
			const bench_kernel *k = e->k;
			size_t len = 0;

			status_bits(st, sizeof(st), e->status);
			smp[0] = 0;
			for (sh = 0; sh < r->nshift && len < sizeof(smp); sh++)
				len += (size_t)snprintf(smp + len, sizeof(smp) - len, "%s%.6f",
							sh ? ";" : "", e->sspeed[sh]);
			plat_log("layout,%s,%s,%s,%d,%.6f,%.6f,%.6f,%.3f,%.3f,%.3f,%d,%d,"
				 "%.2f,%.2f,%s",
				 csv_text(t1, sizeof(t1), k ? k->name : "?"),
				 k ? bench_cat_name(k->category) : "?", st, r->nshift,
				 e->speedup, e->sp_min, e->sp_max, e->spread_pct, e->noise_pct,
				 e->band_pct, e->flip, e->layout, e->tshow[0], e->tshow[1], smp);
		}
	}
	if (r->nvar == 2) {
		plat_log("#geomean,category,speedup,kernels");
		for (c = 0; c < BENCH_CAT_COUNT; c++)
			if (r->ngeo[c])
				plat_log("geomean,%s,%.6f,%d", bench_cat_name((uint32_t)c),
					 r->geo[c], r->ngeo[c]);
		plat_log("geomean,ALL,%.6f,%d", r->geo_all, r->ngeo_all);
		if (multi) {
			plat_log("#geomean_shift,shift,speedup,kernels");
			for (sh = 0; sh < r->nshift; sh++)
				plat_log("geomean_shift,%d,%.6f,%d", r->shift[sh],
					 r->geo_shift[sh], r->ngeo_all);
		}
	}
	plat_log("#end");
}
