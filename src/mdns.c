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

size_t mdns_answer(const uint8_t *q, size_t n, const char *name, const uint8_t addr[4], int from_port, uint8_t *out,
                   size_t cap, int *unicast)
{
    size_t at = 12;
    unsigned qd, i;
    int hit = 0, qu = 0;
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
        if (!strcasecmp(qn, name) && (type == 1 || type == 255) && (cls & 0x7FFF) == 1) {
            hit = 1;
            qu |= (cls & 0x8000) != 0;
        }
    }
    if (!hit || cap < 12)
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
        out[at++] = 0, out[at++] = 1, out[at++] = 0, out[at++] = 1;
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

/* ------------------------------------------------------------------ the responder --- */
static int g_fd = -1;
static pthread_t g_thr;
static volatile int g_run;
static char g_name[128];

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

static void send_to(const uint8_t *b, size_t n, const struct sockaddr_in *to)
{
    sendto(g_fd, b, n, 0, (const struct sockaddr *)to, sizeof *to);
}

static void *mdns_main(void *arg)
{
    struct sockaddr_in group;
    int rejoin = 0;
    (void)arg;
#ifdef __linux__
    pthread_setname_np(pthread_self(), "rfm1-mdns");
#endif
    memset(&group, 0, sizeof group);
    group.sin_family = AF_INET;
    group.sin_port = htons(5353);
    group.sin_addr.s_addr = inet_addr("224.0.0.251");
    {   /* hello: the name, on every interface we can reach */
        uint8_t a[4], out[512];
        size_t n;
        if (if_addr(0, a) && (n = mdns_announce(g_name, a, out, sizeof out)))
            send_to(out, n, &group);
    }
    while (g_run) {
        uint8_t q[1500], out[512];
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
            send_to(out, n, unicast ? &from : &group);
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
