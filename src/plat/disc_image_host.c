/* Host: an image file as a DISC_INTERFACE (512-byte sectors), with injected faults that model the USB mass-storage
 * driver's failure modes (see plat/disc_host.h). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <plat/disc_host.h>

#define SS 512u

static int img_fd = -1, img_ro;
static uint64_t img_sectors;
static xts_fault_cfg_t fc;
static xts_fault_stats_t fs;
static uint64_t rng;
static uint32_t last_write_lba, tag = 0x22112211u;
static uint8_t *tmp;          /* staging for shifted writes */

static uint64_t next_u64(void)
{
  uint64_t z = (rng += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
static double uniform(void) { return (double)(next_u64() >> 11) / 9007199254740992.0; }
static uint32_t below(uint32_t n) { return n ? (uint32_t)(next_u64() % n) : 0; }

static int pio(int write, uint32_t s, uint32_t n, void *b)
{
  size_t bytes = (size_t)n * SS;
  off_t off = (off_t)s * SS;
  ssize_t r = write ? pwrite(img_fd, b, bytes, off) : pread(img_fd, b, bytes, off);
  return r == (ssize_t)bytes;
}

static int armed(void)
{
  if (fc.at) return fs.calls >= fc.at && !fs.injected;
  return fs.calls > fc.after && (!fc.max || fs.injected < fc.max);
}

/* with `at`: pick one of the enabled modes of this call kind (index into the rates), -1 = none enabled */
static int pick(const double *const *rates, int n)
{
  int en[8], k = 0;
  for (int i = 0; i < n; i++)
    if (*rates[i] > 0) en[k++] = i;
  return k ? en[below((uint32_t)k)] : -1;
}

/* does mode i (of n) fire on this call? */
static int fires(const double *const *rates, int n, int i, int *chosen)
{
  if (fc.at) {
    if (*chosen == -2) *chosen = pick(rates, n);
    return *chosen == i;
  }
  return *rates[i] > 0 && uniform() < *rates[i];
}

static void note(const char *what, uint32_t s, uint32_t n, uint32_t k)
{
  fs.injected++;
  if (fc.verbose) fprintf(stderr, "fault #%llu: %s sectors %u+%u (k=%u)\n", (unsigned long long)fs.injected, what, s, n, k);
}

static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void make_cbw(uint8_t cbw[31], uint32_t lba, uint32_t n)
{
  memset(cbw, 0, 31);
  memcpy(cbw, "USBC", 4);
  put_le32(cbw + 4, tag++);
  put_le32(cbw + 8, n * SS);
  cbw[14] = 10;
  cbw[15] = 0x2A;                       /* WRITE(10) */
  cbw[17] = (uint8_t)(lba >> 24); cbw[18] = (uint8_t)(lba >> 16); cbw[19] = (uint8_t)(lba >> 8); cbw[20] = (uint8_t)lba;
  cbw[22] = (uint8_t)(n >> 8); cbw[23] = (uint8_t)n;
}

static int trace = -1;
static void tr(char op, sec_t s, sec_t n)
{
  if (trace < 0) trace = getenv("XTS_DISC_TRACE") != NULL;
  if (trace) fprintf(stderr, "disc %llu %c %u+%u\n", (unsigned long long)fs.calls, op, (unsigned)s, (unsigned)n);
}

static bool d_read(sec_t s, sec_t n, void *buf)
{
  if (img_fd < 0 || !n || (uint64_t)s + n > img_sectors) return false;
  fs.calls++;
  tr('r', s, n);
  fs.reads++;
  if (armed()) {
    const double *const rates[3] = { &fc.short_read, &fc.cbw_read, &fc.read_fail };
    int ch = -2;
    if (fires(rates, 3, 0, &ch)) {
      uint32_t k = below(n);
      note("short_read", s, n, k);
      fs.n_short_read++;
      return k ? pio(0, s, k, buf) : true;
    }
    if (fires(rates, 3, 1, &ch)) {
      uint32_t j = below(n);
      note("cbw_read", s, n, j);
      fs.n_cbw_read++;
      if (!pio(0, s, n, buf)) return false;
      uint8_t *p = (uint8_t *)buf + (size_t)j * SS;
      memcpy(p, "USBS", 4);
      put_le32(p + 4, tag++);
      put_le32(p + 8, 0);
      p[12] = 0;
      return true;
    }
    if (fires(rates, 3, 2, &ch)) {
      uint32_t k = below(n);
      note("read_fail", s, n, k);
      fs.n_read_fail++;
      if (k) pio(0, s, k, buf);
      return false;
    }
  }
  return pio(0, s, n, buf);
}

static bool d_write(sec_t s, sec_t n, const void *buf)
{
  if (img_fd < 0 || img_ro || !n || (uint64_t)s + n > img_sectors) return false;
  fs.calls++;
  fs.writes++;
  tr('w', s, n);
  uint32_t prev = last_write_lba;
  last_write_lba = s;
  if (armed()) {
    const double *const rates[4] = { &fc.torn_write, &fc.cbw_write, &fc.misdirect, &fc.write_fail };
    int ch = -2;
    if (fires(rates, 4, 0, &ch)) {
      uint32_t k = below(n);
      note("torn_write", s, n, k);
      fs.n_torn_write++;
      return k ? pio(1, s, k, (void *)buf) : true;
    }
    if (n <= 256 && fires(rates, 4, 1, &ch)) {
      uint32_t j = below(n);
      note("cbw_write", s, n, j);
      fs.n_cbw_write++;
      size_t bytes = (size_t)n * SS, at = (size_t)j * SS;
      memcpy(tmp, buf, at);
      make_cbw(tmp + at, s, n);
      memcpy(tmp + at + 31, (const uint8_t *)buf + at, bytes - at - 31);
      return pio(1, s, n, tmp);
    }
    if (prev != s && (uint64_t)prev + n <= img_sectors && fires(rates, 4, 2, &ch)) {
      note("misdirect", s, n, prev);
      fs.n_misdirect++;
      return pio(1, prev, n, (void *)buf);
    }
    if (fires(rates, 4, 3, &ch)) {
      uint32_t k = below(n);
      note("write_fail", s, n, k);
      fs.n_write_fail++;
      if (k) pio(1, s, k, (void *)buf);
      return false;
    }
  }
  return pio(1, s, n, (void *)buf);
}

static bool d_true(void) { return img_fd >= 0; }
static s32 d_sectors(void) { return (s32)(img_sectors > 0x7FFFFFFF ? 0x7FFFFFFF : img_sectors); }

static DISC_INTERFACE image_disc = {
  .ioType = 0x58494D47,   /* "XIMG" */
  .features = FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE,
  .startup = d_true,
  .isInserted = d_true,
  .readSectors = d_read,
  .writeSectors = d_write,
  .clearStatus = d_true,
  .shutdown = d_true,
  .sectors = d_sectors,
};

const DISC_INTERFACE *xts_image_disc(void) { return img_fd >= 0 ? &image_disc : NULL; }
void xts_image_fault_stats(xts_fault_stats_t *st) { *st = fs; }

void xts_image_fault_set(const xts_fault_cfg_t *f)
{
  if (f) fc = *f;
  else memset(&fc, 0, sizeof fc);
  rng = fc.seed * 0x2545F4914F6CDD1Dull + 1;
}

int xts_image_disc_open(const char *path, const xts_fault_cfg_t *f, int readonly)
{
  xts_image_disc_close();
  img_ro = readonly;
  image_disc.features = FEATURE_MEDIUM_CANREAD | (readonly ? 0 : FEATURE_MEDIUM_CANWRITE);
  img_fd = open(path, readonly ? O_RDONLY : O_RDWR);
  if (img_fd < 0) return -1;
  struct stat st;
  if (fstat(img_fd, &st) < 0 || st.st_size < (off_t)SS) { close(img_fd); img_fd = -1; return -1; }
  img_sectors = (uint64_t)st.st_size / SS;
  if (!tmp) tmp = malloc((size_t)256 * SS);
  memset(&fs, 0, sizeof fs);
  last_write_lba = 0;
  xts_image_fault_set(f);
  return 0;
}

void xts_image_disc_close(void)
{
  if (img_fd >= 0) close(img_fd);
  img_fd = -1;
}

/* ---- fault spec parser ---- */
static int set_rate(xts_fault_cfg_t *c, const char *k, double v)
{
  struct { const char *n; double *p; } t[] = {
    { "short_read", &c->short_read }, { "cbw_read", &c->cbw_read }, { "torn_write", &c->torn_write },
    { "cbw_write", &c->cbw_write }, { "misdirect", &c->misdirect }, { "read_fail", &c->read_fail },
    { "write_fail", &c->write_fail },
  };
  for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
    if (!strcmp(k, t[i].n)) { *t[i].p = v; return 0; }
  return -1;
}

int xts_fault_parse(const char *spec, xts_fault_cfg_t *c)
{
  memset(c, 0, sizeof *c);
  c->seed = 1;
  if (!spec || !*spec || !strcmp(spec, "none")) return 0;
  char buf[512];
  snprintf(buf, sizeof buf, "%s", spec);
  char *save = NULL;
  for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
    char *eq = strchr(tok, '='), *colon = strchr(tok, ':');
    if (!eq && colon) {                         /* preset:rate */
      *colon = 0;
      double r = strtod(colon + 1, NULL);
      if (!strcmp(tok, "unpatched")) { c->short_read = c->cbw_read = c->torn_write = c->cbw_write = r; }
      else if (!strcmp(tok, "desync")) { c->short_read = c->cbw_read = c->torn_write = c->cbw_write = c->misdirect = r; }
      else if (!strcmp(tok, "patched")) { c->read_fail = c->write_fail = r; }
      else return -1;
      continue;
    }
    if (!eq) return -1;
    *eq = 0;
    const char *v = eq + 1;
    if (!strcmp(tok, "seed")) c->seed = strtoull(v, NULL, 0);
    else if (!strcmp(tok, "after")) c->after = (uint32_t)strtoul(v, NULL, 0);
    else if (!strcmp(tok, "max")) c->max = (uint32_t)strtoul(v, NULL, 0);
    else if (!strcmp(tok, "at")) c->at = (uint32_t)strtoul(v, NULL, 0);
    else if (!strcmp(tok, "verbose")) c->verbose = atoi(v);
    else if (set_rate(c, tok, strtod(v, NULL)) < 0) return -1;
  }
  return 0;
}
