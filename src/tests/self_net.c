/* self.net - the UDP result transport (xts/net.h) seen from the suite:
 *   len<n>   one LOG line of n bytes (1 frame, the 1400/1401 fragment boundary, 3 and 15 frames): queued, then
 *            xts_net_wait_sent on its last frame (2 s). Value: ticks from queueing until every copy was on the wire.
 *   burst    200 LOG lines of 200 bytes back to back, then wait for the last: total ticks (frames/s in the detail).
 *   stats    the transmitter statistics (xts_net_stats) as an info record: frames, copies, fence waits, timeouts.
 * The LOG lines land in run.log on the PC ("self.net ..."); tools/xts_recv.py checks their reassembly and the seqs.
 * Messages go through xts_core_send, so END's per-type counts include them. Host: the same calls on the host sender. */
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include <xts/json.h>
#include <xts/net.h>
#include <core/core.h>              /* xts_core_send (counted for END) */

static const int lens[] = { 64, 1400, 1401, 4000, 20000 };
#define NLENS ((int)(sizeof lens / sizeof lens[0]))
#define BURST 200

static int net_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < NLENS; i++) {
    xts_case_t *c = xts_case_addf(cl, "len%d", lens[i]);
    if (!c) return -1;
    c->arg[0] = 0;
    c->arg[1] = lens[i];
  }
  xts_case_t *c = xts_case_add(cl, "burst");
  if (!c) return -1;
  c->arg[0] = 1;
  if (!(c = xts_case_add(cl, "stats"))) return -1;
  c->arg[0] = 2;
  return 0;
}

static uint32_t fill_line(char *buf, uint32_t n, const char *what, int k)
{
  int h = snprintf(buf, n + 1, "self.net %s %d (%u bytes): ", what, k, (unsigned)n);
  for (uint32_t i = (uint32_t)(h > 0 ? h : 0); i < n; i++) buf[i] = (char)('0' + i % 10);
  buf[n] = 0;
  return n;
}

static void emit(xts_ctx_t *ctx, int status, double ticks, const char *det, int trunc)
{
  xts_record_t r;
  xts_record_init(&r);
  r.metric = "latency";
  r.unit = "ticks";
  r.value = ticks;
  r.status = status;
  r.detail = det;
  r.detail_truncated = trunc;
  xts_emit(ctx, &r);
}

static int net_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  static char buf[20001];
  char det[768];
  xts_json_t j;
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  if (c->arg[0] == 0) {
    uint32_t n = fill_line(buf, (uint32_t)c->arg[1], "len", (int)c->arg[1]);
    uint64_t t0 = xts_tb();
    uint32_t seq = xts_core_send(XTS_PK_LOG, buf, n);
    int w = seq ? xts_net_wait_sent(seq, 2000000) : -1;
    uint64_t dt = xts_tb() - t0;
    xj_kv_uint(&j, "len", n);
    xj_kv_uint(&j, "frames", (n + XTS_NET_MAX_PAYLOAD - 1) / XTS_NET_MAX_PAYLOAD);
    xj_kv_uint(&j, "last_seq", seq);
    xj_kv_str(&j, "wait_sent", seq == 0 ? "not queued" : w == 0 ? "ok" : "timeout");
    xj_end(&j);
    emit(ctx, seq && w == 0 ? XTS_ST_PASS : XTS_ST_FAIL, (double)dt, xj_finish(&j), j.truncated);
  } else if (c->arg[0] == 1) {
    uint32_t last = 0;
    int queued = 0;
    uint64_t t0 = xts_tb();
    for (int k = 0; k < BURST; k++) {
      uint32_t n = fill_line(buf, 200, "burst", k);
      uint32_t s = xts_core_send(XTS_PK_LOG, buf, n);
      if (s) { last = s; queued++; }
    }
    int w = last ? xts_net_wait_sent(last, 5000000) : -1;
    uint64_t dt = xts_tb() - t0;
    xj_kv_int(&j, "messages", BURST);
    xj_kv_int(&j, "queued", queued);
    xj_kv_uint(&j, "last_seq", last);
    xj_kv_str(&j, "wait_sent", w == 0 ? "ok" : "timeout");
    xj_kv_double(&j, "messages_per_s", dt ? (double)queued * (double)xts_tb_hz() / (double)dt : 0.0);
    xj_end(&j);
    emit(ctx, queued == BURST && w == 0 ? XTS_ST_PASS : XTS_ST_FAIL, (double)dt, xj_finish(&j), j.truncated);
  } else {
    xts_net_stats_t s;
    xts_net_stats(&s);
    xj_kv_uint(&j, "messages", s.messages);
    xj_kv_uint(&j, "frames", s.frames);
    xj_kv_uint(&j, "frame_copies", s.frame_copies);
    xj_kv_uint(&j, "bytes", s.bytes);
    xj_kv_uint(&j, "ring_full_waits", s.ring_full_waits);
    xj_kv_uint(&j, "send_failures", s.send_failures);
    xj_kv_uint(&j, "tx_timeouts", s.tx_timeouts);
    xj_kv_uint(&j, "fence_waits", s.fence_waits);
    xj_kv_uint(&j, "fence_wait_ticks", s.fence_wait_ticks);
    xj_kv_uint(&j, "last_seq", s.last_seq);
    xj_kv_bool(&j, "link_up", s.link_up);
    xj_kv_hex(&j, "run_nonce", xts_net_run_nonce(), 16);
    xj_end(&j);
    xts_record_t r;
    xts_record_init(&r);
    r.metric = "count";
    r.unit = "count";
    r.value = (double)s.frames;
    r.status = s.send_failures || s.tx_timeouts ? XTS_ST_FAIL : XTS_ST_INFO;
    r.detail = xj_finish(&j);
    r.detail_truncated = j.truncated;
    xts_emit(ctx, &r);
  }
  return 0;
}

XTS_FAMILY(self_net, .kind = "self", .enumerate = net_enum, .run = net_run, .block_cases = 16,
           .desc = "UDP transport: fragmentation, wait-sent latency, burst rate, transmitter statistics");
