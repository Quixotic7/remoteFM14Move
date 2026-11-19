/* SPDX-License-Identifier: MPL-2.0 */
/* The FM-1 on the Move's USB-A port or over Wi-Fi: see fm1link.h. */
#define _GNU_SOURCE
#include "fm1link.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "minijson.h"
#include "mr_core.h"
#include "rfm/session.h"

#define FRAME_MS 50u                 /* the active slot's screen: at most this often to the session */

static int g_on;
static rfm_session_t *S;
static int g_link = -1;
static uint32_t g_now;
static int g_changed, g_linked;
/* what the FM-1 holds on the slot it plays (let go there on a slot switch or a loss) */
static uint32_t g_keys, g_btns;
static int g_slot = -1;
static fw_t *g_fw;
static uint32_t g_writes, g_last_frame;
static int g_usba_screen;   /* --fm1-usba-screen: the firmware's screen over USB-A too */
static int g_dark;                   /* a dark panel was given (no firmware) */
static uint16_t g_lcd[240 * 240];    /* the screen as the session wants it (emu_hal_t.lcd's layout: BE bytes) */
static uint8_t g_levels[FW_NLEVELS + 1];
static int g_levels_ok;
/* the counters: wire bytes out, frames acknowledged, a second at a time */
static uint32_t g_win_ms, g_win_wire, g_win_acks, g_fps, g_bps, g_send_full;
static char g_state_why[64];
static int g_usb_link = -1;

/* ------------------------------------------------------------------ Wi-Fi (TCP) state --- */
#define W_PROBE_MS 2000u             /* "RFM1?" this often while no FM-1 is connected */
#define W_CONNECT_MS 3000u           /* a TCP connect that takes longer is given up */
#define W_SESSION_MS 12000u          /* connected, and never a session on it this long: dropped (not a RemoteFM1?) */
#define W_SILENT_MS 12000u           /* .. or nothing at all from it this long (TCP's own stalls are shorter: a
                                      * session that restarts on a live connection keeps it, and the FM-1 its screen) */
#define W_BAD_MS 30000u              /* an address that failed is not chosen again for this long (unless asked for) */
#define W_BAD_KEPT_MS 4000u          /* .. the one remembered: sooner (it is the FM-1 coming back) */
#define W_OUT_CAP (512u * 1024u)     /* bytes TCP has not taken yet (2 frames of 15 KB in flight: never near it) */
enum { W_OFF, W_SEARCH, W_CONNECTING, W_CONNECTED };
typedef struct {
    struct sockaddr_in a;
    char name[24];
    uint32_t t_seen, t_bad;
    int busy, used;
} wcand_t;
static int w_on, w_state = W_OFF, w_udp = -1, w_tcp = -1, w_link = -1, w_failed;
static uint16_t w_udp_port = FM1LINK_UDP_PORT;
static uint32_t w_t_probe, w_t_state, w_last_rx, w_rx_gap_max;   /* (the longest silence of the connection, ms) */
static int w_was_active;
static struct sockaddr_in w_fixed, w_kept, w_peer;   /* asked for (cfg.wifi_addr), remembered, connected to */
static int w_has_fixed, w_has_kept;
static char w_kept_path[512], w_peer_str[32], w_peer_name[24];
static wcand_t w_cand[8];
static uint8_t *w_out;
static uint32_t w_out_n;
static uint32_t w_connects, w_drops, w_probes, w_answers;

/* ------------------------------------------------------------------ USB-A --- */
/* USB-MIDI packets (cable 2) -> MIDI bytes, in order */
static int pkts_bytes(const uint8_t (*p)[4], int n, uint8_t *out)
{
    static const int LEN[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };
    int i, k = 0;
    for (i = 0; i < n; i++) {
        const int len = LEN[p[i][0] & 0x0F];
        memcpy(out + k, p[i] + 1, (size_t)len);
        k += len;
    }
    return k;
}

static void on_usba(void *ctx, const uint8_t (*p)[4], int n)
{
    uint8_t b[64 * 3];
    (void)ctx;
    while (n > 0) {
        const int take = n > 64 ? 64 : n, k = pkts_bytes(p, take, b);
        if (k && S)
            rfm_session_rx(S, g_link, b, (uint32_t)k, g_now);
        p += take, n -= take;
    }
}

static int usba_write(void *ctx, const uint8_t *p, uint32_t n)
{
    (void)ctx;
    if (mr_usba_send(p, n) < 0)
        g_send_full++;   /* (never a failure: the session would drop the link for good; a lost frame is resent) */
    return 0;
}

/* ------------------------------------------------------------------ Wi-Fi --- */
static void addr_str(const struct sockaddr_in *a, char *out, size_t cap)
{
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
    snprintf(out, cap, "%s:%u", ip, (unsigned)ntohs(a->sin_port));
}

static int addr_parse(const char *s, struct sockaddr_in *a)   /* HOST[:PORT] (a name is looked up once) */
{
    char host[256];
    const char *c = strrchr(s, ':');
    size_t hl = c ? (size_t)(c - s) : strlen(s);
    struct addrinfo hints, *res = NULL;
    if (!hl || hl >= sizeof host)
        return -1;
    memcpy(host, s, hl);
    host[hl] = 0;
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)(c && atoi(c + 1) > 0 ? atoi(c + 1) : FM1LINK_TCP_PORT));
    if (inet_pton(AF_INET, host, &a->sin_addr) == 1)
        return 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
        return -1;
    a->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

static int same_host(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void w_set(int st, uint32_t now)
{
    if (st != w_state)
        g_changed = 1;                   /* (the page shows the search) */
    w_state = st;
    w_t_state = now;
}

static void w_remember(void)
{
    FILE *f;
    if (!w_kept_path[0] || (w_has_kept && same_host(&w_kept, &w_peer)))
        return;
    w_kept = w_peer;
    w_has_kept = 1;
    if ((f = fopen(w_kept_path, "w"))) {
        fprintf(f, "%s\n", w_peer_str);
        fclose(f);
    }
}

static void w_close(uint32_t now, const char *why, int bad)
{
    int k;
    if (w_tcp >= 0) {
        close(w_tcp);
        w_tcp = -1;
    }
    w_out_n = 0;
    if (w_link >= 0) {
        rfm_session_remove_link(S, w_link, now);   /* (a session on it: DISCOVER, then USB-A if it is there) */
        w_link = -1;
    }
    for (k = 0; bad && k < 8; k++)
        if (w_cand[k].used && same_host(&w_cand[k].a, &w_peer))
            w_cand[k].t_bad = now | 1u;
    if (w_state >= W_CONNECTING) {
        w_drops++;
        fprintf(stderr, "fm1link: Wi-Fi %s %s: %s\n", w_state == W_CONNECTED ? "dropped" : "no connection to",
                w_peer_str, why);
    }
    w_failed = 0;
    w_set(W_SEARCH, now);
    w_t_probe = now - W_PROBE_MS + 500u;   /* (look again soon) */
}

static int tcp_flush(void)
{
    while (w_out_n) {
        const ssize_t k = send(w_tcp, w_out, w_out_n, MSG_NOSIGNAL);
        if (k > 0) {
            memmove(w_out, w_out + k, w_out_n - (uint32_t)k);
            w_out_n -= (uint32_t)k;
        } else if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return 0;
        } else {
            return -1;
        }
    }
    return 0;
}

static int tcp_write(void *ctx, const uint8_t *p, uint32_t n)
{
    (void)ctx;
    if (w_tcp < 0 || w_state != W_CONNECTED || w_out_n + n > W_OUT_CAP) {
        w_failed = 1;                    /* (the session drops the link; the tick closes the socket) */
        return -1;
    }
    memcpy(w_out + w_out_n, p, n);
    w_out_n += n;
    if (tcp_flush() < 0) {
        w_failed = 1;
        return -1;
    }
    return 0;
}

static void w_connect(const struct sockaddr_in *a, const char *name, uint32_t now)
{
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (s < 0)
        return;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    w_peer = *a;
    addr_str(a, w_peer_str, sizeof w_peer_str);
    snprintf(w_peer_name, sizeof w_peer_name, "%s", name ? name : "");
    w_tcp = s;
    w_set(W_CONNECTING, now);
    if (connect(s, (const struct sockaddr *)a, sizeof *a) == 0 || errno == EINPROGRESS)
        return;
    w_close(now, strerror(errno), 1);
}

static void w_probe(uint32_t now)
{
    static const char Q[] = "RFM1?";
    struct sockaddr_in b;
    w_t_probe = now;
    w_probes++;
    memset(&b, 0, sizeof b);
    b.sin_family = AF_INET;
    b.sin_port = htons(w_udp_port);
    b.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(w_udp, Q, 5, 0, (struct sockaddr *)&b, sizeof b);
    b.sin_addr.s_addr = htonl(INADDR_LOOPBACK);    /* (an FM-1 simulator on this machine: the tests) */
    sendto(w_udp, Q, 5, 0, (struct sockaddr *)&b, sizeof b);
    if (w_has_fixed) {
        b.sin_addr = w_fixed.sin_addr;
        sendto(w_udp, Q, 5, 0, (struct sockaddr *)&b, sizeof b);
    }
    if (w_has_kept) {
        b.sin_addr = w_kept.sin_addr;
        sendto(w_udp, Q, 5, 0, (struct sockaddr *)&b, sizeof b);
    }
}

static void w_read_answers(uint32_t now)
{
    uint8_t q[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        rfm_net_info_t ni;
        int k, slot = -1;
        const ssize_t n = recvfrom(w_udp, q, sizeof q, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0)
            return;
        if (rfm_parse_discovery(q, (uint32_t)n, &ni))
            continue;
        w_answers++;
        from.sin_port = htons(ni.port);   /* (the session's port; the address the answer came from) */
        for (k = 0; k < 8; k++)
            if (w_cand[k].used && same_host(&w_cand[k].a, &from))
                slot = k;
        for (k = 0; slot < 0 && k < 8; k++)
            if (!w_cand[k].used || now - w_cand[k].t_seen > 60000u)
                slot = k;
        if (slot < 0)
            continue;
        if (!w_cand[slot].used || !same_host(&w_cand[slot].a, &from))
            memset(&w_cand[slot], 0, sizeof w_cand[slot]);
        w_cand[slot].used = 1;
        w_cand[slot].a = from;
        w_cand[slot].busy = ni.busy;
        w_cand[slot].t_seen = now | 1u;
        snprintf(w_cand[slot].name, sizeof w_cand[slot].name, "%s", ni.name);
    }
}

/* the FM-1 to connect to now, of those that answered in the last 5 s: the one asked for, then the one remembered (busy
 * or not), then any that is free; one that failed lately is left for W_BAD_MS (the remembered one W_BAD_KEPT_MS), the
 * one asked for never. -1 none */
static int w_pick(uint32_t now)
{
    int k, best = -1, rank = 0;
    for (k = 0; k < 8; k++) {
        const wcand_t *c = &w_cand[k];
        int r;
        if (!c->used || now - c->t_seen > 5000u)
            continue;
        r = w_has_fixed && same_host(&c->a, &w_fixed) ? 4 : w_has_kept && same_host(&c->a, &w_kept) ? 3
          : !c->busy ? 2 : 0;
        if (r < 4 && c->t_bad && now - c->t_bad < (r == 3 ? W_BAD_KEPT_MS : W_BAD_MS))
            r = 0;
        if (r > rank)
            rank = r, best = k;
    }
    return best;
}

static void wifi_tick(uint32_t now)
{
    if (!w_on)
        return;
    w_read_answers(now);
    switch (w_state) {
    case W_SEARCH: {
        int k;
        if (now - w_t_probe >= W_PROBE_MS)
            w_probe(now);
        if ((k = w_pick(now)) >= 0) {
            w_connect(&w_cand[k].a, w_cand[k].name, now);
        } else if (w_has_fixed && now - w_t_state >= W_PROBE_MS + 500u) {
            w_connect(&w_fixed, "", now);   /* (no answer: TCP by its address anyway; UDP may be filtered) */
        }
        break;
    }
    case W_CONNECTING: {
        struct timeval tv = {0, 0};
        fd_set wr;
        int err = 0;
        socklen_t el = sizeof err;
        FD_ZERO(&wr);
        FD_SET(w_tcp, &wr);
        if (select(w_tcp + 1, NULL, &wr, NULL, &tv) > 0) {
            if (getsockopt(w_tcp, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
                w_close(now, strerror(err ? err : errno), 1);
                break;
            }
            w_set(W_CONNECTED, now);
            w_last_rx = now;
            w_was_active = 0;
            w_connects++;
            fprintf(stderr, "fm1link: Wi-Fi connected to %s %s\n", w_peer_str, w_peer_name);
            if (w_link < 0)
                w_link = rfm_session_add_link(S, RFM_LINK_TCP, tcp_write, NULL, "Wi-Fi");
            else
                rfm_session_link_reset(S, w_link, now);
            if (w_link < 0) {
                w_close(now, "no room for a link", 0);
                break;
            }
            /* Wi-Fi first: a session on USB-A (or a search) starts again, and the Wi-Fi FM-1 answers first */
            if (rfm_session_state(S) != RFM_S_DISCOVER)
                rfm_session_restart(S, now);
        } else if (now - w_t_state >= W_CONNECT_MS) {
            w_close(now, "the connection timed out", 1);
        }
        break;
    }
    case W_CONNECTED: {
        uint8_t b[16384];
        int k;
        for (k = 0; k < 16; k++) {
            const ssize_t n = recv(w_tcp, b, sizeof b, 0);
            if (n > 0) {
                rfm_session_rx(S, w_link, b, (uint32_t)n, now);
                if (now - w_last_rx > w_rx_gap_max)
                    w_rx_gap_max = now - w_last_rx;
                w_last_rx = now;
                continue;
            }
            if (n == 0) {
                w_close(now, "closed by the FM-1", 0);
                return;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                w_close(now, strerror(errno), 0);
                return;
            }
            break;
        }
        if (w_failed || tcp_flush() < 0) {
            w_close(now, "a write failed", 0);
            return;
        }
        if (rfm_session_frame_link(S) == w_link && rfm_session_state(S) == RFM_S_ACTIVE) {
            w_was_active = 1;                /* (linked: the session's own timeouts watch it) */
            w_remember();
        } else if (now - w_last_rx >= W_SILENT_MS) {
            w_close(now, "nothing from it", 1);
        } else if (!w_was_active && now - w_t_state >= W_SESSION_MS) {
            w_close(now, "no session", 1);
        }
        break;
    }
    default:
        break;
    }
}

static int wifi_init(const fm1link_cfg_t *cfg)
{
    struct sockaddr_in a;
    int one = 1;
    w_udp_port = (uint16_t)(cfg && cfg->udp_port > 0 ? cfg->udp_port : FM1LINK_UDP_PORT);
    if (cfg && cfg->wifi_addr && cfg->wifi_addr[0]) {
        if (addr_parse(cfg->wifi_addr, &w_fixed) == 0)
            w_has_fixed = 1;
        else
            fprintf(stderr, "fm1link: --fm1-addr %s: not an address\n", cfg->wifi_addr);
    }
    if (cfg && cfg->root) {
        FILE *f;
        char line[300];
        snprintf(w_kept_path, sizeof w_kept_path, "%s/fm1-wifi.txt", cfg->root);
        if ((f = fopen(w_kept_path, "r"))) {
            if (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                w_has_kept = addr_parse(line, &w_kept) == 0;
            }
            fclose(f);
        }
    }
    if (!(w_out = malloc(W_OUT_CAP)) || (w_udp = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        return -1;
    fcntl(w_udp, F_SETFL, fcntl(w_udp, F_GETFL, 0) | O_NONBLOCK);
    setsockopt(w_udp, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;              /* (any port: the FM-1 answers the sender) */
    if (bind(w_udp, (struct sockaddr *)&a, sizeof a) < 0)
        return -1;
    w_on = 1;
    w_state = W_SEARCH;
    return 0;
}

/* ------------------------------------------------------------------ the panel --- */
static void release_all(int slot)
{
    int k;
    for (k = 0; k < FW_NKEY; k++)
        if (g_keys >> k & 1)
            engine_fm1_in(slot, FWI_KEY, k, 0);
    for (k = 0; k < FW_NBTN; k++)
        if (g_btns >> k & 1)
            engine_fm1_in(slot, FWI_BTN, k, 0);
    g_keys = g_btns = 0;
}

static void on_event(void *ctx, const rfm_event_t *e)
{
    const int st = rfm_session_state(S);
    (void)ctx;
    if (st != RFM_S_ACTIVE && st != RFM_S_STARTING)
        return;   /* (the unit sends its panel always: only a session counts) */
    switch (e->type) {
    case RFM_EV_KEY:
        if (e->index >= FW_NKEY)
            break;
        if (e->value) {
            g_keys |= 1u << e->index;
            engine_fm1_in(-1, FWI_KEY, e->index, e->vel ? e->vel : 100);
        } else if (g_keys >> e->index & 1) {
            g_keys &= ~(1u << e->index);
            engine_fm1_in(-1, FWI_KEY, e->index, 0);
        }
        break;
    case RFM_EV_PRESS:
        if (e->index < FW_NKEY && (g_keys >> e->index & 1))
            engine_fm1_in(-1, FWI_LEAN, e->index, (e->value * 100 + 63) / 127);
        break;
    case RFM_EV_BUTTON:
        if (e->index >= FW_NBTN)
            break;
        if (e->value) {
            g_btns |= 1u << e->index;
            engine_fm1_in(-1, FWI_BTN, e->index, 1);
        } else if (g_btns >> e->index & 1) {
            g_btns &= ~(1u << e->index);
            engine_fm1_in(-1, FWI_BTN, e->index, 0);
        }
        break;
    case RFM_EV_ENC:
        if (e->index < FW_NENC && e->value)
            engine_fm1_in(-1, FWI_ENC, e->index, e->value);
        break;
    case RFM_EV_MASTER:
        engine_fm1_in(-1, FWI_MASTER, 0, e->value);
        break;
    default:
        break;
    }
}

static void on_state(void *ctx, int st, const char *why)
{
    const int linked = st == RFM_S_ACTIVE;
    (void)ctx;
    snprintf(g_state_why, sizeof g_state_why, "%s", why ? why : "");
    if (!linked)
        release_all(g_slot);   /* (no stuck notes) */
    if (linked != g_linked) {
        const rfm_hello_t *h = rfm_session_hello(S);
        g_linked = linked;
        g_changed = 1;
        if (linked)
            fprintf(stderr, "fm1link: FM-1 linked: %s %s over %s\n", h->name, h->fw_ver,
                    rfm_session_frame_link(S) >= 0 && rfm_session_frame_link(S) == w_link ? w_peer_str : "USB-A");
        else
            fprintf(stderr, "fm1link: FM-1 lost (%s)\n", why ? why : "");
    }
}

static void on_midi_in(void *ctx, const uint8_t *m, uint32_t n)
{
    (void)ctx;
    engine_fm1_midi(m, (int)n);
}

static void on_log(void *ctx, const char *m)
{
    (void)ctx;
    if (strncmp(m, "FRAME_ACK", 9) || strstr(m, "shadow"))   /* (a lost frame is said by the counters; a shadow reset
                                                               * and its reason are logged) */
        fprintf(stderr, "fm1link: %s\n", m);
}

void fm1link_leds(const uint8_t levels[FW_NLEVELS], uint8_t play_green, void *out)
{
    rfm_leds_t *l = (rfm_leds_t *)out;
    uint32_t col, line;
    memset(l, 0, sizeof *l);
    for (col = 0; col < RFM_NCOL; col++)
        for (line = 0; line < 4u; line++) {
            const uint8_t v = levels[col * 4u + line], bit = (uint8_t)(1u << (line + 1u));
            if (v >= 176u)
                l->lit[col] |= bit;
            else if (v >= 96u)
                l->mid[col] |= bit;
            else if (v >= 40u)
                l->dim[col] |= bit;
            else if (v)
                l->faint[col] |= bit;
        }
    l->flags = play_green >= 176 ? RFM_LEDS_F_PLAY_GREEN : 0;
}

/* ------------------------------------------------------------------ the API --- */
int fm1link_init(const fm1link_cfg_t *cfg)
{
    rfm_session_cfg_t sc;
    rfm_session_cb_t cb;
    if (cfg && cfg->off)
        return 0;
    g_usba_screen = cfg && cfg->usba_screen;
    rfm_session_cfg_default(&sc);
    sc.host_name = "RemoteFM14Move";
    sc.transport = RFM_T_AUTO;           /* (USB-A: a SysEx link, slow_link; Wi-Fi: a TCP link, preferred) */
    sc.retry_ms = 2000;              /* HELLO every 2 s while nothing answers */
    sc.keepalive_ms = 3000;
    sc.slow_link = 1;
    sc.slow_bps = cfg && cfg->bps ? cfg->bps : FM1LINK_BPS_DEFAULT;
    sc.slow_wire_cap = FM1LINK_WIRE_CAP;
    memset(&cb, 0, sizeof cb);
    cb.event = on_event;
    cb.state = on_state;
    cb.log = on_log;
    cb.midi_in = on_midi_in;
    if (!(S = rfm_session_new(&sc, &cb)))
        return -1;
    if ((g_link = rfm_session_add_link(S, RFM_LINK_SX, usba_write, NULL, "Move USB-A")) < 0)
        return -1;
    g_usb_link = g_link;
    if (!(cfg && cfg->wifi_off) && wifi_init(cfg) != 0)
        fprintf(stderr, "fm1link: no Wi-Fi search (%s)\n", strerror(errno));
    mr_set_usba_in(on_usba, NULL);
    mr_set_usba_cap(cfg && cfg->usba_cap ? cfg->usba_cap : FM1LINK_CAP_DEFAULT);
    g_on = 1;
    fprintf(stderr, "fm1link: on USB-A, %u B/s of screen, %d packets a transfer; Wi-Fi %s\n", sc.slow_bps,
            cfg && cfg->usba_cap ? cfg->usba_cap : FM1LINK_CAP_DEFAULT,
            w_on ? (w_has_fixed ? cfg->wifi_addr : "by broadcast") : "off");
    return 0;
}

/* USB-A (the Move's bridge, ~8 KB/s) can't carry the firmware's screen: the FM-1 shows a plug on black instead, sent
 * once (black tiles go as fills: a couple of KB). Over Wi-Fi the firmware's own screen goes as before. */
static int g_plug;   /* the plug screen is what the session has */
static const char *const PLUG[16] = {
    "................", ".....#....#.....", ".....#....#.....", ".....#....#.....",
    "...##########...", "...##########...", "...##########...", "...##########...",
    "....########....", ".....######.....", "......####......", ".......##.......",
    ".......##.......", ".......##.......", ".......##.......", ".......##.......",
};
void fm1link_plug_screen(uint16_t *lcd)
{
    const uint16_t c = 0x5E5F, be = (uint16_t)(c >> 8 | c << 8);   /* a light cyan, RGB565 (big-endian bytes) */
    const int sc = 6, x0 = (240 - 16 * sc) / 2, y0 = (240 - 16 * sc) / 2;
    int x, y;
    memset(lcd, 0, 240 * 240 * sizeof *lcd);
    for (y = 0; y < 16 * sc; y++)
        for (x = 0; x < 16 * sc; x++)
            if (PLUG[y / sc][x / sc] == '#')
                lcd[(y0 + y) * 240 + x0 + x] = be;
}
static int on_usba_frames(void) { return !g_usba_screen && !(w_link >= 0 && rfm_session_frame_link(S) == w_link); }

void fm1link_tick(uint32_t now, int slot, fw_t *fw)
{
    if (!g_on)
        return;
    g_now = now;
    mr_usba_poll();   /* (on_usba: into the session) */
    wifi_tick(now);   /* (the FM-1 over Wi-Fi: its bytes into the session) */
    if (slot != g_slot || fw != g_fw) {   /* a slot switch (or its firmware changed): what it shows from now */
        if (g_slot >= 0 && slot != g_slot)
            release_all(g_slot);
        g_slot = slot, g_fw = fw;
        g_writes = 0, g_levels_ok = 0, g_dark = 0;
        g_last_frame = now - FRAME_MS;
    }
    if (fw && !fw->dead) {
        const uint32_t w = fw_lcd_writes(fw);
        uint8_t lv[FW_NLEVELS + 1];
        if (on_usba_frames()) {
            if (!g_plug) {
                g_plug = 1;
                fm1link_plug_screen(g_lcd);
                rfm_session_set_frame(S, g_lcd);
            }
        } else if (g_plug) {   /* Wi-Fi took the frames: the firmware's screen again, at once */
            g_plug = 0, g_writes = 0;
            g_last_frame = now - FRAME_MS;
        }
        if (!g_plug && (w != g_writes || !g_writes) && now - g_last_frame >= FRAME_MS) {
            g_writes = w ? w : 1;
            g_last_frame = now;
            memcpy(g_lcd, fw_lcd(fw), sizeof g_lcd);
            rfm_session_set_frame(S, g_lcd);
        }
        memcpy(lv, fw_leds(fw), FW_NLEVELS);
        lv[FW_NLEVELS] = fw_play_green(fw);
        if (!g_levels_ok || memcmp(lv, g_levels, sizeof lv)) {
            rfm_leds_t l;
            memcpy(g_levels, lv, sizeof lv);
            g_levels_ok = 1;
            fm1link_leds(lv, lv[FW_NLEVELS], &l);
            rfm_session_set_leds(S, &l);
        }
    } else if (!g_dark) {   /* nothing plays: a dark panel */
        rfm_leds_t l;
        g_dark = 1, g_plug = 0;
        memset(g_lcd, 0, sizeof g_lcd);
        rfm_session_set_frame(S, g_lcd);
        memset(&l, 0, sizeof l);
        rfm_session_set_leds(S, &l);
    }
    rfm_session_poll(S, now);
    if (now - g_win_ms >= 1000u) {
        rfm_session_stats_t st;
        const uint32_t dt = now - g_win_ms;
        rfm_session_stats(S, &st);
        if (g_win_ms) {
            g_bps = (st.wire_bytes - g_win_wire) * 1000u / dt;
            g_fps = (st.acks - g_win_acks) * 1000u / dt;
        }
        g_win_ms = now, g_win_wire = st.wire_bytes, g_win_acks = st.acks;
    }
}

int fm1link_changed(void)
{
    const int c = g_changed;
    g_changed = 0;
    return c;
}

int fm1link_linked(void) { return g_linked; }

int fm1link_json(char *out, int cap)
{
    static const char *const WS[] = {"off", "searching", "connecting", "connected"};
    rfm_session_stats_t st;
    const rfm_hello_t *h;
    char nm[80], fv[64], pn[48];
    uint32_t in, in_drop, pk_out;
    int fl, wifi;
    if (!g_on)
        return snprintf(out, (size_t)cap, "{\"linked\":false,\"state\":\"off\",\"link\":\"\",\"wifi\":\"off\"}");
    rfm_session_stats(S, &st);
    h = rfm_session_hello(S);
    mj_escape(g_linked ? h->name : "", nm, sizeof nm);
    mj_escape(g_linked ? h->fw_ver : "", fv, sizeof fv);
    mj_escape(w_state >= W_CONNECTING ? w_peer_name : "", pn, sizeof pn);
    mr_usba_stats(&in, &in_drop, &pk_out);
    fl = rfm_session_frame_link(S);
    wifi = g_linked && fl >= 0 && fl == w_link;
    return snprintf(out, (size_t)cap,
                    "{\"linked\":%s,\"state\":\"%s\",\"name\":\"%s\",\"fw\":\"%s\",\"fps\":%u,\"bps\":%u,\"lost\":%u,"
                    "\"sessions\":%u,\"frames\":%u,\"acks\":%u,\"ack_ms\":%u,\"ack_max\":%u,\"timeouts\":%u,"
                    "\"rate\":%u,\"held\":%u,\"in_flight\":%u,\"queued\":%d,\"usb_in\":%u,\"usb_in_dropped\":%u,"
                    "\"usb_out\":%u,\"send_full\":%u,\"midi_in\":%u,\"events\":%u,"
                    "\"link\":\"%s\",\"addr\":\"%s\",\"wifi\":\"%s\",\"wifi_name\":\"%s\",\"wifi_connects\":%u,"
                    "\"wifi_drops\":%u,\"wifi_answers\":%u,\"panel_ev\":%u,\"draw_us_max\":%u,\"panel_r_gap_max\":%u,"
                    "\"shadow_resets\":%u,\"reset_reason\":%u,\"wifi_rx_ms_max\":%u}",
                    g_linked ? "true" : "false", rfm_session_state_name(rfm_session_state(S)), nm, fv, g_fps, g_bps,
                    st.lost, st.sessions, st.frames, st.acks, st.acks ? st.ack_ms_sum / st.acks : 0, st.ack_ms_max,
                    st.ack_timeouts, st.rate, st.held, st.in_flight, mr_usba_queued(), in, in_drop, pk_out,
                    g_send_full, st.midi_in, st.events, !g_linked ? "" : wifi ? "wifi" : "usb",
                    w_state >= W_CONNECTING ? w_peer_str : "", WS[w_on ? w_state : W_OFF], pn, w_connects, w_drops,
                    w_answers, st.panel_ev, st.draw_us_max, st.panel_r_gap_max, st.shadow_resets, st.reset_reason,
                    w_rx_gap_max);
}

void fm1link_stop(uint32_t now)
{
    if (!g_on)
        return;
    release_all(g_slot);
    if (g_linked) {
        rfm_session_stop(S, now);   /* (SESSION 0 queued: Move Remote sends it while it shuts down) */
        mr_usba_poll();
        if (w_tcp >= 0 && w_state == W_CONNECTED)
            tcp_flush();            /* (over Wi-Fi: it goes now; the socket closes with the process) */
    }
}
