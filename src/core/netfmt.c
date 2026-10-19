/* Wire format of the UDP result transport: see netfmt.h and docs/PROTOCOL.md. */
#include <string.h>
#include "netfmt.h"

static const uint32_t crc_nib[16] = {
  0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
  0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t xts_crc32(uint32_t crc, const void *data, size_t n)
{
  const uint8_t *p = data;
  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    crc = (crc >> 4) ^ crc_nib[crc & 15];
    crc = (crc >> 4) ^ crc_nib[crc & 15];
  }
  return ~crc;
}

uint64_t xts_fnv1a64(uint64_t h, const void *data, size_t n)
{
  const uint8_t *p = data;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= XTS_FNV64_PRIME;
  }
  return h;
}

uint16_t xts_netfmt_frag_count(uint32_t len)
{
  if (len > XTS_NET_MAX_MESSAGE) return 0;
  return len ? (uint16_t)((len + XTS_NET_MAX_PAYLOAD - 1) / XTS_NET_MAX_PAYLOAD) : 1;
}

uint16_t xts_netfmt_frag_len(uint32_t len, uint16_t i)
{
  uint32_t off = (uint32_t)i * XTS_NET_MAX_PAYLOAD;
  if (off >= len) return 0;
  return (uint16_t)(len - off > XTS_NET_MAX_PAYLOAD ? XTS_NET_MAX_PAYLOAD : len - off);
}

static void be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { be16(p, v >> 16); be16(p + 2, v); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) << 16 | rd16(p + 2); }

void xts_netfmt_header(uint8_t out[XTS_NET_HDR_BYTES], const xts_net_hdr_t *h, const void *payload)
{
  memcpy(out, XTS_NET_MAGIC, 4);
  out[4] = XTS_NET_VERSION;
  out[5] = h->type;
  out[6] = h->flags;
  out[7] = h->copy;
  be32(out + 8, (uint32_t)(h->nonce >> 32));
  be32(out + 12, (uint32_t)h->nonce);
  be32(out + 16, h->seq);
  be32(out + 20, h->msg);
  be16(out + 24, h->frag_idx);
  be16(out + 26, h->frag_cnt);
  be16(out + 28, h->len);
  be16(out + 30, 0);
  uint32_t crc = xts_crc32(0, out, 32);
  crc = xts_crc32(crc, payload, h->len);
  be32(out + 32, crc);
}

uint32_t xts_netfmt_udp_payload(uint8_t *out, const xts_net_hdr_t *h, const void *payload)
{
  xts_netfmt_header(out, h, payload);
  if (h->len) memcpy(out + XTS_NET_HDR_BYTES, payload, h->len);
  return XTS_NET_HDR_BYTES + h->len;
}

uint16_t xts_netfmt_ip_checksum(const uint8_t *ip, uint32_t n)
{
  uint32_t s = 0;
  for (uint32_t i = 0; i + 1 < n; i += 2) s += rd16(ip + i);
  if (n & 1) s += (uint32_t)ip[n - 1] << 8;
  while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
  return (uint16_t)~s;
}

/* frame destination: broadcast unless xts_netfmt_set_dest() named a host (unicast is far more reliable when the
 * receiver is behind Wi-Fi: unicast frames get link-layer retries, broadcast frames do not) */
static uint8_t dest_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint32_t dest_ip = 0xFFFFFFFFu;

void xts_netfmt_set_dest(const uint8_t mac[6], uint32_t ip)
{
  memcpy(dest_mac, mac, 6);
  dest_ip = ip;
}

uint32_t xts_netfmt_eth_frame(uint8_t *out, const uint8_t mac[6], uint16_t ip_id, const xts_net_hdr_t *h,
                              const void *payload)
{
  uint8_t *eth = out, *ip = out + XTS_NET_ETH_BYTES, *udp = ip + XTS_NET_IP_BYTES, *x = udp + XTS_NET_UDP_BYTES;
  uint32_t body = XTS_NET_HDR_BYTES + h->len;
  memcpy(eth, dest_mac, 6);
  memcpy(eth + 6, mac, 6);
  be16(eth + 12, 0x0800);
  ip[0] = 0x45;                                  /* version 4, IHL 5 */
  ip[1] = 0;
  be16(ip + 2, XTS_NET_IP_BYTES + XTS_NET_UDP_BYTES + body);
  be16(ip + 4, ip_id);
  be16(ip + 6, 0x4000);                          /* DF */
  ip[8] = 64;
  ip[9] = 17;
  be16(ip + 10, 0);
  ip[12] = 169; ip[13] = 254; ip[14] = mac[4]; ip[15] = mac[5];
  ip[16] = (uint8_t)(dest_ip >> 24); ip[17] = (uint8_t)(dest_ip >> 16); ip[18] = (uint8_t)(dest_ip >> 8); ip[19] = (uint8_t)dest_ip;
  be16(ip + 10, xts_netfmt_ip_checksum(ip, XTS_NET_IP_BYTES));
  be16(udp, XTS_NET_PORT);
  be16(udp + 2, XTS_NET_PORT);
  be16(udp + 4, XTS_NET_UDP_BYTES + body);
  be16(udp + 6, 0);
  xts_netfmt_udp_payload(x, h, payload);
  uint32_t n = XTS_NET_ETH_BYTES + XTS_NET_IP_BYTES + XTS_NET_UDP_BYTES + body;
  if (n < XTS_NET_MIN_FRAME) { memset(out + n, 0, XTS_NET_MIN_FRAME - n); n = XTS_NET_MIN_FRAME; }
  return n;
}

int xts_netfmt_parse(const uint8_t *buf, uint32_t n, xts_net_hdr_t *h)
{
  if (n < XTS_NET_HDR_BYTES || memcmp(buf, XTS_NET_MAGIC, 4) || buf[4] != XTS_NET_VERSION) return -1;
  h->type = buf[5];
  h->flags = buf[6];
  h->copy = buf[7];
  h->nonce = (uint64_t)rd32(buf + 8) << 32 | rd32(buf + 12);
  h->seq = rd32(buf + 16);
  h->msg = rd32(buf + 20);
  h->frag_idx = rd16(buf + 24);
  h->frag_cnt = rd16(buf + 26);
  h->len = rd16(buf + 28);
  if (h->len > XTS_NET_MAX_PAYLOAD || XTS_NET_HDR_BYTES + (uint32_t)h->len > n || !h->frag_cnt || h->frag_idx >= h->frag_cnt)
    return -1;
  uint32_t crc = xts_crc32(xts_crc32(0, buf, 32), buf + XTS_NET_HDR_BYTES, h->len);
  return crc == rd32(buf + 32) ? 0 : -2;
}

static const char *const type_names[XTS_PK_MAX + 1] = { "?", "HELLO", "REC", "LOG", "CRUMB", "CRASH", "HANG", "BLOCK", "END" };
const char *xts_net_type_name(int type) { return type > 0 && type <= XTS_PK_MAX ? type_names[type] : "?"; }

void xts_net_pace_sent(xts_net_pace_t *p, const xts_net_pace_cfg_t *c, uint64_t start, uint64_t first, uint64_t end)
{
  if (c->wake_ticks && (!p->started || first - p->last_end >= c->idle_ticks)) {
    uint64_t w = c->wake_ticks, quiet = p->started ? first - p->last_end : c->wake_max_ticks;   /* boot: the longest */
    if (c->wake_max_ticks > w) w = quiet < w ? w : quiet > c->wake_max_ticks ? c->wake_max_ticks : quiet;
    p->wake_until = first + w;
    p->wakes++;
  }
  p->started = 1;
  p->last_end = end;
  p->next = start + (first < p->wake_until ? c->wake_frame_ticks : c->frame_ticks);
}
