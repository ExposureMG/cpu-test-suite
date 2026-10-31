/* Transport, host build: the xts/net.h API as synchronous UDP datagrams (XTS header + payload, docs/PROTOCOL.md) to
 * XTS_NET_HOST ("<ip>:<port>", default 127.0.0.1:36360; "off" = count but send nothing). The frame format is
 * src/core/netfmt.c, shared with the console transmitter. Test knobs (environment, read by xts_net_init):
 *   XTS_NET_COPIES=<n>          copies per frame (default 1 on the host; the console sends 2)
 *   XTS_NET_FPS=<n>             frame rate cap, copies included (default 0 = unpaced)
 *   XTS_NET_DROP=<rate>,<seed>  drop whole frames (every copy) with probability rate; the dropped seqs are kept
 *                               (xts_net_host_dropped) and appended to XTS_NET_DROP_LOG=<file> if set (one per line)
 *   XTS_NET_CORRUPT=<rate>,<seed>  flip one byte of copy 0 of a frame (needs copies >= 2 to be recoverable)
 *   XTS_NET_REORDER=<window>,<seed> hold up to <window> datagrams and send them in random order (flushed by
 *                               xts_net_wait_sent / xts_net_drain) */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <xts/xts.h>
#include <core/core.h>
#include <core/netfmt.h>

static int fd = -1, off, inited;
static struct sockaddr_in dst;
static uint64_t nonce;
static uint32_t seq, msg;
static xts_net_stats_t st;
static int copies = 1;
static double fps;
static double drop_rate, corrupt_rate;
static xts_rng_t drop_rng, corrupt_rng, reorder_rng;
static FILE *drop_log;
static uint32_t *dropped;
static uint32_t ndropped, dropped_cap;
static int reorder_win;
static struct { uint8_t *b; uint32_t n; } *held;
static int nheld;
static struct timespec t_start;
static uint64_t paced;

static void parse_rate(const char *env, double *rate, xts_rng_t *rng)
{
  const char *s = getenv(env);
  *rate = 0;
  if (!s || !*s) return;
  unsigned long long seed = 1;
  char *e;
  *rate = strtod(s, &e);
  if (*e == ',') seed = strtoull(e + 1, NULL, 0);
  xts_rng_seed(rng, seed);
}

uint64_t xts_net_run_nonce(void)
{
  if (!nonce) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    nonce = ((uint64_t)getpid() << 40) ^ ((uint64_t)ts.tv_sec << 20) ^ (uint64_t)ts.tv_nsec;
    if (!nonce) nonce = 1;
  }
  return nonce;
}

int xts_net_init(void)
{
  if (fd >= 0) close(fd);
  fd = -1;
  if (drop_log) fclose(drop_log);
  drop_log = NULL;
  memset(&st, 0, sizeof st);
  seq = msg = 0;
  ndropped = 0;
  nheld = 0;
  nonce = 0;
  paced = 0;
  xts_net_run_nonce();
  clock_gettime(CLOCK_MONOTONIC, &t_start);
  const char *h = getenv("XTS_NET_HOST");
  off = h && !strcmp(h, "off");
  const char *c = getenv("XTS_NET_COPIES");
  copies = c ? atoi(c) : 1;
  if (copies < 1) copies = 1;
  if (copies > 8) copies = 8;
  const char *f = getenv("XTS_NET_FPS");
  fps = f ? atof(f) : 0;
  parse_rate("XTS_NET_DROP", &drop_rate, &drop_rng);
  parse_rate("XTS_NET_CORRUPT", &corrupt_rate, &corrupt_rng);
  const char *dl = getenv("XTS_NET_DROP_LOG");
  if (dl && *dl && drop_rate > 0) drop_log = fopen(dl, "w");
  double win = 0;
  parse_rate("XTS_NET_REORDER", &win, &reorder_rng);
  reorder_win = (int)win;
  if (reorder_win < 0) reorder_win = 0;
  if (reorder_win > 4096) reorder_win = 4096;
  if (reorder_win && !held) held = calloc(4096, sizeof *held);
  inited = 1;
  st.link_up = 1;
  if (off) return 0;
  char host[64] = "127.0.0.1";
  int port = XTS_NET_PORT;
  if (h && *h) {
    snprintf(host, sizeof host, "%s", h);
    char *colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
  }
  memset(&dst, 0, sizeof dst);
  dst.sin_family = AF_INET;
  dst.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) { st.link_up = 0; return -1; }
  fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) { st.link_up = 0; return -1; }
  int one = 1, buf = 4 << 20;
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
  return 0;
}

static void pace(void)
{
  if (fps <= 0) return;
  paced++;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  double el = (double)(now.tv_sec - t_start.tv_sec) + (double)(now.tv_nsec - t_start.tv_nsec) * 1e-9;
  double want = (double)paced / fps;
  if (want > el) {
    double d = want - el;
    struct timespec ts = { (time_t)d, (long)((d - (double)(time_t)d) * 1e9) };
    nanosleep(&ts, NULL);
  }
}

static int raw_send(const uint8_t *b, uint32_t n)
{
  pace();
  st.frame_copies++;
  st.bytes += n;
  if (off) return 0;
  for (int tries = 0; tries < 2000; tries++) {
    ssize_t r = sendto(fd, b, n, 0, (const struct sockaddr *)&dst, sizeof dst);
    if (r == (ssize_t)n) return 0;
    if (r < 0 && (errno == ENOBUFS || errno == EAGAIN || errno == EINTR)) { usleep(100); continue; }
    if (r < 0 && errno == ECONNREFUSED) continue;     /* a stale ICMP error: the datagram was not sent, retry */
    break;
  }
  st.tx_timeouts++;
  return -1;
}

static void flush_held(void)
{
  while (nheld > 0) {
    int i = (int)xts_rng_below(&reorder_rng, (uint32_t)nheld);
    raw_send(held[i].b, held[i].n);
    free(held[i].b);
    held[i] = held[--nheld];
  }
}

static void out(const uint8_t *b, uint32_t n)
{
  if (!reorder_win) { raw_send(b, n); return; }
  if (nheld >= reorder_win) {
    int i = (int)xts_rng_below(&reorder_rng, (uint32_t)nheld);
    raw_send(held[i].b, held[i].n);
    free(held[i].b);
    held[i] = held[--nheld];
  }
  held[nheld].b = malloc(n);
  if (!held[nheld].b) { raw_send(b, n); return; }
  memcpy(held[nheld].b, b, n);
  held[nheld++].n = n;
}

uint32_t xts_net_send(int type, const void *payload, uint32_t len)
{
  if (!inited || len > XTS_NET_MAX_MESSAGE || (!off && fd < 0)) { st.send_failures++; return 0; }
  uint16_t n = xts_netfmt_frag_count(len);
  uint8_t buf[XTS_NET_HDR_BYTES + XTS_NET_MAX_PAYLOAD];
  msg++;
  st.messages++;
  for (uint16_t i = 0; i < n; i++) {
    xts_net_hdr_t h;
    memset(&h, 0, sizeof h);
    h.type = (uint8_t)type;
    h.nonce = nonce;
    h.seq = ++seq;
    h.msg = msg;
    h.frag_idx = i;
    h.frag_cnt = n;
    h.len = xts_netfmt_frag_len(len, i);
    const uint8_t *p = (const uint8_t *)payload + (size_t)i * XTS_NET_MAX_PAYLOAD;
    st.frames++;
    int drop = drop_rate > 0 && (double)(xts_rng_next(&drop_rng) >> 11) * 0x1.0p-53 < drop_rate;
    int corrupt = corrupt_rate > 0 && (double)(xts_rng_next(&corrupt_rng) >> 11) * 0x1.0p-53 < corrupt_rate;
    if (drop) {
      if (ndropped == dropped_cap) {
        uint32_t nc = dropped_cap ? dropped_cap * 2 : 1024;
        uint32_t *nd = realloc(dropped, nc * sizeof *nd);
        if (nd) { dropped = nd; dropped_cap = nc; }
      }
      if (ndropped < dropped_cap) dropped[ndropped++] = h.seq;
      if (drop_log) { fprintf(drop_log, "%u\n", (unsigned)h.seq); fflush(drop_log); }
      continue;
    }
    for (int c = 0; c < copies; c++) {
      h.copy = (uint8_t)c;
      h.flags = c ? XTS_NET_FLAG_REPEAT : 0;
      uint32_t bn = xts_netfmt_udp_payload(buf, &h, p);
      if (corrupt && c == 0) buf[bn - 1 > XTS_NET_HDR_BYTES ? bn - 1 : 10] ^= 0x5A;
      out(buf, bn);
    }
  }
  st.last_seq = seq;
  return seq;
}

int xts_net_wait_sent(uint32_t s, uint32_t timeout_us)
{
  (void)s;
  (void)timeout_us;
  flush_held();
  return 0;
}

int xts_net_drain(uint32_t timeout_us)
{
  (void)timeout_us;
  flush_held();
  return 0;
}

void xts_net_stats(xts_net_stats_t *s) { if (s) { *s = st; s->last_seq = seq; } }

uint64_t xts_net_new_run(void)
{
  flush_held();
  uint64_t old = nonce;
  do {
    nonce = 0;
    xts_net_run_nonce();
  } while (nonce == old);
  seq = msg = 0;
  int link = st.link_up;
  memset(&st, 0, sizeof st);
  st.link_up = link;
  ndropped = 0;
  return nonce;
}

uint32_t xts_net_host_dropped(uint32_t *seqs, uint32_t max)
{
  for (uint32_t i = 0; seqs && i < ndropped && i < max; i++) seqs[i] = dropped[i];
  return ndropped;
}
