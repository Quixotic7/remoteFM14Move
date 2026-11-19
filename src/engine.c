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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../third_party/abletonmoveremote/common/ring.h"
#include "catalog.h"
#include "engine_dsp.h"
#include "fm1link.h"
#include "fsvc.h"
#include "fwrt.h"
#include "fx.h"
#include "minijson.h"
#include "mr_core.h"
#include "rfm1cs/session.h"
#include "storage.h"
#include "wasm-rt.h"

#define NSLOT RFM_MOVE_FW_SLOTS
#define RING_FRAMES 4096
#define AUTOSAVE_MS 2000u      /* a firmware's flash saved this long after its last change */
#define PREROLL_FRAMES (RING_FRAMES - 4 * FW_BLOCK)   /* the ring's fill before a firmware starts (~81 ms) */
#define INDEX_EVERY_MS (6u * 3600u * 1000u)           /* the catalog's index: fetched again this often */
#define INDEX_RETRY_MS (10u * 60u * 1000u)            /* .. and this long after a failure (offline) */
#define LOG_CAP_DEFAULT (1024 * 1024)                 /* the log file: moved aside past this (two at most) */

static engine_cfg_t C;
static volatile int g_run;
static pthread_t g_thr;
static int g_started;

/* ------------------------------------------------------------------ the ring and the audio thread's side --- */
static mr_ring_t g_ring;                     /* stereo int16 frames: engine -> audio thread */
static volatile uint32_t g_late, g_rt_blocks, g_rt_started;
/* the output opens when the session starts (after --splash): until then the Move is still settling (Move Remote's
 * mlockall pages in the whole process, the FX probes and loads, the firmwares' boot) and nothing can be played yet,
 * so the audio thread gives silence and nothing counts as late */
static volatile int g_out_open;
static int16_t *g_cap_out, *g_cap_mix, *g_cap_raw;
static volatile uint32_t g_cap_out_n;
static uint32_t g_cap_mix_n, g_cap_raw_n, g_cap_max;

int engine_audio(void *ctx, int16_t *out, int frames)
{
    (void)ctx;
    if (!__atomic_load_n(&g_out_open, __ATOMIC_ACQUIRE))
        return 0;
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

/* ---- ain1 (FM1VST docs/VIRTUAL-CARTS.md section 8): the Move's input (Move Remote's mr_set_audio_in_sink: the line
 * in, else the mic) from the audio thread to the engine through a lock-free ring. The audio thread pushes one input
 * block for each output block it pops, and the engine pops one for each it renders, so the input queued plus the
 * output queued stays the same: the engine keeps it at AIN_TOTAL (dropping the input it does not need, or giving
 * silence while it has too little: a firmware's start fills the output ring past the lead). An input frame is
 * therefore heard (in the cart's output) AIN_TOTAL frames after it came in, plus the SPI's own block each way:
 * engine_ain_latency(), the round trip each cart with ain1 is told (rfm1_audio_in_info) */
#define AIN_RING 4096
static mr_ring_t g_ain;
static volatile uint32_t g_ain_in, g_ain_over;   /* (the audio thread's: blocks in, blocks dropped: the ring full) */
static uint32_t g_ain_under, g_ain_dropped, g_lead = 5 * FW_BLOCK;
static uint32_t g_ain_seen, g_ain_seen_ms;       /* (the engine's: g_ain_in when last looked, and when it moved) */
static int g_ain_live;                           /* input is arriving (the sink is called) */
static int16_t g_ain_blk[FW_BLOCK * 2];
#define AIN_TOTAL (g_lead + 2u * FW_BLOCK)
uint32_t engine_ain_latency(void) { return AIN_TOTAL + 2u * FW_BLOCK; }

void engine_audio_in(void *ctx, const int16_t *in, int frames)
{
    (void)ctx;
    if (!g_ain.buf || frames != FW_BLOCK)
        return;
    if (mr_ring_push(&g_ain, in, (uint32_t)frames) != (uint32_t)frames)
        g_ain_over++;
    g_ain_in++;
}

/* the input block for the block rendered now (NULL: no input arrives) */
static const int16_t *ain_take(uint32_t now)
{
    uint32_t ic, oc;
    if (g_ain_in != g_ain_seen)
        g_ain_seen = g_ain_in, g_ain_seen_ms = now, g_ain_live = 1;
    else if (g_ain_live && now - g_ain_seen_ms > 500u)
        g_ain_live = 0;   /* (no input for half a second: the platform gives none, or the audio thread stopped) */
    if (!g_ain_live)
        return NULL;
    ic = mr_ring_count(&g_ain);
    oc = mr_ring_count(&g_ring);
    if (ic + oc > AIN_TOTAL + FW_BLOCK) {   /* (more than the latency needs: the oldest goes) */
        const uint32_t drop = ic + oc - AIN_TOTAL - FW_BLOCK < ic ? ic + oc - AIN_TOTAL - FW_BLOCK : ic;
        mr_ring_pop(&g_ain, NULL, drop);
        g_ain_dropped += drop;
        ic -= drop;
    }
    if (ic >= FW_BLOCK)
        mr_ring_pop(&g_ain, g_ain_blk, FW_BLOCK);
    else {
        memset(g_ain_blk, 0, sizeof g_ain_blk);
        g_ain_under++;
    }
    return g_ain_blk;
}

/* ------------------------------------------------------------------ state (the engine thread's) --- */
typedef struct {
    fw_t fw;
    int on;                    /* a firmware runs here */
    int idx;                   /* its index in the installed firmwares (g_cat->fw) */
    char so[ST_PATH];          /* the object it runs (an update installs another beside it) */
    uint8_t level;             /* 0..127 */
    uint16_t filt;             /* the sequencer tracks it hears (ROUTE) */
    uint32_t dirty_seen;       /* flash_dirty when last looked at */
    uint32_t dirty_ms;         /* .. when it moved (0: saved) */
    int dirty;
    uint64_t ns_mark;          /* the CPU window's start */
    uint32_t cpu_permille;     /* of real time, over the last window */
    uint32_t serial;           /* which instance (the raw capture follows the first) */
    int reboot;                /* a power-on asked after the firmware halted */
    uint8_t pan;               /* 0..127, 64 centre (MIX) */
    uint8_t send[3];           /* to audio FX 1..3, 0..127 (MIX) */
    uint32_t notes;            /* note-ons it was given (the status: tests) */
    int peak;                  /* this meter window's, after level and pan */
    uint32_t mini_writes, mini_ms;   /* the page's mini screen: the LCD writes it was made from, when */
} slot_t;

static slot_t SL[NSLOT];
static int g_active;
static int g_master = -1;                    /* the volume knob's MASTER, -1 not touched yet */
static uint32_t g_serial;
static fw_t *g_raw_fw;                       /* the instance the raw capture and the input log follow */

/* the catalog (catalog.h): the core's list (g_cat->list), the installed firmwares (g_cat->fw); the engine's thread's */
static cat_t *g_cat;
#define g_fws (g_cat->fw)
#define g_nfw (g_cat->nfw)
static int g_dl_busy, g_index_busy, g_sweep;
static uint32_t g_next_index_ms;
static struct { char id[RFM_MOVE_FW_ID_MAX], err[300]; int pct, state; uint32_t ok, failed, index_ok, index_failed; }
    g_dl;                                    /* (the status: state 0 none, 1 downloading, 2 installed, 3 failed) */
static char g_index_err[300];
static int g_log_cap, g_rotating;
static uint32_t g_test_boot_ms;              /* tests: a slow firmware start, as on the Move (RFM14_TEST_BOOT_MS) */
static int g_no_preroll;                     /* tests: the start without the ring filled first (RFM14_NO_PREROLL) */

/* ---- Schwung FX (fx.h): 3 MIDI FX in series on the sequencer's MIDI, 3 audio FX on send / return buses */
typedef struct {
    fx_inst_t *run;            /* the instance running (the engine's), NULL none */
    int want;                  /* the module asked for (FX_LOAD), -1 none */
    uint32_t gen;              /* .. its open's generation (a later FX_LOAD drops an earlier open) */
    int opening;               /* an open is under way */
    uint8_t val[RFM_MOVE_FX_PARAMS];   /* FX_PARAM's values (applied to an instance as it comes in) */
    uint16_t filt;             /* MIDI FX: the tracks it acts on (ROUTE) */
    int last_track;            /* MIDI FX: the track of the last message it took (its generated notes' track) */
    int last_live;             /* MIDI FX: that message came from the pads played live (its notes go to the active slot) */
    uint64_t ns_mark;
    uint32_t cpu_permille, over;
    int peak;                  /* audio FX: its return's peak, this meter window */
    int rpeak, rpeak_last;     /* .. this second's, the last second's (the status) */
    char err[120];             /* why the last load was refused ("" none) */
    char pend_name[RFM_MOVE_FX_NAME];   /* a project's saved module state, for when that module is loaded here */
    char *pend_state;
} fxs_t;
static fx_lib_t *g_fxlib;
static fxs_t FX[FX_KINDS][RFM_MOVE_FX_SLOTS];
static bus_t g_bus[RFM_MOVE_FX_SLOTS];
static int g_fx_budget;
static double g_beat;
static uint32_t g_mix_ver, g_mix_made = ~0u;
static char *g_fx_json;
static int g_peak_win;
static struct { uint8_t track, b[3]; } g_tmidi[32];
static int g_ntmidi;

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
static char g_status[8192];
static char *g_fw_json;                      /* engine_firmwares: made whenever the installed ones change */
static char (*g_web_ids)[RFM_MOVE_FW_ID_MAX];   /* the installed ids (engine_load's check) */
static int g_web_n;
static struct { int slot, activate; char id[RFM_MOVE_FW_ID_MAX]; } g_wload[8];   /* the page's loads (engine_load) */
static int g_activate = -1;                  /* a page's load that makes its slot the active one once it runs */
static int g_nwload;
static char g_fail_id[RFM_MOVE_FW_ID_MAX];   /* the last firmware that failed to load, and how many failed so far */
static uint32_t g_fail_seq;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/* ------------------------------------------------------------------ the catalog --- */
static int has_file(const char *json_path, const char *suffix)
{
    char p[ST_PATH + 32];
    const size_t n = strlen(json_path);
    struct stat st;
    if (n < 5)
        return 0;
    snprintf(p, sizeof p, "%.*s%s", (int)(n - 5), json_path, suffix);
    return stat(p, &st) == 0;
}

/* the page's firmware list (engine_firmwares): each installed firmware's manifest fields, its file's stem (base: the
 * page asks for its skin and licences by it, web.c /api/fw/), the catalog's description and categories (rfm1cs's),
 * whether it has a skin */
static void fw_json(void)
{
    static const char *CATS[] = { "Synth", "Groovebox", "Chords", "Drums", "Other" };
    size_t cap = 1024 + (size_t)g_nfw * 12288, n = 0;   /* (a firmware: at most ~10 KB below) */
    char *o = malloc(cap);
    char (*ids)[RFM_MOVE_FW_ID_MAX] = calloc((size_t)g_nfw + 1, sizeof *ids);
    int i, k;
    if (!o || !ids) {
        free(o), free(ids);
        return;
    }
    n += (size_t)snprintf(o + n, cap - n, "{\"firmwares\":[");
    for (i = 0; i < g_nfw; i++) {
        const st_fw_t *f = &g_fws[i];
        size_t jn;
        char *json = (char *)st_read(f->json, &jn), esc[4][600], desc[CAT_DESC * 2], file[256] = "", built[200] = "";
        const char *man = json ? mj_find(json, "manifest") : NULL, *au = man ? mj_find(man, "authors") : NULL;
        const char *slash = strrchr(f->json, '/');
        const size_t span = au && *au == '[' ? mj_span(au) : 0;
        const int e = cat_find(g_cat, f->id);
        const uint8_t cats = e >= 0 ? g_cat->e[e].cats : 0;
        int first = 1;
        snprintf(ids[i], sizeof ids[i], "%s", f->id);
        if (json)
            mj_string(json, "file", file, sizeof file);
        if (man)
            mj_string(man, "built_by", built, sizeof built);
        mj_escape(f->name, esc[0], sizeof esc[0]);
        mj_escape(f->license, esc[1], sizeof esc[1]);
        mj_escape(f->source_url, esc[2], sizeof esc[2]);
        mj_escape(built, esc[3], sizeof esc[3]);
        mj_escape(e >= 0 && g_cat->e[e].indexed ? g_cat->e[e].desc : "", desc, sizeof desc);
        n += (size_t)snprintf(o + n, cap - n,
                              "%s{\"id\":\"%s\",\"name\":\"%s\",\"version\":\"%s\",\"license\":\"%s\",\"source_url\":\"%s\","
                              "\"sha256\":\"%s\",\"file\":\"%s\",\"base\":\"%.*s\",\"built_by\":\"%s\",\"authors\":%.*s,"
                              "\"skin\":[%s%s%s],\"licenses\":%s,%s\"description\":\"%s\",\"categories\":[",
                              i ? "," : "", f->id, esc[0], f->version, esc[1], esc[2], f->sha256, file,
                              slash ? (int)strlen(slash + 1) - 5 : 0, slash ? slash + 1 : "", esc[3],
                              span && span < 2048 ? (int)span : 2, span && span < 2048 ? au : "[]",
                              has_file(f->json, ".rfm1skin") ? "\"rfm1skin\"" : "",
                              has_file(f->json, ".rfm1skin") && has_file(f->json, ".rfm1skin.bg") ? ",\"rfm1skin.bg\"" : "",
                              has_file(f->json, ".rfm1skin") && has_file(f->json, ".rfm1skin.bgl") ? ",\"rfm1skin.bgl\"" : "",
                              has_file(f->json, ".licenses.txt") ? "true" : "false",
                              man && mj_find(man, "virtual") && !strncmp(mj_find(man, "virtual"), "true", 4)
                                  ? "\"virtual\":true," : "", desc);
        for (k = 0; k < 5; k++)
            if (cats >> k & 1) {
                n += (size_t)snprintf(o + n, cap - n, "%s\"%s\"", first ? "" : ",", CATS[k]);
                first = 0;
            }
        n += (size_t)snprintf(o + n, cap - n, "]}");
        free(json);
        if (n >= cap - 16)
            break;
    }
    snprintf(o + n, cap - n, "]}");
    pthread_mutex_lock(&g_vmu);
    free(g_fw_json);
    g_fw_json = o;
    free(g_web_ids);
    g_web_ids = ids;
    g_web_n = g_nfw;
    pthread_mutex_unlock(&g_vmu);
}

static int catalog_find(const char *id)
{
    int i;
    for (i = 0; i < g_nfw; i++)
        if (!strcmp(g_fws[i].id, id))
            return i;
    return -1;
}

/* the objects the slots run (catalog.h's keep list: an older version a slot runs stays on disk), malloc'd */
static char *keep_list(void)
{
    char *k = calloc(NSLOT, ST_PATH + 1);
    int s;
    if (k)
        for (s = 0; s < NSLOT; s++)
            if (SL[s].on && SL[s].so[0]) {
                strcat(k, SL[s].so);
                strcat(k, "\n");
            }
    return k;
}

static void slots_tell(void);

/* a new catalog (the storage module's network thread made it): taken on the engine's thread (the core reads the list
 * only inside its calls, on this thread). The same index and entries: only what is installed changed (a download, a
 * sweep): the entries are updated in place (rfm_move_fw_installed), so the browser keeps its place. Otherwise a new list
 * (rfm_move_fw_list), the old one freed after the call. */
static void cat_take(cat_t *n)
{
    int i, s;
    if (!n)
        return;
    if (g_cat && n->index_hash == g_cat->index_hash && cat_same_ids(g_cat, n)) {   /* (the same index's bytes) */
        st_fw_t *t = g_cat->fw;
        g_cat->from = n->from;
        for (i = 0; i < n->n; i++) {
            cat_ent_t *o = &g_cat->e[i];
            const cat_ent_t *e = &n->e[i];
            if (strcmp(o->installed, e->installed) || strcmp(o->version, e->version)) {
                memcpy(o->installed, e->installed, sizeof o->installed);
                memcpy(o->version, e->version, sizeof o->version);
                if (!o->indexed)
                    memcpy(o->desc, e->desc, sizeof o->desc);
                rfm_move_fw_installed(&S->core, i, o->installed[0] ? o->installed : NULL);
            }
        }
        g_cat->fw = n->fw, g_cat->nfw = n->nfw;
        n->fw = t;
        cat_free(n);
    } else {
        cat_t *old = g_cat;
        g_cat = n;
        if (S)
            rfm_move_fw_list(&S->core, g_cat->list, g_cat->n);
        cat_free(old);
        if (S)
            slots_tell();
        fprintf(stderr, "engine: the catalog: %d firmwares (%d installed), index %s%s\n", g_cat->n, g_cat->nfw,
                g_cat->from == 2 ? "downloaded " : g_cat->from == 1 ? "bundled " : "none", g_cat->generated);
    }
    for (s = 0; s < NSLOT; s++)
        SL[s].idx = SL[s].on ? catalog_find(SL[s].fw.id) : -1;
    fw_json();
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
static void slot_midi(int s, const uint8_t *b, int n, uint32_t frame)
{
    fw_midi(&SL[s].fw, b, n, SL[s].fw.frames_done + frame);
    if (n == 3 && (b[0] & 0xF0) == 0x90 && b[2])
        SL[s].notes++;
}

/* a sequencer message (track 1..16) after MIDI FX slot `from`: through the rest of the chain (each slot acts on the
 * tracks in its filter and passes the others through), then to every sounding firmware whose filter has the track */
/* MIDI down the MIDI FX chain from slot `from`: each MIDI FX acts on the tracks in its filter and passes the rest on.
 * What comes out goes to the firmwares: the sequencer's (live 0) to every sounding slot whose track filter has the
 * track; the pads played live (live 1) to the active slot only, as without MIDI FX */
static void chain_midi(int from, int track, int live, const uint8_t *b, int n, uint32_t frame)
{
    int k, s;
    if (track < 1 || track > 16)
        return;
    for (k = from; k < RFM_MOVE_FX_SLOTS; k++) {
        fxs_t *x = &FX[RFM_MOVE_FX_MIDI][k];
        if (x->run && (x->filt >> (track - 1) & 1)) {
            uint8_t out[FX_MAX_OUT][3];
            int lens[FX_MAX_OUT], i;
            const int m = fx_midi(x->run, b, n, out, lens, FX_MAX_OUT);
            x->last_track = track, x->last_live = live;
            for (i = 0; i < m; i++)
                if (lens[i] > 0 && lens[i] <= 3)
                    chain_midi(k + 1, track, live, out[i], lens[i], frame);
            return;
        }
    }
    if (live) {
        if (SL[g_active].on)
            slot_midi(g_active, b, n, frame);
        return;
    }
    for (s = 0; s < NSLOT; s++)
        if (SL[s].on && (rfm_move_fw_sounding(&S->core) >> s & 1) && (SL[s].filt >> (track - 1) & 1))
            slot_midi(s, b, n, frame);
}

static void route_midi(const rfm_move_act_t *a)
{
    if (!a->n)
        return;
    if (a->track == 0) {   /* the pads played live (MIDI, Control, the sequencer's live notes): through the MIDI FX as
                            * the track of their channel (the sequencer's tracks play on their own channels), then to
                            * the active slot; system messages straight there */
        if (a->bytes[0] >= 0x80 && a->bytes[0] < 0xF0 && a->n <= 3)
            chain_midi(0, (a->bytes[0] & 0x0F) + 1, 1, a->bytes, a->n, a->frame);
        else if (SL[g_active].on)
            slot_midi(g_active, a->bytes, a->n, a->frame);
        return;
    }
    chain_midi(0, a->track, 0, a->bytes, a->n, a->frame);   /* the sequencer's: the MIDI FX, then the filters */
}

/* each MIDI FX's tick (once a block): its generated notes take its track (its filter's one track, else the track it
 * heard last) and go on down the chain */
static void midi_fx_tick(void)
{
    int k;
    for (k = 0; k < RFM_MOVE_FX_SLOTS; k++) {
        fxs_t *x = &FX[RFM_MOVE_FX_MIDI][k];
        uint8_t out[FX_MAX_OUT][3];
        int lens[FX_MAX_OUT], i, m, t;
        if (!x->run)
            continue;
        m = fx_tick(x->run, FW_BLOCK, out, lens, FX_MAX_OUT);
        for (t = 0; t < 16 && x->filt != (1u << t); t++)
            ;
        t = t < 16 ? t + 1 : x->last_track ? x->last_track : 1;
        for (i = 0; i < m; i++)
            if (lens[i] > 0 && lens[i] <= 3)
                chain_midi(k + 1, t, x->last_live, out[i], lens[i], 0);
    }
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

/* ------------------------------------------------------------------ Schwung FX (the engine's side) --- */
static void notes_off_all(void)   /* a MIDI FX changed: what it held let go (CC 123 on every channel) */
{
    int s, c;
    for (s = 0; s < NSLOT; s++)
        for (c = 0; SL[s].on && c < 16; c++) {
            const uint8_t m[3] = { (uint8_t)(0xB0 | c), 123, 0 };
            fw_midi(&SL[s].fw, m, 3, SL[s].fw.frames_done);
        }
}
static void fx_drop(int kind, int s)
{
    fxs_t *x = &FX[kind][s];
    if (!x->run)
        return;
    {
        st_job_t *j = st_job_new(J_FX_FREE);
        j->ptr = x->run;
        st_post(j);
    }
    x->run = NULL;
    if (kind == RFM_MOVE_FX_MIDI)
        notes_off_all();
}
static void fx_apply(int kind, int s)   /* the core's values into the instance */
{
    fxs_t *x = &FX[kind][s];
    const fx_mod_t *m = fx_inst_mod(x->run);
    int k;
    for (k = 0; m && k < m->np && k < RFM_MOVE_FX_PARAMS; k++)
        if (x->val[k] != RFM_MOVE_FX_UNSET)   /* (an older project's value not kept: the module's own default) */
            fx_set(x->run, k, x->val[k]);
}
static void fx_pend_clear(fxs_t *x)
{
    free(x->pend_state);
    x->pend_state = NULL;
    x->pend_name[0] = 0;
}
static void fx_pend_apply(fxs_t *x)   /* a project's saved state for the module now running here */
{
    const fx_mod_t *m = fx_inst_mod(x->run);
    if (m && x->pend_state && !strcmp(m->name, x->pend_name)) {
        fx_state_set(x->run, x->pend_state);
        fprintf(stderr, "engine: %s: its saved state (%zu bytes)\n", m->name, strlen(x->pend_state));
    }
    fx_pend_clear(x);
}
/* FX_LOAD (the core's, or the page's through it): open it on the storage thread; it comes in on the engine's */
static void fx_want(int kind, int s, int mod)
{
    fxs_t *x = &FX[kind][s];
    const fx_mod_t *m = fx_mod(g_fxlib, kind, mod);
    const rfm_move_fxslot_t *c = kind == RFM_MOVE_FX_MIDI ? &S->core.seq.fx.mfx[s] : &S->core.seq.fx.afx[s];
    memcpy(x->val, c->val, sizeof x->val);   /* (a fresh load: the core's defaults; a project's: its values follow) */
    g_mix_ver++;
    if (x->pend_name[0] && (!m || strcmp(m->name, x->pend_name)))
        fx_pend_clear(x);
    if (!m) {
        x->want = -1, x->gen++, x->opening = 0;
        fx_drop(kind, s);
        return;
    }
    if (x->run && fx_inst_mod(x->run) == m && !x->opening) {   /* there already */
        x->want = mod;
        fx_pend_apply(x);
        fx_apply(kind, s);
        return;
    }
    x->want = mod, x->gen++, x->opening = 1, x->err[0] = 0;
    {
        st_job_t *j = st_job_new(J_FX_OPEN);
        j->obj = (void *)m;
        j->slot = s, j->index = kind, j->value = (int)x->gen;
        snprintf(j->id, sizeof j->id, "%.31s", m->id);
        st_post(j);
    }
}
static void fx_refuse(int kind, int s, const char *why)
{
    fxs_t *x = &FX[kind][s];
    snprintf(x->err, sizeof x->err, "%s", why);
    fprintf(stderr, "engine: %s FX %d: %s\n", kind ? "audio" : "MIDI", s + 1, why);
    x->want = -1, x->gen++, x->opening = 0;
    fx_drop(kind, s);
    fx_pend_clear(x);
    rfm_move_fx_refused(&S->core, kind, s);   /* (the core empties the slot, "Too busy") */
    rfm1cs_session_redraw(S, now_ms());
    g_mix_ver++;
}
static void fx_opened(st_job_t *j)
{
    const int kind = j->index, s = j->slot;
    fxs_t *x;
    if (kind < 0 || kind >= FX_KINDS || s < 0 || s >= RFM_MOVE_FX_SLOTS) {
        fx_close(j->ptr);
        return;
    }
    x = &FX[kind][s];
    if ((uint32_t)j->value != x->gen || x->want < 0) {   /* (asked again since, or emptied) */
        if (j->ptr) {
            st_job_t *f = st_job_new(J_FX_FREE);
            f->ptr = j->ptr;
            st_post(f);
        }
        return;
    }
    if (!j->ptr) {
        x->opening = 0;
        fx_refuse(kind, s, j->err[0] ? j->err : "could not load");
        return;
    }
    if (j->kind == J_FX_OPEN && x->pend_state && !strcmp(fx_inst_mod(j->ptr)->name, x->pend_name)) {
        /* a project's saved state: set on the storage thread before the instance comes in (a module can take
         * milliseconds over it: here that would starve the ring) */
        st_job_t *n = st_job_new(J_FX_STATE);
        n->obj = j->obj, n->ptr = j->ptr, j->ptr = NULL;
        n->slot = j->slot, n->index = j->index, n->value = j->value;
        snprintf(n->id, sizeof n->id, "%s", j->id);
        n->data = (uint8_t *)x->pend_state, n->len = (uint32_t)strlen(x->pend_state);
        x->pend_state = NULL;
        fprintf(stderr, "engine: %s: its saved state (%u bytes), set off the engine thread\n", x->pend_name, n->len);
        fx_pend_clear(x);
        st_post(n);
        return;
    }
    x->opening = 0;
    fx_drop(kind, s);
    x->run = j->ptr;
    x->ns_mark = fx_ns(x->run), x->over = 0, x->last_track = 0, x->last_live = 0;
    fx_pend_apply(x);
    fx_apply(kind, s);
    fprintf(stderr, "engine: %s FX %d: %s\n", kind ? "audio" : "MIDI", s + 1, fx_inst_mod(x->run)->name);
    g_mix_ver++;
}

/* a project's FX modules' own state (get_param "state"), beside the project: move-project-NN.fx.json */
static void fx_state_save(int slot)
{
    size_t cap = 1 << 18, n = 0;
    char *o = malloc(cap), *st = malloc(65536), *esc = malloc(2 * 65536 + 8), p[ST_PATH];
    int kind, s, first = 1;
    if (!o || !st || !esc) {
        free(o), free(st), free(esc);
        return;
    }
    n += (size_t)snprintf(o + n, cap - n, "{");
    for (kind = 0; kind < FX_KINDS; kind++)
        for (s = 0; s < RFM_MOVE_FX_SLOTS; s++) {
            fxs_t *x = &FX[kind][s];
            if (!x->run || fx_state_get(x->run, st, 65536) < 0)
                continue;
            mj_escape(st, esc, 2 * 65536 + 8);
            if (n + strlen(esc) + 128 >= cap)
                continue;
            n += (size_t)snprintf(o + n, cap - n, "%s\"%c%d\":{\"name\":\"%s\",\"state\":\"%s\"}", first ? "" : ",",
                                  kind ? 'a' : 'm', s, fx_inst_mod(x->run)->name, esc);
            first = 0;
        }
    n += (size_t)snprintf(o + n, cap - n, "}\n");
    st_fx_state_path(p, sizeof p, slot);
    job_write(p, (const uint8_t *)o, (uint32_t)n);
    free(o), free(st), free(esc);
}
static void fx_state_take(const char *text)   /* a project's, as it loads: kept until its modules are there */
{
    int kind, s;
    for (kind = 0; kind < FX_KINDS; kind++)
        for (s = 0; s < RFM_MOVE_FX_SLOTS; s++) {
            fxs_t *x = &FX[kind][s];
            char key[4];
            const char *v;
            fx_pend_clear(x);
            snprintf(key, sizeof key, "%c%d", kind ? 'a' : 'm', s);
            if (!text || !(v = mj_find(text, key)) || *v != '{')
                continue;
            if (!(x->pend_state = malloc(65536)) || mj_string(v, "state", x->pend_state, 65536) ||
                mj_string(v, "name", x->pend_name, sizeof x->pend_name))
                fx_pend_clear(x);
        }
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
        if (a->kind == RFM_MOVE_ACT_PROJECT_SAVE)    /* (its FX modules' own state beside it) */
            fx_state_save(a->index);
        if (a->kind == RFM_MOVE_ACT_PROJECT_DELETE || a->kind == RFM_MOVE_ACT_PROJECT_CUT) {
            char p[ST_PATH];
            st_fx_state_path(p, sizeof p, a->index);
            job_write(p, (const uint8_t *)"{}\n", 3);
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
        for (s = 0; s < RFM_MOVE_FX_SLOTS; s++) {
            fxs_t *x = &FX[RFM_MOVE_FX_MIDI][s];
            if (x->filt != a->filt[3 + s] && x->run)
                notes_off_all();
            x->filt = a->filt[3 + s];
        }
        g_mix_ver++;
        break;
    }
    case RFM_MOVE_ACT_MIX:
        if (a->slot < NSLOT) {
            int k;
            SL[a->slot].level = a->mix[0] > 127 ? 127 : a->mix[0];
            SL[a->slot].pan = a->mix[1] > 127 ? 127 : a->mix[1];
            for (k = 0; k < 3; k++)
                SL[a->slot].send[k] = a->mix[2 + k] > 127 ? 127 : a->mix[2 + k];
        }
        g_mix_ver++;
        break;
    case RFM_MOVE_ACT_FW_LEVEL:
        if (a->slot < NSLOT)
            SL[a->slot].level = (uint8_t)(a->value < 0 ? 0 : a->value > 127 ? 127 : a->value);
        g_mix_ver++;
        break;
    case RFM_MOVE_ACT_FX_LOAD:
        if (a->fx < FX_KINDS && a->slot < RFM_MOVE_FX_SLOTS)
            fx_want(a->fx, a->slot, a->value);
        break;
    case RFM_MOVE_ACT_FX_PARAM:
        if (a->fx < FX_KINDS && a->slot < RFM_MOVE_FX_SLOTS && a->param < RFM_MOVE_FX_PARAMS) {
            fxs_t *x = &FX[a->fx][a->slot];
            x->val[a->param] = (uint8_t)(a->value == RFM_MOVE_FX_UNSET ? RFM_MOVE_FX_UNSET : a->value < 0 ? 0 : a->value > 127 ? 127 : a->value);
            if (x->run && x->val[a->param] != RFM_MOVE_FX_UNSET)
                fx_set(x->run, a->param, x->val[a->param]);
            g_mix_ver++;
        }
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
    default:   /* CLICK, AUDIO_REC (no metronome or Sample loop yet) */
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
    g_mix_ver++;
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
    fw_inst_t *pre;
    int i;
    if (!SL[s].on)
        return;
    if (save)
        slot_save(s, 1);
    if (g_raw_fw == &SL[s].fw)
        raw_flush();
    /* its memory freed on the storage thread (megabytes of locked pages: not the engine's time) */
    if ((pre = fw_release(&SL[s].fw))) {
        st_job_t *j = st_job_new(J_FW_FREE);
        j->ptr = pre;
        st_post(j);
    }
    SL[s].on = 0;
    if ((i = catalog_find(SL[s].fw.id)) >= 0 && strcmp(g_fws[i].so, SL[s].so))
        g_sweep = 1;   /* (it ran an older version than the one installed: its files can go now) */
    SL[s].so[0] = 0;
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
    g_mix_ver++;
    if (!fw) {   /* nothing to show: a dark panel */
        static const uint8_t dark[FW_NLEVELS];
        memset(g_lcd_host, 0, sizeof g_lcd_host);
        rfm1cs_session_panel(S, dark, 0);
        rfm1cs_session_frame(S, now_ms(), g_lcd_host);
    }
    view_info();
}

static void render_block(uint32_t now);

/* Before a firmware starts on the engine's thread (its code: _initialize, rfm1_init, a few ms on the Move; the slot's
 * old firmware saved and shut down), the ring is filled well past the lead, so the audio thread has ~80 ms of the
 * other slots' sound meanwhile: no late block. (The slow parts, the object's dlopen and the instance's memory, are made
 * on the storage thread: J_FW_OPEN.) */
static void preroll(void)
{
    if (g_no_preroll)
        return;
    while (g_run && mr_ring_count(&g_ring) + FW_BLOCK <= PREROLL_FRAMES)
        render_block(now_ms());
}

/* a firmware into slot s (an instance fw_instantiate made from its object, a saved flash or none) */
static int slot_boot(int s, fw_inst_t *pre, const uint8_t *flash, uint32_t n)
{
    slot_t *sl = &SL[s];
    char so[ST_PATH];
    int rc;
    snprintf(so, sizeof so, "%s", pre->obj->path);
    preroll();
    if (g_test_boot_ms)
        usleep(g_test_boot_ms * 1000u);
    slot_unload(s, 1);
    rc = fw_create_from(&sl->fw, pre);
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
    snprintf(sl->so, sizeof sl->so, "%s", so);
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
    fw_inst_t *pre;
    char err[200], p[ST_PATH];
    uint8_t *img;
    size_t n = 0;
    if (i < 0)
        return;
    if (!(obj = fw_obj_open(g_fws[i].so, err, sizeof err))) {
        fprintf(stderr, "engine: %s: %s\n", id, err);
        return;
    }
    pre = fw_instantiate(obj);
    fw_obj_close(obj);
    if (!pre) {
        fprintf(stderr, "engine: %s: %s\n", id, fw_inst_why());
        return;
    }
    st_path(p, sizeof p, "states/%s/flash.bin", id);
    img = st_read(p, &n);
    slot_boot(s, pre, img, (uint32_t)n);
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

/* a firmware that could not load: the page hears it (view_info's "failed") */
static void load_failed(const char *id)
{
    snprintf(g_fail_id, sizeof g_fail_id, "%s", id);
    g_fail_seq++;
    view_info();
}

static void page_edit(int op, int a, int b);
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
                load_failed(id);
                break;
            }
            if (SL[s].on && !strcmp(SL[s].fw.id, id) && !strcmp(SL[s].so, g_fws[i].so)) {   /* there already */
                rfm_move_fw_progress(&S->core, id, 100);
                slots_tell();
                if (g_activate == s) {
                    g_activate = -1;
                    page_edit(ENG_OP_ACTIVE, s, 0);
                }
                break;
            }
            rfm_move_fw_progress(&S->core, id, 10);
            {   /* a slot running it (another version: an update) saves its flash first; the storage thread writes
                 * it before it reads it back for this load (one queue, in order) */
                int o;
                for (o = 0; o < NSLOT; o++)
                    if (SL[o].on && !strcmp(SL[o].fw.id, id))
                        slot_save(o, 1);
            }
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
        case RFM_MOVE_ACT_FW_DOWNLOAD: {   /* the network thread fetches, checks and installs it (catalog.h) */
            const int e = cat_find(g_cat, id);
            if (g_dl_busy) {
                rfm_move_fw_progress(&S->core, id, RFM_MOVE_FW_BUSY);
                break;
            }
            snprintf(g_dl.id, sizeof g_dl.id, "%s", id);
            g_dl.pct = 0;
            g_dl.err[0] = 0;
            if (e < 0 || !g_cat->e[e].indexed || !g_cat->e[e].files) {
                snprintf(g_dl.err, sizeof g_dl.err, "%s is not in the catalog's index", id);
                g_dl.state = 3, g_dl.failed++;
                fprintf(stderr, "engine: download: %s\n", g_dl.err);
                rfm_move_fw_progress(&S->core, id, RFM_MOVE_FW_FAILED);
                break;
            }
            {
                st_job_t *j = st_job_new(J_NET_DOWNLOAD);
                snprintf(j->id, sizeof j->id, "%s", id);
                j->text = strdup(g_cat->e[e].files);
                j->total = g_cat->e[e].bytes;
                j->data = (uint8_t *)keep_list();
                st_post(j);
            }
            g_dl_busy = 1;
            g_dl.state = 1;
            fprintf(stderr, "engine: download: %s %s (%llu bytes)\n", id, g_cat->e[e].version,
                    (unsigned long long)g_cat->e[e].bytes);
            break;
        }
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
                if (a->kind == RFM_MOVE_ACT_STATE_LOAD) {   /* (a new instance of the same object, made there) */
                    fw_obj_ref(SL[s].fw.obj);
                    j->obj = SL[s].fw.obj;
                }
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
            if (!j->ptr) {
                fprintf(stderr, "engine: %s: %s\n", j->id, j->err);
                rfm_move_fw_progress(&S->core, j->id, RFM_MOVE_FW_FAILED);
                load_failed(j->id);
                break;
            }
            if (slot_boot(s, j->ptr, j->data, j->len) != 0) {
                rfm_move_fw_progress(&S->core, j->id, RFM_MOVE_FW_FAILED);
                slots_tell();
                load_failed(j->id);
                if (s == g_active)
                    active_changed();
                break;
            }
            slots_tell();
            rfm_move_fw_progress(&S->core, j->id, 100);
            states_scan(s);
            if (g_activate == s) {   /* (the page's chip: load into slot n, then play it) */
                g_activate = -1;
                page_edit(ENG_OP_ACTIVE, s, 0);
            }
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
            if (SL[s].on && !strcmp(SL[s].fw.id, j->id) && j->data && j->ptr) {
                slot_unload(s, 0);   /* (the state replaces what was there: its flash.bin follows on the next save) */
                if (slot_boot(s, j->ptr, j->data, j->len) == 0) {
                    SL[s].dirty = 1, SL[s].dirty_ms = now_ms();
                    if (s == g_active)
                        active_changed();
                }
                slots_tell();
            } else
                fw_inst_free(j->ptr);
            break;
        case R_DL_PROGRESS:
            if (g_dl_busy && !strcmp(j->id, g_dl.id)) {
                g_dl.pct = j->value;
                rfm_move_fw_progress(&S->core, j->id, j->value);
            }
            break;
        case R_DL_DONE:
            g_dl_busy = 0;
            cat_take(j->ptr);   /* (what is installed now, either way) */
            if (j->ok) {
                const int e = cat_find(g_cat, j->id);
                g_dl.state = 2, g_dl.pct = 100, g_dl.ok++;
                fprintf(stderr, "engine: download: %s %s installed\n", j->id, e >= 0 ? g_cat->e[e].installed : "?");
                rfm_move_fw_progress(&S->core, j->id, 100);
            } else {
                g_dl.state = 3, g_dl.failed++;
                snprintf(g_dl.err, sizeof g_dl.err, "%s", j->err);
                fprintf(stderr, "engine: download: %s failed: %s\n", j->id, j->err);
                rfm_move_fw_progress(&S->core, j->id, RFM_MOVE_FW_FAILED);
            }
            break;
        case R_INDEX:
            g_index_busy = 0;
            g_next_index_ms = now_ms() + (j->ok ? INDEX_EVERY_MS : INDEX_RETRY_MS);
#ifdef RFM14_TEST_BUILD
            {   /* (tests/dl_e2e.sh: the index fetched again this often, success or not) */
                const char *t = getenv("RFM14_TEST_INDEX_MS");
                if (t && atoi(t) > 0)
                    g_next_index_ms = now_ms() + (uint32_t)atoi(t);
            }
#endif
            if (j->ok) {
                g_dl.index_ok++;
                g_index_err[0] = 0;
                if (j->ptr)
                    fprintf(stderr, "engine: the catalog's index changed\n");
                cat_take(j->ptr);
            } else {
                g_dl.index_failed++;
                snprintf(g_index_err, sizeof g_index_err, "%s", j->err);
                fprintf(stderr, "engine: the catalog's index: %s (again in %u min)\n", j->err, INDEX_RETRY_MS / 60000u);
            }
            break;
        case R_CATALOG:
            cat_take(j->ptr);
            break;
        case J_FX_OPEN:
        case J_FX_STATE:
            fx_opened(j);
            break;
        case R_PROJECT_LOADED:
            fx_state_take(j->text);   /* (before the core's fx_sync loads its modules) */
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
    char tmp[sizeof g_view.info];
    int n = 0, s, k;
    n += snprintf(tmp + n, sizeof tmp - (size_t)n, "{\"active\":%d,\"fail_seq\":%u,\"failed\":\"%s\",\"slots\":[",
                  g_active, g_fail_seq, g_fail_id);
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
        {   /* the live labels (FM1CORE_LBL_* order; null: not given), and their epoch (0: none) */
            const uint32_t e = fw_labels_epoch(fw);
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"lseq\":%u", e);
            if (e) {
                n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"labels\":[");
                for (k = 0; k < FW_LBL_N && n < (int)sizeof tmp - 64; k++) {
                    const char *l = fw_label(fw, k);
                    if (l) {
                        mj_escape(l, esc, sizeof esc);
                        n += snprintf(tmp + n, sizeof tmp - (size_t)n, "%s\"%s\"", k ? "," : "", esc);
                    } else
                        n += snprintf(tmp + n, sizeof tmp - (size_t)n, "%snull", k ? "," : "");
                }
                n += snprintf(tmp + n, sizeof tmp - (size_t)n, "]");
            }
        }
    }
    if (n < (int)sizeof tmp - 64) {   /* the FM-1 on USB-A (fm1link.c) */
        n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"fm1\":");
        n += fm1link_json(tmp + n, (int)sizeof tmp - n);
        if (n > (int)sizeof tmp - 2)
            n = (int)sizeof tmp - 2;
    }
    snprintf(tmp + n, sizeof tmp - (size_t)n, "}");
    pthread_mutex_lock(&g_vmu);
    snprintf(g_view.info, sizeof g_view.info, "%s", tmp);
    g_view.info_seq++;
    g_view.seq++;
    pthread_mutex_unlock(&g_vmu);
}

/* ---- Mix & FX: the slots, the FX setup and the mix as the core has them (its state is the truth: the Move's screens
 * show the same), with what the host adds (CPU, a load under way, a refusal) */
static const uint8_t PAL[16][3] = {   /* rfm1cs's 16 pad colours (fw.c PAL) */
    { 255, 36, 36 },  { 255, 120, 0 },  { 255, 200, 0 },  { 190, 255, 40 }, { 40, 255, 70 },  { 0, 255, 170 },
    { 20, 220, 255 }, { 40, 120, 255 }, { 90, 60, 255 },  { 170, 60, 255 }, { 255, 50, 210 }, { 255, 110, 150 },
    { 255, 160, 100 }, { 160, 110, 60 }, { 150, 150, 160 }, { 255, 255, 255 },
};
static const uint8_t CATCOL[5] = { 6, 1, 9, 0, 5 };   /* (fw.c: a category's default colour) */
static int slot_color(int s)   /* the library's pad colour of slot s's firmware, as the Move's pads show it */
{
    const rfm_move_fw_state_t *f = &S->core.fw;
    const int fw = f->slot_fw[s];
    int r;
    for (r = 0; r < f->nrec; r++)
        if (!strcmp(f->rec[r].id, f->slot_id[s]) && f->rec[r].color)
            return f->rec[r].color - 1;
    return fw >= 0 && fw < f->n && f->list[fw].cat < 5 ? CATCOL[f->list[fw].cat] : 14;
}
static void mix_json(void)
{
    static char tmp[sizeof g_view.mix];
    const rfm_move_fxset_t *x = &S->core.seq.fx;
    char esc[2][160];
    int n = 0, s, k, kind;
#define PUT(...) do { if (n < (int)sizeof tmp) n += snprintf(tmp + n, sizeof tmp - (size_t)n, __VA_ARGS__); } while (0)
    PUT("{\"type\":\"mix\",\"ver\":%u,\"active\":%d,\"page\":%d,\"focus\":%d,\"fxpages\":%d,\"mixpar\":%d,\"fxopen\":%d,"
        "\"sounding\":%d,\"slots\":[", g_mix_ver, g_active, S->core.s.fx_page, S->core.fw.fx_focus, S->core.fw.fx_pages,
        S->core.fw.mix_par,
        S->core.fw.fx && S->core.seq.projects, rfm_move_fw_sounding(&S->core));
    for (s = 0; s < NSLOT; s++) {
        const slot_t *sl = &SL[s];
        const uint8_t *c = PAL[slot_color(s)];
        PUT("%s{\"on\":%d", s ? "," : "", sl->on);
        if (sl->on) {
            mj_escape(sl->fw.name, esc[0], sizeof esc[0]);
            PUT(",\"id\":\"%s\",\"name\":\"%s\",\"version\":\"%s\",\"cpu\":%.1f,\"notes\":%u", sl->fw.id, esc[0],
                sl->fw.version, sl->cpu_permille / 10.0, sl->notes);
        }
        PUT(",\"color\":\"#%02x%02x%02x\",\"level\":%d,\"pan\":%d,\"sends\":[%d,%d,%d],\"filt\":%u}", c[0], c[1], c[2],
            S->core.fw.level[s], x->pan[s], x->send[s][0], x->send[s][1], x->send[s][2], x->ffilt[s]);
    }
    PUT("]");
    for (kind = 0; kind < FX_KINDS; kind++) {
        PUT(",\"%s\":[", kind ? "afx" : "mfx");
        for (s = 0; s < RFM_MOVE_FX_SLOTS; s++) {
            const rfm_move_fxslot_t *c = kind ? &x->afx[s] : &x->mfx[s];
            const fxs_t *h = &FX[kind][s];
            mj_escape(c->name, esc[0], sizeof esc[0]);
            mj_escape(h->err, esc[1], sizeof esc[1]);
            const fx_mod_t *md = fx_mod(g_fxlib, kind, c->mod);
            const int np = md ? (md->np < RFM_MOVE_FX_PARAMS ? md->np : RFM_MOVE_FX_PARAMS) : 0;
            PUT("%s{\"mod\":%d,\"name\":\"%s\",\"pg\":%d,\"vals\":[", s ? "," : "", c->mod, esc[0], S->core.fw.fx_pg[kind][s]);
            for (k = 0; k < np; k++)   /* (the module's planned parameters only; 255: not set) */
                PUT("%s%d", k ? "," : "", c->val[k]);
            PUT("],\"filt\":%u,\"run\":%d,\"busy\":%d,\"cpu\":%.1f,\"err\":\"%s\"}", kind ? 0xFFFFu : c->filt,
                h->run != NULL, h->opening, h->cpu_permille / 10.0, esc[1]);
        }
        PUT("]");
    }
    if (n < (int)sizeof tmp - 64) {   /* the FM-1 on USB-A (fm1link.c) */
        PUT(",\"fm1\":");
        n += fm1link_json(tmp + n, (int)sizeof tmp - n);
        if (n > (int)sizeof tmp - 2)
            n = (int)sizeof tmp - 2;
    }
    PUT("}");
#undef PUT
    pthread_mutex_lock(&g_vmu);
    memcpy(g_view.mix, tmp, sizeof tmp);
    g_view.mix_seq++;
    g_view.seq++;
    pthread_mutex_unlock(&g_vmu);
}

/* the meters (10 a second) and the slots' mini screens (4 a second, when they changed) for the page */
static uint8_t meter(int peak) { const double v = sqrt((double)(peak > 32767 ? 32767 : peak) / 32767.0) * 255.0; return (uint8_t)(v + 0.5); }
static void view_live(uint32_t now)
{
    static uint32_t last_meter;
    int s;
    if (now - last_meter >= 100u) {
        uint8_t m[7];
        last_meter = now;
        for (s = 0; s < 3; s++) {
            m[s] = meter(SL[s].on ? SL[s].peak : 0), SL[s].peak = 0;
            m[3 + s] = meter(FX[RFM_MOVE_FX_AUDIO][s].peak), FX[RFM_MOVE_FX_AUDIO][s].peak = 0;
        }
        m[6] = meter(g_peak_win), g_peak_win = 0;
        pthread_mutex_lock(&g_vmu);
        if (memcmp(m, g_view.meters, sizeof m)) {
            memcpy(g_view.meters, m, sizeof m);
            g_view.meters_seq++;
            g_view.seq++;
        }
        pthread_mutex_unlock(&g_vmu);
    }
    for (s = 0; s < NSLOT; s++) {
        slot_t *sl = &SL[s];
        uint32_t w;
        static uint8_t mini[ENG_MINI * ENG_MINI * 2];
        const uint8_t *be;
        int x, y;
        if (!sl->on || sl->fw.dead || now - sl->mini_ms < 250u)
            continue;
        w = fw_lcd_writes(&sl->fw);
        if (w == sl->mini_writes && sl->mini_writes)
            continue;
        sl->mini_writes = w ? w : 1, sl->mini_ms = now;
        be = fw_lcd(&sl->fw);
        for (y = 0; y < ENG_MINI; y++)   /* a 5 x 5 box average */
            for (x = 0; x < ENG_MINI; x++) {
                int r = 0, g = 0, b = 0, i, j;
                uint16_t v;
                for (j = 0; j < 5; j++)
                    for (i = 0; i < 5; i++) {
                        const int o = 2 * ((y * 5 + j) * 240 + x * 5 + i);
                        const uint16_t p = (uint16_t)(be[o] << 8 | be[o + 1]);
                        r += p >> 11, g += p >> 5 & 63, b += p & 31;
                    }
                v = (uint16_t)((r / 25) << 11 | (g / 25) << 5 | (b / 25));
                mini[2 * (y * ENG_MINI + x)] = (uint8_t)(v >> 8), mini[2 * (y * ENG_MINI + x) + 1] = (uint8_t)v;
            }
        pthread_mutex_lock(&g_vmu);
        memcpy(g_view.mini[s], mini, sizeof mini);
        g_view.mini_seq[s]++;
        g_view.seq++;
        pthread_mutex_unlock(&g_vmu);
    }
}

/* ---- the page's edits, into the core's state (rfm1cs v0.4.0 has no call for these: its FX setup, mix and routing are
 * public fields of rfm_move_t, written here on the core's thread as its own handlers write them), then the same
 * action the core would give, through on_action: the host's state follows, and the project and library save as after
 * an edit on the Move */
static void emit(rfm_move_act_t *a)
{
    rfm1cs_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = RFM1CS_EV_ACTION, ev.act = a;
    on_action(&ev);
}
/* the page's edits (/ws ops 16..24, /api/edit), on the engine thread outside any core callback: rfm1cs 0.4.1's host
 * setters apply each as the Move's own edit does (the same action through the session to on_action, the same dirty
 * marks: the project's autosave, the library); FW_UNLOAD has no setter: our own handler takes it as from Delete */
static void page_edit(int op, int a, int b)
{
    rfm_move_t *m = &S->core;
    switch (op) {
    case ENG_OP_ACTIVE:
        if (a < 0 || a >= NSLOT || !SL[a].on || a == m->fw.active)
            return;
        rfm_move_fw_set_active(m, a);
        break;
    case ENG_OP_UNLOAD: {
        rfm_move_act_t act;
        if (a < 0 || a >= NSLOT || !SL[a].on)
            return;
        memset(&act, 0, sizeof act);
        act.kind = RFM_MOVE_ACT_FW_UNLOAD, act.index = act.slot = (uint8_t)a;
        emit(&act);
        break;
    }
    case ENG_OP_MIX: {
        const int s = a / 8, f = a % 8, v = b < 0 ? 0 : b > 127 ? 127 : b;
        int sends[RFM_MOVE_FX_SLOTS] = { -1, -1, -1 };
        if (s >= NSLOT || f > 4)
            return;
        if (f == 0)
            rfm_move_fw_set_level(m, s, v);
        else if (f == 1)
            rfm_move_mix_set(m, s, -1, v, NULL);
        else {
            sends[f - 2] = v;
            rfm_move_mix_set(m, s, -1, -1, sends);
        }
        break;
    }
    case ENG_OP_ROUTE: {
        int ff[RFM_MOVE_FW_SLOTS] = { -1, -1, -1 }, mf[RFM_MOVE_FX_SLOTS] = { -1, -1, -1 };
        if (a < 0 || a > 5)
            return;
        if (a < 3)
            ff[a] = (uint16_t)b;
        else
            mf[a - 3] = (uint16_t)b;   /* (one track's bit, or all: the setter makes anything else all) */
        rfm_move_route_set(m, ff, mf);
        break;
    }
    case ENG_OP_FXLOAD:
        if ((a & 3) >= RFM_MOVE_FX_SLOTS)
            return;
        rfm_move_fx_set_module(m, a >> 2 & 1, a & 3, (int16_t)b);
        break;
    case ENG_OP_FXPARAM:   /* (the 0.4 form: parameters 0..7) */
        if ((a >> 3 & 3) >= RFM_MOVE_FX_SLOTS)
            return;
        rfm_move_fx_set_param(m, a >> 5 & 1, a >> 3 & 3, a & 7, b);
        break;
    case ENG_OP_FXPAR:   /* (0.5.0) a parameter 0..127 by index */
        if ((b >> 7 & 3) >= RFM_MOVE_FX_SLOTS || a < 0 || a >= RFM_MOVE_FX_PARAMS)
            return;
        rfm_move_fx_set_param(m, b >> 9 & 1, b >> 7 & 3, a, b & 127);
        break;
    case ENG_OP_FXPG: {   /* (0.5.0) the page a slot's knobs are on (the Move shows it when its pages are open) */
        const int kind = a >> 2 & 1, s = a & 3;
        const rfm_move_fxslot_t *c = kind ? &m->seq.fx.afx[s] : &m->seq.fx.mfx[s];
        const fx_mod_t *md;
        if (s >= RFM_MOVE_FX_SLOTS || !(md = fx_mod(g_fxlib, kind, c->mod)) || b < 0 || b >= (md->npg ? md->npg : 1))
            return;
        m->fw.fx_pg[kind][s] = (uint8_t)b;
        g_mix_ver++;
        break;
    }
    case ENG_OP_PAGE: {   /* a: FX setup's column / the Mixer (0..3), | (row + 1) << 2 the slot selected in it */
        const int pg = a & 3, row = (a >> 2 & 3) - 1;
        if (rfm_move_fx_set_page(m, pg) < 0)
            return;
        if (row >= 0 && row < RFM_MOVE_FX_SLOTS && pg < RFM_MOVE_FX_PAGE_MIX && row != m->fw.fx_focus)
            m->fw.fx_focus = (uint8_t)row, m->fw.fx_pages = 0;   /* (as a tap on that slot's pad) */
        g_mix_ver++;
        break;
    }
    default:
        return;
    }
    rfm1cs_session_redraw(S, now_ms());   /* (the Move's screen: the view shows the new value) */
}

char *engine_fx_modules(void)
{
    char *r;
    pthread_mutex_lock(&g_vmu);
    r = g_fx_json ? strdup(g_fx_json) : NULL;
    pthread_mutex_unlock(&g_vmu);
    return r;
}

int engine_test_midi(int track, int b0, int b1, int b2)
{
    int r = -1;
    if (!C.test_api || track < 0 || track > 16)   /* (track 0: as the pads played live) */
        return -1;
    pthread_mutex_lock(&g_vmu);
    if (g_ntmidi < (int)(sizeof g_tmidi / sizeof g_tmidi[0])) {
        g_tmidi[g_ntmidi].track = (uint8_t)track;
        g_tmidi[g_ntmidi].b[0] = (uint8_t)b0, g_tmidi[g_ntmidi].b[1] = (uint8_t)b1, g_tmidi[g_ntmidi].b[2] = (uint8_t)b2;
        g_ntmidi++;
        r = 0;
    }
    pthread_mutex_unlock(&g_vmu);
    return r;
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

char *engine_firmwares(void)
{
    char *r;
    pthread_mutex_lock(&g_vmu);
    r = g_fw_json ? strdup(g_fw_json) : NULL;
    pthread_mutex_unlock(&g_vmu);
    return r;
}

int engine_load(int slot, const char *id, int activate)
{
    int r = -1, i;
    if (slot < 0 || slot >= NSLOT || !id || strlen(id) >= RFM_MOVE_FW_ID_MAX)
        return -1;
    pthread_mutex_lock(&g_vmu);
    for (i = 0; i < g_web_n; i++)   /* (the installed ids as fw_json last saw them) */
        if (!strcmp(g_web_ids[i], id))
            break;
    if (i < g_web_n && g_nwload < (int)(sizeof g_wload / sizeof g_wload[0])) {
        g_wload[g_nwload].slot = slot;
        g_wload[g_nwload].activate = activate;
        snprintf(g_wload[g_nwload].id, sizeof g_wload[g_nwload].id, "%s", id);
        g_nwload++;
        r = 0;
    }
    pthread_mutex_unlock(&g_vmu);
    return r;
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
            view_info();   /* (the page's labels) */
        }
    }
}

/* the panel's input (the page's, the FM-1's) into slot (-1: the active one); MASTER is the Move's volume, as its knob */
void engine_fm1_in(int slot, int op, int a, int b)
{
    const int s = slot < 0 ? g_active : slot;
    if (op == FWI_MASTER) {
        rfm_move_act_t act;
        rfm1cs_event_t ev;
        memset(&act, 0, sizeof act);
        memset(&ev, 0, sizeof ev);
        act.kind = RFM_MOVE_ACT_MASTER, act.value = (int16_t)b;
        ev.type = RFM1CS_EV_ACTION, ev.act = &act;
        on_action(&ev);
        rfm_move_set_master(&S->core, g_master);
    } else if (s >= 0 && s < NSLOT && SL[s].on)
        fw_in(&SL[s].fw, op, a, b, 0, 0);
}

/* the FM-1's MIDI in (its 5-pin jack): whole messages, as the pads played live (the MIDI FX of their channel's track,
 * then the active slot); system messages straight to the active slot */
void engine_fm1_midi(const uint8_t *m, int n)
{
    int i = 0;
    while (i < n) {
        const uint8_t b = m[i];
        int len = b >= 0xF8 ? 1 : b < 0x80 ? 0 : (b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0 ? 2 : b < 0xF0 ? 3
                  : b == 0xF1 || b == 0xF3 ? 2 : b == 0xF2 ? 3 : 1;
        rfm_move_act_t a;
        if (!len || i + len > n) {
            i++;
            continue;
        }
        memset(&a, 0, sizeof a);
        a.kind = RFM_MOVE_ACT_MIDI, a.track = 0, a.n = (uint8_t)len;
        memcpy(a.bytes, m + i, (size_t)len);
        route_midi(&a);
        i += len;
    }
}

/* ------------------------------------------------------------------ the clock --- */
static void render_block(uint32_t now)
{
    int16_t blk[FW_BLOCK * 2], out[FW_BLOCK * 2];
    const int16_t *ain;
    int s, k, sounding, sends = 0;
    {   /* the clock the modules ask for (host_api_v1: get_bpm, get_clock_status, get_beat_position) */
        const float bpm = (float)rfm_move_seq_bpm10(&S->core) / 10.0f;
        const int run = S->core.seq.playing;
        g_beat = run ? g_beat + (double)FW_BLOCK / FW_FS * bpm / 60.0 : 0.0;
        fx_clock(bpm, run, g_beat);
    }
    rfm1cs_session_advance(S, now, FW_BLOCK, FW_FS);   /* the sequencer: its MIDI stamped within this block (through
                                                         * the MIDI FX: route_midi) */
    midi_fx_tick();
    do_deferred();
    sounding = rfm_move_fw_sounding(&S->core);
    mix_clear(&g_mix, FW_BLOCK);
    for (k = 0; k < RFM_MOVE_FX_SLOTS; k++)
        if (FX[RFM_MOVE_FX_AUDIO][k].run) {
            bus_clear(&g_bus[k], FW_BLOCK);
            sends |= 1 << k;
        }
    ain = ain_take(now);
    for (s = 0; s < NSLOT; s++) {
        slot_t *sl = &SL[s];
        if (!sl->on)
            continue;
        if (!(sounding >> s & 1))
            continue;   /* loaded, past the voices: its clock waits */
        if (fw_has_ain(&sl->fw)) {   /* ain1: the state when it changed, the input right before its audio */
            fw_ain_info(&sl->fw, ain != NULL, engine_ain_latency());
            fw_render_in(&sl->fw, blk, ain);
        } else
            fw_render(&sl->fw, blk);
        if (&sl->fw == g_raw_fw && g_cap_raw && g_cap_raw_n + FW_BLOCK <= g_cap_max) {
            memcpy(g_cap_raw + 2u * g_cap_raw_n, blk, sizeof blk);
            g_cap_raw_n += FW_BLOCK;
        }
        mix_add_lp(&g_mix, blk, FW_BLOCK, sl->level, sl->pan);
        for (k = 0; k < RFM_MOVE_FX_SLOTS; k++)   /* the sends: after its level and pan */
            if (sends >> k & 1)
                bus_add(&g_bus[k], blk, FW_BLOCK, sl->level, sl->pan, sl->send[k]);
        {
            const int p = peak16(blk, FW_BLOCK * 2) * sl->level / 127;
            if (p > sl->peak)
                sl->peak = p;
        }
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
    for (k = 0; k < RFM_MOVE_FX_SLOTS; k++)   /* the returns: each bus through its FX, in place, into the mix */
        if (sends >> k & 1) {
            fxs_t *x = &FX[RFM_MOVE_FX_AUDIO][k];
            int p;
            bus_out(&g_bus[k], blk, FW_BLOCK);
            fx_audio(x->run, blk, FW_BLOCK);
            mix_add_ret(&g_mix, blk, FW_BLOCK);
            if ((p = peak16(blk, FW_BLOCK * 2)) > x->peak)
                x->peak = p;
            if (p > x->rpeak)
                x->rpeak = p;
        }
    mix_out(&g_mix, out, FW_BLOCK);
    for (s = 0; s < FW_BLOCK * 2; s++) {
        const int v = out[s] < 0 ? -out[s] : out[s];
        if (v > g_peak_acc)
            g_peak_acc = v;
        if (v > g_peak_win)
            g_peak_win = v;
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
    static uint32_t last_save_check, last_stat, last_log;
    int s;
    if (g_session_on && !g_index_busy && !g_dl_busy && (int32_t)(now - g_next_index_ms) >= 0) {
        /* the catalog's index: at start (once the controls are up), then every few hours; offline: a while later */
        st_job_t *j = st_job_new(J_NET_INDEX);
        j->used = g_cat ? g_cat->index_hash : 0;
        j->text = keep_list();
        st_post(j);
        g_index_busy = 1;
    }
    if (g_sweep && !g_dl_busy && !g_index_busy) {   /* older versions no slot runs any more: their files go */
        st_job_t *j = st_job_new(J_NET_SCAN);
        j->text = keep_list();
        st_post(j);
        g_sweep = 0;
    }
    if (now - last_log >= 1000u) {   /* the firmwares' held-back lines said; the log file kept under its cap */
        struct stat st;
        last_log = now;
        for (s = 0; s < NSLOT; s++)
            if (SL[s].on)
                fw_log_flush(&SL[s].fw);
        if (g_log_cap > 0 && fstat(2, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > g_log_cap) {
            if (!g_rotating) {
                st_job_t *j = st_job_new(J_LOG_ROTATE);
                j->value = g_log_cap;
                st_post(j);
                g_rotating = 1;
            }
        } else
            g_rotating = 0;
    }
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
        char tmp[sizeof g_status], esc[2][600];
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
                              "\"states\":%u,\"dead\":%d", s ? "," : "", sl->fw.id, sl->cpu_permille / 10.0,
                              sl->fw.worst_us, sl->level, sl->serial, S->core.fw.st_used[s], sl->fw.dead);
                if (sl->fw.wasi && sl->fw.wasi->fs) {   /* a Fantasy CFW's file API (fsvc.h) */
                    fsvc_stats_t fst;
                    fsvc_stats(sl->fw.wasi->fs, &fst);
                    n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"fs_req\":%u,\"fs_done\":%u,\"fs_refused\":%u,"
                                  "\"fs_read\":%llu,\"fs_written\":%llu,\"mem_mb\":%llu", fst.made, fst.served,
                                  fst.refused + fst.busy, (unsigned long long)fst.bytes_in,
                                  (unsigned long long)fst.bytes_out, (unsigned long long)(sl->fw.mem_size >> 20));
                }
                if (fw_has_ain(&sl->fw))   /* ain1: the state told, its latency, the blocks fed, the last block's peak */
                    n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"ain\":%u,\"ain_lat\":%u,\"ain_blocks\":%u,"
                                  "\"ain_peak\":%d", sl->fw.ain_state, sl->fw.ain_lat, sl->fw.ain_blocks, sl->fw.ain_peak);
                n += snprintf(tmp + n, sizeof tmp - (size_t)n, "}");
            } else
                n += snprintf(tmp + n, sizeof tmp - (size_t)n, "%snull", s ? "," : "");
        }
        n += snprintf(tmp + n, sizeof tmp - (size_t)n, "],\"ain_live\":%d,\"ain_in\":%u,\"ain_under\":%u,"
                      "\"ain_dropped\":%u,\"ain_over\":%u,\"ain_latency\":%u", g_ain_live, g_ain_in, g_ain_under,
                      g_ain_dropped, g_ain_over, engine_ain_latency());
        {   /* the FX: CPU per module over the last second; past the budget twice running: refused ("Too busy") */
            int kind, k;
            uint32_t total = 0;
            for (kind = 0; kind < FX_KINDS; kind++)
                for (k = 0; k < RFM_MOVE_FX_SLOTS; k++) {
                    fxs_t *x = &FX[kind][k];
                    const fx_mod_t *md = fx_inst_mod(x->run);
                    if (x->run) {
                        const uint64_t ns = fx_ns(x->run) - x->ns_mark;
                        x->ns_mark = fx_ns(x->run);
                        x->cpu_permille = (uint32_t)(ns / 1000u / (dt ? dt : 1));
                        total += x->cpu_permille;
                        if (x->cpu_permille > (uint32_t)g_fx_budget) {
                            if (++x->over >= 2) {
                                char why[120];
                                snprintf(why, sizeof why, "Too busy: %s took %.1f %% of a core (the budget: %.1f %%)",
                                         md->name, x->cpu_permille / 10.0, g_fx_budget / 10.0);
                                fx_refuse(kind, k, why);
                                md = NULL;
                            }
                        } else
                            x->over = 0;
                    } else
                        x->cpu_permille = 0;
                    x->rpeak_last = x->rpeak, x->rpeak = 0;
                    n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"%cfx%d\":\"%s\",\"%cfx%d_cpu\":%.1f,\"%cfx%d_busy\":%d",
                                  kind ? 'a' : 'm', k, md ? md->id : "", kind ? 'a' : 'm', k, x->cpu_permille / 10.0,
                                  kind ? 'a' : 'm', k, x->opening);
                }
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"fx_cpu\":%.1f,\"fx_mods\":%d,\"core_active\":%d,"
                          "\"rpeak0\":%d,\"rpeak1\":%d,\"rpeak2\":%d,\"fx_page\":%d,\"fx_open\":%d", total / 10.0,
                          g_fxlib ? g_fxlib->n[0] + g_fxlib->n[1] : 0, rfm_move_fw_active(&S->core),
                          FX[1][0].rpeak_last, FX[1][1].rpeak_last, FX[1][2].rpeak_last, S->core.s.fx_page,
                          S->core.fw.fx && S->core.seq.projects);
            g_mix_ver++;   /* (the page's CPU figures, once a second) */
        }
        for (s = 0; s < NSLOT; s++)   /* (flat, for scripts) */
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"id%d\":\"%s\",\"ver%d\":\"%s\",\"boot%d\":%u,\"states%d\":%u,\"notes%d\":%u,"
                          "\"level%d\":%d,\"pan%d\":%d", s,
                          SL[s].on ? SL[s].fw.id : "", s, SL[s].on ? SL[s].fw.version : "", s,
                          SL[s].on ? SL[s].serial : 0, s, S->core.fw.st_used[s], s, SL[s].notes, s, SL[s].level, s, SL[s].pan);
        /* the catalog and its downloads */
        mj_escape(g_dl.err, esc[0], sizeof esc[0]);
        mj_escape(g_index_err, esc[1], sizeof esc[1]);
        n += snprintf(tmp + n, sizeof tmp - (size_t)n,
                      ",\"cat_n\":%d,\"cat_installed\":%d,\"cat_from\":%d,\"cat_gen\":\"%s\",\"dl_id\":\"%s\","
                      "\"dl_state\":%d,\"dl_pct\":%d,\"dl_ok\":%u,\"dl_failed\":%u,\"dl_err\":\"%s\",\"index_ok\":%u,"
                      "\"index_failed\":%u,\"index_err\":\"%s\"",
                      g_cat ? g_cat->n : 0, g_cat ? g_cat->nfw : 0, g_cat ? g_cat->from : 0, g_cat ? g_cat->generated : "",
                      g_dl.id, g_dl.state, g_dl.pct, g_dl.ok, g_dl.failed, esc[0], g_dl.index_ok, g_dl.index_failed,
                      esc[1]);
        n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"fm1\":");
        if (n < (int)sizeof tmp)
            n += fm1link_json(tmp + n, (int)sizeof tmp - n);
        if (n > (int)sizeof tmp - 2)
            n = (int)sizeof tmp - 2;
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
            __atomic_store_n(&g_out_open, 1, __ATOMIC_RELEASE);
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
            /* the page's firmware picks: rfm1cs's FW_LOAD, the same path (do_deferred) */
            pthread_mutex_lock(&g_vmu);
            for (i = 0; i < g_nwload && g_ndefer < (int)(sizeof g_defer / sizeof g_defer[0]); i++) {
                defer_t *d = &g_defer[g_ndefer++];
                memset(d, 0, sizeof *d);
                d->a.kind = RFM_MOVE_ACT_FW_LOAD;
                d->a.slot = (uint8_t)g_wload[i].slot;
                g_activate = g_wload[i].activate ? g_wload[i].slot : -1;
                snprintf(d->id, sizeof d->id, "%s", g_wload[i].id);
            }
            g_nwload = 0;
            pthread_mutex_unlock(&g_vmu);
            {   /* tests: sequencer MIDI from outside (engine_test_midi) */
                struct { uint8_t track, b[3]; } tm[32];
                int nt;
                pthread_mutex_lock(&g_vmu);
                nt = g_ntmidi;
                memcpy(tm, g_tmidi, sizeof tm[0] * (size_t)nt);
                g_ntmidi = 0;
                pthread_mutex_unlock(&g_vmu);
                for (i = 0; i < nt; i++) {
                    rfm_move_act_t a;
                    memset(&a, 0, sizeof a);
                    a.kind = RFM_MOVE_ACT_MIDI, a.track = tm[i].track, a.n = 3;
                    memcpy(a.bytes, tm[i].b, 3);
                    route_midi(&a);
                }
            }
            for (i = 0; i < n; i++) {
                if (pin[i].op >= ENG_OP_ACTIVE && pin[i].op <= ENG_OP_LAST)
                    page_edit(pin[i].op, pin[i].a, pin[i].b);
                else
                    engine_fm1_in(-1, pin[i].op, pin[i].a, pin[i].b);
            }
        }
        /* the FM-1 on USB-A (fm1link.c): its panel and MIDI in, the active slot's screen and LEDs out */
        fm1link_tick(now, g_active, SL[g_active].on ? &SL[g_active].fw : NULL);
        if (fm1link_changed()) {
            view_info();
            g_mix_ver++;
        }
        rfm1cs_session_tick(S, now);
        do_deferred();
        while (mr_ring_count(&g_ring) < lead && g_run)
            render_block(now_ms());
        publish(now);
        do_deferred();
        housekeeping(now);
        view_live(now);
        {   /* (the Move's FX setup opened, closed or on another page: no action says so) */
            static int sig = -1;
            const rfm_move_fw_state_t *f = &S->core.fw;
            int g = S->core.s.fx_page | (f->fx && S->core.seq.projects) << 4 | f->fx_focus << 5 | !!f->fx_pages << 7, k;
            g = g * 31 + f->mix_par;   /* (the Mixer's choice) */
            for (k = 0; k < 2 * RFM_MOVE_FX_SLOTS; k++)   /* (and the slots' pages) */
                g = g * 31 + f->fx_pg[k / RFM_MOVE_FX_SLOTS][k % RFM_MOVE_FX_SLOTS];
            if (g != sig)
                sig = g, g_mix_ver++;
        }
        {
            static uint32_t last_mix;
            if (g_mix_ver != g_mix_made && now - last_mix >= 50u) {   /* (at most 20 a second) */
                last_mix = now;
                g_mix_made = g_mix_ver;
                mix_json();
            }
        }
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
        fm1link_stop(now_ms());
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
    {
        const char *e = getenv("RFM14_TEST_BOOT_MS");
        g_test_boot_ms = e ? (uint32_t)atoi(e) : 0;
        g_no_preroll = getenv("RFM14_NO_PREROLL") && *getenv("RFM14_NO_PREROLL") == '1';
        g_log_cap = C.log_cap > 0 ? C.log_cap : C.log_cap < 0 ? 0 : LOG_CAP_DEFAULT;
    }
    if (st_fw_install_bundled(C.bundled) > 0)
        fprintf(stderr, "engine: bundled firmwares installed into %s/firmwares\n", C.root);
    cat_config(C.bundled_index, C.catalog_url);
    if (!(g_cat = cat_build(NULL)))
        return -1;
    fprintf(stderr, "engine: %d firmware(s) installed; the catalog lists %d (index: %s %s)\n", g_nfw, g_cat->n,
            g_cat->from == 2 ? "downloaded" : g_cat->from == 1 ? "bundled" : "none", g_cat->generated);
    fw_json();
    g_lead = (uint32_t)(C.lead_blocks < 4 ? 4 : C.lead_blocks > 8 ? 8 : C.lead_blocks) * FW_BLOCK;
    if (mr_ring_init(&g_ain, 4, AIN_RING) != 0 || mr_ring_init(&g_ring, 4, RING_FRAMES) != 0 || !(g_tx = malloc(TX_CAP)) || !(S = calloc(1, sizeof *S)))
        return -1;
    {   /* the FM-1 on USB-A: its hooks are set before Move Remote's core starts (main.c: after engine_start) */
        fm1link_cfg_t lc;
        memset(&lc, 0, sizeof lc);
        lc.off = C.fm1_off, lc.bps = C.fm1_bps, lc.usba_cap = C.fm1_cap;
        if (fm1link_init(&lc) != 0)
            fprintf(stderr, "engine: the FM-1 link did not start\n");
    }
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
    rfm_move_fw_list(&S->core, g_cat->list, g_cat->n);
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
    for (s = 0; s < NSLOT; s++)   /* (before the project: its FX setup's MIX and ROUTE come as it loads) */
        SL[s].filt = 0xFFFF, SL[s].level = 127, SL[s].pan = 64;
    {   /* the Schwung FX modules the user installed: the core's FX setup lists them (before the project names them) */
        int kind, k;
        g_fx_budget = C.fx_budget > 0 ? C.fx_budget : 200;
        g_fxlib = fx_scan(C.fx_dir);
        g_fx_json = fx_lib_json(g_fxlib);
        for (kind = 0; kind < FX_KINDS; kind++) {
            for (k = 0; k < RFM_MOVE_FX_SLOTS; k++)
                FX[kind][k].want = -1, FX[kind][k].filt = 0xFFFF;
            rfm_move_fx_modules(&S->core, kind, g_fxlib->core[kind], g_fxlib->n[kind]);
        }
        st_fx_state_path(p, sizeof p, open_slot);   /* (the open project's modules' own state) */
        {
            size_t n;
            uint8_t *t = st_read(p, &n);
            fx_state_take((const char *)t);
            free(t);
        }
    }
    used = st_projects_scan(open_slot, &blob, &nb);
    rfm1cs_session_projects_init(S, now_ms(), used, open_slot, blob, nb);
    free(blob);
    if (st_worker_start(used) != 0)
        return -1;
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
    g_next_index_ms = g_session_at + 2000u;
    if (!C.splash_ms) {
        rfm1cs_session_start(S, now_ms());   /* -> EV_OPEN: the socketpair, adopted by Move Remote's core */
        g_session_on = 1;
        __atomic_store_n(&g_out_open, 1, __ATOMIC_RELEASE);
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
