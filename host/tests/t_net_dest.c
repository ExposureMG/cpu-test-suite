/* Frame destination: broadcast by default, unicast after xts_netfmt_set_dest (make NET_DEST=...). */
#include <string.h>
#include <xts/xts.h>
#include <xts/net.h>
#include <core/netfmt.h>
#include "htest.h"

HTEST(net_frame_destination)
{
  static uint8_t frame[XTS_NET_MAX_FRAME];
  const uint8_t src[6] = { 0x00, 0x1D, 0xD8, 0x12, 0x34, 0x56 };
  xts_net_hdr_t h;
  memset(&h, 0, sizeof h);
  h.type = XTS_PK_LOG; h.seq = 1; h.msg = 1; h.frag_cnt = 1; h.len = 3;
  uint32_t n = xts_netfmt_eth_frame(frame, src, 1, &h, "abc");
  HT_CHECK(n >= 60);
  static const uint8_t bc[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
  HT_CHECK(!memcmp(frame, bc, 6));                                 /* default: Ethernet broadcast */
  HT_CHECK(frame[14 + 16] == 255 && frame[14 + 19] == 255);       /* IPv4 dst 255.255.255.255 */
  const uint8_t pc[6] = { 0x08, 0xD2, 0x3E, 0x0F, 0x6C, 0x2A };
  xts_netfmt_set_dest(pc, 0xC0A80465u);                           /* 192.168.4.101 */
  n = xts_netfmt_eth_frame(frame, src, 2, &h, "abc");
  HT_CHECK(!memcmp(frame, pc, 6));
  HT_CHECK(!memcmp(frame + 6, src, 6));
  HT_CHECK(frame[30] == 192 && frame[31] == 168 && frame[32] == 4 && frame[33] == 101);
  HT_EQ_U64(xts_netfmt_ip_checksum(frame + 14, 20), 0);         /* header checksum valid after the change */
  xts_netfmt_set_dest(bc, 0xFFFFFFFFu);                           /* restore for the other tests */
}
