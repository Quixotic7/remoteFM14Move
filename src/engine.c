/* SPDX-License-Identifier: MPL-2.0 */
/* The engine: see engine.h. */
#define _GNU_SOURCE
#include "engine.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../third_party/abletonmoveremote/common/ring.h"
#include "engine_dsp.h"
#include "fwrt.h"
#include "minijson.h"
#include "mr_core.h"
#include "rfm1cs/session.h"
#include "storage.h"
#include "wasm-rt.h"

#define NSLOT RFM_MOVE_FW_SLOTS
#define RING_FRAMES 4096
#define AUTOSAVE_MS 2000u      /* a firmware's flash saved this long after its last change */

static engine_cfg_t C;
static volatile int g_run;
static pthread_t g_thr;
static int g_started;

/* ------------------------------------------------------------------ the ring and the audio thread's side --- */
static mr_ring_t g_ring;                     /* stereo int16 frames: engine -> audio thread */
static volatile uint32_t g_late, g_rt_blocks, g_rt_started;
static int16_t *g_cap_out, *g_cap_mix, *g_cap_raw;
static volatile uint32_t g_cap_out_n;
static uint32_t g_cap_mix_n, g_cap_raw_n, g_cap_max;

int engine_audio(void *ctx, int16_t *out, int frames)
{
    (void)ctx;
    if (mr_ring_count(&g_ring) < (uint32_t)frames) {
        if (g_rt_started)
            g_late++;   /* the engine is late: this block is silence */
        return 0;
    }
    mr_ring_pop(&g_ring, out, (uint32_t)frames);
    g_rt_started = 1;
    g_rt_blocks++;
    if (g_cap_out) {
        const uint32_t n = g_cap_out_n;
        if (n + (uint32_t)frames <= g_cap_max) {
            memcpy(g_cap_out + 2u * n, out, (size_t)frames * 4u);
            g_cap_out_n = n + (uint32_t)frames;
        }
    }
    return frames;
}

/* ------------------------------------------------------------------ state (the engine thread's) --- */
typedef struct {
    fw_t fw;
    int on;                    /* a firmware runs here */
    int idx;                   /* its index in the catalog */
    uint8_t level;             /* 0..127 */
    uint16_t filt;             /* the sequencer tracks it hears (ROUTE) */
    uint32_t dirty_seen;       /* flash_dirty when last looked at */
    uint32_t dirty_ms;         /* .. when it moved (0: saved) */
    int dirty;
    uint64_t ns_mark;          /* the CPU window's start */
    uint32_t cpu_permille;     /* of real time, over the last window */
    uint32_t serial;           /* which instance (the raw capture follows the first) */
    int reboot;                /* a power-on asked after the firmware halted */
} slot_t;

static slot_t SL[NSLOT];
static int g_active;
static int g_master = -1;                    /* the volume knob's MASTER, -1 not touched yet */
static uint32_t g_serial;
static fw_t *g_raw_fw;                       /* the instance the raw capture and the input log follow */

static st_fw_t g_fws[ST_FW_MAX];
static rfm_move_fw_t g_list[ST_FW_MAX];
static char g_desc[ST_FW_MAX][200];
static int g_nfw;

static rfm1cs_session_t *S;
static int g_sock = -1;
static uint32_t g_attempt, g_open_pending;
static uint32_t g_session_at;   /* engine_cfg_t.splash_ms: when the session starts */
static int g_session_on;
static uint8_t *g_tx;
static size_t g_tx_n;
#define TX_CAP (1024 * 1024)

/* actions the session's callback could not act on inside the call (core calls): done right after it */
typedef struct {
    rfm_move_act_t a;
    char id[RFM_MOVE_FW_ID_MAX];
} defer_t;
static defer_t g_defer[64];
static int g_ndefer;
static int g_save_all;                       /* the Move is going: every flash now */

static mixer_t g_mix;
static uint32_t g_blocks;                    /* blocks rendered */
static int g_peak_acc, g_peak;               /* the mix's peak: this second's so far, the last second's */
static uint32_t g_last_screen_ms, g_lcd_seen;
static uint8_t g_leds_seen[FW_NLEVELS + 1];
static int g_leds_ok;
static uint16_t g_lcd_host[240 * 240];

/* ---- the web's view and input (a mutex the engine holds only to copy) */
static pthread_mutex_t g_vmu = PTHREAD_MUTEX_INITIALIZER;
static engine_view_t g_view;
static uint32_t g_view_lcd_ms;
static struct { int op, a, b; } g_pin[256];
static int g_npin;
static char g_status[1024];

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/* ------------------------------------------------------------------ the catalog --- */
static uint8_t cats_of(const char *id, uint8_t *cat)
{
    /* the catalog's categories are not in the manifest yet: the two bundled firmwares' are known */
    if (!strcmp(id, "felucca"))
        return *cat = 0, RFM_MOVE_CAT_SYNTH;
    if (!strcmp(id, "choralroot"))
        return *cat = 2, RFM_MOVE_CAT_CHORDS | RFM_MOVE_CAT_SYNTH;
    *cat = 255;
    return 0;
}

static void catalog_load(void)
{
    int i;
    g_nfw = st_fw_scan(g_fws, ST_FW_MAX);
    for (i = 0; i < g_nfw; i++) {
        rfm_move_fw_t *e = &g_list[i];
        memset(e, 0, sizeof *e);
        e->id = g_fws[i].id;
        e->name = g_fws[i].name;
        e->version = g_fws[i].version;
        e->installed = g_fws[i].version;
        snprintf(g_desc[i], sizeof g_desc[i], "%s %s. %s. Source: %s", g_fws[i].name, g_fws[i].version,
                 g_fws[i].license, g_fws[i].source_url);
        e->desc = g_desc[i];
        e->cats = cats_of(g_fws[i].id, &e->cat);
    }
    fprintf(stderr, "engine: %d firmware(s) installed\n", g_nfw);
}

static int catalog_find(const char *id)
{
    int i;
    for (i = 0; i < g_nfw; i++)
        if (!strcmp(g_fws[i].id, id))
            return i;
    return -1;
}

/* ------------------------------------------------------------------ the socket to Move Remote's core --- */
static void sock_flush(void)
{
    while (g_sock >= 0 && g_tx_n > 0) {
        const ssize_t w = send(g_sock, g_tx, g_tx_n, 0);
        if (w <= 0)
            return;
        memmove(g_tx, g_tx + w, g_tx_n - (size_t)w);
        g_tx_n -= (size_t)w;
    }
}

static void sock_write(const uint8_t *b, size_t n)
{
    if (g_sock < 0)
        return;
    sock_flush();
    if (g_tx_n == 0) {
        const ssize_t w = send(g_sock, b, n, 0);
        if (w > 0)
            b += w, n -= (size_t)w;
    }
    if (n && g_tx_n + n <= TX_CAP) {
        memcpy(g_tx + g_tx_n, b, n);
        g_tx_n += n;
    }
}

static void sock_open(void)
{
    int sv[2], big = 512 * 1024;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        fprintf(stderr, "engine: socketpair: %s\n", strerror(errno));
        return;
    }
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    fcntl(sv[0], F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &big, sizeof big);
#endif
    if (mr_adopt(sv[1]) != 0) {
        close(sv[0]);
        close(sv[1]);
        return;
    }
    g_sock = sv[0];
    g_tx_n = 0;
}

static void sock_close(void)
{
    if (g_sock >= 0)
        close(g_sock);
    g_sock = -1;
    g_tx_n = 0;
}

/* ------------------------------------------------------------------ the session's events --- */
static void route_midi(const rfm_move_act_t *a)
{
    int s;
    if (!a->n)
        return;
    if (a->track == 0) {   /* the pads played live, Control mode: the shown firmware */
        if (SL[g_active].on)
            fw_midi(&SL[g_active].fw, a->bytes, a->n, SL[g_active].fw.frames_done + a->frame);
        return;
    }
    for (s = 0; s < NSLOT; s++)   /* the sequencer's: every sounding firmware whose filter has the track */
        if (SL[s].on && (rfm_move_fw_sounding(&S->core) >> s & 1) && (SL[s].filt >> (a->track - 1) & 1))
            fw_midi(&SL[s].fw, a->bytes, a->n, SL[s].fw.frames_done + a->frame);
}

static void job_write(const char *path, const uint8_t *b, uint32_t n)
{
    st_job_t *j = st_job_new(J_WRITE);
    snprintf(j->path, sizeof j->path, "%s", path);
    if (n && (j->data = malloc(n))) {
        memcpy(j->data, b, n);
        j->len = n;
    }
    st_post(j);
}

static void on_action(const rfm1cs_event_t *ev)
{
    const rfm_move_act_t *a = ev->act;
    fw_t *fw = SL[g_active].on ? &SL[g_active].fw : NULL;
    switch (a->kind) {
    case RFM_MOVE_ACT_KEY:
        if (fw)
            fw_in(fw, FWI_KEY, a->index, a->down ? (a->vel ? a->vel : 100) : 0, 0, 0);
        break;
    case RFM_MOVE_ACT_PRESS:
        if (fw)
            fw_in(fw, FWI_LEAN, a->index, rfm_move_lean(a->value), 0, 0);
        break;
    case RFM_MOVE_ACT_BTN:
        if (fw)
            fw_in(fw, FWI_BTN, a->index, a->down, 0, 0);
        break;
    case RFM_MOVE_ACT_TURN:
        if (fw && a->value)
            fw_in(fw, FWI_ENC, a->index, a->value, 0, 0);
        break;
    case RFM_MOVE_ACT_MASTER: {   /* the volume knob: the master of every firmware */
        int s;
        g_master = a->value < 0 ? 0 : a->value > 1023 ? 1023 : a->value;
        for (s = 0; s < NSLOT; s++)
            if (SL[s].on)
                fw_in(&SL[s].fw, FWI_MASTER, 0, g_master, 0, 0);
        break;
    }
    case RFM_MOVE_ACT_MIDI:
        route_midi(a);
        break;
    case RFM_MOVE_ACT_SETTINGS: {
        char p[ST_PATH];
        st_path(p, sizeof p, "move/settings.bin");
        job_write(p, ev->data, ev->len);
        break;
    }
    case RFM_MOVE_ACT_FW_LIBRARY: {
        char p[ST_PATH];
        st_path(p, sizeof p, "move/library.rmvl");
        job_write(p, ev->data, ev->len);
        break;
    }
    case RFM_MOVE_ACT_PROJECT_SAVE:
    case RFM_MOVE_ACT_PROJECT_LOAD:
    case RFM_MOVE_ACT_PROJECT_DELETE:
    case RFM_MOVE_ACT_PROJECT_CUT:
    case RFM_MOVE_ACT_PROJECT_COPY:
    case RFM_MOVE_ACT_PROJECT_PASTE:
    case RFM_MOVE_ACT_PROJECT_COLOR: {
        st_job_t *j = st_job_new(J_PROJECT);
        j->act = a->kind, j->index = a->index, j->value = a->value;
        if (a->kind == RFM_MOVE_ACT_PROJECT_SAVE && ev->data && ev->len && (j->data = malloc(ev->len))) {
            memcpy(j->data, ev->data, ev->len);
            j->len = ev->len;
        }
        if (a->kind == RFM_MOVE_ACT_PROJECT_LOAD) {   /* (the project to open next time) */
            char p[ST_PATH], t[8];
            st_path(p, sizeof p, "move/open-project");
            snprintf(t, sizeof t, "%d\n", a->index);
            job_write(p, (const uint8_t *)t, (uint32_t)strlen(t));
        }
        st_post(j);
        break;
    }
    case RFM_MOVE_ACT_ROUTE: {
        int s;
        for (s = 0; s < NSLOT; s++)
            SL[s].filt = a->filt[s];
        break;
    }
    case RFM_MOVE_ACT_MIX:
        if (a->slot < NSLOT)
            SL[a->slot].level = a->mix[0] > 127 ? 127 : a->mix[0];
        break;
    case RFM_MOVE_ACT_FW_LEVEL:
        if (a->slot < NSLOT)
            SL[a->slot].level = (uint8_t)(a->value < 0 ? 0 : a->value > 127 ? 127 : a->value);
        break;
    case RFM_MOVE_ACT_FW_LOAD:
    case RFM_MOVE_ACT_FW_UNLOAD:
    case RFM_MOVE_ACT_FW_ACTIVE:
    case RFM_MOVE_ACT_FW_DOWNLOAD:
    case RFM_MOVE_ACT_STATE_SAVE:
    case RFM_MOVE_ACT_STATE_LOAD:
    case RFM_MOVE_ACT_STATE_DELETE:
        if (g_ndefer < (int)(sizeof g_defer / sizeof g_defer[0])) {
            defer_t *d = &g_defer[g_ndefer++];
            d->a = *a;
            d->a.data = NULL;
            snprintf(d->id, sizeof d->id, "%.*s", a->data ? (int)a->len : 0, a->data ? (const char *)a->data : "");
        }
        break;
    default:   /* CLICK, AUDIO_REC (no metronome or Sample loop yet), FX_* (no FX modules in phase 1) */
        break;
    }
}

static void on_event(void *ctx, const rfm1cs_event_t *ev)
{
    (void)ctx;
    switch (ev->type) {
    case RFM1CS_EV_SEND:
        sock_write(ev->data, ev->len);
        break;
    case RFM1CS_EV_OPEN:
        sock_close();
        sock_open();
        g_attempt = ev->attempt;
        g_open_pending = 1;
        break;
    case RFM1CS_EV_CLOSE:
        sock_close();
        break;
    case RFM1CS_EV_ACTION:
        on_action(ev);
        break;
    case RFM1CS_EV_RELEASED: {
        int s;
        for (s = 0; s < NSLOT; s++)
            if (SL[s].on)
                fw_in(&SL[s].fw, FWI_RELEASE, 0, 0, 0, 0);
        break;
    }
    case RFM1CS_EV_BYE:
    case RFM1CS_EV_LOST:   /* the exit gesture, the power off: save now, the process ends soon */
        fprintf(stderr, "engine: the Move %s (%d)\n", ev->type == RFM1CS_EV_BYE ? "said BYE" : "link was lost",
                ev->reason);
        if (ev->type == RFM1CS_EV_BYE || ev->reason == RFM1CS_WHY_SHUTDOWN)
            g_save_all = 1;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ slots --- */
static void slots_tell(void)
{
    const char *ids[NSLOT];
    int s;
    for (s = 0; s < NSLOT; s++)
        ids[s] = SL[s].on ? SL[s].fw.id : "";
    rfm_move_fw_slots(&S->core, ids);
}

static void view_info(void);

static char *state_json(const fw_t *fw)
{
    char *t = malloc(512);
    const int i = catalog_find(fw->id);
    if (t)
        snprintf(t, 512, "{\"id\":\"%s\",\"version\":\"%s\",\"sha256\":\"%s\",\"saved\":%ld,\"flash\":%u}\n", fw->id,
                 fw->version, i >= 0 ? g_fws[i].sha256 : "", (long)time(NULL), fw->flash_size);
    return t;
}

/* the firmware's flash now, to states/<id>/flash.bin (sync: first what it would save lazily) */
static void slot_save(int s, int sync)
{
    fw_t *fw = &SL[s].fw;
    const uint8_t *img;
    char p[ST_PATH];
    if (!SL[s].on || (fw->dead && !fw->halted))
        return;
    if (sync && !fw->dead)
        fw_in_now(fw, FWI_SYNC, 0, 0, 0);
    if (!(img = fw_flash(fw)))
        return;
    st_path(p, sizeof p, "states/%s/flash.bin", fw->id);
    {
        st_job_t *j = st_job_new(J_WRITE);
        snprintf(j->path, sizeof j->path, "%s", p);
        j->data = malloc(fw->flash_size);
        if (j->data) {
            memcpy(j->data, img, fw->flash_size);
            j->len = fw->flash_size;
        }
        j->text = state_json(fw);
        st_post(j);
    }
    SL[s].dirty = 0;
    SL[s].dirty_seen = fw_flash_dirty(fw);
}

static void raw_flush(void)   /* the test capture of the first instance: its blocks and its input log */
{
    if (!g_raw_fw)
        return;
    if (C.capture_raw && g_cap_raw)
        st_write(C.capture_raw, g_cap_raw, (size_t)g_cap_raw_n * 4u);
    if (C.log_inputs && g_raw_fw->log)
        st_write(C.log_inputs, g_raw_fw->log, (size_t)g_raw_fw->log_n * sizeof(fw_log_t));
    g_raw_fw = NULL;
}

static void slot_unload(int s, int save)
{
    if (!SL[s].on)
        return;
    if (save)
        slot_save(s, 1);
    if (g_raw_fw == &SL[s].fw)
        raw_flush();
    fw_destroy(&SL[s].fw);
    SL[s].on = 0;
}

static void active_changed(void)
{
    int k;
    fw_t *fw = SL[g_active].on ? &SL[g_active].fw : NULL;
    rfm_move_set_keymap(&S->core, fw && fw->has_keymap ? (const int8_t(*)[11])fw->keymap : NULL);
    for (k = 0; k <= RFM_MOVE_E_MASTER; k++)
        rfm_move_set_label(&S->core, k, NULL);
    if (fw)
        fw->lbl_epoch = 0;
    g_lcd_seen = 0;
    g_leds_ok = 0;
    if (!fw) {   /* nothing to show: a dark panel */
        static const uint8_t dark[FW_NLEVELS];
        memset(g_lcd_host, 0, sizeof g_lcd_host);
        rfm1cs_session_panel(S, dark, 0);
        rfm1cs_session_frame(S, now_ms(), g_lcd_host);
    }
    view_info();
}

/* a firmware into slot s (its object open, a saved flash or none) */
static int slot_boot(int s, fw_obj_t *obj, const uint8_t *flash, uint32_t n)
{
    slot_t *sl = &SL[s];
    int rc;
    slot_unload(s, 1);
    rc = fw_create(&sl->fw, obj);
    fw_obj_close(obj);   /* (the instance holds it) */
    if (rc != 0) {
        fprintf(stderr, "engine: slot %d: %s\n", s + 1, sl->fw.err);
        fw_destroy(&sl->fw);
        return -1;
    }
    if (!g_serial && (C.capture_raw || C.log_inputs)) {
        g_raw_fw = &sl->fw;
        fw_log_start(&sl->fw, 1u << 20);
    }
    if (fw_boot(&sl->fw, flash, n) != 0) {
        fprintf(stderr, "engine: slot %d: boot: %s\n", s + 1, sl->fw.err);
        if (g_raw_fw == &sl->fw)
            g_raw_fw = NULL;
        fw_destroy(&sl->fw);
        return -1;
    }
    sl->on = 1;
    sl->reboot = 0;
    sl->serial = ++g_serial;
    sl->idx = catalog_find(sl->fw.id);
    sl->level = (uint8_t)rfm_move_fw_level(&S->core, s);
    sl->dirty = 0;
    sl->dirty_seen = fw_flash_dirty(&sl->fw);
    sl->ns_mark = sl->fw.ns_total;
    if (g_master >= 0)
        fw_in_now(&sl->fw, FWI_MASTER, 0, g_master, 0);
    fprintf(stderr, "engine: slot %d: %s %s (%s flash)\n", s + 1, sl->fw.name, sl->fw.version,
            flash && n ? "its saved" : "a fresh");
    return 0;
}

/* at start (the engine's thread, before any sound): straight from the root, no worker */
static void slot_load_now(int s, const char *id)
{
    const int i = catalog_find(id);
    fw_obj_t *obj;
    char err[200], p[ST_PATH];
    uint8_t *img;
    size_t n = 0;
    if (i < 0)
        return;
    if (!(obj = fw_obj_open(g_fws[i].so, err, sizeof err))) {
        fprintf(stderr, "engine: %s: %s\n", id, err);
        return;
    }
    st_path(p, sizeof p, "states/%s/flash.bin", id);
    img = st_read(p, &n);
    slot_boot(s, obj, img, (uint32_t)n);
    free(img);
}

static void states_scan(int s)
{
    st_job_t *j;
    if (!SL[s].on)
        return;
    j = st_job_new(J_STATE_SCAN);
    j->slot = s;
    snprintf(j->id, sizeof j->id, "%s", SL[s].fw.id);
    st_post(j);
}

static void do_deferred(void)
{
    int k;
    for (k = 0; k < g_ndefer; k++) {
        const rfm_move_act_t *a = &g_defer[k].a;
        const char *id = g_defer[k].id;
        const int s = a->slot < NSLOT ? a->slot : 0;
        switch (a->kind) {
        case RFM_MOVE_ACT_FW_LOAD: {
            const int i = catalog_find(id);
            if (i < 0) {
                rfm_move_fw_progress(&S->core, id, RFM_MOVE_FW_FAILED);
                break;
            }
            if (SL[s].on && !strcmp(SL[s].fw.id, id)) {   /* there already */
                rfm_move_fw_progress(&S->core, id, 100);
                slots_tell();
                break;
            }
            rfm_move_fw_progress(&S->core, id, 10);
            {
                st_job_t *j = st_job_new(J_FW_OPEN);
                j->slot = s;
                snprintf(j->id, sizeof j->id, "%s", id);
                snprintf(j->path, sizeof j->path, "%s", g_fws[i].so);
                st_path(j->aux_path, sizeof j->aux_path, "states/%s/flash.bin", id);
                st_post(j);
            }
            break;
        }
        case RFM_MOVE_ACT_FW_UNLOAD:
            slot_unload(s, 1);
            slots_tell();
            if (s == g_active)
                active_changed();
            break;
        case RFM_MOVE_ACT_FW_ACTIVE:
            if (SL[g_active].on)
                fw_in(&SL[g_active].fw, FWI_RELEASE, 0, 0, 0, 0);
            g_active = s;
            active_changed();
            break;
        case RFM_MOVE_ACT_FW_DOWNLOAD:   /* (phase 2: the catalog) */
            rfm_move_fw_progress(&S->core, id, RFM_MOVE_FW_FAILED);
            break;
        case RFM_MOVE_ACT_STATE_SAVE:
            if (SL[s].on) {
                fw_t *fw = &SL[s].fw;
                const uint8_t *img;
                fw_in_now(fw, FWI_SYNC, 0, 0, 0);
                if ((img = fw_flash(fw))) {
                    st_job_t *j = st_job_new(J_STATE_SAVE);
                    j->slot = s, j->index = a->index;
                    snprintf(j->id, sizeof j->id, "%s", fw->id);
                    if ((j->data = malloc(fw->flash_size))) {
                        memcpy(j->data, img, fw->flash_size);
                        j->len = fw->flash_size;
                    }
                    j->text = state_json(fw);
                    st_post(j);
                }
            }
            break;
        case RFM_MOVE_ACT_STATE_LOAD:
        case RFM_MOVE_ACT_STATE_DELETE:
            if (SL[s].on) {
                st_job_t *j = st_job_new(a->kind == RFM_MOVE_ACT_STATE_LOAD ? J_STATE_LOAD : J_STATE_DELETE);
                j->slot = s, j->index = a->index;
                snprintf(j->id, sizeof j->id, "%s", SL[s].fw.id);
                st_post(j);
            }
            break;
        default:
            break;
        }
    }
    g_ndefer = 0;
}

static void do_results(void)
{
    st_job_t *j;
    int n = 0;
    while (n++ < 16 && (j = st_take())) {
        const int s = j->slot >= 0 && j->slot < NSLOT ? j->slot : 0;
        switch (j->kind) {
        case J_FW_OPEN:
            if (!j->obj) {
                fprintf(stderr, "engine: %s: %s\n", j->id, j->err);
                rfm_move_fw_progress(&S->core, j->id, RFM_MOVE_FW_FAILED);
                break;
            }
            if (slot_boot(s, j->obj, j->data, j->len) != 0) {
                rfm_move_fw_progress(&S->core, j->id, RFM_MOVE_FW_FAILED);
                slots_tell();
                break;
            }
            slots_tell();
            rfm_move_fw_progress(&S->core, j->id, 100);
            states_scan(s);
            if (s == g_active)
                active_changed();
            view_info();
            break;
        case J_STATE_SAVE:
        case J_STATE_DELETE:
        case J_STATE_SCAN:
            if (SL[s].on && !strcmp(SL[s].fw.id, j->id))
                rfm_move_fw_states(&S->core, s, j->used);
            break;
        case J_STATE_LOAD:
            if (SL[s].on && !strcmp(SL[s].fw.id, j->id) && j->data) {
                fw_obj_t *obj = SL[s].fw.obj;
                fw_obj_ref(obj);
                slot_unload(s, 0);   /* (the state replaces what was there: its flash.bin follows on the next save) */
                if (slot_boot(s, obj, j->data, j->len) == 0) {
                    SL[s].dirty = 1, SL[s].dirty_ms = now_ms();
                    if (s == g_active)
                        active_changed();
                }
                slots_tell();
            }
            break;
        case R_PROJECT_LOADED:
            if (rfm1cs_session_project_answer(S, now_ms(), j->index, j->data, j->len) < 0)
                rfm1cs_session_project_answer(S, now_ms(), j->index, NULL, 0);
            break;
        case R_PROJECT_STORED:
            rfm_move_project_dir(&S->core, j->used);
            rfm_move_project_info(&S->core, j->index, j->data, j->len);
            break;
        default:
            break;
        }
        st_job_free(j);
        do_deferred();
    }
}

/* ------------------------------------------------------------------ the web's view --- */
static void view_info(void)
{
    fw_t *fw = SL[g_active].on ? &SL[g_active].fw : NULL;
    char tmp[2048];
    int n = 0, s, k;
    n += snprintf(tmp + n, sizeof tmp - (size_t)n, "{\"active\":%d,\"slots\":[", g_active);
    for (s = 0; s < NSLOT; s++) {
        if (SL[s].on) {
            const int i = SL[s].idx;
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, "%s{\"id\":\"%s\",\"name\":\"%s\",\"version\":\"%s\",\"license\":\"%s\"}",
                          s ? "," : "", SL[s].fw.id, SL[s].fw.name, SL[s].fw.version, i >= 0 ? g_fws[i].license : "");
        } else
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, "%snull", s ? "," : "");
    }
    n += snprintf(tmp + n, sizeof tmp - (size_t)n, "]");
    if (fw) {
        uint8_t btn_id[FW_NBTN], enc_id[FW_NENC];
        int8_t enc_dir[FW_NENC];
        char esc[64];
        fw_panel_ids(fw, btn_id, enc_id, enc_dir);
        n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"buttons\":[");
        for (k = 0; k < FW_NBTN; k++) {
            mj_escape(fw->btn_name[k], esc, sizeof esc);
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, "%s[\"%s\",%u]", k ? "," : "", esc, btn_id[k]);
        }
        n += snprintf(tmp + n, sizeof tmp - (size_t)n, "],\"enc\":[");
        for (k = 0; k < FW_NENC; k++)
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, "%s[%u,%d]", k ? "," : "", enc_id[k], enc_dir[k]);
        n += snprintf(tmp + n, sizeof tmp - (size_t)n, "],\"keymap\":[");
        for (k = 0; k < 66; k++)
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, "%s%d", k ? "," : "",
                          fw->has_keymap ? fw->keymap[k / 11][k % 11] : -1);
        n += snprintf(tmp + n, sizeof tmp - (size_t)n, "]");
    }
    snprintf(tmp + n, sizeof tmp - (size_t)n, "}");
    pthread_mutex_lock(&g_vmu);
    snprintf(g_view.info, sizeof g_view.info, "%s", tmp);
    g_view.info_seq++;
    g_view.seq++;
    pthread_mutex_unlock(&g_vmu);
}

int engine_view(engine_view_t *v, uint32_t *seq)
{
    int r = 0;
    pthread_mutex_lock(&g_vmu);
    if (g_view.seq != *seq) {
        memcpy(v, &g_view, sizeof *v);
        *seq = g_view.seq;
        r = 1;
    }
    pthread_mutex_unlock(&g_vmu);
    return r;
}

void engine_panel(int op, int a, int b)
{
    pthread_mutex_lock(&g_vmu);
    if (g_npin < (int)(sizeof g_pin / sizeof g_pin[0])) {
        g_pin[g_npin].op = op, g_pin[g_npin].a = a, g_pin[g_npin].b = b;
        g_npin++;
    }
    pthread_mutex_unlock(&g_vmu);
}

int engine_status(char *out, int cap)
{
    int n;
    pthread_mutex_lock(&g_vmu);
    n = snprintf(out, (size_t)cap, "%s", g_status);
    pthread_mutex_unlock(&g_vmu);
    return n;
}

/* the active firmware's screen and LEDs: to the session (the Move) and to the page */
static void publish(uint32_t now)
{
    fw_t *fw = SL[g_active].on ? &SL[g_active].fw : NULL;
    uint8_t leds[FW_NLEVELS + 1];
    uint32_t w;
    int changed = 0;
    if (!fw || fw->dead)
        return;
    memcpy(leds, fw_leds(fw), FW_NLEVELS);
    leds[FW_NLEVELS] = fw_play_green(fw);
    if (!g_leds_ok || memcmp(leds, g_leds_seen, sizeof leds)) {
        memcpy(g_leds_seen, leds, sizeof leds);
        g_leds_ok = 1;
        rfm1cs_session_panel(S, leds, leds[FW_NLEVELS]);
        changed = 1;
    }
    w = fw_lcd_writes(fw);
    if ((w != g_lcd_seen || !g_lcd_seen) && now - g_last_screen_ms >= RFM1CS_SCREEN_MS) {
        const uint8_t *be = fw_lcd(fw);
        int i;
        g_lcd_seen = w ? w : 1;
        g_last_screen_ms = now;
        for (i = 0; i < 240 * 240; i++)
            g_lcd_host[i] = (uint16_t)(be[2 * i] << 8 | be[2 * i + 1]);
        rfm1cs_session_frame(S, now, g_lcd_host);
        pthread_mutex_lock(&g_vmu);
        memcpy(g_view.lcd, be, ENG_LCD_BYTES);
        g_view.lcd_seq++;
        g_view.seq++;
        g_view_lcd_ms = now;
        pthread_mutex_unlock(&g_vmu);
    }
    if (changed) {
        pthread_mutex_lock(&g_vmu);
        memcpy(g_view.leds, leds, FW_NLEVELS);
        g_view.play_green = leds[FW_NLEVELS];
        g_view.seq++;
        pthread_mutex_unlock(&g_vmu);
    }
    {   /* live labels: the knobs' names on the Move's screen */
        const uint32_t e = fw_labels_epoch(fw);
        if (e && e != fw->lbl_epoch) {
            /* FM1CORE_LBL_KNOB0 + role (SELECT ALGO PRESETS K1..K4 MASTER) -> rfm1cs's encoders */
            static const int ENC[8] = { RFM_MOVE_E_SELECT, RFM_MOVE_E_ALGO, RFM_MOVE_E_PRESETS, RFM_MOVE_E_K1,
                                        RFM_MOVE_E_K2, RFM_MOVE_E_K3, RFM_MOVE_E_K4, RFM_MOVE_E_MASTER };
            int r;
            fw->lbl_epoch = e;
            for (r = 0; r < 8; r++)
                rfm_move_set_label(&S->core, ENC[r], fw_label(fw, r));
            rfm1cs_session_redraw(S, now);
        }
    }
}

/* ------------------------------------------------------------------ the clock --- */
static void render_block(uint32_t now)
{
    int16_t blk[FW_BLOCK * 2], out[FW_BLOCK * 2];
    int s, sounding;
    rfm1cs_session_advance(S, now, FW_BLOCK, FW_FS);   /* the sequencer: its MIDI stamped within this block */
    do_deferred();
    sounding = rfm_move_fw_sounding(&S->core);
    mix_clear(&g_mix, FW_BLOCK);
    for (s = 0; s < NSLOT; s++) {
        slot_t *sl = &SL[s];
        if (!sl->on)
            continue;
        if (!(sounding >> s & 1))
            continue;   /* loaded, past the voices: its clock waits */
        fw_render(&sl->fw, blk);
        if (&sl->fw == g_raw_fw && g_cap_raw && g_cap_raw_n + FW_BLOCK <= g_cap_max) {
            memcpy(g_cap_raw + 2u * g_cap_raw_n, blk, sizeof blk);
            g_cap_raw_n += FW_BLOCK;
        }
        mix_add(&g_mix, blk, FW_BLOCK, sl->level);
        if (s == g_active) {   /* its MIDI out: the clock the sequencer can follow */
            uint32_t pk[16];
            const int k = fw_midi_out(&sl->fw, pk, 16);
            int i;
            for (i = 0; i < k; i++) {
                const uint8_t b = (uint8_t)(pk[i] >> 8);
                if (b == 0xF8 || b == 0xFA || b == 0xFB || b == 0xFC) {
                    rfm1cs_session_midi_in(S, now, &b, 1);
                } else if (b == 0xF2) {
                    const uint8_t m[3] = { b, (uint8_t)(pk[i] >> 16), (uint8_t)(pk[i] >> 24) };
                    rfm1cs_session_midi_in(S, now, m, 3);
                }
            }
            do_deferred();
        }
    }
    mix_out(&g_mix, out, FW_BLOCK);
    for (s = 0; s < FW_BLOCK * 2; s++) {
        const int v = out[s] < 0 ? -out[s] : out[s];
        if (v > g_peak_acc)
            g_peak_acc = v;
    }
    mr_ring_push(&g_ring, out, FW_BLOCK);
    if (g_cap_mix && g_cap_mix_n + FW_BLOCK <= g_cap_max) {
        memcpy(g_cap_mix + 2u * g_cap_mix_n, out, sizeof out);
        g_cap_mix_n += FW_BLOCK;
    }
    g_blocks++;
}

static void housekeeping(uint32_t now)
{
    static uint32_t last_save_check, last_stat;
    int s;
    if (now - last_save_check >= 250u) {   /* the flash: saved once it has been quiet a while */
        last_save_check = now;
        for (s = 0; s < NSLOT; s++) {
            slot_t *sl = &SL[s];
            uint32_t d;
            if (sl->on && sl->fw.dead && sl->fw.halted && !sl->reboot) {
                /* the firmware rebooted (or exited): power it on again, from its flash as last saved */
                const int i = catalog_find(sl->fw.id);
                fprintf(stderr, "engine: slot %d halted (0x%x): powering it on again\n", s + 1, sl->fw.halted);
                sl->reboot = 1;
                slot_save(s, 0);   /* (the worker writes it before it reads it back: one queue, in order) */
                if (i >= 0) {
                    st_job_t *j = st_job_new(J_FW_OPEN);
                    j->slot = s;
                    snprintf(j->id, sizeof j->id, "%s", sl->fw.id);
                    snprintf(j->path, sizeof j->path, "%s", g_fws[i].so);
                    st_path(j->aux_path, sizeof j->aux_path, "states/%s/flash.bin", sl->fw.id);
                    st_post(j);
                }
            }
            if (!sl->on || sl->fw.dead)
                continue;
            d = fw_flash_dirty(&sl->fw);
            if (d != sl->dirty_seen) {
                sl->dirty_seen = d;
                sl->dirty = 1;
                sl->dirty_ms = now;
            } else if (sl->dirty && now - sl->dirty_ms >= AUTOSAVE_MS)
                slot_save(s, 0);

        }
    }
    if (now - last_stat >= 1000u) {   /* the status: CPU per slot over the last second */
        static uint32_t last_late;
        char tmp[1024];
        int n;
        const uint32_t dt = last_stat ? now - last_stat : 1000u;
        last_stat = now;
        g_peak = g_peak_acc;
        g_peak_acc = 0;
        n = snprintf(tmp, sizeof tmp, "{\"blocks\":%u,\"late\":%u,\"rt_blocks\":%u,\"ring\":%u,\"limited\":%u,"
                     "\"peak\":%d,\"session\":%d,\"mode\":%d,\"inputs\":%u,\"led_writes\":%u,\"screens\":%u,"
                     "\"active\":%d,\"master\":%d,\"slots\":[",
                     g_blocks, g_late, g_rt_blocks, mr_ring_count(&g_ring), g_mix.limited, g_peak, S->st.state,
                     S->st.mode, S->st.inputs, S->st.led_writes, S->st.screens, g_active, g_master);
        for (s = 0; s < NSLOT; s++) {
            slot_t *sl = &SL[s];
            if (sl->on) {
                const uint64_t ns = sl->fw.ns_total - sl->ns_mark;
                sl->ns_mark = sl->fw.ns_total;
                sl->cpu_permille = (uint32_t)(ns / 1000u / (dt ? dt : 1));   /* us per ms = permille */
                n += snprintf(tmp + n, sizeof tmp - (size_t)n,
                              "%s{\"id\":\"%s\",\"cpu\":%.1f,\"worst_us\":%u,\"level\":%u,\"boot\":%u,"
                              "\"states\":%u,\"dead\":%d}", s ? "," : "", sl->fw.id, sl->cpu_permille / 10.0,
                              sl->fw.worst_us, sl->level, sl->serial, S->core.fw.st_used[s], sl->fw.dead);
            } else
                n += snprintf(tmp + n, sizeof tmp - (size_t)n, "%snull", s ? "," : "");
        }
        n += snprintf(tmp + n, sizeof tmp - (size_t)n, "]");
        for (s = 0; s < NSLOT; s++)   /* (flat, for scripts) */
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"id%d\":\"%s\",\"boot%d\":%u,\"states%d\":%u", s,
                          SL[s].on ? SL[s].fw.id : "", s, SL[s].on ? SL[s].serial : 0, s, S->core.fw.st_used[s]);
        snprintf(tmp + n, sizeof tmp - (size_t)n, "}");
        pthread_mutex_lock(&g_vmu);
        memcpy(g_status, tmp, sizeof tmp);
        pthread_mutex_unlock(&g_vmu);
        if (g_late != last_late)
            fprintf(stderr, "engine: %u late block(s) so far\n", g_late);
        last_late = g_late;
    }
}

static void *engine_main(void *arg)
{
    const uint32_t lead = (uint32_t)(C.lead_blocks < 4 ? 4 : C.lead_blocks > 8 ? 8 : C.lead_blocks) * FW_BLOCK;
    uint8_t buf[16384];
    (void)arg;
#ifdef __linux__
    pthread_setname_np(pthread_self(), "rfm1-engine");
    if (C.cpu_mask) {
        cpu_set_t set;
        int c;
        CPU_ZERO(&set);
        for (c = 0; c < 8; c++)
            if (C.cpu_mask >> c & 1)
                CPU_SET(c, &set);
        sched_setaffinity(0, sizeof set, &set);
    }
#endif
    wasm_rt_init();
    while (g_run) {
        const uint32_t now = now_ms();
        int k = 0;
        if (!g_session_on && (int32_t)(now - g_session_at) >= 0) {
            rfm1cs_session_start(S, now);
            g_session_on = 1;
        }
        if (g_open_pending) {   /* (EV_OPEN came from inside a session call) */
            g_open_pending = 0;
            if (g_sock >= 0)
                rfm1cs_session_opened(S, now, g_attempt);
            else
                rfm1cs_session_closed(S, now, g_attempt, 0);
        }
        while (g_sock >= 0 && k++ < 8) {   /* Move Remote's bytes: the Move's input, its STATUS */
            const ssize_t r = recv(g_sock, buf, sizeof buf, 0);
            if (r > 0)
                rfm1cs_session_rx(S, now, g_attempt, buf, (uint32_t)r);
            else {
                if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    sock_close();
                    rfm1cs_session_closed(S, now, g_attempt, 0);
                }
                break;
            }
        }
        do_deferred();
        do_results();
        {   /* the page's panel input */
            struct { int op, a, b; } pin[256];
            int n, i;
            pthread_mutex_lock(&g_vmu);
            n = g_npin;
            memcpy(pin, g_pin, sizeof pin[0] * (size_t)n);
            g_npin = 0;
            pthread_mutex_unlock(&g_vmu);
            for (i = 0; i < n; i++) {
                if (pin[i].op == FWI_MASTER) {
                    rfm_move_act_t a;
                    rfm1cs_event_t ev;
                    memset(&a, 0, sizeof a);
                    memset(&ev, 0, sizeof ev);
                    a.kind = RFM_MOVE_ACT_MASTER, a.value = (int16_t)pin[i].b;
                    ev.type = RFM1CS_EV_ACTION, ev.act = &a;
                    on_action(&ev);
                    rfm_move_set_master(&S->core, g_master);
                } else if (SL[g_active].on)
                    fw_in(&SL[g_active].fw, pin[i].op, pin[i].a, pin[i].b, 0, 0);
            }
        }
        rfm1cs_session_tick(S, now);
        do_deferred();
        while (mr_ring_count(&g_ring) < lead && g_run)
            render_block(now_ms());
        publish(now);
        do_deferred();
        housekeeping(now);
        if (g_save_all) {
            int s;
            g_save_all = 0;
            for (s = 0; s < NSLOT; s++)
                slot_save(s, 1);
            rfm1cs_session_flush(S);
        }
        sock_flush();
        {   /* wait: for the Move's bytes, or a millisecond */
            struct pollfd pf = { g_sock, POLLIN, 0 };
            poll(&pf, g_sock >= 0 ? 1 : 0, 1);
        }
    }
    /* the end: every firmware's flash and the project saved, the captures written */
    {
        int s;
        rfm1cs_session_flush(S);
        do_deferred();
        for (s = 0; s < NSLOT; s++)
            slot_unload(s, 1);
        raw_flush();
        rfm1cs_session_stop(S);
        sock_close();
    }
    return NULL;
}

/* ------------------------------------------------------------------ start, stop --- */
int engine_start(const engine_cfg_t *cfg)
{
    uint8_t *settings, *lib, *blob;
    size_t ns = 0, nl = 0;
    uint32_t nb = 0, used;
    int open_slot = 0, s;
    char p[ST_PATH];
    C = *cfg;
    if (st_init(C.root) != 0) {
        fprintf(stderr, "engine: cannot make %s\n", C.root);
        return -1;
    }
    if (st_fw_install_bundled(C.bundled) > 0)
        fprintf(stderr, "engine: bundled firmwares installed into %s/firmwares\n", C.root);
    catalog_load();
    if (mr_ring_init(&g_ring, 4, RING_FRAMES) != 0 || !(g_tx = malloc(TX_CAP)) || !(S = calloc(1, sizeof *S)))
        return -1;
    if (C.capture_seconds) {
        g_cap_max = C.capture_seconds * FW_FS;
        if (C.capture_out)
            g_cap_out = calloc(g_cap_max, 4);
        if (C.capture_mix)
            g_cap_mix = calloc(g_cap_max, 4);
        if (C.capture_raw)
            g_cap_raw = calloc(g_cap_max, 4);
    }
    /* rfm1cs: its settings, the library, the catalog, the projects */
    st_path(p, sizeof p, "move/settings.bin");
    settings = st_read(p, &ns);
    rfm1cs_session_init(S, settings, (uint32_t)ns, "RemoteFM1 on Move", on_event, NULL);
    free(settings);
    rfm1cs_session_give_up(S, -1);
    st_path(p, sizeof p, "move/library.rmvl");
    lib = st_read(p, &nl);
    if (rfm_move_fw_library(&S->core, lib, (uint32_t)nl) < 0)
        fprintf(stderr, "engine: %s is not a library; starting empty\n", p);
    free(lib);
    rfm_move_fw_list(&S->core, g_list, g_nfw);
    rfm_move_fw_voices(&S->core, NSLOT);
    rfm_move_set_master(&S->core, 768);
    {
        size_t n;
        uint8_t *t;
        st_path(p, sizeof p, "move/open-project");
        if ((t = st_read(p, &n))) {
            open_slot = atoi((char *)t);
            if (open_slot < 0 || open_slot >= RFM_MOVE_PROJECTS)
                open_slot = 0;
            free(t);
        }
    }
    used = st_projects_scan(open_slot, &blob, &nb);
    rfm1cs_session_projects_init(S, now_ms(), used, open_slot, blob, nb);
    free(blob);
    if (st_worker_start(used) != 0)
        return -1;
    for (s = 0; s < NSLOT; s++)
        SL[s].filt = 0xFFFF, SL[s].level = 127;
    g_active = rfm_move_fw_active(&S->core);
    /* the firmwares the library had on its slots; none: the default on slot 1 */
    {
        int any = 0;
        for (s = 0; s < NSLOT; s++)
            if (S->core.fw.slot_id[s][0] && catalog_find(S->core.fw.slot_id[s]) >= 0) {
                slot_load_now(s, S->core.fw.slot_id[s]);
                any |= SL[s].on;
            }
        if (!any && g_nfw > 0) {
            const char *id = C.default_fw && catalog_find(C.default_fw) >= 0 ? C.default_fw : g_fws[0].id;
            g_active = 0;
            slot_load_now(0, id);
        }
    }
    slots_tell();
    for (s = 0; s < NSLOT; s++)
        states_scan(s);
    {   /* the active slot (the library's, or the loaded one) */
        rfm_move_act_t a;
        memset(&a, 0, sizeof a);
        if (!SL[g_active].on)
            for (s = 0; s < NSLOT; s++)
                if (SL[s].on) {
                    g_active = s;
                    break;
                }
    }
    active_changed();
    g_session_at = now_ms() + C.splash_ms;
    if (!C.splash_ms) {
        rfm1cs_session_start(S, now_ms());   /* -> EV_OPEN: the socketpair, adopted by Move Remote's core */
        g_session_on = 1;
    }
    g_run = 1;
    if (pthread_create(&g_thr, NULL, engine_main, NULL) != 0)
        return -1;
    g_started = 1;
    return 0;
}

void engine_stop(void)
{
    if (!g_started)
        return;
    g_run = 0;
    pthread_join(g_thr, NULL);
    st_worker_stop();
    if (C.capture_out && g_cap_out)
        st_write(C.capture_out, g_cap_out, (size_t)g_cap_out_n * 4u);
    if (C.capture_mix && g_cap_mix)
        st_write(C.capture_mix, g_cap_mix, (size_t)g_cap_mix_n * 4u);
    {
        char tmp[1024];
        engine_status(tmp, sizeof tmp);
        fprintf(stderr, "engine: stopped: %s\n", tmp);
    }
    g_started = 0;
}
