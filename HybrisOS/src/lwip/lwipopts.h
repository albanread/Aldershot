/* lwipopts.h: the lwIP configuration the HAL uses.  lwIP runs with no
 * operating system under it (NO_SYS): only the HAL calls it, with
 * interrupts off, on one core.  The HAL uses the raw API, on which
 * hal/socket.c builds the box's BSD sockets, with IPv4, DHCP from QEMU's
 * user network, and the loopback interface, 127.0.0.1. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                  1
#define SYS_LIGHTWEIGHT_PROT    0
#define LWIP_SOCKET             0
#define LWIP_NETCONN            0
#define LWIP_TIMERS             1

#define MEM_ALIGNMENT           8
#define MEM_SIZE                (4 * 1024 * 1024)
#define MEMP_NUM_PBUF           256
#define PBUF_POOL_SIZE          1024
#define MEMP_NUM_RAW_PCB        16
#define MEMP_NUM_UDP_PCB        64
#define MEMP_NUM_TCP_PCB        128
#define MEMP_NUM_TCP_PCB_LISTEN 32
#define MEMP_NUM_TCP_SEG        2048
#define MEMP_NUM_REASSDATA      8
#define IP_REASS_MAX_PBUFS      64
#define MEMP_NUM_FRAG_PBUF      32
#define MEMP_NUM_ARP_QUEUE      64

#define LWIP_IPV4               1
#define LWIP_IPV6               0
#define LWIP_ARP                1
#define LWIP_ETHERNET           1
#define LWIP_ICMP               1
#define LWIP_RAW                1
#define LWIP_UDP                1
#define LWIP_TCP                1
#define LWIP_IGMP               0
#define LWIP_DHCP               1
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_AUTOIP             0
#define LWIP_DNS                1               /* keeps DHCP's name servers; musl queries them */
#define DNS_MAX_SERVERS         2
#define LWIP_BROADCAST_PING     1
#define IP_REASSEMBLY           1
#define IP_FRAG                 1
#define SO_REUSE                1
#define SO_REUSE_RXTOALL        1
#define LWIP_NETIF_HOSTNAME     1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LOOPBACK     1
#define LWIP_HAVE_LOOPIF        1
#define LWIP_LOOPBACK_MAX_PBUFS 0
#define LWIP_SINGLE_NETIF       0

#define TCP_MSS                 1460
#define TCP_WND                 (40 * TCP_MSS)
#define TCP_SND_BUF             (40 * TCP_MSS)
#define TCP_SND_QUEUELEN        (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_LISTEN_BACKLOG      1
#define LWIP_TCP_KEEPALIVE      1
#define TCP_QUEUE_OOSEQ         1

#define LWIP_STATS              0
#define LWIP_STATS_DISPLAY      0
#define LWIP_DEBUG              0
#define LWIP_PROVIDE_ERRNO      0

#endif
