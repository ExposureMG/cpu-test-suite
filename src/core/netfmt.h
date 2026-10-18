/* XTS - wire format of the UDP result transport (docs/PROTOCOL.md), shared by the console transmitter
 * (src/plat/net_console.c), the host sender (src/plat/net_host.c) and the host tests. Pure functions: no allocation, no
 * stdio, no globals written, safe on any core.
 *
 * A transmitter only moves bytes:
 *   n = xts_netfmt_frag_count(len);                       // frames of one message
 *   for (i = 0; i < n; i++)
 *     for (c = 0; c < copies; c++) {
 *       xts_net_hdr_t h = { type, c ? 1 : 0, c, nonce, first_seq + i, msg, i, n, frag_len(i) };
 *       flen = xts_netfmt_eth_frame(txbuf, mac, ip_id++, &h, payload + i * XTS_NET_MAX_PAYLOAD);   // Ethernet frame
 *       ... hand txbuf[0..flen) to the NIC ...
 *     }
 * The host sender uses xts_netfmt_udp_payload (XTS header + payload, the UDP datagram body) instead. */
#ifndef XTS_NETFMT_H
#define XTS_NETFMT_H
#include <stddef.h>
#include <stdint.h>
#include <xts/net.h>

#define XTS_NET_ETH_BYTES 14
#define XTS_NET_IP_BYTES 20
#define XTS_NET_UDP_BYTES 8
#define XTS_NET_FRAME_OVERHEAD (XTS_NET_ETH_BYTES + XTS_NET_IP_BYTES + XTS_NET_UDP_BYTES + XTS_NET_HDR_BYTES)   /* 78 */
#define XTS_NET_MAX_FRAME (XTS_NET_FRAME_OVERHEAD + XTS_NET_MAX_PAYLOAD)   /* 1478: below the 1514-byte Ethernet limit */
#define XTS_NET_MIN_FRAME 60                                                /* shorter frames are zero-padded */
#define XTS_NET_FLAG_REPEAT 0x01                                            /* header flags bit0: a repeat copy */
#define XTS_FNV64_INIT 0xcbf29ce484222325ull
#define XTS_FNV64_PRIME 0x100000001b3ull

typedef struct xts_net_hdr {
  uint8_t type;            /* XTS_PK_* */
  uint8_t flags;           /* XTS_NET_FLAG_REPEAT for copies 1.. */
  uint8_t copy;            /* copy index 0..copies-1 */
  uint64_t nonce;          /* run nonce */
  uint32_t seq;            /* frame sequence number (1..), the same for every copy */
  uint32_t msg;            /* message number (1..) */
  uint16_t frag_idx, frag_cnt;
  uint16_t len;            /* payload bytes in this frame (<= XTS_NET_MAX_PAYLOAD) */
} xts_net_hdr_t;

/* CRC-32 (IEEE 802.3, reflected, as zlib.crc32): start with crc = 0, chain calls over consecutive buffers */
uint32_t xts_crc32(uint32_t crc, const void *data, size_t n);
/* FNV-1a 64 (rec_hash): start with h = XTS_FNV64_INIT */
uint64_t xts_fnv1a64(uint64_t h, const void *data, size_t n);

/* frames needed for a message of len bytes (>= 1; 0 if len > XTS_NET_MAX_MESSAGE) */
uint16_t xts_netfmt_frag_count(uint32_t len);
/* payload bytes of fragment i of a len-byte message */
uint16_t xts_netfmt_frag_len(uint32_t len, uint16_t i);

/* the 36-byte XTS header for h (big-endian, CRC over header bytes 0..31 + payload[0..h->len)) */
void xts_netfmt_header(uint8_t out[XTS_NET_HDR_BYTES], const xts_net_hdr_t *h, const void *payload);
/* XTS header + payload (the UDP datagram body) into out; returns XTS_NET_HDR_BYTES + h->len */
uint32_t xts_netfmt_udp_payload(uint8_t *out, const xts_net_hdr_t *h, const void *payload);
/* Destination of every frame built below: broadcast (ff:ff:ff:ff:ff:ff, 255.255.255.255) by default; a unicast host
 * (its MAC and IPv4 address, host byte order) after this call. Build time: make NET_DEST=auto|<ip> (src/plat/net_console.c). */
void xts_netfmt_set_dest(const uint8_t mac[6], uint32_t ip);
/* A complete Ethernet frame: broadcast (or xts_netfmt_set_dest) dst, src = mac, IPv4 169.254.mac[4].mac[5] -> 255.255.255.255 (DF, TTL 64,
 * header checksum), UDP 36360 -> 36360 (checksum 0), XTS header, payload; zero-padded to XTS_NET_MIN_FRAME. out must hold
 * XTS_NET_MAX_FRAME bytes. Returns the frame length (without the FCS, which the NIC appends). */
uint32_t xts_netfmt_eth_frame(uint8_t *out, const uint8_t mac[6], uint16_t ip_id, const xts_net_hdr_t *h,
                              const void *payload);
/* IPv4 header checksum (one's complement of the one's-complement sum of 16-bit words; 0 over a valid header) */
uint16_t xts_netfmt_ip_checksum(const uint8_t *ip, uint32_t n);

/* Parse an XTS header + payload (a UDP datagram body). 0 ok and *h filled; -1 short/magic/version/length, -2 CRC. */
int xts_netfmt_parse(const uint8_t *buf, uint32_t n, xts_net_hdr_t *h);

/* message type name ("HELLO", "REC", ...; "?" if unknown) and the reverse */
const char *xts_net_type_name(int type);
#define XTS_PK_MAX 8
#endif
