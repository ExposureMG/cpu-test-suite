/*
 * qrt_host.c - hosted (x86-64 Linux) platform layer for the host build of
 * the qemu runner and of qselftest.c: stdio output, a malloc'ed 64 KB-aligned
 * arena, crash context on stderr via signal(), main() -> qrt_main().
 */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bench.h"
#include "qrt.h"

int qrt_main(int argc, char **argv);

void qrt_putc(char c)
{
	putchar(c);
}

void qrt_flush(void)
{
	fflush(stdout);
}

void qrt_exit(int status)
{
	fflush(stdout);
	exit(status);
}

#define QRT_ARENA_TOTAL (BENCH_ARENA_BYTES + BENCH_ARENA_ALIGN)

static unsigned char *arena;

unsigned char *qrt_arena(void)
{
	if (!arena) {
		void *p = NULL;

		if (posix_memalign(&p, BENCH_ARENA_ALIGN, QRT_ARENA_TOTAL) != 0 || !p) {
			fprintf(stderr, "qrt_host: cannot allocate the arena\n");
			exit(2);
		}
		arena = p;
	}
	return arena;
}

uint32_t qrt_arena_size(void)
{
	return QRT_ARENA_TOTAL;
}

uint32_t qrt_vscr(void)
{
	return 0;
}

void qrt_stack_paint(void)
{
}

uint32_t qrt_stack_used(void)
{
	return 0;
}

static const char *ctx_kernel = "-";
static const char *ctx_what = "startup";

void qrt_set_context(const char *kernel, const char *what)
{
	ctx_kernel = kernel;
	ctx_what = what;
}

static void on_fatal_signal(int sig)
{
	char buf[256];
	int n;

	fflush(stdout);
	n = snprintf(buf, sizeof buf, "\n*** CRASH: signal %d in %s [%s]\n", sig, ctx_kernel, ctx_what);
	if (n > 0)
		(void)!write(1, buf, (size_t)n);
	_exit(3);
}

void qrt_install_crash_handlers(void)
{
	signal(SIGSEGV, on_fatal_signal);
	signal(SIGILL, on_fatal_signal);
	signal(SIGBUS, on_fatal_signal);
	signal(SIGFPE, on_fatal_signal);
	signal(SIGABRT, on_fatal_signal);
}

char *qrt_read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long len;

	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)len + 1);
	if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[len] = 0;
	fclose(f);
	return buf;
}

int main(int argc, char **argv)
{
	int r = qrt_main(argc, argv);

	fflush(stdout);
	return r;
}
