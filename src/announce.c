/* SPDX-License-Identifier: MPL-2.0 */
/* RemoteFM1's words for Schwung's screen reader: announce.h has the design. The shared memory's layout is Schwung's
 * src/host/tts_shm.h (MIT, Copyright (c) 2025-2026 Charles Vestal), version 1, restated here field for field; the
 * request and ring handling follow its shim side, src/host/tts_client.c. */
#define _GNU_SOURCE
#include "announce.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* ---- Schwung's tts_shm.h, version 1 */
#define TTS_SHM_MAGIC 0x31535454u   /* "TTS1" */
#define TTS_SHM_VERSION 1
#define TTS_SHM_TEXT_LEN 8192
#define TTS_SHM_RING_SAMPLES 32768u
typedef struct {
    int32_t enabled;
    int32_t volume;   /* 0..100 */
    int32_t engine;   /* 0 eSpeak, 1 Flite */
    float speed;
    float pitch;      /* Hz */
} tts_settings_t;
typedef struct {
    uint32_t magic;
    uint32_t version;
    /* the client -> the sidecar (seqlock: req_seq odd while being written) */
    volatile uint32_t req_seq;
    uint32_t speak_count;
    tts_settings_t want;
    char text[TTS_SHM_TEXT_LEN];
    /* the sidecar -> the client */
    volatile uint32_t sidecar_pid;
    volatile uint32_t state_seq;
    tts_settings_t state;
    volatile uint32_t discard_until;
    volatile uint32_t speak_ack;
    /* the audio ring (SPSC, monotonic sample counters) */
    volatile uint32_t audio_write;
    volatile uint32_t audio_read;
    int16_t ring[TTS_SHM_RING_SAMPLES];
} tts_shm_t;
_Static_assert(sizeof(tts_shm_t) == 73808, "Schwung's tts_shm_t (version 1) is 73808 bytes");

#define DEF_SIDECAR "/data/UserData/schwung/schwung-tts"
#define DEF_SHM "/schwung-tts"
#define DEF_CONFIG "/data/UserData/schwung/config"
#define READY_MS 10000u        /* the sidecar's settings: this long at most */
#define MAX_WAIT_MS 1500u      /* words wait at most this long for a quiet moment */
#define EV_MAX 600
#define VAL_MAX 300

static tts_shm_t *g_shm;
static int g_on;               /* configured and the sidecar running (or starting) */
static int g_ready;            /* the sidecar published its settings */
static pid_t g_pid = -1;
static uint32_t g_seen_state, g_start_ms, g_check_ms;
static tts_settings_t g_want;
static tts_settings_t g_user;  /* the user's settings from shadow_config.json (Schwung 1.9+), when it has them */
static int g_user_set;
static uint32_t g_count;                       /* speak_count (the engine's) */
static volatile uint32_t g_speak;              /* .. as the audio thread sees it */
static volatile int g_mix_on;                  /* the audio thread may read the ring */
static uint32_t g_debounce = 300;
static char g_ev[EV_MAX], g_val[VAL_MAX];      /* pending: the events (joined), the value (the latest) */
static uint32_t g_first_ms, g_last_ms;
static uint32_t g_said;
static volatile uint32_t g_mixed;
/* (ann_fw) a firmware's words pending: cut (NOW in the burst), keep (KEEP in it), when the burst began and grew last */
#define FW_MAX 1600
#define QUEUE_MAX_MS 8000u     /* queued words wait at most this long for the speech playing */
#define KEEP_MAX_MS 15000u     /* a KEEP holds the rest at most this long */
static char g_fw[FW_MAX];
static int g_fw_cut, g_fw_keep;
static uint32_t g_fw_first, g_fw_ms;
static uint32_t g_keep_ms;     /* a KEEP utterance sent then (0 none): heard whole */
static uint32_t g_pub_ms, g_mix_seen, g_mix_ms;   /* the last utterance sent; the audio last seen moving */

static uint32_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}

static int read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    size_t n;
    if (!f)
        return -1;
    n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

/* a key's value in a flat JSON file: its text after the colon (NULL: not there) */
static const char *jkey(const char *buf, const char *key)
{
    char k[64];
    const char *p;
    snprintf(k, sizeof k, "\"%s\"", key);
    if (!(p = strstr(buf, k)) || !(p = strchr(p + strlen(k), ':')))
        return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* Schwung 1.9's Global Settings > Screen Reader: shadow_config.json beside config/ (the older
 * config/screen_reader_state.txt is read when it does not say). 1 on, 0 off, -1 not said */
static int user_setting(const char *dir, uint32_t *debounce)
{
    char path[512], buf[8192];
    const char *v;
    snprintf(path, sizeof path, "%s/../shadow_config.json", dir);
    if (read_file(path, buf, sizeof buf) <= 0 || !(v = jkey(buf, "screen_reader_enabled")))
        return -1;
    if (strncmp(v, "true", 4) && *v != '1')
        return 0;
    memset(&g_user, 0, sizeof g_user);
    g_user.enabled = 1;
    g_user.engine = (v = jkey(buf, "screen_reader_engine")) && !strncmp(v, "\"flite\"", 7) ? 1 : 0;
    g_user.volume = (v = jkey(buf, "screen_reader_volume")) ? atoi(v) : 70;
    g_user.speed = (v = jkey(buf, "screen_reader_speed")) ? (float)atof(v) : 1.0f;
    g_user.pitch = (v = jkey(buf, "screen_reader_pitch")) ? (float)atof(v) : 110.0f;
    if (g_user.volume < 0 || g_user.volume > 100)
        g_user.volume = 70;
    if ((v = jkey(buf, "tts_debounce_ms"))) {
        const int d = atoi(v);
        *debounce = (uint32_t)(d < 0 ? 0 : d > 1000 ? 1000 : d);
    }
    g_user_set = 1;
    return 1;
}

int ann_start(const ann_cfg_t *cfg)
{
    const char *side = cfg && cfg->sidecar ? cfg->sidecar : DEF_SIDECAR;
    const char *name = cfg && cfg->shm ? cfg->shm : DEF_SHM;
    const char *dir = cfg && cfg->config_dir ? cfg->config_dir : DEF_CONFIG;
    const int mode = cfg ? cfg->mode : 0;
    char path[512], buf[2048];
    struct stat st;
    int fd;
    if (mode < 0)
        return 0;
    if (mode == 0) {   /* the user's setting: Schwung's Global Settings > Screen Reader */
        const int u = user_setting(dir, &g_debounce);
        snprintf(path, sizeof path, "%s/screen_reader_state.txt", dir);
        if (u == 0 || (u < 0 && (read_file(path, buf, sizeof buf) < 1 || buf[0] != '1'))) {
            fprintf(stderr, "screen reader: off (Schwung's setting)\n");
            return 0;
        }
    }
    snprintf(path, sizeof path, "%s/tts.json", dir);
    if (!g_user_set && read_file(path, buf, sizeof buf) > 0) {   /* the shim's debounce, the user's "Speak Delay" */
        const char *p = strstr(buf, "\"debounce_ms\"");
        if (p && (p = strchr(p, ':'))) {
            const int v = atoi(p + 1);
            g_debounce = (uint32_t)(v < 0 ? 0 : v > 1000 ? 1000 : v);
        }
    }
    if (access(side, X_OK) != 0) {
        fprintf(stderr, "screen reader: no %s (Schwung's speech): off\n", side);
        return 0;
    }
    fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        fprintf(stderr, "screen reader: %s: %s: off\n", name, strerror(errno));
        return 0;
    }
    (void)fchmod(fd, 0666);   /* (as Schwung makes its segments: either uid opens it next time) */
    if (fstat(fd, &st) != 0 || ((size_t)st.st_size < sizeof(tts_shm_t) && ftruncate(fd, sizeof(tts_shm_t)) != 0)) {
        fprintf(stderr, "screen reader: %s: %s: off\n", name, strerror(errno));
        close(fd);
        return 0;
    }
    g_shm = mmap(NULL, sizeof(tts_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (g_shm == MAP_FAILED) {
        g_shm = NULL;
        fprintf(stderr, "screen reader: %s: mmap: %s: off\n", name, strerror(errno));
        return 0;
    }
    g_shm->version = TTS_SHM_VERSION;
    __atomic_store_n(&g_shm->magic, TTS_SHM_MAGIC, __ATOMIC_RELEASE);
    g_seen_state = __atomic_load_n(&g_shm->state_seq, __ATOMIC_ACQUIRE);
    {   /* the sidecar, our child: a session of its own, the default signals, our stderr (the log) */
        posix_spawnattr_t at;
        posix_spawn_file_actions_t fa;
        sigset_t none, def;
        char *argv[] = { (char *)"schwung-tts", NULL };
        int rc;
        sigemptyset(&none);
        sigemptyset(&def);
        sigaddset(&def, SIGPIPE);
        sigaddset(&def, SIGTERM);
        sigaddset(&def, SIGINT);
        posix_spawnattr_init(&at);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
        posix_spawnattr_setsigmask(&at, &none);
        posix_spawnattr_setsigdefault(&at, &def);
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        rc = posix_spawn(&g_pid, side, &fa, &at, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&at);
        if (rc != 0) {
            fprintf(stderr, "screen reader: %s: %s: off\n", side, strerror(rc));
            munmap(g_shm, sizeof(tts_shm_t));
            g_shm = NULL;
            g_pid = -1;
            return 0;
        }
    }
    g_on = 1;
    g_start_ms = g_check_ms = mono_ms();
    fprintf(stderr, "screen reader: on: %s (pid %d), %s, debounce %u ms\n", side, (int)g_pid, name, g_debounce);
    return 1;
}

static void off(const char *why)
{
    fprintf(stderr, "screen reader: off: %s\n", why);
    g_on = 0;
    __atomic_store_n(&g_mix_on, 0, __ATOMIC_RELEASE);
}

/* the request block, as the shim writes it (one writer: the engine's thread) */
static void publish(const char *text)
{
    const uint32_t seq = g_shm->req_seq;
    size_t n = strlen(text);   /* (ours: at most EV_MAX + VAL_MAX) */
    __atomic_store_n(&g_shm->req_seq, seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (n > TTS_SHM_TEXT_LEN - 1)
        n = TTS_SHM_TEXT_LEN - 1;
    g_shm->want = g_want;
    memcpy(g_shm->text, text, n);
    g_shm->text[n] = 0;
    g_shm->speak_count = ++g_count;
    __atomic_store_n(&g_shm->req_seq, seq + 2, __ATOMIC_RELEASE);
    __atomic_store_n(&g_speak, g_count, __ATOMIC_RELEASE);
    g_said++;
    g_pub_ms = mono_ms();
    g_keep_ms = 0;
}

/* the speech still playing: sent and its audio not come yet (the sidecar's start), or its audio moving */
static int playing(uint32_t now)
{
    const uint32_t mx = g_mixed;
    if (mx != g_mix_seen)
        g_mix_seen = mx, g_mix_ms = now;
    return (g_pub_ms && now - g_pub_ms < 300u) || (g_mix_ms && now - g_mix_ms < 120u);
}

/* a KEEP utterance still playing: nothing cuts it */
static int kept(uint32_t now)
{
    if (g_keep_ms && (!playing(now) || now - g_keep_ms >= KEEP_MAX_MS))
        g_keep_ms = 0;
    return g_keep_ms != 0;
}

static void speak_pending(void)
{
    char t[EV_MAX + VAL_MAX + 4];
    snprintf(t, sizeof t, "%s%s%s", g_ev, g_ev[0] && g_val[0] ? ". " : "", g_val);
    g_ev[0] = g_val[0] = 0;
    if (!t[0])
        return;
    fprintf(stderr, "screen reader: \"%s\"\n", t);
    publish(t);
}

int ann_on(void)
{
    return g_on;
}

static void add(int kind, const char *t)
{
    const uint32_t now = mono_ms();
    if (!t[0])
        return;
    if (!g_ev[0] && !g_val[0])
        g_first_ms = now;
    g_last_ms = now;
    if (kind == ANN_VALUE) {
        snprintf(g_val, sizeof g_val, "%s", t);
        return;
    }
    g_val[0] = 0;   /* (a value before an event is stale: the event changed what it was about) */
    {
        const size_t e = strlen(g_ev), n = strlen(t);
        if (e >= n && !strcmp(g_ev + e - n, t) && (e == n || g_ev[e - n - 1] == ' '))
            return;   /* (the same event again) */
        if (e && e + 2 + n < sizeof g_ev)
            memcpy(g_ev + e, ". ", 3);
        else if (e)   /* (too many: the newest win) */
            g_ev[0] = 0;
        strncat(g_ev, t, sizeof g_ev - strlen(g_ev) - 1);
    }
}

void ann_say(int kind, const char *fmt, ...)
{
    char t[VAL_MAX];
    va_list ap;
    if (!g_on)
        return;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    add(kind, t);
}

void ann_now(const char *text)
{
    if (!g_on)
        return;
    add(ANN_EVENT, text);
    if (g_ready)
        speak_pending();
}

void ann_tick(uint32_t now_unused)
{
    uint32_t now;
    (void)now_unused;
    if (!g_on)
        return;
    now = mono_ms();
    if (now - g_check_ms >= 500u) {   /* the sidecar still there */
        int status;
        g_check_ms = now;
        if (g_pid > 0 && waitpid(g_pid, &status, WNOHANG) == g_pid) {
            g_pid = -1;
            off("the sidecar exited");
            return;
        }
    }
    if (!g_ready) {
        const uint32_t s = __atomic_load_n(&g_shm->state_seq, __ATOMIC_ACQUIRE);
        if (s == g_seen_state) {
            if (now - g_start_ms > READY_MS)
                off("the sidecar never published its settings");
            return;
        }
        g_seen_state = s;
        g_want = g_shm->state;   /* (restated as they are: nothing of the user's is changed or saved) */
        if (g_user_set) {        /* Schwung 1.9: the user's own settings, as the shim would pass them */
            g_want.enabled = 1;
            g_want.engine = g_user.engine, g_want.volume = g_user.volume;
            g_want.speed = g_user.speed, g_want.pitch = g_user.pitch;
        }
        g_count = g_shm->speak_count;
        __atomic_store_n(&g_speak, g_count, __ATOMIC_RELEASE);
        g_ready = 1;
        if (!g_want.enabled) {
            off("the sidecar has the screen reader off");
            return;
        }
        fprintf(stderr, "screen reader: ready: %s, volume %d, speed %.2f, pitch %.0f\n",
                g_want.engine == 1 ? "Flite" : "eSpeak", g_want.volume, g_want.speed, g_want.pitch);
        __atomic_store_n(&g_mix_on, 1, __ATOMIC_RELEASE);
    }
    if (g_fw[0] && now - g_fw_ms >= ANN_FW_GAP_MS && !kept(now) &&
        (g_fw_cut || !playing(now) || now - g_fw_first >= QUEUE_MAX_MS)) {   /* a firmware's own words */
        char t[EV_MAX + VAL_MAX + FW_MAX + 8];
        const int keep = g_fw_keep;
        snprintf(t, sizeof t, "%s%s%s%s%s", g_ev, g_ev[0] && g_val[0] ? ". " : "", g_val, g_ev[0] || g_val[0] ? ". " : "",
                 g_fw);
        g_ev[0] = g_val[0] = g_fw[0] = 0;
        g_fw_cut = g_fw_keep = 0;
        fprintf(stderr, "screen reader: \"%s\"%s\n", t, keep ? " (kept whole)" : "");
        publish(t);
        if (keep)
            g_keep_ms = now ? now : 1;
        return;
    }
    if ((g_ev[0] || g_val[0]) && (now - g_last_ms >= g_debounce || now - g_first_ms >= MAX_WAIT_MS) && !kept(now))
        speak_pending();
}

void ann_fw(int flags, const char *text)
{
    const uint32_t now = mono_ms();
    if (!g_on || !text || !text[0])
        return;
    if (flags & ANN_FW_NOW || !g_fw[0]) {   /* (NOW: what of the firmware's was pending is cut off too) */
        snprintf(g_fw, sizeof g_fw, "%s", text);
        g_fw_cut = flags & ANN_FW_NOW ? 1 : 0;
        g_fw_keep = 0;
        g_fw_first = now;
    } else {
        const size_t e = strlen(g_fw), n = strlen(text);
        if (e + 2 + n < sizeof g_fw) {
            memcpy(g_fw + e, ". ", 3);
            memcpy(g_fw + e + 2, text, n + 1);
        }
    }
    g_fw_keep |= flags & ANN_FW_KEEP ? 1 : 0;
    g_fw_ms = now;
}

/* the frames the ring holds for the utterance asked last (the shim's ring_avail: what the sidecar pushed before it
 * acted on the newest request is the previous utterance's, dropped) */
static uint32_t ring_avail(uint32_t *rd)
{
    static uint32_t seen_ack;
    uint32_t r = g_shm->audio_read, avail;
    const uint32_t ack = __atomic_load_n(&g_shm->speak_ack, __ATOMIC_ACQUIRE);
    const uint32_t w = __atomic_load_n(&g_shm->audio_write, __ATOMIC_ACQUIRE);
    const uint32_t want = __atomic_load_n(&g_speak, __ATOMIC_ACQUIRE);
    if (ack != want) {
        r = w;
        avail = 0;
    } else {
        if (ack != seen_ack) {
            const uint32_t du = g_shm->discard_until;
            if ((int32_t)(du - r) > 0)
                r = du;
            seen_ack = ack;
        }
        avail = w - r;
        if (avail > TTS_SHM_RING_SAMPLES) {   /* (the sidecar restarted under us) */
            r = w;
            avail = 0;
        }
    }
    if (r != g_shm->audio_read)
        __atomic_store_n(&g_shm->audio_read, r, __ATOMIC_RELEASE);
    *rd = r;
    return avail / 2;
}

int ann_mix(int16_t *out, int frames, int have)
{
    uint32_t r, n, i;
    if (!__atomic_load_n(&g_mix_on, __ATOMIC_ACQUIRE) || frames <= 0)
        return have;
    n = ring_avail(&r);
    if (!n)
        return have;
    if (n > (uint32_t)frames)
        n = (uint32_t)frames;
    if (have < frames)   /* (the engine gave less: silence under the words) */
        memset(out + 2 * (have < 0 ? 0 : have), 0, (size_t)(frames - (have < 0 ? 0 : have)) * 4u);
    for (i = 0; i < 2 * n; i++) {
        const int32_t v = out[i] + g_shm->ring[(r + i) & (TTS_SHM_RING_SAMPLES - 1)];
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    __atomic_store_n(&g_shm->audio_read, r + 2 * n, __ATOMIC_RELEASE);
    g_mixed += n;
    return frames;
}

void ann_stop(void)
{
    if (g_on && g_ready && (g_ev[0] || g_val[0]))
        speak_pending();
    __atomic_store_n(&g_mix_on, 0, __ATOMIC_RELEASE);
    if (g_pid > 0) {
        int status, k;
        /* the last request delivered (the sidecar acts on it within ~5 ms) */
        for (k = 0; g_shm && k < 40 && __atomic_load_n(&g_shm->speak_ack, __ATOMIC_ACQUIRE) != g_count; k++)
            usleep(10000);
        kill(g_pid, SIGTERM);
        for (k = 0; k < 50 && waitpid(g_pid, &status, WNOHANG) != g_pid; k++)
            usleep(10000);
        if (k == 50) {
            kill(g_pid, SIGKILL);
            waitpid(g_pid, &status, 0);
        }
        g_pid = -1;
    }
    if (g_shm)
        munmap(g_shm, sizeof(tts_shm_t));   /* (the segment stays: it is the shim's when Move is back) */
    g_shm = NULL;
    g_on = g_ready = 0;
}

void ann_stats(int *on, uint32_t *said, uint32_t *acked, uint32_t *mixed)
{
    *on = g_on && g_ready;
    *said = g_said;
    *acked = g_shm ? __atomic_load_n(&g_shm->speak_ack, __ATOMIC_ACQUIRE) : 0;
    *mixed = g_mixed;
}
