/* SPDX-License-Identifier: MPL-2.0 */
/* The simulator's FM-1 on USB-A (tests; the simulator's host only): libremotefm1's fake unit (third_party/RemoteFM1
 * host/src/fake.c) behind Move Remote's simulated USB-A (plat_sim.c mr_sim_usba_*: the surface controller's bridge,
 * which loses what a burst from the device does not fit in its buffer, and takes 31 packets a transfer). A thread
 * runs it every millisecond: the Move's cable-2 packets into the unit, the unit's MIDI out as USB-MIDI packets into the
 * bridge, all at once (a USB device bursts).
 *
 * The tests drive it through the page's test API (web.c, --test-api): POST /api/test/fm1 {"op":..,"a":..,"b":..}
 * (key a vel|0, press a p, btn a 1|0, enc a detents, master b, trs a note b vel (a note on the unit's 5-pin jack),
 * plug a 0|1 (1: a fresh unit plugged in), wifi a 0|1 b PORT (a fresh unit plugged in, RemoteFM1 Wi-Fi when a: its
 * WIFI answers, the networks around FM1SIM_WIFI_NETS, joined it says 127.0.0.1:PORT; the tests' password
 * FM1SIM_WIFI_PASS), plain a 0|1 (1: a plain MIDI device in its place, not RemoteFM1: it answers nothing)) and
 * GET /api/test/fm1: how the unit stands (its session, its screen
 * against the active slot's, its LEDs against the slot's, its counters, the bridge's; dev_msgs, dev_last: the channel
 * messages the Move sent on USB-A, whoever is plugged in: Out's, midiout.h). */
#define _GNU_SOURCE
#include "fm1sim.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "engine.h"
#include "fm1link.h"
#include "minijson.h"
#include "mr_core.h"
#include "rfm/fake.h"

static pthread_t g_thr;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_run;
static rfm_fake_t *F;
static int g_wifi, g_wifi_port;      /* the next fresh unit: RemoteFM1 Wi-Fi (fake.h cfg.wifi), its TCP port */
#define FM1SIM_WIFI_NETS "Dogsontheroof:-52:1:6;Phone hotspot:-58:1:1;Cafe open:-71:0:11;" \
                         "A neighbour's network with a long name:-80:1:3;Another neighbour's long network:-84:1:9;" \
                         "Yet another network nearby, far:-88:1:13"
#define FM1SIM_WIFI_PASS "test-pass-1234"   /* (fake credentials: tests/fm1wifisetup_e2e.sh) */
static int g_plugged, g_plain;
static uint32_t g_dev_msgs;
static uint8_t g_dev_last[3];
static uint32_t g_plugs;
/* the unit's MIDI out -> USB-MIDI packets (whole SysEx, 3 bytes a packet) */
static struct { uint8_t b[3]; int n, sx; } P;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static rfm_fake_t *fresh(void)
{
    rfm_fake_cfg_t fc;
    rfm_fake_cfg_default(&fc);   /* a RemoteFM1 0.3.6 unit on USB: HOLD, MIDI_IN, TOUCH */
    fc.has_bulk = 0;
    fc.ble = 0;
    if (g_wifi) {
        fc.wifi = 1;
        fc.wifi_nets = FM1SIM_WIFI_NETS;
        fc.wifi_ip = 0x0100007Fu;    /* 127.0.0.1: the Wi-Fi simulator on this machine (tests) */
        fc.wifi_port = (uint16_t)g_wifi_port;
        fc.wifi_join_ms = 1200;
    }
    return rfm_fake_new(&fc);
}

static int packetize(const uint8_t *b, int n, uint8_t (*out)[4], int max)
{
    int i, k = 0;
    for (i = 0; i < n && k < max; i++) {
        const uint8_t c = b[i];
        if (c >= 0xF8) {
            out[k][0] = 0x0F, out[k][1] = c, out[k][2] = out[k][3] = 0;
            k++;
            continue;
        }
        if (c == 0xF0) {
            P.sx = 1, P.n = 0;
        } else if (c >= 0x80 && c != 0xF7) {
            P.sx = 0, P.n = 0;
        }
        if (P.sx) {
            P.b[P.n++] = c;
            if (c == 0xF7) {
                memset(out[k], 0, 4);
                out[k][0] = (uint8_t)(0x04 + P.n);
                memcpy(out[k] + 1, P.b, (size_t)P.n);
                k++;
                P.sx = 0, P.n = 0;
            } else if (P.n == 3) {
                out[k][0] = 0x04;
                memcpy(out[k] + 1, P.b, 3);
                k++;
                P.n = 0;
            }
            continue;
        }
        if (c >= 0x80 || P.n) {
            const uint8_t st = c >= 0x80 ? c : P.b[0];
            const int len = (st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0 ? 2 : 3;
            if (c >= 0x80)
                P.n = 0;
            P.b[P.n++] = c;
            if (P.n == len) {
                memset(out[k], 0, 4);
                out[k][0] = (uint8_t)(st >> 4);
                memcpy(out[k] + 1, P.b, (size_t)len);
                k++;
                P.n = 0;
            }
        }
    }
    return k;
}

static void *sim_main(void *arg)
{
    static uint8_t buf[65536], pk[4096][4];
    (void)arg;
    while (g_run) {
        const uint32_t now = now_ms();
        int n, i, k = 0;
        pthread_mutex_lock(&g_mu);
        n = mr_sim_usba_dev_recv(pk, 4096);   /* the Move's cable 2 -> the unit */
        for (i = 0; i < n && g_plugged; i++)   /* (the channel messages: Out's) */
            if ((pk[i][0] & 0x0F) >= 0x8 && (pk[i][0] & 0x0F) <= 0xE) {
                g_dev_msgs++;
                memcpy(g_dev_last, pk[i] + 1, 3);
            }
        if (g_plain)
            n = 0;
        if (n > 0 && g_plugged && F) {
            static const int LEN[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };
            for (i = 0; i < n; i++) {
                memcpy(buf + k, pk[i] + 1, (size_t)LEN[pk[i][0] & 0x0F]);
                k += LEN[pk[i][0] & 0x0F];
            }
            rfm_fake_rx_sx(F, buf, (uint32_t)k, now);
        }
        if (g_plugged && F && !g_plain) {
            uint32_t got;
            rfm_fake_poll(F, now);
            while ((got = rfm_fake_take_sx(F, buf, sizeof buf)) > 0) {   /* the unit -> the bridge, at once */
                const int np = packetize(buf, (int)got, pk, 4096);
                if (np)
                    mr_sim_usba_dev_send((const uint8_t(*)[4])pk, np);
            }
        }
        pthread_mutex_unlock(&g_mu);
        usleep(1000);
    }
    return NULL;
}

int fm1sim_start(void)
{
    F = fresh();
    if (!F)
        return -1;
    g_plugged = 1;
    g_plugs = 1;
    mr_sim_usba_plug(1);
    g_run = 1;
    if (pthread_create(&g_thr, NULL, sim_main, NULL) != 0)
        return -1;
    fprintf(stderr, "fm1sim: a fake FM-1 (RemoteFM1) on the simulated USB-A\n");
    return 0;
}

void fm1sim_stop(void)
{
    if (!g_run)
        return;
    g_run = 0;
    pthread_join(g_thr, NULL);
    rfm_fake_free(F);
    F = NULL;
}

int fm1sim_cmd(const char *op, long a, long b)
{
    int r = 0;
    if (!g_run)
        return -1;
    pthread_mutex_lock(&g_mu);
    if (!strcmp(op, "plug")) {
        if (a && !g_plugged) {   /* a fresh unit, plugged in */
            rfm_fake_free(F);
            F = fresh();
            memset(&P, 0, sizeof P);
            g_plugs++;
        }
        g_plugged = a != 0;
        mr_sim_usba_plug(g_plugged);
    } else if (!strcmp(op, "plain")) {   /* a plain MIDI device on USB-A (a: 1), or a fresh unit again (0) */
        if (!a && g_plain) {
            rfm_fake_free(F);
            F = fresh();
            memset(&P, 0, sizeof P);
            g_plugs++;
        }
        g_plain = a != 0;
        g_plugged = 1;
        mr_sim_usba_plug(1);
    } else if (!strcmp(op, "wifi")) {   /* a fresh unit plugged in: RemoteFM1 Wi-Fi (a) or not */
        g_wifi = a != 0;
        g_wifi_port = b > 0 && b < 65536 ? (int)b : 7740;
        rfm_fake_free(F);
        F = fresh();
        memset(&P, 0, sizeof P);
        g_plugs++;
        g_plugged = 1;
        mr_sim_usba_plug(1);
    } else if (!F || !g_plugged)
        r = -1;
    else if (!strcmp(op, "key"))
        rfm_fake_key_vel(F, (uint32_t)a, b > 0, (uint32_t)(b > 0 ? b : 0));
    else if (!strcmp(op, "press"))
        rfm_fake_press(F, (uint32_t)a, (uint32_t)b);
    else if (!strcmp(op, "btn"))
        rfm_fake_button(F, (uint32_t)a, b != 0);
    else if (!strcmp(op, "enc"))
        rfm_fake_enc(F, (uint32_t)a, (int32_t)b);
    else if (!strcmp(op, "master"))
        rfm_fake_master(F, (uint32_t)b);
    else if (!strcmp(op, "trs")) {
        const uint8_t m[3] = { (uint8_t)(b ? 0x90 : 0x80), (uint8_t)(a & 0x7F), (uint8_t)(b & 0x7F) };
        rfm_fake_trs(F, m, 3);
    } else
        r = -1;
    pthread_mutex_unlock(&g_mu);
    return r;
}

int fm1sim_json(char *out, int cap)
{
    static engine_view_t v;   /* (the web thread's: one at a time) */
    uint32_t seq = 0, led_pkts = 0, usba_pkts = 0, lost = 0, same = 0, i;
    rfm_fake_stats_t fs;
    rfm_leds_t want;
    int sess = 0, exact, leds = 0, holding = 0, plug = 0, wst = 0, wkept = 0, wpass = 0;
    uint32_t wreqs = 0, wsets = 0, wscans = 0;
    char wssid[33] = "", wesc[80];
    static uint16_t plug_px[240 * 240];   /* (the USB-A plug screen, made once) */
    static int plug_made;
    if (!g_run)
        return snprintf(out, (size_t)cap, "{\"on\":0}");
    engine_view(&v, &seq);
    mr_sim_counts(&led_pkts, &usba_pkts, &lost);
    fm1link_leds(v.leds, v.play_green, &want);
    pthread_mutex_lock(&g_mu);
    memset(&fs, 0, sizeof fs);
    if (F) {
        const uint8_t *px = rfm_fake_panel(F);
        rfm_fake_stats(F, &fs);
        sess = rfm_fake_session(F);
        holding = rfm_fake_holding(F);
        for (i = 0; i < 240u * 240u; i++)
            same += px[2 * i] == v.lcd[2 * i] && px[2 * i + 1] == v.lcd[2 * i + 1];
        leds = rfm_leds_equal(rfm_fake_leds(F), &want);
        if (!plug_made)
            fm1link_plug_screen(plug_px), plug_made = 1;
        plug = !memcmp(px, plug_px, sizeof plug_px);
        if (g_wifi) {
            char k[33];
            wst = rfm_fake_wifi_state(F, wssid);
            while (rfm_fake_wifi_kept(F, wkept, k))
                wpass += rfm_fake_wifi_pass_is(F, k, FM1SIM_WIFI_PASS), wkept++;
            rfm_fake_wifi_counts(F, &wreqs, &wsets, &wscans);
        }
    }
    mj_escape(wssid, wesc, sizeof wesc);
    exact = same == 240u * 240u;
    pthread_mutex_unlock(&g_mu);
    return snprintf(out, (size_t)cap,
                    "{\"on\":1,\"plugged\":%d,\"plugs\":%u,\"session\":%d,\"screen_same\":%u,\"screen_pct\":%.1f,"
                    "\"screen_exact\":%d,\"plug\":%d,\"holding\":%d,\"leds_match\":%d,\"hellos\":%u,\"sessions\":%u,\"frames\":%u,"
                    "\"held_frames\":%u,\"decode_errs\":%u,\"keepalive_expired\":%u,\"leds\":%u,\"panel_updates\":%u,"
                    "\"led_pkts\":%u,\"usba_pkts\":%u,\"bridge_lost\":%u,\"wifi\":%d,\"wifi_state\":%d,\"wifi_ssid\":\"%s\","
                    "\"wifi_kept\":%d,\"wifi_pass_ok\":%d,\"wifi_reqs\":%u,\"wifi_sets\":%u,\"wifi_scans\":%u,"
                    "\"plain\":%d,\"dev_msgs\":%u,\"dev_last\":\"%02x%02x%02x\"}",
                    g_plugged, g_plugs, sess, same, same * 100.0 / (240.0 * 240.0), exact, plug, holding, leds, fs.hellos,
                    fs.sessions_started, fs.frames, fs.held_frames, fs.decode_errs, fs.keepalive_expired, fs.leds,
                    fs.panel_updates, led_pkts, usba_pkts, lost, g_wifi, wst, wesc, wkept, wpass, wreqs, wsets, wscans,
                    g_plain, g_dev_msgs, g_dev_last[0], g_dev_last[1], g_dev_last[2]);
}
