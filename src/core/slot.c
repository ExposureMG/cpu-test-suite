/* Report files on the stick: crash.json (64 KB) and progress.bin (one sector), pre-allocated per run at a block
 * boundary while storage is healthy, then written only raw, one verified sector at a time (see core.h "crash reports").
 *
 * Layout of every sector (S = device sector size, 512..4096):
 *   byte 0          '{' (sector 0) or ','
 *   bytes 1..29     "slot_NNN":"<magic>"        magic = 16 chars at XTS_SLOT_MAGIC_OFF (13), new at every arming
 *   bytes 30..      members  ,"key":value  (a member never crosses a sector), then spaces
 *   byte S-2        '}' in the file's last sector, else a space;   byte S-1  '\n'
 * so the file is one JSON object whatever subset of sectors holds members. A raw write only replaces a sector that
 * still holds the magic (xts_plat_raw_write), and only with a sector carrying it too.
 *
 * Arming (xts_slots_arm): write both templates through libfat (stdio), fsync, xts_plat_storage_flush, locate their
 * sectors (xts_plat_file_extent; crash.json must be one contiguous run: after a fragmented allocation the file is removed,
 * a filler file of the same size takes those clusters, and crash.json is written again - up to 4 times; the fillers are
 * deleted afterwards),
 * read sector 0 of each back raw and check the magic, then keep paths/extents/magic in RAM with a checksum that the crash
 * path verifies before it writes anything. libfat may still hold clean cache pages over these sectors; a page written
 * back at a later boundary can restore an older progress.bin sector, which is why the breadcrumb is rewritten at every
 * block start (after the boundary flush). The crash path writes crash.json and stops: libfat never runs again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "core.h"
#include <plat/disc_guard.h>      /* XTS_DISC_* codes */

#define ARM_TRIES 4

typedef struct slotfile {
  char path[320];
  uint32_t first, nsec;
} slotfile_t;

static struct {
  slotfile_t crash, prog;
  char magic[XTS_SLOT_MAGIC_LEN + 1];
  uint32_t ss;
  int armed;
  uint32_t sum;                   /* over everything above (xts_slots_armed) */
} S;
static uint32_t used;             /* crash.json sectors written raw this boot (to disarm after a thread report) */

static uint8_t tmpl[XTS_CRASH_JSON_BYTES];     /* template image (arming / disarm) */
static uint8_t crumb_img[XTS_DISC_MAX_SECTOR];
static uint8_t rd_sec[XTS_DISC_MAX_SECTOR];
char xts_prev_boot_how[200] = "not determined";

static uint32_t fnv(const void *p, size_t n, uint32_t h)
{
  const uint8_t *b = p;
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
  return h;
}
static uint32_t meta_sum(void) { return fnv(&S, offsetof(__typeof__(S), sum), 2166136261u); }
int xts_slots_armed(void) { return S.armed && S.sum == meta_sum(); }
const char *xts_slot_crash_path(void) { return S.crash.path; }
const char *xts_slot_progress_path(void) { return S.prog.path; }
const char *xts_slot_magic(void) { return S.magic; }
uint32_t xts_slot_sector_size(void) { return S.ss; }
uint32_t xts_slot_crash_sectors(void) { return S.crash.nsec; }

/* ---- sector packer (crash-path safe) ---- */
static void put_dec3(uint8_t *o, uint32_t v)
{
  o[0] = (uint8_t)('0' + v / 100 % 10);
  o[1] = (uint8_t)('0' + v / 10 % 10);
  o[2] = (uint8_t)('0' + v % 10);
}

static void sector_template(uint8_t *s, uint32_t ss, uint32_t idx, int last, const char *magic)
{
  for (uint32_t i = 0; i < ss; i++) s[i] = ' ';
  s[0] = idx ? ',' : '{';
  memcpy(s + 1, "\"slot_", 6);
  put_dec3(s + 7, idx);
  memcpy(s + 10, "\":\"", 3);
  memcpy(s + XTS_SLOT_MAGIC_OFF, magic, XTS_SLOT_MAGIC_LEN);
  s[XTS_SLOT_MAGIC_OFF + XTS_SLOT_MAGIC_LEN] = '"';
  if (last) s[ss - 2] = '}';
  s[ss - 1] = '\n';
}

void xts_spack_init(xts_spack_t *p, uint8_t *img, uint32_t ss, uint32_t nsec, const char *magic)
{
  p->img = img;
  p->ss = ss;
  p->nsec = nsec;
  p->cur = 0;
  p->pos = XTS_SLOT_HDR;
  p->magic = magic;
  p->dropped = 0;
  for (uint32_t i = 0; i < nsec; i++) sector_template(img + (size_t)i * ss, ss, i, i + 1 == nsec, magic);
}

int xts_spack_put(xts_spack_t *p, const char *m, uint32_t len)
{
  uint32_t lim = p->ss - 2;                 /* S-2: '}' (last sector) or a space, S-1: '\n' */
  if (len > lim - XTS_SLOT_HDR) { p->dropped++; return -1; }
  if (p->pos + len > lim) {
    if (p->cur + 1 >= p->nsec) { p->dropped++; return -1; }
    p->cur++;
    p->pos = XTS_SLOT_HDR;
  }
  memcpy(p->img + (size_t)p->cur * p->ss + p->pos, m, len);
  p->pos += len;
  return 0;
}

uint32_t xts_spack_used(const xts_spack_t *p) { return p->cur + 1; }

/* ---- small crash-safe text helpers ---- */
static uint32_t cat(char *o, uint32_t n, uint32_t cap, const char *s)
{
  while (*s && n + 1 < cap) o[n++] = *s++;
  o[n] = 0;
  return n;
}

/* ---- arming ---- */
static int write_file(const char *path, const uint8_t *data, size_t n)
{
  xts_fat_busy++;
  FILE *f = fopen(path, "wb");
  int ok = 0;
  if (f) {
    size_t w = fwrite(data, 1, n, f);
    int fl = fflush(f), sy = fsync(fileno(f)), cl = fclose(f);
    ok = w == n && fl == 0 && sy == 0 && cl == 0;
  }
  xts_fat_busy--;
  return ok ? 0 : -1;
}

static void make_magic(const char *run_id, int boot)
{
  static uint32_t salt;
  uint32_t h = fnv(run_id, strlen(run_id), 2166136261u);
  uint64_t tb = xts_tb();
  h = fnv(&tb, sizeof tb, h);
  h = fnv(&boot, sizeof boot, h);
  salt += 0x9E3779B9u;
  h ^= salt;
  uint32_t h2 = fnv(&h, sizeof h, (uint32_t)(tb >> 7) * 2654435761u);
  static const char hex[] = "0123456789abcdef";
  memcpy(S.magic, "XTS1", 4);
  for (int i = 0; i < 8; i++) S.magic[4 + i] = hex[(h >> (28 - 4 * i)) & 15];
  for (int i = 0; i < 4; i++) S.magic[12 + i] = hex[(h2 >> (12 - 4 * i)) & 15];
  S.magic[XTS_SLOT_MAGIC_LEN] = 0;
}

/* the armed crash.json: sector 0 says "armed" */
static void crash_template(uint8_t *img, const char *run_id, int boot)
{
  xts_spack_t p;
  xts_spack_init(&p, img, S.ss, S.crash.nsec, S.magic);
  char m[300];
  int n = snprintf(m, sizeof m, ",\"xts\":\"crash report slot (filled by the crash path, read at the next boot)\","
                   "\"state\":\"armed\",\"run_id\":\"%s\",\"boot\":%d", run_id, boot);
  xts_spack_put(&p, m, (uint32_t)n);
}

static int locate(slotfile_t *f, uint32_t want)
{
  uint32_t first = 0, nsec = 0;
  int r = xts_plat_file_extent(f->path, &first, &nsec);
  if (r < 0) return r;
  if (nsec < want) return -3;
  f->first = first;
  f->nsec = want;
  return 0;
}

/* read sector 0 back raw: the extent really is this file */
static int verify_magic(const slotfile_t *f)
{
  int r = xts_plat_raw_read(f->path, f->first, 0, rd_sec);
  if (r) return r;
  return memcmp(rd_sec + XTS_SLOT_MAGIC_OFF, S.magic, XTS_SLOT_MAGIC_LEN) ? XTS_DISC_ENOMAGIC : 0;
}

int xts_slots_arm(const char *rundir, const char *run_id, int boot)
{
  uint64_t t0 = xts_tb();
  S.armed = 0;
  used = 0;
  S.sum = 0;
  if (xts_storage_dead()) return -1;
  S.ss = xts_plat_sector_size();
  if (S.ss < 512 || S.ss > XTS_DISC_MAX_SECTOR) S.ss = 512;
  make_magic(run_id, boot);
  xts_path(S.crash.path, sizeof S.crash.path, rundir, "crash.json", NULL, NULL);
  xts_path(S.prog.path, sizeof S.prog.path, rundir, "progress.bin", NULL, NULL);
  S.crash.nsec = XTS_CRASH_JSON_BYTES / S.ss;
  S.prog.nsec = 1;
  /* progress.bin: one sector (always one cluster: contiguous) */
  xts_spack_t p;
  xts_spack_init(&p, crumb_img, S.ss, 1, S.magic);
  char m[300];
  int n = snprintf(m, sizeof m, ",\"xts\":\"breadcrumb (written before cases)\",\"state\":\"armed\",\"run_id\":\"%s\",\"boot\":%d",
                   run_id, boot);
  xts_spack_put(&p, m, (uint32_t)n);
  const char *why = NULL;
  char fillers[ARM_TRIES][340];
  int nfill = 0, r = 0;
  if (write_file(S.prog.path, crumb_img, S.ss) < 0) why = "cannot write progress.bin";
  for (int attempt = 0; !why && attempt < ARM_TRIES; attempt++) {
    crash_template(tmpl, run_id, boot);
    if (write_file(S.crash.path, tmpl, XTS_CRASH_JSON_BYTES) < 0) { why = "cannot write crash.json"; break; }
    xts_fat_busy++;
    int fr = xts_plat_storage_flush();
    xts_fat_busy--;
    if (fr) { why = "fatFlush failed"; break; }
    r = locate(&S.crash, S.crash.nsec);
    if (r != -2) break;
    /* fragmented: give that allocation to a filler file of the same size (it keeps the holes occupied) and allocate
     * crash.json again after it */
    char nm[40];
    snprintf(nm, sizeof nm, "slotfill%d.tmp", attempt);
    xts_path(fillers[nfill], sizeof fillers[0], rundir, nm, NULL, NULL);
    xts_fat_busy++;
    int rm = remove(S.crash.path);
    xts_fat_busy--;
    if (rm != 0 || write_file(fillers[nfill], tmpl, XTS_CRASH_JSON_BYTES) < 0) { why = "crash.json is fragmented and the retry failed"; break; }
    nfill++;
    xts_log("slots: crash.json allocation not contiguous (attempt %d): filler %s, allocating again", attempt + 1, nm);
  }
  if (!why && r == -2) why = "crash.json could not be allocated contiguously";
  else if (!why && r == -1) why = "the file system cannot locate files (fatFileExtent missing)";
  else if (!why && r == -3) why = "crash.json is shorter than expected";
  if (!why && locate(&S.prog, 1) < 0) why = "cannot locate progress.bin";
  if (!why) {
    int v = verify_magic(&S.crash);
    if (!v) v = verify_magic(&S.prog);
    if (v) why = v == XTS_DISC_ENOMAGIC ? "raw read-back of the slot sectors does not show the magic" : "raw read-back failed";
  }
  for (int i = 0; i < nfill; i++) {
    xts_fat_busy++;
    remove(fillers[i]);
    xts_fat_busy--;
  }
  if (nfill) {
    xts_fat_busy++;
    xts_plat_storage_flush();
    xts_fat_busy--;
  }
  double ms = (double)(xts_tb() - t0) * 1000.0 / (double)xts_tb_hz();
  if (why) {
    xts_screen_error("crash report slots NOT armed: %s (a crash will not be reported in crash.json; progress.bin "
                     "breadcrumbs off; the journal still marks the block)", why);
    return -1;
  }
  S.armed = 1;
  S.sum = meta_sum();
  xts_log("slots: armed in %.1f ms: crash.json sectors %lu-%lu (%lu x %lu bytes), progress.bin sector %lu, magic %s", ms,
          (unsigned long)S.crash.first, (unsigned long)(S.crash.first + S.crash.nsec - 1), (unsigned long)S.crash.nsec,
          (unsigned long)S.ss, (unsigned long)S.prog.first, S.magic);
  return 0;
}

/* ---- raw writes ---- */
static int raw_write(const slotfile_t *f, uint32_t idx, const uint8_t *sec)
{
  return xts_plat_raw_write(f->path, f->first, idx, sec, S.magic, XTS_SLOT_MAGIC_LEN, XTS_SLOT_MAGIC_OFF);
}

int xts_slot_crash_write(const uint8_t *img, uint32_t n)
{
  if (!xts_slots_armed()) return XTS_DISC_EINIT;
  if (n < 1 || n > S.crash.nsec) return XTS_DISC_EINVAL;
  used = n > used ? n : used;
  for (uint32_t i = 1; i < n; i++) {
    int r = raw_write(&S.crash, i, img + (size_t)i * S.ss);
    if (r) return r;
  }
  return raw_write(&S.crash, 0, img);          /* sector 0 ("state":"crashed") last: the commit */
}

int xts_slot_crash_disarm(void)
{
  if (!xts_slots_armed() || !used) return 0;
  crash_template(tmpl, xts_crumb.run_id, xts_crumb.boot);
  int r = raw_write(&S.crash, 0, tmpl);         /* sector 0 first: "armed" again, the rest is ignored from here on */
  for (uint32_t i = 1; !r && i < used; i++) r = raw_write(&S.crash, i, tmpl + (size_t)i * S.ss);
  if (!r) used = 0;
  return r;
}

/* ---- breadcrumb (progress.bin) ---- */
#define NKIND 8
static const char *const kinds[NKIND] = { "self", "semantic", "timing", "sched", "mem", "smt", "calib", "other" };
static struct { uint64_t n, ticks, max, fails; } kst[NKIND], blk;

static int kind_idx(const char *k)
{
  for (int i = 0; i < NKIND - 1; i++)
    if (!strcmp(k, kinds[i])) return i;
  return NKIND - 1;
}

/* JSON string member (escaped, crash-safe) */
static uint32_t mstr(char *o, uint32_t n, uint32_t cap, const char *key, const char *v)
{
  n = cat(o, n, cap, ",\"");
  n = cat(o, n, cap, key);
  n = cat(o, n, cap, "\":\"");
  for (; v && *v && n + 8 < cap; v++) {
    unsigned char c = (unsigned char)*v;
    if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = (char)c; }
    else if (c < 0x20 || c >= 0x7F) o[n++] = '?';
    else o[n++] = (char)c;
  }
  o[n] = 0;
  return cat(o, n, cap, "\"");
}
static uint32_t mnum(char *o, uint32_t n, uint32_t cap, const char *key, int64_t v)
{
  char t[24];
  int k = 0;
  uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u && k < 22);
  if (v < 0) t[k++] = '-';
  n = cat(o, n, cap, ",\"");
  n = cat(o, n, cap, key);
  n = cat(o, n, cap, "\":");
  while (k && n + 1 < cap) o[n++] = t[--k];
  o[n] = 0;
  return n;
}

int xts_slot_crumb_write(void)
{
  if (!xts_slots_armed()) return XTS_DISC_EINIT;
  const xts_crumb_t *c = &xts_crumb;
  static char m[XTS_DISC_MAX_SECTOR];
  uint32_t cap = S.ss - XTS_SLOT_HDR - 2, n = 0;
  if (cap > sizeof m) cap = sizeof m;
  m[0] = 0;
  n = mstr(m, n, cap, "state", c->phase == XTS_PH_CASE ? "case" : xts_phase_name(c->phase));
  n = mstr(m, n, cap, "run_id", c->run_id);
  n = mnum(m, n, cap, "boot", c->boot);
  n = mstr(m, n, cap, "block", c->block);
  n = mnum(m, n, cap, "case_rel", c->case_rel);
  n = mnum(m, n, cap, "case_index", c->case_index);
  n = mnum(m, n, cap, "repeat", c->repeat);
  n = mstr(m, n, cap, "case_id", c->case_id);
  n = mstr(m, n, cap, "family", c->family);
  n = mstr(m, n, cap, "kind", c->kind);
  n = mstr(m, n, cap, "mnem", c->mnem);
  n = mnum(m, n, cap, "every", c->every);
  n = mnum(m, n, cap, "ncase", c->ncase);
  n = mnum(m, n, cap, "seq", (int64_t)c->seq);
  n = mnum(m, n, cap, "tb", (int64_t)xts_tb());
  xts_spack_t p;
  xts_spack_init(&p, crumb_img, S.ss, 1, S.magic);
  xts_spack_put(&p, m, n);
  uint64_t t0 = xts_tb();
  int r = raw_write(&S.prog, 0, crumb_img);
  uint64_t dt = xts_tb() - t0;
  int k = kind_idx(c->kind);
  kst[k].n++;
  kst[k].ticks += dt;
  if (dt > kst[k].max) kst[k].max = dt;
  if (r) kst[k].fails++;
  blk.n++;
  blk.ticks += dt;
  if (dt > blk.max) blk.max = dt;
  return r;
}

static double tms(uint64_t t) { return (double)t * 1000.0 / (double)xts_tb_hz(); }

void xts_slot_crumb_block_stats(uint64_t *n, double *ms, double *max_ms)
{
  *n = blk.n;
  *ms = tms(blk.ticks);
  *max_ms = tms(blk.max);
  memset(&blk, 0, sizeof blk);
}

void xts_slot_crumb_stats(char *out, size_t cap)
{
  size_t o = 0;
  out[0] = 0;
  for (int i = 0; i < NKIND && o < cap; i++) {
    if (!kst[i].n) continue;
    int w = snprintf(out + o, cap - o, "%s%s n=%llu total %.1f ms avg %.3f ms max %.3f ms fails %llu", o ? "; " : "", kinds[i],
                     (unsigned long long)kst[i].n, tms(kst[i].ticks), tms(kst[i].ticks) / (double)kst[i].n, tms(kst[i].max),
                     (unsigned long long)kst[i].fails);
    if (w > 0) o += (size_t)w;
  }
  if (!o) snprintf(out, cap, "none");
}

/* ---- reading the previous boot's files ---- */
size_t xts_json_squeeze(const char *in, size_t n, char *out, size_t cap)
{
  size_t o = 0;
  int str = 0, esc = 0;
  for (size_t i = 0; i < n && in[i] && o + 1 < cap; i++) {
    char c = in[i];
    if (str) {
      out[o++] = c;
      if (esc) esc = 0;
      else if (c == '\\') esc = 1;
      else if (c == '"') str = 0;
      continue;
    }
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    /* drop "slot_NNN":"...", members (and the comma before / after them) */
    if (c == '"' && !strncmp(in + i, "\"slot_", 6) && i + 11 < n && in[i + 9] == '"' && in[i + 10] == ':') {
      size_t j = i + 11;
      if (in[j] == '"') {
        j++;
        while (j < n && in[j] && in[j] != '"') j++;
        j++;
      }
      while (j < n && (in[j] == ' ' || in[j] == '\n' || in[j] == '\r' || in[j] == '\t')) j++;
      if (j < n && in[j] == ',') j++;
      else if (o && out[o - 1] == ',') o--;
      i = j - 1;
      continue;
    }
    if (c == '"') str = 1;
    out[o++] = c;
  }
  out[o] = 0;
  return o;
}

int xts_json_get(const char *json, const char *key, char *out, size_t cap)
{
  char pat[80];
  snprintf(pat, sizeof pat, "\"%s\":", key);
  const char *p = strstr(json, pat);
  if (!p || !cap) return -1;
  p += strlen(pat);
  size_t o = 0;
  if (*p == '"') {
    for (p++; *p && *p != '"' && o + 1 < cap; p++) {
      if (*p == '\\' && p[1]) p++;
      out[o++] = *p;
    }
  } else {
    while (*p && *p != ',' && *p != '}' && o + 1 < cap) out[o++] = *p++;
  }
  out[o] = 0;
  return 0;
}

static int geti(const char *j, const char *k, int def)
{
  char v[32];
  return xts_json_get(j, k, v, sizeof v) ? def : atoi(v);
}

int xts_prev_load(const char *rundir, const char *run_id, xts_prev_t *p)
{
  static char raw[XTS_CRASH_JSON_BYTES + 1];
  static char ctext[XTS_CRASH_JSON_BYTES + 1], ptext[XTS_DISC_MAX_SECTOR + 1];
  char path[340], v[64];
  memset(p, 0, sizeof *p);
  p->crash_text = ctext;
  p->crumb_text = ptext;
  ctext[0] = ptext[0] = 0;
  p->crash_case_index = p->crash_case_rel = p->crumb_case_index = p->crumb_case_rel = -1;
  int n = xts_read_small(xts_path(path, sizeof path, rundir, "crash.json", NULL, NULL), raw, sizeof raw);
  int have_crash_file = n > 0;
  if (n > 0) {
    xts_json_squeeze(raw, (size_t)n, ctext, sizeof ctext);
    if (!xts_json_get(ctext, "state", v, sizeof v) && !strcmp(v, "crashed") && !xts_json_get(ctext, "run_id", v, sizeof v) &&
        !strcmp(v, run_id)) {
      p->crash = 1;
      xts_json_get(ctext, "kind", p->crash_kind, sizeof p->crash_kind);
      xts_json_get(ctext, "block", p->crash_block, sizeof p->crash_block);
      xts_json_get(ctext, "case_id", p->crash_case_id, sizeof p->crash_case_id);
      xts_json_get(ctext, "phase", p->crash_phase, sizeof p->crash_phase);
      xts_json_get(ctext, "outcome", p->crash_outcome, sizeof p->crash_outcome);
      xts_json_get(ctext, "vector", p->crash_vector, sizeof p->crash_vector);
      xts_json_get(ctext, "srr0", p->crash_srr0, sizeof p->crash_srr0);
      p->crash_case_index = geti(ctext, "case_index", -1);
      p->crash_case_rel = geti(ctext, "case_rel", -1);
      for (int i = 1; i <= XTS_CRASH_THREADS; i++) {
        char k[24];
        snprintf(k, sizeof k, "thr%d_vector", i);
        if (!xts_json_get(ctext, k, v, sizeof v)) p->nthr++;
      }
    }
  }
  n = xts_read_small(xts_path(path, sizeof path, rundir, "progress.bin", NULL, NULL), raw, sizeof raw);
  int have_prog_file = n > 0;
  if (n > 0) {
    xts_json_squeeze(raw, (size_t)n, ptext, sizeof ptext);
    if (!xts_json_get(ptext, "run_id", v, sizeof v) && !strcmp(v, run_id) && !xts_json_get(ptext, "state", v, sizeof v) &&
        strcmp(v, "armed")) {
      p->crumb = 1;
      snprintf(p->crumb_phase, sizeof p->crumb_phase, "%s", v);
      xts_json_get(ptext, "block", p->crumb_block, sizeof p->crumb_block);
      xts_json_get(ptext, "case_id", p->crumb_case_id, sizeof p->crumb_case_id);
      p->crumb_case_index = geti(ptext, "case_index", -1);
      p->crumb_case_rel = geti(ptext, "case_rel", -1);
      p->crumb_every = geti(ptext, "every", 0);
      p->crumb_boot = geti(ptext, "boot", 0);
    }
  }
  if (p->crash && !strcmp(p->crash_kind, "thread"))
    snprintf(p->how, sizeof p->how, "crash.json filled (crash of %d other hardware thread(s) during block %s); progress.bin: %s",
             p->nthr, p->crash_block[0] ? p->crash_block : "-", p->crumb ? p->crumb_phase : "no breadcrumb");
  else if (p->crash)
    snprintf(p->how, sizeof p->how, "crash.json filled (%s crash, vector %s, block %s, case %s)", p->crash_kind,
             p->crash_vector, p->crash_block[0] ? p->crash_block : "-", p->crash_case_id[0] ? p->crash_case_id : "-");
  else if (p->crumb)
    snprintf(p->how, sizeof p->how, "crash.json %s; progress.bin: %s in block %s case %s", have_crash_file ? "armed (no crash report)" : "missing",
             p->crumb_phase, p->crumb_block, p->crumb_case_id[0] ? p->crumb_case_id : "-");
  else
    snprintf(p->how, sizeof p->how, "no report: crash.json %s, progress.bin %s", have_crash_file ? "armed" : "missing",
             have_prog_file ? "without a breadcrumb of this run" : "missing");
  return p->crash || p->crumb;
}
