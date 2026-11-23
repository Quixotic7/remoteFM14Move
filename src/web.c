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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "engine.h"
#include "files.h"
#include "fm1link.h"
#include "fsvc.h"
#include "minijson.h"
#include "mr_ws.h"
#include "memsnap.h"
#include "projmem.h"
#include "storage.h"
#ifdef RFM14_PLAT_SIM
#include "fm1sim.h"
#endif

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
    /* an upload under way (POST /api/files/audio/<name>): its body streamed into a temporary file */
    int up_fd;               /* (-1 none; 0 is never used: cfree's memset) */
    uint64_t up_left, up_total;
    uint8_t up_head[12];
    char up_tmp[ST_PATH + 64], up_dst[ST_PATH + 64];
    /* the page's MIDI (ws op 32: the editors' SysEx in any chunking) assembled into whole messages; its MIDI out read
     * from this position (engine_midi_out) */
    uint8_t *mx;
    uint32_t mx_n;
    uint8_t mx_st, mx_d[2], mx_nd;
    int mx_sysex;
    uint32_t mo_pos;
    /* the file browser (/api/fs): an upload's kind (0 audio/'s, 1 /api/fs/put: no WAV check unless into audio/;
     * up_replace: may take an existing name's place); a download streamed from a file or a zip */
    int up_fs, up_magic, up_replace;
    int dl_fd;               /* (-1 / 0 none) */
    uint64_t dl_left;
    files_zip_t *zip;
    /* the page's States and projects (/api/store): a state or project file on its way (up_store: its request, filled
     * in as the upload starts), a request the engine is doing (sreq, answered when done; sdir: the file's folder,
     * removed then) */
    int up_store;
    eng_store_t up_sr;
    int sreq;
    uint64_t sreq_ms;
    char sdir[ST_PATH + 64];
} client_t;
#define UP_MAX (256ull * 1024 * 1024)      /* a WAV's size cap */
#define UP_RESERVE (64ull * 1024 * 1024)   /* left free on the disk after an upload */

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
    if (c->up_fd > 0) {      /* (an upload cut short: nothing kept) */
        close(c->up_fd);
        unlink(c->up_tmp);
    }
    if (c->dl_fd > 0)
        close(c->dl_fd);
    files_zip_free(c->zip);
    if (c->fd >= 0)
        close(c->fd);
    free(c->in), free(c->out), free(c->lcd), free(c->mx);
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

/* POST /api/download {"id"}: a catalog firmware downloaded, checked and installed (engine_download: as the Move's
 * own catalog does it; the status's dl_* follow it) */
static void api_download(client_t *c, const char *body)
{
    char id[64];
    if (!body || mj_string(body, "id", id, sizeof id)) {
        json_reply(c, "400 Bad Request", "{\"ok\":false,\"error\":\"want {id}\"}");
        return;
    }
    if (engine_download(id) != 0) {
        json_reply(c, "409 Conflict", "{\"ok\":false,\"error\":\"a bad id, or a download already asked for\"}");
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

/* The FM-1's Wi-Fi set up (fm1link.h fm1link_wifi_*), the FM-1 on the Move's USB-A:
 *   GET  /api/fm1/wifi            its state, the networks it hears and keeps, a set up under way
 *   POST /api/fm1/wifi/scan       look for networks (the list in GET's "scan" once done)
 *   POST /api/fm1/wifi/set        {"ssid", "password" (none or "": an open network), "pick", "scan_seq"}: join it
 *   POST /api/fm1/wifi/forget     {"ssid"}: a network it keeps
 * 409: no FM-1 with RemoteFM1 Wi-Fi on USB-A; 400: a bad request. The password: only in this body, which is wiped
 * once read (never logged, never in a URL, never kept on the Move) */
static void wipe_str(const char *p)
{
    volatile char *v = (volatile char *)(uintptr_t)p;
    while (v && *v)
        *v++ = 0;
}

static void api_fm1_wifi(client_t *c, const char *what, const char *body)
{
    static const char NO_UNIT[] = "{\"ok\":false,\"error\":\"no FM-1 with RemoteFM1 Wi-Fi on the Move's USB-A\"}";
    int r;
    if (!strcmp(what, "scan")) {
        r = fm1link_wifi_scan();
    } else if (!strcmp(what, "set")) {
        char ssid[128] = "", pass[160] = "";
        long pick = -1, seq = 0;
        if (!body || mj_string(body, "ssid", ssid, sizeof ssid)) {
            wipe_str(body);
            json_reply(c, "400 Bad Request", "{\"ok\":false,\"error\":\"want {ssid, password}\"}");
            return;
        }
        mj_string(body, "password", pass, sizeof pass);   /* (none: an open network) */
        mj_int(body, "pick", &pick);
        mj_int(body, "scan_seq", &seq);
        wipe_str(body);                              /* (the request's copy of the password) */
        r = strlen(pass) >= sizeof pass - 1 ? -2 : fm1link_wifi_set(ssid, pass, (int)pick, (int)seq);   /* (wipes pass) */
        wipe_str(pass);
        if (r == -2) {
            json_reply(c, "400 Bad Request", "{\"ok\":false,\"error\":\"a network name of 1 to 32 bytes; a password of 8 to 63 characters, or none for an open network\"}");
            return;
        }
    } else if (!strcmp(what, "forget")) {
        char ssid[128] = "";
        if (!body || mj_string(body, "ssid", ssid, sizeof ssid)) {
            json_reply(c, "400 Bad Request", "{\"ok\":false,\"error\":\"want {ssid}\"}");
            return;
        }
        r = fm1link_wifi_forget(ssid);
        if (r == -2) {
            json_reply(c, "404 Not Found", "{\"ok\":false,\"error\":\"the FM-1 does not keep that network\"}");
            return;
        }
    } else {
        http_reply(c, "404 Not Found", "text/plain", "not found\n", 10);
        return;
    }
    if (r != 0)
        json_reply(c, "409 Conflict", NO_UNIT);
    else
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

/* ------------------------------------------------------------------ the audio folder (a Fantasy CFW's samples) --- */
/* GET /api/files/audio: the RemoteFM1 folder's audio/ (FM1VST docs/VIRTUAL-CARTS.md section 3: every Fantasy CFW
 * reads it): {"files": [{"name", "size", "mtime", "dir"}], "free": bytes, "max": a WAV's cap}, A-Z;
 * POST /api/files/audio/<name> (the body: the file, Content-Length) adds one, DELETE /api/files/audio/<name> removes
 * one. WAV only (RIFF/WAVE), a plain name (letters, digits, space _ - . , ( ) +; not hidden; .wav), the size capped
 * and the disk kept UP_RESERVE free; written to a temporary file, renamed when whole. The carts' fs_changed moves. */
static int name_ok(const char *n)
{
    const size_t k = strlen(n);
    size_t i;
    if (k < 5 || k > 96 || n[0] == '.' || n[0] == ' ' || strcasecmp(n + k - 4, ".wav"))
        return 0;
    for (i = 0; i < k; i++)
        if (!((n[i] >= 'a' && n[i] <= 'z') || (n[i] >= 'A' && n[i] <= 'Z') || (n[i] >= '0' && n[i] <= '9') ||
              strchr(" _-.,()+", n[i])))
            return 0;
    return !strstr(n, "..");
}
/* the URL's name, %XX decoded, checked: 0, or -1 */
static int url_name(const char *in, char *out, size_t cap)
{
    size_t k = 0;
    for (; *in && k + 1 < cap; in++) {
        if (*in == '%' && in[1] && in[2]) {
            char hx[3] = { in[1], in[2], 0 };
            char *end;
            const long v = strtol(hx, &end, 16);
            if (*end || v <= 0 || v > 0x7e)
                return -1;
            out[k++] = (char)v;
            in += 2;
        } else
            out[k++] = *in == '+' ? ' ' : *in;
    }
    out[k] = 0;
    return *in || !name_ok(out) ? -1 : 0;
}
static uint64_t disk_free(void)
{
    struct statvfs v;
#ifdef RFM14_TEST_BUILD
    {   /* (tests: the free space a full disk would have) */
        const char *e = getenv("RFM14_TEST_DISK_FREE_MB");
        if (e && *e)
            return (uint64_t)atoll(e) << 20;
    }
#endif
    return statvfs(st_root(), &v) == 0 ? (uint64_t)v.f_bavail * v.f_frsize : 0;
}
static int by_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void api_audio_list(client_t *c)
{
    char dir[ST_PATH], **names = NULL;
    size_t n = 0, cap = 0, i, o = 0, ocap = 256;
    char *out = malloc(ocap);
    DIR *d;
    struct dirent *e;
    st_path(dir, sizeof dir, "audio");
    if ((d = opendir(dir))) {
        while ((e = readdir(d)) && n < 4096) {
            if (e->d_name[0] == '.')
                continue;
            if (n == cap) {
                char **nn = realloc(names, (cap ? cap * 2 : 64) * sizeof *names);
                if (!nn)
                    break;
                names = nn;
                cap = cap ? cap * 2 : 64;
            }
            if (!(names[n] = strdup(e->d_name)))
                break;
            n++;
        }
        closedir(d);
    }
    if (n)
        qsort(names, n, sizeof *names, by_str);
    if (!out) {
        http_reply(c, "500 Internal Server Error", "text/plain", "", 0);
        return;
    }
    o += (size_t)snprintf(out, ocap, "{\"files\":[");
    for (i = 0; i < n; i++) {
        char p[ST_PATH + 260], esc[600];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        if (stat(p, &st) == 0) {
            mj_escape(names[i], esc, sizeof esc);
            if (o + strlen(esc) + 128 > ocap) {
                char *b = realloc(out, ocap = ocap * 2 + strlen(esc) + 128);
                if (!b)
                    break;
                out = b;
            }
            o += (size_t)snprintf(out + o, ocap - o, "%s{\"name\":\"%s\",\"size\":%llu,\"mtime\":%ld,\"dir\":%s}",
                                  o > 10 ? "," : "", esc, (unsigned long long)st.st_size, (long)st.st_mtime,
                                  S_ISDIR(st.st_mode) ? "true" : "false");
        }
    }
    for (i = 0; i < n; i++)
        free(names[i]);
    free(names);
    if (o + 96 > ocap) {
        char *b = realloc(out, ocap = o + 96);
        if (b)
            out = b;
    }
    if (o + 96 <= ocap)
        snprintf(out + o, ocap - o, "],\"free\":%llu,\"max\":%llu}", (unsigned long long)disk_free(), UP_MAX);
    json_reply(c, "200 OK", out);
    free(out);
}

static void api_audio_delete(client_t *c, const char *urlname)
{
    char name[128], p[ST_PATH + 160];
    struct stat st;
    if (url_name(urlname, name, sizeof name) != 0) {
        json_reply(c, "400 Bad Request", "{\"ok\":false,\"error\":\"not a WAV's name\"}");
        return;
    }
    st_path(p, sizeof p, "audio/%s", name);
    if (lstat(p, &st) != 0 || !S_ISREG(st.st_mode)) {
        json_reply(c, "404 Not Found", "{\"ok\":false,\"error\":\"no such file\"}");
        return;
    }
    if (unlink(p) != 0) {
        json_reply(c, "500 Internal Server Error", "{\"ok\":false,\"error\":\"cannot delete it\"}");
        return;
    }
    fsvc_bump_changed();
    fprintf(stderr, "web: audio/%s deleted\n", name);
    json_reply(c, "200 OK", "{\"ok\":true}");
}

static int fs_put_start(client_t *c, const char *query, long want);
static int store_put_start(client_t *c, const char *query, long want);
static void store_put_done(client_t *c);
/* the headers of POST /api/files/audio/<name> or /api/fs/put?.. seen: 1 an upload starts (or was refused: replied),
 * 0 not one */
static int upload_start(client_t *c, const char *req, long want)
{
    char path[4096], name[128];
    if (sscanf(req, "POST %4095s", path) != 1)
        return 0;
    if (!strncmp(path, "/api/fs/put?", 12)) {
        c->closing = 0;
        return fs_put_start(c, path + 12, want);
    }
    if (!strncmp(path, "/api/store/put?", 15)) {
        c->closing = 0;
        return store_put_start(c, path + 15, want);
    }
    if (strncmp(path, "/api/files/audio/", 17))
        return 0;
    path[strcspn(path, "?#")] = 0;
    c->closing = 0;
    c->up_fs = 0, c->up_magic = 1, c->up_replace = 1;
    if (url_name(path + 17, name, sizeof name) != 0) {
        json_reply(c, "400 Bad Request", "{\"ok\":false,\"error\":\"a WAV file's name: letters, digits, space _ - . , ( ) +, ending .wav\"}");
        return 1;
    }
    if (want <= 44) {
        json_reply(c, "411 Length Required", "{\"ok\":false,\"error\":\"the file, with its Content-Length\"}");
        return 1;
    }
    if ((uint64_t)want > UP_MAX) {
        json_reply(c, "413 Payload Too Large", "{\"ok\":false,\"error\":\"at most 256 MB a file\"}");
        return 1;
    }
    if ((uint64_t)want + UP_RESERVE > disk_free()) {
        json_reply(c, "507 Insufficient Storage", "{\"ok\":false,\"error\":\"not enough room on the Move\"}");
        return 1;
    }
    st_path(c->up_dst, sizeof c->up_dst, "audio/%s", name);
    st_path(c->up_tmp, sizeof c->up_tmp, "audio/.upload-%d.part", (int)(c - g_c));
    c->up_fd = open(c->up_tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (c->up_fd <= 0) {
        c->up_fd = -1;
        json_reply(c, "500 Internal Server Error", "{\"ok\":false,\"error\":\"cannot write in audio/\"}");
        return 1;
    }
    c->up_left = c->up_total = (uint64_t)want;
    return 1;
}

/* body bytes of an upload: written; whole: checked, renamed, answered. -1: the client is done with (replied) */
static void upload_feed(client_t *c, const uint8_t *b, size_t n)
{
    const uint64_t at = c->up_total - c->up_left;
    if (n > c->up_left)
        n = (size_t)c->up_left;
    if (at < sizeof c->up_head)
        memcpy(c->up_head + at, b, n < sizeof c->up_head - at ? n : sizeof c->up_head - at);
    while (n) {
        const ssize_t w = write(c->up_fd, b, n);
        if (w <= 0) {
            close(c->up_fd);
            c->up_fd = -1;
            unlink(c->up_tmp);
            json_reply(c, "507 Insufficient Storage", "{\"ok\":false,\"error\":\"the write failed (disk full?)\"}");
            return;
        }
        b += w, n -= (size_t)w, c->up_left -= (uint64_t)w;
    }
    if (c->up_left)
        return;
    close(c->up_fd);
    c->up_fd = -1;
    if (c->up_store) {   /* (a state or project file: checked, then the engine puts it in place) */
        store_put_done(c);
        return;
    }
    if (c->up_magic && (memcmp(c->up_head, "RIFF", 4) || memcmp(c->up_head + 8, "WAVE", 4)) &&
        (!c->up_fs || memcmp(c->up_head, "FORM", 4) || (memcmp(c->up_head + 8, "AIFF", 4) && memcmp(c->up_head + 8, "AIFC", 4)))) {
        unlink(c->up_tmp);
        json_reply(c, "415 Unsupported Media Type", c->up_fs ? "{\"ok\":false,\"error\":\"audio/ takes WAV and AIFF files only\"}"
                                                             : "{\"ok\":false,\"error\":\"not a WAV file (RIFF/WAVE)\"}");
        return;
    }
    if (!c->up_replace && access(c->up_dst, F_OK) == 0) {   /* (made meanwhile) */
        unlink(c->up_tmp);
        json_reply(c, "409 Conflict", "{\"ok\":false,\"error\":\"a file of that name is there already\"}");
        return;
    }
    if (rename(c->up_tmp, c->up_dst) != 0) {
        unlink(c->up_tmp);
        json_reply(c, "500 Internal Server Error", "{\"ok\":false,\"error\":\"cannot keep it\"}");
        return;
    }
    fsvc_bump_changed();
    fprintf(stderr, "web: %s added (%llu bytes)\n", c->up_dst, (unsigned long long)c->up_total);
    json_reply(c, "201 Created", "{\"ok\":true}");
}

/* ------------------------------------------------------------------ the file browser (/api/fs) --- */
/* the RemoteFM1 folder, files.h's rules:
 *   GET  /api/fs/list?path=P                 a folder: {"path", "writable", "why", "entries": [{"name", "size",
 *                                            "mtime", "dir"}], "free", "max", "busy": the slots' ids, "open_project"}
 *   GET  /api/fs/get?path=P                  a file (streamed; attachment)
 *   GET  /api/fs/zip?path=P                  a folder as a .zip (store only, streamed, its length known first)
 *   POST /api/fs/put?path=DIR&name=N[&replace=1]   the body into DIR/N (FS_UP_MAX at most, FS_RESERVE left free;
 *                                            a temporary file, renamed when whole; audio/: WAV or AIFF only)
 *   POST /api/fs/mkdir?path=P                a new folder
 *   POST /api/fs/rename?path=P&to=Q          P moved to Q (a free name; both where the page may change things)
 *   POST /api/fs/delete?path=P[&recursive=1&confirm=NAME]   a file or an empty folder; a folder with what is in it
 *                                            only with recursive=1 and confirm= its own name
 * Refusals: 400 (the path), 403 (files.h: read-only, or in use: why), 404, 409 (there already, not empty), 413, 507. */
#define FS_UP_MAX (256ull * 1024 * 1024)
#define FS_RESERVE (64ull * 1024 * 1024)

/* a query parameter, %XX decoded: 0, -1 missing or bad */
static int qparam(const char *q, const char *key, char *out, size_t cap)
{
    const size_t kl = strlen(key);
    while (q && *q) {
        const char *amp = strchr(q, '&'), *end = amp ? amp : q + strlen(q);
        if (!strncmp(q, key, kl) && q[kl] == '=')
            return files_unescape(q + kl + 1, (size_t)(end - q - (long)kl - 1), out, cap);
        q = amp ? amp + 1 : NULL;
    }
    return -1;
}

static void fs_err(client_t *c, const char *status, const char *why)
{
    char esc[700], b[800];
    mj_escape(why, esc, sizeof esc);
    snprintf(b, sizeof b, "{\"ok\":false,\"error\":\"%s\"}", esc);
    json_reply(c, status, b);
}

static void fs_use(files_use_t *u)
{
    char p[ST_PATH];
    size_t n;
    uint8_t *t;
    engine_running_ids(u->ids);
    u->open_project = 0;   /* (no move/open-project yet: the core opened project 1) */
    st_path(p, sizeof p, "move/open-project");
    if ((t = st_read(p, &n))) {
        u->open_project = atoi((char *)t);
        if (u->open_project < 0 || u->open_project > 31)
            u->open_project = -1;
        free(t);
    }
}

/* the path parameter checked (the rules, no links on the way): 0, or replied */
static int fs_path(client_t *c, const char *q, const char *key, char *rel, size_t cap, int is_new, char *abs_, size_t acap)
{
    const char *why = NULL;
    if (qparam(q, key, rel, cap) != 0) {
        fs_err(c, "400 Bad Request", "no path, or a bad %-escape in it");
        return -1;
    }
    if (files_rel_ok(rel, is_new, &why) != 0) {
        char m[300];
        snprintf(m, sizeof m, "%s: %s", key, why);
        fs_err(c, "400 Bad Request", m);
        return -1;
    }
    if (files_no_links(st_root(), rel) != 0) {
        fs_err(c, "403 Forbidden", "a symbolic link on the way: not followed");
        return -1;
    }
    snprintf(abs_, acap, "%s%s%s", st_root(), *rel ? "/" : "", rel);
    return 0;
}

static int fs_refused(client_t *c, const char *rel, int op)
{
    files_use_t u;
    char why[400];
    fs_use(&u);
    if (files_refuse(rel, op, &u, why, sizeof why)) {
        fs_err(c, "403 Forbidden", why);
        return 1;
    }
    return 0;
}

static void fs_changed(const char *rel)   /* (the carts' fs_changed moves: audio/, code/, data/) */
{
    if (!strncmp(rel, "audio", 5) || !strncmp(rel, "code", 4) || !strncmp(rel, "data", 4))
        fsvc_bump_changed();
}

static void api_fs_list(client_t *c, const char *q)
{
    char rel[FILES_PATH_MAX + 1], abs_[ST_PATH + FILES_PATH_MAX], *j, tail[400], ids[3][96];
    files_use_t u;
    int s;
    if (fs_path(c, q, "path", rel, sizeof rel, 0, abs_, sizeof abs_) != 0)
        return;
    if (!(j = files_list_json(st_root(), rel))) {
        fs_err(c, errno == ENOTDIR ? "400 Bad Request" : "404 Not Found", errno == ENOTDIR ? "not a folder" : "no such folder");
        return;
    }
    fs_use(&u);
    for (s = 0; s < 3; s++)
        mj_escape(u.ids[s], ids[s], sizeof ids[s]);
    snprintf(tail, sizeof tail, ",\"free\":%llu,\"max\":%llu,\"busy\":[\"%s\",\"%s\",\"%s\"],\"open_project\":%d}",
             (unsigned long long)disk_free(), FS_UP_MAX, ids[0], ids[1], ids[2], u.open_project);
    {
        const size_t a = strlen(j), b = strlen(tail);
        char *all = malloc(a + b + 1);
        if (!all) {
            free(j);
            http_reply(c, "500 Internal Server Error", "text/plain", "", 0);
            return;
        }
        memcpy(all, j, a), memcpy(all + a, tail, b + 1);
        json_reply(c, "200 OK", all);
        free(all);
    }
    free(j);
}

/* a streamed reply's head: its length known; name the file's (Content-Disposition: attachment, UTF-8) */
static void stream_head(client_t *c, const char *type, uint64_t len, const char *name)
{
    char h[1600], enc[900];
    size_t i, k = 0;
    for (i = 0; name[i] && k + 4 < sizeof enc; i++) {
        const unsigned char x = (unsigned char)name[i];
        if ((x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z') || (x >= '0' && x <= '9') || strchr("-_.", x))
            enc[k++] = (char)x;
        else
            k += (size_t)snprintf(enc + k, sizeof enc - k, "%%%02X", x);
    }
    enc[k] = 0;
    k = (size_t)snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
                         "Content-Disposition: attachment; filename*=UTF-8''%s\r\nCache-Control: no-cache\r\n"
                         "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n", type, (unsigned long long)len, enc);
    put(c, h, k);
}

static void api_fs_get(client_t *c, const char *q)
{
    char rel[FILES_PATH_MAX + 1], abs_[ST_PATH + FILES_PATH_MAX];
    struct stat st;
    int fd;
    if (fs_path(c, q, "path", rel, sizeof rel, 0, abs_, sizeof abs_) != 0)
        return;
    if (!*rel || (fd = open(abs_, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) < 0) {
        fs_err(c, "404 Not Found", "no such file");
        return;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        fs_err(c, "400 Bad Request", "not a file (a folder: /api/fs/zip)");
        return;
    }
    stream_head(c, "application/octet-stream", (uint64_t)st.st_size, strrchr(rel, '/') ? strrchr(rel, '/') + 1 : rel);
    c->dl_fd = fd;
    c->dl_left = (uint64_t)st.st_size;
    c->closing = c->dl_left ? 0 : 1;
}

static void api_fs_zip(client_t *c, const char *q)
{
    char rel[FILES_PATH_MAX + 1], abs_[ST_PATH + FILES_PATH_MAX], name[FILES_SEG_MAX + 8];
    const char *why = NULL, *last;
    uint64_t len = 0;
    if (fs_path(c, q, "path", rel, sizeof rel, 0, abs_, sizeof abs_) != 0)
        return;
    if (!(c->zip = files_zip_plan(st_root(), rel, &len, &why))) {
        fs_err(c, why && !strcmp(why, "no such folder") ? "404 Not Found" : "413 Payload Too Large", why ? why : "no zip");
        return;
    }
    last = strrchr(rel, '/');
    snprintf(name, sizeof name, "%s.zip", *rel ? (last ? last + 1 : rel) : "RemoteFM1");
    stream_head(c, "application/zip", len, name);
    c->closing = 0;
    fprintf(stderr, "web: %s/ as a zip (%llu bytes)\n", *rel ? rel : ".", (unsigned long long)len);
}

/* a streamed download's next piece, while the page's buffer has room: 1 more to come, 0 done */
static int stream_fill(client_t *c)
{
    const size_t room = OUT_CAP / 2;
    if (c->out_n - c->out_off >= room)
        return 1;
    if (c->out_off) {   /* (the buffer from its start again) */
        memmove(c->out, c->out + c->out_off, c->out_n - c->out_off);
        c->out_n -= c->out_off;
        c->out_off = 0;
    }
    while (c->out_n < room) {
        const size_t cap = OUT_CAP - c->out_n;
        long got;
        if (c->dl_fd > 0) {
            const size_t want = c->dl_left < cap ? (size_t)c->dl_left : cap;
            ssize_t r = read(c->dl_fd, c->out + c->out_n, want);
            if (r <= 0) {   /* (shorter than it was: the connection ends early, the browser sees it cut) */
                close(c->dl_fd);
                c->dl_fd = -1;
                c->closing = 2;
                return 0;
            }
            c->out_n += (size_t)r, c->dl_left -= (uint64_t)r;
            if (!c->dl_left) {
                close(c->dl_fd);
                c->dl_fd = -1;
                c->closing = 1;
                return 0;
            }
        } else if (c->zip) {
            got = files_zip_read(c->zip, c->out + c->out_n, cap);
            if (got < 0) {
                files_zip_free(c->zip);
                c->zip = NULL;
                c->closing = 2;
                return 0;
            }
            c->out_n += (size_t)got;
            if (!got) {
                files_zip_free(c->zip);
                c->zip = NULL;
                c->closing = 1;
                return 0;
            }
        } else
            return 0;
    }
    return 1;
}

static int fs_put_start(client_t *c, const char *q, long want)
{
    char dir[FILES_PATH_MAX + 1], name[FILES_SEG_MAX + 1], rel[FILES_PATH_MAX + 1], abs_[ST_PATH + FILES_PATH_MAX], v[8];
    const char *why = NULL;
    struct stat st;
    const size_t ql = strcspn(q, " #");
    char qq[4096];
    snprintf(qq, sizeof qq, "%.*s", (int)ql, q);
    if (fs_path(c, qq, "path", dir, sizeof dir, 0, abs_, sizeof abs_) != 0)
        return 1;
    if (qparam(qq, "name", name, sizeof name) != 0 || strchr(name, '/') ||
        snprintf(rel, sizeof rel, "%s%s%s", dir, *dir ? "/" : "", name) >= (int)sizeof rel || files_rel_ok(rel, 1, &why) != 0) {
        fs_err(c, "400 Bad Request", why ? why : "a file name (name=), no / in it");
        return 1;
    }
    c->up_replace = qparam(qq, "replace", v, sizeof v) == 0 && v[0] == '1';
    if (lstat(abs_, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fs_err(c, "404 Not Found", "no such folder");
        return 1;
    }
    snprintf(abs_, sizeof abs_, "%s/%s", st_root(), rel);
    if (lstat(abs_, &st) == 0) {
        if (!c->up_replace || !S_ISREG(st.st_mode)) {
            fs_err(c, "409 Conflict", S_ISREG(st.st_mode) ? "a file of that name is there already (replace=1 takes its place)"
                                                           : "a folder or a link of that name is there");
            return 1;
        }
        if (fs_refused(c, rel, FILES_REPLACE))
            return 1;
    } else if (fs_refused(c, rel, FILES_PUT))
        return 1;
    if (want < 0 || (want == 0 && !strstr((const char *)c->in, "Content-Length"))) {
        fs_err(c, "411 Length Required", "the file, with its Content-Length");
        return 1;
    }
    if ((uint64_t)want > FS_UP_MAX) {
        fs_err(c, "413 Payload Too Large", "at most 256 MB a file");
        return 1;
    }
    if ((uint64_t)want + FS_RESERVE > disk_free()) {
        fs_err(c, "507 Insufficient Storage", "not enough room on the Move (64 MB stay free)");
        return 1;
    }
    c->up_fs = 1;
    c->up_magic = !strncmp(rel, "audio/", 6);
    snprintf(c->up_dst, sizeof c->up_dst, "%s", abs_);
    snprintf(c->up_tmp, sizeof c->up_tmp, "%s/%s%s.upload-%d.part", st_root(), dir, *dir ? "/" : "", (int)(c - g_c));
    if (strlen(abs_) >= sizeof c->up_dst - 1 || strlen(c->up_tmp) >= sizeof c->up_tmp - 1) {
        fs_err(c, "400 Bad Request", "the path is too long here");
        return 1;
    }
    if (!want) {   /* (an empty file: made now) */
        const int fd = open(c->up_tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0 || close(fd) != 0 || (c->up_magic && (unlink(c->up_tmp), 1))) {
            fs_err(c, c->up_magic ? "415 Unsupported Media Type" : "500 Internal Server Error",
                   c->up_magic ? "audio/ takes WAV and AIFF files only" : "cannot write there");
            return 1;
        }
        if (rename(c->up_tmp, c->up_dst) != 0) {
            unlink(c->up_tmp);
            fs_err(c, "500 Internal Server Error", "cannot keep it");
            return 1;
        }
        fs_changed(rel);
        json_reply(c, "201 Created", "{\"ok\":true}");
        return 1;
    }
    c->up_fd = open(c->up_tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (c->up_fd <= 0) {
        c->up_fd = -1;
        fs_err(c, "500 Internal Server Error", "cannot write there");
        return 1;
    }
    memset(c->up_head, 0, sizeof c->up_head);
    c->up_left = c->up_total = (uint64_t)want;
    return 1;
}

static void api_fs_post(client_t *c, const char *what, const char *q)
{
    char rel[FILES_PATH_MAX + 1], abs_[ST_PATH + FILES_PATH_MAX];
    struct stat st;
    if (!strcmp(what, "mkdir")) {
        if (fs_path(c, q, "path", rel, sizeof rel, 1, abs_, sizeof abs_) != 0 || fs_refused(c, rel, FILES_MKDIR))
            return;
        if (mkdir(abs_, 0755) != 0) {
            fs_err(c, errno == EEXIST ? "409 Conflict" : errno == ENOENT ? "404 Not Found" : "500 Internal Server Error",
                   errno == EEXIST ? "something of that name is there already" : errno == ENOENT ? "no such folder to make it in"
                                   : "cannot make it");
            return;
        }
        fs_changed(rel);
        fprintf(stderr, "web: %s/ made\n", rel);
        json_reply(c, "201 Created", "{\"ok\":true}");
    } else if (!strcmp(what, "rename")) {
        char to[FILES_PATH_MAX + 1], absto[ST_PATH + FILES_PATH_MAX];
        const size_t n = strlen(rel);
        if (fs_path(c, q, "path", rel, sizeof rel, 0, abs_, sizeof abs_) != 0 ||
            fs_path(c, q, "to", to, sizeof to, 1, absto, sizeof absto) != 0 || fs_refused(c, rel, FILES_RENAME))
            return;
        if (lstat(abs_, &st) != 0) {
            fs_err(c, "404 Not Found", "no such file or folder");
            return;
        }
        if (fs_refused(c, to, S_ISDIR(st.st_mode) ? FILES_MKDIR : FILES_PUT))
            return;
        (void)n;
        if (!strncmp(to, rel, strlen(rel)) && to[strlen(rel)] == '/') {
            fs_err(c, "400 Bad Request", "a folder cannot go inside itself");
            return;
        }
        if (lstat(absto, &st) == 0) {
            fs_err(c, "409 Conflict", "something of that name is there already");
            return;
        }
        if (rename(abs_, absto) != 0) {
            fs_err(c, errno == ENOENT ? "404 Not Found" : "500 Internal Server Error",
                   errno == ENOENT ? "no such folder to move it into" : "cannot rename it");
            return;
        }
        fs_changed(rel), fs_changed(to);
        fprintf(stderr, "web: %s -> %s\n", rel, to);
        json_reply(c, "200 OK", "{\"ok\":true}");
    } else if (!strcmp(what, "delete")) {
        char v[8], conf[FILES_SEG_MAX + 1];
        const char *last;
        int rec;
        if (fs_path(c, q, "path", rel, sizeof rel, 0, abs_, sizeof abs_) != 0 || fs_refused(c, rel, FILES_DELETE))
            return;
        if (lstat(abs_, &st) != 0) {
            fs_err(c, "404 Not Found", "no such file or folder");
            return;
        }
        rec = qparam(q, "recursive", v, sizeof v) == 0 && v[0] == '1';
        last = strrchr(rel, '/');
        last = last ? last + 1 : rel;
        if (S_ISDIR(st.st_mode) && rmdir(abs_) != 0) {
            if (errno != ENOTEMPTY && errno != EEXIST) {
                fs_err(c, "500 Internal Server Error", "cannot delete it");
                return;
            }
            if (!rec || qparam(q, "confirm", conf, sizeof conf) != 0 || strcmp(conf, last)) {
                fs_err(c, "409 Conflict", "the folder is not empty: recursive=1 and confirm= its name delete it with everything in it");
                return;
            }
            if (files_rmtree(abs_) != 0) {
                fs_err(c, "500 Internal Server Error", "not all of it could be deleted");
                return;
            }
        } else if (!S_ISDIR(st.st_mode) && unlink(abs_) != 0) {
            fs_err(c, "500 Internal Server Error", "cannot delete it");
            return;
        }
        fs_changed(rel);
        fprintf(stderr, "web: %s deleted%s\n", rel, rec ? " (with what was in it)" : "");
        json_reply(c, "200 OK", "{\"ok\":true}");
    } else
        http_reply(c, "404 Not Found", "text/plain", "not found\n", 10);
}

/* ------------------------------------------------------------------ States and projects (/api/store) --- */
/* The page's States (each slot's firmware's 32) and the sequencer's 32 projects, done by the engine as the Move's pads
 * do them (engine.h engine_store; README "The page's States and projects"):
 *   GET  /api/store                          engine_store_json: what is stored, the active state, the open project
 *   POST /api/store {"op", "slot", "k", "to", "id", "replace"}   op: state-save, state-load, state-delete (slot, k,
 *                                            id: the firmware the page means), project-open, project-save,
 *                                            project-dup (k into the empty to), project-delete (k). Answered when
 *                                            done: 200 {"ok":true,"message"}, or 409 {"ok":false,"error"}
 *   GET  /api/store/state?slot=S&k=K         state K of slot S's firmware as a file: a .zip (store only, as /api/fs/zip
 *                                            makes) of state-KK.bin, .json (id, version, sha256, the image's hash,
 *                                            the .mem's) and .mem (its working memory, when it has one)
 *   GET  /api/store/project?k=K              project K as a file: a .zip of move-project-KK.rfmmove, .fx.json and
 *                                            its firmware copies .fwN.bin / .json / .mem
 *   POST /api/store/put?kind=state&slot=S&k=K[&replace=1][&load=1]   a state file as state K: it must be the slot's
 *                                            firmware's (its .json's id); over the active state only with load=1
 *   POST /api/store/put?kind=project&k=K[&replace=1]   a project file (or a lone .rfmmove: RemoteFM1's web player's
 *                                            Backup) as project K; never over the open project
 * A file is checked whole first (its names, its hashes, its firmware), unpacked into a hidden folder of the root
 * (.store-up-N/), then put in place by the storage thread with renames: a state or project there is the old one or the
 * new one, never half of each. The file browser's own rules (files.h) stay as they are: it never writes these. */
#define STORE_UP_MAX (256ull * 1024 * 1024)
static int g_store_up;   /* (the hidden folders' numbers) */

static int store_slot_id(int s, char *id, size_t cap)   /* the firmware in slot s: 0, -1 none */
{
    char ids[3][32];
    if (s < 0 || s > 2)
        return -1;
    engine_running_ids(ids);
    if (!ids[s][0])
        return -1;
    snprintf(id, cap, "%s", ids[s]);
    return 0;
}
/* a firmware's name (the installed ones'), else its id */
static void fw_name(const char *id, char *out, size_t cap)
{
    char *j = engine_firmwares();
    const char *it = j ? mj_find(j, "firmwares") : NULL, *e;
    char x[64];
    snprintf(out, cap, "%s", id);
    if (it && *it == '[')
        while ((e = mj_elem(&it)))
            if (!mj_string(e, "id", x, sizeof x) && !strcmp(x, id)) {
                mj_string(e, "name", out, cap);
                break;
            }
    free(j);
}
/* a request answered once the engine has done it (web_main: engine_store_done) */
static void store_wait(client_t *c, const eng_store_t *r, const char *dir)
{
    const int id = engine_store(r);
    if (id <= 0) {
        if (dir)
            files_rmtree(dir);
        fs_err(c, "503 Service Unavailable", "the Move is busy: try again");
        return;
    }
    c->sreq = id;
    c->sreq_ms = now_ms();
    snprintf(c->sdir, sizeof c->sdir, "%s", dir ? dir : "");
    c->closing = 0;
}

static void api_store(client_t *c, const char *body)
{
    static const char *const OPS[] = { "state-save", "state-load", "state-delete", "", "project-open", "project-save",
                                       "project-dup", "project-delete" };
    char op[32] = "";
    long slot = 0, k = -1, to = -1, rep = 0;
    eng_store_t r;
    int i;
    memset(&r, 0, sizeof r);
    if (!body || mj_string(body, "op", op, sizeof op) || mj_int(body, "k", &k)) {
        fs_err(c, "400 Bad Request", "want {op, k}");
        return;
    }
    for (i = 0; i < 8 && strcmp(OPS[i], op); i++) {}
    if (i == 8 || !op[0]) {
        fs_err(c, "400 Bad Request", "no such op");
        return;
    }
    mj_int(body, "slot", &slot);
    mj_int(body, "to", &to);
    mj_int(body, "replace", &rep);
    r.op = ENG_ST_SAVE + i, r.slot = (int)slot, r.k = (int)k, r.to = (int)to, r.flags = rep ? ENG_STF_REPLACE : 0;
    if (r.op <= ENG_ST_DELETE && mj_string(body, "id", r.id, sizeof r.id) && store_slot_id(r.slot, r.id, sizeof r.id)) {
        fs_err(c, "409 Conflict", "that slot is empty");
        return;
    }
    store_wait(c, &r, NULL);
}

static void api_store_zip(client_t *c, const char *what, const char *q)
{
    char v[16], id[32], rel[ST_PATH], pre[40], top[160], ver[40] = "", p[ST_PATH + 64], name[200];
    const char *why = NULL;
    uint64_t len = 0;
    const int state = !strcmp(what, "state");
    long k = qparam(q, "k", v, sizeof v) == 0 ? atol(v) : -1, s = qparam(q, "slot", v, sizeof v) == 0 ? atol(v) : 0;
    if (k < 0 || k > 31 || (state && store_slot_id((int)s, id, sizeof id) != 0)) {
        fs_err(c, "404 Not Found", state ? "no such state (an empty slot?)" : "no such project");
        return;
    }
    if (state) {
        snprintf(rel, sizeof rel, "states/%s", id);
        snprintf(pre, sizeof pre, "state-%02ld.", k);
        snprintf(p, sizeof p, "%s/%s/state-%02ld.json", st_root(), rel, k);
        pm_json_str(p, "version", ver, sizeof ver);
        snprintf(top, sizeof top, "%s%s%s-state-%02ld", id, ver[0] ? "-" : "", ver, k + 1);
        snprintf(p, sizeof p, "%s/%s/state-%02ld.bin", st_root(), rel, k);
    } else {
        snprintf(rel, sizeof rel, "move/projects");
        snprintf(pre, sizeof pre, "move-project-%02ld.", k + 1);
        snprintf(top, sizeof top, "move-project-%02ld", k + 1);
        snprintf(p, sizeof p, "%s/%s/move-project-%02ld.rfmmove", st_root(), rel, k + 1);
    }
    if (access(p, F_OK) != 0) {
        fs_err(c, "404 Not Found", state ? "that state is empty" : "that project is empty");
        return;
    }
    if (!(c->zip = files_zip_plan_sel(st_root(), rel, pre, top, &len, &why))) {
        fs_err(c, "500 Internal Server Error", why ? why : "no zip");
        return;
    }
    snprintf(name, sizeof name, "%s.zip", top);
    stream_head(c, "application/zip", len, name);
    c->closing = 0;
    fprintf(stderr, "web: %s as a file (%llu bytes)\n", top, (unsigned long long)len);
}

static int store_put_start(client_t *c, const char *q, long want)
{
    char v[16], kind[16] = "";
    const size_t ql = strcspn(q, " #");
    char qq[1024];
    eng_store_t *r = &c->up_sr;
    snprintf(qq, sizeof qq, "%.*s", (int)(ql < sizeof qq ? ql : sizeof qq - 1), q);
    memset(r, 0, sizeof *r);
    qparam(qq, "kind", kind, sizeof kind);
    r->k = qparam(qq, "k", v, sizeof v) == 0 ? atoi(v) : -1;
    r->slot = qparam(qq, "slot", v, sizeof v) == 0 ? atoi(v) : 0;
    r->flags = (qparam(qq, "replace", v, sizeof v) == 0 && v[0] == '1' ? ENG_STF_REPLACE : 0) |
               (qparam(qq, "load", v, sizeof v) == 0 && v[0] == '1' ? ENG_STF_LOAD : 0);
    if (strcmp(kind, "state") && strcmp(kind, "project")) {
        fs_err(c, "400 Bad Request", "kind=state or kind=project");
        return 1;
    }
    r->op = kind[0] == 's' ? ENG_ST_IMPORT : ENG_PJ_IMPORT;
    if (r->k < 0 || r->k > 31) {
        fs_err(c, "400 Bad Request", "k: 0..31");
        return 1;
    }
    if (r->op == ENG_ST_IMPORT && store_slot_id(r->slot, r->id, sizeof r->id) != 0) {
        fs_err(c, "409 Conflict", "that slot is empty: a state goes to the firmware it was saved from");
        return 1;
    }
    if (want <= 0) {
        fs_err(c, "411 Length Required", "the file, with its Content-Length");
        return 1;
    }
    if ((uint64_t)want > STORE_UP_MAX) {
        fs_err(c, "413 Payload Too Large", "at most 256 MB");
        return 1;
    }
    if ((uint64_t)want + FS_RESERVE > disk_free()) {
        fs_err(c, "507 Insufficient Storage", "not enough room on the Move (64 MB stay free)");
        return 1;
    }
    snprintf(c->up_tmp, sizeof c->up_tmp, "%s/.store-up-%d.part", st_root(), ++g_store_up);
    c->up_fd = open(c->up_tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (c->up_fd <= 0) {
        c->up_fd = -1;
        fs_err(c, "500 Internal Server Error", "cannot write on the Move");
        return 1;
    }
    c->up_store = 1, c->up_fs = 1, c->up_magic = 0;
    memset(c->up_head, 0, sizeof c->up_head);
    c->up_left = c->up_total = (uint64_t)want;
    return 1;
}

/* the names in dir: 0 each one passes ok(name), else -1 with the first that does not in *bad */
typedef int (*name_ok_t)(const char *name, const char *stem);
static int dir_names(const char *dir, name_ok_t ok, const char *stem, char *bad, size_t cap)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int rc = 0;
    if (!d)
        return -1;
    while ((e = readdir(d)) && !rc)
        if (e->d_name[0] != '.' && !ok(e->d_name, stem))
            snprintf(bad, cap, "%s", e->d_name), rc = -1;
    closedir(d);
    return rc;
}
static int is_state_name(const char *n, const char *stem)   /* stem "state-NN" */
{
    const size_t l = strlen(stem);
    return !strncmp(n, stem, l) && (!strcmp(n + l, ".bin") || !strcmp(n + l, ".json") || !strcmp(n + l, ".mem"));
}
static int is_proj_name(const char *n, const char *stem)    /* stem "move-project-NN" (or any .rfmmove's) */
{
    const size_t l = strlen(stem);
    const char *t = n + l;
    if (strncmp(n, stem, l))
        return 0;
    return !strcmp(t, ".rfmmove") || !strcmp(t, ".fx.json") ||
           (!strncmp(t, ".fw", 3) && t[3] >= '1' && t[3] <= '3' && (!strcmp(t + 4, ".bin") || !strcmp(t + 4, ".json") ||
                                                                     !strcmp(t + 4, ".mem")));
}
/* the one name in dir ending in suffix (its stem into stem): 0, -1 none or more than one */
static int one_stem(const char *dir, const char *suffix, char *stem, size_t cap)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    const size_t sl = strlen(suffix);
    if (!d)
        return -1;
    while ((e = readdir(d))) {
        const size_t l = strlen(e->d_name);
        if (e->d_name[0] != '.' && l > sl && !strcmp(e->d_name + l - sl, suffix)) {
            if (n++ == 0)
                snprintf(stem, cap, "%.*s", (int)(l - sl), e->d_name);
        }
    }
    closedir(d);
    return n == 1 ? 0 : -1;
}
/* an image's and its .mem's hashes as its .json names them: NULL right (or not named), else why */
static const char *hashes_ok(const char *bin, const char *json)
{
    char want[80] = "", got[65], mp[ST_PATH + 80];
    size_t n = 0;
    uint8_t *b;
    if (pm_json_str(json, "hash", want, sizeof want) == 0 && want[0]) {
        if (!(b = st_read(bin, &n)))
            return "its flash image could not be read";
        pm_hash_hex(b, (uint32_t)n, got);
        free(b);
        if (strcmp(want, got))
            return "its flash image is damaged (its checksum does not match its .json)";
    }
    ms_path(mp, sizeof mp, bin);
    if (pm_json_str(json, "mem", want, sizeof want) == 0 && want[0] && (b = st_read(mp, &n))) {
        pm_hash_hex(b, (uint32_t)n, got);
        free(b);
        if (strcmp(want, got))
            return "its working memory is damaged (its checksum does not match its .json)";
    }
    return NULL;
}
static int ren(const char *dir, const char *from, const char *to)
{
    char a[ST_PATH + 300], b[ST_PATH + 300];
    snprintf(a, sizeof a, "%s/%s", dir, from);
    snprintf(b, sizeof b, "%s/%s", dir, to);
    return access(a, F_OK) != 0 ? 0 : rename(a, b);
}
/* a state file: one state-NN.bin with its .json (and .mem), the slot's firmware's. NULL ok (named s.*), else why */
static const char *check_state(const char *dir, const eng_store_t *r, char *why, size_t cap)
{
    char stem[300], bad[300] = "", bin[ST_PATH + 300], js[ST_PATH + 300], id[64] = "", ver[40] = "", a[320], b[64];
    const char *w;
    if (one_stem(dir, ".rfmmove", stem, sizeof stem) == 0)
        return "this is a project file: upload it under Projects";
    if (one_stem(dir, ".bin", stem, sizeof stem) != 0 || strncmp(stem, "state-", 6))
        return "not a state file: it holds no state-NN.bin, or more than one";
    if (dir_names(dir, is_state_name, stem, bad, sizeof bad) != 0) {
        snprintf(why, cap, "not a state file: it holds %s", bad);
        return why;
    }
    snprintf(bin, sizeof bin, "%s/%s.bin", dir, stem);
    snprintf(js, sizeof js, "%s/%s.json", dir, stem);
    if (pm_json_id(js, id, sizeof id) != 0)
        return "not a state file: its .json names no firmware";
    pm_json_str(js, "version", ver, sizeof ver);
    if (strcmp(id, r->id)) {
        fw_name(id, a, sizeof a);
        fw_name(r->id, b, sizeof b);
        snprintf(why, cap, "this is a state of %s%s%s, and slot %d runs %s: a state loads only into its own firmware", a,
                 ver[0] ? " " : "", ver, r->slot + 1, b);
        return why;
    }
    if ((w = hashes_ok(bin, js)))
        return w;
    snprintf(a, sizeof a, "%s.mem", stem);
    if (ren(dir, a, "s.mem"))
        return "the file could not be unpacked";
    snprintf(a, sizeof a, "%s.json", stem);
    if (ren(dir, a, "s.json"))
        return "the file could not be unpacked";
    snprintf(a, sizeof a, "%s.bin", stem);
    return ren(dir, a, "s.bin") ? "the file could not be unpacked" : NULL;
}
/* a project file: one .rfmmove, and its .fx.json and .fwN.* beside it. NULL ok (named p.*), else why */
static const char *check_project(const char *dir, char *why, size_t cap)
{
    static const char *const SUF[] = { ".rfmmove", ".fx.json", ".fw1.bin", ".fw1.json", ".fw1.mem", ".fw2.bin", ".fw2.json",
                                       ".fw2.mem", ".fw3.bin", ".fw3.json", ".fw3.mem" };
    char stem[300], bad[300] = "", a[ST_PATH + 300], b[ST_PATH + 300];
    const char *w;
    int k;
    if (one_stem(dir, ".rfmmove", stem, sizeof stem) != 0)
        return one_stem(dir, ".bin", stem, sizeof stem) == 0 && !strncmp(stem, "state-", 6)
                   ? "this is a state file: upload it under States, into a slot running its firmware"
                   : "not a project file: it holds no .rfmmove project, or more than one";
    if (dir_names(dir, is_proj_name, stem, bad, sizeof bad) != 0) {
        snprintf(why, cap, "not a project file: it holds %s", bad);
        return why;
    }
    for (k = 1; k <= 3; k++) {
        snprintf(a, sizeof a, "%s/%s.fw%d.bin", dir, stem, k);
        snprintf(b, sizeof b, "%s/%s.fw%d.json", dir, stem, k);
        if (access(a, F_OK) == 0 && access(b, F_OK) != 0) {
            snprintf(why, cap, "its firmware %d's copy has no .json naming the firmware", k);
            return why;
        }
        if (access(a, F_OK) == 0 && (w = hashes_ok(a, b))) {
            snprintf(why, cap, "firmware %d: %s", k, w);
            return why;
        }
    }
    for (k = 0; k < (int)(sizeof SUF / sizeof SUF[0]); k++) {
        snprintf(a, sizeof a, "%s%s", stem, SUF[k]);
        snprintf(b, sizeof b, "p%s", SUF[k]);
        if (ren(dir, a, b))
            return "the file could not be unpacked";
    }
    return NULL;
}

static void store_put_done(client_t *c)
{
    char dir[ST_PATH + 64], whyb[400];
    const char *why = NULL;
    uint8_t head[4] = { 0 };
    int fd;
    c->up_store = 0;
    snprintf(dir, sizeof dir, "%s/.store-up-%d", st_root(), g_store_up);
    files_rmtree(dir);
    if (mkdir(dir, 0755) != 0)
        why = "cannot unpack it on the Move";
    else if ((fd = open(c->up_tmp, O_RDONLY | O_CLOEXEC)) < 0 || read(fd, head, 4) != 4 || close(fd) != 0)
        why = "the file could not be read";
    else if (!memcmp(head, "PK\3\4", 4)) {
        if (files_unzip(c->up_tmp, dir, 16, STORE_UP_MAX, &why) < 0 && !why)
            why = "not a .zip file";
    } else if (!memcmp(head, "RMVP", 4) && c->up_sr.op == ENG_PJ_IMPORT) {   /* (a lone project: the web player's) */
        char p[ST_PATH + 80];
        snprintf(p, sizeof p, "%s/move-project.rfmmove", dir);
        if (rename(c->up_tmp, p) != 0)
            why = "the file could not be read";
    } else
        why = c->up_sr.op == ENG_ST_IMPORT ? "not a state file (a .zip the page downloaded from a Move)"
                                           : "not a project file (a .zip the page downloaded from a Move, or a .rfmmove)";
    unlink(c->up_tmp);
    if (!why)
        why = c->up_sr.op == ENG_ST_IMPORT ? check_state(dir, &c->up_sr, whyb, sizeof whyb) : check_project(dir, whyb, sizeof whyb);
    if (why) {
        char m[400];
        files_rmtree(dir);
        snprintf(m, sizeof m, "%s", why);
        m[0] = (char)(m[0] >= 'a' && m[0] <= 'z' ? m[0] - 32 : m[0]);
        fprintf(stderr, "web: a %s file refused: %s\n", c->up_sr.op == ENG_ST_IMPORT ? "state" : "project", m);
        fs_err(c, "422 Unprocessable Entity", m);
        return;
    }
    snprintf(c->up_sr.dir, sizeof c->up_sr.dir, "%s", dir);
    store_wait(c, &c->up_sr, dir);
}

/* a request under way: answered once the engine has done it (2 minutes at most) */
static void store_poll(client_t *c, uint64_t ms)
{
    char msg[220], esc[460], b[600];
    const int st = engine_store_done(c->sreq, msg, sizeof msg);
    if (st == 0 && ms - c->sreq_ms < 120000u) {
        c->born = ms;   /* (not a request that never came whole) */
        return;
    }
    if (st != 0 && c->sdir[0])   /* (the file's folder: what the storage thread left of it) */
        files_rmtree(c->sdir);
    c->sdir[0] = 0;
    c->sreq = 0;
    mj_escape(st == 0 ? "the Move did not answer in time" : st == -2 ? "the Move lost the request" : msg, esc, sizeof esc);
    if (st == 1) {
        snprintf(b, sizeof b, "{\"ok\":true,\"message\":\"%s\"}", esc);
        json_reply(c, "200 OK", b);
    } else {
        snprintf(b, sizeof b, "{\"ok\":false,\"error\":\"%s\"}", esc);
        json_reply(c, st == 0 ? "504 Gateway Timeout" : "409 Conflict", b);
    }
}

static void request(client_t *c, const char *body)
{
    char path[4096] = "", key[64], query[4096] = "";
    const char *req = (const char *)c->in;
    const int post = !strncmp(req, "POST ", 5);
    {   /* (the query: the file browser's parameters) */
        char first[4096] = "";
        const char *qm;
        if (sscanf(req, "%*s %4095s", first) == 1 && (qm = strchr(first, '?')))
            snprintf(query, sizeof query, "%.*s", (int)strcspn(qm + 1, "#"), qm + 1);
    }
    if (sscanf(req, "DELETE %4095s", path) == 1) {
        path[strcspn(path, "?#")] = 0;
        if (!strncmp(path, "/api/files/audio/", 17))
            api_audio_delete(c, path + 17);
        else
            http_reply(c, "405 Method Not Allowed", "text/plain", "", 0);
        return;
    }
    if (!post && sscanf(req, "GET %4095s", path) != 1) {
        http_reply(c, "405 Method Not Allowed", "text/plain", "", 0);
        return;
    }
    if (post && sscanf(req, "POST %4095s", path) != 1)
        path[0] = 0;
    path[strcspn(path, "?#")] = 0;
    if (post) {
        if (!strcmp(path, "/api/load"))
            api_load(c, body);
        else if (!strcmp(path, "/api/store"))
            api_store(c, body);
        else if (!strcmp(path, "/api/edit"))
            api_edit(c, body, 0);
        else if (!strcmp(path, "/api/download"))
            api_download(c, body);
        else if (!strcmp(path, "/api/test/midi"))
            api_edit(c, body, 1);
        else if (!strcmp(path, "/api/test/midiout")) {   /* (--test-api) {"hex": whole messages}: Out from the block */
            char hx[16384];
            uint8_t m[sizeof hx / 2];
            size_t k = 0;
            unsigned v;
            if (!body || mj_string(body, "hex", hx, sizeof hx)) {
                json_reply(c, "400 Bad Request", "{\"ok\":false}");
                return;
            }
            while (hx[2 * k] && hx[2 * k + 1] && sscanf(hx + 2 * k, "%2x", &v) == 1)
                m[k++] = (uint8_t)v;
            if (!k || engine_test_midiout(m, (int)k) != 0)
                json_reply(c, "403 Forbidden", "{\"ok\":false}");
            else
                json_reply(c, "202 Accepted", "{\"ok\":true}");
        }
        else if (!strcmp(path, "/api/test/poke")) {   /* (--test-api) {"slot", "off", "val"}: a byte of its memory */
            long sl = -1, off = -1, val = -1;
            if (!body || mj_int(body, "slot", &sl) || mj_int(body, "off", &off) || mj_int(body, "val", &val) || off < 0 ||
                val < 0 || val > 255)
                json_reply(c, "400 Bad Request", "{\"ok\":false}");
            else if (engine_test_poke((int)sl, (uint32_t)off, (int)val) != 0)
                json_reply(c, "403 Forbidden", "{\"ok\":false}");
            else
                json_reply(c, "202 Accepted", "{\"ok\":true}");
        }
        else if (!strcmp(path, "/api/test/peek")) {   /* (--test-api) {"slot", "off", "n"} or {"slot", "lcd": 1}: hex / sum */
            long sl = -1, off = 0, n = 0, lcd = 0;
            uint8_t b[64];
            uint32_t sum = 0;
            char out[200];
            int k, o;
            if (!body || mj_int(body, "slot", &sl) || ((mj_int(body, "lcd", &lcd) || !lcd) &&
                                                       (mj_int(body, "off", &off) || mj_int(body, "n", &n) || off < 0 ||
                                                        n < 1 || n > 64)))
                json_reply(c, "400 Bad Request", "{\"ok\":false}");
            else if (engine_test_peek((int)sl, lcd ? 1 : 0, (uint32_t)off, (uint32_t)n, b, &sum) != 0)
                json_reply(c, "403 Forbidden", "{\"ok\":false}");
            else {
                if (lcd)
                    snprintf(out, sizeof out, "{\"ok\":true,\"sum\":\"%08x\"}", sum);
                else {
                    o = snprintf(out, sizeof out, "{\"ok\":true,\"hex\":\"");
                    for (k = 0; k < n; k++)
                        o += snprintf(out + o, sizeof out - (size_t)o, "%02x", b[k]);
                    snprintf(out + o, sizeof out - (size_t)o, "\"}");
                }
                json_reply(c, "200 OK", out);
            }
        }
        else if (!strncmp(path, "/api/fs/", 8))
            api_fs_post(c, path + 8, query);
        else if (!strncmp(path, "/api/fm1/wifi/", 14))
            api_fm1_wifi(c, path + 14, body);
#ifdef RFM14_PLAT_SIM
        else if (!strcmp(path, "/api/test/fm1")) {   /* the simulator's fake FM-1 (fm1sim.c, --fake-fm1) */
            char op[16];
            long a = 0, b = 0;
            if (!body || mj_string(body, "op", op, sizeof op)) {
                json_reply(c, "400 Bad Request", "{\"ok\":false}");
                return;
            }
            mj_int(body, "a", &a);
            mj_int(body, "b", &b);
            if (fm1sim_cmd(op, a, b) != 0)
                json_reply(c, "403 Forbidden", "{\"ok\":false}");
            else
                json_reply(c, "202 Accepted", "{\"ok\":true}");
        }
#endif
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
        c->mo_pos = engine_midi_out_pos();
        c->full = 1;
        c->lcd = calloc(1, ENG_LCD_BYTES);
        return;
    }
    if (!strcmp(path, "/api/status")) {
        char s[8192];
        const int n = engine_status(s, sizeof s);
        http_reply(c, "200 OK", "application/json", s, (size_t)(n > 0 ? n : 0));
    }
#ifdef RFM14_PLAT_SIM
    else if (!strcmp(path, "/api/test/fm1")) {
        char s[2048];
        const int n = fm1sim_json(s, sizeof s);
        http_reply(c, "200 OK", "application/json", s, (size_t)(n > 0 ? n : 0));
    }
#endif
    else if (!strcmp(path, "/api/fm1/wifi")) {
        static char s[12288];   /* (the web thread's) */
        const int n = fm1link_wifi_json(s, sizeof s);
        http_reply(c, "200 OK", "application/json", s, (size_t)(n > 0 ? n : 0));
    }
    else if (!strcmp(path, "/api/info"))
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
    } else if (!strcmp(path, "/api/catalog")) {
        char *j = engine_catalog();
        json_reply(c, "200 OK", j ? j : "{\"firmwares\":[],\"favorites\":[]}");
        free(j);
    } else if (!strncmp(path, "/api/fw/", 8))
        api_fw_file(c, path + 8);
    else if (!strcmp(path, "/api/files/audio") || !strcmp(path, "/api/files/audio/"))
        api_audio_list(c);
    else if (!strcmp(path, "/api/fs/list"))
        api_fs_list(c, query);
    else if (!strcmp(path, "/api/fs/get"))
        api_fs_get(c, query);
    else if (!strcmp(path, "/api/fs/zip"))
        api_fs_zip(c, query);
    else if (!strcmp(path, "/api/store")) {
        char *j = engine_store_json();
        json_reply(c, "200 OK", j ? j : "{\"ver\":0,\"open\":-1,\"pending\":-1,\"loading\":0,\"used\":0,\"dirty\":0,"
                                        "\"projects\":[],\"slots\":[null,null,null]}");
        free(j);
    } else if (!strcmp(path, "/api/store/state") || !strcmp(path, "/api/store/project"))
        api_store_zip(c, path + 11, query);
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

/* the page's MIDI bytes (any chunking) -> whole messages for the active firmware (engine_midi): a SysEx F0..F7 as one
 * (64 KB at most), channel and system messages with their data bytes (running status kept), real time on its own */
static void midi_bytes(client_t *c, const uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        const uint8_t x = b[i];
        if (x >= 0xF8) {
            engine_midi(&x, 1);
            continue;
        }
        if (x == 0xF0) {
            if (!c->mx && !(c->mx = malloc(65536)))
                continue;
            c->mx[0] = x, c->mx_n = 1, c->mx_sysex = 1, c->mx_st = 0;
            continue;
        }
        if (c->mx_sysex) {
            if (x == 0xF7) {
                c->mx[c->mx_n++] = x;
                engine_midi(c->mx, (int)c->mx_n);
                c->mx_sysex = 0;
                continue;
            }
            if (x < 0x80) {
                if (c->mx_n < 65534)
                    c->mx[c->mx_n++] = x;
                else
                    c->mx_sysex = 0;   /* (too long: dropped) */
                continue;
            }
            c->mx_sysex = 0;           /* (a status byte cut it off: dropped) */
        }
        if (x >= 0x80) {
            c->mx_st = x, c->mx_nd = 0;
            if (x >= 0xF4) {           /* (F4, F5 undefined; F6 tune request alone; F7 stray) */
                if (x == 0xF6)
                    engine_midi(&x, 1);
                c->mx_st = 0;
            }
            continue;
        }
        if (!c->mx_st)
            continue;
        c->mx_d[c->mx_nd++] = x;
        {
            const uint8_t st = c->mx_st;
            const int need = ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0 || st == 0xF1 || st == 0xF3) ? 1 : 2;
            if (c->mx_nd >= need) {
                const uint8_t m[3] = { st, c->mx_d[0], c->mx_d[1] };
                engine_midi(m, 1 + need);
                c->mx_nd = 0;
                if (st >= 0xF0)
                    c->mx_st = 0;
            }
        }
    }
}

/* whole frames from a page: 4-byte panel messages, or (a frame that starts with 32) MIDI bytes; close; ping */
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
            else if (op == 0x2 && plen >= 1 && d[0] == 32)   /* MIDI for the active firmware (its editors' SysEx) */
                midi_bytes(c, d + 1, plen - 1);
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
        if (c->up_fd > 0) {   /* an upload's body: straight to its file (the 10 s limit counts from its last bytes) */
            c->born = now_ms();
            upload_feed(c, c->in, (size_t)r);
            if (c->up_fd <= 0)
                return;
            continue;
        }
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
                if (c->up_fd == 0 && upload_start(c, (const char *)c->in, want)) {
                    const size_t have = c->in_n - head;
                    c->in_n = 0;
                    if (c->up_fd > 0) {
                        if (have)
                            upload_feed(c, c->in + head, have);
                        if (c->up_fd > 0)
                            continue;
                    }
                    return;
                }
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
        if (w > 0 && !c->ws)
            c->born = now_ms();   /* (a reply under way: its time counts from its last bytes out) */
    }
    c->out_off = c->out_n = 0;
}

/* what changed since this page's last look */
static void push(client_t *c)
{
    {   /* the active firmware's MIDI out (SysEx and channel messages): binary 5 + its bytes, in pieces */
        uint8_t m[1 + 8192];
        int k;
        while (c->out_n - c->out_off < LAG && (k = engine_midi_out(m + 1, (int)sizeof m - 1, &c->mo_pos)) > 0) {
            m[0] = 5;
            ws_send(c, 0x2, NULL, 0, m, (size_t)k + 1);
        }
    }
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
        uint64_t ms;
        pf[np].fd = g_listen, pf[np].events = POLLIN, pc[np++] = NULL;
        for (i = 0; i < MAXC; i++)
            if (g_c[i].fd >= 0) {
                pf[np].fd = g_c[i].fd;
                pf[np].events = (short)(POLLIN | (g_c[i].out_n > g_c[i].out_off ? POLLOUT : 0));
                pc[np++] = &g_c[i];
            }
        poll(pf, (nfds_t)np, 20);
        ms = now_ms();
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
        ms = now_ms();   /* (cread and cwrite stamp born with now_ms(): ages from a time taken before them underflowed) */
        for (i = 0; i < MAXC; i++) {
            client_t *c = &g_c[i];
            if (c->fd < 0)
                continue;
            if (c->ws && !c->closing)
                push(c);
            else if (c->sreq > 0)   /* a States or projects request: answered once the engine has done it */
                store_poll(c, ms);
            else if (c->dl_fd > 0 || c->zip) {   /* a download under way: more while the page takes it */
                stream_fill(c);
                if (ms > c->born && ms - c->born > 30000u)
                    c->closing = 2;   /* (a reader gone quiet for 30 s) */
            } else if (!c->ws && ms > c->born && ms - c->born > 10000u && c->out_n <= c->out_off)
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
    {   /* (a state or project file a page sent before a stop, never put in place: gone) */
        DIR *d = opendir(st_root());
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (!strncmp(e->d_name, ".store-up-", 10)) {
                char p[ST_PATH + 300];
                snprintf(p, sizeof p, "%s/%s", st_root(), e->d_name);
                files_rmtree(p);
            }
        if (d)
            closedir(d);
    }
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
