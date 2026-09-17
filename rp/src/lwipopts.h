#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// Common settings used in most of the pico_w examples
// (see https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html for details)

// allow override in some examples
#ifndef NO_SYS
#define NO_SYS 1
#endif

// lwIP's statistics cost flash and RAM, so they are opt-in. Without them
// an exhausted pool or heap inside lwIP is silent: allocations just fail.
// Set to 1 (here or as a compile definition, see DEVOPS_LWIP_STATS in
// CMakeLists.txt) to report the pool counters in the health report.
// Debug builds only; ignored in release.
#ifndef DEVOPS_LWIP_STATS
#define DEVOPS_LWIP_STATS 0
#endif

// allow override in some examples
#ifndef LWIP_SOCKET
#define LWIP_SOCKET 0
#endif
#if PICO_CYW43_ARCH_POLL
#define MEM_LIBC_MALLOC 1
#else
// MEM_LIBC_MALLOC is incompatible with non polling versions
#define MEM_LIBC_MALLOC 0
#endif

#define MEM_ALIGNMENT 4
#define MEM_SIZE 4096

#define MEM_SANITY_CHECK 0
#define MEM_OVERFLOW_CHECK 0

#define MEMP_NUM_PBUF 8
// Measured on hardware with DEVOPS_LWIP_STATS (2026-09-16, build b7f138f):
// a 4 MB upload, a 4 MB download, a listing, a 40-request
// burst, two concurrent clients and 20 aborted transfers, with no pool
// reporting a single allocation failure.
//
// tcp_pcb peaked at 4 of 4 and stayed there: the server closes every
// connection itself, so each request leaves a pcb in TIME_WAIT for 2 x TCP_MSL
// and the pool is permanently full between requests. 6 slots is the 2 HTTP
// connections plus the debug stream plus headroom, and TCP_MSL below drains
// the rest six times faster. (Listening pcbs come from their own pool.)
#define MEMP_NUM_TCP_PCB 6
// Peaked at 9 of 16 with two clients and aborted transfers. TCP_SND_QUEUELEN
// is 8 per connection, so 16 is what two senders can queue; left as is.
#define MEMP_NUM_TCP_SEG 16
#define MEMP_NUM_ARP_QUEUE 2
// Peaked at 2 of 12 even during 4 MB transfers: in poll mode a received pbuf
// is handled and freed before the driver allocates the next one, so the pool
// is only really used for out-of-order segments. Kept at 12 anyway -- this is
// the receive path, and a pool that runs dry drops packets off the air.
#define PBUF_POOL_SIZE 12
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 0
// IGMP is required by the mDNS responder for multicast group membership.
#define LWIP_IGMP 1
#define TCP_MSS 1460
// lwIP's default MSL is 60 s, so a closed connection holds its pcb for 2
// minutes of TIME_WAIT and the 4-slot pool measured full after a handful of
// requests. 10 s matches Booster and drains it in 20 s. The
// risk MSL guards against -- a delayed segment from an old connection landing
// on a new one with the same port pair -- needs a reused ephemeral port within
// the window, which a LAN client does not do.
#define TCP_MSL 10000UL
// Peak lwIP heap use (PBUF_RAM, from libc malloc here) was 12,920 bytes across
// the same scenarios; the 4 x MSS window costs nothing extra in .bss.
#define TCP_WND (4 * TCP_MSS)
#define TCP_SND_BUF (4 * TCP_MSS)
#define TCP_SND_QUEUELEN ((2 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETCONN 0
// The "err" counters reveal a silently failed allocation. Debug builds
// with DEVOPS_LWIP_STATS only; they cost RAM and flash.
#if defined(_DEBUG) && (_DEBUG != 0) && (DEVOPS_LWIP_STATS != 0)
#define MEM_STATS 1
#define MEMP_STATS 1
#else
#define MEM_STATS 0
#define MEMP_STATS 0
#endif
#define SYS_STATS 0
#define LINK_STATS 0
// #define ETH_PAD_SIZE                2
#define LWIP_CHKSUM_ALGORITHM 3
#define LWIP_DHCP 1
#define LWIP_IPV4 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_DNS 1
#define LWIP_TCP_KEEPALIVE 0
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define DHCP_DOES_ARP_CHECK 0
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_DHCP_GET_NTP_SRV 0

// Keyed off _DEBUG, not NDEBUG. Both build types compile as CMake
// Release, which defines NDEBUG, so "#ifndef NDEBUG" left the statistics
// out of every build.
#if defined(_DEBUG) && (_DEBUG != 0) && (DEVOPS_LWIP_STATS != 0)
#define LWIP_DEBUG 1
#define LWIP_STATS 1
#define LWIP_STATS_DISPLAY 1
#endif

#define ETHARP_DEBUG LWIP_DBG_OFF
#define NETIF_DEBUG LWIP_DBG_OFF
#define PBUF_DEBUG LWIP_DBG_OFF
#define API_LIB_DEBUG LWIP_DBG_OFF
#define API_MSG_DEBUG LWIP_DBG_OFF
#define SOCKETS_DEBUG LWIP_DBG_OFF
#define ICMP_DEBUG LWIP_DBG_OFF
#define INET_DEBUG LWIP_DBG_OFF
#define IP_DEBUG LWIP_DBG_OFF
#define IP_REASS_DEBUG LWIP_DBG_OFF
#define RAW_DEBUG LWIP_DBG_OFF
#define MEM_DEBUG LWIP_DBG_OFF
#define MEMP_DEBUG LWIP_DBG_OFF
#define SYS_DEBUG LWIP_DBG_OFF
#define TCP_DEBUG LWIP_DBG_OFF
#define TCP_INPUT_DEBUG LWIP_DBG_OFF
#define TCP_OUTPUT_DEBUG LWIP_DBG_OFF
#define TCP_RTO_DEBUG LWIP_DBG_OFF
#define TCP_CWND_DEBUG LWIP_DBG_OFF
#define TCP_WND_DEBUG LWIP_DBG_OFF
#define TCP_FR_DEBUG LWIP_DBG_OFF
#define TCP_QLEN_DEBUG LWIP_DBG_OFF
#define TCP_RST_DEBUG LWIP_DBG_OFF
#define UDP_DEBUG LWIP_DBG_OFF
#define TCPIP_DEBUG LWIP_DBG_OFF
#define PPP_DEBUG LWIP_DBG_OFF
#define SLIP_DEBUG LWIP_DBG_OFF
#define DHCP_DEBUG LWIP_DBG_OFF

// Custom flags
// #define TCP_FAST_INTERVAL 50
#define TCP_NODELAY 1

#define LWIP_NETIF_API \
  0  //  Not needed. Sequential API, and therefore for platforms with OSes only.
#define LWIP_SOCKET \
  0  //  Not needed. Sequential API, and therefore for platforms with OSes only.

// We do not use the lwIP `httpd` app — ships a custom HTTP/1.1
// server on lwIP's raw TCP API (see rp/src/http_server.c). The
// LWIP_HTTPD_* knobs and HTTPD_FSDATA_FILE are intentionally not set.

// mDNS responder so the device is reachable at <hostname>.local. The
// responder hangs per-netif data off LWIP_NUM_NETIF_CLIENT_DATA, which
// must be ≥ 1 for mdns_resp_add_netif() to work.
#define LWIP_MDNS_RESPONDER 1
#define LWIP_NUM_NETIF_CLIENT_DATA 1
#define MDNS_MAX_SERVICES 1
#define MDNS_RESP_USENETIF_EXTCALLBACK 1
// mDNS schedules several lwIP timers; default MEMP_NUM_SYS_TIMEOUT is
// too small once the responder is on and we still have DHCP/etc.
// Bumped to 32 to mirror md-browser, which runs the same combination
// reliably.
#define MEMP_NUM_SYS_TIMEOUT 32

// Only plain HTTP client: keep ALTCP/TLS disabled to save memory.
#define LWIP_ALTCP 0
#define MEMP_NUM_ALTCP_PCB 0
#define LWIP_ALTCP_TLS 0
#define LWIP_ALTCP_TLS_MBEDTLS 0

// Note bug in lwip with LWIP_ALTCP and LWIP_DEBUG
// https://savannah.nongnu.org/bugs/index.php?62159
// #define LWIP_DEBUG 1
// #undef LWIP_DEBUG
// #define LWIP_DEBUG                  1
// #define MEMP_OVERFLOW_CHECK         2
// #define MEMP_SANITY_CHECK           1

#endif /* __LWIPOPTS_H__ */
