/*
 * qrt.h - the qemu runner's tiny runtime API.
 *
 * Two implementations:
 *   qrt.c       freestanding, for qemu-ppc (Linux PPC32 user mode): raw
 *               write/exit_group system calls, a static arena, crash handlers;
 *   qrt_host.c  hosted (stdio/malloc), for the host build of the same runner
 *               used by the big-endian/little-endian cross-check.
 * qrunner.c and qselftest.c are written against this API only.
 */
#ifndef QRT_H
#define QRT_H

#include <stddef.h>
#include <stdint.h>

/* ---- output (buffered; qrt_flush() pushes it out) --------------------- */
void qrt_putc(char c);
void qrt_puts(const char *s);
void qrt_put_u32(uint32_t v);                 /* decimal */
void qrt_put_s32(int32_t v);                  /* decimal */
void qrt_put_hex(uint32_t v, int digits);     /* digits 1..8, no prefix */
void qrt_put_hex64(uint64_t v);               /* 16 digits, no prefix */
void qrt_put_double(double d, int sig);       /* rough decimal, d.ddde+XX */
void qrt_put_padded(const char *s, int width);/* left-aligned, space padded */
void qrt_put_dbits(double d);                 /* "0x" + 16 hex digits */
void qrt_flush(void);
void qrt_exit(int status) __attribute__((noreturn));

/* ---- the arena --------------------------------------------------------- */
/* BENCH_ARENA_BYTES + 64 KB, 64 KB-aligned (room for the 4 KB guard). */
unsigned char *qrt_arena(void);
uint32_t qrt_arena_size(void);

/* ---- crash reporting ---------------------------------------------------- */
/* What is running right now; printed if a signal kills the process. */
void qrt_set_context(const char *kernel, const char *what);
void qrt_install_crash_handlers(void);

/* ---- small helpers (no libc) --------------------------------------------- */
size_t qrt_strlen(const char *s);
int qrt_streq(const char *a, const char *b);
int qrt_parse_u32(const char *s, uint32_t *out);
uint64_t qrt_dbits(double d);
double qrt_bitsd(uint64_t b);

/* Stack high-water mark: qrt_stack_paint() fills QRT_STACK_PROBE bytes
 * below the caller's frame with a pattern; qrt_stack_used() returns how many
 * bytes below that frame were written since (0 on the host build). */
#define QRT_STACK_PROBE (128u * 1024u)
void qrt_stack_paint(void);
uint32_t qrt_stack_used(void);

/* VSCR (AltiVec status/control).  0 on the host. */
uint32_t qrt_vscr(void);

/* Host build only: read a whole text file (NUL-terminated), NULL on error. */
char *qrt_read_file(const char *path);

#endif /* QRT_H */
