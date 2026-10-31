/*
 * platform_xenon.c - libxenon implementation of platform.h (Xbox 360).
 *
 * Checked against the libxenon sources (it cannot be run here):
 *   xenos/xenos.h            void xenos_init(int videoMode), VIDEO_MODE_AUTO
 *   console/console.h        console_init, console_clrscr, console_set_colors
 *                            (BGRA words 0xBBGGRR00, as the CONSOLE_COLOR_*
 *                            values), console_get_dimensions (characters
 *                            after overscan), console_set_cursor,
 *                            console_putch, console_close
 *   xenon_soc/xenon_power.h  xenon_make_it_faster, XENON_SPEED_*;
 *                            xenon_get_speed() exists in xenon_power.c but is
 *                            not declared in the header
 *   ppc/register.h           mfspr(); includes ppc/xenonsprs.h, which
 *                            #defines lower-case SPR names (pvr, pir, pid,
 *                            hdec, tcr, ...): no identifiers with those names
 *                            in this file
 *   ppc/timebase.h           PPC_TIMEBASE_FREQ (its mftb() reads TBL then
 *                            TBU without a retry, so it is not used)
 *   xenon_uart/xenon_uart.h  putch(): UART only ('\n' becomes "\r\n")
 *
 * Console semantics (drivers/console/console.c): console_putch() draws at
 * the cursor and advances it; reaching max_x, or '\n', starts a new line,
 * and a new line past the last row scrolls the screen.  Every character
 * flushes the framebuffer (memdcbst), so text is drawn sparingly: the final
 * screen skips blanks (it was cleared) and never writes the last column, so
 * the cursor can neither wrap nor scroll.  printf() goes through newlib to
 * vfs_console_write(), i.e. to the framebuffer console (stdout_hook) and the
 * UART (putch).
 *
 * Startup state (startup_from_xell.S): main() runs on thread 0 with
 * MSR[EE] = 0, so nothing interrupts a measurement.  Returning from main()
 * calls exit(), which tries to reload XeLL, so plat_finish() never returns.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <console/console.h>
#include <ppc/register.h>
#include <ppc/timebase.h>
#include <xenon_soc/xenon_power.h>
#include <xenon_uart/xenon_uart.h>
#include <xenos/xenos.h>

#include "bench.h"
#include "platform.h"

/* drivers/xenon_soc/xenon_power.c: returns ld(0x20061188) & 7, the
 * XENON_SPEED_* value the CPU runs at.  Not declared in xenon_power.h. */
extern int xenon_get_speed(void);

#define SPR_PVR 287
#define BG CONSOLE_COLOR_BLACK

/* Foreground colours, BGRA words like the CONSOLE_COLOR_* values. */
static const unsigned int fg[PLAT_NCOLORS] = {
	[PLAT_TEXT] = CONSOLE_COLOR_GREY,    /* C0C0C0 */
	[PLAT_BRIGHT] = CONSOLE_COLOR_WHITE, /* FFFFFF */
	[PLAT_DIM] = 0x80808000u,            /* 808080 grey   */
	[PLAT_HEAD] = 0xFFC08000u,           /* R80 GC0 BFF: light blue */
	[PLAT_GOOD] = 0x40E04000u,           /* R40 GE0 B40: green */
	[PLAT_BAD] = 0x4040FF00u,            /* RFF G40 B40: red   */
	[PLAT_WARN] = CONSOLE_COLOR_YELLOW,  /* FFFF00 */
	[PLAT_VAR_A] = 0xFFFF4000u,          /* R40 GFF BFF: cyan  */
	[PLAT_VAR_B] = CONSOLE_COLOR_ORANGE, /* FF6600 */
};

static void *arena;
static int status_col; /* column of the scrolling console's cursor */
static char cpu_desc[96];

static unsigned int color_of(int c)
{
	return c >= 0 && c < PLAT_NCOLORS ? fg[c] : fg[PLAT_TEXT];
}

static void screen_dims(int *cols, int *rows)
{
	unsigned int w = 0, h = 0;

	console_get_dimensions(&w, &h);
	*cols = (int)w;
	*rows = (int)h;
}

int plat_init(int argc, char **argv, bench_opts *opts)
{
	void *raw;

	(void)argc;
	(void)argv;
	(void)opts; /* no command line on the console: compile-time defaults */
	arena = NULL; /* .bss is cleared by the ELF loader; do not depend on it */
	status_col = 0;

	xenos_init(VIDEO_MODE_AUTO);
	console_set_colors(BG, fg[PLAT_TEXT]); /* allowed before console_init */
	console_init();                        /* clears the screen, hooks stdout */
	xenon_make_it_faster(XENON_SPEED_FULL);

	/* The heap runs from the end of .bss to 0x9E000000 (app.lds), and
	 * libxenon's sbrk has no limit check: 32 MB + 72 KB is fine. */
	raw = malloc(BENCH_ARENA_BYTES + BENCH_ARENA_ALIGN + 2 * PLAT_GUARD_BYTES);
	if (raw) {
		uintptr_t p = (uintptr_t)raw + PLAT_GUARD_BYTES;

		p = (p + BENCH_ARENA_ALIGN - 1) & ~(uintptr_t)(BENCH_ARENA_ALIGN - 1);
		arena = (void *)p;
	}
	return 0;
}

void *plat_arena(void)
{
	return arena;
}

/* 64-bit timebase from 32-bit reads: TBU, TBL, TBU again until TBU did not
 * change (a carry from TBL into TBU between the reads). */
uint64_t plat_ticks(void)
{
	uint32_t hi, lo, hi2;

	do {
		__asm__ __volatile__("mftbu %0" : "=r"(hi));
		__asm__ __volatile__("mftbl %0" : "=r"(lo));
		__asm__ __volatile__("mftbu %0" : "=r"(hi2));
	} while (hi != hi2);
	return ((uint64_t)hi << 32) | lo;
}

uint64_t plat_ticks_per_sec(void)
{
	return (uint64_t)PPC_TIMEBASE_FREQ; /* 3.192 GHz / 64 = 49.875 MHz */
}

double plat_cycles_per_tick(void)
{
	return 64.0;
}

const char *plat_name(void)
{
	return "Xenon";
}

static const char *speed_name(int s)
{
	switch (s) {
	case XENON_SPEED_FULL:
		return "full";
	case XENON_SPEED_3_2:
		return "3_2";
	case XENON_SPEED_1_2:
		return "1_2";
	case XENON_SPEED_1_3:
		return "1_3";
	default:
		return "unknown";
	}
}

const char *plat_cpu_desc(void)
{
	unsigned int v = mfspr(SPR_PVR);
	int s = xenon_get_speed();

	snprintf(cpu_desc, sizeof(cpu_desc), "PVR %08x, speed %d (%s)", v, s,
		 speed_name(s));
	return cpu_desc;
}

int plat_cpu_warning(void)
{
	return xenon_get_speed() != XENON_SPEED_FULL;
}

/* Scrolling console + UART through printf.  Lines are cut at cols - 1 so
 * the console never wraps a line on its own. */
void plat_status(int color, const char *fmt, ...)
{
	char in[512], out[512];
	const char *p;
	size_t n = 0;
	int cols, rows;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(in, sizeof(in), fmt, ap);
	va_end(ap);
	screen_dims(&cols, &rows);
	for (p = in; *p && n < sizeof(out) - 1; p++) {
		unsigned char c = (unsigned char)*p;

		if (c == '\n') {
			out[n++] = '\n';
			status_col = 0;
		} else if (status_col < cols - 1) {
			out[n++] = (c < 32 || c > 126) ? '?' : (char)c;
			status_col++;
		}
	}
	out[n] = 0;
	console_set_colors(BG, color_of(color));
	fputs(out, stdout);
	fflush(stdout); /* the colour applies when the text reaches the console */
	console_set_colors(BG, fg[PLAT_TEXT]);
}

/* Serial only. */
void plat_log(const char *fmt, ...)
{
	char buf[1024];
	const char *p;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	for (p = buf; *p; p++)
		putch((unsigned char)*p);
	putch('\n');
}

void plat_screen_size(int *cols, int *rows)
{
	screen_dims(cols, rows);
}

void plat_screen_clear(void)
{
	console_set_colors(BG, fg[PLAT_TEXT]);
	console_clrscr(); /* background fill, cursor to 0,0 */
}

void plat_screen_put(int col, int row, int color, const char *text)
{
	int cols, rows, i;

	if (!text)
		return;
	screen_dims(&cols, &rows);
	if (row < 0 || row >= rows)
		return;
	console_set_colors(BG, color_of(color));
	for (i = 0; text[i]; i++) {
		unsigned char c = (unsigned char)text[i];
		int x = col + i;

		if (x < 0)
			continue;
		if (x >= cols - 1)
			break; /* the last column would move the cursor to a new line */
		if (c == ' ')
			continue; /* cleared already; saves a framebuffer flush */
		if (c < 32 || c > 126)
			c = '?';
		console_set_cursor(x, row);
		console_putch((char)c);
	}
	console_set_colors(BG, fg[PLAT_TEXT]);
}

int plat_screen_done(void)
{
	/* From here on printf reaches the UART only: nothing can scroll the
	 * report off the screen. */
	fflush(stdout);
	console_close();
	return 0;
}

int plat_finish(int status)
{
	(void)status;
	fflush(stdout);
	console_close();
	for (;;)
		__asm__ __volatile__("or 1,1,1"); /* low thread priority */
	return status;
}
