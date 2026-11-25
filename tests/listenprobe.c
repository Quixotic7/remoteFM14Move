/* SPDX-License-Identifier: MPL-2.0 */
/* listenprobe: a Listen here page from the test's side (src/listen.h, web.c /ws/audio). Connects to
 * ws://127.0.0.1:PORT/ws/audio, reads the audio frames for MS ms and says "hb" every 500 ms, as the page does.
 *   listenprobe PORT MS [--spk] [--quiet-after T] [--spk-at T:V]..
 * --spk: ask to keep the Move's speaker on (?spk=1). --quiet-after T: from T ms on, neither a heartbeat nor a read (a
 * page frozen in the background); the end then says whether the host closed the connection. --spk-at T:V: "spk V"
 * sent T ms after the connection.
 * Prints one line: frames, the frames with sound (peak > 64), the gaps in the frames' numbers, the peak, the frames'
 * rate and size, the last state message, whether the host closed it ("closed 1"). Exit 0 when the handshake came. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static long ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void send_text(int fd, const char *s)
{
    uint8_t f[64];
    const uint8_t mask[4] = { 9, 8, 7, 6 };
    const size_t n = strlen(s);
    size_t i;
    f[0] = 0x81;
    f[1] = (uint8_t)(0x80 | n);
    memcpy(f + 2, mask, 4);
    for (i = 0; i < n; i++)
        f[6 + i] = (uint8_t)s[i] ^ mask[i & 3];
    if (write(fd, f, 6 + n) < 0)
        perror("write");
}

int main(int argc, char **argv)
{
    int port, dur, spk = 0, fd, i, ok = 0, closed = 0, nspk = 0;
    long quiet = -1, t0, last_hb = -1000;
    struct { long at; int v, sent; } spks[8];
    struct sockaddr_in a;
    static uint8_t buf[1 << 20];
    size_t n = 0;
    char req[256], state[256] = "";
    long frames = 0, loud = 0, gaps = 0, peak = 0, rate = 0, fsize = 0, seq_next = -1;
    if (argc < 3)
        return 2;
    port = atoi(argv[1]);
    dur = atoi(argv[2]);
    for (i = 3; i < argc; i++)
        if (!strcmp(argv[i], "--spk"))
            spk = 1;
        else if (!strcmp(argv[i], "--quiet-after") && i + 1 < argc)
            quiet = atol(argv[++i]);
        else if (!strcmp(argv[i], "--spk-at") && i + 1 < argc && nspk < 8) {
            if (sscanf(argv[++i], "%ld:%d", &spks[nspk].at, &spks[nspk].v) == 2)
                spks[nspk++].sent = 0;
        }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(0x7F000001);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        perror("connect");
        return 1;
    }
    snprintf(req, sizeof req, "GET /ws/audio?spk=%d HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", spk);
    if (write(fd, req, strlen(req)) < 0)
        return 1;
    t0 = ms();
    while (ms() - t0 < dur) {
        struct pollfd pf = { fd, POLLIN, 0 };
        const long t = ms() - t0;
        const int quiet_now = quiet >= 0 && t >= quiet;
        if (quiet_now) {   /* (frozen: no reads, no heartbeat; a look at the end only) */
            usleep(20000);
            continue;
        }
        if (t - last_hb >= 500) {
            send_text(fd, "hb");
            last_hb = t;
        }
        for (i = 0; i < nspk; i++)
            if (!spks[i].sent && t >= spks[i].at) {
                char m[8];
                snprintf(m, sizeof m, "spk %d", spks[i].v ? 1 : 0);
                send_text(fd, m);
                spks[i].sent = 1;
            }
        if (poll(&pf, 1, 20) <= 0)
            continue;
        {
            const ssize_t r = read(fd, buf + n, sizeof buf - n);
            if (r <= 0) {
                closed = 1;
                break;
            }
            n += (size_t)r;
        }
        for (;;) {   /* the 101 first, then frames */
            size_t h = 2, len;
            if (n >= 4 && !memcmp(buf, "HTTP", 4)) {
                char *e;
                buf[n < sizeof buf ? n : sizeof buf - 1] = 0;
                if (!(e = strstr((char *)buf, "\r\n\r\n")))
                    break;
                if (!strstr((char *)buf, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="))
                    return printf("listenprobe: a wrong handshake\n"), 1;
                ok = 1;
                h = (size_t)(e + 4 - (char *)buf);
                memmove(buf, buf + h, n - h);
                n -= h;
                continue;
            }
            if (n < 2)
                break;
            len = buf[1] & 0x7F;
            if (len == 126) {
                if (n < 4)
                    break;
                len = (size_t)buf[2] << 8 | buf[3], h = 4;
            } else if (len == 127)
                return printf("listenprobe: a 64-bit frame\n"), 1;
            if (n < h + len)
                break;
            {
                const uint8_t *d = buf + h;
                const int op = buf[0] & 0x0F;
                if (op == 0x1) {
                    snprintf(state, sizeof state, "%.*s", (int)(len < 255 ? len : 255), (const char *)d);
                } else if (op == 0x2 && len >= 12 && d[0] == 'L') {
                    const long nf = d[2] | d[3] << 8;
                    const long seq = (long)((uint32_t)d[4] | (uint32_t)d[5] << 8 | (uint32_t)d[6] << 16 | (uint32_t)d[7] << 24);
                    long p = 0, k;
                    rate = (long)((uint32_t)d[8] | (uint32_t)d[9] << 8 | (uint32_t)d[10] << 16 | (uint32_t)d[11] << 24);
                    fsize = nf;
                    if (seq_next >= 0 && seq != seq_next)
                        gaps++;
                    seq_next = seq + 1;
                    for (k = 0; k < 2 * nf && 12 + 2 * k + 1 < (long)len; k++) {
                        const int v = (int16_t)(d[12 + 2 * k] | d[13 + 2 * k] << 8);
                        const long av = v < 0 ? -v : v;
                        if (av > p)
                            p = av;
                    }
                    frames++;
                    if (p > 64)
                        loud++;
                    if (p > peak)
                        peak = p;
                } else if (op == 0x8) {
                    closed = 1;
                }
            }
            memmove(buf, buf + h + len, n - h - len);
            n -= h + len;
        }
        if (closed)
            break;
    }
    if (quiet >= 0 && !closed) {   /* (the end of a frozen stretch: did the host let it go?) */
        char tmp[65536];
        struct pollfd pf = { fd, POLLIN, 0 };
        while (poll(&pf, 1, 50) > 0) {
            const ssize_t r = read(fd, tmp, sizeof tmp);
            if (r <= 0) {
                closed = 1;
                break;
            }
        }
    }
    printf("listenprobe: frames %ld loud %ld gaps %ld peak %ld rate %ld size %ld closed %d state %s\n", frames, loud,
           gaps, peak, rate, fsize, closed, state);
    close(fd);
    return ok ? 0 : 1;
}
