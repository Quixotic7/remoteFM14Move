/* SPDX-License-Identifier: MPL-2.0 */
/* wsprobe: the page's WebSocket from the test's side. Connects to ws://127.0.0.1:PORT/ws, reads for MS ms, and
 * optionally plays a key (panel input: KEY down after 300 ms, up 300 ms before the end).
 *   wsprobe PORT MS [--key K] [--btn ID] [--sysex HEX] [--op OP:A:B@T]..
 * --op: one panel op (fwrt.h FWI_KEY .. FWI_MASTER: 1 KEY a key b velocity, 3 BTN a matrix button b down, 4 ENC a
 * matrix encoder b detents) sent T ms after the connection (the tests' FM-1 panel: a button held while a knob turns).
 * --sysex: MIDI bytes (hex, e.g. F07D464C01F7) sent as the page's MIDI (op 32) after 300 ms, in pieces of 5 bytes; the
 * firmware's MIDI out that comes back (op 5) is printed as hex ("wsprobe: midi out ...").
 * Prints what came: info messages, screen rows (and whether one whole screen came first), LED messages, the slot
 * the info names. Exit 0 when info, a whole screen and the LEDs all came. */
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

static void send_frame(int fd, const uint8_t *p, size_t n)
{
    uint8_t f[8 + 4096];
    const uint8_t mask[4] = { 1, 2, 3, 4 };
    size_t i, h = 2;
    if (n > 4096)
        return;
    f[0] = 0x82;
    if (n < 126)
        f[1] = (uint8_t)(0x80 | n);
    else
        f[1] = 0x80 | 126, f[2] = (uint8_t)(n >> 8), f[3] = (uint8_t)n, h = 4;
    memcpy(f + h, mask, 4);
    for (i = 0; i < n; i++)
        f[h + 4 + i] = p[i] ^ mask[i & 3];
    if (write(fd, f, h + 4 + n) < 0)
        perror("write");
}

int main(int argc, char **argv)
{
    int port, dur, key = -1, btn = -1, fd, i, infos = 0, leds = 0, rows = 0, whole = 0, first_screen = 1;
    struct sockaddr_in a;
    static uint8_t buf[1 << 22];
    size_t n = 0;
    char req[256], info[512] = "";
    long t0, down_at = -1;
    static uint8_t sx[4096], mo[65536];
    size_t nsx = 0, nmo = 0;
    int sx_sent = 0;
    struct { int op, a, b; long at; int sent; } ops[64];
    int nops = 0;
    if (argc < 3)
        return 2;
    port = atoi(argv[1]);
    dur = atoi(argv[2]);
    for (i = 3; i + 1 < argc; i += 2)
        if (!strcmp(argv[i], "--key"))
            key = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--btn"))
            btn = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--op") && nops < 64) {
            if (sscanf(argv[i + 1], "%d:%d:%d@%ld", &ops[nops].op, &ops[nops].a, &ops[nops].b, &ops[nops].at) == 4)
                ops[nops++].sent = 0;
        } else if (!strcmp(argv[i], "--sysex")) {
            const char *h = argv[i + 1];
            while (h[0] && h[1] && nsx < sizeof sx) {
                char b[3] = { h[0], h[1], 0 };
                sx[nsx++] = (uint8_t)strtoul(b, NULL, 16);
                h += 2;
            }
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
    snprintf(req, sizeof req, "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    if (write(fd, req, strlen(req)) < 0)
        return 1;
    t0 = ms();
    while (ms() - t0 < dur) {
        struct pollfd pf = { fd, POLLIN, 0 };
        const long t = ms() - t0;
        if ((key >= 0 || btn >= 0) && down_at < 0 && t >= 300) {
            const uint8_t m[4] = { (uint8_t)(key >= 0 ? 1 : 3), (uint8_t)(key >= 0 ? key : btn), key >= 0 ? 100 : 1, 0 };
            send_frame(fd, m, 4);
            down_at = t;
        }
        if ((key >= 0 || btn >= 0) && down_at >= 0 && down_at < 1000000 && t >= dur - 300) {
            const uint8_t m[4] = { (uint8_t)(key >= 0 ? 1 : 3), (uint8_t)(key >= 0 ? key : btn), 0, 0 };
            send_frame(fd, m, 4);
            down_at = 1000000;
        }
        for (i = 0; i < nops; i++)
            if (!ops[i].sent && t >= ops[i].at) {
                const uint8_t m[4] = { (uint8_t)ops[i].op, (uint8_t)ops[i].a, (uint8_t)(ops[i].b & 0xFF),
                                       (uint8_t)((ops[i].b >> 8) & 0xFF) };
                send_frame(fd, m, 4);
                ops[i].sent = 1;
            }
        if (nsx && !sx_sent && t >= 300) {   /* the page's MIDI: op 32 + bytes, in pieces of 5 */
            size_t k;
            for (k = 0; k < nsx; k += 5) {
                uint8_t m[6];
                const size_t take = nsx - k < 5 ? nsx - k : 5;
                m[0] = 32;
                memcpy(m + 1, sx + k, take);
                send_frame(fd, m, take + 1);
            }
            sx_sent = 1;
        }
        if (poll(&pf, 1, 20) <= 0)
            continue;
        {
            const ssize_t r = read(fd, buf + n, sizeof buf - n);
            if (r <= 0)
                break;
            n += (size_t)r;
        }
        for (;;) {   /* the 101 first, then frames */
            size_t h = 2, len;
            if (n >= 4 && !memcmp(buf, "HTTP", 4)) {
                char *e = strstr((char *)buf, "\r\n\r\n");
                if (!e)
                    break;
                if (!strstr((char *)buf, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="))
                    return printf("wsprobe: a wrong handshake\n"), 1;
                len = (size_t)(e + 4 - (char *)buf);
                memmove(buf, buf + len, n - len);
                n -= len;
                continue;
            }
            if (n < 2)
                break;
            len = buf[1] & 0x7F;
            if (len == 126) {
                if (n < 4)
                    break;
                len = (size_t)buf[2] << 8 | buf[3], h = 4;
            } else if (len == 127) {
                if (n < 10)
                    break;
                len = 0;
                for (i = 2; i < 10; i++)
                    len = len << 8 | buf[i];
                h = 10;
            }
            if (n < h + len)
                break;
            if ((buf[0] & 0x0F) == 1) {
                infos++;
                snprintf(info, sizeof info, "%.*s", (int)(len < 500 ? len : 500), (char *)buf + h);
            } else if ((buf[0] & 0x0F) == 2 && len >= 1) {
                if (buf[h] == 1 && len >= 3) {
                    rows += buf[h + 2];
                    if (first_screen)
                        whole = buf[h + 1] == 0 && buf[h + 2] == 240 && len == 3 + 240 * 480;
                    first_screen = 0;
                } else if (buf[h] == 2 && len == 46)
                    leds++;
                else if (buf[h] == 5)
                    for (i = 1; i < (int)len && nmo < sizeof mo; i++)
                        mo[nmo++] = buf[h + i];
            }
            memmove(buf, buf + h + len, n - h - len);
            n -= h + len;
        }
    }
    close(fd);
    printf("wsprobe: %d info, %d screen rows (the first a whole screen: %s), %d LED messages\n", infos, rows,
           whole ? "yes" : "no", leds);
    printf("wsprobe: info %s\n", info);
    if (nsx || nmo) {
        size_t k;
        printf("wsprobe: midi out %zu bytes:", nmo);
        for (k = 0; k < nmo && k < 2048; k++)
            printf(" %02X", mo[k]);
        printf("\n");
    }
    return infos > 0 && whole && leds > 0 ? 0 : 1;
}
