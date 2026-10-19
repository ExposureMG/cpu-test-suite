/* XTS - result transport (replaces all storage). Wire format: docs/PROTOCOL.md.
 *
 * Messages leave as UDP datagrams sent by a dedicated transmitter thread (PIR 4, see xts/xcore.h) that owns the NIC:
 * libxenon's DHCP + lwIP UDP by default (its ARP/DHCP housekeeping runs only on that thread, between measurements), or
 * XTS's own raw TX-only driver with make NET_DRIVER=own. The suite (PIR 2) only enqueues messages into an uncached
 * ring; the transmitter fragments them into frames, sends each frame XTS_NET_COPIES times, paces the frame rate and never starts a frame
 * while a measurement runs (xts_xc_measuring()). The PC side is tools/xts_recv.py.
 *
 * Host build: the same API sends real UDP datagrams to XTS_NET_HOST (default 127.0.0.1:36360) synchronously.
 *
 * Only the suite thread calls xts_net_send / xts_net_wait_sent / xts_net_drain (single producer). None of them may be
 * called while the test core is executing a case (the suite is blocked in xts_xc_call then anyway).
 */
#ifndef XTS_NET_H
#define XTS_NET_H
#include <stdint.h>

#define XTS_NET_PORT 36360
#define XTS_NET_MAGIC "XTS1"
#define XTS_NET_VERSION 1
#define XTS_NET_HDR_BYTES 36           /* see docs/PROTOCOL.md */
#define XTS_NET_MAX_PAYLOAD 1400       /* per frame: Ethernet 1514 - 14 eth - 20 ip - 8 udp - 36 hdr, rounded down */
#define XTS_NET_MAX_MESSAGE 32768      /* one message (a results.jsonl line) is fragmented into <= 24 frames */

/* message types (header byte "type") */
enum {
  XTS_PK_HELLO = 1,   /* pre-run broadcast: JSON {sysinfo, profile, plan:{blocks,cases}, cores, build_id} - sent 3x */
  XTS_PK_REC = 2,     /* one results.jsonl line (the record schema of CONTRIBUTING.md, unchanged) */
  XTS_PK_LOG = 3,     /* one run.log line */
  XTS_PK_CRUMB = 4,   /* case about to run: JSON {block, case_index, case_id, family, mnem, word, tb} */
  XTS_PK_CRASH = 5,   /* crash report JSON (registers, DAR/DSISR, PIR, case, input state, last log lines) */
  XTS_PK_HANG = 6,    /* hang report JSON (last crumb, timeout, heartbeats) */
  XTS_PK_BLOCK = 7,   /* block end: JSON {block, records} */
  XTS_PK_END = 8,     /* post-run broadcast: JSON {status, last_seq, messages:{TYPE:n}, rec_hash} - sent 3x */
};

/* Bring the transport up. Console: NIC reset/link/TX ring, then the transmitter thread (xts_xc_start_roles must have
 * started PIR 4 or this starts it). Host: UDP socket. 0 ok, <0 error (the caller shows it on screen and halts). */
int xts_net_init(void);

/* Enqueue one message. Fragments it, assigns frame sequence numbers. Returns the sequence number of the message's LAST
 * frame (>0), or 0 if it could not be queued (ring full beyond the timeout, message too large, transport down).
 * Never blocks for longer than the internal ring timeout (default 2 s). */
uint32_t xts_net_send(int type, const void *payload, uint32_t len);

/* Wait until every copy of the frame with sequence `seq` has been handed to the NIC and the NIC reported it sent.
 * 0 ok, -1 timeout. Used for breadcrumbs (a crumb must be on the wire before its case runs) and before END. */
int xts_net_wait_sent(uint32_t seq, uint32_t timeout_us);

/* Wait until the ring is empty and everything queued has been sent. 0 ok, -1 timeout. */
int xts_net_drain(uint32_t timeout_us);

typedef struct xts_net_stats {
  uint64_t messages, frames, frame_copies, bytes;
  uint64_t ring_full_waits, send_failures, tx_timeouts;
  uint64_t fence_waits, fence_wait_ticks;     /* times / timebase ticks the transmitter waited for a measurement */
  uint64_t wake_windows;                      /* slow-start windows after an idle link (console, xts_net_pace_sent) */
  uint32_t last_seq;
  int link_up;
} xts_net_stats_t;
void xts_net_stats(xts_net_stats_t *s);

/* The run nonce in every header (console: MAC + timebase at boot; host: pid + time). */
uint64_t xts_net_run_nonce(void);
#endif
