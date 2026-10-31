/* Result transport, console: the message ring between the suite (producer) and the transmitter thread on PIR 4
 * (consumer), and the NIC path. Interface: xts/net.h, wire format: docs/PROTOCOL.md (frames/datagrams are built by
 * src/core/netfmt.c, shared with the host sender and the host tests).
 *
 * Default NIC path: libxenon's own network stack - network_init() (DHCP; libxenon's enet.c driver) and lwIP UDP. This is
 * the path that delivered frames on hardware during bring-up; XTS's raw frames never reached the network there.
 *   - the source address is the one the router handed out by DHCP; the PC's MAC is resolved by ARP before sending;
 *   - each copy of each XTS frame is one udp_sendto (unicast with NET_DEST, else broadcast);
 *   - lwIP's housekeeping (network_poll: ARP replies, DHCP lease) runs only on the transmitter, only between
 *     measurements (inside the fence). The NIC can still store incoming frames in memory during a measurement.
 *
 * Optional NIC path (make NET_DRIVER=own, XTS_NET_OWN_DRIVER): a TX-only driver using the register sequence of enet.c
 * without lwIP: descriptors and buffers in UNCACHED memory (DMA address = EA & 0x1FFFFFFF), RX enabled once but never
 * read, RX/TX started after link-up, every hardware wait bounded, source IP 169.254.mac[4].mac[5] (link-local), and a
 * NET_SETTLE_S wait after link-up. It was not seen to deliver frames on the bring-up network.
 * MAC: xenon_config_get_mac_addr (a read-only NAND config read, as libxenon does).
 *
 * Ring (uncached, XTS_NET_RING_BYTES): variable-length entries, 8-byte aligned:
 *   w0 entry bytes (header + payload rounded up to 8) or RING_WRAP (continue at offset 0), w1 type, w2 len, w3 msg,
 *   w4 first frame seq, w5 frame count, then the payload. The producer publishes `head` after the entry is complete; the
 *   consumer publishes `tail` after the whole message is sent and `sent_seq` after every frame (all copies done).
 *
 * Transmitter loop: one frame copy at a time - fence (xts_xc_fence_enter: never while the test core measures), hand the
 * copy to the NIC path (udp_sendto, or the own driver's descriptor), wait for it to be on the wire, leave the fence.
 * Copies of a frame are >= XTS_NET_COPY_GAP_US apart; frames are paced so that at most XTS_NET_FPS copies per second
 * leave on average. After the link was quiet for XTS_NET_WAKE_IDLE_MS (a measurement pause, the run start), the next
 * XTS_NET_WAKE_MS go out at XTS_NET_WAKE_FPS only, so a receiver on Wi-Fi in power save wakes before the AP's buffer for
 * it overflows (xts_net_pace_sent in src/core/netfmt.c). The transmitter never calls malloc or stdio (they belong to
 * the suite thread); its only cached data are its stack, a 1.4 KB staging buffer and a 1.5 KB frame buffer.
 */
#include <stdio.h>
#include <string.h>
#include <ppc/cache.h>
#include <time/time.h>
#include <xenon_nand/xenon_config.h>
#include <xenon_smc/xenon_gpio.h>
#include <xts/xts.h>
#include <xts/net.h>
#include <xts/xcore.h>
#include <core/netfmt.h>
#include <core/core.h>              /* xts_suite_hang_report_json */
/* XTS_NET_OWN_DRIVER (make NET_DRIVER=own): XTS's own TX-only descriptor driver below. Default (0): frames go through
 * libxenon's own driver (drivers/lwip/xenon/netif/enet.c: enet_init/enet_open + its linkoutput), the code proven to
 * transmit on hardware - XTS's own driver had every descriptor complete without a frame ever reaching the network
 * (bring-up 2026-10-18, see git log). Only the driver is used: no DHCP, no ARP, no lwIP timers, RX is never read. */
#ifndef XTS_NET_OWN_DRIVER
#define XTS_NET_OWN_DRIVER 0
#endif
/* Seconds to wait after link-up before the first frame (make NET_SETTLE_S=). Bringing the NIC up resets the PHY, so
 * the switch/router port sees link down/up and may not forward for several seconds (hardware bring-up 2026-10-18:
 * ~6.5 s in which nothing from the console reached the PC, lwIP's retried DHCP included; every XTS frame had been sent
 * inside that window). */
#ifndef XTS_NET_SETTLE_S
#define XTS_NET_SETTLE_S 10
#endif
#if !XTS_NET_OWN_DRIVER
#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/ip.h>
#include <lwip/ip_addr.h>
#include <lwip/udp.h>
#include <netif/etharp.h>
#include <network/network.h>
/* Default transport: libxenon's network_init() (DHCP, as the old isa/hwtest harness - the one path that delivered frames
 * on hardware during bring-up) and lwIP UDP. The XTS frame format is unchanged (header + payload in the UDP datagram).
 * lwIP needs its receive processing for ARP and the DHCP lease: network_poll() runs only on the transmitter, only
 * between measurements (inside the fence). The NIC may still store incoming frames in memory during a measurement. */
extern struct netif netif;                               /* libxenon drivers/network/network.c */
static struct udp_pcb *upcb;
static ip_addr_t udst;
static int dhcp_ok;
#endif
#include <plat/xcore_console.h>

#ifndef XTS_NET_COPIES
#define XTS_NET_COPIES 2
#endif
#ifndef XTS_NET_FPS
#define XTS_NET_FPS 2000
#endif
#ifndef XTS_NET_COPY_GAP_US
#define XTS_NET_COPY_GAP_US 50
#endif
/* wake window (make NET_WAKE_MS= NET_WAKE_IDLE_MS= NET_WAKE_FPS=; NET_WAKE_MS=0 disables it): after XTS_NET_WAKE_IDLE_MS
 * without a frame, XTS_NET_WAKE_MS at XTS_NET_WAKE_FPS copies per second. 200 copies/s put at most ~24 copies into the
 * AP's power-save buffer before a dozing receiver wakes at the next beacon (~100 ms; on hardware 63 and 96 copies fitted
 * and the receiver was awake again 70-120 ms after the burst began). A 500 ms window at 200 copies/s delays a CRUMB queued
 * behind 16 RECs by at most ~500 ms, inside CRUMB_WAIT_US (src/core/main.c). */
#ifndef XTS_NET_WAKE_MS
#define XTS_NET_WAKE_MS 150
#endif
#ifndef XTS_NET_WAKE_IDLE_MS
#define XTS_NET_WAKE_IDLE_MS 50
#endif
#ifndef XTS_NET_WAKE_FPS
#define XTS_NET_WAKE_FPS 200
#endif
/* the window grows with the silence before it, up to XTS_NET_WAKE_MAX_MS (make NET_WAKE_MAX_MS=; = NET_WAKE_MS: fixed) */
#ifndef XTS_NET_WAKE_MAX_MS
#define XTS_NET_WAKE_MAX_MS 500
#endif
#ifndef XTS_NET_RING_BYTES
#define XTS_NET_RING_BYTES (4u << 20)
#endif
#if XTS_NET_COPIES < 1 || XTS_NET_COPIES > 4
#error "XTS_NET_COPIES must be 1..4"
#endif
_Static_assert((XTS_NET_RING_BYTES & (XTS_NET_RING_BYTES - 1)) == 0, "ring size must be a power of two");

#define TX_DESC 16
#define RX_DESC 16
#define BUF_BYTES 1536                  /* >= enet.c's MTU 1528, a multiple of 128 */
#define RX_MTU 1528
#define RING_WRAP 0xFFFFFFFFu
#define ENT_HDR 24
#define RING_TIMEOUT_MS 2000            /* xts_net_send: longest wait for ring space */
#define TX_DONE_TIMEOUT_US 3000         /* one frame: 1478 bytes are ~120 us on the 100 Mbit wire */
#define FENCE_SAFETY_MS 300000          /* a measurement longer than this: send anyway (counted) */
#define MDIO_TIMEOUT_US 20000
#define PHY_RESET_TIMEOUT_MS 2000
#define LINK_TIMEOUT_MS 10000

#define NIC(off) ((volatile uint32_t *)(0xea001400u + (off)))   /* little-endian registers: bswap like enet.c */
static inline uint32_t bswap(uint32_t v) { return __builtin_bswap32(v); }
static inline void sync_(void) { __asm__ volatile("sync" ::: "memory"); }

/* ---- shared state (uncached) ---- */
typedef struct netq {
  volatile uint32_t head, last_seq, pad0[30];          /* producer (suite) */
  volatile uint32_t tail, sent_seq, ready, pad1[29];   /* consumer (transmitter); ready: set by xts_net_init */
  volatile uint32_t frames_hi, frames_lo, copies_hi, copies_lo, bytes_hi, bytes_lo, send_failures, tx_timeouts,
      fence_waits, fence_ticks_hi, fence_ticks_lo, fence_overrun, messages_sent, link_up, crash_reported, wakes, pad2[16];
} netq_t;
static netq_t *q;
static volatile uint32_t *ring;                          /* uncached, XTS_NET_RING_BYTES */
static volatile uint32_t *txd, *rxd;                     /* descriptors: 4 words each, little-endian */
static uint8_t *txbuf, *rxbuf;                           /* uncached buffers */
static int t_wptr;                                       /* next TX descriptor (transmitter) */
static uint8_t mac[6];
static uint64_t nonce;
static int up;

/* producer state (suite thread only) */
static uint32_t p_head, p_next_seq = 1, p_next_msg = 1;
static uint64_t p_messages, p_ring_full_waits, p_send_failures;

static void add64(volatile uint32_t *hi, volatile uint32_t *lo, uint32_t v)
{
  uint32_t l = *lo + v;
  if (l < v) *hi = *hi + 1;
  *lo = l;
}
static uint64_t get64(volatile uint32_t *hi, volatile uint32_t *lo) { return ((uint64_t)*hi << 32) | *lo; }

static int timed_out(uint64_t t0, uint64_t us) { return xts_xc_tb() - t0 > xts_xc_us_ticks(us); }

/* ---- NIC ---- */
void xts_netc_quiesce(void)
{
  *NIC(0x24) = 0;            /* 0xea001424: interrupts off */
  *NIC(0x00) = 0;            /* TX off (enet_quiesce) */
  *NIC(0x10) = 0;            /* RX off */
  sync_();
}

/* libxenon's exit path (xenon_syscalls.c shutdown_drivers) calls enet_quiesce: defined here, so libxenon's enet.o -
 * and lwIP with it - stays out of the link */
/* weak: the default transport links libxenon's enet.o, whose enet_quiesce then wins; NET_DRIVER=own builds keep this
 * one, so enet.o (and lwIP) stay out of the link */
__attribute__((weak)) void enet_quiesce(void) { xts_netc_quiesce(); }

static int mdio_wait(void)
{
  uint64_t t0 = xts_xc_tb();
  while (bswap(*NIC(0x44)) & 0x10)
    if (timed_out(t0, MDIO_TIMEOUT_US)) return -1;
  return 0;
}
static int phy_read(int addr, uint16_t *v)
{
  *NIC(0x44) = bswap((uint32_t)(addr << 11) | 0x50);
  mdelay(1);                                             /* as enet.c: the busy bit is not set at once */
  if (mdio_wait() < 0) return -1;
  *v = (uint16_t)(bswap(*NIC(0x44)) >> 16);
  return 0;
}
static int phy_write(int addr, uint16_t data)
{
  *NIC(0x44) = bswap((uint32_t)(addr << 11) | 0x70 | ((uint32_t)data << 16));
  mdelay(1);
  return mdio_wait();
}

static void tx_init(void)
{
  for (int i = 0; i < TX_DESC; i++) {
    txd[i * 4 + 0] = 0;
    txd[i * 4 + 1] = 0;
    txd[i * 4 + 2] = 0;
    txd[i * 4 + 3] = i == TX_DESC - 1 ? bswap(0x80000000u) : 0;
  }
  sync_();
  uint32_t pa = xts_uc_phys((const void *)txd);           /* EA & 0x1FFFFFFF: right for the 0x8/0x9 and 0xA/0xB aliases */
  *NIC(0x00) = bswap(0x00001c00);
  *NIC(0x04) = bswap(pa);
  *NIC(0x00) = bswap(0x00011c00);
  *NIC(0x04) = bswap(pa);
  *NIC(0x00) = bswap(0x00001c00);
}

static void rx_init(void)
{
  for (int i = 0; i < RX_DESC; i++) {
    rxd[i * 4 + 0] = 0;
    rxd[i * 4 + 1] = bswap(0xc0000000u);
    rxd[i * 4 + 2] = bswap(xts_uc_phys(rxbuf + i * BUF_BYTES));
    rxd[i * 4 + 3] = bswap(RX_MTU | (i == RX_DESC - 1 ? 0x80000000u : 0));
  }
  sync_();
  *NIC(0x14) = bswap(xts_uc_phys((const void *)rxd));   /* RX queue start */
  *NIC(0x40) = 0x1100004;
  *NIC(0x50) = 0;
}

__attribute__((unused)) static int link_status(void)
{
  uint16_t v;
  return phy_read(1, &v) == 0 && (v & 4);
}

__attribute__((unused)) static int nic_open(void)
{
  *NIC(0x24) = 0;                                        /* interrupts off */
  *NIC(0x28) = 0x01805508;                               /* reset pulse: XeLL's NIC may still be doing DMA */
  udelay(100);
  *NIC(0x28) = 0x01005508;
  *NIC(0x44) = bswap(4);
  udelay(100);
  *NIC(0x44) = bswap(0);
  *NIC(0x78) = 0xf2050000;                               /* MTU */
  *NIC(0x50) = 0x60230000;
  *NIC(0x78) = 0xF2050000u | ((uint32_t)mac[0] << 8) | mac[1];
  *NIC(0x7c) = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
  *NIC(0x60) = 0x380e0000u | ((uint32_t)mac[0] << 8) | mac[1];
  *NIC(0x64) = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
  *NIC(0x68) = 0;                                        /* multicast hash */
  *NIC(0x6c) = 0;
  *NIC(0x00) = 0x001c0000;
  *NIC(0x10) = bswap(0x00101c00);                        /* RX configured, NOT enabled */
  *NIC(0x40) = 0x01190004;
  tx_init();
  rx_init();
  *NIC(0x40) = 0x01100004;
  /* PHY out of reset (SMC GPIO, as enet_open), soft reset, then the link */
  xenon_gpio_control(0, 0, 0x10);
  xenon_gpio_control(4, 0, 0x10);
  int r = 0;
  if (phy_write(0, 0x9000) < 0) r = -1;
  uint64_t t0 = xts_xc_tb();
  uint16_t v = 0x8000;
  while (r == 0 && (phy_read(0, &v) < 0 || (v & 0x8000))) {
    if (timed_out(t0, PHY_RESET_TIMEOUT_MS * 1000ull)) { r = -2; break; }
    mdelay(10);
  }
  return r;                                              /* RX/TX are started by nic_start once the link is up */
}

/* Start the MAC - AFTER the PHY reports link, exactly in libxenon enet_open's order. Enabling TX right after the PHY
 * reset (before autonegotiation finished) left the NIC "sending" frames that never reached the network (hardware
 * bring-up 2026-10-18: every descriptor completed, nothing arrived, while enet.c's order worked on the same boot path).
 * RX is enabled as enet.c does (the configuration proven to transmit); received frames are never read, so the RX ring
 * simply fills (16 frames) and the NIC stops storing them. */
__attribute__((unused)) static void nic_start(void)
{
  *NIC(0x28) = 0x01005508;
  *NIC(0x10) = bswap(0x00101c11);                        /* RX enable, as enet_open */
  *NIC(0x00) = bswap(0x00001c01);                        /* TX on */
  sync_();
}

void xts_netc_mac(uint8_t m[6]) { memcpy(m, mac, 6); }
int xts_netc_ready(void) { return q && q->ready; }
uint64_t xts_net_run_nonce(void) { return nonce; }

/* stats at the start of the current run (xts_net_new_run): xts_net_stats reports this run's share */
static xts_net_stats_t base;

static uint64_t make_nonce(void)
{
  uint64_t m48 = 0;
  for (int i = 0; i < 6; i++) m48 = (m48 << 8) | mac[i];
  uint64_t n = (m48 << 16) ^ xts_xc_tb();
  return n ? n : 1;
}

uint64_t xts_net_new_run(void)
{
  if (!up) return nonce;
  xts_net_drain(20u * 1000u * 1000u);                    /* the previous run's END is on the wire */
  uint64_t n = make_nonce();
  while (n == nonce) n = make_nonce();
  xts_net_stats_t raw;
  memset(&base, 0, sizeof base);
  xts_net_stats(&raw);                                   /* totals so far */
  base = raw;
  nonce = n;
  p_next_seq = 1;
  p_next_msg = 1;
  q->last_seq = 0;
  q->sent_seq = 0;                                       /* the transmitter is idle: nothing queued after the drain */
  sync_();
  return nonce;
}

int xts_net_init(void)
{
  if (up) return 0;
  xts_xc_init_early();
  if (!q) {
    q = xts_uc_alloc(sizeof *q);
    ring = xts_uc_alloc(XTS_NET_RING_BYTES);
    txd = xts_uc_alloc(TX_DESC * 16);
    rxd = xts_uc_alloc(RX_DESC * 16);
    txbuf = xts_uc_alloc(TX_DESC * BUF_BYTES);
    rxbuf = xts_uc_alloc(RX_DESC * BUF_BYTES);
    if (!q || !ring || !txd || !rxd || !txbuf || !rxbuf) {
      q = NULL;
      xts_plat_screen("XTS net: no uncached memory for the NIC rings");
      return -1;
    }
  }
  xenon_config_get_mac_addr(mac);
  if (!(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) || (mac[0] & 1)) {   /* invalid: libxenon's default */
    static const uint8_t dmac[6] = { 0x00, 0x22, 0x48, 0xFF, 0xFF, 0xFF };
    memcpy(mac, dmac, 6);
  }
  nonce = make_nonce();
  char line[160];
#ifdef XTS_NET_DEST_IP
  {
    static const uint8_t dmac_dest[6] = { XTS_NET_DEST_MAC };
    xts_netfmt_set_dest(dmac_dest, (uint32_t)XTS_NET_DEST_IP);
    snprintf(line, sizeof line, "XTS net: MAC %02X:%02X:%02X:%02X:%02X:%02X  IP 169.254.%u.%u  UDP unicast to %u.%u.%u.%u port %u",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], mac[4], mac[5], (unsigned)(XTS_NET_DEST_IP >> 24) & 255,
             (unsigned)(XTS_NET_DEST_IP >> 16) & 255, (unsigned)(XTS_NET_DEST_IP >> 8) & 255, (unsigned)XTS_NET_DEST_IP & 255,
             XTS_NET_PORT);
  }
#else
  snprintf(line, sizeof line, "XTS net: MAC %02X:%02X:%02X:%02X:%02X:%02X  IP 169.254.%u.%u  UDP broadcast port %u", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5], mac[4], mac[5], XTS_NET_PORT);
#endif
  xts_plat_screen(line);
#if !XTS_NET_OWN_DRIVER
  {
    xts_plat_screen("XTS net: network_init (libxenon lwIP, DHCP)...");
    int nr = network_init();
    dhcp_ok = netif.ip_addr.addr != 0 && nr == 0;
    int link = (netif.flags & NETIF_FLAG_LINK_UP) != 0;
    q->link_up = (uint32_t)link;
    if (!link) {
      xts_plat_screen("XTS net: NO ETHERNET LINK: connect the console to the network and reboot");
      return -3;
    }
    memcpy(mac, netif.hwaddr, 6);
    upcb = udp_new();
    if (!upcb || udp_bind(upcb, IP_ADDR_ANY, XTS_NET_PORT) != ERR_OK) {
      xts_plat_screen("XTS net: udp_new/udp_bind failed");
      return -2;
    }
#ifdef XTS_NET_DEST_IP
    IP4_ADDR(&udst, (XTS_NET_DEST_IP >> 24) & 255, (XTS_NET_DEST_IP >> 16) & 255, (XTS_NET_DEST_IP >> 8) & 255,
             XTS_NET_DEST_IP & 255);
    /* resolve the PC's MAC before the first datagram (lwIP would otherwise queue only one packet per pending ARP) */
    struct eth_addr *ea;
    ip_addr_t *ia;
    uint64_t t0 = xts_xc_tb();
    int resolved = 0;
    while (!timed_out(t0, 3000000ull)) {
      if (etharp_find_addr(&netif, &udst, &ea, &ia) >= 0) { resolved = 1; break; }
      etharp_request(&netif, &udst);
      uint64_t t1 = xts_xc_tb();
      while (!timed_out(t1, 200000ull)) network_poll();
    }
#else
    udst = *IP_ADDR_BROADCAST;
    int resolved = 1;
#endif
    snprintf(line, sizeof line, "XTS net: %s IP %s -> %s port %u (%s), %d copies/frame, <= %d frames/s",
             dhcp_ok ? "DHCP" : "static", ipaddr_ntoa(&netif.ip_addr), ipaddr_ntoa(&udst), XTS_NET_PORT,
             resolved ? "PC MAC resolved" : "PC MAC NOT resolved yet", XTS_NET_COPIES, XTS_NET_FPS);
    xts_plat_screen(line);
  }
#else
  int r = nic_open();
  if (r < 0) {
    snprintf(line, sizeof line, "XTS net: PHY did not answer (%s)", r == -1 ? "MDIO timeout" : "reset bit stuck");
    xts_plat_screen(line);
    return -2;
  }
  uint64_t t0 = xts_xc_tb();
  int link = link_status();
  if (!link) xts_plat_screen("XTS net: waiting for the Ethernet link (10 s)...");
  while (!link && !timed_out(t0, LINK_TIMEOUT_MS * 1000ull)) {
    mdelay(250);
    link = link_status();
  }
  q->link_up = (uint32_t)link;
  if (!link) {
    xts_plat_screen("XTS net: NO ETHERNET LINK: connect the console to the PC running tools/xts_recv.py and reboot");
    return -3;
  }
  nic_start();
  snprintf(line, sizeof line, "XTS net: link up, TX %d descriptors, %d copies/frame, <= %d frames/s, ring %u KB",
           TX_DESC, XTS_NET_COPIES, XTS_NET_FPS, (unsigned)(XTS_NET_RING_BYTES >> 10));
  xts_plat_screen(line);
#endif
  int settle = XTS_NET_SETTLE_S;
#if !XTS_NET_OWN_DRIVER
  if (dhcp_ok) settle = 0;                               /* DHCP answered: the port forwards again */
#endif
  for (int left = settle; left > 0; left--) {   /* let the network start forwarding again (see above) */
    snprintf(line, sizeof line, "XTS net: waiting %d s for the network to forward after the link reset", left);
    xts_plat_status(line);
    mdelay(1000);
  }
  if (settle > 0) {
    snprintf(line, sizeof line, "XTS net: waited %d s after link-up (NET_SETTLE_S); sending", settle);
    xts_plat_screen(line);
  }
  sync_();
  q->ready = 1;
  sync_();
  up = 1;
  return 0;
}

/* ---- producer (suite thread) ---- */
static uint32_t ring_free(void) { return XTS_NET_RING_BYTES - (p_head - q->tail); }

static int wait_space(uint32_t need)
{
  if (ring_free() >= need) return 0;
  p_ring_full_waits++;
  uint64_t t0 = xts_xc_tb();
  while (ring_free() < need) {
    if (timed_out(t0, RING_TIMEOUT_MS * 1000ull)) return -1;
    xts_xc_spin_ticks(500);
  }
  return 0;
}

/* big-endian words from bytes (any alignment) into the uncached ring: aligned word stores only */
static void ring_copy(uint32_t off, const uint8_t *src, uint32_t len)
{
  volatile uint32_t *d = ring + off / 4;
  uint32_t n = len / 4, i;
  for (i = 0; i < n; i++, src += 4) d[i] = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) | src[3];
  uint32_t rest = len & 3;
  if (rest) {
    uint32_t w = 0;
    for (uint32_t k = 0; k < rest; k++) w |= (uint32_t)src[k] << (24 - 8 * k);
    d[i] = w;
  }
}

uint32_t xts_net_send(int type, const void *payload, uint32_t len)
{
  if (!up || len > XTS_NET_MAX_MESSAGE || (len && !payload)) { p_send_failures++; return 0; }
  uint16_t frags = xts_netfmt_frag_count(len);
  uint32_t need = ENT_HDR + ((len + 7u) & ~7u);
  uint32_t pos = p_head & (XTS_NET_RING_BYTES - 1), tail_room = XTS_NET_RING_BYTES - pos;
  uint32_t total = tail_room < need ? tail_room + need : need;
  if (wait_space(total) < 0) { p_send_failures++; return 0; }
  if (tail_room < need) {
    ring[pos / 4] = RING_WRAP;
    p_head += tail_room;
    pos = 0;
  }
  uint32_t first = p_next_seq;
  volatile uint32_t *e = ring + pos / 4;
  e[1] = (uint32_t)type;
  e[2] = len;
  e[3] = p_next_msg;
  e[4] = first;
  e[5] = frags;
  if (len) ring_copy(pos + ENT_HDR, payload, len);
  e[0] = need;
  sync_();                                               /* the whole entry before the new head */
  p_head += need;
  p_next_seq += frags;
  p_next_msg++;
  p_messages++;
  q->last_seq = first + frags - 1;
  q->head = p_head;
  sync_();
  xts_xc_beat(XTS_PIR_SUITE);
  return first + frags - 1;
}

static int seq_done(uint32_t seq) { return (int32_t)(q->sent_seq - seq) >= 0; }

int xts_net_wait_sent(uint32_t seq, uint32_t timeout_us)
{
  if (!up) return -1;
  uint64_t t0 = xts_xc_tb();
  while (!seq_done(seq)) {
    if (timed_out(t0, timeout_us)) return seq_done(seq) ? 0 : -1;
    xts_xc_spin_ticks(100);
  }
  return 0;
}

int xts_net_drain(uint32_t timeout_us)
{
  if (!up) return -1;
  uint64_t t0 = xts_xc_tb();
  while (q->tail != p_head || !seq_done(p_next_seq - 1)) {
    if (timed_out(t0, timeout_us)) return -1;
    xts_xc_spin_ticks(500);
  }
  return 0;
}

void xts_net_stats(xts_net_stats_t *s)
{
  memset(s, 0, sizeof *s);
  s->messages = p_messages;
  s->ring_full_waits = p_ring_full_waits;
  s->last_seq = p_next_seq - 1;
  if (!q) return;
  s->frames = get64(&q->frames_hi, &q->frames_lo);
  s->frame_copies = get64(&q->copies_hi, &q->copies_lo);
  s->bytes = get64(&q->bytes_hi, &q->bytes_lo);
  s->send_failures = p_send_failures + q->send_failures;
  s->tx_timeouts = q->tx_timeouts;
  s->fence_waits = q->fence_waits;
  s->fence_wait_ticks = get64(&q->fence_ticks_hi, &q->fence_ticks_lo);
  s->wake_windows = q->wakes;
  s->link_up = (int)q->link_up;
  s->messages -= base.messages;                          /* this run's share (all zero in the first run) */
  s->ring_full_waits -= base.ring_full_waits;
  s->frames -= base.frames;
  s->frame_copies -= base.frame_copies;
  s->bytes -= base.bytes;
  s->send_failures -= base.send_failures;
  s->tx_timeouts -= base.tx_timeouts;
  s->fence_waits -= base.fence_waits;
  s->fence_wait_ticks -= base.fence_wait_ticks;
  s->wake_windows -= base.wake_windows;
}

/* ---- transmitter (PIR 4) ---- */
static uint32_t stage[XTS_NET_MAX_PAYLOAD / 4 + 2];      /* one fragment's payload (cached, aligned) */
static uint8_t fbuf[XTS_NET_MAX_FRAME + 8] __attribute__((aligned(8)));
static xts_net_pace_t t_pace;

static int desc_free(int d) { return !(bswap(txd[d * 4 + 1]) & 0x80000000u); }

/* wait (bounded) until the test core is not measuring and enter the fence; 1 = entered (always, eventually) */
static void fence_wait_enter(void)
{
  if (xts_xc_fence_enter()) return;
  q->fence_waits = q->fence_waits + 1;
  uint64_t t0 = xts_xc_tb();
  xts_xc_prio_low();
  while (!xts_xc_fence_enter()) {
    xts_xc_spin_ticks(250);                              /* ~5 us between uncached polls: quiet during measurements */
    if (timed_out(t0, FENCE_SAFETY_MS * 1000ull)) {
      q->fence_overrun = q->fence_overrun + 1;
      break;                                             /* tx_busy is 0 here; sending anyway */
    }
  }
  xts_xc_prio_medium();
  add64(&q->fence_ticks_hi, &q->fence_ticks_lo, (uint32_t)(xts_xc_tb() - t0));
}

/* one copy of a frame, entirely inside the fence (no uncached access for this frame while the test core measures):
 * copy 0 stages the fragment's payload from the ring, every copy builds its frame (netfmt), copies it into the
 * descriptor's buffer, hands it to the NIC and waits until the NIC released the descriptor (DMA done) */
static int send_copy(const xts_net_hdr_t *h, volatile const uint32_t *ring_src)
{
  int d = t_wptr, r = 0;
  fence_wait_enter();
  if (ring_src)
    for (uint32_t w = 0; w < (h->len + 3u) / 4; w++) stage[w] = ring_src[w];
  uint32_t flen = xts_netfmt_eth_frame(fbuf, mac, (uint16_t)h->seq, h, stage);
#if !XTS_NET_OWN_DRIVER
  (void)d;
  (void)flen;
  {
    static uint8_t body[XTS_NET_HDR_BYTES + XTS_NET_MAX_PAYLOAD + 8];
    uint32_t blen = xts_netfmt_udp_payload(body, h, stage);
    struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, (u16_t)blen, PBUF_RAM);
    if (!pb) {
      r = -1;
    } else {
      memcpy(pb->payload, body, blen);
      if (udp_sendto(upcb, pb, &udst, XTS_NET_PORT) != ERR_OK) r = -1;
      pbuf_free(pb);
      uint64_t t0 = xts_xc_tb();                         /* the driver reports no completion: wait out the wire time */
      uint64_t wire = xts_xc_us_ticks((blen + 42u) / 12u + 30u);
      while (xts_xc_tb() - t0 < wire) { }
    }
    if (r < 0) q->tx_timeouts = q->tx_timeouts + 1;
    xts_xc_fence_leave();
    add64(&q->copies_hi, &q->copies_lo, 1);
    add64(&q->bytes_hi, &q->bytes_lo, blen + 42u);
    return r;
  }
#endif
  if (!desc_free(d)) {                                   /* should never happen: every copy is waited for */
    uint64_t t0 = xts_xc_tb();
    while (!desc_free(d))
      if (timed_out(t0, TX_DONE_TIMEOUT_US)) { q->tx_timeouts = q->tx_timeouts + 1; r = -1; break; }
  }
  if (r < 0) { xts_xc_fence_leave(); return -1; }
  volatile uint32_t *dst = (volatile uint32_t *)(txbuf + d * BUF_BYTES);
  const uint32_t *src = (const uint32_t *)fbuf;
  for (uint32_t i = 0; i < (flen + 3) / 4; i++) dst[i] = src[i];
  txd[d * 4 + 0] = bswap(flen);
  txd[d * 4 + 2] = bswap(xts_uc_phys((const void *)dst));
  txd[d * 4 + 3] = bswap(flen | (d == TX_DESC - 1 ? 0x80000000u : 0));
  sync_();
  txd[d * 4 + 1] = bswap(0xc0230000u);                   /* ownership to the NIC, last */
  sync_();
  *NIC(0x00) = *NIC(0x00) | bswap(0x10);                 /* Q0_START */
  sync_();
  t_wptr = (d + 1) % TX_DESC;
  uint64_t t0 = xts_xc_tb();
  while (!desc_free(d))
    if (timed_out(t0, TX_DONE_TIMEOUT_US)) { q->tx_timeouts = q->tx_timeouts + 1; r = -1; break; }
  xts_xc_fence_leave();
  add64(&q->copies_hi, &q->copies_lo, 1);
  add64(&q->bytes_hi, &q->bytes_lo, flen);
  return r;
}

static void send_frame(const xts_net_hdr_t *h0, volatile const uint32_t *ring_src)
{
  const xts_net_pace_cfg_t pc = { xts_xc_us_ticks(1000000u / XTS_NET_FPS) * XTS_NET_COPIES,
                                  xts_xc_us_ticks(1000000u / XTS_NET_WAKE_FPS) * XTS_NET_COPIES,
                                  xts_xc_us_ticks(XTS_NET_WAKE_IDLE_MS * 1000u), xts_xc_us_ticks(XTS_NET_WAKE_MS * 1000u),
                                  xts_xc_us_ticks(XTS_NET_WAKE_MAX_MS * 1000u) };
  while (xts_xc_tb() < t_pace.next) ;                   /* pacing (bounded: one frame period) */
  uint64_t start = xts_xc_tb(), last = 0, first = start;
  for (int c = 0; c < XTS_NET_COPIES; c++) {
    xts_net_hdr_t h = *h0;
    h.copy = (uint8_t)c;
    h.flags = c ? XTS_NET_FLAG_REPEAT : 0;
    if (c) while (xts_xc_tb() - last < xts_xc_us_ticks(XTS_NET_COPY_GAP_US)) ;
    last = xts_xc_tb();
    if (send_copy(&h, c ? NULL : ring_src) < 0) q->send_failures = q->send_failures + 1;
    if (!c) first = xts_xc_tb();                         /* copy 0 left: after any measurement fence */
  }
  add64(&q->frames_hi, &q->frames_lo, 1);
  uint64_t wakes = t_pace.wakes;
  xts_net_pace_sent(&t_pace, &pc, start, first, xts_xc_tb());
  if (t_pace.wakes != wakes) q->wakes = (uint32_t)t_pace.wakes;
}

/* the suite core crashed: nobody else will say so. Once the ring is drained, the transmitter becomes the producer
 * (the suite is gone) and queues a CRASH report from PIR 2's crash slot. */
static char *put_s(char *o, const char *s)
{
  while (*s) *o++ = *s++;
  return o;
}
static char *put_hex(char *o, const char *key, uint64_t v, int digits)
{
  o = put_s(o, key);
  *o++ = '"';
  for (int i = digits - 1; i >= 0; i--) *o++ = "0123456789ABCDEF"[(v >> (4 * i)) & 15];
  *o++ = '"';
  return o;
}
static void report_suite_crash(void)
{
  static char buf[1600];
  const xts_xc_crash_t *s = xts_xc_crash_slot(XTS_PIR_SUITE);
  char *o = buf;
  o = put_s(o, "{\"pir\":2,\"reporter\":\"transmitter\",\"note\":\"the suite core crashed\"");
  o = put_hex(o, ",\"vector\":", s->vector, 4);
  o = put_hex(o, ",\"srr0\":", s->srr0, 16);
  o = put_hex(o, ",\"srr1\":", s->srr1, 16);
  o = put_hex(o, ",\"dar\":", s->dar, 16);
  o = put_hex(o, ",\"dsisr\":", s->dsisr, 8);
  o = put_hex(o, ",\"lr\":", s->lr, 16);
  o = put_hex(o, ",\"ctr\":", s->ctr, 16);
  o = put_hex(o, ",\"cr\":", s->cr, 8);
  o = put_hex(o, ",\"xer\":", s->xer, 16);
  o = put_s(o, ",\"gpr\":[");
  for (int i = 0; i < 32; i++) o = put_hex(o, i ? "," : "", s->gpr[i], 16);
  o = put_s(o, "]");
  o = put_hex(o, ",\"tb\":", s->tb, 16);
  o = put_s(o, "}");
  xts_net_send(XTS_PK_CRASH, buf, (uint32_t)(o - buf));
}

/* suite watchdog: between HELLO and END the suite core beats at least every ~1 ms (xts_xc_call's wait loop) or with every
 * call; XTS_NET_SUITE_STALL_MS without a beat (no crash slot) means it stopped - a livelock or a loop that never returns,
 * which nobody else could report. Far above any legitimate pause (calib's 2 s RTC interval, 2 s ring waits). */
#ifndef XTS_NET_SUITE_STALL_MS
#define XTS_NET_SUITE_STALL_MS 10000
#endif
static int run_live, run_ended, suite_stall_reported;

static void report_suite_hang(uint64_t stalled_ticks)
{
  static char buf[4096];
  int n = xts_suite_hang_report_json(buf, sizeof buf, stalled_ticks * 1000u / xts_xc_us_ticks(1000000u));
  xts_net_send(XTS_PK_HANG, buf, (uint32_t)n);
}

static int tx_one_message(void)
{
  uint32_t tail = q->tail, pos = tail & (XTS_NET_RING_BYTES - 1);
  volatile uint32_t *e = ring + pos / 4;
  uint32_t n = e[0];
  if (n == RING_WRAP) {
    q->tail = tail + (XTS_NET_RING_BYTES - pos);
    sync_();
    return 1;
  }
  uint32_t type = e[1], len = e[2], msg = e[3], seq = e[4], frags = e[5];
  if (type == XTS_PK_HELLO) run_live = 1, run_ended = 0;  /* a multi-pass session: every pass has HELLO ... END */
  if (type == XTS_PK_END) run_ended = 1;
  if (n < ENT_HDR || n > ENT_HDR + XTS_NET_MAX_MESSAGE + 8 || len > XTS_NET_MAX_MESSAGE ||
      frags != xts_netfmt_frag_count(len) || n > XTS_NET_RING_BYTES - pos) {
    q->send_failures = q->send_failures + 1;             /* corrupt entry: drop everything queued so far */
    q->tail = q->head;
    sync_();
    return 0;
  }
  for (uint32_t i = 0; i < frags; i++) {
    xts_net_hdr_t h = { (uint8_t)type, 0, 0, nonce, seq + i, msg, (uint16_t)i, (uint16_t)frags,
                        xts_netfmt_frag_len(len, (uint16_t)i) };
    send_frame(&h, e + ENT_HDR / 4 + i * (XTS_NET_MAX_PAYLOAD / 4));
    q->sent_seq = seq + i;
    sync_();
    xts_xc_beat(XTS_PIR_NET);
  }
  q->messages_sent = q->messages_sent + 1;
  q->tail = tail + n;
  sync_();
  return 1;
}

void xts_netc_tx_main(void)
{
  uint64_t beat = 0;
  for (;;) {                                             /* until xts_net_init finished (heartbeat meanwhile) */
    if (q && q->ready) break;
    xts_xc_prio_low();
    xts_xc_spin_ticks(5000);
    xts_xc_beat(XTS_PIR_NET);
  }
  xts_xc_prio_medium();
  for (;;) {
    if (q->tail != q->head) {
      xts_xc_prio_medium();
      tx_one_message();
      continue;
    }
    if (!q->crash_reported && xts_xc_crash_slot(XTS_PIR_SUITE)->valid) {
      q->crash_reported = 1;
      report_suite_crash();
      continue;
    }
    if (run_live && !run_ended && !suite_stall_reported && !q->crash_reported) {
      static uint64_t hb_val, hb_tb;
      uint64_t hb = xts_xc_heartbeat(XTS_PIR_SUITE), t = xts_xc_tb();
      if (hb != hb_val || !hb_tb) { hb_val = hb; hb_tb = t; }
      else if (t - hb_tb > xts_xc_us_ticks((uint64_t)XTS_NET_SUITE_STALL_MS * 1000u)) {
        suite_stall_reported = 1;                          /* the suite cannot: the transmitter reports it */
        report_suite_hang(t - hb_tb);
        continue;
      }
    }
    xts_xc_prio_low();                                    /* idle: ~20 us between uncached polls */
    xts_xc_spin_ticks(1000);
    uint64_t now = xts_xc_tb();
#if !XTS_NET_OWN_DRIVER
    static uint64_t last_poll;
    if (now - last_poll > xts_xc_us_ticks(20000)) {        /* lwIP housekeeping (ARP, DHCP lease), never mid-measurement */
      last_poll = now;
      if (xts_xc_fence_enter()) {
        xts_xc_prio_medium();
        network_poll();
        xts_xc_fence_leave();
      }
    }
#endif
    if (now - beat > xts_xc_us_ticks(1000)) { beat = now; xts_xc_beat(XTS_PIR_NET); }
  }
}
