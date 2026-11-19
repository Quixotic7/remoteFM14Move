/* SPDX-License-Identifier: MPL-2.0 */
/* The page on :7730: see web.h. */
#define _GNU_SOURCE
#include "web.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "engine.h"
#include "minijson.h"
#include "mr_ws.h"
#include "storage.h"

#define MAXC 24     /* (a page loads ~40 files, 6 at a time, beside its WebSocket; a second page, a phone) */
#define FILE_MAX (2 * 1024 * 1024)
#define IN_CAP 65536
#define OUT_CAP (4 * 1024 * 1024)
#define LAG (1024 * 1024)   /* more than this queued for a page: no screens until it reads */

typedef struct {
    int fd, ws;
    uint8_t *in, *out;
    size_t in_n, out_n, out_off;
    int closing;
    uint32_t lcd_seq, info_seq, mix_seq, meters_seq, mini_seq[3];
    uint8_t leds[45];
    int leds_ok, full;
    uint8_t *lcd;            /* what this page has (for the changed rows) */
    uint64_t born;
} client_t;

static int g_listen = -1;
static char g_site[512];     /* the site's folder (web_start) */
static client_t g_c[MAXC];
static pthread_t g_thr;
static volatile int g_run;
static engine_view_t g_v;    /* (the thread's copy) */
static uint32_t g_vseq;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void cfree(client_t *c)
{
    if (c->fd >= 0)
        close(c->fd);
    free(c->in), free(c->out), free(c->lcd);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static int put(client_t *c, const void *p, size_t n)
{
    if (c->out_off && c->out_n + n > OUT_CAP) {
        memmove(c->out, c->out + c->out_off, c->out_n - c->out_off);
        c->out_n -= c->out_off;
        c->out_off = 0;
    }
    if (c->out_n + n > OUT_CAP) {
        c->closing = 2;
        return -1;
    }
    memcpy(c->out + c->out_n, p, n);
    c->out_n += n;
    return 0;
}

static void ws_send(client_t *c, int op, const uint8_t *hdr, size_t hn, const uint8_t *p, size_t n)
{
    uint8_t h[10];
    size_t k = 0;
    const size_t len = hn + n;
    h[k++] = (uint8_t)(0x80 | op);
    if (len <= 125)
        h[k++] = (uint8_t)len;
    else if (len <= 65535)
        h[k++] = 126, h[k++] = (uint8_t)(len >> 8), h[k++] = (uint8_t)len;
    else {
        int i;
        h[k++] = 127;
        for (i = 7; i >= 0; i--)
            h[k++] = (uint8_t)((uint64_t)len >> (8 * i));
    }
    put(c, h, k);
    if (hn)
        put(c, hdr, hn);
    if (n)
        put(c, p, n);
}

static void http_reply(client_t *c, const char *status, const char *type, const void *body, size_t n)
{
    char h[320];
    const int k = snprintf(h, sizeof h,
                           "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-cache\r\n"
                           "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n", status, type, n);
    put(c, h, (size_t)k);
    if (n)
        put(c, body, n);
    c->closing = 1;
}

static const char *header(const char *req, const char *name, char *val, size_t cap)
{
    const char *p = req;
    const size_t nl = strlen(name);
    while ((p = strchr(p, '\n'))) {
        p++;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            size_t k = 0;
            while (*v == ' ')
                v++;
            while (*v && *v != '\r' && *v != '\n' && k + 1 < cap)
                val[k++] = *v++;
            val[k] = 0;
            return val;
        }
    }
    return NULL;
}

/* a file's content type, by its name */
static const char *type_of(const char *path)
{
    static const char *T[][2] = {
        { ".html", "text/html; charset=utf-8" }, { ".js", "text/javascript; charset=utf-8" },
        { ".mjs", "text/javascript; charset=utf-8" }, { ".css", "text/css; charset=utf-8" },
        { ".json", "application/json" }, { ".woff", "font/woff" }, { ".woff2", "font/woff2" },
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".svg", "image/svg+xml" }, { ".ico", "image/x-icon" },
        { ".txt", "text/plain; charset=utf-8" }, { ".md", "text/plain; charset=utf-8" }, { ".wasm", "application/wasm" },
    };
    const char *dot = strrchr(path, '.');
    size_t i;
    for (i = 0; dot && i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(dot, T[i][0]))
            return T[i][1];
    return "application/octet-stream";
}

/* a path the page may ask for: letters, digits, - _ . / only, no "..", no hidden files */
static int safe_path(const char *p)
{
    const char *q;
    if (*p != '/' || strstr(p, "..") || strstr(p, "/."))
        return 0;
    for (q = p; *q; q++)
        if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || strchr("-_./", *q)))
            return 0;
    return 1;
}

static void send_file(client_t *c, const char *file, const char *type)
{
    FILE *f = fopen(file, "rb");
    long len;
    uint8_t *b;
    if (!f) {
        http_reply(c, "404 Not Found", "text/plain", "not found\n", 10);
        return;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > FILE_MAX || !(b = malloc((size_t)len + 1))) {
        fclose(f);
        http_reply(c, "500 Internal Server Error", "text/plain", "", 0);
        return;
    }
    if (fread(b, 1, (size_t)len, f) != (size_t)len)
        len = -1;
    fclose(f);
    if (len < 0)
        http_reply(c, "500 Internal Server Error", "text/plain", "", 0);
    else
        http_reply(c, "200 OK", type ? type : type_of(file), b, (size_t)len);
    free(b);
}

static void json_reply(client_t *c, const char *status, const char *json)
{
    http_reply(c, status, "application/json", json, strlen(json));
}

/* POST /api/load {"slot": 0..2, "id": "<firmware id>", "activate": 0|1}: the firmware into that slot (engine_load) */
static void api_load(client_t *c, const char *body)
{
    char id[64];
    long slot = -1, act = 0;
    if (!body || mj_string(body, "id", id, sizeof id) || mj_int(body, "slot", &slot)) {
        json_reply(c, "400 Bad Request", "{\"ok\":false,\"error\":\"want {slot, id}\"}");
        return;
    }
    mj_int(body, "activate", &act);
    if (engine_load((int)slot, id, act != 0) != 0) {
        json_reply(c, "404 Not Found", "{\"ok\":false,\"error\":\"no such firmware or slot\"}");
        return;
    }
    json_reply(c, "202 Accepted", "{\"ok\":true}");
}

/* POST /api/edit {"op": 16..24, "a": n, "b": n}: a Mix & FX edit or a slot switch (engine.h ENG_OP_*), as the
 * WebSocket's ops; POST /api/test/midi {"track": 0..16 (0: as the pads played live), "b0", "b1", "b2"}: tests only (--test-api) */
static void api_edit(client_t *c, const char *body, int test)
{
    long op = 0, a = 0, b = 0, t = 0, b0 = 0, b1 = 0, b2 = 0;
    if (!body || (test ? mj_int(body, "track", &t) || mj_int(body, "b0", &b0) || mj_int(body, "b1", &b1) ||
                         mj_int(body, "b2", &b2)
                       : mj_int(body, "op", &op) || mj_int(body, "a", &a) || mj_int(body, "b", &b))) {
        json_reply(c, "400 Bad Request", "{\"ok\":false}");
        return;
    }
    if (test) {
        if (engine_test_midi((int)t, (int)b0, (int)b1, (int)b2) != 0)
            json_reply(c, "403 Forbidden", "{\"ok\":false}");
        else
            json_reply(c, "202 Accepted", "{\"ok\":true}");
        return;
    }
    if (op < ENG_OP_ACTIVE || op > ENG_OP_LAST) {
        json_reply(c, "400 Bad Request", "{\"ok\":false,\"error\":\"no such op\"}");
        return;
    }
    engine_panel((int)op, (int)a, (int)b);
    json_reply(c, "202 Accepted", "{\"ok\":true}");
}

/* GET /api/fw/<id>-<version>.<what>: an installed firmware's licences or skin (RFM1-FORMAT.md section 8, kept beside
 * the compiled firmware by tools/build_fw.sh) from the RemoteFM1 folder's firmwares/ */
static void api_fw_file(client_t *c, const char *name)
{
    static const char *SUF[][2] = {
        { ".licenses.txt", "text/plain; charset=utf-8" }, { ".rfm1skin", "application/json" },
        { ".rfm1skin.bg", "application/octet-stream" }, { ".rfm1skin.bgl", "application/octet-stream" },
    };
    char p[ST_PATH];
    const size_t n = strlen(name);
    size_t i;
    if (strchr(name, '/') || !n)
        goto none;
    for (i = 0; i < sizeof SUF / sizeof SUF[0]; i++) {
        const size_t k = strlen(SUF[i][0]);
        if (n > k && !strcmp(name + n - k, SUF[i][0])) {
            st_path(p, sizeof p, "firmwares/%s", name);
            send_file(c, p, SUF[i][1]);
            return;
        }
    }
none:
    http_reply(c, "404 Not Found", "text/plain", "not found\n", 10);
}

static void request(client_t *c, const char *body)
{
    char path[256] = "", key[64];
    const char *req = (const char *)c->in;
    const int post = !strncmp(req, "POST ", 5);
    if (!post && sscanf(req, "GET %255s", path) != 1) {
        http_reply(c, "405 Method Not Allowed", "text/plain", "", 0);
        return;
    }
    if (post && sscanf(req, "POST %255s", path) != 1)
        path[0] = 0;
    path[strcspn(path, "?#")] = 0;
    if (post) {
        if (!strcmp(path, "/api/load"))
            api_load(c, body);
        else if (!strcmp(path, "/api/edit"))
            api_edit(c, body, 0);
        else if (!strcmp(path, "/api/test/midi"))
            api_edit(c, body, 1);
        else
            http_reply(c, "404 Not Found", "text/plain", "not found\n", 10);
        return;
    }
    if (!strcmp(path, "/ws") && header(req, "Sec-WebSocket-Key", key, sizeof key)) {
        char acc[32], h[256];
        int k;
        mr_ws_accept(key, acc);
        k = snprintf(h, sizeof h, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                  "Sec-WebSocket-Accept: %s\r\n\r\n", acc);
        put(c, h, (size_t)k);
        c->ws = 1;
        c->in_n = 0;
        c->full = 1;
        c->lcd = calloc(1, ENG_LCD_BYTES);
        return;
    }
    if (!strcmp(path, "/api/status")) {
        char s[8192];
        const int n = engine_status(s, sizeof s);
        http_reply(c, "200 OK", "application/json", s, (size_t)(n > 0 ? n : 0));
    } else if (!strcmp(path, "/api/info"))
        http_reply(c, "200 OK", "application/json", g_v.info, strlen(g_v.info));
    else if (!strcmp(path, "/api/mix"))
        http_reply(c, "200 OK", "application/json", g_v.mix, strlen(g_v.mix));
    else if (!strcmp(path, "/api/fx")) {
        char *j = engine_fx_modules();
        json_reply(c, "200 OK", j ? j : "{\"midi\":[],\"audio\":[]}");
        free(j);
    } else if (!strcmp(path, "/api/firmwares")) {
        char *j = engine_firmwares();
        json_reply(c, "200 OK", j ? j : "{\"firmwares\":[]}");
        free(j);
    } else if (!strncmp(path, "/api/fw/", 8))
        api_fw_file(c, path + 8);
    else if (!strncmp(path, "/api/", 5) || !safe_path(path) || !g_site[0])
        http_reply(c, "404 Not Found", "text/plain", "not found\n", 10);
    else {   /* the site (scripts/build_site.sh: RemoteFM1's web/site for the Move) */
        char file[sizeof g_site + 300];
        snprintf(file, sizeof file, "%s%s%s", g_site, path, path[strlen(path) - 1] == '/' ? "index.html" : "");
        if (!strcmp(path, "/favicon.ico") && access(file, R_OK) != 0)
            http_reply(c, "204 No Content", "image/x-icon", "", 0);   /* (the browser asks; the site has none) */
        else
            send_file(c, file, NULL);
    }
}

/* whole frames from a page: 4-byte panel messages; close; ping */
static int ws_in(client_t *c)
{
    size_t off = 0;
    for (;;) {
        uint8_t *f = c->in + off;
        const size_t have = c->in_n - off;
        size_t h = 2, plen, k;
        int op;
        if (have < 2)
            break;
        op = f[0] & 0x0F;
        plen = f[1] & 0x7F;
        if (plen == 126) {
            if (have < 4)
                break;
            plen = (size_t)f[2] << 8 | f[3], h = 4;
        } else if (plen == 127)
            return -1;   /* (nothing a page sends is that long) */
        if (!(f[1] & 0x80))
            return -1;
        if (have < h + 4 + plen)
            break;
        for (k = 0; k < plen; k++)
            f[h + 4 + k] ^= f[h + (k & 3)];
        {
            const uint8_t *d = f + h + 4;
            if (op == 0x8)
                return -1;
            if (op == 0x9)
                ws_send(c, 0xA, NULL, 0, d, plen > 125 ? 125 : plen);
            else if (op == 0x2)
                for (k = 0; k + 4 <= plen; k += 4)
                    if ((d[k] >= 1 && d[k] <= 5) || (d[k] >= ENG_OP_ACTIVE && d[k] <= ENG_OP_LAST))
                        /* KEY LEAN BTN ENC MASTER (fwrt.h FWI_*) and the Mix & FX ops (engine.h ENG_OP_*) only */
                        engine_panel(d[k], d[k + 1], (int16_t)(d[k + 2] | d[k + 3] << 8));
        }
        off += h + 4 + plen;
    }
    if (off) {
        memmove(c->in, c->in + off, c->in_n - off);
        c->in_n -= off;
    }
    return 0;
}

static void cread(client_t *c)
{
    for (;;) {
        const ssize_t r = recv(c->fd, c->in + c->in_n, IN_CAP - 1 - c->in_n, 0);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            cfree(c);
            return;
        }
        if (r < 0)
            return;
        c->in_n += (size_t)r;
        if (c->ws) {
            if (ws_in(c) < 0) {
                cfree(c);
                return;
            }
        } else {
            const char *end;
            c->in[c->in_n] = 0;
            if ((end = strstr((char *)c->in, "\r\n\r\n"))) {
                char v[32];
                const size_t head = (size_t)(end + 4 - (const char *)c->in);
                const long want = header((const char *)c->in, "Content-Length", v, sizeof v) ? atol(v) : 0;
                if (want < 0 || head + (size_t)want >= IN_CAP - 1) {
                    cfree(c);
                    return;
                }
                if (c->in_n < head + (size_t)want)
                    continue;   /* (the body is still coming) */
                request(c, (const char *)c->in + head);
                c->in_n = 0;   /* (one request per connection: Connection: close) */
                return;
            } else if (c->in_n >= IN_CAP - 1) {
                cfree(c);
                return;
            }
        }
        if (c->in_n >= IN_CAP - 1) {
            cfree(c);
            return;
        }
    }
}

static void cwrite(client_t *c)
{
    while (c->out_off < c->out_n) {
        const ssize_t w = send(c->fd, c->out + c->out_off, c->out_n - c->out_off, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                cfree(c);
            return;
        }
        c->out_off += (size_t)w;
    }
    c->out_off = c->out_n = 0;
}

/* what changed since this page's last look */
static void push(client_t *c)
{
    if (c->info_seq != g_v.info_seq) {
        c->info_seq = g_v.info_seq;
        ws_send(c, 0x1, NULL, 0, (const uint8_t *)g_v.info, strlen(g_v.info));
    }
    if (!c->leds_ok || memcmp(c->leds, g_v.leds, 44) || c->leds[44] != g_v.play_green) {
        uint8_t m[46];
        memcpy(c->leds, g_v.leds, 44);
        c->leds[44] = g_v.play_green;
        c->leds_ok = 1;
        m[0] = 2;
        memcpy(m + 1, c->leds, 45);
        ws_send(c, 0x2, NULL, 0, m, sizeof m);
    }
    if (c->mix_seq != g_v.mix_seq && g_v.mix[0]) {
        c->mix_seq = g_v.mix_seq;
        ws_send(c, 0x1, NULL, 0, (const uint8_t *)g_v.mix, strlen(g_v.mix));
    }
    if (c->meters_seq != g_v.meters_seq) {
        uint8_t m[8];
        c->meters_seq = g_v.meters_seq;
        m[0] = 3;
        memcpy(m + 1, g_v.meters, 7);
        ws_send(c, 0x2, NULL, 0, m, sizeof m);
    }
    {   /* the slots' mini screens (engine.c view_live: 48 x 48, at most 4 a second each) */
        int s;
        for (s = 0; s < 3; s++)
            if (c->mini_seq[s] != g_v.mini_seq[s] && c->out_n - c->out_off < LAG) {
                const uint8_t hdr[3] = { 4, (uint8_t)s, ENG_MINI };
                c->mini_seq[s] = g_v.mini_seq[s];
                ws_send(c, 0x2, hdr, 3, g_v.mini[s], sizeof g_v.mini[s]);
            }
    }
    if (c->lcd_seq != g_v.lcd_seq && c->out_n - c->out_off < LAG) {
        int y0 = 0, y1 = 239;
        const int row = 240 * 2;
        c->lcd_seq = g_v.lcd_seq;
        if (!c->full) {
            while (y0 <= 239 && !memcmp(c->lcd + y0 * row, g_v.lcd + y0 * row, (size_t)row))
                y0++;
            while (y1 >= y0 && !memcmp(c->lcd + y1 * row, g_v.lcd + y1 * row, (size_t)row))
                y1--;
        }
        c->full = 0;
        if (y0 <= y1) {
            const uint8_t hdr[3] = { 1, (uint8_t)y0, (uint8_t)(y1 - y0 + 1) };
            memcpy(c->lcd + y0 * row, g_v.lcd + y0 * row, (size_t)(y1 - y0 + 1) * (size_t)row);
            ws_send(c, 0x2, hdr, 3, g_v.lcd + y0 * row, (size_t)(y1 - y0 + 1) * (size_t)row);
        }
    }
}

static void *web_main(void *arg)
{
    (void)arg;
#ifdef __linux__
    pthread_setname_np(pthread_self(), "rfm1-web");
#endif
    while (g_run) {
        struct pollfd pf[MAXC + 1];
        client_t *pc[MAXC + 1];
        int np = 0, i;
        const uint64_t ms = now_ms();
        pf[np].fd = g_listen, pf[np].events = POLLIN, pc[np++] = NULL;
        for (i = 0; i < MAXC; i++)
            if (g_c[i].fd >= 0) {
                pf[np].fd = g_c[i].fd;
                pf[np].events = (short)(POLLIN | (g_c[i].out_n > g_c[i].out_off ? POLLOUT : 0));
                pc[np++] = &g_c[i];
            }
        poll(pf, (nfds_t)np, 20);
        if (pf[0].revents & POLLIN)
            for (;;) {
                const int fd = accept(g_listen, NULL, NULL);
                int one = 1;
                client_t *c = NULL;
                if (fd < 0)
                    break;
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                fcntl(fd, F_SETFD, FD_CLOEXEC);
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                for (i = 0; i < MAXC; i++)
                    if (g_c[i].fd < 0) {
                        c = &g_c[i];
                        break;
                    }
                if (!c) {
                    close(fd);
                    continue;
                }
                c->fd = fd;
                c->in = malloc(IN_CAP);
                c->out = malloc(OUT_CAP);
                c->born = ms;
                if (!c->in || !c->out)
                    cfree(c);
            }
        for (i = 1; i < np; i++)
            if (pc[i]->fd == pf[i].fd && (pf[i].revents & (POLLIN | POLLHUP | POLLERR)))
                cread(pc[i]);
        engine_view(&g_v, &g_vseq);
        for (i = 0; i < MAXC; i++) {
            client_t *c = &g_c[i];
            if (c->fd < 0)
                continue;
            if (c->ws && !c->closing)
                push(c);
            else if (!c->ws && ms - c->born > 10000u)
                c->closing = 2;   /* (a request that never came whole) */
            if (c->out_n > c->out_off)
                cwrite(c);
            if (c->fd >= 0 && (c->closing == 2 || (c->closing && c->out_n <= c->out_off)))
                cfree(c);
        }
    }
    return NULL;
}

int web_start(int port, const char *site)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0), one = 1, zero = 0, i;
    if (fd >= 0) {
        struct sockaddr_in6 a;
        memset(&a, 0, sizeof a);
        a.sin6_family = AF_INET6;
        a.sin6_addr = in6addr_any;
        a.sin6_port = htons((uint16_t)port);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
            close(fd);
            fd = -1;
        }
    }
    if (fd < 0) {
        struct sockaddr_in a;
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((uint16_t)port);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
            close(fd);
            return -1;
        }
    }
    if (listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    g_listen = fd;
    snprintf(g_site, sizeof g_site, "%s", site ? site : "");
    for (i = 0; i < MAXC; i++)
        g_c[i].fd = -1;
    g_run = 1;
    if (pthread_create(&g_thr, NULL, web_main, NULL) != 0) {
        close(fd);
        g_listen = -1;
        return -1;
    }
    fprintf(stderr, "web: http://<this Move>:%d/ (the site: %s)\n", port, g_site[0] ? g_site : "none");
    return 0;
}

void web_stop(void)
{
    int i;
    if (g_listen < 0)
        return;
    g_run = 0;
    pthread_join(g_thr, NULL);
    for (i = 0; i < MAXC; i++)
        if (g_c[i].fd >= 0)
            cfree(&g_c[i]);
    close(g_listen);
    g_listen = -1;
}
