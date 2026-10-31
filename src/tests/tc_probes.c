/* tc.probes - the libxenon-toolchain hardware correctness probes (libxenon-toolchain tools/xts-probes, README.md there)
 * inside XTS: one boot answers them and the answers arrive on the PC as records.
 *
 * Cases, one per probe group, in the order the standalone xts_probes.elf prints them:
 *   vmul    the -O3 replacement sequences for the illegal AltiVec integer multiplies against scalar mullw: one record per
 *           kernel, case_id "vmul/<kernel>", metric count = mismatching results of 1024, pass iff 0 (fail otherwise,
 *           and fail for a kernel that reported nothing)
 *   est     fres / frsqrte / vrefp / vrsqrtefp and the 128 forms: raw input/output bits      \  info records
 *   recip   the -mrecip=all float/double results (+ the estimate each sequence started from)   > "<group>#<part>",
 *   vpkd3d  the vD words vpkd3d128 writes for every (type, pack, slot)                       /  metric count = lines
 * Every record's detail: group, part, parts, line0 (index of its first line within the group), nlines, lines (the raw
 * tab-separated result lines of xts_probes.elf, no newline), fpscr/vscr (the test core's, when the probes started).
 * Records stay below ~8 KB of detail (a REC message may be 32 KB). libxenon-toolchain tools/xts_probes_check.py rebuilds
 * results.txt from the records of a run directory, byte for byte, and checks it.
 *
 * The probe objects are compiled by the NEW toolchain (-m32 -mcpu=xenon: VMX128 and the multiply sequences under test,
 * which the old compiler cannot emit; -m32 is the old ELF32 SysV ABI) and linked into this ELF when the Makefile finds
 * TC_GCC (block "tc.probes"; it defines XTS_TC_PROBES for this file). They run on the test core (PIR 0) through
 * xts_core_xc_call - a crash or hang there becomes a crash/hang record for the case, like any case - and write their
 * lines into a RAM buffer; the suite turns the buffer into records after the call. FPSCR and VSCR of the test core are
 * restored afterwards. Host builds (nothing executes) and console builds without the probe objects emit one skip
 * record per case. */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include <xts/json.h>

#if defined(XTS_CONSOLE) && defined(XTS_TC_PROBES)
#define TCP_LINKED 1
#include <core/core.h>              /* xts_core_xc_call (as self.cores): a lost test core gives the case's crash/hang record */
#include <xts_probes.h>             /* libxenon-toolchain tools/xts-probes (-I from the Makefile) */
#else
#define TCP_LINKED 0
#endif

enum { G_VMUL, G_EST, G_RECIP, G_VPKD3D, G_N };
static const char *const grp_name[G_N] = { "vmul", "est", "recip", "vpkd3d" };

#define TCP_PART_BYTES 6000          /* JSON-escaped line text per record (the rest of a record is < 1 KB) */
#define TCP_MAX_LINES 1024           /* lines of one group (est has 384) */

static int tcp_enum(xts_caselist_t *cl)
{
  for (int g = 0; g < G_N; g++) {
    xts_case_t *c = xts_case_add(cl, grp_name[g]);
    if (!c) return -1;
    c->arg[0] = g;
  }
  return 0;
}

static void tcp_skip(xts_ctx_t *ctx, const char *why)
{
  char det[192];
  xts_json_t j;
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  xj_kv_str(&j, "why", why);
  xj_end(&j);
  xts_record_t r;
  xts_record_init(&r);
  r.metric = "count";
  r.unit = "count";
  r.status = XTS_ST_SKIP;
  r.detail = xj_finish(&j);
  r.detail_truncated = j.truncated;
  xts_emit(ctx, &r);
}

#if TCP_LINKED
_Static_assert(XTS_PROBES_VMUL == 1u << G_VMUL && XTS_PROBES_EST == 1u << G_EST && XTS_PROBES_RECIP == 1u << G_RECIP &&
               XTS_PROBES_VPKD3D == 1u << G_VPKD3D, "group bits of xts_probes.h");

/* the kernels probe_vmul.c defines, to notice one that reported nothing */
static const char *const vmul_kernels[] = {
#define K(name, ta, tb, expr) #name,
#include <probe_vmul_kernels.def>
#undef K
};
#define NKERNELS ((int)(sizeof vmul_kernels / sizeof vmul_kernels[0]))

/* shared with the test core (handed over with xts_dcbf_range on both sides, as the exec engine does) */
static struct tcp_job {
  uint32_t groups;
  uint32_t len, nlines, overflow;
  uint32_t fpscr, vscr;
  char buf[96 * 1024];               /* all four groups together print ~33 KB */
} job __attribute__((aligned(128)));

/* test core: append one line (the output callback of xts_probes_run_groups) */
static void tcp_out(const char *line, void *ctx)
{
  struct tcp_job *jb = ctx;
  size_t n = strlen(line);
  if (jb->len + n + 1 > sizeof jb->buf) {
    jb->overflow++;
    return;
  }
  memcpy(jb->buf + jb->len, line, n);
  jb->buf[jb->len + n] = '\n';
  jb->len += (uint32_t)n + 1;
  jb->nlines++;
}

/* test core (PIR 0): run the groups with the C runtime's FPSCR/VSCR (libxenon's VSCR[NJ]), restore both afterwards */
static void tcp_job_fn(void *a, void *b)
{
  (void)b;
  struct tcp_job *jb = a;
  double fs;
  __vector unsigned int vs;
  __asm__ volatile("mffs %0" : "=f"(fs));
  __asm__ volatile("mfvscr %0" : "=v"(vs));
  uint64_t fsb;
  uint32_t vsw[4] __attribute__((aligned(16)));
  memcpy(&fsb, &fs, 8);
  memcpy(vsw, &vs, 16);
  jb->fpscr = (uint32_t)fsb;
  jb->vscr = vsw[3];
  xts_probes_run_groups(jb->groups, tcp_out, jb);
  __asm__ volatile("mtfsf 0xff,%0" : : "f"(fs));
  __asm__ volatile("mtvscr %0" : : "v"(vs));
  __asm__ volatile("sync" ::: "memory");
}

/* split a line into tab-separated fields (in place); returns the count */
static int tcp_fields(char *s, char **f, int max)
{
  int n = 0;
  while (n < max) {
    f[n++] = s;
    char *t = strchr(s, '\t');
    if (!t) break;
    *t = 0;
    s = t + 1;
  }
  return n;
}

/* bytes a line costs inside the "lines" array: quotes, comma, one extra byte per escaped tab/quote/backslash */
static int tcp_cost(const char *l)
{
  int n = 3;
  for (; *l; l++) n += (*l == '\t' || *l == '"' || *l == '\\') ? 2 : 1;
  return n;
}

static void tcp_head(xts_json_t *j, int g, int part, int parts, int line0, int nlines)
{
  xj_obj(j);
  xj_kv_str(j, "group", grp_name[g]);
  xj_kv_int(j, "part", part);
  xj_kv_int(j, "parts", parts);
  xj_kv_int(j, "line0", line0);
  xj_kv_int(j, "nlines", nlines);
  xj_kv_hex(j, "fpscr", job.fpscr, 8);
  xj_kv_hex(j, "vscr", job.vscr, 8);
}

static void tcp_lines(xts_json_t *j, const char *const *lines, int n)
{
  xj_key(j, "lines");
  xj_arr(j);
  for (int i = 0; i < n; i++) xj_str(j, lines[i]);
  xj_end(j);
}

static void tcp_emit(xts_ctx_t *ctx, const char *case_id, double value, int status, xts_json_t *j)
{
  xts_record_t r;
  xts_record_init(&r);
  r.case_id = case_id;
  r.metric = "count";
  r.unit = "count";
  r.value = value;
  r.status = status;
  r.detail = xj_finish(j);
  r.detail_truncated = j->truncated;
  xts_emit(ctx, &r);
}

static void tcp_error(xts_ctx_t *ctx, int g, const char *why, int nlines)
{
  static char det[512];
  xts_json_t j;
  xj_init(&j, det, sizeof det);
  tcp_head(&j, g, 0, 0, 0, nlines);
  xj_kv_str(&j, "error", why);
  xj_end(&j);
  tcp_emit(ctx, NULL, nlines, XTS_ST_FAIL, &j);
}

static int tcp_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  static const char *lines[TCP_MAX_LINES];
  static char lcopy[XTS_PROBES_LINE_MAX];
  static char det[16384];
  int g = (int)c->arg[0];
  if (ctx->host || xts_exec_is_host()) {
    tcp_skip(ctx, "host: the probes run only on the console");
    return 0;
  }
  memset(&job, 0, offsetof(struct tcp_job, buf));
  job.groups = 1u << g;
  xts_dcbf_range(&job, sizeof job);
  xts_core_xc_call(tcp_job_fn, &job, NULL);         /* returns only with XTS_XC_OK */
  xts_dcbf_range(&job, sizeof job);                 /* the suite reads what the test core wrote */
  if (job.overflow) {
    tcp_error(ctx, g, "line buffer overflow (tc_probes.c job.buf)", (int)job.nlines);
    return 0;
  }
  /* the lines (NUL-terminated in place); every one must belong to this group */
  int n = 0;
  for (uint32_t o = 0; o < job.len && n < TCP_MAX_LINES;) {
    char *l = job.buf + o, *e = memchr(l, '\n', job.len - o);
    if (!e) break;
    *e = 0;
    o = (uint32_t)(e - job.buf) + 1;
    size_t gl = strlen(grp_name[g]);
    if (strncmp(l, grp_name[g], gl) || l[gl] != '\t') {
      tcp_error(ctx, g, "a result line of another group", n);
      return 0;
    }
    lines[n++] = l;
  }
  if (n != (int)job.nlines) {
    tcp_error(ctx, g, "line count does not match the buffer (more than TCP_MAX_LINES?)", (int)job.nlines);
    return 0;
  }
  if (n == 0) {
    tcp_error(ctx, g, "no result lines", 0);
    return 0;
  }
  xts_json_t j;
  char cid[48];
  if (g == G_VMUL) {
    /* vmul <kernel> <n> <mismatches> <first index> <got> <want>: one record per kernel */
    int seen[NKERNELS];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < n; i++) {
      char *f[8];
      snprintf(lcopy, sizeof lcopy, "%s", lines[i]);
      int nf = tcp_fields(lcopy, f, 8);
      long long cnt = -1, bad = -1, first = -1, got = 0, want = 0;
      int ok = nf == 7 && sscanf(f[2], "%lld", &cnt) == 1 && sscanf(f[3], "%lld", &bad) == 1 &&
               sscanf(f[4], "%lld", &first) == 1 && sscanf(f[5], "%lld", &got) == 1 && sscanf(f[6], "%lld", &want) == 1;
      for (int k = 0; ok && k < NKERNELS; k++)
        if (!strcmp(f[1], vmul_kernels[k])) seen[k]++;
      snprintf(cid, sizeof cid, "vmul/%s", nf > 1 ? f[1] : "?");
      xj_init(&j, det, sizeof det);
      tcp_head(&j, g, i, n, i, 1);
      xj_kv_str(&j, "kernel", nf > 1 ? f[1] : "");
      xj_kv_int(&j, "n", cnt);
      xj_kv_int(&j, "mismatches", bad);
      xj_kv_int(&j, "first", first);
      xj_kv_int(&j, "got", got);
      xj_kv_int(&j, "want", want);
      if (!ok) xj_kv_str(&j, "error", "unparsable vmul line");
      tcp_lines(&j, &lines[i], 1);
      xj_end(&j);
      tcp_emit(ctx, cid, ok ? (double)bad : NAN, ok && bad == 0 && cnt > 0 ? XTS_ST_PASS : XTS_ST_FAIL, &j);
    }
    for (int k = 0; k < NKERNELS; k++)
      if (seen[k] != 1) {
        snprintf(cid, sizeof cid, "vmul/%s", vmul_kernels[k]);
        xj_init(&j, det, sizeof det);
        tcp_head(&j, g, 0, 0, 0, 0);
        xj_kv_str(&j, "kernel", vmul_kernels[k]);
        xj_kv_str(&j, "error", seen[k] ? "kernel reported more than once" : "kernel reported nothing");
        xj_end(&j);
        tcp_emit(ctx, cid, NAN, XTS_ST_FAIL, &j);
      }
    return 0;
  }
  /* est / recip / vpkd3d: consecutive lines, at most TCP_PART_BYTES of escaped text per record */
  static int start[TCP_MAX_LINES + 1];
  int parts = 0;
  for (int i = 0, used = 0; i < n; i++) {
    int cost = tcp_cost(lines[i]);
    if (i == 0 || used + cost > TCP_PART_BYTES) {
      start[parts++] = i;
      used = 0;
    }
    used += cost;
  }
  start[parts] = n;
  for (int p = 0; p < parts; p++) {
    int a = start[p], m = start[p + 1] - a;
    snprintf(cid, sizeof cid, "%s#%d", grp_name[g], p);
    xj_init(&j, det, sizeof det);
    tcp_head(&j, g, p, parts, a, m);
    tcp_lines(&j, &lines[a], m);
    xj_end(&j);
    tcp_emit(ctx, cid, m, j.truncated ? XTS_ST_FAIL : XTS_ST_INFO, &j);
  }
  return 0;
}
#else
static int tcp_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  (void)c;
  if (ctx->host || xts_exec_is_host()) tcp_skip(ctx, "host: the probes run only on the console");
  else tcp_skip(ctx, "built without the probe objects (Makefile: TC_GCC not found)");
  return 0;
}
#endif

XTS_FAMILY(tc_probes, .kind = "probe", .enumerate = tcp_enum, .run = tcp_run, .block_cases = 1,
           .desc = "libxenon-toolchain hardware probes (tools/xts-probes): -O3 vmul sequences vs mullw, estimate bits, "
                   "-mrecip results, vpkd3d128 word layout");
