/*
 * main.c - LibXenon CPU benchmark: run every kernel of the variants linked
 * into this image, draw the comparison, dump the CSV log, stop.
 *
 * On Xenon plat_finish() never returns (returning from main would make
 * libxenon reload XeLL and wipe the screen).  On the host the exit status is
 * a bit mask: 1 = a kernel failed, 2 = the report did not fit (clipped
 * characters or kernels left out), 4 = setup error.
 */
#include <stddef.h>

#include "platform.h"
#include "report.h"
#include "runner.h"

extern const bench_variant_info *const bench_variants[];
extern const int bench_nvariants;

static bench_results results; /* ~80 KB: keep it off the stack */

int main(int argc, char **argv)
{
	bench_opts opts;
	int rc, status = 0, missing, clipped, j;
	void *arena;

	opts.samples = BENCH_SAMPLES;
	opts.sample_ms = BENCH_SAMPLE_MS;
	opts.quick = 0;
	opts.only = NULL;
	opts.csv = 1;
	rc = plat_init(argc, argv, &opts);
	if (rc)
		return plat_finish(rc);

	plat_status(PLAT_HEAD, "LibXenon CPU benchmark %s\n", BENCH_VERSION);
	for (j = 0; j < bench_nvariants; j++) {
		const bench_variant_info *v = bench_variants[j];

		plat_status(j ? PLAT_VAR_B : PLAT_VAR_A, "  %s %-10s %s, cpu %s, cflags %s\n",
			    bench_nvariants == 2 ? (j ? "B" : "A") : "=",
			    v->label ? v->label : "?", v->compiler ? v->compiler : "?",
			    v->cpu ? v->cpu : "?", v->cflags ? v->cflags : "?");
	}
	plat_status(plat_cpu_warning() ? PLAT_WARN : PLAT_TEXT, "  %s %s\n", plat_name(),
		    plat_cpu_desc());
	plat_status(PLAT_TEXT, "  %d x %d ms samples per kernel and variant%s\n",
		    opts.samples, opts.sample_ms, opts.quick ? " (quick)" : "");

	arena = plat_arena();
	if (!arena) {
		plat_status(PLAT_BAD, "cannot allocate the %u MB arena\n",
			    (unsigned)(BENCH_ARENA_BYTES >> 20));
		return plat_finish(4);
	}

	rc = bench_run_all(&results, bench_variants, bench_nvariants, &opts,
			   report_progress, arena);
	if (results.error)
		plat_status(PLAT_BAD, "error: %s\n", results.error);
	if (rc)
		status |= 4;
	if (results.nfailed)
		status |= 1;
	plat_status(PLAT_TEXT, "drawing the report...\n");

	missing = report_draw(&results);
	clipped = plat_screen_done();
	if (missing || clipped)
		status |= 2;
	if (opts.csv)
		report_log(&results);
	return plat_finish(status);
}
