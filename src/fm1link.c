/* SPDX-License-Identifier: MPL-2.0 */
/* The FM-1 on the Move's USB-A port: see fm1link.h. */
#define _GNU_SOURCE
#include "fm1link.h"

#include <stdio.h>
#include <string.h>

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
static int g_dark;                   /* a dark panel was given (no firmware) */
static uint16_t g_lcd[240 * 240];    /* the screen as the session wants it (emu_hal_t.lcd's layout: BE bytes) */
static uint8_t g_levels[FW_NLEVELS + 1];
static int g_levels_ok;
/* the counters: wire bytes out, frames acknowledged, a second at a time */
static uint32_t g_win_ms, g_win_wire, g_win_acks, g_fps, g_bps, g_send_full;
static char g_state_why[64];

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
            fprintf(stderr, "fm1link: FM-1 linked: %s %s\n", h->name, h->fw_ver);
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
    if (strncmp(m, "FRAME_ACK", 9))   /* (a lost frame is said by the counters) */
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
    rfm_session_cfg_default(&sc);
    sc.host_name = "RemoteFM14Move";
    sc.transport = RFM_T_MIDI;
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
    mr_set_usba_in(on_usba, NULL);
    mr_set_usba_cap(cfg && cfg->usba_cap ? cfg->usba_cap : FM1LINK_CAP_DEFAULT);
    g_on = 1;
    fprintf(stderr, "fm1link: on USB-A, %u B/s of screen, %d packets a transfer\n", sc.slow_bps,
            cfg && cfg->usba_cap ? cfg->usba_cap : FM1LINK_CAP_DEFAULT);
    return 0;
}

void fm1link_tick(uint32_t now, int slot, fw_t *fw)
{
    if (!g_on)
        return;
    g_now = now;
    mr_usba_poll();   /* (on_usba: into the session) */
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
        if ((w != g_writes || !g_writes) && now - g_last_frame >= FRAME_MS) {
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
        g_dark = 1;
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
    rfm_session_stats_t st;
    const rfm_hello_t *h;
    char nm[80], fv[64];
    uint32_t in, in_drop, pk_out;
    if (!g_on)
        return snprintf(out, (size_t)cap, "{\"linked\":false,\"state\":\"off\"}");
    rfm_session_stats(S, &st);
    h = rfm_session_hello(S);
    mj_escape(g_linked ? h->name : "", nm, sizeof nm);
    mj_escape(g_linked ? h->fw_ver : "", fv, sizeof fv);
    mr_usba_stats(&in, &in_drop, &pk_out);
    return snprintf(out, (size_t)cap,
                    "{\"linked\":%s,\"state\":\"%s\",\"name\":\"%s\",\"fw\":\"%s\",\"fps\":%u,\"bps\":%u,\"lost\":%u,"
                    "\"sessions\":%u,\"frames\":%u,\"acks\":%u,\"ack_ms\":%u,\"ack_max\":%u,\"timeouts\":%u,"
                    "\"rate\":%u,\"held\":%u,\"in_flight\":%u,\"queued\":%d,\"usb_in\":%u,\"usb_in_dropped\":%u,"
                    "\"usb_out\":%u,\"send_full\":%u,\"midi_in\":%u,\"events\":%u}",
                    g_linked ? "true" : "false", rfm_session_state_name(rfm_session_state(S)), nm, fv, g_fps, g_bps,
                    st.lost, st.sessions, st.frames, st.acks, st.acks ? st.ack_ms_sum / st.acks : 0, st.ack_ms_max,
                    st.ack_timeouts, st.rate, st.held, st.in_flight, mr_usba_queued(), in, in_drop, pk_out,
                    g_send_full, st.midi_in, st.events);
}

void fm1link_stop(uint32_t now)
{
    if (!g_on)
        return;
    release_all(g_slot);
    if (g_linked) {
        rfm_session_stop(S, now);   /* (SESSION 0 queued: Move Remote sends it while it shuts down) */
        mr_usba_poll();
    }
}
