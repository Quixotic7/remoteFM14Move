/* SPDX-License-Identifier: MPL-2.0 */
/* The mDNS responder: see mdns.h. */
#define _GNU_SOURCE
#include "mdns.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TTL 120

/* a name at q[*at] (labels, maybe compressed) as dotted text; -1 if damaged */
static int read_name(const uint8_t *q, size_t n, size_t *at, char *out, size_t cap)
{
    size_t p = *at, o = 0;
    int jumped = 0, hops = 0;
    for (;;) {
        uint8_t l;
        if (p >= n)
            return -1;
        l = q[p];
        if ((l & 0xC0) == 0xC0) {
            if (p + 1 >= n || ++hops > 16)
                return -1;
            if (!jumped)
                *at = p + 2;
            jumped = 1;
            p = (size_t)(l & 0x3F) << 8 | q[p + 1];
            continue;
        }
        if (l & 0xC0)
            return -1;
        p++;
        if (l == 0)
            break;
        if (p + l > n || o + l + 2 > cap)
            return -1;
        if (o)
            out[o++] = '.';
        memcpy(out + o, q + p, l);
        o += l;
        p += l;
    }
    if (!jumped)
        *at = p;
    out[o] = 0;
    return 0;
}

static size_t put_name(uint8_t *out, size_t cap, size_t at, const char *name)
{
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        const size_t l = dot ? (size_t)(dot - s) : strlen(s);
        if (l == 0 || l > 63 || at + 1 + l >= cap)
            return 0;
        out[at++] = (uint8_t)l;
        memcpy(out + at, s, l);
        at += l;
        s += l + (dot ? 1 : 0);
    }
    if (at >= cap)
        return 0;
    out[at++] = 0;
    return at;
}

static size_t put_a(uint8_t *out, size_t cap, size_t at, const char *name, const uint8_t addr[4], int flush)
{
    if (!(at = put_name(out, cap, at, name)) || at + 14 > cap)
        return 0;
    out[at++] = 0, out[at++] = 1;                               /* type A */
    out[at++] = (uint8_t)(flush ? 0x80 : 0), out[at++] = 1;     /* class IN, the cache-flush bit */
    out[at++] = 0, out[at++] = 0, out[at++] = 0, out[at++] = TTL;
    out[at++] = 0, out[at++] = 4;
    memcpy(out + at, addr, 4);
    return at + 4;
}

/* RFC 6762 6.1: "this name has an A record and nothing else", so an AAAA query is answered at once (a resolver
 * would otherwise wait for an IPv6 address that never comes) */
static size_t put_nsec(uint8_t *out, size_t cap, size_t at, const char *name)
{
    size_t rd;
    if (!(at = put_name(out, cap, at, name)) || at + 10 > cap)
        return 0;
    out[at++] = 0, out[at++] = 47;                              /* type NSEC */
    out[at++] = 0x80, out[at++] = 1;                            /* class IN, cache-flush */
    out[at++] = 0, out[at++] = 0, out[at++] = 0, out[at++] = TTL;
    rd = at;
    at += 2;
    if (!(at = put_name(out, cap, at, name)) || at + 3 > cap)   /* the next name: ours */
        return 0;
    out[at++] = 0, out[at++] = 1, out[at++] = 0x40;             /* window 0, one byte: type 1 (A) only */
    out[rd] = (uint8_t)((at - rd - 2) >> 8), out[rd + 1] = (uint8_t)(at - rd - 2);
    return at;
}

size_t mdns_answer(const uint8_t *q, size_t n, const char *name, const uint8_t addr[4], int from_port, uint8_t *out,
                   size_t cap, int *unicast)
{
    size_t at = 12;
    unsigned qd, i;
    int hit = 0, aaaa = 0, qu = 0;
    const int legacy = from_port != 5353;
    if (n < 12 || (q[2] & 0x80) || (q[2] & 0x78))   /* a response, or not a standard query */
        return 0;
    qd = (unsigned)q[4] << 8 | q[5];
    for (i = 0; i < qd && i < 16; i++) {
        char qn[256];
        unsigned type, cls;
        if (read_name(q, n, &at, qn, sizeof qn) || at + 4 > n)
            return 0;
        type = (unsigned)q[at] << 8 | q[at + 1];
        cls = (unsigned)q[at + 2] << 8 | q[at + 3];
        at += 4;
        if (!strcasecmp(qn, name) && (type == 1 || type == 255 || type == 28) && (cls & 0x7FFF) == 1) {
            if (type == 28)
                aaaa = 1;
            else
                hit = 1;
            qu |= (cls & 0x8000) != 0;
        }
    }
    if ((!hit && !aaaa) || cap < 12)
        return 0;
    *unicast = legacy || qu;
    memset(out, 0, 12);
    if (legacy)   /* a plain resolver: its id, and the question back */
        out[0] = q[0], out[1] = q[1];
    out[2] = 0x84;   /* a response, authoritative */
    out[7] = 1;      /* one answer */
    at = 12;
    if (legacy) {
        out[5] = 1;
        if (!(at = put_name(out, cap, at, name)) || at + 4 > cap)
            return 0;
        out[at++] = 0, out[at++] = (uint8_t)(hit ? 1 : 28), out[at++] = 0, out[at++] = 1;
    }
    if (!hit) {   /* AAAA only: the NSEC as the answer, the A record as additional */
        out[11] = 1;
        if (!(at = put_nsec(out, cap, at, name)))
            return 0;
    }
    return put_a(out, cap, at, name, addr, !legacy);
}

size_t mdns_announce(const char *name, const uint8_t addr[4], uint8_t *out, size_t cap)
{
    if (cap < 12)
        return 0;
    memset(out, 0, 12);
    out[2] = 0x84;
    out[7] = 1;
    return put_a(out, cap, 12, name, addr, 1);
}

/* ------------------------------------------------------------------ a DNS-SD service --- */
#define SD_ENUM "_services._dns-sd._udp.local"

/* a record's head (name, type, class with the cache-flush bit, TTL, rdlength to come): where its data starts, 0 no room */
static size_t rr_head(uint8_t *out, size_t cap, size_t at, const char *name, int type, int flush)
{
    if (!(at = put_name(out, cap, at, name)) || at + 10 > cap)
        return 0;
    out[at++] = 0, out[at++] = (uint8_t)type;
    out[at++] = (uint8_t)(flush ? 0x80 : 0), out[at++] = 1;
    out[at++] = 0, out[at++] = 0, out[at++] = 0, out[at++] = TTL;
    return at + 2;
}
static size_t rr_end(uint8_t *out, size_t rd, size_t at)
{
    if (!at)
        return 0;
    out[rd - 2] = (uint8_t)((at - rd) >> 8), out[rd - 1] = (uint8_t)(at - rd);
    return at;
}
static size_t put_ptr(uint8_t *out, size_t cap, size_t at, const char *owner, const char *to)
{
    const size_t rd = at ? rr_head(out, cap, at, owner, 12, 0) : 0;   /* (shared: no cache flush) */
    return rd ? rr_end(out, rd, put_name(out, cap, rd, to)) : 0;
}
static size_t put_srv(uint8_t *out, size_t cap, size_t at, const char *full, int port, const char *host, int flush)
{
    const size_t rd = at ? rr_head(out, cap, at, full, 33, flush) : 0;
    if (!rd || rd + 6 > cap)
        return 0;
    memset(out + rd, 0, 4);   /* priority, weight */
    out[rd + 4] = (uint8_t)(port >> 8), out[rd + 5] = (uint8_t)port;
    return rr_end(out, rd, put_name(out, cap, rd + 6, host));
}
static size_t put_txt(uint8_t *out, size_t cap, size_t at, const char *full, int flush)
{
    const size_t rd = at ? rr_head(out, cap, at, full, 16, flush) : 0;
    if (!rd || rd + 1 > cap)
        return 0;
    out[rd] = 0;   /* one empty string */
    return rr_end(out, rd, rd + 1);
}

enum { W_PTR = 1, W_ENUM = 2, W_SRV = 4, W_TXT = 8 };

/* the records asked for (w), and their additionals, after a header at out[0..12) (and a legacy question) at at */
static size_t svc_records(const mdns_svc_t *svc, const char *name, const uint8_t addr[4], int w, int flush,
                          uint8_t *out, size_t cap, size_t at)
{
    char full[256];
    unsigned an = 0, ar = 0;
    snprintf(full, sizeof full, "%s.%s", svc->instance, svc->type);
    if (w & W_ENUM)
        at = put_ptr(out, cap, at, SD_ENUM, svc->type), an++;
    if (w & W_PTR)
        at = put_ptr(out, cap, at, svc->type, full), an++;
    if (w & W_SRV)
        at = put_srv(out, cap, at, full, svc->port, name, flush), an++;
    if (w & W_TXT)
        at = put_txt(out, cap, at, full, flush), an++;
    if ((w & W_PTR) && !(w & W_SRV))   /* (a browser: the rest it will ask for, at once) */
        at = put_srv(out, cap, at, full, svc->port, name, flush), ar++;
    if ((w & W_PTR) && !(w & W_TXT))
        at = put_txt(out, cap, at, full, flush), ar++;
    if (w & (W_PTR | W_SRV))
        at = at ? put_a(out, cap, at, name, addr, flush) : 0, ar++;
    if (!at)
        return 0;
    out[6] = (uint8_t)(an >> 8), out[7] = (uint8_t)an;
    out[10] = (uint8_t)(ar >> 8), out[11] = (uint8_t)ar;
    return at;
}

size_t mdns_answer_svc(const uint8_t *q, size_t n, const mdns_svc_t *svc, const char *name, const uint8_t addr[4],
                       int from_port, uint8_t *out, size_t cap, int *unicast)
{
    char full[256];
    size_t at = 12, qend;
    unsigned qd, i;
    int w = 0, qu = 0;
    const int legacy = from_port != 5353;
    if (n < 12 || (q[2] & 0x80) || (q[2] & 0x78) || cap < 12)
        return 0;
    snprintf(full, sizeof full, "%s.%s", svc->instance, svc->type);
    qd = (unsigned)q[4] << 8 | q[5];
    for (i = 0; i < qd && i < 16; i++) {
        char qn[256];
        unsigned type, cls;
        int hit = 0;
        if (read_name(q, n, &at, qn, sizeof qn) || at + 4 > n)
            return 0;
        type = (unsigned)q[at] << 8 | q[at + 1];
        cls = (unsigned)q[at + 2] << 8 | q[at + 3];
        at += 4;
        if ((cls & 0x7FFF) != 1 && (cls & 0x7FFF) != 255)
            continue;
        if (!strcasecmp(qn, svc->type) && (type == 12 || type == 255))
            w |= W_PTR, hit = 1;
        if (!strcasecmp(qn, SD_ENUM) && (type == 12 || type == 255))
            w |= W_ENUM, hit = 1;
        if (!strcasecmp(qn, full) && (type == 33 || type == 16 || type == 255)) {
            w |= type == 33 ? W_SRV : type == 16 ? W_TXT : W_SRV | W_TXT;
            hit = 1;
        }
        if (hit)
            qu |= (cls & 0x8000) != 0;
    }
    if (!w)
        return 0;
    qend = at;
    *unicast = legacy || qu;
    memset(out, 0, 12);
    out[2] = 0x84;
    at = 12;
    if (legacy) {   /* a plain resolver: its id, and its questions back */
        out[0] = q[0], out[1] = q[1];
        if (qend > cap)
            return 0;
        memcpy(out + 12, q + 12, qend - 12);
        out[4] = q[4], out[5] = q[5];
        at = qend;
    }
    return svc_records(svc, name, addr, w, !legacy, out, cap, at);
}

size_t mdns_announce_svc(const mdns_svc_t *svc, const char *name, const uint8_t addr[4], uint8_t *out, size_t cap)
{
    if (cap < 12)
        return 0;
    memset(out, 0, 12);
    out[2] = 0x84;
    return svc_records(svc, name, addr, W_ENUM | W_PTR | W_SRV | W_TXT, 1, out, cap, 12);
}

/* ------------------------------------------------------------------ the responder --- */
static int g_fd = -1;
static pthread_t g_thr;
static volatile int g_run;
static char g_name[128];
static char g_svc_inst[64], g_svc_type[96];
static mdns_svc_t g_svc;   /* (port 0: none) */

void mdns_service(const char *instance, const char *type, int port)
{
    snprintf(g_svc_inst, sizeof g_svc_inst, "%s", instance);
    snprintf(g_svc_type, sizeof g_svc_type, "%s", type);
    g_svc.instance = g_svc_inst, g_svc.type = g_svc_type, g_svc.port = port;
}

/* the IPv4 address of interface ifindex (0: the first that is up and not loopback) */
static int if_addr(unsigned ifindex, uint8_t out[4])
{
    struct ifaddrs *ifa, *i;
    int ok = 0;
    if (getifaddrs(&ifa) != 0)
        return 0;
    for (i = ifa; i && !ok; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) ||
            !(i->ifa_flags & IFF_UP))
            continue;
        if (ifindex && if_nametoindex(i->ifa_name) != ifindex)
            continue;
        memcpy(out, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, 4);
        ok = 1;
    }
    freeifaddrs(ifa);
    return ok;
}

static void join_all(int fd)
{
    struct ifaddrs *ifa, *i;
    if (getifaddrs(&ifa) != 0)
        return;
    for (i = ifa; i; i = i->ifa_next) {
        struct ip_mreq m;
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) ||
            !(i->ifa_flags & IFF_MULTICAST))
            continue;
        m.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
        m.imr_interface = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);   /* (already joined: EADDRINUSE, fine) */
    }
    freeifaddrs(ifa);
}

/* to the group: out of the interface whose address is addr (the one the question came in on; the USB-C link and
 * Wi-Fi are both multicast), else to an address */
static void send_to(const uint8_t *b, size_t n, const struct sockaddr_in *to, const uint8_t addr[4])
{
    if (addr) {
        struct in_addr ia;
        memcpy(&ia, addr, 4);
        setsockopt(g_fd, IPPROTO_IP, IP_MULTICAST_IF, &ia, sizeof ia);
    }
    sendto(g_fd, b, n, 0, (const struct sockaddr *)to, sizeof *to);
}

/* hello: the name (and the service) on every interface that is up */
static void announce_all(const struct sockaddr_in *group)
{
    struct ifaddrs *ifa, *i;
    if (getifaddrs(&ifa) != 0)
        return;
    for (i = ifa; i; i = i->ifa_next) {
        uint8_t a[4], out[1024];
        size_t n;
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) ||
            !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_MULTICAST))
            continue;
        memcpy(a, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, 4);
        if ((n = mdns_announce(g_name, a, out, sizeof out)))
            send_to(out, n, group, a);
        if (g_svc.port && (n = mdns_announce_svc(&g_svc, g_name, a, out, sizeof out)))
            send_to(out, n, group, a);
    }
    freeifaddrs(ifa);
}

static void *mdns_main(void *arg)
{
    struct sockaddr_in group;
    int rejoin = 0, hellos = 1;
    (void)arg;
#ifdef __linux__
    pthread_setname_np(pthread_self(), "rfm1-mdns");
#endif
    memset(&group, 0, sizeof group);
    group.sin_family = AF_INET;
    group.sin_port = htons(5353);
    group.sin_addr.s_addr = inet_addr("224.0.0.251");
    announce_all(&group);   /* (and again a second later: RFC 6762 8.3 asks for two) */
    while (g_run) {
        uint8_t q[1500], out[1024];
        struct sockaddr_in from;
        char cbuf[256];
        struct iovec iov = { q, sizeof q };
        struct msghdr mh;
        struct pollfd pf = { g_fd, POLLIN, 0 };
        unsigned ifindex = 0;
        ssize_t r;
        int unicast = 0;
        size_t n;
        uint8_t a[4];
        if (poll(&pf, 1, 200) <= 0) {
            if (hellos < 2 && rejoin == 4)
                announce_all(&group), hellos++;
            if (++rejoin >= 50) {   /* every ~10 s: interfaces come and go (USB, Wi-Fi) */
                rejoin = 0;
                join_all(g_fd);
            }
            continue;
        }
        memset(&mh, 0, sizeof mh);
        mh.msg_name = &from;
        mh.msg_namelen = sizeof from;
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof cbuf;
        if ((r = recvmsg(g_fd, &mh, 0)) <= 0)
            continue;
#ifdef IP_PKTINFO
        {
            struct cmsghdr *c;
            for (c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
                if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO)
                    ifindex = (unsigned)((struct in_pktinfo *)CMSG_DATA(c))->ipi_ifindex;
        }
#endif
        if (!if_addr(ifindex, a) && !if_addr(0, a))
            continue;
        n = mdns_answer(q, (size_t)r, g_name, a, ntohs(from.sin_port), out, sizeof out, &unicast);
        if (n)
            send_to(out, n, unicast ? &from : &group, unicast ? NULL : a);
        if (g_svc.port &&
            (n = mdns_answer_svc(q, (size_t)r, &g_svc, g_name, a, ntohs(from.sin_port), out, sizeof out, &unicast)))
            send_to(out, n, unicast ? &from : &group, unicast ? NULL : a);
    }
    return NULL;
}

int mdns_start(const char *name)
{
    struct sockaddr_in a;
    int one = 1;
    unsigned char ttl = 255;
    snprintf(g_name, sizeof g_name, "%s", name);
    if ((g_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        return -1;
    setsockopt(g_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(g_fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
#ifdef IP_PKTINFO
    setsockopt(g_fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof one);
#endif
    setsockopt(g_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(5353);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_fd, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "mdns: cannot bind 5353 (%s): no %s\n", strerror(errno), g_name);
        close(g_fd);
        g_fd = -1;
        return -1;
    }
    join_all(g_fd);
    g_run = 1;
    if (pthread_create(&g_thr, NULL, mdns_main, NULL) != 0) {
        close(g_fd);
        g_fd = -1;
        return -1;
    }
    if (g_svc.port)
        fprintf(stderr, "mdns: answering for %s and \"%s\" (%s, port %d)\n", g_name, g_svc.instance, g_svc.type, g_svc.port);
    else
        fprintf(stderr, "mdns: answering for %s\n", g_name);
    return 0;
}

void mdns_stop(void)
{
    if (g_fd < 0)
        return;
    g_run = 0;
    pthread_join(g_thr, NULL);
    close(g_fd);
    g_fd = -1;
}
