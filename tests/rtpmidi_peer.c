/* SPDX-License-Identifier: MPL-2.0 */
/* A tiny AppleMIDI initiator (tests/midiout_e2e.sh), as a Mac's Audio MIDI Setup is one: finds the session by mDNS
 * (a legacy query for _apple-midi._udp.local: the PTR, its SRV's port), invites both ports (IN, OK), syncs the clock
 * (CK 0, 1, 2), says it is ready, then takes RTP-MIDI packets (RS for each) until the MIDI they carry is as long as
 * expected or the time is up; prints it, leaves (BY). Exit 0 when the bytes are the expected ones.
 *   rtpmidi-peer [--host IP] [--mdns-port N] [--port N] [--name NAME] [--ready FILE] [--timeout S] --expect HEX */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const char *g_host = "127.0.0.1";
static uint32_t g_ssrc = 0x5EED1234u, g_token = 0x0BADCAFEu;

static uint64_t now10k(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 10000u + (uint64_t)ts.tv_nsec / 100000u;
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static struct sockaddr_in addr(int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, g_host, &a.sin_addr);
    return a;
}

static int udp(void)
{
    struct sockaddr_in a;
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    bind(fd, (struct sockaddr *)&a, sizeof a);
    return fd;
}

static int wait_rx(int fd, uint8_t *b, int cap, int ms)
{
    struct pollfd pf = { fd, POLLIN, 0 };
    if (poll(&pf, 1, ms) <= 0)
        return -1;
    return (int)recv(fd, b, (size_t)cap, 0);
}

/* ---- mDNS: a name at q[*at] as dotted text (compression followed) */
static int rd_name(const uint8_t *q, int n, int *at, char *out, int cap)
{
    int p = *at, o = 0, jumped = 0, hops = 0;
    for (;;) {
        int l;
        if (p >= n)
            return -1;
        l = q[p];
        if ((l & 0xC0) == 0xC0) {
            if (p + 1 >= n || ++hops > 16)
                return -1;
            if (!jumped)
                *at = p + 2;
            jumped = 1;
            p = (l & 0x3F) << 8 | q[p + 1];
            continue;
        }
        p++;
        if (!l)
            break;
        if (p + l > n || o + l + 2 > cap)
            return -1;
        if (o)
            out[o++] = '.';
        memcpy(out + o, q + p, (size_t)l);
        o += l, p += l;
    }
    if (!jumped)
        *at = p;
    out[o] = 0;
    return 0;
}

/* the service's port by mDNS (its instance's name in inst): the port, or -1 */
static int mdns_find(int mport, char *inst, int icap)
{
    static const char T[] = "_apple-midi._udp.local";
    uint8_t q[64], r[1500];
    struct sockaddr_in to = addr(mport);
    const int fd = udp();
    int n = 12, try, port = -1;
    const char *s = T;
    memset(q, 0, 12);
    q[0] = 0x42, q[1] = 0x42, q[5] = 1;
    while (*s) {
        const char *dot = strchr(s, '.');
        const int l = dot ? (int)(dot - s) : (int)strlen(s);
        q[n++] = (uint8_t)l;
        memcpy(q + n, s, (size_t)l);
        n += l;
        s += l + (dot ? 1 : 0);
    }
    q[n++] = 0, q[n++] = 0, q[n++] = 12, q[n++] = 0, q[n++] = 1;   /* PTR, IN */
    for (try = 0; try < 5 && port < 0; try++) {
        int k, at, an, ar, i;
        sendto(fd, q, (size_t)n, 0, (struct sockaddr *)&to, sizeof to);
        if ((k = wait_rx(fd, r, sizeof r, 1000)) < 12 || r[0] != 0x42 || r[1] != 0x42)
            continue;
        at = 12;
        for (i = 0; i < (r[4] << 8 | r[5]); i++) {   /* (the question back) */
            char nm[256];
            if (rd_name(r, k, &at, nm, sizeof nm))
                break;
            at += 4;
        }
        an = r[6] << 8 | r[7], ar = r[10] << 8 | r[11];
        for (i = 0; i < an + ar && at < k; i++) {
            char nm[256], tgt[256];
            int type, len, rd;
            if (rd_name(r, k, &at, nm, sizeof nm) || at + 10 > k)
                break;
            type = r[at] << 8 | r[at + 1];
            len = r[at + 8] << 8 | r[at + 9];
            at += 10;
            rd = at;
            if (type == 12 && !strcasecmp(nm, T) && !rd_name(r, k, &rd, tgt, sizeof tgt)) {
                char *dot = strstr(tgt, "._apple-midi");
                if (dot)
                    *dot = 0;
                snprintf(inst, (size_t)icap, "%s", tgt);
            } else if (type == 33 && len >= 6)
                port = r[at + 4] << 8 | r[at + 5];
            at += len;
        }
    }
    close(fd);
    return inst[0] ? port : -1;
}

/* ---- AppleMIDI */
static void exch(int fd, int port, const char *cmd, const char *name)
{
    uint8_t b[96];
    struct sockaddr_in to = addr(port);
    int n = 16;
    b[0] = b[1] = 0xFF, b[2] = (uint8_t)cmd[0], b[3] = (uint8_t)cmd[1];
    put32(b + 4, 2), put32(b + 8, g_token), put32(b + 12, g_ssrc);
    if (name) {
        strcpy((char *)b + 16, name);
        n += (int)strlen(name) + 1;
    }
    sendto(fd, b, (size_t)n, 0, (struct sockaddr *)&to, sizeof to);
}

static int invite(int fd, int port, const char *want)
{
    int t;
    for (t = 0; t < 5; t++) {
        uint8_t b[256];
        int k;
        exch(fd, port, "IN", "rtpmidi-peer");
        if ((k = wait_rx(fd, b, sizeof b - 1, 1000)) >= 16 && b[0] == 0xFF && b[2] == 'O' && b[3] == 'K' &&
            get32(b + 8) == g_token) {
            b[k] = 0;
            printf("OK on %d from \"%s\" (ssrc %08x)\n", port, k > 16 ? (char *)b + 16 : "", get32(b + 12));
            if (want && strcmp(want, k > 16 ? (char *)b + 16 : "")) {
                printf("  the name is not \"%s\"\n", want);
                return -1;
            }
            return 0;
        }
    }
    return -1;
}

static int clock_sync(int fd, int port)
{
    uint8_t b[64];
    struct sockaddr_in to = addr(port);
    int t, k;
    for (t = 0; t < 5; t++) {
        const uint64_t t1 = now10k();
        memset(b, 0, 36);
        b[0] = b[1] = 0xFF, b[2] = 'C', b[3] = 'K';
        put32(b + 4, g_ssrc);
        put32(b + 12, (uint32_t)(t1 >> 32)), put32(b + 16, (uint32_t)t1);
        sendto(fd, b, 36, 0, (struct sockaddr *)&to, sizeof to);
        if ((k = wait_rx(fd, b, sizeof b, 1000)) >= 36 && b[2] == 'C' && b[3] == 'K' && b[8] == 1 &&
            get32(b + 16) == (uint32_t)t1) {
            const uint64_t t2 = (uint64_t)get32(b + 20) << 32 | get32(b + 24), t3 = now10k();
            b[8] = 2;
            put32(b + 4, g_ssrc);
            put32(b + 28, (uint32_t)(t3 >> 32)), put32(b + 32, (uint32_t)t3);
            sendto(fd, b, 36, 0, (struct sockaddr *)&to, sizeof to);
            printf("CK: round trip %.1f ms, their clock %llu\n", (double)(t3 - t1) / 10.0, (unsigned long long)t2);
            return 0;
        }
    }
    return -1;
}

/* the MIDI list of one RTP-MIDI packet into out (segmented SysEx put back together): bytes added, -1 damaged */
static int midi_list(const uint8_t *b, int n, uint8_t *out, int *cmds)
{
    int at = 12, len, end, k = 0, first = 1, rs = 0;
    if (n < 13 || (b[0] & 0xC0) != 0x80 || (b[1] & 0x7F) != 0x61)
        return -1;
    if (b[at] & 0x80)
        len = (b[at] & 0x0F) << 8 | b[at + 1], at += 2;
    else
        len = b[at] & 0x0F, at += 1;
    if (b[12] & 0x20)   /* (Z: the first has a delta too) */
        first = 0;
    end = at + len;
    if (end > n)
        return -1;
    while (at < end) {
        int st, l, i;
        if (!first) {   /* the delta time */
            for (i = 0; i < 4 && at < end && (b[at] & 0x80); i++)
                at++;
            at++;
        }
        first = 0;
        if (at >= end)
            break;
        st = b[at];
        if (st < 0x80) {   /* (running status) */
            if (!rs)
                return -1;
            out[k++] = (uint8_t)rs;
            st = rs;
            l = ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 1 : 2;
            memcpy(out + k, b + at, (size_t)l);
            k += l, at += l;
            (*cmds)++;
            continue;
        }
        if (st == 0xF0 || st == 0xF7) {   /* SysEx, whole or a segment: F0 .. F7, F0 .. F0, F7 .. F0, F7 .. F7 */
            int j = at + 1;
            while (j < end && b[j] != 0xF7 && b[j] != 0xF0)
                j++;
            if (j >= end)
                return -1;
            if (st == 0xF0)
                out[k++] = 0xF0;
            memcpy(out + k, b + at + 1, (size_t)(j - at - 1));
            k += j - at - 1;
            if (b[j] == 0xF7)
                out[k++] = 0xF7;
            at = j + 1;
            (*cmds)++;
            continue;
        }
        if (st < 0xF0)
            l = ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 2 : 3, rs = st;
        else
            l = st == 0xF1 || st == 0xF3 ? 2 : st == 0xF2 ? 3 : 1;
        memcpy(out + k, b + at, (size_t)l);
        k += l, at += l;
        (*cmds)++;
    }
    return k;
}

int main(int argc, char **argv)
{
    static uint8_t want[16384], got[65536];
    int i, port = 0, mport = 0, nwant = 0, ngot = 0, timeout = 15, pkts = 0, cmds = 0, cfd, dfd;
    const char *ready = NULL, *name = NULL, *hex = NULL;
    char inst[128] = "";
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc)
            g_host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mdns-port") && i + 1 < argc)
            mport = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)
            name = argv[++i];
        else if (!strcmp(argv[i], "--ready") && i + 1 < argc)
            ready = argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc)
            timeout = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--expect") && i + 1 < argc)
            hex = argv[++i];
        else {
            fprintf(stderr, "usage: rtpmidi-peer [--host IP] [--mdns-port N] [--port N] [--name NAME] [--ready FILE] "
                            "[--timeout S] --expect HEX\n");
            return 2;
        }
    }
    for (i = 0; hex && hex[2 * i] && hex[2 * i + 1] && nwant < (int)sizeof want; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        want[nwant++] = (uint8_t)v;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (mport) {
        const int p = mdns_find(mport, inst, sizeof inst);
        printf("mdns: \"%s\" port %d\n", inst, p);
        if (p <= 0 || (name && strcmp(inst, name)))
            return 1;
        if (!port)
            port = p;
        else if (port != p)
            return printf("mdns: the port is not %d\n", port), 1;
    }
    if (port <= 0)
        return 2;
    cfd = udp(), dfd = udp();
    if (invite(cfd, port, name) || invite(dfd, port + 1, name) || clock_sync(dfd, port + 1)) {
        printf("no session\n");
        return 1;
    }
    if (ready) {
        FILE *f = fopen(ready, "w");
        if (f)
            fclose(f);
    }
    printf("READY\n");
    {
        const uint64_t until = now10k() + (uint64_t)timeout * 10000u;
        while (ngot < nwant && now10k() < until) {
            uint8_t b[1500];
            int k = wait_rx(dfd, b, sizeof b, 100), m;
            if (k <= 0 || b[0] == 0xFF)
                continue;
            if ((m = midi_list(b, k, got + ngot, &cmds)) < 0) {
                printf("a damaged packet (%d bytes)\n", k);
                return 1;
            }
            ngot += m;
            pkts++;
            {   /* RS: what came, on the control port */
                uint8_t r[12];
                struct sockaddr_in to = addr(port);
                r[0] = r[1] = 0xFF, r[2] = 'R', r[3] = 'S';
                put32(r + 4, g_ssrc);
                put32(r + 8, (uint32_t)(b[2] << 8 | b[3]) << 16);
                sendto(cfd, r, sizeof r, 0, (struct sockaddr *)&to, sizeof to);
            }
        }
    }
    printf("got %d bytes in %d packet(s), %d command(s): ", ngot, pkts, cmds);
    for (i = 0; i < ngot && i < 24; i++)
        printf("%02x", got[i]);
    printf("%s\n", ngot > 24 ? ".." : "");
    exch(cfd, port, "BY", NULL);
    return ngot == nwant && !memcmp(got, want, (size_t)nwant) ? 0 : 1;
}
