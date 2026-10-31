/*
 * platform.h - what the portable harness (runner.c, report.c, main.c) needs
 * from the machine it runs on.
 *
 * Two implementations:
 *   platform_xenon.c  libxenon on the Xbox 360: framebuffer console, UART,
 *                     timebase.  Never returns from plat_finish().
 *   platform_host.c   host build: the "framebuffer" is a virtual cols x rows
 *                     character grid printed to stdout at the end; clipped
 *                     characters are counted so `make check` can fail on them.
 *
 * Screen model (identical on both): a grid of cols x rows character cells,
 * cleared to the background colour by plat_screen_clear().  plat_screen_put()
 * draws text left to right from (col, row) and never wraps.  The last column
 * (col == cols - 1) is never drawn: on Xenon a character there would advance
 * the console cursor past max_x, which starts a new line and, on the last
 * row, scrolls the whole screen.  Spaces are transparent (they leave the
 * cell as it is), which on Xenon saves a framebuffer flush per blank cell.
 * Text must be printable ASCII; anything else is drawn as '?'.
 */
#ifndef BENCH_PLATFORM_H
#define BENCH_PLATFORM_H

#include <stdint.h>

/* Portable colour names; each platform maps them to its own values. */
enum plat_color {
	PLAT_TEXT = 0, /* normal text                          */
	PLAT_BRIGHT,   /* emphasis                             */
	PLAT_DIM,      /* secondary text, units, separators    */
	PLAT_HEAD,     /* title and column headings            */
	PLAT_GOOD,     /* green: A faster, checks passed       */
	PLAT_BAD,      /* red: A slower, failed kernels        */
	PLAT_WARN,     /* yellow: noisy timings, warnings      */
	PLAT_VAR_A,    /* variant A label                      */
	PLAT_VAR_B,    /* variant B label                      */
	PLAT_NCOLORS
};

/* Bytes the platform reserves on both sides of the arena for the runner's
 * guard patterns (see runner.c). */
#define PLAT_GUARD_BYTES 4096u

/* Run options.  main() fills the defaults; the host's plat_init() parses the
 * command line into them, the Xenon one leaves them alone. */
typedef struct bench_opts {
	int samples;      /* samples per variant and kernel                  */
	int sample_ms;    /* target length of one sample                     */
	int quick;        /* host --quick: few, short samples                */
	const char *only; /* comma-separated kernel ids to run, NULL = all   */
	int csv;          /* dump the CSV log after the report               */
} bench_opts;

/* Initialise video/console/clock speed (Xenon) or parse the command line
 * (host) and allocate the arena.  Returns 0 on success, otherwise an exit
 * status for plat_finish() (for example a host usage error). */
int plat_init(int argc, char **argv, bench_opts *opts);

/* The arena: BENCH_ARENA_BYTES, BENCH_ARENA_ALIGN-aligned, with at least
 * PLAT_GUARD_BYTES of writable slack before it and after its end.  NULL if
 * it could not be allocated. */
void *plat_arena(void);

/* Monotonic tick counter (Xenon: the timebase; host: nanoseconds). */
uint64_t plat_ticks(void);
uint64_t plat_ticks_per_sec(void);
/* CPU cycles per tick (Xenon 64.0), 0 if unknown (host). */
double plat_cycles_per_tick(void);

/* "Xenon" / "host". */
const char *plat_name(void);
/* One line describing the CPU and its clock state. */
const char *plat_cpu_desc(void);
/* Non-zero if plat_cpu_desc() describes something that may distort the
 * measurement (Xenon: CPU not at full speed). */
int plat_cpu_warning(void);

/* Progress text: scrolling console and serial (Xenon) or stdout (host).
 * A '\n' in fmt ends the line; a line longer than cols - 1 is cut, so the
 * console never wraps it. */
void plat_status(int color, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* One line of machine-readable log: serial only on Xenon, stdout on the
 * host.  No newline in fmt. */
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Final screen. */
void plat_screen_size(int *cols, int *rows);
void plat_screen_clear(void);
void plat_screen_put(int col, int row, int color, const char *text);
/* The report is complete.  Host: print the grid and the clip statistics,
 * return the number of clipped or invalid characters.  Xenon: detach printf
 * from the framebuffer so nothing can scroll the report away; returns 0. */
int plat_screen_done(void);

/* End of the program.  Xenon: spins forever at low thread priority so the
 * report stays on screen (returning from main would make libxenon reload
 * XeLL).  Host: returns the exit status for main. */
int plat_finish(int status);

#endif /* BENCH_PLATFORM_H */
