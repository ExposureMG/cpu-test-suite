/* Transport: the wire format (src/core/netfmt.c), the in-RAM results store, and end-to-end runs through the real
 * receiver (tools/xts_recv.py): every record the sink emits arrives once, in order, byte for byte; injected loss shows up
 * exactly in receive.json; duplicated, corrupted and reordered frames are absorbed. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include <xts/net.h>
#include <core/core.h>
#include <core/netfmt.h>
#include "htest.h"

HTEST(net_fmt)
{
  HT_EQ_U64(xts_crc32(0, "123456789", 9), 0xCBF43926u);
  HT_EQ_U64(xts_crc32(xts_crc32(0, "1234", 4), "56789", 5), 0xCBF43926u);
  HT_EQ_U64(xts_fnv1a64(XTS_FNV64_INIT, "", 0), 0xcbf29ce484222325ull);
  HT_EQ_U64(xts_fnv1a64(XTS_FNV64_INIT, "a", 1), 0xaf63dc4c8601ec8cull);
  HT_EQ_U64(xts_netfmt_frag_count(0), 1);
  HT_EQ_U64(xts_netfmt_frag_count(1400), 1);
  HT_EQ_U64(xts_netfmt_frag_count(1401), 2);
  HT_EQ_U64(xts_netfmt_frag_count(32768), 24);
  HT_EQ_U64(xts_netfmt_frag_count(32769), 0);
  HT_EQ_U64(xts_netfmt_frag_len(1401, 0), 1400);
  HT_EQ_U64(xts_netfmt_frag_len(1401, 1), 1);
  HT_EQ_U64(XTS_NET_MAX_FRAME, 1478);
  HT_CHECK(!strcmp(xts_net_type_name(XTS_PK_CRUMB), "CRUMB") && !strcmp(xts_net_type_name(9), "?"));
  /* header: big-endian fields, CRC over header 0..31 + payload */
  xts_net_hdr_t h = { .type = XTS_PK_REC, .flags = 1, .copy = 1, .nonce = 0x0102030405060708ull, .seq = 0x11223344,
                      .msg = 0x55667788, .frag_idx = 2, .frag_cnt = 3, .len = 5 };
  uint8_t b[XTS_NET_HDR_BYTES + 8];
  HT_EQ_U64(xts_netfmt_udp_payload(b, &h, "hello"), XTS_NET_HDR_BYTES + 5);
  HT_CHECK(!memcmp(b, "XTS1\x01\x02\x01\x01\x01\x02\x03\x04\x05\x06\x07\x08\x11\x22\x33\x44\x55\x66\x77\x88\x00\x02\x00\x03"
                      "\x00\x05\x00\x00", 32));
  uint32_t crc = xts_crc32(xts_crc32(0, b, 32), "hello", 5);
  HT_EQ_U64((uint32_t)b[32] << 24 | b[33] << 16 | b[34] << 8 | b[35], crc);
  xts_net_hdr_t p;
  HT_EQ_U64(xts_netfmt_parse(b, sizeof b - 3, &p), 0);
  HT_CHECK(p.seq == h.seq && p.msg == h.msg && p.nonce == h.nonce && p.len == 5 && p.frag_idx == 2 && p.frag_cnt == 3);
  b[XTS_NET_HDR_BYTES + 1] ^= 1;
  HT_EQ_U64(xts_netfmt_parse(b, sizeof b - 3, &p), (uint64_t)-2);
  b[0] = 'Y';
  HT_EQ_U64(xts_netfmt_parse(b, sizeof b - 3, &p), (uint64_t)-1);
  /* Ethernet frame: broadcast, IPv4 169.254.mac4.mac5 -> 255.255.255.255, DF, valid header checksum, UDP 36360, padding */
  uint8_t f[XTS_NET_MAX_FRAME];
  static const uint8_t mac[6] = { 0x00, 0x22, 0x48, 0xAA, 0xBB, 0xCC };
  h.len = 1;
  uint32_t n = xts_netfmt_eth_frame(f, mac, 0x1234, &h, "x");
  HT_EQ_U64(n, 14 + 20 + 8 + 36 + 1);
  HT_CHECK(!memcmp(f, "\xff\xff\xff\xff\xff\xff\x00\x22\x48\xaa\xbb\xcc\x08\x00", 14));
  HT_CHECK(f[14] == 0x45 && f[20] == 0x40 && f[22] == 64 && f[23] == 17);
  HT_CHECK(f[26] == 169 && f[27] == 254 && f[28] == 0xBB && f[29] == 0xCC && f[30] == 255 && f[33] == 255);
  HT_EQ_U64(xts_netfmt_ip_checksum(f + 14, 20), 0);
  HT_EQ_U64((f[16] << 8 | f[17]), 20 + 8 + 36 + 1);
  HT_CHECK(f[34] == 0x8E && f[35] == 0x08 && f[36] == 0x8E && f[37] == 0x08 && f[40] == 0 && f[41] == 0);
  HT_EQ_U64(xts_netfmt_parse(f + 42, n - 42, &p), 0);
  h.len = 0;
  HT_EQ_U64(xts_netfmt_eth_frame(f, mac, 1, &h, ""), 14 + 20 + 8 + 36);   /* 78 >= 60: no padding needed */
}

/* ---- pacing with a wake window (xts_net_pace_sent) ---- */
/* A receiver on Wi-Fi in power save, as seen on hardware: it dozes 100 ms after the last datagram; while it dozes the
 * access point buffers up to 64 datagrams for it and drops the rest; it wakes at the first beacon (every 102.4 ms, phase
 * `ph`) after a datagram was buffered. Times in us. */
typedef struct { double last_rx, wake_at, ph; int buffered, dropped; } psrx_t;
static void psrx_copy(psrx_t *r, double t)
{
  if (r->buffered && t >= r->wake_at) { r->last_rx = r->wake_at; r->buffered = 0; }
  if (t < r->last_rx + 100000.0) { r->last_rx = t; return; }          /* awake */
  if (!r->buffered) {                                                   /* doze: TIM in the next beacon */
    double k = ceil((t - r->ph) / 102400.0);
    r->wake_at = r->ph + k * 102400.0;
  }
  if (r->buffered < 64) r->buffered++; else r->dropped++;
}
/* nframes queued at once after an idle gap of gap_us (the 209 ms self.cores pause); 3 copies 50 us apart, 1000
 * copies/s; returns datagrams dropped and the time the last frame left */
static int pace_run(int wake, double ph, int nframes, double gap_us, double *done)
{
  const xts_net_pace_cfg_t c = { 3000, wake ? 15000 : 3000, 50000, wake ? 150000 : 0 };
  xts_net_pace_t p = { 0 };
  psrx_t r = { 0, 0, ph, 0, 0 };
  uint64_t now = 1000000;
  /* one frame long before (receiver awake then), then the gap */
  xts_net_pace_sent(&p, &c, now, now, now + 300);
  psrx_copy(&r, (double)now);
  now += (uint64_t)gap_us;
  for (int i = 0; i < nframes; i++) {
    if (now < p.next) now = p.next;
    uint64_t start = now;
    for (int k = 0; k < 3; k++) psrx_copy(&r, (double)(start + 50u * (unsigned)k));
    now = start + 250;
    xts_net_pace_sent(&p, &c, start, start + 100, now);
  }
  *done = (double)now;
  return r.dropped;
}

HTEST(net_pace_wake)
{
  /* the window: opened by the first frame and after >= idle_ticks of silence, spacing wake_frame_ticks inside it */
  const xts_net_pace_cfg_t c = { 10, 100, 1000, 500 };
  xts_net_pace_t p = { 0 };
  xts_net_pace_sent(&p, &c, 5000, 5002, 5004);                         /* first frame ever: window 5002..5502 */
  HT_EQ_U64(p.wakes, 1);
  HT_EQ_U64(p.next, 5100);
  xts_net_pace_sent(&p, &c, 5100, 5101, 5103);
  xts_net_pace_sent(&p, &c, 5501, 5501, 5503);                         /* last start inside the window */
  HT_EQ_U64(p.next, 5601);
  xts_net_pace_sent(&p, &c, 5601, 5602, 5604);                         /* after it: normal spacing, no new window */
  HT_EQ_U64(p.next, 5611);
  HT_EQ_U64(p.wakes, 1);
  xts_net_pace_sent(&p, &c, 6603, 6603, 6605);                         /* 999 ticks of silence: below idle_ticks */
  HT_EQ_U64(p.wakes, 1);
  HT_EQ_U64(p.next, 6613);
  xts_net_pace_sent(&p, &c, 6613, 7605, 7607);                         /* copy 0 held 1000 ticks (fence): a window */
  HT_EQ_U64(p.wakes, 2);
  HT_EQ_U64(p.next, 6713);
  const xts_net_pace_cfg_t off = { 10, 100, 1000, 0 };                 /* wake_ticks 0: plain pacing */
  xts_net_pace_t q = { 0 };
  xts_net_pace_sent(&q, &off, 100, 101, 102);
  xts_net_pace_sent(&q, &off, 5000, 5001, 5002);
  HT_CHECK(q.wakes == 0 && q.next == 5010);

  /* the hardware failure (seq 57-73 of a timing run): 49 frames right after a 209 ms pause. Without the window some
   * beacon phases drop datagrams (and whole frames: all 3 copies); with it none does, at a cost below the window. */
  int lossy_phases = 0, worst_wake = 0;
  double extra = 0;
  for (int ph = 0; ph < 1024; ph++) {
    double t_old, t_new;
    int d_old = pace_run(0, ph * 100.0, 49, 209000, &t_old);
    int d_new = pace_run(1, ph * 100.0, 49, 209000, &t_new);
    if (d_old) lossy_phases++;
    if (d_new > worst_wake) worst_wake = d_new;
    if (t_new - t_old > extra) extra = t_new - t_old;
  }
  HT_CHECK(lossy_phases > 100);
  HT_EQ_U64(worst_wake, 0);
  HT_CHECK(extra < 150000.0);
  /* a long burst (2000 frames) after the pause: still nothing lost, the window costs < 150 ms once */
  double t_old, t_new;
  HT_EQ_U64(pace_run(1, 51200, 2000, 209000, &t_new), 0);
  pace_run(0, 51200, 2000, 209000, &t_old);
  HT_CHECK(t_new - t_old < 150000.0);
}

/* ---- the in-RAM results store ---- */
static int tnf_enum(xts_caselist_t *cl) { xts_case_add(cl, "a"); return 0; }
static int tnf_run(const xts_case_t *c, xts_ctx_t *ctx) { (void)c; (void)ctx; return 0; }
static int tnf_finish(xts_ctx_t *ctx) { (void)ctx; return 0; }
XTS_FAMILY(t_netfin, .kind = "test", .enumerate = tnf_enum, .run = tnf_run, .finish = tnf_finish);

static int cnt_cb(const char *line, void *ud)
{
  uint64_t *v = ud;
  const char *s = strstr(line, "\"seq\":");
  uint64_t seq = s ? strtoull(s + 6, NULL, 10) : 0;
  if (seq <= v[1]) v[2]++;                     /* out of order */
  v[1] = seq;
  v[0]++;
  return 0;
}

HTEST(net_results_store)
{
  xts_registry_init(NULL);
  xts_sink_discard();
  xts_sink_open("store", "host");
  xts_results_set_cap(256u << 10);
  static const xts_family_t fin = { .name = "t_netfin", .kind = "test" }, oth = { .name = "t_netoth", .kind = "test" };
  xts_case_t c = { .id = "x", .insn = -1 };
  xts_condvec_t cv;
  xts_plan_condvec(NULL, &cv);
  xts_ctx_t cf = { .fam = &fin, .c = &c, .block_key = "k", .cond = &cv }, co = { .fam = &oth, .c = &c, .block_key = "k", .cond = &cv };
  static char det[2100];
  memset(det, 0, sizeof det);
  strcpy(det, "{\"pad\":\"");
  memset(det + 8, 'p', 2000);
  strcat(det, "\"}");
  for (int i = 0; i < 100; i++) {        /* ~2.5 KB each: 250 KB, below the cap */
    xts_emit_status(&cf, XTS_ST_PASS, det);
    xts_emit_status(&co, XTS_ST_PASS, det);
  }
  uint64_t v[3] = { 0 };
  HT_EQ_U64(xts_results_foreach("t.netoth", cnt_cb, v), 0);   /* the dotted name is the family's key */
  HT_EQ_U64(xts_results_foreach("t_netoth", cnt_cb, v), 0);   /* evicted when the total passed 256 KB */
  memset(v, 0, sizeof v);
  HT_EQ_U64(xts_results_foreach("t_netfin", cnt_cb, v), 100); /* finish-hook family: kept */
  HT_EQ_U64(v[2], 0);
  uint64_t bytes, lines, dropped;
  int ev;
  xts_results_stats(&bytes, &lines, &dropped, &ev);
  HT_CHECK(ev == 1 && dropped > 0 && bytes <= (256u << 10));
  for (int i = 0; i < 100; i++) xts_emit_status(&cf, XTS_ST_PASS, det);   /* beyond the cap: dropped, counted */
  xts_results_stats(&bytes, &lines, &dropped, &ev);
  memset(v, 0, sizeof v);
  int n = xts_results_foreach("t_netfin", cnt_cb, v);
  HT_CHECK(n > 100 && n < 200 && v[2] == 0);
  /* NULL: every family, merged back into seq order */
  xts_results_set_cap(0);
  xts_sink_open("store2", "host");
  for (int i = 0; i < 30; i++) {
    xts_emit_status(i % 3 ? &cf : &co, XTS_ST_PASS, NULL);
    if (i % 7 == 0) xts_emit_status(&co, XTS_ST_FAIL, NULL);
  }
  memset(v, 0, sizeof v);
  HT_EQ_U64(xts_results_foreach(NULL, cnt_cb, v), 35);
  HT_EQ_U64(v[2], 0);
  xts_sink_close();
  xts_results_reset();
  HT_EQ_U64(xts_results_foreach(NULL, cnt_cb, v), (uint64_t)-1);   /* no run open */
}

/* ---- end to end ---- */
static int e2e_n = 60;
static int e2e_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < e2e_n; i++) xts_case_addf(cl, "e%03d", i);
  return 0;
}
static int e2e_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  static char det[6000];
  int big = ctx->case_index % 9 == 4;                  /* some records span several frames */
  snprintf(det, sizeof det, "{\"i\":%d,\"s\":\"%s\",\"esc\":\"q\\\"b\\\\n\"}", ctx->case_index, big ? "" : "short");
  if (big) {
    size_t o = strlen(det) - 1;
    det[o] = 0;
    strcat(det, ",\"pad\":\"");
    size_t l = strlen(det);
    memset(det + l, 'x', 4000);
    strcpy(det + l + 4000, "\"}");
  }
  xts_emit_value(ctx, "count", ctx->case_index, "count", ctx->case_index == 13 ? XTS_ST_FAIL : XTS_ST_PASS);
  xts_emit_status(ctx, XTS_ST_INFO, det);
  if (ctx->case_index % 10 == 0) xts_log("e2e case %s", c->id);
  return 0;
}
XTS_FAMILY(t_nete2e, .kind = "timing", .axes = "sf", .enumerate = e2e_enum, .run = e2e_run, .block_cases = 16);
static const xts_profile_row_t e2e_rows[] = { { .families = "t.nete2e", .axes = "sf=*" } };
static const xts_profile_t e2e_prof = { "tnete2e", "transport end-to-end", e2e_rows, 1 };

static FILE *tapf;
static void e2e_tap(const char *family, const char *case_id, const xts_record_t *r, const char *line)
{
  (void)family; (void)case_id; (void)r;
  fprintf(tapf, "%s\n", line);
}
static char tap_path[700];
static int e2e_child(void *ud)
{
  (void)ud;
  tapf = fopen(tap_path, "w");
  xts_sink_tap = e2e_tap;
  xts_log("pre-HELLO marker line");                     /* buffered, sent right after HELLO */
  int r = xts_run(&e2e_prof, NULL);
  fclose(tapf);
  return r == 0 ? 0 : 1;
}

static void tap_for(const char *name)
{
  snprintf(tap_path, sizeof tap_path, "%s/%s.tap", ht_tmp, name);
}

HTEST(net_e2e_exact)
{
  char rd[700];
  int rrc = -1;
  tap_for("net_e2e");
  HT_EQ_U64(ht_recv_run("net_e2e", e2e_child, NULL, NULL, rd, sizeof rd, &rrc), 0);
  HT_EQ_U64(rrc, 0);
  char *tap = strdup(ht_slurp(tap_path) ? ht_slurp(tap_path) : "?");
  const char *res = ht_run_file(rd, "results.jsonl");
  HT_CHECK(res && !strcmp(tap, res));                  /* every record once, in order, byte for byte */
  HT_EQ_U64(ht_count_substr(tap, "\n"), 4 * e2e_n);
  free(tap);
  const char *rcv = ht_run_file(rd, "receive.json");
  HT_CHECK(rcv && strstr(rcv, "\"complete\": true") && strstr(rcv, "\"hash_ok\": true") && strstr(rcv, "\"missing_frames\": 0"));
  HT_CHECK(rcv && strstr(rcv, "\"status\": \"complete\""));
  const char *log = ht_run_file(rd, "run.log");
  HT_CHECK(log && strstr(log, "pre-HELLO marker line") && strstr(log, "e2e case e050") && strstr(log, "breadcrumbs: "));
  const char *si = ht_run_file(rd, "sysinfo.json");
  HT_CHECK(si && strstr(si, "\"console_tag\": \"host\"") && strstr(si, "\"profile\": \"tnete2e\""));
  const char *sum = ht_run_file(rd, "summary.json");
  HT_CHECK(sum && strstr(sum, "\"records\": 240") && strstr(sum, "\"fail\": 2"));
  const char *csv = ht_run_file(rd, "results.csv");
  HT_EQ_U64(ht_count_substr(csv, "\n"), 241);
  HT_CHECK(csv && strstr(csv, ",t.nete2e,e013,,,") && strstr(csv, "sf=64"));
  const char *fl = ht_run_file(rd, "failures.jsonl");
  HT_EQ_U64(ht_count_substr(fl, "\n"), 2);
}

/* 5% of the frames dropped: receive.json names exactly those seqs (the ones below the highest seq that arrived) */
HTEST(net_e2e_loss)
{
  char rd[700], env[900], drops[700];
  int rrc = -1;
  tap_for("net_loss");
  snprintf(drops, sizeof drops, "%s/net_loss.drops", ht_tmp);
  snprintf(env, sizeof env, "XTS_NET_DROP=0.05,7;XTS_NET_DROP_LOG=%s", drops);
  HT_EQ_U64(ht_recv_run("net_loss", e2e_child, NULL, env, rd, sizeof rd, &rrc), 0);
  HT_EQ_U64(rrc, 1);                                   /* incomplete */
  const char *rcv = ht_run_file(rd, "receive.json");
  HT_CHECK(rcv && strstr(rcv, "\"complete\": false"));
  long maxseen = 0;
  const char *m = rcv ? strstr(rcv, "\"max_seq_seen\": ") : NULL;
  if (m) maxseen = strtol(m + 16, NULL, 10);
  /* expand missing_seq ranges */
  static uint8_t miss[200000], drop[200000];
  memset(miss, 0, sizeof miss);
  memset(drop, 0, sizeof drop);
  const char *ms = rcv ? strstr(rcv, "\"missing_seq\": [") : NULL;
  int nmiss = 0, ndrop = 0, ok = ms != NULL;
  if (ms) {
    const char *p = ms + 16, *end = strstr(p, "\"missing_in_end_range\"");
    while (p && end && p < end) {
      const char *q = strchr(p, '[');
      if (!q || q > end) break;
      char *e1;
      long a = strtol(q + 1, &e1, 10);
      long b = strtol(strchr(e1, ',') + 1, &e1, 10);
      for (long s = a; s <= b && s < 200000; s++) { miss[s] = 1; nmiss++; }
      p = e1;
    }
  }
  const char *dl = ht_slurp(drops);
  for (const char *p = dl; p && *p;) {
    long s = strtol(p, (char **)&p, 10);
    if (s > 0 && s <= maxseen && s < 200000) { drop[s] = 1; ndrop++; }
    while (*p == '\n') p++;
  }
  for (long s = 1; s < 200000; s++) ok &= miss[s] == drop[s];
  HT_CHECK(ok);
  HT_CHECK(ndrop > 10 && ndrop == nmiss);
  printf("    loss: %d frames dropped and reported missing (of %ld)\n", ndrop, maxseen);
}

/* 3 copies per frame, copy 0 of 10% of the frames corrupted, a 64-datagram reorder window: still complete and exact */
HTEST(net_e2e_dup_corrupt_reorder)
{
  char rd[700];
  int rrc = -1;
  tap_for("net_dup");
  HT_EQ_U64(ht_recv_run("net_dup", e2e_child, NULL, "XTS_NET_COPIES=3;XTS_NET_CORRUPT=0.1,3;XTS_NET_REORDER=64,5", rd,
                        sizeof rd, &rrc), 0);
  HT_EQ_U64(rrc, 0);
  char *tap = strdup(ht_slurp(tap_path) ? ht_slurp(tap_path) : "?");
  const char *res = ht_run_file(rd, "results.jsonl");
  HT_CHECK(res && !strcmp(tap, res));
  free(tap);
  const char *rcv = ht_run_file(rd, "receive.json");
  HT_CHECK(rcv && strstr(rcv, "\"complete\": true") && !strstr(rcv, "\"crc_errors\": 0,") && !strstr(rcv, "\"duplicates\": 0,"));
}
