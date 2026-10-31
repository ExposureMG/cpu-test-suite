/*
 * platform_host.c - host implementation of platform.h.
 *
 * The "framebuffer" is a virtual cols x rows character grid with the same
 * rules as the libxenon console (see platform.h): the last column is never
 * written, text does not wrap, spaces are transparent.  plat_screen_done()
 * prints the grid inside a frame (ANSI colours when stdout is a terminal)
 * and reports every character that fell outside the grid or was not
 * printable; main() turns that into a non-zero exit status, which makes
 * `make check` fail.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bench.h"
#include "platform.h"

static int scr_cols = 160, scr_rows = 45;
static char *grid;
static unsigned char *gcolor;
static unsigned long clipped, invalid;
static int use_color = -1; /* -1: when stdout is a terminal */
static int frame = 1;
static int status_col;     /* current column of the progress console */
static void *arena_raw, *arena;
static char cpu_desc[128];
static double ghz;         /* --ghz: cycles per tick (ns), 0 = unknown */
static int xpreview;       /* --xenon-preview: Xenon header texts and units */

static const char *const ansi[PLAT_NCOLORS] = {
	[PLAT_TEXT] = "\033[0m",
	[PLAT_BRIGHT] = "\033[0;97m",
	[PLAT_DIM] = "\033[0;90m",
	[PLAT_HEAD] = "\033[1;97m",
	[PLAT_GOOD] = "\033[0;92m",
	[PLAT_BAD] = "\033[0;91m",
	[PLAT_WARN] = "\033[0;93m",
	[PLAT_VAR_A] = "\033[0;96m",
	[PLAT_VAR_B] = "\033[0;33m",
};

static void usage(FILE *f, const bench_opts *o)
{
	fprintf(f,
		"usage: bench-host [options]\n"
		"  --quick            3 samples x 5 ms per kernel (logic and layout checks)\n"
		"  --only ID[,ID...]  run only these kernels\n"
		"  --samples N        samples per kernel and variant (default %d)\n"
		"  --sample-ms N      target length of one sample in ms (default %d)\n"
		"  --cols N           virtual screen width in characters (default 160)\n"
		"  --rows N           virtual screen height (default 45)\n"
		"  --color, --no-color  ANSI colours (default: if stdout is a terminal)\n"
		"  --no-frame         print the screen without the border\n"
		"  --no-csv           do not print the CSV log\n"
		"  --ghz F            CPU clock, to show cycles per unit (default: unknown)\n"
		"  --xenon-preview    layout preview: Xenon header texts, a 49.875 MHz\n"
		"                     timebase and 64 cycles per tick (numbers are host's)\n"
		"exit status: 0 ok, |1 a kernel failed, |2 the report did not fit, "
		"|4 setup error\n",
		o->samples, o->sample_ms);
}

static int parse_int(const char *s, int lo, int hi, int *out)
{
	char *end;
	long v;

	if (!s)
		return -1;
	v = strtol(s, &end, 10);
	if (end == s || *end || v < lo || v > hi)
		return -1;
	*out = (int)v;
	return 0;
}

int plat_init(int argc, char **argv, bench_opts *o)
{
	int i, samples = -1, sample_ms = -1;
	size_t n;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!strcmp(a, "--quick")) {
			o->quick = 1;
		} else if (!strcmp(a, "--only") && v) {
			o->only = v;
			i++;
		} else if (!strcmp(a, "--samples") && !parse_int(v, 1, 31, &samples)) {
			i++;
		} else if (!strcmp(a, "--sample-ms") && !parse_int(v, 1, 10000, &sample_ms)) {
			i++;
		} else if (!strcmp(a, "--cols") && !parse_int(v, 1, 1000, &scr_cols)) {
			i++;
		} else if (!strcmp(a, "--rows") && !parse_int(v, 1, 1000, &scr_rows)) {
			i++;
		} else if (!strcmp(a, "--color")) {
			use_color = 1;
		} else if (!strcmp(a, "--no-color")) {
			use_color = 0;
		} else if (!strcmp(a, "--no-frame")) {
			frame = 0;
		} else if (!strcmp(a, "--no-csv")) {
			o->csv = 0;
		} else if (!strcmp(a, "--xenon-preview")) {
			xpreview = 1;
		} else if (!strcmp(a, "--ghz") && v && (ghz = strtod(v, NULL)) > 0.0 &&
			   ghz < 100.0) {
			i++;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(stdout, o);
			exit(0);
		} else {
			fprintf(stderr, "bench-host: bad option or value: %s\n", a);
			usage(stderr, o);
			return 4;
		}
	}
	if (o->quick) {
		o->samples = 3;
		o->sample_ms = 5;
	}
	if (samples > 0)
		o->samples = samples;
	if (sample_ms > 0)
		o->sample_ms = sample_ms;
	if (use_color < 0)
		use_color = isatty(STDOUT_FILENO);

	n = (size_t)scr_cols * (size_t)scr_rows;
	grid = malloc(n);
	gcolor = malloc(n);
	if (!grid || !gcolor) {
		fprintf(stderr, "bench-host: out of memory\n");
		return 4;
	}
	memset(grid, ' ', n);
	memset(gcolor, PLAT_TEXT, n);

	arena_raw = malloc(BENCH_ARENA_BYTES + BENCH_ARENA_ALIGN + 2 * PLAT_GUARD_BYTES);
	if (arena_raw) {
		uintptr_t p = (uintptr_t)arena_raw + PLAT_GUARD_BYTES;

		p = (p + BENCH_ARENA_ALIGN - 1) & ~(uintptr_t)(BENCH_ARENA_ALIGN - 1);
		arena = (void *)p;
	}
	return 0;
}

void *plat_arena(void)
{
	return arena;
}

uint64_t plat_ticks(void)
{
	struct timespec ts;
	uint64_t ns;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	ns = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
	/* preview: 49.875 MHz = ns * 399 / 8000 */
	return xpreview ? ns / 8000u * 399u + ns % 8000u * 399u / 8000u : ns;
}

uint64_t plat_ticks_per_sec(void)
{
	return xpreview ? 49875000u : 1000000000u;
}

double plat_cycles_per_tick(void)
{
	return xpreview ? 64.0 : ghz; /* host ticks are nanoseconds */
}

const char *plat_name(void)
{
	return xpreview ? "Xenon" : "host";
}

const char *plat_cpu_desc(void)
{
	FILE *f;
	char line[256];

	if (cpu_desc[0])
		return cpu_desc;
	if (xpreview) {
		snprintf(cpu_desc, sizeof(cpu_desc), "PVR 00710800, speed 1 (full)");
		return cpu_desc;
	}
	snprintf(cpu_desc, sizeof(cpu_desc), "unknown CPU");
	f = fopen("/proc/cpuinfo", "r");
	if (!f)
		return cpu_desc;
	while (fgets(line, sizeof(line), f)) {
		char *p = strchr(line, ':');
		size_t l;

		if (strncmp(line, "model name", 10) || !p)
			continue;
		p++;
		while (*p == ' ' || *p == '\t')
			p++;
		l = strlen(p);
		while (l && (p[l - 1] == '\n' || p[l - 1] == ' '))
			p[--l] = 0;
		if (l)
			snprintf(cpu_desc, sizeof(cpu_desc), "%s", p);
		break;
	}
	fclose(f);
	return cpu_desc;
}

int plat_cpu_warning(void)
{
	return 0;
}

/* Same line discipline as the Xenon console: cut at cols - 1. */
void plat_status(int color, const char *fmt, ...)
{
	char buf[512];
	const char *p;
	int colored = 0;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (color < 0 || color >= PLAT_NCOLORS)
		color = PLAT_TEXT;
	for (p = buf; *p; p++) {
		if (*p == '\n') {
			if (colored)
				fputs(ansi[PLAT_TEXT], stdout);
			colored = 0;
			putchar('\n');
			status_col = 0;
		} else if (status_col < scr_cols - 1) {
			if (use_color > 0 && !colored && color != PLAT_TEXT) {
				fputs(ansi[color], stdout);
				colored = 1;
			}
			putchar(*p);
			status_col++;
		}
	}
	if (colored)
		fputs(ansi[PLAT_TEXT], stdout);
	fflush(stdout);
}

void plat_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

void plat_screen_size(int *cols, int *rows)
{
	*cols = scr_cols;
	*rows = scr_rows;
}

void plat_screen_clear(void)
{
	size_t n = (size_t)scr_cols * (size_t)scr_rows;

	memset(grid, ' ', n);
	memset(gcolor, PLAT_TEXT, n);
}

void plat_screen_put(int col, int row, int color, const char *text)
{
	int i;

	if (!text)
		return;
	if (color < 0 || color >= PLAT_NCOLORS)
		color = PLAT_TEXT;
	for (i = 0; text[i]; i++) {
		unsigned char c = (unsigned char)text[i];
		int x = col + i;

		if (c < 32 || c > 126) {
			invalid++;
			c = '?';
		}
		if (c == ' ')
			continue; /* transparent */
		if (row < 0 || row >= scr_rows || x < 0 || x >= scr_cols - 1) {
			clipped++;
			continue;
		}
		grid[(size_t)row * scr_cols + x] = (char)c;
		gcolor[(size_t)row * scr_cols + x] = (unsigned char)color;
	}
}

int plat_screen_done(void)
{
	int x, y, cur;

	if (frame) {
		putchar('+');
		for (x = 0; x < scr_cols; x++)
			putchar('-');
		puts("+");
	}
	for (y = 0; y < scr_rows; y++) {
		const char *g = grid + (size_t)y * scr_cols;
		const unsigned char *c = gcolor + (size_t)y * scr_cols;
		int end = scr_cols;

		if (!frame)
			while (end > 0 && g[end - 1] == ' ')
				end--;
		if (frame)
			putchar('|');
		cur = -1;
		for (x = 0; x < end; x++) {
			if (use_color > 0 && g[x] != ' ' && c[x] != cur) {
				cur = c[x];
				fputs(ansi[cur], stdout);
			}
			putchar(g[x]);
		}
		if (use_color > 0 && cur >= 0)
			fputs(ansi[PLAT_TEXT], stdout);
		if (frame)
			putchar('|');
		putchar('\n');
	}
	if (frame) {
		putchar('+');
		for (x = 0; x < scr_cols; x++)
			putchar('-');
		puts("+");
	}
	printf("host screen %dx%d: %lu clipped, %lu invalid characters\n", scr_cols,
	       scr_rows, clipped, invalid);
	fflush(stdout);
	return (int)(clipped + invalid > 0x7fffffffu ? 0x7fffffff : clipped + invalid);
}

int plat_finish(int status)
{
	fflush(stdout);
	free(arena_raw);
	free(grid);
	free(gcolor);
	return status;
}
