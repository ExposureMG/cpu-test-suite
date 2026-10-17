/* Disc guard (src/plat/disc_guard.c) against an in-memory device that misbehaves on request: double-read verification,
 * write read-back, the poison latch, statistics, and the raw-sector API for crash reports. */
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include <plat/disc_guard.h>
#include "htest.h"

#define MSEC 4096
static uint8_t *mem;
static int short_reads, fail_reads, torn_writes, fail_writes, flaky_reads;   /* remaining misbehaviours */
static unsigned reads, writes;

static bool m_read(sec_t s, sec_t n, void *b)
{
  if (s + n > MSEC) return false;
  reads++;
  if (fail_reads) { fail_reads--; return false; }
  if (short_reads) { short_reads--; memcpy(b, mem + (size_t)s * 512, (size_t)(n / 2) * 512); return true; }   /* stale tail */
  memcpy(b, mem + (size_t)s * 512, (size_t)n * 512);
  if (flaky_reads) { flaky_reads--; ((uint8_t *)b)[7] ^= 0x40; }       /* one flipped bit, reported as success */
  return true;
}
static bool m_write(sec_t s, sec_t n, const void *b)
{
  if (s + n > MSEC) return false;
  writes++;
  if (fail_writes) { fail_writes--; return false; }
  if (torn_writes) { torn_writes--; memcpy(mem + (size_t)s * 512, b, (size_t)(n / 2) * 512); return true; }
  memcpy(mem + (size_t)s * 512, b, (size_t)n * 512);
  return true;
}
static bool m_true(void) { return true; }
static s32 m_sectors(void) { return MSEC; }
static DISC_INTERFACE mdev = { 1, FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE, m_true, m_true, m_read, m_write, m_true,
                               m_true, m_sectors };

static char notified[400];
static void note(const char *m) { snprintf(notified, sizeof notified, "%s", m); }

static void setup(void)
{
  if (!mem) mem = malloc((size_t)MSEC * 512);
  for (size_t i = 0; i < (size_t)MSEC * 512; i++) mem[i] = (uint8_t)(i * 7 + (i >> 9));
  short_reads = fail_reads = torn_writes = fail_writes = flaky_reads = 0;
  reads = writes = 0;
  notified[0] = 0;
  xts_disc_guard_init(&mdev, 512, 1);
  xts_disc_set_notify(note);
}

HTEST(storage_guard_reads)
{
  static uint8_t buf[200 * 512];
  setup();
  const DISC_INTERFACE *g = xts_disc_guard_iface();
  HT_CHECK(g && g->readSectors(10, 64, buf));
  HT_CHECK(!memcmp(buf, mem + 10 * 512, 64 * 512));
  HT_EQ_U64(reads, 2);                                  /* read twice */
  /* a short read that reports success: the pair disagrees, the retry pair agrees -> correct data, not poisoned */
  short_reads = 1;
  memset(buf, 0, sizeof buf);
  HT_CHECK(g->readSectors(100, 64, buf));
  HT_CHECK(!memcmp(buf, mem + 100 * 512, 64 * 512));
  HT_CHECK(!xts_disc_poisoned());
  xts_storage_status_t st;
  xts_disc_status(&st);
  HT_EQ_U64(st.read_retries, 1);
  HT_EQ_U64(st.sectors_read, 128);
  /* larger than one chunk: split into 64-sector device calls */
  reads = 0;
  HT_CHECK(g->readSectors(300, 150, buf));
  HT_CHECK(!memcmp(buf, mem + 300 * 512, 150 * 512));
  HT_EQ_U64(reads, 6);
  /* a bit flip on one of the two reads, twice in a row: fails and latches */
  flaky_reads = 1;
  HT_CHECK(g->readSectors(5, 8, buf));                  /* once: repaired by the retry */
  HT_CHECK(!xts_disc_poisoned());
  short_reads = 2;                                      /* both reads of a pair equally short: the fill patterns differ */
  HT_CHECK(g->readSectors(30, 8, buf) && !memcmp(buf, mem + 30 * 512, 8 * 512));
  short_reads = 4;
  HT_CHECK(!g->readSectors(20, 8, buf));                /* both pairs bad -> error */
  HT_CHECK(xts_disc_poisoned());
  xts_disc_status(&st);
  HT_CHECK(!strcmp(st.op, "read") && st.sector == 20 && st.count == 8);
  HT_CHECK(strstr(notified, "STORAGE ERROR: read of sectors 20-27") && strstr(notified, "Stop using this stick"));
  /* reads still work after the latch, writes are refused */
  HT_CHECK(g->readSectors(10, 4, buf));
  unsigned w0 = writes;
  HT_CHECK(!g->writeSectors(10, 4, buf));
  HT_EQ_U64(writes, w0);
  xts_disc_status(&st);
  HT_EQ_U64(st.writes_refused, 1);
}

HTEST(storage_guard_writes)
{
  static uint8_t buf[64 * 512], back[64 * 512];
  setup();
  const DISC_INTERFACE *g = xts_disc_guard_iface();
  for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i ^ 0x5C);
  HT_CHECK(g->writeSectors(200, 64, buf));
  HT_CHECK(!memcmp(mem + 200 * 512, buf, sizeof buf));
  HT_EQ_U64(writes, 1);
  HT_EQ_U64(reads, 1);                                  /* read back once */
  /* a torn write reported as success: the read-back differs -> error + latch */
  torn_writes = 1;
  buf[0] ^= 1;
  buf[sizeof buf - 1] ^= 1;
  HT_CHECK(!g->writeSectors(300, 64, buf));
  HT_CHECK(xts_disc_poisoned());
  xts_storage_status_t st;
  xts_disc_status(&st);
  HT_CHECK(!strcmp(st.op, "verify") && st.sector == 300);
  HT_CHECK(!g->writeSectors(400, 1, buf));              /* refused */
  HT_CHECK(g->readSectors(200, 64, back) && !memcmp(back, mem + 200 * 512, sizeof back));
  /* a device-reported write error latches too */
  setup();
  fail_writes = 1;
  HT_CHECK(!xts_disc_guard_iface()->writeSectors(50, 2, buf));
  xts_disc_status(&st);
  HT_CHECK(st.poisoned && !strcmp(st.op, "write"));
  /* pass-through mode (guard off): no verification, no latch */
  xts_disc_guard_init(&mdev, 512, 0);
  reads = writes = 0;
  torn_writes = 1;
  HT_CHECK(xts_disc_guard_iface()->writeSectors(60, 4, buf));
  HT_EQ_U64(reads, 0);
  HT_CHECK(!xts_disc_poisoned());
  xts_disc_status(&st);
  HT_CHECK(!st.guard && st.sectors_written == 4);
}

HTEST(storage_guard_raw_marked)
{
  static uint8_t sec[512], data[512], back[512];
  static const char magic[9] = "XTSCRSH1";         /* 8 bytes used */
  setup();
  /* slot 700 carries the magic at offset 16; slot 701 does not */
  memcpy(mem + 700 * 512 + 16, magic, 8);
  memset(data, 0x33, sizeof data);
  memcpy(data + 16, magic, 8);
  HT_EQ_U64(xts_disc_write_marked(701, data, magic, 8, 16, 0), (uint64_t)(int64_t)XTS_DISC_ENOMAGIC);
  HT_CHECK(mem[701 * 512] != 0x33);
  memset(sec, 0x44, sizeof sec);                        /* new contents without the magic: refused */
  HT_EQ_U64(xts_disc_write_marked(700, sec, magic, 8, 16, 0), (uint64_t)(int64_t)XTS_DISC_EINVAL);
  HT_EQ_U64(xts_disc_write_marked(700, data, magic, 8, 510, 0), (uint64_t)(int64_t)XTS_DISC_EINVAL);
  HT_EQ_U64(xts_disc_write_marked(700, data, magic, 8, 16, 0), 0);
  HT_CHECK(!memcmp(mem + 700 * 512, data, 512));
  HT_EQ_U64(xts_disc_read(700, 1, back), 0);
  HT_CHECK(!memcmp(back, data, 512));
  /* the slot keeps its magic: a second write works */
  data[100] = 0x77;
  HT_EQ_U64(xts_disc_write_marked(700, data, magic, 8, 16, 0), 0);
  HT_EQ_U64(mem[700 * 512 + 100], 0x77);
  /* a lost write of unchanged contents cannot be told apart from a good one (and needs not be) */
  torn_writes = 1;
  HT_EQ_U64(xts_disc_write_marked(700, data, magic, 8, 16, 0), 0);
  HT_EQ_U64(mem[700 * 512 + 100], 0x77);
  /* a lost write of new contents: EVERIFY and the latch */
  data[100] = 0x78;
  torn_writes = 1;
  HT_EQ_U64(xts_disc_write_marked(700, data, magic, 8, 16, 0), (uint64_t)(int64_t)XTS_DISC_EVERIFY);
  HT_CHECK(xts_disc_poisoned());
  /* poisoned: refused unless the caller insists */
  HT_EQ_U64(xts_disc_write_marked(700, data, magic, 8, 16, 0), (uint64_t)(int64_t)XTS_DISC_EPOISONED);
  HT_EQ_U64(xts_disc_write_marked(700, data, magic, 8, 16, XTS_DISC_WM_ALLOW_POISONED), 0);
  HT_EQ_U64(mem[700 * 512 + 100], 0x78);
  HT_EQ_U64(xts_disc_sector_size(), 512);
}
