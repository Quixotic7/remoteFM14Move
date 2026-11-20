/* SPDX-License-Identifier: MPL-2.0 */
/* "Out": see midiout.h. */
#define _GNU_SOURCE
#include "midiout.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../third_party/abletonmoveremote/common/ring.h"
#include "mdns.h"
#include "minijson.h"
#include "mr_core.h"

#define LIST_MAX 1200   /* an RTP packet's MIDI list at most (a packet ~1.2 KB: no fragments on any link) */
#define SEG_MAX 1000    /* a SysEx longer than a packet: segments of this many data bytes (RFC 6295 3.2) */
#define HDR 6           /* a queued message: its length (2), its stamp (4, 10 kHz), the bytes */

static midiout_cfg_t C;
static int g_on;
static mr_ring_t g_q;   /* the producer's messages -> midiout_poll */
static uint8_t g_body[MIDIOUT_MSG_MAX];
static volatile uint32_t g_dropped, g_filtered;   /* (both sides add: atomics) */
static uint32_t g_queued, g_rtp_sent, g_rtp_pkts, g_rtp_rx, g_rtp_ck, g_rtp_rs, g_usba_sent, g_usba_skipped;
static int g_usba_state;   /* 0 off, 1 on, 2 an FM-1's (skipped) */

/* the session clock: 10 kHz, monotonic (CK's timestamps and the RTP's) */
static uint64_t now10k(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 10000u + (uint64_t)ts.tv_nsec / 100000u;
}

/* ------------------------------------------------------------------ the queue (the producer's side) --- */
/* the message at p (n bytes left): its length, or -skip when damaged (a stray data byte, a short message, an
 * unterminated or interrupted SysEx) */
static int msg_len(const uint8_t *p, int n)
{
    const uint8_t b = p[0];
    int len, k;
    if (b == 0xF0) {
        for (k = 1; k < n && p[k] < 0x80; k++)
            ;
        return k < n && p[k] == 0xF7 ? k + 1 : -k;
    }
    if (b >= 0xF8)
        return 1;
    if (b < 0x80 || b == 0xF7 || b == 0xF4 || b == 0xF5)
        return -1;
    if (b < 0xF0)
        len = (b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0 ? 2 : 3;
    else
        len = b == 0xF1 || b == 0xF3 ? 2 : b == 0xF2 ? 3 : 1;
    for (k = 1; k < len; k++)
        if (k >= n || p[k] >= 0x80)
            return -k;
    return len;
}

/* header and body into the ring in one go: the consumer sees both or neither (one release of head) */
static int ring_put(const uint8_t *h, const uint8_t *b, uint32_t bn)
{
    mr_ring_t *r = &g_q;
    const uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint32_t i;
    if (mr_ring_space(r) < HDR + bn)
        return -1;
    for (i = 0; i < HDR + bn;) {
        const uint32_t at = (head + i) & (r->cap - 1), left = HDR + bn - i;
        uint32_t run = r->cap - at;
        if (run > left)
            run = left;
        if (i < HDR) {   /* (the header: byte by byte, it may wrap) */
            r->buf[at] = h[i++];
            continue;
        }
        memcpy(r->buf + at, b + (i - HDR), run);
        i += run;
    }
    __atomic_store_n(&r->head, head + HDR + bn, __ATOMIC_RELEASE);
    return 0;
}

int midiout_send(const uint8_t *msg, int n)
{
    int i = 0, sent = 0;
    uint32_t t;
    if (!__atomic_load_n(&g_on, __ATOMIC_ACQUIRE) || !msg || n <= 0)
        return 0;
    t = (uint32_t)now10k();
    while (i < n) {
        const int len = msg_len(msg + i, n - i);
        uint8_t h[HDR];
        if (len < 0) {
            __atomic_add_fetch(&g_dropped, 1, __ATOMIC_RELAXED);
            i += -len;
            continue;
        }
        if (msg[i] >= 0xF8 && !C.realtime) {
            __atomic_add_fetch(&g_filtered, 1, __ATOMIC_RELAXED);
            i += len;
            continue;
        }
        h[0] = (uint8_t)len, h[1] = (uint8_t)(len >> 8);
        memcpy(h + 2, &t, 4);
        if (len > MIDIOUT_MSG_MAX || ring_put(h, msg + i, (uint32_t)len) != 0)
            __atomic_add_fetch(&g_dropped, 1, __ATOMIC_RELAXED);
        else
            sent++;
        i += len;
    }
    return sent;
}

int midiout_send_pkts(midiout_asm_t *a, const uint32_t *pk, int n)
{
    static const int8_t LEN[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };
    int i, sent = 0;
    for (i = 0; i < n; i++) {
        const int cin = (int)(pk[i] & 0xF), len = LEN[cin];
        uint8_t b[3];
        int k;
        for (k = 0; k < len; k++)
            b[k] = (uint8_t)(pk[i] >> (8 * (k + 1)));
        if (!len)
            continue;
        if (cin == 0x4 || ((cin == 0x5 || cin == 0x6 || cin == 0x7) && (a->on || b[0] == 0xF0))) {
            if (b[0] == 0xF0)   /* (a SysEx starts: one under way is cut, dropped) */
                a->on = 1, a->n = 0;
            if (a->on == 1) {
                if (a->n + len > MIDIOUT_MSG_MAX)
                    a->on = 2;   /* (too long: the rest skipped to its end) */
                else
                    memcpy(a->sx + a->n, b, (size_t)len), a->n += len;
            }
            if (cin != 0x4 && a->on) {
                if (a->on == 1)
                    sent += midiout_send(a->sx, a->n);
                else
                    __atomic_add_fetch(&g_dropped, 1, __ATOMIC_RELAXED);
                a->on = 0, a->n = 0;
            }
            continue;
        }
        sent += midiout_send(b, len);   /* (a real-time byte may come inside a SysEx: it goes on its own) */
    }
    return sent;
}

/* ------------------------------------------------------------------ RTP-MIDI (the session) --- */
typedef struct {
    int used, ready;                 /* ready: both ports invited (data goes) */
    uint32_t ssrc, token, seen;
    struct sockaddr_in ctl, data;
    char name[64];
} peer_t;

static int g_cfd = -1, g_dfd = -1;   /* control, data */
static uint32_t g_ssrc;
static uint16_t g_seq;
static peer_t P[MIDIOUT_PEERS];
static struct { uint8_t list[LIST_MAX]; int n, cmds, msgs; uint32_t t0, tl; } K;   /* the packet being filled */

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)), put32(p + 4, (uint32_t)v); }

static int udp_open(int port)
{
    struct sockaddr_in a;
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* an exchange packet (IN, OK, NO, BY): FFFF, the command, version 2, the token, our SSRC, our name (not for BY) */
static void exch(int fd, const struct sockaddr_in *to, const char *cmd, uint32_t token)
{
    uint8_t b[96];
    int n = 16;
    b[0] = b[1] = 0xFF, b[2] = (uint8_t)cmd[0], b[3] = (uint8_t)cmd[1];
    put32(b + 4, 2), put32(b + 8, token), put32(b + 12, g_ssrc);
    if (cmd[0] != 'B') {
        const char *nm = C.name ? C.name : MIDIOUT_NAME;
        const size_t l = strlen(nm) < 63 ? strlen(nm) : 63;
        memcpy(b + 16, nm, l);
        b[16 + l] = 0;
        n += (int)l + 1;
    }
    sendto(fd, b, (size_t)n, 0, (const struct sockaddr *)to, sizeof *to);
}

static peer_t *find(uint32_t ssrc, const struct sockaddr_in *from)
{
    int i;
    for (i = 0; i < MIDIOUT_PEERS; i++)
        if (P[i].used && P[i].ssrc == ssrc && P[i].ctl.sin_addr.s_addr == from->sin_addr.s_addr)
            return &P[i];
    return NULL;
}

static void drop(peer_t *p, const char *why)
{
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &p->ctl.sin_addr, ip, sizeof ip);
    fprintf(stderr, "midiout: \"%s\" (%s) %s\n", p->name, ip, why);
    memset(p, 0, sizeof *p);
}

/* one packet on the control (data 0) or data port (1) */
static void on_packet(int data, const uint8_t *b, int n, const struct sockaddr_in *from, uint32_t now)
{
    const int fd = data ? g_dfd : g_cfd;
    peer_t *p;
    if (n >= 12 && (b[0] & 0xC0) == 0x80 && b[0] != 0xFF) {   /* RTP: a peer's own MIDI */
        /* STUB (incoming MIDI): counted and ignored. An input would parse the MIDI list here (RFC 6295 3) and answer
         * with RS now and then so the sender can trim its journal */
        g_rtp_rx++;
        if ((p = find(get32(b + 8), from)))
            p->seen = now;
        return;
    }
    if (n < 12 || b[0] != 0xFF || b[1] != 0xFF)
        return;
    if (b[2] == 'I' && b[3] == 'N' && n >= 16) {   /* an invitation */
        const uint32_t token = get32(b + 8), ssrc = get32(b + 12);
        int i;
        if (!(p = find(ssrc, from)) && !data)
            for (i = 0; i < MIDIOUT_PEERS && !p; i++)
                if (!P[i].used)
                    p = &P[i];
        if (!p) {   /* (full, or a data invitation with no control one) */
            exch(fd, from, "NO", token);
            return;
        }
        if (!p->used) {
            memset(p, 0, sizeof *p);
            p->used = 1, p->ssrc = ssrc, p->ctl = *from;
            snprintf(p->name, sizeof p->name, "%.*s", n > 16 ? n - 16 : 0, (const char *)b + 16);
        }
        p->token = token, p->seen = now;
        if (data) {
            char ip[INET_ADDRSTRLEN] = "?";
            p->data = *from;
            if (!p->ready) {
                inet_ntop(AF_INET, &from->sin_addr, ip, sizeof ip);
                fprintf(stderr, "midiout: \"%s\" (%s) joined the session\n", p->name, ip);
            }
            p->ready = 1;
        } else
            p->ctl = *from;
        exch(fd, from, "OK", token);
    } else if (b[2] == 'B' && b[3] == 'Y' && n >= 16) {
        if ((p = find(get32(b + 12), from)))
            drop(p, "left (BY)");
    } else if (b[2] == 'C' && b[3] == 'K' && n >= 36) {   /* clock sync: theirs (0) answered (1), their last (2) noted */
        if ((p = find(get32(b + 4), from)))
            p->seen = now;
        g_rtp_ck++;
        if (b[8] == 0) {
            uint8_t r[36];
            memcpy(r, b, 36);
            put32(r + 4, g_ssrc);
            r[8] = 1;
            put64(r + 20, now10k());
            sendto(fd, r, sizeof r, 0, (const struct sockaddr *)from, sizeof *from);
        }
    } else if (b[2] == 'R' && b[3] == 'S' && n >= 12) {   /* receiver feedback: what they have (no journal to trim) */
        if ((p = find(get32(b + 4), from)))
            p->seen = now;
        g_rtp_rs++;
    }
}

static void rx(int data, uint32_t now)
{
    const int fd = data ? g_dfd : g_cfd;
    int k;
    for (k = 0; fd >= 0 && k < 32; k++) {
        uint8_t b[1500];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const ssize_t r = recvfrom(fd, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (r < 0)
            break;
        on_packet(data, b, (int)r, &from, now);
    }
}

static int ready_peers(void)
{
    int i, k = 0;
    for (i = 0; i < MIDIOUT_PEERS; i++)
        k += P[i].used && P[i].ready;
    return k;
}

/* the packet out to every peer joined: RTP header (PT 97, the first command's stamp), the command section's header
 * (B = 1 past 15 bytes; no journal, the first command without a delta, its status there) and the list */
static void flush(void)
{
    uint8_t b[12 + 2 + LIST_MAX];
    int h = 12, i;
    if (!K.cmds)
        return;
    b[0] = 0x80, b[1] = 0x61, b[2] = (uint8_t)(g_seq >> 8), b[3] = (uint8_t)g_seq;
    put32(b + 4, K.t0), put32(b + 8, g_ssrc);
    if (K.n <= 15)
        b[h++] = (uint8_t)K.n;
    else
        b[h++] = (uint8_t)(0x80 | K.n >> 8), b[h++] = (uint8_t)K.n;
    memcpy(b + h, K.list, (size_t)K.n);
    for (i = 0; i < MIDIOUT_PEERS; i++)
        if (P[i].used && P[i].ready)
            sendto(g_dfd, b, (size_t)(h + K.n), 0, (const struct sockaddr *)&P[i].data, sizeof P[i].data);
    g_seq++;
    g_rtp_pkts++;
    g_rtp_sent += (uint32_t)K.msgs;
    K.n = K.cmds = K.msgs = 0;
}

/* a command (a whole message, or a SysEx segment) at t into the packet: its delta (variable length, 10 kHz) first */
static void add_cmd(const uint8_t *c, int n, uint32_t t, int ends_msg)
{
    uint8_t d[4];
    int dn = 0;
    if (K.cmds) {
        const uint32_t dt = (int32_t)(t - K.tl) > 0 ? t - K.tl : 0;
        if (dt >= 1u << 21)
            d[dn++] = (uint8_t)(0x80 | (dt >> 21 & 0x7F));
        if (dt >= 1u << 14)
            d[dn++] = (uint8_t)(0x80 | (dt >> 14 & 0x7F));
        if (dt >= 1u << 7)
            d[dn++] = (uint8_t)(0x80 | (dt >> 7 & 0x7F));
        d[dn++] = (uint8_t)(dt & 0x7F);
    }
    if (K.cmds && K.n + dn + n > LIST_MAX) {
        flush();
        dn = 0;
    }
    if (!K.cmds)
        K.t0 = t;
    memcpy(K.list + K.n, d, (size_t)dn);
    memcpy(K.list + K.n + dn, c, (size_t)n);
    K.n += dn + n;
    K.cmds++;
    K.msgs += ends_msg;
    K.tl = t;
}

static void rtp_msg(const uint8_t *m, int n, uint32_t t)
{
    uint8_t seg[SEG_MAX + 2];
    int at = 1, data = n - 2;
    if (n <= LIST_MAX - 4 || m[0] != 0xF0) {
        add_cmd(m, n, t, 1);
        return;
    }
    while (data > 0) {   /* a long SysEx: F0 .. F0, then F7 .. F0, ending F7 .. F7, a packet each */
        const int take = data > SEG_MAX ? SEG_MAX : data;
        seg[0] = at == 1 ? 0xF0 : 0xF7;
        memcpy(seg + 1, m + at, (size_t)take);
        seg[1 + take] = take == data ? 0xF7 : 0xF0;
        flush();
        add_cmd(seg, take + 2, t, take == data);
        flush();
        at += take, data -= take;
    }
}

/* ------------------------------------------------------------------ start, poll, stop --- */
int midiout_start(const midiout_cfg_t *cfg)
{
    memset(&C, 0, sizeof C);
    if (cfg)
        C = *cfg;
    if (mr_ring_init(&g_q, 1, MIDIOUT_RING) != 0)
        return -1;
    g_ssrc = (uint32_t)now10k() * 2654435761u ^ (uint32_t)getpid();
    if (C.rtp_port >= 0) {
        const int port = C.rtp_port ? C.rtp_port : MIDIOUT_RTP_PORT;
        C.rtp_port = port;
        if ((g_cfd = udp_open(port)) < 0 || (g_dfd = udp_open(port + 1)) < 0) {
            fprintf(stderr, "midiout: cannot bind UDP %d/%d (%s): no network MIDI\n", port, port + 1, strerror(errno));
            if (g_cfd >= 0)
                close(g_cfd);
            g_cfd = g_dfd = -1;
            C.rtp_port = -1;
        } else {
            fprintf(stderr, "midiout: network MIDI \"%s\" on UDP %d/%d\n", C.name ? C.name : MIDIOUT_NAME, port, port + 1);
            mdns_service(C.name ? C.name : MIDIOUT_NAME, "_apple-midi._udp.local", port);   /* (when mdns runs) */
        }
    }
    fprintf(stderr, "midiout: USB-A %s; real-time bytes %s\n", C.usba ? "on (not while an FM-1 runs RemoteFM1 there)"
            : "off (--no-fm1)", C.realtime ? "sent" : "dropped");
    __atomic_store_n(&g_on, 1, __ATOMIC_RELEASE);
    return 0;
}

void midiout_poll(uint32_t now)
{
    int i, peers;
    if (!g_on)
        return;
    rx(0, now);
    rx(1, now);
    for (i = 0; i < MIDIOUT_PEERS; i++)
        if (P[i].used && now - P[i].seen > MIDIOUT_PEER_MS)
            drop(&P[i], "silent: dropped");
    peers = ready_peers();
    g_usba_state = !C.usba ? 0 : C.usba_busy && C.usba_busy(now) ? 2 : 1;
    for (;;) {
        uint8_t h[HDR];
        uint32_t t;
        int len;
        if (mr_ring_count(&g_q) < HDR)
            break;
        mr_ring_pop(&g_q, h, HDR);   /* (the body is there: header and body go in at once) */
        len = h[0] | h[1] << 8;
        memcpy(&t, h + 2, 4);
        mr_ring_pop(&g_q, g_body, (uint32_t)len);
        g_queued++;
        if (peers)
            rtp_msg(g_body, len, t);
        if (g_usba_state == 1) {
            const int r = mr_usba_send(g_body, (size_t)len);
            if (r < 0)
                __atomic_add_fetch(&g_dropped, 1, __ATOMIC_RELAXED);   /* (USB-A's queue full) */
            else if (r > 0)
                g_usba_sent++;
        } else if (g_usba_state == 2)
            g_usba_skipped++;
    }
    flush();
}

int midiout_json(char *out, int cap)
{
    static const char *const US[] = { "off", "on", "fm1" };
    int n, i, first = 1;
    n = snprintf(out, (size_t)cap, ",\"midiout\":{\"rtp_port\":%d,\"rtp_peers\":%d,\"rtp_sent\":%u,\"rtp_packets\":%u,"
                 "\"rtp_rx\":%u,\"rtp_ck\":%u,\"rtp_rs\":%u,\"usba\":\"%s\",\"usba_out_sent\":%u,\"usba_skipped\":%u,"
                 "\"queued\":%u,\"filtered\":%u,\"dropped\":%u,\"peers\":[", g_on ? C.rtp_port : -1, ready_peers(),
                 g_rtp_sent, g_rtp_pkts, g_rtp_rx, g_rtp_ck, g_rtp_rs, US[g_usba_state], g_usba_sent, g_usba_skipped,
                 g_queued, g_filtered, g_dropped);
    for (i = 0; i < MIDIOUT_PEERS && n < cap; i++)
        if (P[i].used && P[i].ready) {
            char e[140];
            mj_escape(P[i].name, e, sizeof e);
            n += snprintf(out + n, (size_t)(cap - n), "%s\"%s\"", first ? "" : ",", e);
            first = 0;
        }
    if (n < cap)
        n += snprintf(out + n, (size_t)(cap - n), "]}");
    return n < cap ? n : cap - 1;
}

void midiout_stop(void)
{
    int i;
    if (!g_on)
        return;
    __atomic_store_n(&g_on, 0, __ATOMIC_RELEASE);
    for (i = 0; i < MIDIOUT_PEERS; i++)
        if (P[i].used && g_cfd >= 0)
            exch(g_cfd, &P[i].ctl, "BY", P[i].token);
    memset(P, 0, sizeof P);
    if (g_cfd >= 0)
        close(g_cfd);
    if (g_dfd >= 0)
        close(g_dfd);
    g_cfd = g_dfd = -1;
}
