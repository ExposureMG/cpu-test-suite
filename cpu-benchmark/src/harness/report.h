/*
 * report.h - progress lines, the final screen and the serial CSV log
 * (DESIGN.md section 6).  Portable: draws through platform.h only.
 */
#ifndef BENCH_REPORT_H
#define BENCH_REPORT_H

#include "runner.h"

/* Progress callback for bench_run_all(): phase 0 prints "[ i/n] name cat "
 * as a partial line before the kernel runs (so a crash shows which kernel
 * was running), phase 1 completes it with the timings and the check. */
void report_progress(const bench_results *r, int i, int phase);

/* Clears the screen and draws the most informative layout that fits.
 * Returns the number of kernels that could not be shown (0 = complete). */
int report_draw(const bench_results *r);

/* Name of the layout report_draw() chose: "wide", "compact", "minimal". */
const char *report_layout(void);

/* Everything as CSV lines through plat_log() (serial only on Xenon). */
void report_log(const bench_results *r);

#endif /* BENCH_REPORT_H */
