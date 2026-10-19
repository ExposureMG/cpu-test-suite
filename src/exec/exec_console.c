/* Exec engine, console backend: runs cases through the generated slot (slot.S) and the kernel buffer.
 *
 * Cores (xts/xcore.h): this code runs on the SUITE core (PIR 2) - it prepares the input image, writes the slot word or
 * the kernel (dcbst + sync + icbi here, xts_sync_code), runs the exec hooks - and hands the timed part to the TEST core
 * (PIR 0) through xts_core_xc_call: slot_job() runs there after the executor's sync; isync, reads the C runtime
 * FPSCR/VSCR/VRSAVE, takes the timebase around xts_slot_run and reads the in-case trap record. The 0x600/0x700 stubs
 * record in-case traps in the HV context block of the thread that trapped (SPRG1); that is PIR 0's block, addressed
 * explicitly (xts_xc_test_ctx_real), never the caller's SPRG1. A crash or hang of the test core does not return here
 * (xts_core_xc_call -> xts_core_test_core_lost). xts_touch_range runs on the test core too (it is about ITS caches).
 *
 * Data the two cores share (the input/output images, scratch, the job record, prepared big-buffer regions) is handed over
 * with an explicit dcbf on BOTH sides: the semantic run on hardware showed the test core loading its own stale L1 copy of
 * scratch after the suite had rewritten it and flushed it from the suite core (loads returned what an earlier store case
 * left there, ~50% of all load cases). So a dcbf on the suite core does not reach the test core's L1. The writer flushes
 * its copy, the reader drops its own (stage_job on the test core; the suite after the call) - all outside the timed part.
 * self.cores data_sync_* measures the effect directly. */
#include <stdio.h>
#include <string.h>
#include <ppc/cache.h>
#include <xts/xts.h>
#include <exec/exec_priv.h>
#include <core/core.h>              /* xts_core_xc_call */
#include <plat/xcore_console.h>     /* xts_xc_test_ctx_real, xts_xc_mypir */

extern int xts_slot_run(const void *in, void *out);
extern volatile uint32_t xts_slot_insn[];
extern char xts_slot_end[];
extern void xts_slot_post(void);
extern char xts_vec_stub[], xts_vec_stub_end[];
extern char xts_vec_r1_lo[], xts_vec_r1_hi[], xts_vec_s0[], xts_vec_s2[], xts_vec_s3[], xts_vec_s4[], xts_vec_s5[];
extern char xts_vec_li_out[], xts_vec_b_out[], xts_vec_li_in[];
extern char exc_base[];
extern char ex_program[];   /* exported by the patched except.S (ex_alignment and ex_program are the same label) */

#define KBUF_WORDS 65536                      /* 256 KB: branch-history and I-cache capacity tests (L1 I is 32 KB) */
static uint32_t kbuf[KBUF_WORDS + 64] __attribute__((aligned(128)));
static xts_state_t in_img __attribute__((aligned(128)));
static xts_state_t out_img __attribute__((aligned(128)));
static uint8_t scratch[XTS_SCRATCH_BYTES] __attribute__((aligned(128)));
static int vectors_installed;

int xts_exec_is_host(void) { return 0; }
uint64_t xts_scratch_addr(void) { return (uint32_t)scratch; }
uint64_t xts_exec_big_addr(const void *p) { return (uint32_t)p; }
uint64_t xts_exec_slot_addr(void) { return (uint32_t)xts_slot_insn; }
uint64_t xts_exec_post_addr(void) { return (uint32_t)xts_slot_post; }
uint64_t xts_exec_kernel_addr(void) { return (uint32_t)kbuf; }
uint32_t xts_exec_kernel_capacity(void) { return KBUF_WORDS; }

void xts_sync_code(void *addr, uint32_t len)
{
  memdcbst(addr, (int)len);
  asm volatile("sync" ::: "memory");
  memicbi(addr, (int)len);
  asm volatile("isync" ::: "memory");
}

static void dcbf_here(void *addr, uint32_t len)
{
  asm volatile("sync" ::: "memory");
  memdcbf(addr, (int)len);
  asm volatile("sync" ::: "memory");
}

static void dcbf_job(void *a, void *b) { dcbf_here(a, (uint32_t)b); }

/* out of every cache that can hold it: the suite core's (where the data was usually just written) AND the test core's
 * (a dcbf on one core does not invalidate the other core's L1, see the top of this file) */
void xts_dcbf_range(void *addr, uint32_t len)
{
  dcbf_here(addr, len);
  if (xts_xc_mypir() != 0) xts_core_xc_call(dcbf_job, addr, (void *)len);
}

static void touch_job(void *a, void *b)
{
  volatile uint32_t sink = 0;
  uint32_t len = (uint32_t)b;
  for (uint32_t o = 0; o < len; o += 128) sink += *(const volatile uint32_t *)((const uint8_t *)a + o);
  (void)sink;
}

/* the lines must end up in the TEST core's L1 (and the shared L2): touch them there */
void xts_touch_range(const void *addr, uint32_t len)
{
  if (xts_xc_mypir() == 0) touch_job((void *)addr, (void *)len);
  else xts_core_xc_call(touch_job, (void *)addr, (void *)len);
}

/* PIR 0's HV context block (cached alias: libxenon maps real RAM at EA 0x80000000) */
static volatile uint64_t *trap_ctx(void) { return (volatile uint64_t *)(xts_xc_test_ctx_real() | 0x80000000u); }

static void patch_imm(uint8_t *dst, const char *label, uint32_t value)
{
  uint32_t *p = (uint32_t *)(dst + (label - xts_vec_stub));
  p[0] = (p[0] & 0xFFFF0000u) | (value >> 16);       /* lis 4, hi */
  p[1] = (p[1] & 0xFFFF0000u) | (value & 0xFFFF);    /* ori 4, 4, lo */
}

/* RANGE16: addis 4,3,ha(-lo) ; addi 4,4,lo16(-lo) ; cmplwi 4,size */
static void patch_r16(uint8_t *dst, const char *label, uint32_t lo, uint32_t hi)
{
  uint32_t *p = (uint32_t *)(dst + (label - xts_vec_stub));
  uint32_t v = 0u - lo, size = hi > lo ? hi - lo : 0;
  if (size > 0xFFFFu) size = 0;                      /* xts_exec_trap_range refuses these; never widen silently */
  p[0] = (p[0] & 0xFFFF0000u) | (((v + 0x8000u) >> 16) & 0xFFFFu);
  p[1] = (p[1] & 0xFFFF0000u) | (v & 0xFFFFu);
  p[2] = (p[2] & 0xFFFF0000u) | size;
}

static int install_stub(int vector)
{
  int n = (int)(xts_vec_stub_end - xts_vec_stub);
  if (n > 0x100) { xts_log("exec: vector stub is %d bytes (> 0x100)", n); return -1; }
  uint32_t slot_bytes = (uint32_t)xts_slot_end - (uint32_t)xts_slot_insn;
  if (slot_bytes > 0xFFFFu) { xts_log("exec: slot range is %u bytes (> 64 KB)", (unsigned)slot_bytes); return -1; }
  uint8_t *dst = (uint8_t *)exc_base + vector;
  memcpy(dst, xts_vec_stub, n);
  patch_r16(dst, xts_vec_s0, (uint32_t)xts_slot_insn, (uint32_t)xts_slot_end);
  patch_imm(dst, xts_vec_r1_lo, (uint32_t)kbuf);
  patch_imm(dst, xts_vec_r1_hi, (uint32_t)kbuf + sizeof kbuf);
  const char *extra[XTS_EXEC_EXTRA_RANGES] = { xts_vec_s2, xts_vec_s3, xts_vec_s4, xts_vec_s5 };
  for (int i = 0; i < XTS_EXEC_EXTRA_RANGES; i++)
    patch_r16(dst, extra[i], (uint32_t)xts_exec_ranges[i].lo, (uint32_t)xts_exec_ranges[i].hi);
  *(uint32_t *)(dst + (xts_vec_li_out - xts_vec_stub)) = 0x38600000u | (uint32_t)vector;   /* li 3, vector */
  *(uint32_t *)(dst + (xts_vec_li_in - xts_vec_stub)) = 0x38600000u | (uint32_t)vector;
  uint8_t *b = dst + (xts_vec_b_out - xts_vec_stub);
  *(uint32_t *)b = xts_branch_word((uint32_t)b, (uint32_t)ex_program);
  memdcbf(dst, n);
  asm volatile("sync" ::: "memory");
  memicbi(dst, n);
  asm volatile("isync" ::: "memory");
  return 0;
}

static int reinstall(void) { return vectors_installed ? xts_exec_init() : 0; }

int xts_exec_init(void)
{
  int r = install_stub(0x600) | install_stub(0x700);
  vectors_installed = (r == 0);
  xts_exec_ranges_changed = reinstall;
  return r;
}

static uint32_t rd_fpscr(void)
{
  double f;
  uint64_t v;
  asm volatile("mffs %0" : "=f"(f));
  memcpy(&v, &f, 8);
  return (uint32_t)v;
}

static uint32_t rd_vscr(void)
{
  uint32_t v[4] __attribute__((aligned(16)));
  asm volatile("mfvscr 0\n\tstvx 0, 0, %0" : : "r"(v) : "v0", "memory");
  return v[3];
}

static uint32_t rd_vrsave(void)
{
  uint32_t v;
  asm volatile("mfspr %0, 256" : "=r"(v));
  return v;
}

/* the timed part, on the test core (PIR 0); written there, read by the suite after the call */
typedef struct slot_job {
  uint64_t ticks;
  uint64_t srr0, srr1, dar;
  uint32_t vector;
  uint32_t fpscr_before, fpscr_after, vscr_before, vscr_after, vrsave_before, vrsave_after;
} slot_job_t;
static slot_job_t job __attribute__((aligned(128)));

int xts_exec_runtime_intact(void)
{
  return job.fpscr_before == job.fpscr_after && job.vscr_before == job.vscr_after && job.vrsave_before == job.vrsave_after;
}

/* everything one case hands between the cores */
static void flush_shared(void)
{
  memdcbf(&in_img, sizeof in_img);
  memdcbf(&out_img, sizeof out_img);
  memdcbf(scratch, sizeof scratch);
  memdcbf(&job, sizeof job);
  asm volatile("sync" ::: "memory");
}

/* on the test core, before the pre hooks (a cache-axis touch must come after it): drop its own copies of the shared
 * buffers so the case loads what the suite just wrote */
static void stage_job(void *a, void *b)
{
  (void)a;
  (void)b;
  asm volatile("sync" ::: "memory");
  flush_shared();
}

static void slot_job(void *a, void *b)
{
  slot_job_t *j = a;
  volatile uint64_t *ctx = trap_ctx();
  (void)b;
  ctx[36] = 0;
  ctx[37] = 0;
  ctx[80] = 0;
  ctx[81] = 0;
  asm volatile("sync" ::: "memory");
  j->fpscr_before = rd_fpscr();
  j->vscr_before = rd_vscr();
  j->vrsave_before = rd_vrsave();
  uint64_t t0 = xts_tb();
  xts_slot_run(&in_img, &out_img);
  j->ticks = xts_tb() - t0;
  j->fpscr_after = rd_fpscr();
  j->vscr_after = rd_vscr();
  j->vrsave_after = rd_vrsave();
  j->vector = (uint32_t)ctx[80];
  j->srr0 = ctx[36];
  j->srr1 = ctx[37];
  j->dar = ctx[81];
}

/* run whatever the slot currently holds with x->in / x->mem_in */
static int run_prepared(xts_exec_t *x, uint64_t kernel_start, uint64_t kernel_bytes)
{
  in_img = x->in;
  xts_exec_resolve_code_ptrs(x, &in_img, kernel_start);
  in_img.outptr = (uint32_t)&out_img;
  memset(&out_img, 0, sizeof out_img);
  memcpy(scratch, x->mem_in, XTS_SCRATCH_BYTES);
  memset(&job, 0, sizeof job);
  asm volatile("sync" ::: "memory");
  flush_shared();                                    /* the suite's writes leave its caches */
  if (xts_xc_mypir() != 0) xts_core_xc_call(stage_job, NULL, NULL);   /* the test core drops its stale copies */
  if (xts_exec_hooks_run_pre(x) < 0) return x->status;
  xts_core_xc_call(slot_job, &job, NULL);            /* returns only with XTS_XC_OK (a lost test core never returns) */
  flush_shared();                                    /* the suite drops its copies before reading the results */
  x->ticks = job.ticks;
  x->status = XTS_EXEC_OK;                           /* what the post hooks see */
  xts_exec_hooks_run_post(x);
  memset(&x->trap, 0, sizeof x->trap);
  x->trap.vector = job.vector;
  x->trap.trapped = x->trap.vector != 0;
  if (x->trap.trapped) {
    x->trap.srr0 = job.srr0;
    x->trap.srr1 = job.srr1;
    x->trap.dar = job.dar;
    x->trap.cause = x->trap.vector == 0x700 ? (uint32_t)((x->trap.srr1 >> 16) & 0xFF) : 0;
    uint32_t a = (uint32_t)x->trap.srr0;
    if (a >= (uint32_t)xts_slot_insn && a < (uint32_t)xts_slot_end) {
      x->trap.where = 0;
      x->trap.offset = a - (uint32_t)xts_slot_insn;
    } else if (kernel_bytes && a >= (uint32_t)kernel_start && a < (uint32_t)(kernel_start + kernel_bytes)) {
      x->trap.where = 1;
      x->trap.offset = a - (uint32_t)kernel_start;
    } else if (a >= (uint32_t)kbuf && a < (uint32_t)kbuf + sizeof kbuf) {
      x->trap.where = 1;
      x->trap.offset = a - (uint32_t)kbuf;
    } else {
      int w = xts_exec_trap_classify(a, &x->trap.offset);
      x->trap.where = w >= 0 ? w : 2;
    }
  }
  x->out = out_img;
  memcpy(x->mem_out, scratch, XTS_SCRATCH_BYTES);
  x->status = XTS_EXEC_OK;
  return XTS_EXEC_OK;
}

static uint64_t last_kernel_start, last_kernel_bytes;

int xts_exec_word(xts_exec_t *x, uint32_t word)
{
  if (x->err[0]) { x->status = XTS_EXEC_NOTRUN; return x->status; }
  if (xts_exec_word_assert(x, word) < 0) return x->status;      /* memory word with an EA outside test memory: never run */
  xts_slot_insn[0] = word;
  xts_sync_code((void *)xts_slot_insn, 4);
  last_kernel_start = last_kernel_bytes = 0;
  x->run_word = word;
  x->run_kernel = NULL;
  x->run_kernel_words = 0;
  return run_prepared(x, 0, 0);
}

int xts_exec_kernel(xts_exec_t *x, struct xts_kbuf *k)
{
  if (x->err[0]) { x->status = XTS_EXEC_NOTRUN; return x->status; }
  if (!k || k->err || !k->finished || !k->nexits) {
    snprintf(x->err, sizeof x->err, "kernel not finished or has errors: %s", k ? k->errmsg : "null");
    x->status = XTS_EXEC_BADKERNEL;
    return x->status;
  }
  uint32_t off = xts_env.code_offset & 124;
  if (k->n + off / 4 > KBUF_WORDS) {
    snprintf(x->err, sizeof x->err, "kernel too large (%u words)", (unsigned)k->n);
    x->status = XTS_EXEC_BADKERNEL;
    return x->status;
  }
  uint32_t *dst = kbuf + off / 4;
  for (uint32_t i = 0; i < off / 4; i++) kbuf[i] = 0x60000000u;
  for (uint32_t i = 0; i < k->n; i++)
    dst[i] = k->tag[i] == XK_TAG_EXIT ? xts_branch_word((uint32_t)&dst[i], (uint32_t)xts_slot_post) : k->w[i];
  xts_sync_code(kbuf, (k->n + off / 4) * 4);
  xts_slot_insn[0] = xts_branch_word((uint32_t)xts_slot_insn, (uint32_t)dst);
  xts_sync_code((void *)xts_slot_insn, 4);
  last_kernel_start = (uint32_t)dst;
  last_kernel_bytes = (uint64_t)k->n * 4;
  x->run_word = 0;
  x->run_kernel = k->w;
  x->run_kernel_words = k->n;
  return run_prepared(x, last_kernel_start, last_kernel_bytes);
}

int xts_exec_again(xts_exec_t *x)
{
  return run_prepared(x, last_kernel_start, last_kernel_bytes);
}
