/*
 * qrt_fmt.c - portable formatting and string helpers of the qemu runner
 * runtime (no libc).  Linked into both the qemu-ppc and the host build, so
 * both print exactly the same text.  The platform provides qrt_putc().
 */
#include "qrt.h"

void qrt_puts(const char *s)
{
	while (*s)
		qrt_putc(*s++);
}

void qrt_put_u32(uint32_t v)
{
	char buf[12];
	int n = 0;

	do {
		buf[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v);
	while (n)
		qrt_putc(buf[--n]);
}

void qrt_put_s32(int32_t v)
{
	if (v < 0) {
		qrt_putc('-');
		qrt_put_u32(0u - (uint32_t)v);
	} else {
		qrt_put_u32((uint32_t)v);
	}
}

void qrt_put_hex(uint32_t v, int digits)
{
	static const char hex[] = "0123456789abcdef";
	int i;

	if (digits < 1)
		digits = 1;
	if (digits > 8)
		digits = 8;
	for (i = digits - 1; i >= 0; i--)
		qrt_putc(hex[(v >> (4 * i)) & 15u]);
}

void qrt_put_hex64(uint64_t v)
{
	qrt_put_hex((uint32_t)(v >> 32), 8);
	qrt_put_hex((uint32_t)v, 8);
}

void qrt_put_dbits(double d)
{
	qrt_puts("0x");
	qrt_put_hex64(qrt_dbits(d));
}

void qrt_put_padded(const char *s, int width)
{
	int n = (int)qrt_strlen(s);

	qrt_puts(s);
	while (n++ < width)
		qrt_putc(' ');
}

/*
 * Rough decimal: sig significant digits (1..16), "d.ddde+XX".  Scaling by
 * powers of ten costs a few ulps, which is fine for display: exact values are
 * always printed as bit patterns next to it.
 */
void qrt_put_double(double d, int sig)
{
	static const double p10[9] = { 1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64, 1e128, 1e256 };
	static const double n10[9] = { 1e-1, 1e-2, 1e-4, 1e-8, 1e-16, 1e-32, 1e-64, 1e-128, 1e-256 };
	uint64_t b = qrt_dbits(d);
	uint32_t ex = (uint32_t)(b >> 52) & 0x7ffu;
	int e10 = 0, k, i;
	double x, half;

	if (sig < 1)
		sig = 1;
	if (sig > 16)
		sig = 16;
	if (b >> 63)
		qrt_putc('-');
	if (ex == 0x7ffu) {
		qrt_puts((b << 12) ? "nan" : "inf");
		return;
	}
	x = qrt_bitsd(b & ~((uint64_t)1 << 63));
	if (x == 0.0) {
		qrt_putc('0');
		if (sig > 1) {
			qrt_putc('.');
			for (i = 1; i < sig; i++)
				qrt_putc('0');
		}
		qrt_puts("e+00");
		return;
	}
	if (x >= 10.0) {
		for (k = 8; k >= 0; k--)
			if (x >= p10[k]) {
				x *= n10[k];
				e10 += 1 << k;
			}
	} else if (x < 1.0) {
		for (k = 8; k >= 0; k--)
			if (x < n10[k]) {
				x *= p10[k];
				e10 -= 1 << k;
			}
		if (x < 1.0) {
			x *= 10.0;
			e10--;
		}
	}
	/* x is now about [1, 10); round to sig digits. */
	half = 5.0;
	for (i = 0; i < sig; i++)
		half *= 0.1;
	x += half;
	if (x >= 10.0) {
		x *= 0.1;
		e10++;
	}
	if (x < 1.0) /* scaling error just below 1.0 */
		x = 1.0;
	for (i = 0; i < sig; i++) {
		int dgt = (int)x;

		if (dgt > 9)
			dgt = 9;
		if (dgt < 0)
			dgt = 0;
		qrt_putc((char)('0' + dgt));
		if (i == 0 && sig > 1)
			qrt_putc('.');
		x = (x - dgt) * 10.0;
	}
	qrt_putc('e');
	if (e10 < 0) {
		qrt_putc('-');
		e10 = -e10;
	} else {
		qrt_putc('+');
	}
	if (e10 < 10)
		qrt_putc('0');
	qrt_put_u32((uint32_t)e10);
}

size_t qrt_strlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

int qrt_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

int qrt_parse_u32(const char *s, uint32_t *out)
{
	uint32_t v = 0;

	if (!*s)
		return 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9' || v > 429496728u)
			return 0;
		v = v * 10u + (uint32_t)(*s - '0');
	}
	*out = v;
	return 1;
}

uint64_t qrt_dbits(double d)
{
	union {
		double d;
		uint64_t u;
	} c;

	c.d = d;
	return c.u;
}

double qrt_bitsd(uint64_t b)
{
	union {
		double d;
		uint64_t u;
	} c;

	c.u = b;
	return c.d;
}
