/*
 * bench_lib.h - access to each toolchain's own newlib routines (LIB kernels).
 *
 * A combined A/B image can only link one libc, so tools/libprefix.py extracts
 * the members that define the routines below from a toolchain's libc.a/libm.a
 * (plus everything they depend on inside libc/libm), prefixes their symbols
 * with "<variant>_lib_" and hands the Makefile one relocatable object per
 * variant.  Kernel code calls BLIB(memcpy)(...) and gets the copy of memcpy
 * that was compiled by its own toolchain.
 *
 * On the host build (-DBENCH_HOST) BLIB(fn) is simply fn.
 *
 * BENCH_LIB_SYMBOLS is the list tools/libprefix.py is asked to provide; keep it
 * in sync with the prototypes below.
 */
#ifndef BENCH_LIB_H
#define BENCH_LIB_H

#include "bench_kernel.h"

#define BENCH_LIB_SYMBOLS \
	"memcpy memmove memset memcmp strlen strcmp strchr qsort " \
	"sinf cosf expf logf sqrtf sin cos exp log sqrt"

#ifdef BENCH_HOST

#include <math.h>
#include <stdlib.h>
#include <string.h>
#define BLIB(fn) fn

#else

#define BLIB(fn) BENCH_PASTE2(BENCH_PASTE2(BENCH_VARIANT, lib), fn)

void *BLIB(memcpy)(void *dst, const void *src, size_t n);
void *BLIB(memmove)(void *dst, const void *src, size_t n);
void *BLIB(memset)(void *dst, int c, size_t n);
int BLIB(memcmp)(const void *a, const void *b, size_t n);
size_t BLIB(strlen)(const char *s);
int BLIB(strcmp)(const char *a, const char *b);
char *BLIB(strchr)(const char *s, int c);
void BLIB(qsort)(void *base, size_t n, size_t size,
		 int (*cmp)(const void *, const void *));
float BLIB(sinf)(float x);
float BLIB(cosf)(float x);
float BLIB(expf)(float x);
float BLIB(logf)(float x);
float BLIB(sqrtf)(float x);
double BLIB(sin)(double x);
double BLIB(cos)(double x);
double BLIB(exp)(double x);
double BLIB(log)(double x);
double BLIB(sqrt)(double x);

#endif /* BENCH_HOST */

#endif /* BENCH_LIB_H */
