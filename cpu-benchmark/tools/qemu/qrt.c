/*
 * qrt.c - freestanding platform layer of the qemu-ppc runner (Linux PPC32
 * user mode under qemu-ppc).  No libc: output goes straight to write(2),
 * the arena is a static 64 KB-aligned array, and fatal signals print what was
 * running before the process exits.
 *
 * start.S provides _start, qrt_syscall, qrt_exit and the VSCR accessors.
 */
#include "bench.h"
#include "qrt.h"

long qrt_syscall(long nr, long a, long b, long c, long d);
uint32_t qrt_get_vscr(void);

#define NR_write 4
#define NR_rt_sigaction 173
#define NR_sigaltstack 185

/* ---- output --------------------------------------------------------------- */

static char out_buf[4096];
static uint32_t out_len;

static void write_all(const char *p, uint32_t n)
{
	while (n) {
		long r = qrt_syscall(NR_write, 1, (long)p, (long)n, 0);

		if (r == -4) /* EINTR */
			continue;
		if (r <= 0)
			return;
		p += r;
		n -= (uint32_t)r;
	}
}

void qrt_flush(void)
{
	if (out_len) {
		write_all(out_buf, out_len);
		out_len = 0;
	}
}

void qrt_putc(char c)
{
	if (out_len == sizeof out_buf)
		qrt_flush();
	out_buf[out_len++] = c;
}

/* ---- arena ----------------------------------------------------------------- */

#define QRT_ARENA_TOTAL (BENCH_ARENA_BYTES + BENCH_ARENA_ALIGN)

static unsigned char arena[QRT_ARENA_TOTAL] __attribute__((aligned(BENCH_ARENA_ALIGN)));

unsigned char *qrt_arena(void)
{
	return arena;
}

uint32_t qrt_arena_size(void)
{
	return QRT_ARENA_TOTAL;
}

uint32_t qrt_vscr(void)
{
	return qrt_get_vscr();
}

/* ---- stack high-water mark ------------------------------------------------ */

static uintptr_t stack_top, stack_lo;

static uint32_t stack_pattern(uintptr_t a)
{
	return 0x5717ac4bu ^ (uint32_t)(a * 2654435761u);
}

/* Paints [frame - QRT_STACK_PROBE, frame - 64) below this call's frame,
 * which is at or just below the caller's: everything the caller calls next
 * lives below it.  The 32-bit SysV ABI has no red zone, so the area below
 * r1 is free (fatal signals run on the alternate stack). */
__attribute__((noinline)) void qrt_stack_paint(void)
{
	uintptr_t here = (uintptr_t)__builtin_frame_address(0) & ~(uintptr_t)15;
	volatile uint32_t *p;

	stack_top = here;
	stack_lo = here - QRT_STACK_PROBE;
	for (p = (volatile uint32_t *)stack_lo; (uintptr_t)p < here - 64u; p++)
		*p = stack_pattern((uintptr_t)p);
}

uint32_t qrt_stack_used(void)
{
	const volatile uint32_t *p = (const volatile uint32_t *)stack_lo;

	if (!stack_lo)
		return 0;
	while ((uintptr_t)p < stack_top && *p == stack_pattern((uintptr_t)p))
		p++;
	return (uint32_t)(stack_top - (uintptr_t)p);
}

char *qrt_read_file(const char *path)
{
	(void)path;
	return NULL;
}

/* ---- crash reporting --------------------------------------------------------- */

static const char *ctx_kernel = "-";
static const char *ctx_what = "startup";

void qrt_set_context(const char *kernel, const char *what)
{
	ctx_kernel = kernel;
	ctx_what = what;
}

/* Kernel ABI structures (PPC32). */
struct k_sigaction {
	void (*handler)(int, void *, void *);
	uint32_t flags;
	void (*restorer)(void);
	uint32_t mask[2];
};

struct k_stack {
	void *ss_sp;
	int ss_flags;
	uint32_t ss_size;
};

#define K_SA_SIGINFO 0x00000004u
#define K_SA_ONSTACK 0x08000000u

static unsigned char alt_stack[65536] __attribute__((aligned(16)));

static const char *sig_name(int sig)
{
	switch (sig) {
	case 4: return "SIGILL";
	case 5: return "SIGTRAP";
	case 6: return "SIGABRT";
	case 7: return "SIGBUS";
	case 8: return "SIGFPE";
	case 11: return "SIGSEGV";
	default: return "signal";
	}
}

static void on_fatal_signal(int sig, void *info, void *uc)
{
	const uint32_t *si = (const uint32_t *)info;
	const unsigned char *u = (const unsigned char *)uc;

	qrt_flush();
	qrt_puts("\n*** CRASH: ");
	qrt_puts(sig_name(sig));
	qrt_puts(" (");
	qrt_put_u32((uint32_t)sig);
	qrt_puts(") in ");
	qrt_puts(ctx_kernel);
	qrt_puts(" [");
	qrt_puts(ctx_what);
	qrt_puts("]");
	if (si) {
		/* siginfo: si_signo, si_errno, si_code, si_addr */
		qrt_puts(" addr 0x");
		qrt_put_hex(si[3], 8);
	}
	if (u) {
		/* PPC32 ucontext: uc_regs (struct mcontext *) at offset 48;
		 * mc_gregs[32] = nip, mc_gregs[36] = link. */
		const uint32_t *regs = *(const uint32_t *const *)(const void *)(u + 48);

		if (regs && !((uintptr_t)regs & 3u)) {
			qrt_puts(" pc 0x");
			qrt_put_hex(regs[32], 8);
			qrt_puts(" lr 0x");
			qrt_put_hex(regs[36], 8);
		}
	}
	qrt_puts("\n*** (map pc/lr to a symbol with build/qemu/bench-qemu.map or xenon-addr2line)\n");
	qrt_flush();
	qrt_exit(3);
}

void qrt_install_crash_handlers(void)
{
	static const int sigs[] = { 4, 5, 6, 7, 8, 11 };
	struct k_stack ss;
	unsigned i;

	ss.ss_sp = alt_stack;
	ss.ss_flags = 0;
	ss.ss_size = sizeof alt_stack;
	qrt_syscall(NR_sigaltstack, (long)&ss, 0, 0, 0);

	for (i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
		struct k_sigaction sa;

		sa.handler = on_fatal_signal;
		sa.flags = K_SA_SIGINFO | K_SA_ONSTACK;
		sa.restorer = 0;
		sa.mask[0] = 0;
		sa.mask[1] = 0;
		qrt_syscall(NR_rt_sigaction, sigs[i], (long)&sa, 0, 8);
	}
}
