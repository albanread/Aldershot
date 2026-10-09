/* net.c: the network, consisting of the virtio-net driver and lwIP running
 * over it.
 *
 * The other end is usually QEMU's user network (slirp).  It answers DHCP,
 * provides a name server at 10.0.2.3 and reaches the Mac's network, and
 * needs no set-up on the Mac.  The HAL asks for an address before /init
 * starts, as Linux's ip=dhcp does, so the interface already has its
 * address when the box starts.
 *
 * lwIP runs with no operating system under it (NO_SYS): only the HAL calls
 * it, with interrupts off, on one core.  Frames arrive by interrupt, and
 * the interrupt handler passes them to lwIP.  The HAL runs lwIP's timers
 * and the loopback interface's queue at each tick and after each socket
 * call (net_poll).  socket.c builds the box's BSD sockets on lwIP's raw
 * API. */
#include "hal.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/timeouts.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"

#define F_MAC   (1u << 5)                       /* VIRTIO_NET_F_MAC */
#define HDR     12                              /* virtio_net_hdr_v1 */
#define QSIZE   256
#define BUFSZ   2048

static uint64_t base;
static unsigned slot;
static struct vq rxq, txq;
static uint8_t *rxbuf, *txbuf;
static uint64_t rxbuf_pa, txbuf_pa;
static uint8_t mac[6];
static struct netif nif;
static int up;

unsigned hal_random(void)
{
    static uint64_t s;
    if (!s)
        s = counter_read() | 1;
    s ^= s << 13, s ^= s >> 7, s ^= s << 17;
    return (unsigned)(s >> 16);
}

u32_t sys_now(void)
{
    return (u32_t)(counter_read() * 1000 / counter_freq());
}

int net_attach(uint64_t b, unsigned n)
{
    if (base)
        return -1;
    if (virtio_begin(b, F_MAC))
        return -1;
    uint64_t r = pages_alloc_run(QSIZE * BUFSZ / PAGE_SIZE), t = pages_alloc_run(QSIZE * BUFSZ / PAGE_SIZE);
    if (!r || !t || virtio_queue(b, 0, &rxq, QSIZE) || virtio_queue(b, 1, &txq, QSIZE))
        return -1;
    rxbuf = pa_to_va(r), txbuf = pa_to_va(t), rxbuf_pa = r, txbuf_pa = t;
    for (int i = 0; i < 6; i++)
        mac[i] = mmio_r8(b + 0x100 + (unsigned)i);
    for (uint16_t i = 0; i < QSIZE; i++) {
        rxq.desc[i] = (struct vdesc){ r + (uint64_t)i * BUFSZ, BUFSZ, VDESC_WRITE, 0 };
        virtio_offer(&rxq, i);
    }
    virtio_go(b);
    virtio_notify(b, 0);
    base = b, slot = n;
    spi_enable(VIRTIO_IRQ0 + n);
    return 0;
}

/* ---- Frames ------------------------------------------------------------- */

/* Pass the frames the device has received to lwIP, and reclaim the transmit
 * buffers the device has finished with. */
static int service(void)
{
    if (!base)
        return 0;
    mmio_w32(base + 0x064, mmio_r32(base + 0x060));         /* acknowledge the interrupt */
    int got = 0;
    __asm__ volatile("dmb ish" ::: "memory");
    while (rxq.used_seen != rxq.used[1]) {
        const volatile uint32_t *e = (const volatile uint32_t *)(rxq.used + 2) + 2 * (rxq.used_seen % QSIZE);
        uint16_t id = (uint16_t)(e[0] % QSIZE);
        uint32_t len = e[1];
        rxq.used_seen++;
        if (len > HDR && len <= BUFSZ && up) {
            struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)(len - HDR), PBUF_POOL);
            if (p) {
                pbuf_take(p, rxbuf + (uint64_t)id * BUFSZ + HDR, (u16_t)(len - HDR));
                counts.net_rx_packets++, counts.net_rx_bytes += len - HDR;
                if (nif.input(p, &nif) != ERR_OK)
                    pbuf_free(p);
            }
        }
        virtio_offer(&rxq, id);
        got = 1;
    }
    if (got)
        virtio_notify(base, 0);
    txq.used_seen = txq.used[1];                /* frames sent; their buffers are free again */
    return got;
}

static err_t linkoutput(struct netif *n, struct pbuf *p)
{
    (void)n;
    if (p->tot_len + HDR > BUFSZ)
        return ERR_BUF;
    counts.net_tx_packets++, counts.net_tx_bytes += p->tot_len;
    while ((uint16_t)(txq.avail_idx - txq.used[1]) >= QSIZE)   /* wait while all are in use */
        __asm__ volatile("yield" ::: "memory");
    uint16_t i = txq.avail_idx % QSIZE;
    uint8_t *b = txbuf + (uint64_t)i * BUFSZ;
    memset(b, 0, HDR);
    pbuf_copy_partial(p, b + HDR, p->tot_len, 0);
    txq.desc[i] = (struct vdesc){ txbuf_pa + (uint64_t)i * BUFSZ, (uint32_t)(p->tot_len + HDR), 0, 0 };
    virtio_offer(&txq, i);
    virtio_notify(base, 1);
    return ERR_OK;
}

static err_t nif_init(struct netif *n)
{
    n->name[0] = 'e', n->name[1] = 'n';
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, mac, 6);
    n->mtu = 1500;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
    n->output = etharp_output;
    n->linkoutput = linkoutput;
    netif_set_hostname(n, "rosgd");
    return ERR_OK;
}

int net_irq(unsigned n)
{
    if (!base || n != slot)
        return 0;
    service();
    net_poll();
    return 1;
}

void net_poll(void)
{
    sys_check_timeouts();
    netif_poll_all();
}

/* ---- Start-up ------------------------------------------------------------ */

void net_init(void)
{
    lwip_init();                                /* also creates the loopback, 127.0.0.1 */
    if (!base) {
        kprintf("HAL: network: no virtio-net device\n");
        return;
    }
    netif_add(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, 0, nif_init, ethernet_input);
    netif_set_default(&nif);
    netif_set_up(&nif);
    up = 1;
    dhcp_start(&nif);
    /* Wait up to three seconds for an address.  The box's interrupts are
     * not enabled yet, so this loop services the device itself. */
    uint64_t until = counter_read() + 3 * counter_freq();
    while (!dhcp_supplied_address(&nif) && (int64_t)(counter_read() - until) < 0) {
        service();
        net_poll();
    }
    if (dhcp_supplied_address(&nif))
        kprintf("HAL: network: %s by DHCP; name server %s\n", ip4addr_ntoa(netif_ip4_addr(&nif)),
                ipaddr_ntoa(dns_getserver(0)));
    else
        kprintf("HAL: network: no answer to DHCP\n");
}

/* Return the text of Linux's /proc/net/pnp, in the form Linux's ip=dhcp
 * writes it, giving what DHCP supplied. */
const char *net_pnp(void)
{
    static char text[256];
    char *p = text;
    const char *head = "#PROTO: DHCP\n";
    memcpy(p, head, strlen(head)), p += strlen(head);
    for (u8_t i = 0; i < DNS_MAX_SERVERS; i++) {
        const ip_addr_t *a = dns_getserver(i);
        if (ip_addr_isany(a))
            continue;
        const char *s = ipaddr_ntoa(a);
        memcpy(p, "nameserver ", 11), p += 11;
        memcpy(p, s, strlen(s)), p += strlen(s);
        *p++ = '\n';
    }
    *p = 0;
    return text;
}

/* Return an interface, for socket.c's ioctls and netlink: 0 is the
 * loopback, and 1 is the network interface, if there is one. */
struct netif *net_iface(int i)
{
    if (i == 0)
        return netif_get_by_index(1);           /* lo, which lwIP creates first */
    return i == 1 && base ? &nif : 0;
}
