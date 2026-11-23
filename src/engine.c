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
#include <dirent.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/futex.h>
#endif

#include "../third_party/abletonmoveremote/common/ring.h"
#include "catalog.h"
#include "engine_dsp.h"
#include "fm1link.h"
#include "midiout.h"
#include "announce.h"
#include "fmreader.h"
#include "fsvc.h"
#include "fwrt.h"
#include "fx.h"
#include "memsnap.h"
#include "minijson.h"
#include "mr_core.h"
#include "projmem.h"
#include "rfm1cs/session.h"
#include "storage.h"
#include "wasm-rt.h"

#define NSLOT RFM_MOVE_FW_SLOTS
#define RING_FRAMES 4096
#define AUTOSAVE_MS 2000u      /* a firmware's flash saved this long after its last change */
#define MEM_AUTOSAVE_MS 30000u /* (README "The working memory") the active state's and the open project's working
                                * memory: written again at most this often while the firmware is played and nothing
                                * else wrote it */
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

static int engine_audio_fw(int16_t *out, int frames);
int engine_audio(void *ctx, int16_t *out, int frames)
{
    (void)ctx;
    /* Schwung's screen reader over the firmwares (announce.h; nothing when it is off) */
    return ann_mix(out, frames, engine_audio_fw(out, frames));
}

static int engine_audio_fw(int16_t *out, int frames)
{
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
    int pk_acc, pk_last;       /* this second's peak so far, the last second's (the status: tests) */
    uint32_t mini_writes, mini_ms;   /* the page's mini screen: the LCD writes it was made from, when */
    uint32_t rend_avg_us, rend_max_us;   /* its blocks' render time over the last second (the log's) */
    uint64_t c1_job_ns, c1_wait_ns;      /* core 1's counters when last looked at */
    uint64_t sp_audio, sp_tick, sp_frame;   /* fwrt's split (rfm1_audio, tick + idle, frame) when last looked at */
    void *frames_inst;                   /* the instance the screen rate was last given to, and that rate (FWI_FRAMES) */
    int frames_ms;
    uint32_t sp_nframe;
    uint32_t c1_cpu;                     /* core 1's jobs, permille of a core over the last second */
    /* tk1, the takeover (FM1VST docs/RFM1-TAKEOVER.md): what rfm1cs was last told from its record (tk_poll) */
    int tk_known;                        /* rfm_move_set_takeover was called for this firmware */
    uint32_t tk_claims, tk_scr, tk_led, tk_say;
    char tk_name[16];
    uint32_t tk_in, tk_states;           /* TK_INPUT / TK_STATE actions given to it (the status: tests) */
    /* (rfm1cs 0.13.0, RFM_MOVE_CAP_PROJFW) whose memory this is (projmem.h): every write of it (slot_write) also goes
     * to that project's copy and to the active state, when they are behind it */
    int pc_proj;                         /* the project whose copy (move-project-NN.fwK.bin) it is, -1 none */
    uint32_t pc_seen;                    /* .. flash_dirty when that copy was last written or read */
    int pc_stale;                        /* .. the copy is behind it whatever flash_dirty says */
    int st_cur;                          /* the active state (states/<id>/state-KK.bin), -1 none */
    uint32_t st_seen;
    int st_stale;
    uint32_t pfw_gen;                    /* a project's load of this slot (J_PFW_OPEN): a later one wins */
    uint8_t *undo;                       /* STATE_NEW's undo: the memory before it (NULL none), its state, its id */
    uint32_t undo_n;
    int undo_k;
    char undo_id[RFM_MOVE_FW_ID_MAX];
    ms_snap_t *undo_snap;                /* .. and its working memory (NULL: none, its module's are not whole) */
    /* (README "The working memory") the working memory (memsnap.h): when it was last written to the active state and
     * the project's copy (a snapshot), the firmware's input count then (fw_t.n_in: played since?), the snapshots
     * taken and the last one's cost; restored: this instance came from a snapshot (not booted from its flash) */
    uint32_t mem_ms, in_seen;
    uint32_t snaps, snap_us, restored;
} slot_t;

static slot_t SL[NSLOT];
static int g_active;
static int g_proj_open = -1;                 /* (rfm1cs 0.13.0) the project the slots' memories belong to */
static uint32_t g_pfw_shown, g_pfw_ended;    /* .. its loading screen came up, went (the status: tests) */
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
static int g_nest;                           /* (inside render_block or one of the core's actions: slot_snap copies
                                             * without rendering ahead) */

/* ---- Schwung FX (fx.h): 3 MIDI FX in series on the sequencer's MIDI, 3 audio FX on send / return buses */
typedef struct {
    fx_inst_t *run;            /* the instance running (the engine's), NULL none */
    int want;                  /* the module asked for (FX_LOAD), -1 none */
    uint32_t gen;              /* .. its open's generation (a later FX_LOAD drops an earlier open) */
    int opening;               /* an open is under way */
    uint8_t val[RFM_MOVE_FX_PARAMS];   /* FX_PARAM's values (applied to an instance as it comes in) */
    uint16_t filt;             /* MIDI FX: the tracks it acts on (ROUTE) */
    int last_track;            /* MIDI FX: the track of the last message it took (its generated notes' track) */
    int last_live;             /* MIDI FX: where the last message it took was going: -1 the sequencer's (the filters),
                                * else the slot the pads played live (its own notes go there too) */
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
/* tests: messages for Out (engine_test_midiout), sent from the next block; and the active slot's packets as messages
 * (--midi-out) */
static uint8_t g_tmo[8192];
static int g_ntmo;
static midiout_asm_t g_mo_asm[NSLOT];   /* (one each: a slot's SysEx put together on its own) */
/* (rfm1cs 0.6.6) each slot's MIDI routes, from RFM_MOVE_ACT_ROUTE: bit t its MIDI out to slot t too, RFM_MOVE_MROUTE_OUT
 * to Out (midiout.h); engine thread only */
static uint8_t g_mroute[NSLOT];

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
static char g_status[12288];
static char *g_fw_json;                      /* engine_firmwares: made whenever the installed ones change */
static char *g_cat_json;                     /* engine_catalog: the whole catalog, made with g_fw_json ("{...,"
                                              * "\"firmwares\":[...]": the favourites go after it) */
static char g_fav_json[RFM_MOVE_FW_LIB_MAX * (RFM_MOVE_FW_ID_MAX + 3) + 4] = "[]";   /* the library's favourites */
static char g_wdl[RFM_MOVE_FW_ID_MAX];       /* the page's download (engine_download), "" none */
static char (*g_web_ids)[RFM_MOVE_FW_ID_MAX];   /* the installed ids (engine_load's check) */
static int g_web_n;
static struct { int slot, activate; char id[RFM_MOVE_FW_ID_MAX]; } g_wload[8];   /* the page's loads (engine_load) */
static int g_activate = -1;                  /* a page's load that makes its slot the active one once it runs */
static int g_nwload;
static char g_fail_id[RFM_MOVE_FW_ID_MAX];   /* the last firmware that failed to load, and how many failed so far */
static uint32_t g_fail_seq;
/* the page's States and projects (engine_store): its requests (g_vmu), their outcomes (g_vmu: a ring the web thread
 * reads), the ones under way (the engine's thread), the States and projects as JSON (g_vmu) and its version */
static struct { eng_store_t r; int id; } g_sq[8];
static int g_nsq, g_sq_next;
static struct { int id, st; char msg[200]; } g_sdone[32];
static int g_sdone_at;
typedef struct { int id, op, slot, k, to, flags; char err[200]; } spend_t;
static spend_t g_spend[16];
static char *g_store_json;
static uint32_t g_store_ver;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
static uint64_t now_ns(void);

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
static void cat_json(void);
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
    cat_json();
}

/* base64 of n bytes into out (4 * ((n + 2) / 3) + 1 bytes) */
static void b64(const uint8_t *in, int n, char *out)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i, o = 0;
    for (i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = A[v >> 18 & 63], out[o++] = A[v >> 12 & 63];
        out[o++] = i + 1 < n ? A[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? A[v & 63] : '=';
    }
    out[o] = 0;
}

/* the page's firmware browser (engine_catalog): every entry of the catalog (the index's and the installed ones), as
 * the Move's own browser lists them: id, name, version, the version installed ("" none), the description, the
 * categories, a Fantasy CFW, downloadable (in the index) and its bytes, the thumbnail (64 x 64, 1 bit, base64) */
static void cat_json(void)
{
    static const char *CATS[] = { "Synth", "Groovebox", "Chords", "Drums", "Other" };
    const int ne = g_cat ? g_cat->n : 0;
    size_t cap = 512 + (size_t)ne * (CAT_DESC * 2 + 1024), n = 0;
    char *o = malloc(cap), th[4 * ((RFM_MOVE_FW_THUMB_BYTES + 2) / 3) + 1];
    int i, k;
    if (!o)
        return;
    n += (size_t)snprintf(o + n, cap - n, "{\"generated\":\"%s\",\"from\":%d,\"firmwares\":[", g_cat ? g_cat->generated : "",
                          g_cat ? g_cat->from : 0);
    for (i = 0; i < ne; i++) {
        const cat_ent_t *e = &g_cat->e[i];
        char name[100], desc[CAT_DESC * 2];
        int first = 1;
        mj_escape(e->name, name, sizeof name);
        mj_escape(e->desc, desc, sizeof desc);
        if (e->has_thumb)
            b64(e->thumb, RFM_MOVE_FW_THUMB_BYTES, th);
        else
            th[0] = 0;
        n += (size_t)snprintf(o + n, cap - n,
                              "%s{\"id\":\"%s\",\"name\":\"%s\",\"version\":\"%s\",\"installed\":\"%s\",\"description\":\"%s\","
                              "\"virtual\":%s,\"indexed\":%s,\"bytes\":%llu,\"thumb\":\"%s\",\"categories\":[",
                              i ? "," : "", e->id, name, e->version, e->installed, desc, e->virt ? "true" : "false",
                              e->indexed && e->files ? "true" : "false", (unsigned long long)e->bytes, th);
        for (k = 0; k < 5; k++)
            if (e->cats >> k & 1) {
                n += (size_t)snprintf(o + n, cap - n, "%s\"%s\"", first ? "" : ",", CATS[k]);
                first = 0;
            }
        n += (size_t)snprintf(o + n, cap - n, "]}");
        if (n >= cap - 16)
            break;
    }
    snprintf(o + n, cap - n, "]");
    pthread_mutex_lock(&g_vmu);
    free(g_cat_json);
    g_cat_json = o;
    pthread_mutex_unlock(&g_vmu);
}

/* the library's favourites in their order (the Move's Favorites), as a JSON array of ids: into g_fav_json when they
 * changed (the engine's thread: the core's library is its) */
static void fav_json(void)
{
    const rfm_move_t *m = &S->core;
    char t[sizeof g_fav_json];
    int k, n = 0;
    n += snprintf(t + n, sizeof t - (size_t)n, "[");
    for (k = 0; k < m->fw.nfav && k < RFM_MOVE_FW_LIB_MAX; k++) {
        const unsigned r = m->fw.fav[k];
        if (r < m->fw.nrec && m->fw.rec[r].id[0])
            n += snprintf(t + n, sizeof t - (size_t)n, "%s\"%.*s\"", n > 1 ? "," : "", RFM_MOVE_FW_ID_MAX - 1, m->fw.rec[r].id);
        if (n >= (int)sizeof t - RFM_MOVE_FW_ID_MAX - 4)
            break;
    }
    snprintf(t + n, sizeof t - (size_t)n, "]");
    pthread_mutex_lock(&g_vmu);
    if (strcmp(t, g_fav_json))
        memcpy(g_fav_json, t, sizeof t);
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
 * What comes out goes to the firmwares: the sequencer's (live -1) to every sounding slot whose track filter has the
 * track; the pads played live (live: the slot, the active one, or a latched note's own: rfm1cs 0.5.4 dest) to that
 * slot only, as without MIDI FX */
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
    if (live >= 0) {
        if (live < NSLOT && SL[live].on)
            slot_midi(live, b, n, frame);
        return;
    }
    for (s = 0; s < NSLOT; s++)
        if (SL[s].on && (rfm_move_fw_sounding(&S->core) >> s & 1) && (SL[s].filt >> (track - 1) & 1))
            slot_midi(s, b, n, frame);
}

/* (rfm1cs 0.6.6) a slot's MIDI out (fw_midi_out's packets) where the Routing page sends it: its channel messages into
 * each routed slot (from the start of that slot's next block: a slot before it has played this one), everything to
 * Out (midiout.h: its real-time bytes filtered there). The core never sets a cycle, and nothing routed is routed on:
 * a target's own MIDI out is its own */
static void route_out(int s, const uint32_t *pk, int n)
{
    const int r = g_mroute[s];
    int i, t;
    if (!n || !r)
        return;
    if (r & RFM_MOVE_MROUTE_OUT)
        midiout_send_pkts(&g_mo_asm[s], pk, n);
    if (!(r & ((1 << NSLOT) - 1)))
        return;
    for (i = 0; i < n; i++) {
        const uint8_t st = (uint8_t)(pk[i] >> 8);
        const uint8_t m[3] = { st, (uint8_t)(pk[i] >> 16 & 0x7F), (uint8_t)(pk[i] >> 24 & 0x7F) };
        const int len = (st & 0xE0) == 0xC0 ? 2 : 3;   /* (program change, channel pressure: 2 bytes) */
        if (st < 0x80 || st >= 0xF0 || (pk[i] & 0x0F) < 0x8 || (pk[i] & 0x0F) > 0xE)
            continue;   /* (channel messages only: CIN 8..E) */
        for (t = 0; t < NSLOT; t++)
            if (t != s && (r >> t & 1) && SL[t].on)
                slot_midi(t, m, len, 0);
    }
}

static void route_midi(const rfm_move_act_t *a)
{
    if (!a->n)
        return;
#ifdef RFM_MOVE_DEST_ALL
    if (a->track == 0 && a->dest == RFM_MOVE_DEST_ALL) {   /* (rfm1cs 0.9.0) the sequencer's clock (FA, F8, FC): every
                                                         * loaded firmware, so their own sequencers and arps follow */
        int t;
        for (t = 0; t < NSLOT; t++)
            if (SL[t].on)
                slot_midi(t, a->bytes, a->n, a->frame);
        return;
    }
#endif
    if (a->track == 0) {   /* the pads played live (MIDI, Control, the sequencer's live notes): through the MIDI FX as
                            * the track of their channel (the sequencer's tracks play on their own channels), then to
                            * the active slot, or (rfm1cs 0.5.4) dest's: a latched note's off goes to the slot it was
                            * played on, whichever is active now; system messages straight there */
        const int to = a->dest >= 1 && a->dest <= NSLOT ? a->dest - 1 : g_active;
        if (a->bytes[0] >= 0x80 && a->bytes[0] < 0xF0 && a->n <= 3)
            chain_midi(0, (a->bytes[0] & 0x0F) + 1, to, a->bytes, a->n, a->frame);
        else if (SL[to].on)
            slot_midi(to, a->bytes, a->n, a->frame);
        return;
    }
    chain_midi(0, a->track, -1, a->bytes, a->n, a->frame);   /* the sequencer's: the MIDI FX, then the filters */
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
    x->ns_mark = fx_ns(x->run), x->over = 0, x->last_track = 0, x->last_live = -1;
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


static void slot_write(int s, int sync, int flash, int mem, ms_snap_t **keep);

/* (rfm1cs 0.6.12) the Speech mode (Off / On / Jog, Global Settings): the host's own words only while open; closing
 * cuts off what is playing; Jog's touch beginning: a firmware that speaks for itself asked for its screen (WHERE) */
static void speech_sync(void)
{
#ifdef RFM_MOVE_HAS_SPEECH
    static int was;
    int open;
    if (!S)
        return;
    open = rfm_move_speech_open(&S->core);
    ann_set_open(open);
    if (open && !was && rfm_move_speech_mode(&S->core) == RFM_MOVE_SPEECH_JOG && fmr_speaks())
        fmr_where();
    was = open;
#else
    ann_set_open(1);
#endif
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
    case RFM_MOVE_ACT_TK_INPUT:   /* (rfm1cs 0.7.0) a claimed control of the slot's takeover: rfm1_tk_input, in time
                                   * order with its ticks (the record read after its block: tk_poll) */
        if (a->slot < NSLOT && SL[a->slot].on && ++SL[a->slot].tk_in)
            fw_in(&SL[a->slot].fw, FWI_TK_INPUT, 0, 0,
                  (uint32_t)a->pkt[0] | (uint32_t)a->pkt[1] << 8 | (uint32_t)a->pkt[2] << 16 | (uint32_t)a->pkt[3] << 24, 0);
        break;
    case RFM_MOVE_ACT_TK_STATE:   /* .. shown with these claims (down 1) or gone: rfm1_tk_state */
        if (a->slot < NSLOT && SL[a->slot].on && ++SL[a->slot].tk_states)
            fw_in(&SL[a->slot].fw, FWI_TK_STATE, a->down ? 1 : 0, a->down ? a->tk_claims : 0, a->tk_flags, 0);
        break;
    case RFM_MOVE_ACT_MASTER: {   /* the volume knob: the master of every firmware */
        int s;
        g_master = a->value < 0 ? 0 : a->value > 1023 ? 1023 : a->value;
#ifndef RFM_MOVE_HAS_SPEECH   /* (else rfm1cs says its volume popup) */
        ann_say(ANN_VALUE, "Volume %d%%", (g_master * 100 + 511) / 1023);
#endif
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
        int s;
        /* (rfm1cs 0.13.0) the open project's firmware copies brought up to date before it is saved (as they are),
         * copied or cut (synced: the buffer gets what the firmwares hold now); the storage thread moves them with it */
        if (a->index == g_proj_open && (a->kind == RFM_MOVE_ACT_PROJECT_SAVE || a->kind == RFM_MOVE_ACT_PROJECT_COPY ||
                                        a->kind == RFM_MOVE_ACT_PROJECT_CUT))
            for (s = 0; s < NSLOT; s++)
                slot_write(s, a->kind != RFM_MOVE_ACT_PROJECT_SAVE, 0, a->kind != RFM_MOVE_ACT_PROJECT_SAVE, NULL);
        if (a->kind == RFM_MOVE_ACT_PROJECT_DELETE || a->kind == RFM_MOVE_ACT_PROJECT_CUT)
            for (s = 0; s < NSLOT; s++)   /* (its copies are gone: written again if it is saved again) */
                if (SL[s].pc_proj == a->index)
                    SL[s].pc_stale = 1;
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
        for (s = 0; s < NSLOT; s++) {   /* (0.6.6) the MIDI routes: a route gone, its notes let go (All Notes Off) */
            const int gone = g_mroute[s] & ~a->mroute[s];
            int t, c;
            for (t = 0; t <= NSLOT; t++) {
                const int bit = t < NSLOT ? 1 << t : RFM_MOVE_MROUTE_OUT;
                if (!(gone & bit) || (t < NSLOT && !SL[t].on))
                    continue;
                for (c = 0; c < 16; c++) {
                    const uint8_t m[3] = { (uint8_t)(0xB0 | c), 123, 0 };
                    if (t < NSLOT)
                        slot_midi(t, m, 3, 0);
                    else
                        midiout_send(m, 3);
                }
            }
            g_mroute[s] = a->mroute[s];
        }
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
    case RFM_MOVE_ACT_PROJECT_FW:   /* (rfm1cs 0.13.0) */
    case RFM_MOVE_ACT_STATE_COPY:
    case RFM_MOVE_ACT_STATE_PASTE:
    case RFM_MOVE_ACT_STATE_NEW:
    case RFM_MOVE_ACT_STATE_UNDO:
        if (g_ndefer < (int)(sizeof g_defer / sizeof g_defer[0])) {
            defer_t *d = &g_defer[g_ndefer++];
            d->a = *a;
            d->a.data = NULL;
            snprintf(d->id, sizeof d->id, "%.*s", a->data ? (int)a->len : 0, a->data ? (const char *)a->data : "");
        }
        break;
#ifdef RFM_MOVE_HAS_SPEECH
    case RFM_MOVE_ACT_SAY:   /* (rfm1cs's words for what the Move's screen shows: the screen reader; gated by the core
                              * on its Speech mode, its mode's own change said in any mode) */
        speech_sync();       /* (first: a mode just closed cuts what was playing, not these words) */
        if (a->data && a->len)
            ann_core(a->index == RFM_MOVE_SAY_EVENT ? ANN_EVENT : ANN_VALUE, (const char *)a->data, (int)a->len);
        break;
#endif
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
        g_nest++;   /* (the core's callback: no rendering from in here) */
        on_action(ev);
        g_nest--;
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
        if (ev->type == RFM1CS_EV_BYE)   /* (best effort: Move Remote stops the audio a moment later) */
            ann_now("Leaving RemoteFM1");
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

/* (README "The working memory") a firmware's working memory now, for the active state and the project's copy: NULL
 * when its module's snapshots are not whole (snapsafe.h: flash only), or it is not running. Taken between two blocks
 * on the engine's thread, a copy of all its memory (8 MB for most): the ring is filled a little further first (as
 * preroll does for a start, but only by twice what the last copy took), so the audio thread has the other slots'
 * sound meanwhile and no block is late; not from inside a block or one of the core's actions (g_nest) */
static uint32_t g_snap_us_last, g_snap_us_max, g_restore_us_max;
static uint32_t g_mem_autosave_ms = MEM_AUTOSAVE_MS;   /* (tests: RFM14_TEST_MEM_MS) */
static void render_block(uint32_t now);
static ms_snap_t *slot_snap(int s)
{
    slot_t *sl = &SL[s];
    ms_snap_t *m;
    uint64_t t0;
    if (!sl->on || !fw_snap_able(&sl->fw))
        return NULL;
    if (!g_no_preroll && !g_nest) {   /* (the copy's time: the last one's, or ~1.5 GB/s, the Move's order) */
        const uint64_t est = g_snap_us_last > sl->fw.mem_size / 1500u ? g_snap_us_last : sl->fw.mem_size / 1500u;
        uint64_t room = est * 2u * FW_FS / 1000000u + FW_BLOCK;
        uint32_t want;
        if (room < 4u * FW_BLOCK)
            room = 4u * FW_BLOCK;
        want = g_lead + (uint32_t)(room < PREROLL_FRAMES ? room : PREROLL_FRAMES);
        if (want > PREROLL_FRAMES)
            want = PREROLL_FRAMES;
        while (g_run && mr_ring_count(&g_ring) + FW_BLOCK <= want)
            render_block(now_ms());
    }
    t0 = now_ns();
    if (!(m = ms_new(sl->fw.mem_size)))
        return NULL;
    if (fw_snap_take(&sl->fw, &m->h, m->mem) != 0) {
        ms_free(m);
        return NULL;
    }
    g_snap_us_last = (uint32_t)((now_ns() - t0) / 1000u);
    if (g_snap_us_last > g_snap_us_max)
        g_snap_us_max = g_snap_us_last;
    sl->snaps++, sl->snap_us = g_snap_us_last;
    sl->mem_ms = now_ms(), sl->in_seen = sl->fw.n_in;
    return m;
}

/* the firmware's memory now (sync: first what it would save lazily): to states/<id>/flash.bin (flash), and (rfm1cs
 * 0.13.0) to the copy of the project it belongs to and to its active state, each only when behind it (1 MB files are
 * not written again unchanged), or (mem) whatever its flash says; those two always with its working memory (README
 * "The working memory": a snapshot, slot_snap). One image, one job, up to three files. keep: the caller gets the
 * working memory too (a reference, NULL when none) */
static void slot_write(int s, int sync, int flash, int mem, ms_snap_t **keep)
{
    slot_t *sl = &SL[s];
    fw_t *fw = &sl->fw;
    const uint8_t *img;
    uint32_t d;
    int pc, st, nt = 0;
    st_job_t *j;
    char *to[3];
    ms_snap_t *snap = NULL;
    if (keep)
        *keep = NULL;
    if (!sl->on || (fw->dead && !fw->halted))
        return;
    if (sync && !fw->dead)
        fw_in_now(fw, FWI_SYNC, 0, 0, 0);
    if (!(img = fw_flash(fw)))
        return;
    d = fw_flash_dirty(fw);
    pc = sl->pc_proj >= 0 && (sl->pc_stale || d != sl->pc_seen || mem);
    st = sl->st_cur >= 0 && (sl->st_stale || d != sl->st_seen || mem);
    if (pc || st || keep)   /* (after the sync: the image and the memory are one moment) */
        snap = slot_snap(s);
    if (keep)
        *keep = ms_ref(snap);
    if (!flash && !pc && !st) {
        ms_free(snap);
        return;
    }
    img = fw_flash(fw);   /* (the same image: nothing ran since; asked again as the copy above may be long) */
    j = st_job_new(J_WRITE);
    to[0] = j->path, to[1] = j->aux_path, to[2] = j->path2;
    if (flash)
        st_path(to[nt++], ST_PATH, "states/%s/flash.bin", fw->id);
    if (pc)
        j->act |= 1 << nt, pm_copy_path(to[nt++], ST_PATH, st_root(), sl->pc_proj, s);
    if (st)
        j->act |= 1 << nt, st_path(to[nt++], ST_PATH, "states/%s/state-%02d.bin", fw->id, sl->st_cur);
    if (img && (j->data = malloc(fw->flash_size))) {
        memcpy(j->data, img, fw->flash_size);
        j->len = fw->flash_size;
    }
    j->snap = snap;   /* (the job's reference; NULL: an older .mem beside the state and the copy removed) */
    j->text = state_json(fw);
    st_post(j);
    if (flash)
        sl->dirty = 0, sl->dirty_seen = d;
    if (pc)
        sl->pc_seen = d, sl->pc_stale = 0;
    if (st)
        sl->st_seen = d, sl->st_stale = 0;
}
static void slot_save(int s, int sync) { slot_write(s, sync, 1, sync, NULL); }

/* (rfm1cs 0.13.0) after a boot: whose memory the slot holds now (proj, -1 none), its active state (-1 none), and
 * whether that state's file and the project's copy hold it already; flash.bin follows on the autosave */
static void mem_new(int s, int proj, int st, int st_clean, int pc_clean)
{
    slot_t *sl = &SL[s];
    const uint32_t d = sl->on ? fw_flash_dirty(&sl->fw) : 0;
    sl->pc_proj = proj, sl->pc_seen = d, sl->pc_stale = !pc_clean;
    sl->st_cur = st, sl->st_seen = d, sl->st_stale = !st_clean;
    sl->dirty = 1, sl->dirty_ms = now_ms();
    sl->mem_ms = now_ms(), sl->in_seen = sl->on ? sl->fw.n_in : 0;
}
static void undo_drop(int s)
{
    free(SL[s].undo);
    ms_free(SL[s].undo_snap);
    SL[s].undo = NULL, SL[s].undo_n = 0, SL[s].undo_snap = NULL;
}
/* another project's firmwares are coming (PROJECT_FW), or it loaded: the outgoing project's copies written first,
 * synced (nothing of its last seconds lost); from now on no slot's memory is any project's until the new one's copy
 * is in it (pfw_want) or it is taken as the new one's (an older project: proj_adopt) */
static void proj_switch(int p)
{
    int s;
    if (p == g_proj_open)
        return;
    for (s = 0; s < NSLOT; s++) {
        if (SL[s].on && SL[s].pc_proj >= 0)
            slot_write(s, 1, 1, 1, NULL);
        SL[s].pc_proj = -1;
    }
    fprintf(stderr, "engine: project %d open (the slots' copies were project %d's)\n", p + 1, g_proj_open + 1);
    g_proj_open = p;
}
/* a project of version 1..8 (no firmwares of its own) or an empty one: what the slots hold now is its; its copies
 * are written at its next save (it becomes version 9 then) */
static void proj_adopt(int p)
{
    int s;
    g_proj_open = p;
    for (s = 0; s < NSLOT; s++)
        if (SL[s].on)
            SL[s].pc_proj = p, SL[s].pc_stale = 1;
}

/* the tests' capture per slot (README "Threads": every slot replayed offline): the first instance each slot runs, its
 * blocks and its input log, as raw-slotN.raw and inputs-slotN.log beside raw.raw */
static struct { fw_t *fw; int16_t *pcm; uint32_t n; int done; } g_scap[NSLOT];
static void scap_flush(int s)
{
    char p[ST_PATH + 32];
    const char *slash = C.capture_raw ? strrchr(C.capture_raw, '/') : NULL;
    if (!g_scap[s].fw || !slash)
        return;
    snprintf(p, sizeof p, "%.*s/raw-slot%d.raw", (int)(slash - C.capture_raw), C.capture_raw, s + 1);
    st_write(p, g_scap[s].pcm, (size_t)g_scap[s].n * 4u);
    snprintf(p, sizeof p, "%.*s/inputs-slot%d.log", (int)(slash - C.capture_raw), C.capture_raw, s + 1);
    if (g_scap[s].fw->log)
        st_write(p, g_scap[s].fw->log, (size_t)g_scap[s].fw->log_n * sizeof(fw_log_t));
    g_scap[s].fw = NULL;
    g_scap[s].done = 1;
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

static void run_ids_sync(void);

/* ------------------------------------------------------------------ the takeover (tk1) --- */
/* FM1VST docs/RFM1-TAKEOVER.md section 6, rfm1cs 0.7.0 (README "Takeover"): each slot's record (fw_tk) read after
 * every block it rendered (its frames, its rfm1_tk_input calls) and on its boot; what moved goes to rfm1cs: the
 * claims and name (rfm_move_set_takeover), the screen (scr_seq), the LEDs (led_seq), the words (say_seq). The core
 * sends back TK_INPUT and TK_STATE (on_action: queued to the slot's firmware). A firmware gone or replaced: claims 0
 * first (the core keeps a slot's takeover until told) */
static void tk_clear(int s)
{
    if (!SL[s].tk_known)
        return;
    SL[s].tk_known = 0;
    rfm_move_set_takeover(&S->core, s, 0, NULL);
    rfm1cs_session_redraw(S, now_ms());
    rfm1cs_session_flush_leds(S);
}

static void tk_poll(int s, int fresh)
{
    slot_t *sl = &SL[s];
    const uint8_t *r = sl->on ? fw_tk(&sl->fw) : NULL;
    uint32_t claims, scr, led, say;
    char name[16];
    int redraw = 0, leds = 0;
    if (!r) {   /* (no tk1, or the firmware died) */
        tk_clear(s);
        return;
    }
    memcpy(&claims, r + FW_TK_CLAIMS, 4);
    memcpy(&scr, r + FW_TK_SCR_SEQ, 4);
    memcpy(&led, r + FW_TK_LED_SEQ, 4);
    memcpy(&say, r + FW_TK_SAY_SEQ, 4);
    memcpy(name, r + FW_TK_NAME, sizeof name);
    name[sizeof name - 1] = 0;
    claims &= RFM_MOVE_TK_ALL;
    if (fresh || !sl->tk_known || claims != sl->tk_claims || strcmp(name, sl->tk_name)) {
        sl->tk_known = 1;
        sl->tk_claims = claims;
        memcpy(sl->tk_name, name, sizeof name);
        rfm_move_set_takeover(&S->core, s, claims, name);
        redraw = leds = 1;
    }
    if (fresh || scr != sl->tk_scr) {
        sl->tk_scr = scr;
        rfm_move_tk_screen(&S->core, s, r + FW_TK_SCREEN);
        redraw |= rfm_move_tk_shown(&S->core) == s;
    }
    if (fresh || led != sl->tk_led) {
        sl->tk_led = led;
        rfm_move_tk_leds(&S->core, s, (const uint8_t(*)[3])(r + FW_TK_LED));
        leds |= rfm_move_tk_shown(&S->core) == s;
    }
    if (fresh)
        sl->tk_say = say;   /* (words from before: not said) */
    else if (say != sl->tk_say) {
        char w[129];
        sl->tk_say = say;
        memcpy(w, r + FW_TK_SAY, 128);
        w[128] = 0;
        rfm_move_tk_say(&S->core, s, w, r[FW_TK_SAY_FLAGS] & 1);   /* (only while shown and words are open) */
    }
    if (redraw)
        rfm1cs_session_redraw(S, now_ms());
    if (leds)
        rfm1cs_session_flush_leds(S);
}

static void slot_unload(int s, int save)
{
    fw_inst_t *pre;
    int i;
    if (!SL[s].on)
        return;
    tk_clear(s);   /* (its takeover closed first: the firmware gets its releases and rfm1_tk_state(0) queued) */
    if (save) {   /* (no rendering ahead for its snapshot: a block would read its takeover's record again) */
        g_nest++;
        slot_save(s, 1);
        g_nest--;
    }
    if (g_raw_fw == &SL[s].fw)
        raw_flush();
    if (g_scap[s].fw == &SL[s].fw)
        scap_flush(s);
    /* its memory freed on the storage thread (megabytes of locked pages: not the engine's time) */
    if ((pre = fw_release(&SL[s].fw))) {
        st_job_t *j = st_job_new(J_FW_FREE);
        j->ptr = pre;
        st_post(j);
    }
    SL[s].on = 0;
    SL[s].pc_proj = SL[s].st_cur = -1;   /* (rfm1cs 0.13.0: no memory here now; mem_new says whose the next one is) */
    if ((i = catalog_find(SL[s].fw.id)) >= 0 && strcmp(g_fws[i].so, SL[s].so))
        g_sweep = 1;   /* (it ran an older version than the one installed: its files can go now) */
    SL[s].so[0] = 0;
    run_ids_sync();
}

/* rd1: the active firmware's last readout, for the page (engine thread only; seq 0: none since the slot came up) */
static fw_readout_t g_rd;
/* ------------------------------------------------------------------ the screen reader (announce.h) --- */
/* a firmware's name as the Move shows it (the catalog's list), else its id */
static const char *fw_name(const char *id)
{
    const rfm_move_fw_state_t *f = &S->core.fw;
    int i;
    for (i = 0; f->list && i < f->n; i++)
        if (f->list[i].id && !strcmp(f->list[i].id, id))
            return f->list[i].name && f->list[i].name[0] ? f->list[i].name : id;
    return id;
}

static void say_slot(int kind, const char *before, int s)
{
    ann_say(kind, "%s%sSlot %d, %s", before ? before : "", before ? ", " : "", s + 1,
            SL[s].on ? fw_name(SL[s].fw.id) : "empty");
}

/* What the Move's screen shows, told from the core's public state (rfm1cs-abletonmove 0.5.9 has no words of its own
 * for a screen reader): the mode, the projects view's track (the firmware slots, Projects), FX setup's page, the
 * firmware selected (with the catalog's state; its description when the jog turns to it), the lists and filters, the
 * slot played, latch, a knob touched in FM-1 mode (its name, as the firmware labels it), Control mode's CCs and MIDI
 * mode's octave, channel and layout. The host's own events (loads, downloads, states, readouts, the volume, the FM-1
 * linked, leaving) are said where they happen. */
static void speak_watch(void)
{
    static const char *const MODE[4] = { "FM-1 mode", "MIDI mode", "Control mode", "Sequencer mode" };
    static const char *const FXPG[4] = { "MIDI FX", "Routing", "Audio FX", "Mixer" };
    static const char *const CAT[5] = { "Synth", "Groovebox", "Chords", "Drums", "Other" };
    static const int ROLE[7] = { 0, 1, 3, 4, 5, 6, 2 };   /* encoder (SELECT ALGO K1..K4 PRESETS) -> its label's role */
    static struct {
        int init, view, active, cat, scope, filt, states, sel, jog, latch, touched, bank, oct, ch, layout;
        uint8_t cc[RFM_MOVE_NKNOBS];
    } W;
    const rfm_move_t *m;
    const rfm_move_fw_state_t *f;
    int view, k, fwtrack;
    if (!ann_on() || !g_session_on)
        return;
    speech_sync();
    m = &S->core;
    f = &m->fw;
    view = !m->seq.projects ? m->s.mode : f->fx ? 100 + (m->s.fx_page & 3) : 10 + f->track;
    fwtrack = view >= 10 && view < 13;
    if (!W.init) {   /* entering: the tool, the mode, the slot played */
        W.init = 1;
        ann_say(ANN_EVENT, "RemoteFM1");
        W.view = -1;
        W.active = g_active;
        W.latch = m->latch, W.touched = m->touched;
        W.oct = m->s.octave, W.ch = m->s.channel, W.layout = m->s.layout;
        W.bank = m->s.bank & (RFM_MOVE_NBANKS - 1);
        memcpy(W.cc, m->cc_val[W.bank], sizeof W.cc);
        if (view < 10)
            say_slot(ANN_EVENT, MODE[view & 3], g_active);
#ifdef RFM_MOVE_HAS_SPEECH
        rfm_move_set_speech(&S->core, 1);
#endif
    }
#ifdef RFM_MOVE_HAS_SPEECH
    return;   /* (rfm1cs says the rest: its screens in its own words; the host says its own events) */
#endif
    if (view != W.view) {
        const int was = W.view;
        W.view = view;
        if (was != -1 || view >= 10) {
            if (view < 10)
                ann_say(ANN_EVENT, "%s", MODE[view & 3]);
            else if (view >= 100)
                ann_say(ANN_EVENT, "%s%s", was >= 100 ? "" : "FX setup, ", FXPG[view - 100]);
            else if (view == 13)
                ann_say(ANN_EVENT, "Projects");
            else
                say_slot(ANN_EVENT, was >= 10 && was < 13 ? NULL : "Firmwares", view - 10);
        }
        W.cat = f->catalog, W.scope = m->s.fw_scope, W.filt = m->s.fw_filter, W.states = f->states;
        W.sel = f->sel, W.jog = f->jog;
    }
    if (g_active != W.active) {   /* (Back + Track, Down, the page) */
        W.active = g_active;
        if (!fwtrack)
            say_slot(ANN_EVENT, "Playing", g_active);
    }
    if (fwtrack) {
        if (f->catalog != W.cat || m->s.fw_scope != W.scope) {
            W.cat = f->catalog, W.scope = m->s.fw_scope;
            ann_say(ANN_EVENT, "%s", f->catalog ? "Catalog" : m->s.fw_scope ? "All firmwares" : "Favorites");
        }
        if (m->s.fw_filter != W.filt) {
            int st = 0;
            W.filt = m->s.fw_filter;
            for (k = 3; k <= 16 && !st; k++)
                if (W.filt >> (k - 1) & 1)
                    st = k;
            if (!st)
                ann_say(ANN_EVENT, "No filter");
            else if (st <= 7)
                ann_say(ANN_EVENT, "Filter %s", CAT[st - 3]);
            else if (st <= 15)
                ann_say(ANN_EVENT, "Filter %s", f->tag[st - 8][0] ? f->tag[st - 8] : "tag");
            else
                ann_say(ANN_EVENT, "Filter Fantasy");
        }
        if (f->states != W.states) {
            W.states = f->states;
            if (f->states)
                ann_say(ANN_EVENT, "States");
            else
                say_slot(ANN_EVENT, NULL, view - 10);
        }
        if (f->sel != W.sel) {   /* (a pad, the banks: the latest selection wins) */
            W.sel = f->sel, W.jog = f->jog;
            if (f->sel >= 0 && f->list && f->sel < f->n) {
                const rfm_move_fw_t *e = &f->list[f->sel];
                const int inst = e->installed && e->installed[0];
                ann_say(ANN_VALUE, "%s%s", e->name ? e->name : e->id,
                        !f->catalog ? "" : !inst ? ", not installed"
                        : e->version && strcmp(e->installed, e->version) ? ", update ready" : ", installed");
            }
        }
        if (f->jog != W.jog) {   /* (the jog: the description) */
            if (!W.jog && f->jog && f->sel >= 0 && f->list && f->sel < f->n && f->list[f->sel].desc)
                ann_say(ANN_VALUE, "%.250s", f->list[f->sel].desc);
            W.jog = f->jog;
        }
    }
    if (m->latch != W.latch) {
        W.latch = m->latch;
        ann_say(ANN_EVENT, "Latch %s", m->latch ? "on" : "off");
    }
    if (m->touched != W.touched) {   /* FM-1 mode: a knob touched, its name (its value: the firmware's readout) */
        W.touched = m->touched;
        if (view == RFM_MOVE_MODE_FM1 && m->touched >= 0 && m->touched < RFM_MOVE_NKNOBS) {
            const int e = rfm_move_knob_enc(m->touched);
            if (e >= 0 && e < 7) {
                const int r = ROLE[e];
                ann_say(ANN_VALUE, "%s", m->lbl_has[r] && m->lbl[r][0] ? m->lbl[r] : m->label[e][0] ? m->label[e]
                        : rfm_move_enc_name(e));
            }
        }
    }
    if (view == RFM_MOVE_MODE_CONTROL) {   /* Control mode: the bank, each knob's CC and value */
        const int b = m->s.bank & (RFM_MOVE_NBANKS - 1);
        if (b != W.bank) {
            W.bank = b;
            memcpy(W.cc, m->cc_val[b], sizeof W.cc);
            ann_say(ANN_VALUE, "Bank %d", b + 1);
        }
        for (k = 0; k < RFM_MOVE_NKNOBS; k++)
            if (m->cc_val[b][k] != W.cc[k]) {
                W.cc[k] = m->cc_val[b][k];
                ann_say(ANN_VALUE, "CC %d: %d", m->s.cc[b][k], W.cc[k]);
            }
    } else
        W.bank = -1;
    if (view == RFM_MOVE_MODE_MIDI || view == RFM_MOVE_MODE_CONTROL) {   /* MIDI's octave, channel and layout */
        if (m->s.octave != W.oct)
            ann_say(ANN_VALUE, "Octave %d", m->s.octave);
        if (m->s.channel != W.ch)
            ann_say(ANN_VALUE, "Channel %d", m->s.channel + 1);
        if (m->s.layout != W.layout)
            ann_say(ANN_VALUE, "%s layout", rfm_move_layout_name(m->s.layout));
    }
    W.oct = m->s.octave, W.ch = m->s.channel, W.layout = m->s.layout;
}

static void active_changed(void)
{
    int k;
    fw_t *fw = SL[g_active].on ? &SL[g_active].fw : NULL;
    rfm_move_set_keymap(&S->core, fw && fw->has_keymap ? (const int8_t(*)[11])fw->keymap : NULL);
    for (k = 0; k <= RFM_MOVE_E_MASTER; k++)
        rfm_move_set_label(&S->core, k, NULL);
    rfm_move_set_labels(&S->core, NULL);   /* (0.5.6) the new firmware's labels come with its next epoch */
    rfm_move_readouts_clear(&S->core);     /* (rd1, rfm1cs 0.5.7) its readouts too; those it made while in the
                                            * background are not shown (their seq taken) */
    memset(&g_rd, 0, sizeof g_rd);
    if (fw)
        (void)fw_readout(fw, NULL, 0);
    {
        const char *nm[RFM_MOVE_NBTN];
        for (k = 0; k < RFM_MOVE_NBTN; k++)
            nm[k] = fw && k < FW_NBTN && fw->btn_name[k][0] ? fw->btn_name[k] : NULL;
        rfm_move_set_button_names(&S->core, nm);
    }
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

/* a firmware into slot s (an instance fw_instantiate made from its object, a saved flash or none; snap: its working
 * memory (README "The working memory"), restored instead of a boot when it is this very module's, else the flash is
 * booted) */
static int slot_boot(int s, fw_inst_t *pre, const uint8_t *flash, uint32_t n, const ms_snap_t *snap)
{
    slot_t *sl = &SL[s];
    char so[ST_PATH];
    int rc, restored = 0;
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
    if (C.capture_raw && g_cap_max && !g_scap[s].done && (g_scap[s].pcm || (g_scap[s].pcm = calloc(g_cap_max, 4)))) {
        g_scap[s].fw = &sl->fw;
        g_scap[s].n = 0;
        if (!sl->fw.log)
            fw_log_start(&sl->fw, 1u << 20);
    }
    if (snap) {   /* (no rfm1_init: the firmware as it was, nothing re-applied from its flash) */
        const uint64_t t0 = now_ns();
        if (fw_snap_restore(&sl->fw, &snap->h, snap->mem) == 0) {
            const uint32_t us = (uint32_t)((now_ns() - t0) / 1000u);
            restored = 1;
            if (us > g_restore_us_max)
                g_restore_us_max = us;
            if (sl->fw.tk_ptr)   /* (its takeover not shown, as after a boot, until rfm1cs shows it: tk_poll) */
                fw_tk_state(&sl->fw, 0, 0, 0);
            /* what was held when it was taken let go, as after a boot nothing is: the panel's keys and buttons, and
             * the notes MIDI held (CC 123 on every channel, the host's All Notes Off): nothing will let them go */
            if (sl->fw.keys_held || sl->fw.btns_held)
                fw_in_now(&sl->fw, FWI_RELEASE, 0, 0, 0);
            {
                int c;
                for (c = 0; c < 16; c++) {
                    const uint8_t m[3] = { (uint8_t)(0xB0 | c), 123, 0 };
                    fw_midi(&sl->fw, m, 3, sl->fw.frames_done);
                }
            }
        } else
            fprintf(stderr, "engine: slot %d: %s: booted from its flash instead\n", s + 1, sl->fw.err);
    }
    if (!restored && fw_boot(&sl->fw, flash, n) != 0) {
        fprintf(stderr, "engine: slot %d: boot: %s\n", s + 1, sl->fw.err);
        if (g_raw_fw == &sl->fw)
            g_raw_fw = NULL;
        if (g_scap[s].fw == &sl->fw)
            g_scap[s].fw = NULL;
        fw_destroy(&sl->fw);
        return -1;
    }
    sl->on = 1;
    sl->restored = (uint32_t)restored;
    snprintf(sl->so, sizeof sl->so, "%s", so);
    sl->reboot = 0;
    sl->serial = ++g_serial;
    sl->idx = catalog_find(sl->fw.id);
    sl->level = (uint8_t)rfm_move_fw_level(&S->core, s);
    sl->dirty = 0;
    sl->dirty_seen = fw_flash_dirty(&sl->fw);
    sl->ns_mark = sl->fw.ns_total;
    sl->c1_job_ns = sl->c1_wait_ns = 0;
    if (g_master >= 0)
        fw_in_now(&sl->fw, FWI_MASTER, 0, g_master, 0);
    sl->tk_known = 0;
    sl->tk_in = sl->tk_states = 0;
    tk_poll(s, 1);   /* (tk1: its takeover's claims and name to rfm1cs, or none) */
    fprintf(stderr, "engine: slot %d: %s %s (%s)\n", s + 1, sl->fw.name, sl->fw.version,
            restored ? "its saved working memory" : flash && n ? "its saved flash" : "a fresh flash");
    run_ids_sync();
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
    slot_boot(s, pre, img, (uint32_t)n, NULL);
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

/* (rfm1cs 0.13.0) PROJECT_FW: project p wants firmware id in slot s ("" empty). The outgoing project's copies first
 * (proj_switch); then: the same firmware there with this project's memory already: ready; "": unloaded; a firmware not
 * installed: the slot emptied, the project's copy file left alone (it keeps the firmware's name: installed later, the
 * copy is there); else the storage thread makes the instance (the same object when the same firmware runs here: only
 * its memory swapped) and reads the project's copy (none: flash.bin; none: a fresh memory), J_PFW_OPEN */
static void pfw_want(int s, int p, const char *id)
{
    slot_t *sl = &SL[s];
    int i;
    st_job_t *j;
    if (p < 0 || p >= RFM_MOVE_PROJECTS)
        return;
    proj_switch(p);
    sl->pfw_gen++;
    if (sl->on && sl->pc_proj == p && (!id[0] || strcmp(sl->fw.id, id)))
        sl->pc_proj = -1;   /* (the open project's slot made anew, as after a cut of it: what goes is not its now) */
    if (!id[0]) {   /* (nothing to answer for "") */
        if (sl->on) {
            undo_drop(s);
            slot_unload(s, 1);
            slots_tell();
            if (s == g_active)
                active_changed();
        }
        return;
    }
    if (sl->on && !strcmp(sl->fw.id, id) && sl->pc_proj == p) {   /* (this project's memory already) */
        rfm_move_proj_fw(&S->core, s, RFM_MOVE_PFW_READY);
        slots_tell();
        rfm_move_fw_state_cur(&S->core, s, sl->st_cur);
        return;
    }
    if ((i = catalog_find(id)) < 0) {
        fprintf(stderr, "engine: project %d: %s is not installed (slot %d left empty)\n", p + 1, id, s + 1);
        if (sl->on) {
            undo_drop(s);
            slot_unload(s, 1);
            if (s == g_active)
                active_changed();
        }
        rfm_move_proj_fw(&S->core, s, RFM_MOVE_PFW_MISSING);
        slots_tell();
        return;
    }
    j = st_job_new(J_PFW_OPEN);
    j->slot = s, j->index = p, j->value = (int)sl->pfw_gen;
    snprintf(j->id, sizeof j->id, "%s", id);
    if (sl->on && !strcmp(sl->fw.id, id) && !strcmp(sl->so, g_fws[i].so) && sl->fw.obj) {
        fw_obj_ref(sl->fw.obj);
        j->obj = sl->fw.obj;
    } else
        snprintf(j->path, sizeof j->path, "%s", g_fws[i].so);
    pm_copy_path(j->aux_path, sizeof j->aux_path, st_root(), p, s);
    st_path(j->path2, sizeof j->path2, "states/%s/flash.bin", id);
    {   /* (another slot running it: its flash.bin first, as FW_LOAD's) */
        int o;
        for (o = 0; o < NSLOT; o++)
            if (o != s && SL[o].on && !strcmp(SL[o].fw.id, id))
                slot_save(o, 1);
    }
    st_post(j);
    rfm_move_proj_fw(&S->core, s, 10);
}

static void store_watch(uint32_t now, int force);
/* ---- the page's States and projects: a request's outcome for the web thread (st 1 done, -1 refused or failed) */
static void store_fin(int id, int st, const char *msg)
{
    int i;
    pthread_mutex_lock(&g_vmu);
    for (i = 0; i < (int)(sizeof g_sdone / sizeof g_sdone[0]); i++)
        if (g_sdone[i].id == id)
            break;
    if (i == (int)(sizeof g_sdone / sizeof g_sdone[0]))
        i = g_sdone_at++ % (int)(sizeof g_sdone / sizeof g_sdone[0]);
    g_sdone[i].id = id, g_sdone[i].st = st;
    snprintf(g_sdone[i].msg, sizeof g_sdone[i].msg, "%s", msg ? msg : "");
    pthread_mutex_unlock(&g_vmu);
    if (st < 0)
        fprintf(stderr, "engine: the page's request %d: %s\n", id, msg ? msg : "refused");
}
static spend_t *store_pend(int id)
{
    int i;
    for (i = 0; id && i < (int)(sizeof g_spend / sizeof g_spend[0]); i++)
        if (g_spend[i].id == id)
            return &g_spend[i];
    return NULL;
}
static void store_failed(int id, const char *why)   /* (a job's failure: the request says it when it is answered) */
{
    spend_t *p = store_pend(id);
    if (p && !p->err[0])
        snprintf(p->err, sizeof p->err, "%s", why);
}
/* every job of request id is back: what it asked for is checked against what the Move has now */
static void store_synced(int id)
{
    spend_t *p = store_pend(id);
    const rfm_move_t *m = &S->core;
    char why[200] = "", ok[64] = "Done";
    int k, s;
    if (!p)
        return;
    k = p->k, s = p->slot;
    if (p->err[0])
        snprintf(why, sizeof why, "%s", p->err);
    else
        switch (p->op) {
        case ENG_ST_SAVE:
        case ENG_ST_IMPORT:
            if (!(m->fw.st_used[s] >> k & 1))
                snprintf(why, sizeof why, "state %d was not written", k + 1);
            else if ((p->flags & ENG_STF_LOAD) && SL[s].st_cur != k)
                snprintf(why, sizeof why, "state %d was written but did not load", k + 1);
            snprintf(ok, sizeof ok, "State %d saved", k + 1);
            break;
        case ENG_ST_LOAD:
            if (!SL[s].on || SL[s].st_cur != k)
                snprintf(why, sizeof why, "state %d did not load", k + 1);
            snprintf(ok, sizeof ok, "State %d loaded", k + 1);
            break;
        case ENG_ST_DELETE:
            if (m->fw.st_used[s] >> k & 1)
                snprintf(why, sizeof why, "state %d is still there", k + 1);
            snprintf(ok, sizeof ok, "State %d deleted", k + 1);
            break;
        case ENG_PJ_OPEN:
            if (rfm_move_project_slot(m) != k)
                snprintf(why, sizeof why, "project %d did not open", k + 1);
            snprintf(ok, sizeof ok, "Project %d open", k + 1);
            break;
        case ENG_PJ_DELETE:
            if (m->seq.proj_used >> k & 1)
                snprintf(why, sizeof why, "project %d is still there", k + 1);
            snprintf(ok, sizeof ok, "Project %d deleted", k + 1);
            break;
        default:   /* (ENG_PJ_SAVE, _DUP, _IMPORT: the project there now) */
            k = p->op == ENG_PJ_DUP ? p->to : k;
            if (!(m->seq.proj_used >> k & 1))
                snprintf(why, sizeof why, "project %d was not written", k + 1);
            snprintf(ok, sizeof ok, "%s", p->op == ENG_PJ_SAVE ? "Saved" : "Project saved");
            break;
        }
    store_watch(now_ms(), 1);   /* (what the page reads next is what is done) */
    store_fin(id, why[0] ? -1 : 1, why[0] ? why : ok);
    p->id = 0;
}

static void page_edit(int op, int a, int b);
static void do_deferred(void)
{
    static int busy;   /* (an action that renders ahead (slot_snap) runs render_block, which comes back here: the
                        * actions it adds are this loop's, after the one under way) */
    int k;
    if (busy)
        return;
    busy = 1;
    for (k = 0; k < g_ndefer; k++) {
        const rfm_move_act_t *a = &g_defer[k].a;
        const char *id = g_defer[k].id;
        const int s = a->slot < NSLOT ? a->slot : 0;
        switch (a->kind) {
        case RFM_MOVE_ACT_FW_LOAD: {
            const int i = catalog_find(id);
            SL[s].pfw_gen++;   /* (a project's load of this slot under way: this one wins) */
            if (i < 0) {
                rfm_move_fw_progress(&S->core, id, RFM_MOVE_FW_FAILED);
                load_failed(id);
                break;
            }
            if (SL[s].on && !strcmp(SL[s].fw.id, id) && !strcmp(SL[s].so, g_fws[i].so) && SL[s].fw.obj &&
                !strcmp(SL[s].fw.obj->vt->sha256, g_fws[i].sha256)) {   /* there already (the same build: a rebuild
                                                                         * of its version installed: loaded again) */
                rfm_move_fw_progress(&S->core, id, 100);
                slots_tell();
                if (g_activate == s) {
                    g_activate = -1;
                    page_edit(ENG_OP_ACTIVE, s, 0);
                }
                break;
            }
            rfm_move_fw_progress(&S->core, id, 10);
            ann_say(ANN_EVENT, "Loading %s", fw_name(id));
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
            SL[s].pfw_gen++;
            undo_drop(s);
            slot_unload(s, 1);
            slots_tell();
            ann_say(ANN_EVENT, "Slot %d emptied", s + 1);
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
            ann_say(ANN_EVENT, "Downloading %s", fw_name(id));
            fprintf(stderr, "engine: download: %s %s (%llu bytes)\n", id, g_cat->e[e].version,
                    (unsigned long long)g_cat->e[e].bytes);
            break;
        }
        case RFM_MOVE_ACT_STATE_SAVE:
            if (SL[s].on) {
                fw_t *fw = &SL[s].fw;
                const uint8_t *img;
                ms_snap_t *m;
                /* (synced; the state active so far and the project's copy get it too when they are behind on its
                 * flash: another state saved is not one loaded over it; m: the working memory now, the same moment as
                 * the image) */
                slot_write(s, 1, 0, 0, &m);
                if ((img = fw_flash(fw))) {
                    st_job_t *j = st_job_new(J_STATE_SAVE);
                    j->slot = s, j->index = a->index;
                    snprintf(j->id, sizeof j->id, "%s", fw->id);
                    if ((j->data = malloc(fw->flash_size))) {
                        memcpy(j->data, img, fw->flash_size);
                        j->len = fw->flash_size;
                    }
                    j->snap = m, m = NULL;
                    j->text = state_json(fw);
                    st_post(j);
                    if (a->index < RFM_MOVE_FW_STATES) {   /* (the saved one is the active one: it holds the memory) */
                        SL[s].st_cur = a->index, SL[s].st_seen = fw_flash_dirty(fw), SL[s].st_stale = 0;
                    }
                }
                ms_free(m);
            }
            break;
        case RFM_MOVE_ACT_STATE_LOAD:
        case RFM_MOVE_ACT_STATE_DELETE:
        case RFM_MOVE_ACT_STATE_COPY:
            if (SL[s].on) {
                const int k = a->kind == RFM_MOVE_ACT_STATE_LOAD ? J_STATE_LOAD : a->kind == RFM_MOVE_ACT_STATE_DELETE
                              ? J_STATE_DELETE : J_STATE_COPY;
                st_job_t *j;
                /* (rfm1cs 0.13.0) the active state gets the memory first (synced): a load leaves it as it was left,
                 * a copy or a delete puts what the firmware holds now in the buffer */
                if (a->kind == RFM_MOVE_ACT_STATE_LOAD || SL[s].st_cur == a->index)
                    slot_write(s, 1, 0, 1, NULL);
                if (a->kind == RFM_MOVE_ACT_STATE_DELETE && SL[s].st_cur == a->index)
                    SL[s].st_cur = -1;   /* (the firmware keeps what it holds, no state is active) */
                j = st_job_new(k);
                j->slot = s, j->index = a->index, j->act = STA_LOAD;
                snprintf(j->id, sizeof j->id, "%s", SL[s].fw.id);
                if (a->kind == RFM_MOVE_ACT_STATE_LOAD) {   /* (a new instance of the same object, made there) */
                    fw_obj_ref(SL[s].fw.obj);
                    j->obj = SL[s].fw.obj;
                }
                st_post(j);
            }
            break;
        case RFM_MOVE_ACT_STATE_PASTE:   /* (rfm1cs 0.13.0) the buffer stored as that state; value 1: loaded too */
            if (SL[s].on && a->index < RFM_MOVE_FW_STATES) {
                st_job_t *j = st_job_new(J_STATE_PASTE);
                j->slot = s, j->index = a->index, j->value = a->value ? 1 : 0;
                snprintf(j->id, sizeof j->id, "%s", SL[s].fw.id);
                j->text = state_json(&SL[s].fw);
                if (j->value) {   /* (the active one replaced: no autosave into it until the paste is in) */
                    if (SL[s].st_cur == a->index)
                        SL[s].st_cur = -1;
                    fw_obj_ref(SL[s].fw.obj);
                    j->obj = SL[s].fw.obj;
                }
                st_post(j);
            }
            break;
        case RFM_MOVE_ACT_STATE_NEW:   /* (rfm1cs 0.13.0) the memory kept (the undo), the firmware as first installed */
            if (SL[s].on) {
                slot_t *sl = &SL[s];
                const uint8_t *img;
                st_job_t *j;
                ms_snap_t *m;
                /* (the active state and the project's copy have it: the undo's state is right; m: the working memory
                 * the undo brings back) */
                slot_write(s, 1, 0, 1, &m);
                undo_drop(s);
                if ((img = fw_flash(&sl->fw)) && (sl->undo = malloc(sl->fw.flash_size))) {
                    memcpy(sl->undo, img, sl->fw.flash_size);
                    sl->undo_n = sl->fw.flash_size;
                    sl->undo_k = sl->st_cur;
                    memcpy(sl->undo_id, sl->fw.id, sizeof sl->undo_id);   /* (both RFM_MOVE_FW_ID_MAX, NUL-ended) */
                    sl->undo_snap = m, m = NULL;
                }
                ms_free(m);
                sl->st_cur = -1;
                j = st_job_new(J_STATE_LOAD);
                j->slot = s, j->index = -1, j->act = STA_NEW;
                snprintf(j->id, sizeof j->id, "%s", sl->fw.id);
                fw_obj_ref(sl->fw.obj);
                j->obj = sl->fw.obj;
                st_post(j);
            }
            break;
        case RFM_MOVE_ACT_STATE_UNDO:   /* (rfm1cs 0.13.0) the memory New kept, back; its state active again */
            if (SL[s].on && SL[s].undo && !strcmp(SL[s].undo_id, SL[s].fw.id)) {
                slot_t *sl = &SL[s];
                st_job_t *j = st_job_new(J_STATE_LOAD);
                j->slot = s, j->act = STA_UNDO;
                j->index = j->value = a->value >= 0 && a->value < RFM_MOVE_FW_STATES ? a->value : -1;
                snprintf(j->id, sizeof j->id, "%s", sl->fw.id);
                j->data = sl->undo, j->len = sl->undo_n;
                j->snap = sl->undo_snap;
                sl->undo = NULL, sl->undo_n = 0, sl->undo_snap = NULL;
                fw_obj_ref(sl->fw.obj);
                j->obj = sl->fw.obj;
                st_post(j);
            } else if (SL[s].on)   /* (nothing kept: the firmware stays as it is, and so does its active state) */
                rfm_move_fw_state_cur(&S->core, s, SL[s].st_cur);
            break;
        case RFM_MOVE_ACT_PROJECT_FW:   /* (rfm1cs 0.13.0) project a->index's firmware for this slot */
            pfw_want(s, a->index, id);
            break;
        default:
            break;
        }
    }
    g_ndefer = 0;
    busy = 0;
}

static void do_results(void)
{
    st_job_t *j;
    int n = 0;
    while (n++ < 16 && (j = st_take())) {
        const int s = j->slot >= 0 && j->slot < NSLOT ? j->slot : 0;
        switch (j->kind) {
        case J_FW_OPEN: {
            /* (a halted firmware powered on again keeps whose memory it was and its active state; a firmware loaded
             * by hand belongs to the open project, no state active) */
            const int again = SL[s].on && SL[s].reboot && !strcmp(SL[s].fw.id, j->id);
            const int pp = again ? SL[s].pc_proj : g_proj_open, sk = again ? SL[s].st_cur : -1;
            if (!again)
                undo_drop(s);
            if (!j->ptr) {
                fprintf(stderr, "engine: %s: %s\n", j->id, j->err);
                ann_say(ANN_EVENT, "%s did not load", fw_name(j->id));
                rfm_move_fw_progress(&S->core, j->id, RFM_MOVE_FW_FAILED);
                load_failed(j->id);
                break;
            }
            if (slot_boot(s, j->ptr, j->data, j->len, NULL) != 0) {
                ann_say(ANN_EVENT, "%s did not start", fw_name(j->id));
                rfm_move_fw_progress(&S->core, j->id, RFM_MOVE_FW_FAILED);
                slots_tell();
                load_failed(j->id);
                if (s == g_active)
                    active_changed();
                break;
            }
            mem_new(s, pp, sk, 0, 0);
            slots_tell();
            rfm_move_fw_state_cur(&S->core, s, sk);
            rfm_move_fw_progress(&S->core, j->id, 100);
            ann_say(ANN_EVENT, "%s loaded, slot %d", fw_name(j->id), s + 1);
            states_scan(s);
            if (g_activate == s) {   /* (the page's chip: load into slot n, then play it) */
                g_activate = -1;
                page_edit(ENG_OP_ACTIVE, s, 0);
            }
            if (s == g_active)
                active_changed();
            view_info();
            break;
        }
        case J_STATE_SAVE:
            fprintf(stderr, "engine: %s: state %d saved, %s\n", j->id, j->index + 1,
                    j->ok ? "with its working memory" : "its flash only (no working memory for this module)");
            /* fall through */
        case J_STATE_DELETE:
        case J_STATE_SCAN:
        case J_STATE_COPY:
            if (SL[s].on && !strcmp(SL[s].fw.id, j->id))
                rfm_move_fw_states(&S->core, s, j->used);
            if (j->kind == J_STATE_SAVE || j->kind == J_STATE_DELETE)
                ann_say(ANN_EVENT, "State %d %s", j->index + 1, j->kind == J_STATE_SAVE ? "saved" : "deleted");
            break;
        case J_STATE_PASTE:
        case J_STATE_IMPORT:   /* (the page's state file: as a paste) */
            if (SL[s].on && !strcmp(SL[s].fw.id, j->id))
                rfm_move_fw_states(&S->core, s, j->used);
            if (j->kind == J_STATE_IMPORT && j->ok)
                fprintf(stderr, "engine: %s: state %d from the page's file%s\n", j->id, j->index + 1,
                        j->value ? ", loaded" : "");
            if (!j->value) {
                fw_inst_free(j->ptr);
                break;
            }
            /* (onto the active one: loaded too, as a state load) */
            /* fall through */
        case J_STATE_LOAD: {
            /* a state (STA_LOAD, a paste onto the active one), a fresh memory (STA_NEW) or the one New kept
             * (STA_UNDO) into the same firmware: a new instance of its object, booted with it */
            const int fresh = j->kind == J_STATE_LOAD && j->act == STA_NEW;
            const int k = j->kind != J_STATE_LOAD || j->act == STA_LOAD ? j->index : j->act == STA_UNDO ? j->value : -1;
            if (SL[s].on && !strcmp(SL[s].fw.id, j->id) && j->ptr && (j->data || fresh)) {
                const int pp = SL[s].pc_proj;
                uint8_t *keep = SL[s].undo;   /* (New's undo survives its own restart) */
                ms_snap_t *keep_snap = SL[s].undo_snap;
                SL[s].undo = NULL, SL[s].undo_snap = NULL;
                slot_unload(s, 0);   /* (the memory was written before the job: the state or New replaces it) */
                if (slot_boot(s, j->ptr, j->data, j->len, j->snap) == 0) {
                    mem_new(s, pp, k, 1, 0);
                    if (!fresh)   /* (README "The working memory": the working memory, or why the flash alone) */
                        fprintf(stderr, "engine: slot %d: %s %d: %s%s\n", s + 1, j->kind == J_STATE_PASTE ? "pasted state" :
                                j->kind == J_STATE_IMPORT ? "the page's state" :
                                j->act == STA_UNDO ? "undo to state" : "state", k + 1,
                                SL[s].restored ? "its working memory restored" : "its flash only: ",
                                SL[s].restored ? "" : j->memwhy[0] ? j->memwhy : "the snapshot was refused");
                    if (fresh) {
                        SL[s].undo = keep, keep = NULL;
                        SL[s].undo_snap = keep_snap, keep_snap = NULL;
                        ann_say(ANN_EVENT, "New state");
                    } else if (j->kind == J_STATE_LOAD && j->act == STA_UNDO)
                        ann_say(ANN_EVENT, "Undo");
                    else
                        ann_say(ANN_EVENT, "State %d loaded", j->index + 1);
                    if (s == g_active)
                        active_changed();
                }
                free(keep);
                ms_free(keep_snap);
                slots_tell();
                rfm_move_fw_state_cur(&S->core, s, SL[s].on ? SL[s].st_cur : -1);
            } else {
                fw_inst_free(j->ptr);
                if (SL[s].on && !strcmp(SL[s].fw.id, j->id))   /* (it did not happen: the core hears what is active) */
                    rfm_move_fw_state_cur(&S->core, s, SL[s].st_cur);
            }
            break;
        }
        case J_PFW_OPEN:   /* (rfm1cs 0.13.0) a project's firmware with its memory: booted, the core told */
            if (j->value != (int)SL[s].pfw_gen || j->index != g_proj_open) {   /* (another load since) */
                fw_inst_free(j->ptr);
                break;
            }
            undo_drop(s);
            if (!j->ptr || slot_boot(s, j->ptr, j->data, j->len, j->snap) != 0) {
                fprintf(stderr, "engine: project %d: slot %d: %s did not load%s%s\n", j->index + 1, s + 1, j->id,
                        j->err[0] ? ": " : "", j->err);
                slot_unload(s, 1);
                rfm_move_proj_fw(&S->core, s, RFM_MOVE_PFW_FAILED);
                slots_tell();
                if (s == g_active)
                    active_changed();
                break;
            }
            mem_new(s, j->index, j->match, 1, j->ok == 2);
            fprintf(stderr, "engine: project %d: slot %d: %s with %s%s%s%s%s\n", j->index + 1, s + 1, j->id,
                    j->ok == 2 ? "the project's memory" : j->ok == 1 ? "its last memory (flash.bin)" : "a fresh memory",
                    SL[s].restored ? " (its working memory restored)" : j->ok == 2 ? " (its flash only: " : "",
                    !SL[s].restored && j->ok == 2 ? (j->memwhy[0] ? j->memwhy : "the snapshot was refused") : "",
                    !SL[s].restored && j->ok == 2 ? ")" : "", j->match >= 0 ? ", the active state's" : "");
            slots_tell();
            rfm_move_proj_fw(&S->core, s, RFM_MOVE_PFW_READY);
            rfm_move_fw_state_cur(&S->core, s, j->match);
            states_scan(s);
            if (s == g_active)
                active_changed();
            view_info();
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
                ann_say(ANN_EVENT, "%s installed", fw_name(j->id));
            } else {
                g_dl.state = 3, g_dl.failed++;
                ann_say(ANN_EVENT, "%s: the download failed", fw_name(j->id));
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
        case R_PROJECT_LOADED: {
            const int nd = g_ndefer;
            int k, asked = 0;
            fx_state_take(j->text);   /* (before the core's fx_sync loads its modules) */
            proj_switch(j->index);    /* (rfm1cs 0.13.0: the outgoing project's firmware copies first) */
            if (rfm1cs_session_project_answer(S, now_ms(), j->index, j->data, j->len) < 0)
                rfm1cs_session_project_answer(S, now_ms(), j->index, NULL, 0);
            for (k = nd; k < g_ndefer; k++)
                asked |= g_defer[k].a.kind == RFM_MOVE_ACT_PROJECT_FW;
            if (!asked)   /* (a project of version 1..8, or an empty one: the firmwares stay, now this one's) */
                proj_adopt(j->index);
            break;
        }
        case R_PROJECT_STORED:
            rfm_move_project_dir(&S->core, j->used);
            rfm_move_project_info(&S->core, j->index, j->data, j->len);
            break;
        case J_SYNC:   /* (a page's request: everything it asked of the storage thread is done) */
            store_synced(j->req);
            break;
        default:
            break;
        }
        if (j->req && j->err[0])   /* (a page's request: its job's failure, said when it is answered) */
            store_failed(j->req, j->err);
        st_job_free(j);
        do_deferred();
    }
}

/* ------------------------------------------------------------------ the web's view --- */
static char g_run_ids[NSLOT][32];             /* the ids in the slots, for the page's file browser (g_vmu) */
static void run_ids_sync(void)
{
    int s;
    pthread_mutex_lock(&g_vmu);
    for (s = 0; s < NSLOT; s++)
        snprintf(g_run_ids[s], sizeof g_run_ids[s], "%s", SL[s].on ? SL[s].fw.id : "");
    pthread_mutex_unlock(&g_vmu);
}

void engine_running_ids(char ids[3][32])
{
    int s;
    pthread_mutex_lock(&g_vmu);
    for (s = 0; s < 3; s++)
        snprintf(ids[s], 32, "%s", s < NSLOT ? g_run_ids[s] : "");
    pthread_mutex_unlock(&g_vmu);
}

static void view_info(void)
{
    fw_t *fw = SL[g_active].on ? &SL[g_active].fw : NULL;
    char tmp[sizeof g_view.info];
    int n = 0, s, k;
    run_ids_sync();
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
            if (C.test_api && e && n < (int)sizeof tmp - 49 * 40) {   /* (tests: the labels as rfm1cs holds them) */
                n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"move_labels\":[");
                for (k = 0; k < RFM_MOVE_LBL_N; k++) {
                    mj_escape(S->core.lbl_has[k] ? S->core.lbl[k] : "", esc, sizeof esc);
                    n += snprintf(tmp + n, sizeof tmp - (size_t)n, "%s%s%s%s", k ? "," : "",
                                  S->core.lbl_has[k] ? "\"" : "", S->core.lbl_has[k] ? esc : "null",
                                  S->core.lbl_has[k] ? "\"" : "");
                }
                n += snprintf(tmp + n, sizeof tmp - (size_t)n, "]");
            }
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
        if (g_rd.seq && n < (int)sizeof tmp - 160) {   /* rd1: the last readout (FM1VST docs/RFM1.md "Readouts") */
            char et[40], ev[40];
            mj_escape(g_rd.title, et, sizeof et);
            mj_escape(g_rd.value, ev, sizeof ev);
            n += snprintf(tmp + n, sizeof tmp - (size_t)n,
                          ",\"readout\":{\"seq\":%u,\"control\":%d,\"kind\":%d,\"flags\":%d,\"frac\":%d,\"title\":\"%s\","
                          "\"value\":\"%s\"}", g_rd.seq, g_rd.control, g_rd.kind, g_rd.flags, g_rd.frac, et, ev);
        }
    }
    if (n < (int)sizeof tmp - 64) {   /* the FM-1 on USB-A (fm1link.c) */
        fav_json();
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
        "\"sounding\":%d,\"store_ver\":%u,\"slots\":[", g_mix_ver, g_active, S->core.s.fx_page, S->core.fw.fx_focus,
        S->core.fw.fx_pages, S->core.fw.mix_par,
        S->core.fw.fx && S->core.seq.projects, rfm_move_fw_sounding(&S->core), g_store_ver);
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
#ifdef RFM_MOVE_HAS_SPEECH   /* (the page's switch: the core draws no popup for it, so it says nothing; without
                              * rfm1cs's words, speak_watch says it) */
        say_slot(ANN_EVENT, "Playing", a);
#endif
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
    case ENG_OP_MROUTE:
        (void)rfm_move_midi_route_set(m, a, b);   /* (its ROUTE act brings the masks here) */
        break;
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

/* ---- the page's States and projects (engine_store): on the engine thread, outside any core callback. A state is
 * asked for as its pad asks (rfm1cs projfw.c st_pad: the action deferred and done by do_deferred, the core's st_used /
 * st_cur as the pad leaves them, New's undo gone); a project as the project grid does (project.c: rfmv_proj_open's
 * save-then-load, the cut of the open one); what no pad does (a delete that keeps the Move's buffer, a copy into an
 * empty slot, a file from the page) by the storage thread's own jobs, the store told as for any save. rfm1cs 0.13.0
 * has no call for these (README "The page's States and projects": the entry points it would take), so its public
 * fields are written here as its own handlers write them, as page_edit does for FX setup. */
static void store_defer(int kind, int s, int k)
{
    defer_t *d;
    if (g_ndefer >= (int)(sizeof g_defer / sizeof g_defer[0]))
        do_deferred();
    d = &g_defer[g_ndefer++];
    memset(d, 0, sizeof *d);
    d->a.kind = (uint8_t)kind, d->a.slot = (uint8_t)s, d->a.index = (uint8_t)k;
    snprintf(d->id, sizeof d->id, "%s", SL[s].fw.id);
    do_deferred();   /* (its job posted now: the request's J_SYNC goes after it) */
}
static void store_run(const eng_store_t *r, int id)
{
    rfm_move_t *m = &S->core;
    const int s = r->slot, k = r->k, cur = rfm_move_project_slot(m);
    char why[200] = "";
    st_job_t *j;
    spend_t *p = NULL;
    int i;
    for (i = 0; i < (int)(sizeof g_spend / sizeof g_spend[0]) && !p; i++)
        if (!g_spend[i].id)
            p = &g_spend[i];
    if (!p)
        snprintf(why, sizeof why, "the Move is busy with the page's last requests: try again");
    else if (k < 0 || k >= RFM_MOVE_FW_STATES)
        snprintf(why, sizeof why, "no such %s", r->op <= ENG_ST_IMPORT ? "state" : "project");
    else if (r->op >= ENG_ST_SAVE && r->op <= ENG_ST_IMPORT) {
        const int used = s >= 0 && s < NSLOT ? (int)(m->fw.st_used[s] >> k & 1) : 0;
        const int act = s >= 0 && s < NSLOT ? m->fw.st_cur[s] : -1;
        if (s < 0 || s >= NSLOT || !SL[s].on)
            snprintf(why, sizeof why, "slot %d is empty", s + 1);
        else if (strcmp(SL[s].fw.id, r->id))
            snprintf(why, sizeof why, "slot %d runs %s now", s + 1, SL[s].fw.name);
        else if (rfm_move_proj_fw_loading(m))
            snprintf(why, sizeof why, "a project is loading on the Move: try again in a moment");
        else if (r->op == ENG_ST_SAVE) {
            if (used && !(r->flags & ENG_STF_REPLACE))
                snprintf(why, sizeof why, "state %d holds a sound already", k + 1);
            else {
                store_defer(RFM_MOVE_ACT_STATE_SAVE, s, k);
                m->fw.st_used[s] |= 1u << k;
                rfm_move_fw_state_cur(m, s, k);
            }
        } else if (r->op == ENG_ST_LOAD) {
            if (!used)
                snprintf(why, sizeof why, "state %d is empty", k + 1);
            else {
                store_defer(RFM_MOVE_ACT_STATE_LOAD, s, k);
                rfm_move_fw_state_cur(m, s, k);
            }
        } else if (r->op == ENG_ST_DELETE) {
            if (!used)
                snprintf(why, sizeof why, "state %d is empty", k + 1);
            else {   /* (as Delete + its pad, but the Move's state buffer left as it is: the page asked, not a cut) */
                j = st_job_new(J_STATE_DELETE);
                if (SL[s].st_cur == k)
                    SL[s].st_cur = -1;   /* (the firmware keeps what it holds; no state is active) */
                j->slot = s, j->index = k, j->value = 1;
                snprintf(j->id, sizeof j->id, "%s", SL[s].fw.id);
                st_post(j);
                m->fw.st_used[s] &= ~(1u << k);
                if (act == k)
                    rfm_move_fw_state_cur(m, s, -1);
            }
        } else {   /* ENG_ST_IMPORT: as a paste (STATE_PASTE): onto the active one, loaded too */
            const int onto = act == k || SL[s].st_cur == k;
            if (used && !(r->flags & ENG_STF_REPLACE))
                snprintf(why, sizeof why, "state %d holds a sound already", k + 1);
            else if (onto && !(r->flags & ENG_STF_LOAD))
                snprintf(why, sizeof why, "state %d is the active state: it can only be replaced and loaded at once", k + 1);
            else {
                j = st_job_new(J_STATE_IMPORT);
                j->slot = s, j->index = k, j->req = id, j->total = SL[s].fw.flash_size;
                j->value = r->flags & ENG_STF_LOAD ? 1 : 0;
                snprintf(j->id, sizeof j->id, "%s", SL[s].fw.id);
                snprintf(j->path, sizeof j->path, "%s", r->dir);
                if (j->value) {
                    if (!onto)   /* (the active state as it is left, as a state load writes it) */
                        slot_write(s, 1, 0, 1, NULL);
                    else
                        SL[s].st_cur = -1;   /* (no autosave into it until the file is in) */
                    fw_obj_ref(SL[s].fw.obj);
                    j->obj = SL[s].fw.obj;
                }
                st_post(j);
            }
        }
        if (!why[0] && r->op != ENG_ST_LOAD) {   /* (a state function: New's undo gone, as after any pad; a load's
                                                  * own result drops the host's) */
            if (m->fw.undo == s + 1)
                m->fw.undo = 0;
            undo_drop(s);
        } else if (!why[0] && m->fw.undo == s + 1)
            m->fw.undo = 0;
    } else {
        const uint32_t used = m->seq.proj_used;
        if (m->seq.proj_pending >= 0 || rfm_move_proj_fw_loading(m))
            snprintf(why, sizeof why, "a project is loading on the Move: try again in a moment");
        else if (r->op == ENG_PJ_OPEN) {
            if (k != cur && !(used >> k & 1))
                snprintf(why, sizeof why, "project %d is empty", k + 1);
            else if (k != cur) {   /* (rfmv_proj_open: the open one stored first, then the slot asked for) */
                rfm_move_act_t a;
                rfm_move_project_flush(m);
                m->seq.proj_pending = (int8_t)k;
                memset(&a, 0, sizeof a);
                a.kind = RFM_MOVE_ACT_PROJECT_LOAD, a.index = (uint8_t)k;
                emit(&a);
            }
        } else if (r->op == ENG_PJ_SAVE) {
            if (k != cur)
                snprintf(why, sizeof why, "project %d is not the open one", k + 1);
            else {   /* (its autosave now, changed or not: the blob, its FX state, and its firmware copies with
                      * the working memory as it is now, synced: "Saved" is the sound now, not the last autosave's) */
                for (i = 0; i < NSLOT; i++)
                    if (SL[i].on && SL[i].pc_proj == k)
                        slot_write(i, 1, 0, 1, NULL);
                m->seq.dirty = 1;
                rfm_move_project_flush(m);
            }
        } else if (r->op == ENG_PJ_DUP) {
            const int to = r->to;
            if (to < 0 || to >= RFM_MOVE_PROJECTS || to == k)
                snprintf(why, sizeof why, "no such project to copy it into");
            else if (!(used >> k & 1))
                snprintf(why, sizeof why, "project %d is empty", k + 1);
            else if ((used >> to & 1) || to == cur)
                snprintf(why, sizeof why, "project %d is not empty", to + 1);
            else {
                if (k == cur) {   /* (the open one as it is now, its copies too: as a copy of it on the Move) */
                    rfm_move_project_flush(m);
                    for (i = 0; i < NSLOT; i++)
                        if (SL[i].on && SL[i].pc_proj == k)
                            slot_write(i, 1, 0, 1, NULL);
                }
                j = st_job_new(J_PROJECT_DUP);
                j->index = k, j->value = to, j->req = id;
                st_post(j);
            }
        } else if (r->op == ENG_PJ_DELETE) {
            if (!(used >> k & 1) && !(k == cur && m->seq.dirty))
                snprintf(why, sizeof why, "project %d is empty", k + 1);
            else {   /* (the project grid's Delete + pad: gone, and the open one an empty project there; the
                      * Move's project buffer left as it is) */
                rfm_move_act_t a;
                memset(&a, 0, sizeof a);
                a.kind = RFM_MOVE_ACT_PROJECT_DELETE, a.index = (uint8_t)k;
                emit(&a);
                m->seq.proj_used &= ~(1u << k);
                rfm_move_project_info(m, k, NULL, 0);
                if (k == cur) {
                    rfm1cs_session_project_answer(S, now_ms(), k, NULL, 0);
                    m->seq.proj_used &= ~(1u << k);
                }
            }
        } else if (r->op == ENG_PJ_IMPORT) {
            if (k == cur || k == g_proj_open)
                snprintf(why, sizeof why, "project %d is open on the Move: open another project first", k + 1);
            else if ((used >> k & 1) && !(r->flags & ENG_STF_REPLACE))
                snprintf(why, sizeof why, "project %d holds a project already", k + 1);
            else {
                j = st_job_new(J_PROJECT_IMPORT);
                j->index = k, j->req = id;
                snprintf(j->path, sizeof j->path, "%s", r->dir);
                st_post(j);
            }
        } else
            snprintf(why, sizeof why, "not a request this Move knows");
    }
    if (why[0]) {
        why[0] = (char)(why[0] >= 'a' && why[0] <= 'z' ? why[0] - 32 : why[0]);
        store_fin(id, -1, why);
        return;
    }
    memset(p, 0, sizeof *p);
    p->id = id, p->op = r->op, p->slot = s, p->k = k, p->to = r->to, p->flags = r->flags;
    j = st_job_new(J_SYNC);   /* (back once everything above is done: then the page hears) */
    j->req = id;
    st_post(j);
    g_mix_ver++;
    rfm1cs_session_redraw(S, now_ms());   /* (the Move's pads and screen show it) */
}

/* the States and projects as JSON (engine_store_json), made when what it says changed (looked at every 100 ms) */
static void store_watch(uint32_t now, int force)
{
    static uint32_t last, sig_was;
    const rfm_move_t *m = &S->core;
    uint32_t h = 2166136261u;
    char *t;
    int n = 0, s, k, first = 1;
    const size_t cap = 16384;
#define MIX(v) (h = (h ^ (uint32_t)(v)) * 16777619u)
    if (!force && now - last < 100u && g_store_json)
        return;
    last = now;
    MIX(m->seq.proj_used), MIX(rfm_move_project_slot(m)), MIX(m->seq.proj_pending), MIX(rfm_move_proj_fw_loading(m));
    MIX(m->seq.dirty);
    for (s = 0; s < NSLOT; s++) {
        MIX(SL[s].on), MIX(SL[s].on ? SL[s].serial : 0), MIX(m->fw.st_used[s]), MIX(m->fw.st_cur[s] + 1);
        MIX(slot_color(s));
    }
    for (k = 0; k < RFM_MOVE_PROJECTS; k++) {
        const rfm_move_proj_info_t *in = &m->seq.info[k];
        const char *c;
        MIX(in->ok), MIX(in->color), MIX(in->bpm10), MIX(in->clips);
        for (s = 0; s < RFM_MOVE_FW_SLOTS; s++)
            for (c = in->fw[s]; *c; c++)
                MIX(*c);
    }
#undef MIX
    if (h == sig_was && g_store_json)
        return;
    sig_was = h;
    if (!(t = malloc(cap)))
        return;
    g_store_ver++;
#define PUT(...) do { if (n < (int)cap) n += snprintf(t + n, cap - (size_t)n, __VA_ARGS__); } while (0)
    PUT("{\"ver\":%u,\"open\":%d,\"pending\":%d,\"loading\":%d,\"used\":%u,\"dirty\":%d,\"projects\":[", g_store_ver,
        rfm_move_project_slot(m), m->seq.proj_pending, rfm_move_proj_fw_loading(m), m->seq.proj_used, m->seq.dirty ? 1 : 0);
    for (k = 0; k < RFM_MOVE_PROJECTS; k++) {
        const rfm_move_proj_info_t *in = &m->seq.info[k];
        const uint8_t *c = PAL[(in->color ? in->color - 1 : k % RFM_MOVE_PROJ_COLORS) % 16];
        if (!(m->seq.proj_used >> k & 1))
            continue;
        PUT("%s{\"k\":%d,\"color\":\"#%02x%02x%02x\",\"bpm\":%.1f,\"clips\":%d,\"fw\":[\"%s\",\"%s\",\"%s\"]}",
            first ? "" : ",", k, c[0], c[1], c[2], in->ok ? in->bpm10 / 10.0 : 0.0, in->ok ? in->clips : 0,
            in->ok ? in->fw[0] : "", in->ok ? in->fw[1] : "", in->ok ? in->fw[2] : "");
        first = 0;
    }
    PUT("],\"slots\":[");
    for (s = 0; s < NSLOT; s++) {
        char nm[160];
        const uint8_t *c = PAL[slot_color(s)];
        if (!SL[s].on) {
            PUT("%snull", s ? "," : "");
            continue;
        }
        mj_escape(SL[s].fw.name, nm, sizeof nm);
        PUT("%s{\"id\":\"%s\",\"name\":\"%s\",\"version\":\"%s\",\"used\":%u,\"cur\":%d,\"color\":\"#%02x%02x%02x\"}",
            s ? "," : "", SL[s].fw.id, nm, SL[s].fw.version, m->fw.st_used[s], m->fw.st_cur[s], c[0], c[1], c[2]);
    }
    PUT("]}");
#undef PUT
    pthread_mutex_lock(&g_vmu);
    free(g_store_json);
    g_store_json = t;
    pthread_mutex_unlock(&g_vmu);
    g_mix_ver++;   /* (its version in the mix JSON: the page reads it again) */
}

int engine_store(const eng_store_t *r)
{
    int id = -1;
    pthread_mutex_lock(&g_vmu);
    if (g_nsq < (int)(sizeof g_sq / sizeof g_sq[0])) {
        int i;
        id = ++g_sq_next;
        g_sq[g_nsq].r = *r;
        g_sq[g_nsq].r.id[sizeof g_sq[g_nsq].r.id - 1] = 0;
        g_sq[g_nsq].r.dir[sizeof g_sq[g_nsq].r.dir - 1] = 0;
        g_sq[g_nsq++].id = id;
        i = g_sdone_at++ % (int)(sizeof g_sdone / sizeof g_sdone[0]);   /* (under way) */
        g_sdone[i].id = id, g_sdone[i].st = 0, g_sdone[i].msg[0] = 0;
    }
    pthread_mutex_unlock(&g_vmu);
    return id;
}

int engine_store_done(int req, char *msg, int cap)
{
    int i, st = -2;
    pthread_mutex_lock(&g_vmu);
    for (i = 0; i < (int)(sizeof g_sdone / sizeof g_sdone[0]); i++)
        if (g_sdone[i].id == req) {
            st = g_sdone[i].st;
            if (msg && cap > 0)
                snprintf(msg, (size_t)cap, "%s", g_sdone[i].msg);
            break;
        }
    pthread_mutex_unlock(&g_vmu);
    return st;
}

char *engine_store_json(void)
{
    char *r;
    pthread_mutex_lock(&g_vmu);
    r = g_store_json ? strdup(g_store_json) : NULL;
    pthread_mutex_unlock(&g_vmu);
    return r;
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

/* tests: bytes of a slot's memory changed from outside (a firmware's own edit, as far as the host can tell) */
static struct { int slot; uint32_t off; uint8_t val; } g_tpoke[8];
static int g_ntpoke;
int engine_test_poke(int slot, uint32_t off, int val)
{
    int r = -1;
    if (!C.test_api || slot < 0 || slot >= NSLOT)
        return -1;
    pthread_mutex_lock(&g_vmu);
    if (g_ntpoke < (int)(sizeof g_tpoke / sizeof g_tpoke[0])) {
        g_tpoke[g_ntpoke].slot = slot, g_tpoke[g_ntpoke].off = off, g_tpoke[g_ntpoke].val = (uint8_t)val;
        g_ntpoke++;
        r = 0;
    }
    pthread_mutex_unlock(&g_vmu);
    return r;
}
static void test_pokes(void)
{
    int i, n;
    struct { int slot; uint32_t off; uint8_t val; } p[8];
    pthread_mutex_lock(&g_vmu);
    n = g_ntpoke;
    memcpy(p, g_tpoke, sizeof p[0] * (size_t)n);
    g_ntpoke = 0;
    pthread_mutex_unlock(&g_vmu);
    for (i = 0; i < n; i++) {
        slot_t *sl = &SL[p[i].slot];
        uint8_t *img = sl->on ? (uint8_t *)fw_flash(&sl->fw) : NULL;
        if (!img || p[i].off >= sl->fw.flash_size)
            continue;
        img[p[i].off] = p[i].val;
        sl->pc_stale = sl->st_stale = 1;   /* (the firmware's own counter did not move: the host's flags do) */
        sl->dirty = 1, sl->dirty_ms = now_ms();
    }
}

static pthread_mutex_t g_peek_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_peek_cv = PTHREAD_COND_INITIALIZER;
static struct { int want, done, ok, slot, lcd; uint32_t off, n, sum; uint8_t b[64]; } g_peek;
int engine_test_peek(int slot, int lcd, uint32_t off, uint32_t n, uint8_t *out, uint32_t *sum)
{
    struct timespec until;
    int ok;
    if (!C.test_api || slot < 0 || slot >= NSLOT || n > sizeof g_peek.b)
        return -1;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 2;
    pthread_mutex_lock(&g_peek_mu);
    while (g_peek.want)   /* (one at a time) */
        if (pthread_cond_timedwait(&g_peek_cv, &g_peek_mu, &until) != 0) {
            pthread_mutex_unlock(&g_peek_mu);
            return -1;
        }
    g_peek.want = 1, g_peek.done = 0, g_peek.slot = slot, g_peek.lcd = lcd, g_peek.off = off, g_peek.n = n;
    while (!g_peek.done)
        if (pthread_cond_timedwait(&g_peek_cv, &g_peek_mu, &until) != 0)
            break;
    ok = g_peek.done && g_peek.ok;
    if (ok && out)
        memcpy(out, g_peek.b, n);
    if (ok && sum)
        *sum = g_peek.sum;
    g_peek.want = 0;
    pthread_cond_broadcast(&g_peek_cv);
    pthread_mutex_unlock(&g_peek_mu);
    return ok ? 0 : -1;
}
static void test_peeks(void)   /* (the engine's thread) */
{
    pthread_mutex_lock(&g_peek_mu);
    if (g_peek.want && !g_peek.done) {
        fw_t *fw = SL[g_peek.slot].on && !SL[g_peek.slot].fw.dead ? &SL[g_peek.slot].fw : NULL;
        g_peek.ok = 0;
        if (fw && g_peek.lcd) {
            const uint8_t *l = fw_lcd(fw);
            uint32_t h = 2166136261u, i;
            for (i = 0; i < FW_LCD_W * FW_LCD_H * 2; i++)
                h = (h ^ l[i]) * 16777619u;
            g_peek.sum = h, g_peek.ok = 1;
        } else if (fw && (uint64_t)g_peek.off + g_peek.n <= fw->mem_size) {
            memcpy(g_peek.b, fw->mem + g_peek.off, g_peek.n);
            g_peek.ok = 1;
        }
        g_peek.done = 1;
        pthread_cond_broadcast(&g_peek_cv);
    }
    pthread_mutex_unlock(&g_peek_mu);
}

int engine_test_midiout(const uint8_t *msgs, int n)
{
    int r = -1;
    if (!C.test_api || n <= 0)
        return -1;
    pthread_mutex_lock(&g_vmu);
    if (g_ntmo + n <= (int)sizeof g_tmo) {
        memcpy(g_tmo + g_ntmo, msgs, (size_t)n);
        g_ntmo += n;
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

/* ---- MIDI to and from the page (the firmware editors' SysEx: web.c's ws op 32 in, op 5 out) */
#define PM_CAP (256 * 1024)
static uint8_t *g_pm;                        /* in: whole messages, each [len lo, len hi, bytes] (g_vmu) */
static uint32_t g_pm_n, g_pm_dropped;
#define MO_CAP 65536u
static uint8_t g_mo[MO_CAP];                 /* out: the active firmware's MIDI out, SysEx and channel messages (g_vmu) */
static uint32_t g_mo_pos;                    /* bytes ever written (a page reads from its own position) */
static uint32_t g_pm_msgs, g_pm_bytes, g_mo_bytes;   /* (the status) */

int engine_midi(const uint8_t *m, int n)
{
    int r = -1;
    if (n <= 0 || n > 65535)
        return -1;
    pthread_mutex_lock(&g_vmu);
    if (!g_pm)
        g_pm = malloc(PM_CAP);
    if (g_pm && g_pm_n + 2u + (uint32_t)n <= PM_CAP) {
        g_pm[g_pm_n] = (uint8_t)n, g_pm[g_pm_n + 1] = (uint8_t)(n >> 8);
        memcpy(g_pm + g_pm_n + 2, m, (size_t)n);
        g_pm_n += 2u + (uint32_t)n;
        r = 0;
    } else
        g_pm_dropped++;
    pthread_mutex_unlock(&g_vmu);
    return r;
}

uint32_t engine_midi_out_pos(void)
{
    uint32_t p;
    pthread_mutex_lock(&g_vmu);
    p = g_mo_pos;
    pthread_mutex_unlock(&g_vmu);
    return p;
}

int engine_midi_out(uint8_t *buf, int cap, uint32_t *pos)
{
    uint32_t avail, k, i;
    pthread_mutex_lock(&g_vmu);
    if (g_mo_pos - *pos > MO_CAP)   /* (fell behind: from what is still kept) */
        *pos = g_mo_pos - MO_CAP;
    avail = g_mo_pos - *pos;
    k = avail < (uint32_t)cap ? avail : (uint32_t)cap;
    for (i = 0; i < k; i++)
        buf[i] = g_mo[(*pos + i) % MO_CAP];
    *pos += k;
    pthread_mutex_unlock(&g_vmu);
    return (int)k;
}

/* the screen reader protocol's HELLO / BYE into a slot's firmware (fmreader.h) */
static int fmr_send(int s, const uint8_t *m, int n)
{
    return s >= 0 && s < NSLOT && SL[s].on && !SL[s].fw.dead ? fw_midi_feed(&SL[s].fw, m, n) : -1;
}

/* the page's messages into the active firmware (fw_midi_feed: as many packets a device ms as it takes) */
static void page_midi(void)
{
    static uint8_t *tmp;
    uint32_t n, at = 0;
    if (!g_pm_n)   /* (a racy peek: the next loop sees it) */
        return;
    if (!tmp && !(tmp = malloc(PM_CAP)))
        return;
    pthread_mutex_lock(&g_vmu);
    n = g_pm_n;
    memcpy(tmp, g_pm, n);
    g_pm_n = 0;
    pthread_mutex_unlock(&g_vmu);
    while (at + 2 <= n) {
        const uint32_t len = (uint32_t)tmp[at] | (uint32_t)tmp[at + 1] << 8;
        if (at + 2 + len > n)
            break;
        if (SL[g_active].on && fw_midi_feed(&SL[g_active].fw, tmp + at + 2, (int)len) == 0)
            g_pm_msgs++, g_pm_bytes += len;
        else
            g_pm_dropped++;
        at += 2 + len;
    }
}

/* the active firmware's MIDI out for the page (not the real-time bytes: those are the sequencer's) */
static void midi_to_page(const uint32_t *pk, int k)
{
    int i, j;
    pthread_mutex_lock(&g_vmu);
    for (i = 0; i < k; i++) {
        uint8_t b[3];
        const int n = fw_pkt_bytes(pk[i], b);
        if ((pk[i] & 0xF) == 0xF || b[0] == 0xF2 || (n == 1 && b[0] >= 0xF8))
            continue;
        for (j = 0; j < n; j++)
            g_mo[g_mo_pos++ % MO_CAP] = b[j];
        g_mo_bytes += (uint32_t)n;
    }
    pthread_mutex_unlock(&g_vmu);
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

char *engine_catalog(void)
{
    char *r = NULL;
    pthread_mutex_lock(&g_vmu);
    if (g_cat_json) {
        const size_t a = strlen(g_cat_json), b = strlen(g_fav_json);
        if ((r = malloc(a + b + 20))) {
            memcpy(r, g_cat_json, a);
            memcpy(r + a, ",\"favorites\":", 13);
            memcpy(r + a + 13, g_fav_json, b);
            memcpy(r + a + 13 + b, "}", 2);
        }
    }
    pthread_mutex_unlock(&g_vmu);
    return r;
}

int engine_download(const char *id)
{
    size_t i, n = id ? strlen(id) : 0;
    int r = -1;
    if (!n || n >= RFM_MOVE_FW_ID_MAX)
        return -1;
    for (i = 0; i < n; i++)
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9') || id[i] == '_' || id[i] == '-'))
            return -1;
    pthread_mutex_lock(&g_vmu);
    if (!g_wdl[0]) {
        memcpy(g_wdl, id, n + 1);
        r = 0;
    }
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

/* FRAMES (README "Frames"): the slot on screen draws at the Move's rate (33 ms: rfm1cs sends the OLED at most every
 * 33 ms), the others a few times a second (the page's small screens); every slot keeps tick() each ms and idle() in
 * between, as the ABI's clock has it. The rate goes in as an input (FWI_FRAMES), so the input log replays it */
static void frames_policy(void)
{
    int s;
    const int fa = C.frame_ms_active > 0 ? C.frame_ms_active : 33, fb = C.frame_ms_bg > 0 ? C.frame_ms_bg : 200;
    for (s = 0; s < NSLOT; s++) {
        slot_t *sl = &SL[s];
        const int want = s == g_active ? fa : fb;
        if (!sl->on || !sl->fw.inst || sl->fw.dead)
            continue;
        if (sl->frames_inst != sl->fw.inst || sl->frames_ms != want) {
            fw_in(&sl->fw, FWI_FRAMES, want > 255 ? 255 : want, 0, 0, 0);
            sl->frames_inst = sl->fw.inst, sl->frames_ms = want;
        }
    }
}

/* the active firmware's screen and LEDs: to the session (the Move) and to the page */
static void publish(uint32_t now)
{
    frames_policy();
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
    {   /* live labels: all 49 (knobs, buttons, keys: DYNAMIC-LABELS order) to rfm1cs (0.5.6), which shows them */
        const uint32_t e = fw_labels_epoch(fw);
        if (e && e != fw->lbl_epoch) {
            static char txt[RFM_MOVE_LBL_N][16];   /* (fw_label's text is one buffer, rewritten by each call: a copy
                                                    * each, else every label is the last one read: "SEL/KEYS") */
            const char *lbl[RFM_MOVE_LBL_N];
            int r;
            fw->lbl_epoch = e;
            for (r = 0; r < RFM_MOVE_LBL_N; r++) {
                const char *l = fw_label(fw, r);
                lbl[r] = l ? txt[r] : NULL;
                if (l)
                    snprintf(txt[r], sizeof txt[r], "%s", l);
            }
            rfm_move_set_labels(&S->core, lbl);
            rfm1cs_session_redraw(S, now);
            view_info();   /* (the page's labels) */
        }
    }
    {   /* rd1: a new readout (its seq moved) to rfm1cs (0.5.7: the knob's popup, its name then its value) and the page */
        fw_readout_t r;
        if (fw_readout(fw, &r, 1)) {
            rfm_move_readout(&S->core, r.control, r.kind, r.title, r.value, r.frac, r.flags, r.ms);
#ifndef RFM_MOVE_HAS_SPEECH   /* (else rfm1cs says it, with its popup) */
            if (r.title[0] && r.value[0])   /* (Schwung's "label: value") */
                ann_say(ANN_VALUE, "%s: %s", r.title, r.value);
            else
                ann_say(ANN_VALUE, "%s", r.title[0] ? r.title : r.value);
#endif
            g_rd = r;
            rfm1cs_session_redraw(S, now);
            view_info();
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


/* ------------------------------------------------------------------ the slots' render threads --- */
/* README "Threads". Each slot renders on a thread of its own (rfm1-slot1..3, cores 0..2), fork-join per block: the
 * engine's thread does everything that touches a firmware between two blocks, exactly as before (the sequencer's
 * MIDI, the panel, the queued inputs, MIDI out, the flash, the screen), then hands block N to every sounding slot's
 * thread and waits for them all, then mixes in slot order. While a slot's thread renders, nothing else touches that
 * instance; so each firmware gets the same calls in the same order as on one thread, and the sound is the same bytes.
 * --threads 0 (cfg.serial): every slot rendered on the engine's thread, as before. */
#define SPIN_GO_NS 50000u       /* a slot's thread spins this long for the next block before it sleeps */
#define SPIN_JOIN_NS 30000u     /* the engine spins this long for a slot before it sleeps */
typedef struct {
    pthread_t thr;
    int started, tid, slot;
    uint32_t go, done, quit;            /* (futex words: go moves per block handed over, done follows it) */
    int16_t blk[FW_BLOCK * 2];
    const int16_t *ain;                 /* this block's input (fw_render_in), NULL none */
    int with_ain;
    /* the render time of this slot's blocks: this window (the engine reads and clears it once a second) */
    uint64_t win_ns, win_n;
    uint32_t win_max_us;
} rthr_t;
static rthr_t RT[NSLOT];
static int g_threads;                   /* the slots render on their own threads */
static uint64_t g_blk_ns, g_blk_n, g_par_ns;   /* this window: render_block's whole time, its blocks, the fork-join's */
static uint32_t g_blk_max_us, g_blk_avg_us, g_blk_max_last;   /* (the last second's, for the log) */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
static void fu_wait(uint32_t *a, uint32_t v)
{
#ifdef __linux__
    syscall(SYS_futex, a, FUTEX_WAIT_PRIVATE, v, NULL, NULL, 0);
#else
    (void)a, (void)v;
    sched_yield();
#endif
}
static void fu_wake(uint32_t *a)
{
#ifdef __linux__
    syscall(SYS_futex, a, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
#else
    (void)a;
#endif
}

/* ---- real time without root: Move Remote's way. bin/heal (the module's setuid helper, installed by schwung-heal)
 * puts a named thread of this process at SCHED_FIFO 70; the thread is then lowered to its own priority, which needs
 * no privilege. No helper (the simulator, a missing install): normal priority, as before */
static void thread_setup(const char *name, int core, int prio)
{
#ifdef __linux__
    pthread_setname_np(pthread_self(), name);
    if (C.cpu_mask) {   /* (the Move: cores 0..2; a slot's own core when it has one) */
        cpu_set_t set;
        int c;
        CPU_ZERO(&set);
        for (c = 0; c < 8; c++)
            if (core >= 0 ? c == core : C.cpu_mask >> c & 1)
                CPU_SET(c, &set);
        sched_setaffinity(0, sizeof set, &set);
    }
    if (C.heal && prio > 0) {
        struct stat st;
        const int tid = (int)syscall(SYS_gettid);
        if (stat(C.heal, &st) == 0 && st.st_uid == 0 && (st.st_mode & S_ISUID)) {
            char pid[16], t[16];
            char *argv[] = { (char *)C.heal, "rt", pid, t, NULL };
            extern char **environ;
            pid_t child;
            int status = -1;
            struct sched_param sp = { .sched_priority = prio };
            snprintf(pid, sizeof pid, "%d", (int)getpid());
            snprintf(t, sizeof t, "%d", tid);
            if (posix_spawn(&child, C.heal, NULL, NULL, argv, environ) == 0)
                waitpid(child, &status, 0);
            if (sched_getscheduler(0) == SCHED_FIFO)
                sched_setscheduler(0, SCHED_FIFO, &sp);   /* (70 -> prio: lowering is allowed) */
        }
        fprintf(stderr, "engine: thread %s (tid %d): %s %d\n", name, tid,
                sched_getscheduler(0) == SCHED_FIFO ? "SCHED_FIFO" : "normal priority, not",
                sched_getscheduler(0) == SCHED_FIFO ? prio : 0);
    }
#else
    (void)name, (void)core, (void)prio;
#endif
}
#define PRIO_ENGINE 58
#define PRIO_SLOT 60
#define PRIO_C1 60

static void c1_thread_init(fw_t *fw)   /* (fwrt.h fw_on_c1_thread: a core-1 thread starting) */
{
    char name[16];
    int s;
    for (s = 0; s < NSLOT && &SL[s].fw != fw; s++)
        ;
    snprintf(name, sizeof name, "rfm1-c1-%d", s + 1);
    thread_setup(name, -1, PRIO_C1);
}

static void slot_render(int s, int16_t *blk, const int16_t *ain, int with_ain)
{
    slot_t *sl = &SL[s];
    rthr_t *r = &RT[s];
    const uint64_t t0 = now_ns();
    uint64_t dt;
    if (with_ain)
        fw_render_in(&sl->fw, blk, ain);
    else
        fw_render(&sl->fw, blk);
    dt = now_ns() - t0;
    r->win_ns += dt;
    r->win_n++;
    if (dt / 1000u > r->win_max_us)
        r->win_max_us = (uint32_t)(dt / 1000u);
}

static void *rthr_main(void *arg)
{
    rthr_t *r = arg;
    char name[16];
    uint32_t seen = 0;
    r->tid = (int)syscall(SYS_gettid);
    snprintf(name, sizeof name, "rfm1-slot%d", r->slot + 1);
    thread_setup(name, r->slot, PRIO_SLOT);
    for (;;) {
        uint32_t g;
        const uint64_t t0 = now_ns();
        while ((g = __atomic_load_n(&r->go, __ATOMIC_ACQUIRE)) == seen && !__atomic_load_n(&r->quit, __ATOMIC_ACQUIRE))
            if (now_ns() - t0 > SPIN_GO_NS)
                fu_wait(&r->go, seen);
        if (g == seen)
            break;   /* (quit) */
        seen = g;
        slot_render(r->slot, r->blk, r->ain, r->with_ain);
        __atomic_store_n(&r->done, seen, __ATOMIC_RELEASE);
        fu_wake(&r->done);
    }
    return NULL;
}

static void rthr_start(void)
{
    int s;
    if (C.serial)
        return;
    for (s = 0; s < NSLOT; s++) {
        RT[s].slot = s;
        if (pthread_create(&RT[s].thr, NULL, rthr_main, &RT[s]) != 0) {
            fprintf(stderr, "engine: no render thread for slot %d: every slot on the engine's thread\n", s + 1);
            break;
        }
        RT[s].started = 1;
    }
    g_threads = s == NSLOT;
}

static void rthr_stop(void)
{
    int s;
    for (s = 0; s < NSLOT; s++)
        if (RT[s].started) {
            __atomic_store_n(&RT[s].quit, 1, __ATOMIC_RELEASE);
            fu_wake(&RT[s].go);
            pthread_join(RT[s].thr, NULL);
            RT[s].started = 0;
        }
    g_threads = 0;
}

/* the sounding slots' blocks (bit n of mask: slot n), rendered on their threads at once (or here, in order); each
 * slot's block in RT[s].blk */
static void render_slots(int mask, const int16_t *ain)
{
    const uint64_t t0 = now_ns();
    int s;
    for (s = 0; s < NSLOT; s++)
        if (mask >> s & 1) {
            RT[s].with_ain = fw_has_ain(&SL[s].fw);
            RT[s].ain = ain;
        }
    if (!g_threads) {
        for (s = 0; s < NSLOT; s++)
            if (mask >> s & 1)
                slot_render(s, RT[s].blk, RT[s].ain, RT[s].with_ain);
    } else {
        for (s = 0; s < NSLOT; s++)
            if (mask >> s & 1) {
                __atomic_store_n(&RT[s].go, RT[s].go + 1, __ATOMIC_RELEASE);
                fu_wake(&RT[s].go);
            }
        for (s = 0; s < NSLOT; s++)
            if (mask >> s & 1) {
                const uint64_t t1 = now_ns();
                uint32_t d;
                while ((d = __atomic_load_n(&RT[s].done, __ATOMIC_ACQUIRE)) != RT[s].go)
                    if (now_ns() - t1 > SPIN_JOIN_NS)
                        fu_wait(&RT[s].done, d);
            }
    }
    g_par_ns += now_ns() - t0;
}

/* ---- each thread's CPU (/proc/self/task/<tid>/stat): by name, % of a core over the last window */
#define TCPU_MAX 48
static struct { int tid; char name[16]; uint64_t ticks; uint32_t permille; int seen; } g_tcpu[TCPU_MAX];
static int g_ntcpu;
static void tcpu_sample(uint32_t dt_ms)
{
#ifdef __linux__
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    long hz = sysconf(_SC_CLK_TCK);
    int i, k;
    if (!d)
        return;
    for (i = 0; i < g_ntcpu; i++)
        g_tcpu[i].seen = 0;
    while ((e = readdir(d))) {
        char p[64], buf[512], *q;
        unsigned long ut = 0, st = 0;
        int tid = atoi(e->d_name), fd, n;
        if (tid <= 0)
            continue;
        snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
        if ((fd = open(p, O_RDONLY)) < 0)
            continue;
        n = (int)read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = 0;
        if (!(q = strrchr(buf, ')')))
            continue;
        /* after "pid (comm)": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
        if (sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2)
            continue;
        for (i = 0; i < g_ntcpu && g_tcpu[i].tid != tid; i++)
            ;
        if (i == g_ntcpu) {
            char *a = strchr(buf, '(');
            if (g_ntcpu == TCPU_MAX || !a)
                continue;
            g_ntcpu++;
            g_tcpu[i].tid = tid;
            snprintf(g_tcpu[i].name, sizeof g_tcpu[i].name, "%.*s", (int)(q - a - 1), a + 1);
            g_tcpu[i].ticks = ut + st;
            g_tcpu[i].permille = 0;
        } else {
            const uint64_t dtk = ut + st - g_tcpu[i].ticks;
            g_tcpu[i].ticks = ut + st;
            g_tcpu[i].permille = (uint32_t)(dtk * 1000000u / (uint64_t)(hz > 0 ? hz : 100) / (dt_ms ? dt_ms : 1));
        }
        g_tcpu[i].seen = 1;
    }
    closedir(d);
    for (i = k = 0; i < g_ntcpu; i++)   /* (threads gone) */
        if (g_tcpu[i].seen)
            g_tcpu[k++] = g_tcpu[i];
    g_ntcpu = k;
#else
    (void)dt_ms;
#endif
}
/* {"rfm1-engine":12.5,...}: the threads that used any CPU, plus every rfm1-* and mr-audio */
static int tcpu_json(char *out, int cap)
{
    int i, n = snprintf(out, (size_t)cap, "{");
    for (i = 0; i < g_ntcpu && n < cap; i++)
        if (g_tcpu[i].permille || !strncmp(g_tcpu[i].name, "rfm1-", 5) || !strcmp(g_tcpu[i].name, "mr-audio"))
            n += snprintf(out + n, (size_t)(cap - n), "%s\"%s\":%.1f", n > 1 ? "," : "", g_tcpu[i].name,
                          g_tcpu[i].permille / 10.0);
    if (n < cap)
        n += snprintf(out + n, (size_t)(cap - n), "}");
    return n < cap ? n : cap - 1;
}

/* ------------------------------------------------------------------ the clock --- */
static void render_block_(uint32_t now);
static void render_block(uint32_t now)   /* (g_nest: a snapshot does not render ahead from inside one) */
{
    g_nest++;
    render_block_(now);
    g_nest--;
}
static void render_block_(uint32_t now)
{
    int16_t blk[FW_BLOCK * 2], out[FW_BLOCK * 2];
    const int16_t *ain;
    int s, k, sounding, sends = 0;
    const uint64_t t_blk = now_ns();
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
    if (C.test_api) {   /* (tests: Out's messages from the block, as a firmware's would go) */
        static uint8_t m[sizeof g_tmo];
        int mn;
        pthread_mutex_lock(&g_vmu);   /* (the engine's own mutex, held only to copy) */
        mn = g_ntmo;
        memcpy(m, g_tmo, (size_t)mn);
        g_ntmo = 0;
        pthread_mutex_unlock(&g_vmu);
        if (mn)
            midiout_send(m, mn);
    }
    {   /* the slots that sound: ain1's state first (between calls, as before), then their blocks, at once */
        int mask = 0;
        for (s = 0; s < NSLOT; s++) {
            slot_t *sl = &SL[s];
            if (!sl->on || !(sounding >> s & 1))
                continue;   /* (loaded, past the voices: its clock waits) */
            if (fw_has_ain(&sl->fw))   /* ain1: the state when it changed, the input right before its audio */
                fw_ain_info(&sl->fw, ain != NULL, engine_ain_latency());
            mask |= 1 << s;
        }
        render_slots(mask, ain);
        for (s = 0; s < NSLOT; s++)   /* tk1: each record after the block (its frames, its rfm1_tk_input calls) */
            if (mask >> s & 1)
                tk_poll(s, 0);
        for (s = 0; s < NSLOT; s++) {   /* the mix, in slot order */
            slot_t *sl = &SL[s];
            const int16_t *sb = RT[s].blk;
            if (!(mask >> s & 1))
                continue;
            if (&sl->fw == g_raw_fw && g_cap_raw && g_cap_raw_n + FW_BLOCK <= g_cap_max) {
                memcpy(g_cap_raw + 2u * g_cap_raw_n, sb, sizeof RT[s].blk);
                g_cap_raw_n += FW_BLOCK;
            }
            if (&sl->fw == g_scap[s].fw && g_scap[s].n + FW_BLOCK <= g_cap_max) {
                memcpy(g_scap[s].pcm + 2u * g_scap[s].n, sb, sizeof RT[s].blk);
                g_scap[s].n += FW_BLOCK;
            }
            mix_add_lp(&g_mix, sb, FW_BLOCK, sl->level, sl->pan);
            for (k = 0; k < RFM_MOVE_FX_SLOTS; k++)   /* the sends: after its level and pan */
                if (sends >> k & 1)
                    bus_add(&g_bus[k], sb, FW_BLOCK, sl->level, sl->pan, sl->send[k]);
            {
                const int p = peak16(sb, FW_BLOCK * 2) * sl->level / 127;
                if (p > sl->peak)
                    sl->peak = p;
                if (p > sl->pk_acc)
                    sl->pk_acc = p;
            }
            if (s != g_active && sl->on && g_mroute[s]) {   /* (0.6.6) a slot not played: its MIDI out where it's routed */
                uint32_t raw[64], pk[65];
                const int n = fmr_filter(s, raw, fw_midi_out(&sl->fw, raw, 64), pk);   /* (the reader's frames out) */
                route_out(s, pk, n);
            }
            if (s == g_active && sl->on) {   /* its MIDI out: the clock the sequencer can follow; the rest to the page */
                uint32_t raw[64], pk[65];
                const int n = fmr_filter(s, raw, fw_midi_out(&sl->fw, raw, 64), pk);   /* (the reader's frames out) */
                int i;
                for (i = 0; i < n; i++) {
                    const uint8_t b = (uint8_t)(pk[i] >> 8);
                    if (b == 0xF8 || b == 0xFA || b == 0xFB || b == 0xFC) {
                        rfm1cs_session_midi_in(S, now, &b, 1);
                    } else if (b == 0xF2) {
                        const uint8_t m[3] = { b, (uint8_t)(pk[i] >> 16), (uint8_t)(pk[i] >> 24) };
                        rfm1cs_session_midi_in(S, now, m, 3);
                    }
                }
                if (n)
                    midi_to_page(pk, n);
                if (n && C.midi_out && !(g_mroute[s] & RFM_MOVE_MROUTE_OUT))   /* (--midi-out: the slot played to Out) */
                    midiout_send_pkts(&g_mo_asm[s], pk, n);
                route_out(s, pk, n);
            }
        }
        do_deferred();   /* (what the active slot's clock made the sequencer do: after every slot's block is mixed) */
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
    {
        const uint64_t dt = now_ns() - t_blk;
        g_blk_ns += dt;
        g_blk_n++;
        if (dt / 1000u > g_blk_max_us)
            g_blk_max_us = (uint32_t)(dt / 1000u);
    }
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
        /* (README "The working memory") the active state's and the open project's working memory: written again when
         * the firmware was played since it was last written (its input; not its clock: a loop playing by itself is
         * not a change of its sound) and that was MEM_AUTOSAVE_MS ago; one slot a pass (each a copy of its memory) */
        for (s = 0; s < NSLOT; s++) {
            const int k = (int)((last_save_check / 250u + (uint32_t)s) % NSLOT);   /* (the slots in turn) */
            slot_t *sl = &SL[k];
            if (sl->on && !sl->fw.dead && (sl->pc_proj >= 0 || sl->st_cur >= 0) && sl->fw.n_in != sl->in_seen &&
                now - sl->mem_ms >= g_mem_autosave_ms && fw_snap_able(&sl->fw)) {
                slot_write(k, 0, 0, 1, NULL);
                break;
            }
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
                     "\"active\":%d,\"master\":%d,\"move_mode\":%d,\"tk_shown\":%d,\"seq_playing\":%d,"
                     "\"seq_following\":%d,",
                     g_blocks, g_late, g_rt_blocks, mr_ring_count(&g_ring), g_mix.limited, g_peak, S->st.state,
                     S->st.mode, S->st.inputs, S->st.led_writes, S->st.screens, g_active, g_master,
                     rfm_move_mode(&S->core), rfm_move_tk_shown(&S->core), S->core.seq.playing ? 1 : 0,
                     S->core.seq.following ? 1 : 0);
        if (C.test_api) {   /* (tests: the pads' colours as last written to the Move, 6 hex digits each) */
            int p;
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, "\"move_pads\":\"");
            for (p = 0; p < RFM_MOVE_NPADS; p++)
                n += snprintf(tmp + n, sizeof tmp - (size_t)n, "%02x%02x%02x", S->core.sent[p][0], S->core.sent[p][1],
                              S->core.sent[p][2]);
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, "\",");
        }
        n += snprintf(tmp + n, sizeof tmp - (size_t)n, "\"slots\":[");
        for (s = 0; s < NSLOT; s++) {
            slot_t *sl = &SL[s];
            if (sl->on) {
                const uint64_t ns = sl->fw.ns_total - sl->ns_mark;
                sl->ns_mark = sl->fw.ns_total;
                sl->cpu_permille = (uint32_t)(ns / 1000u / (dt ? dt : 1));   /* us per ms = permille */
                sl->pk_last = sl->pk_acc, sl->pk_acc = 0;
                n += snprintf(tmp + n, sizeof tmp - (size_t)n,
                              "%s{\"id\":\"%s\",\"cpu\":%.1f,\"worst_us\":%u,\"level\":%u,\"boot\":%u,"
                              "\"states\":%u,\"dead\":%d,\"peak\":%d,\"notes\":%u", s ? "," : "", sl->fw.id,
                              sl->cpu_permille / 10.0, sl->fw.worst_us, sl->level, sl->serial, S->core.fw.st_used[s],
                              sl->fw.dead, sl->pk_last, sl->notes);
                if (sl->fw.wasi && sl->fw.wasi->fs) {   /* a Fantasy CFW's file API (fsvc.h) */
                    fsvc_stats_t fst;
                    fsvc_stats(sl->fw.wasi->fs, &fst);
                    n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"fs_req\":%u,\"fs_done\":%u,\"fs_refused\":%u,"
                                  "\"fs_read\":%llu,\"fs_written\":%llu,\"mem_mb\":%llu", fst.made, fst.served,
                                  fst.refused + fst.busy, (unsigned long long)fst.bytes_in,
                                  (unsigned long long)fst.bytes_out, (unsigned long long)(sl->fw.mem_size >> 20));
                }
                {   /* threads: this slot's blocks over the last second (avg, worst), its object, core 1 */
                    rthr_t *r = &RT[s];
                    fw_c1_stats_t c1;
                    n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"render_avg_us\":%u,\"render_max_us\":%u,"
                                  "\"render_blocks\":%u,\"obj\":\"%s\"", r->win_n ? (uint32_t)(r->win_ns / r->win_n / 1000u) : 0,
                                  r->win_max_us, (uint32_t)r->win_n, !sl->fw.obj ? "" : sl->fw.obj->mt ? "mt"
                                  : sl->fw.obj->lg ? "old" : "old-locked");
                    {   /* where the blocks' time went: the sound, the clock, the screen (frame calls, their mean) */
                        const uint64_t da = sl->fw.ns_audio - sl->sp_audio, dk = sl->fw.ns_tick - sl->sp_tick,
                                       df = sl->fw.ns_frame - sl->sp_frame;
                        const uint32_t nf = sl->fw.n_frame - sl->sp_nframe, nb = r->win_n ? (uint32_t)r->win_n : 1u;
                        n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"split\":{\"audio_us\":%u,\"tick_us\":%u,"
                                      "\"frame_us\":%u,\"frames\":%u,\"frame_call_us\":%u}", (uint32_t)(da / 1000u / nb),
                                      (uint32_t)(dk / 1000u / nb), (uint32_t)(df / 1000u / nb), nf,
                                      nf ? (uint32_t)(df / 1000u / nf) : 0u);
                        sl->sp_audio = sl->fw.ns_audio, sl->sp_tick = sl->fw.ns_tick, sl->sp_frame = sl->fw.ns_frame;
                        sl->sp_nframe = sl->fw.n_frame;
                    }
                    sl->rend_avg_us = r->win_n ? (uint32_t)(r->win_ns / r->win_n / 1000u) : 0;
                    sl->rend_max_us = r->win_max_us;
                    r->win_ns = r->win_n = 0, r->win_max_us = 0;
                    fw_c1_stats(&sl->fw, &c1);
                    if (c1.state) {
                        const uint64_t dj = c1.job_ns - sl->c1_job_ns, dw = c1.wait_ns - sl->c1_wait_ns;
                        sl->c1_job_ns = c1.job_ns, sl->c1_wait_ns = c1.wait_ns;
                        sl->c1_cpu = (uint32_t)(dj / 1000u / (dt ? dt : 1));
                        n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"c1\":%d,\"c1_jobs\":%llu,\"c1_inline\":%llu,"
                                      "\"c1_cpu\":%.1f,\"c1_wait_cpu\":%.1f,\"c1_worst_us\":%u", c1.state,
                                      (unsigned long long)c1.jobs, (unsigned long long)c1.inline_jobs, sl->c1_cpu / 10.0,
                                      (double)(dw / 1000u / (dt ? dt : 1)) / 10.0, c1.job_worst_us);
                    } else
                        sl->c1_cpu = 0;
                }
                if (fw_has_ain(&sl->fw))   /* ain1: the state told, its latency, the blocks fed, the last block's peak */
                    n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"ain\":%u,\"ain_lat\":%u,\"ain_blocks\":%u,"
                                  "\"ain_peak\":%d", sl->fw.ain_state, sl->fw.ain_lat, sl->fw.ain_blocks, sl->fw.ain_peak);
                if (sl->tk_known) {   /* tk1: the takeover's claims (0: none now), its name, what the core gave it */
                    mj_escape(sl->tk_name, esc[0], sizeof esc[0]);
                    n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"tk\":%u,\"tk_name\":\"%s\",\"tk_in\":%u,"
                                  "\"tk_states\":%u", sl->tk_claims, esc[0], sl->tk_in, sl->tk_states);
                }
                n += snprintf(tmp + n, sizeof tmp - (size_t)n, "}");
            } else
                n += snprintf(tmp + n, sizeof tmp - (size_t)n, "%snull", s ? "," : "");
        }
        {   /* threads: the blocks (render_block whole, the slots' fork-join), each thread's CPU */
            char tj[1024];
            tcpu_sample(dt);
            tcpu_json(tj, sizeof tj);
            g_blk_avg_us = g_blk_n ? (uint32_t)(g_blk_ns / g_blk_n / 1000u) : 0;
            g_blk_max_last = g_blk_max_us;
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, "],\"threads\":%d,\"block_avg_us\":%u,\"block_max_us\":%u,"
                          "\"block_cpu\":%.1f,\"par_cpu\":%.1f,\"tcpu\":%s", g_threads, g_blk_avg_us, g_blk_max_us,
                          (double)(g_blk_ns / 1000u / (dt ? dt : 1)) / 10.0, (double)(g_par_ns / 1000u / (dt ? dt : 1)) / 10.0, tj);
            g_blk_ns = g_blk_n = g_par_ns = 0, g_blk_max_us = 0;
        }
        n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"ain_live\":%d,\"ain_in\":%u,\"ain_under\":%u,"
                      "\"ain_dropped\":%u,\"ain_over\":%u,\"ain_latency\":%u,\"pmidi_msgs\":%u,\"pmidi_bytes\":%u,"
                      "\"pmidi_dropped\":%u,\"midi_out_bytes\":%u", g_ain_live, g_ain_in, g_ain_under,
                      g_ain_dropped, g_ain_over, engine_ain_latency(), g_pm_msgs, g_pm_bytes, g_pm_dropped, g_mo_bytes);
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
                          "\"level%d\":%d,\"pan%d\":%d,\"sha%d\":\"%.12s\"", s,
                          SL[s].on ? SL[s].fw.id : "", s, SL[s].on ? SL[s].fw.version : "", s,
                          SL[s].on ? SL[s].serial : 0, s, S->core.fw.st_used[s], s, SL[s].notes, s, SL[s].level, s, SL[s].pan,
                          s, SL[s].on && SL[s].fw.obj ? SL[s].fw.obj->vt->sha256 : "");   /* (sha: its .rfm1's) */
        /* (rfm1cs 0.13.0) a project's firmwares, the states: the project the slots' memories are (proj_open) and the
         * core's open one (proj_cur), its firmwares loading; per slot whose memory it is (pcproj), the active state as
         * the host autosaves it (stcur) and as the core shows it (cstcur), the open project's wanted firmware (want),
         * a New's undo kept (undo) */
        n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"proj_open\":%d,\"proj_cur\":%d,\"pfw_loading\":%d,"
                      "\"pfw_shown\":%u,\"pfw_ended\":%u,\"projects\":%d,\"states_open\":%d", g_proj_open,
                      rfm_move_project_slot(&S->core), rfm_move_proj_fw_loading(&S->core), g_pfw_shown, g_pfw_ended,
                      S->core.seq.projects ? 1 : 0, rfm_move_states_open(&S->core));
        for (s = 0; s < NSLOT; s++)
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"pcproj%d\":%d,\"stcur%d\":%d,\"cstcur%d\":%d,"
                          "\"want%d\":\"%s\",\"undo%d\":%d", s, SL[s].on ? SL[s].pc_proj : -1, s,
                          SL[s].on ? SL[s].st_cur : -1, s, S->core.fw.st_cur[s], s, S->core.pfw_want[s], s,
                          SL[s].undo != NULL);
        /* (README "The working memory") snapshots taken per slot, the last one's cost (us), whether the running
         * instance came from one (not booted from its flash), its module's are whole (snapable); the costliest
         * snapshot and its counterpart so far */
        for (s = 0; s < NSLOT; s++)
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"snaps%d\":%u,\"snapus%d\":%u,\"restored%d\":%u,"
                          "\"snapable%d\":%d", s, SL[s].snaps, s, SL[s].snap_us, s, SL[s].on ? SL[s].restored : 0, s,
                          SL[s].on ? fw_snap_able(&SL[s].fw) : 0);
        n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"snap_max_us\":%u,\"restore_max_us\":%u", g_snap_us_max,
                      g_restore_us_max);
        /* the catalog and its downloads */
        mj_escape(g_dl.err, esc[0], sizeof esc[0]);
        mj_escape(g_index_err, esc[1], sizeof esc[1]);
        {   /* (the entries offered as an update: installed, another version or another build of it) */
            int k, up = 0;
            for (k = 0; g_cat && k < g_cat->n; k++)
                up += g_cat->e[k].installed[0] && strcmp(g_cat->e[k].installed, g_cat->e[k].version) != 0;
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"cat_updates\":%d", up);
        }
        n += snprintf(tmp + n, sizeof tmp - (size_t)n,
                      ",\"cat_n\":%d,\"cat_installed\":%d,\"cat_from\":%d,\"cat_gen\":\"%s\",\"dl_id\":\"%s\","
                      "\"dl_state\":%d,\"dl_pct\":%d,\"dl_ok\":%u,\"dl_failed\":%u,\"dl_err\":\"%s\",\"index_ok\":%u,"
                      "\"index_failed\":%u,\"index_err\":\"%s\"",
                      g_cat ? g_cat->n : 0, g_cat ? g_cat->nfw : 0, g_cat ? g_cat->from : 0, g_cat ? g_cat->generated : "",
                      g_dl.id, g_dl.state, g_dl.pct, g_dl.ok, g_dl.failed, esc[0], g_dl.index_ok, g_dl.index_failed,
                      esc[1]);
        {   /* the screen reader (announce.h): on, utterances sent, the sidecar's answers, frames mixed */
            int on;
            uint32_t said, acked, mixed;
            ann_stats(&on, &said, &acked, &mixed);
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"tts_on\":%d,\"tts_said\":%u,\"tts_acked\":%u,"
                          "\"tts_mixed\":%u", on, said, acked, mixed);
#ifdef RFM_MOVE_HAS_SPEECH
            if (S)   /* (rfm1cs 0.6.12: the Speech mode, 0 Off 1 On 2 Jog; open: words may be said now) */
                n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"speech_mode\":%d,\"speech_open\":%d",
                              rfm_move_speech_mode(&S->core), rfm_move_speech_open(&S->core));
#endif
            if (n < (int)sizeof tmp)   /* (the screen reader protocol: the firmware that speaks for itself) */
                n += fmr_json(tmp + n, (int)sizeof tmp - n);
        }
        n += snprintf(tmp + n, sizeof tmp - (size_t)n, ",\"mroute\":[%d,%d,%d]", g_mroute[0], g_mroute[1],
                      g_mroute[2]);   /* (rfm1cs 0.6.6: each slot's MIDI routes, as the engine forwards them) */
        if (n < (int)sizeof tmp)   /* Out (midiout.h): the RTP-MIDI peers, what went, USB-A */
            n += midiout_json(tmp + n, (int)sizeof tmp - n);
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
        {   /* every 10 s: the render time split (does it fit?) */
            static uint32_t last_split;
            if (now - last_split >= 10000u) {
                char line[1400], tj[1024];
                int m = 0;
                last_split = now;
                for (s = 0; s < NSLOT; s++)
                    if (SL[s].on)
                        m += snprintf(line + m, sizeof line - (size_t)m, " slot%d %s %u/%u us%s%s", s + 1, SL[s].fw.id,
                                      SL[s].rend_avg_us, SL[s].rend_max_us, !SL[s].fw.obj || SL[s].fw.obj->mt ? ""
                                      : SL[s].fw.obj->lg ? " (old object)" : " (old object, locked)",
                                      SL[s].fw.c1 ? " +core1" : "");
                tcpu_json(tj, sizeof tj);
                fprintf(stderr, "engine: blocks %u/%u us (avg/max of 2902),%s; %s; cpu %s\n", g_blk_avg_us,
                        g_blk_max_last, line, g_threads ? "slots on their threads" : "slots on one thread", tj);
            }
        }
    }
}

static void *engine_main(void *arg)
{
    const uint32_t lead = (uint32_t)(C.lead_blocks < 4 ? 4 : C.lead_blocks > 8 ? 8 : C.lead_blocks) * FW_BLOCK;
    uint8_t buf[16384];
    (void)arg;
    thread_setup("rfm1-engine", -1, PRIO_ENGINE);
    wasm_rt_init();
    rthr_start();   /* (the slots' render threads: README "Threads") */
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
        {   /* the page's States and projects (engine_store) */
            struct { eng_store_t r; int id; } sq[8];
            int n, i;
            pthread_mutex_lock(&g_vmu);
            n = g_nsq;
            memcpy(sq, g_sq, sizeof sq[0] * (size_t)n);
            g_nsq = 0;
            pthread_mutex_unlock(&g_vmu);
            for (i = 0; i < n; i++)
                store_run(&sq[i].r, sq[i].id);
            store_watch(now_ms(), 0);
        }
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
            if (g_wdl[0] && g_ndefer < (int)(sizeof g_defer / sizeof g_defer[0])) {   /* the page's download: FW_DOWNLOAD */
                defer_t *d = &g_defer[g_ndefer++];
                memset(d, 0, sizeof *d);
                d->a.kind = RFM_MOVE_ACT_FW_DOWNLOAD;
                snprintf(d->id, sizeof d->id, "%s", g_wdl);
                g_wdl[0] = 0;
            }
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
            page_midi();
            if (C.test_api)
                test_pokes(), test_peeks();
            for (i = 0; i < n; i++) {
                if (pin[i].op >= ENG_OP_ACTIVE && pin[i].op <= ENG_OP_LAST)
                    page_edit(pin[i].op, pin[i].a, pin[i].b);
                else
                    engine_fm1_in(-1, pin[i].op, pin[i].a, pin[i].b);
            }
        }
        /* the screen reader protocol (fmreader.h): HELLO to the active firmware while speech is on */
        speech_sync();
        fmr_tick(now, g_active, SL[g_active].on && !SL[g_active].fw.dead ? SL[g_active].serial : 0,
                 ann_on() && g_session_on
#ifdef RFM_MOVE_HAS_SPEECH
                     && (rfm_move_speech_open(&S->core) || rfm_move_speech_mode(&S->core) == RFM_MOVE_SPEECH_JOG)
#endif
                 );
#ifdef RFM_MOVE_HAS_SPEECH
        if (S && rfm_move_fw_speech(&S->core) != (fmr_speaks() != 0))   /* (rfm1cs 0.6.11: its words for the firmware's
                                                                          * controls step back while it speaks itself) */
            rfm_move_set_fw_speech(&S->core, fmr_speaks());
#endif
        /* the FM-1 on USB-A (fm1link.c): its panel and MIDI in, the active slot's screen and LEDs out */
        fm1link_tick(now, g_active, SL[g_active].on ? &SL[g_active].fw : NULL);
        midiout_poll(now);   /* (Out: on this thread, as USB-A's other sender) */
        if (fm1link_changed()) {
            static int was_linked;
            const int linked = fm1link_linked();
            view_info();
            g_mix_ver++;
            if (linked != was_linked) {   /* (rfm1cs 0.5.4: the Move's own screen says so, as the page does) */
                was_linked = linked;
                rfm_move_popup(&S->core, linked ? "FM-1 linked" : "FM-1 lost");
#ifndef RFM_MOVE_HAS_SPEECH   /* (else rfm1cs says its popup) */
                ann_say(ANN_EVENT, "%s", linked ? "FM-1 linked" : "FM-1 lost");
#endif
                rfm1cs_session_redraw(S, now);
            }
        }
        rfm1cs_session_tick(S, now);
        do_deferred();
        {   /* (rfm1cs 0.13.0) a project's loading screen, counted as it comes and goes (the status) */
            static int was;
            const int l = rfm_move_proj_fw_loading(&S->core);
            if (l != was)
                (l ? g_pfw_shown++ : g_pfw_ended++), was = l;
        }
        while (mr_ring_count(&g_ring) < lead && g_run)
            render_block(now_ms());
        publish(now);
        do_deferred();
        speak_watch();
        ann_tick(now);
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
        rthr_stop();
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
    int open_slot = 0, s, pfw_start = 0;
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
#ifdef RFM14_TEST_BUILD
        if (getenv("RFM14_TEST_MEM_MS") && atoi(getenv("RFM14_TEST_MEM_MS")) > 0)   /* (tests/states_e2e.sh) */
            g_mem_autosave_ms = (uint32_t)atoi(getenv("RFM14_TEST_MEM_MS"));
#endif
        g_log_cap = C.log_cap > 0 ? C.log_cap : C.log_cap < 0 ? 0 : LOG_CAP_DEFAULT;
    }
    fw_on_c1_thread = c1_thread_init;   /* (core 1's threads: named, on cores 0..2, real time as the slots') */
    if (st_fw_install_bundled(C.bundled) > 0)
        fprintf(stderr, "engine: bundled firmwares installed into %s/firmwares\n", C.root);
    if (C.dev_bundled && st_fw_install_dev(C.dev_bundled) > 0)
        fprintf(stderr, "engine: development builds installed into %s/firmwares (from %s)\n", C.root, C.dev_bundled);
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
        lc.wifi_off = C.fm1_wifi_off, lc.usba_screen = C.fm1_usba_screen, lc.wifi_addr = C.fm1_addr, lc.root = C.root;
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
#ifdef RFM_MOVE_HAS_SPEECH
    /* (rfm1cs 0.6.12) the Speech mode while the settings have none: Schwung's screen reader setting (announce.h) */
    rfm_move_set_speech_default(&S->core, ann_user_on() ? RFM_MOVE_SPEECH_ON : RFM_MOVE_SPEECH_OFF);
#endif
    free(settings);
    rfm1cs_session_give_up(S, -1);
    st_path(p, sizeof p, "move/library.rmvl");
    lib = st_read(p, &nl);
    if (rfm_move_fw_library(&S->core, lib, (uint32_t)nl) < 0)
        fprintf(stderr, "engine: %s is not a library; starting empty\n", p);
    free(lib);
    /* (rfm1cs 0.13.0) a project holds its firmwares: before the projects (rfm_move_project_dir: a fresh install's first
     * project gets the new-project defaults) and before the list */
    rfm_move_fw_caps(&S->core, RFM_MOVE_CAPS_ALL | RFM_MOVE_CAP_PROJFW);
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
        SL[s].filt = 0xFFFF, SL[s].level = 127, SL[s].pan = 64, SL[s].pc_proj = SL[s].st_cur = -1;
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
    {
        const int nd = g_ndefer;
        int k;
        rfm1cs_session_projects_init(S, now_ms(), used, open_slot, blob, nb);
        /* (rfm1cs 0.13.0) a version 9 project asked for its firmwares (PROJECT_FW, deferred): the engine's loop loads
         * them as for any project (pfw_want: its copies, the loading screen); the library's slots are not restored
         * then. Older (or none): the library's, as before, and they are the open project's from now on */
        for (k = nd; k < g_ndefer; k++)
            pfw_start |= g_defer[k].a.kind == RFM_MOVE_ACT_PROJECT_FW;
    }
    free(blob);
    if (st_worker_start(used) != 0)
        return -1;
    g_active = rfm_move_fw_active(&S->core);
    g_proj_open = rfm_move_project_slot(&S->core);
    /* the firmwares the library had on its slots; none: the default on slot 1 */
    if (!pfw_start) {
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
        proj_adopt(g_proj_open);
    } else
        fprintf(stderr, "engine: project %d holds its firmwares: they load from its copies\n", g_proj_open + 1);
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
    fmr_start(C.root, fmr_send);   /* (its fetch thread: an ordinary one, made before the engine's) */
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
    fmr_stop();
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
