/*
 * qblib.c - stand-ins for the BLIB() routines (include/bench_lib.h) in the
 * qemu-ppc build.
 *
 * The real A/B image links each toolchain's own newlib through
 * tools/libprefix.py; that code is built for the Xenon and cannot run on a
 * G4.  Here one simple, correct implementation serves both namespaces:
 * xenon3_lib_<fn> and old16_lib_<fn> are aliases of qblib_<fn>.  So LIB
 * kernels check the calling kernel code (both compilers) but not newlib.
 *
 * The plain memcpy/memmove/memset/memcmp names are exported too, for calls
 * GCC itself may emit (struct copies, -Os); the kernels must not need them
 * in the real build, and the qemu check reports objects that reference them.
 *
 * -DQLG_HOST builds only the qblib_ names (host unit test).
 */
#include <stddef.h>
#include <stdint.h>

#include "qmath.h"

typedef int (*qblib_cmp)(const void *, const void *);

/* Word accesses to memory of any effective type. */
typedef uint32_t __attribute__((may_alias)) qblib_w32;

void *qblib_memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0) {
		while (n >= 4) {
			*(qblib_w32 *)(void *)d = *(const qblib_w32 *)(const void *)s;
			d += 4;
			s += 4;
			n -= 4;
		}
	}
	while (n--)
		*d++ = *s++;
	return dst;
}

void *qblib_memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	if (d == s || n == 0)
		return dst;
	if (d < s || d >= s + n)
		return qblib_memcpy(dst, src, n);
	/* overlapping, dst above src: copy backwards */
	d += n;
	s += n;
	while (n--)
		*--d = *--s;
	return dst;
}

void *qblib_memset(void *dst, int c, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	unsigned char b = (unsigned char)c;

	if (n >= 8) {
		uint32_t w = b * 0x01010101u;

		while (((uintptr_t)d & 3u) && n) {
			*d++ = b;
			n--;
		}
		while (n >= 4) {
			*(qblib_w32 *)(void *)d = w;
			d += 4;
			n -= 4;
		}
	}
	while (n--)
		*d++ = b;
	return dst;
}

int qblib_memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;

	for (; n; n--, p++, q++)
		if (*p != *q)
			return (int)*p - (int)*q;
	return 0;
}

size_t qblib_strlen(const char *s)
{
	const char *p = s;

	while (*p)
		p++;
	return (size_t)(p - s);
}

int qblib_strcmp(const char *a, const char *b)
{
	const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;

	while (*p && *p == *q) {
		p++;
		q++;
	}
	return (int)*p - (int)*q;
}

char *qblib_strchr(const char *s, int c)
{
	char ch = (char)c;

	for (;; s++) {
		if (*s == ch)
			return (char *)s;
		if (!*s)
			return NULL;
	}
}

/* qsort: heapsort (in place, O(n log n), no recursion, any element size). */
static void swap_elems(unsigned char *a, unsigned char *b, size_t size)
{
	if ((((uintptr_t)a | (uintptr_t)b | size) & 3u) == 0) {
		for (; size; size -= 4, a += 4, b += 4) {
			uint32_t t = *(qblib_w32 *)(void *)a;

			*(qblib_w32 *)(void *)a = *(qblib_w32 *)(void *)b;
			*(qblib_w32 *)(void *)b = t;
		}
		return;
	}
	for (; size; size--, a++, b++) {
		unsigned char t = *a;

		*a = *b;
		*b = t;
	}
}

static void sift_down(unsigned char *base, size_t root, size_t n, size_t size, qblib_cmp cmp)
{
	for (;;) {
		size_t child = 2 * root + 1;

		if (child >= n)
			return;
		if (child + 1 < n && cmp(base + child * size, base + (child + 1) * size) < 0)
			child++;
		if (cmp(base + root * size, base + child * size) >= 0)
			return;
		swap_elems(base + root * size, base + child * size, size);
		root = child;
	}
}

void qblib_qsort(void *base, size_t n, size_t size, qblib_cmp cmp)
{
	unsigned char *b = (unsigned char *)base;
	size_t i;

	if (n < 2 || size == 0)
		return;
	for (i = n / 2; i-- > 0;)
		sift_down(b, i, n, size, cmp);
	for (i = n - 1; i > 0; i--) {
		swap_elems(b, b + i * size, size);
		sift_down(b, 0, i, size, cmp);
	}
}

#ifndef QLG_HOST

#define QBLIB_ALIAS(ret, fn, args)                                         \
	ret xenon3_lib_##fn args __attribute__((alias("qblib_" #fn)));     \
	ret old16_lib_##fn args __attribute__((alias("qblib_" #fn)));

QBLIB_ALIAS(void *, memcpy, (void *, const void *, size_t))
QBLIB_ALIAS(void *, memmove, (void *, const void *, size_t))
QBLIB_ALIAS(void *, memset, (void *, int, size_t))
QBLIB_ALIAS(int, memcmp, (const void *, const void *, size_t))
QBLIB_ALIAS(size_t, strlen, (const char *))
QBLIB_ALIAS(int, strcmp, (const char *, const char *))
QBLIB_ALIAS(char *, strchr, (const char *, int))
QBLIB_ALIAS(void, qsort, (void *, size_t, size_t, qblib_cmp))

/* The math stand-ins live in qmath.c; aliases must be defined next to their
 * target, so these are thin wrappers instead. */
#define QBLIB_WRAP(ret, fn, type)                                          \
	ret xenon3_lib_##fn(type x) { return qm_##fn(x); }                 \
	ret old16_lib_##fn(type x) { return qm_##fn(x); }

QBLIB_WRAP(float, sinf, float)
QBLIB_WRAP(float, cosf, float)
QBLIB_WRAP(float, expf, float)
QBLIB_WRAP(float, logf, float)
QBLIB_WRAP(float, sqrtf, float)
QBLIB_WRAP(double, sin, double)
QBLIB_WRAP(double, cos, double)
QBLIB_WRAP(double, exp, double)
QBLIB_WRAP(double, log, double)
QBLIB_WRAP(double, sqrt, double)

/* Calls GCC may generate on its own. */
void *memcpy(void *, const void *, size_t) __attribute__((alias("qblib_memcpy")));
void *memmove(void *, const void *, size_t) __attribute__((alias("qblib_memmove")));
void *memset(void *, int, size_t) __attribute__((alias("qblib_memset")));
int memcmp(const void *, const void *, size_t) __attribute__((alias("qblib_memcmp")));

#endif /* !QLG_HOST */
