/* SPDX-License-Identifier: MPL-2.0 */
/* The FM-1 screen reader protocol, RemoteFM1 as its reader: fmreader.h has the design. */
#define _GNU_SOURCE
#include "fmreader.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "announce.h"
#include "fmwords.h"
#include "https.h"
#include "minijson.h"

#define HELLO_MS 2000u      /* the protocol's: a HELLO every 2 s */
#define ANSWER_MS 3000u     /* no answer this long after the first HELLO: no support (known) */
#define ASKS 3              /* HELLOs a firmware that never answered is sent */
#define MSG_MAX 320         /* a frame's bytes kept (SAY: 240 characters at most) */
#define WORDS_BYTES (2u << 20)
#define NWF 4
#define WHERE_MS 400u       /* the answer and no SAY this long after: WHERE (another reader was listening already) */
#define HOLD_MS 4000u       /* SAYs wait this long at most for the words file being read or fetched */
#define QN 24

/* a words file: free, being read or fetched (the fetch thread's), loaded or failed (the engine's) */
enum { WF_FREE, WF_PENDING, WF_LOADED, WF_FAILED };
typedef struct {
    char file[64], url[256];
    int state;                 /* (atomic: the hand-over between the threads) */
    fmw_t w;
} wf_t;

typedef struct {
    uint32_t serial;           /* the firmware's boot this is about */
    int sup;                   /* 1 it answered, -1 not within ANSWER_MS, 0 not known yet */
    int asks;                  /* HELLOs sent since its last answer */
    uint32_t first_ms;         /* .. the first one */
    char name[64], link[256];
    int wf;                    /* its words file (WF), -1 none */
    /* its MIDI out's SysEx: 0 none, 1 a start held (F0 7D 53: ours if 52 comes next), 2 ours, 3 another's */
    int st, n;
    uint32_t held;
    uint8_t buf[MSG_MAX];
} fs_t;

static fs_t F[FMR_SLOTS];
static wf_t WF[NWF];
static fmr_send_fn g_send;
static char g_root[512];
static int g_cur = -1;         /* the slot listened to */
static int g_heard;            /* .. it answered since it became the one */
static uint32_t g_where_ms;    /* .. then, with nothing said since (0: said, or WHERE sent) */
static int g_speech;
static uint32_t g_next, g_said;
static pthread_t g_thr;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_quit, g_started;
static volatile int g_cancel;
static uint32_t g_now;
static struct { int flags; char t[244]; } Q[QN];   /* SAYs waiting for the words file (the first screen's) */
static int g_qn;
static uint32_t g_q_ms;

/* ------------------------------------------------------------------ the words files (the fetch thread) --- */
static char *read_all(const char *path, size_t max)
{
    FILE *f = fopen(path, "rb");
    char *b;
    size_t n;
    if (!f)
        return NULL;
    if (!(b = malloc(max + 1))) {
        fclose(f);
        return NULL;
    }
    n = fread(b, 1, max + 1, f);
    fclose(f);
    if (n > max) {
        free(b);
        return NULL;
    }
    b[n] = 0;
    return b;
}

/* one words file: <root>/words/<file> when kept, else fetched there first */
static int words_get(wf_t *e)
{
    char path[1024], tmp[1100], err[256];
    char *json;
    int fetched = 0, rc;
    snprintf(path, sizeof path, "%s/words/%s", g_root, e->file);
    if (access(path, R_OK) != 0) {
        snprintf(tmp, sizeof tmp, "%s/words", g_root);
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "fm1-reader: %s: %s\n", tmp, strerror(errno));
            return -1;
        }
        snprintf(tmp, sizeof tmp, "%s.part", path);
        if (https_get(e->url, tmp, WORDS_BYTES, 0, NULL, NULL, &g_cancel, err, sizeof err) != 0) {
            fprintf(stderr, "fm1-reader: %s: %s\n", e->url, err);
            return -1;
        }
        fetched = 1;
        if (rename(tmp, path) != 0) {
            unlink(tmp);
            return -1;
        }
    }
    if (!(json = read_all(path, WORDS_BYTES)))
        return -1;
    rc = fmw_load(&e->w, json);
    free(json);
    if (rc != 0) {   /* (not a words file: not kept) */
        fprintf(stderr, "fm1-reader: %s: not a words file (format 1)\n", fetched ? e->url : path);
        unlink(path);
        return -1;
    }
    fprintf(stderr, "fm1-reader: words %s: %d entries (%s)\n", e->file, e->w.n, fetched ? "fetched, kept" : "kept");
    return 0;
}

static void *fetch_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mu);
    while (!g_quit) {
        wf_t *e = NULL;
        int i;
        for (i = 0; i < NWF && !e; i++)
            if (__atomic_load_n(&WF[i].state, __ATOMIC_ACQUIRE) == WF_PENDING)
                e = &WF[i];
        if (!e) {
            pthread_cond_wait(&g_cv, &g_mu);
            continue;
        }
        pthread_mutex_unlock(&g_mu);
        i = words_get(e);
        __atomic_store_n(&e->state, i == 0 ? WF_LOADED : WF_FAILED, __ATOMIC_RELEASE);
        pthread_mutex_lock(&g_mu);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

/* the engine's: the words file a link names (its entry; -1 a link that is no words file's) */
static int words_want(const char *url)
{
    const char *f = strrchr(url, '/');
    size_t n, k;
    int i, pick = -1;
    if (strncmp(url, "https://", 8) || !f || !f[1] || f[1] == '.' || (n = strlen(++f)) >= sizeof WF[0].file || n < 6 ||
        strcmp(f + n - 5, ".json") || strlen(url) >= sizeof WF[0].url)
        return -1;
    for (k = 0; k < n; k++)
        if (!((f[k] >= 'a' && f[k] <= 'z') || (f[k] >= 'A' && f[k] <= 'Z') || (f[k] >= '0' && f[k] <= '9') ||
              f[k] == '.' || f[k] == '-' || f[k] == '_'))
            return -1;
    for (i = 0; i < NWF; i++)   /* (the same file: the same words, whichever site it came from) */
        if (__atomic_load_n(&WF[i].state, __ATOMIC_ACQUIRE) != WF_FREE && !strcmp(WF[i].file, f))
            return i;
    for (i = 0; i < NWF && pick < 0; i++) {   /* a free entry, else one no slot uses (never one being fetched) */
        const int st = __atomic_load_n(&WF[i].state, __ATOMIC_ACQUIRE);
        int s, used = 0;
        for (s = 0; s < FMR_SLOTS; s++)
            used |= F[s].wf == i;
        if (st == WF_FREE || (st != WF_PENDING && !used))
            pick = i;
    }
    if (pick < 0 || !g_started)
        return -1;
    fmw_free(&WF[pick].w);
    snprintf(WF[pick].file, sizeof WF[pick].file, "%s", f);
    snprintf(WF[pick].url, sizeof WF[pick].url, "%s", url);
    pthread_mutex_lock(&g_mu);
    __atomic_store_n(&WF[pick].state, WF_PENDING, __ATOMIC_RELEASE);
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return pick;
}

/* ------------------------------------------------------------------ the protocol (the engine's thread) --- */
int fmr_start(const char *root, fmr_send_fn send)
{
    pthread_attr_t at;
    struct sched_param sp;
    int s;
    snprintf(g_root, sizeof g_root, "%s", root ? root : ".");
    g_send = send;
    for (s = 0; s < FMR_SLOTS; s++)
        F[s].wf = -1;
    memset(&sp, 0, sizeof sp);
    pthread_attr_init(&at);   /* (an ordinary thread, whatever the caller's: it waits on the network) */
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_OTHER);
    pthread_attr_setschedparam(&at, &sp);
    g_quit = 0;
    g_started = pthread_create(&g_thr, &at, fetch_main, NULL) == 0;
    pthread_attr_destroy(&at);
    return g_started ? 0 : -1;
}

void fmr_stop(void)
{
    int i;
    if (g_started) {
        pthread_mutex_lock(&g_mu);
        g_quit = 1;
        g_cancel = 1;
        pthread_cond_signal(&g_cv);
        pthread_mutex_unlock(&g_mu);
        pthread_join(g_thr, NULL);
        g_started = 0;
    }
    for (i = 0; i < NWF; i++) {
        fmw_free(&WF[i].w);
        WF[i].state = WF_FREE;
    }
}

static void slot_reset(fs_t *P, uint32_t serial)
{
    memset(P, 0, sizeof *P);
    P->serial = serial;
    P->wf = -1;
}

/* the SAYs waiting, in words, to the screen reader: once the words file of the slot listened to is no longer being read
 * or fetched (or has taken HOLD_MS); force: now */
static void say_queued(int force)
{
    const int wf = g_cur >= 0 ? F[g_cur].wf : -1;
    const int st = wf >= 0 ? __atomic_load_n(&WF[wf].state, __ATOMIC_ACQUIRE) : WF_FAILED;
    char o[1200];
    int i;
    if (!g_qn || (st == WF_PENDING && !force && g_now - g_q_ms < HOLD_MS))
        return;
    for (i = 0; i < g_qn; i++) {
        fmw_spoken(Q[i].t, st == WF_LOADED ? &WF[wf].w : NULL, o, sizeof o);
        ann_fw(Q[i].flags, o);
        g_said++;
    }
    g_qn = 0;
}

static void said(int s, const uint8_t *m, int n)
{
    static const uint8_t BYE[] = { 0xF0, 0x7D, 0x53, 0x52, 0x02, 0xF7 };
    fs_t *P = &F[s];
    char t[MSG_MAX];
    int i, k = 0;
    if (n < 6 || m[n - 1] != 0xF7)
        return;
    for (i = 6; i < n - 1 && k < (int)sizeof t - 1; i++)   /* (ASCII: anything else a space) */
        t[k++] = (char)(m[i] >= 0x20 && m[i] < 0x7F ? m[i] : m[i] ? ' ' : 0);
    t[k] = 0;
    if (m[4] == 1) {   /* HELLO's answer: version, name, 0, link (a loopback's HELLO has no name: not a device) */
        char ln[256];
        int j, b = 0;
        for (j = 6; j < n - 1 && m[j]; j++)
            ;
        for (j++; j < n - 1 && b < (int)sizeof ln - 1; j++)
            if (m[j] > 0x20 && m[j] < 0x7F)
                ln[b++] = (char)m[j];
        ln[b] = 0;
        if (n < 8 || !t[0])
            return;
        t[sizeof P->name - 1 < (size_t)k ? sizeof P->name - 1 : (size_t)k] = 0;
        if (P->sup != 1 || strcmp(P->name, t) || strcmp(P->link, ln))
            fprintf(stderr, "fm1-reader: slot %d: %s answers (words: %s)\n", s + 1, t, ln[0] ? ln : "none");
        if (strcmp(P->link, ln) || P->wf < 0)
            P->wf = ln[0] ? words_want(ln) : -1;
        snprintf(P->name, sizeof P->name, "%s", t);
        snprintf(P->link, sizeof P->link, "%s", ln);
        P->sup = 1;
        P->asks = 0;
        if (s == g_cur && !g_heard)
            g_heard = 1, g_where_ms = g_now ? g_now : 1;
        else if (g_send)   /* (a slot no longer listened to that still answered: let it stop now) */
            g_send(s, BYE, sizeof BYE);
        return;
    }
    if (m[4] != 16 || s != g_cur || !g_heard || !g_speech || !t[0])
        return;   /* (SAY only from the slot listened to, after its answer: what it queued before is stale) */
    g_where_ms = 0;
    if (m[5] & 1)
        g_qn = 0;   /* (NOW: what waited is cut off anyway) */
    if (g_qn < QN) {
        if (!g_qn)
            g_q_ms = g_now;
        Q[g_qn].flags = (m[5] & 1 ? ANN_FW_NOW : 0) | (m[5] & 2 ? ANN_FW_KEEP : 0);
        snprintf(Q[g_qn].t, sizeof Q[g_qn].t, "%s", t);
        g_qn++;
    }
    say_queued(0);
}

void fmr_tick(uint32_t now, int active, uint32_t serial, int speech)
{
    static const uint8_t HELLO[] = { 0xF0, 0x7D, 0x53, 0x52, 0x01, 0x01, 0xF7 };
    static const uint8_t BYE[] = { 0xF0, 0x7D, 0x53, 0x52, 0x02, 0xF7 };
    const int want = speech && active >= 0 && active < FMR_SLOTS && serial;
    fs_t *P;
    g_speech = speech;
    g_now = now;
    if (g_cur >= 0 && (g_cur != active || !want)) {   /* not listened to any more */
        if (g_send && F[g_cur].sup == 1 && g_send(g_cur, BYE, sizeof BYE) == 0)
            fprintf(stderr, "fm1-reader: slot %d: BYE\n", g_cur + 1);
        g_cur = -1;
        g_qn = 0;
    }
    if (!want || !g_send)
        return;
    P = &F[active];
    if (P->serial != serial) {   /* another firmware (or the same booted again): nothing known */
        slot_reset(P, serial);
        if (g_cur == active)
            g_cur = -1;
    }
    if (g_cur != active) {
        g_cur = active;
        g_heard = 0, g_where_ms = 0;
        g_qn = 0;
        g_next = now;
    }
    if ((int32_t)(now - g_next) >= 0) {
        g_next = now + HELLO_MS;
        if ((P->sup == 1 || P->asks < ASKS) && g_send(active, HELLO, sizeof HELLO) == 0 && !P->asks++)
            P->first_ms = now ? now : 1;
    }
    if (g_heard && g_where_ms && now - g_where_ms >= WHERE_MS) {   /* (the HELLO was no news to it: the screen asked) */
        static const uint8_t WHERE[] = { 0xF0, 0x7D, 0x53, 0x52, 0x03, 0xF7 };
        g_where_ms = 0;
        g_send(active, WHERE, sizeof WHERE);
    }
    if (!P->sup && P->first_ms && now - P->first_ms >= ANSWER_MS) {
        P->sup = -1;
        fprintf(stderr, "fm1-reader: slot %d: no answer (no screen reader protocol)\n", active + 1);
    }
    say_queued(0);
}

int fmr_filter(int s, const uint32_t *pk, int n, uint32_t *out)
{
    static const int8_t LEN[8] = { 0, 0, 0, 0, 3, 1, 2, 3 };
    fs_t *P = &F[s < 0 || s >= FMR_SLOTS ? 0 : s];
    int i, k = 0, j;
    for (i = 0; i < n; i++) {
        const uint32_t p = pk[i];
        const int cin = (int)(p & 0xF);
        const uint8_t b0 = (uint8_t)(p >> 8), b1 = (uint8_t)(p >> 16), b2 = (uint8_t)(p >> 24);
        if (cin < 4 || cin > 7) {   /* (not SysEx: a real-time byte may come in the middle of one) */
            out[k++] = p;
            continue;
        }
        if (cin == 4 && b0 == 0xF0) {   /* a start: what came before is over */
            if (P->st == 1)
                out[k++] = P->held;
            P->st = 0;
        }
        switch (P->st) {
        case 0:
            if (cin == 4 && b0 == 0xF0 && b1 == 0x7D && b2 == 0x53) {
                P->held = p, P->st = 1, P->n = 0;
                for (j = 0; j < 3; j++)
                    P->buf[P->n++] = (uint8_t)(p >> (8 * (j + 1)));
            } else {
                out[k++] = p;
                if (cin == 4 && b0 == 0xF0)
                    P->st = 3;
            }
            break;
        case 1:
            if (b0 != 0x52) {   /* another's (7D is anyone's): as it came */
                out[k++] = P->held;
                out[k++] = p;
                P->st = cin == 4 ? 3 : 0;
                break;
            }
            P->st = 2;
            /* fall through */
        case 2:
            for (j = 0; j < LEN[cin]; j++)
                if (P->n < MSG_MAX)
                    P->buf[P->n++] = (uint8_t)(p >> (8 * (j + 1)));
            if (cin != 4) {
                P->st = 0;
                if (P->n < MSG_MAX)
                    said(s, P->buf, P->n);
            }
            break;
        default:
            out[k++] = p;
            if (cin != 4)
                P->st = 0;
            break;
        }
    }
    return k;
}

int fmr_speaks(void)
{
    return g_speech && g_cur >= 0 && F[g_cur].sup == 1;
}

int fmr_json(char *out, int cap)
{
    const fs_t *P = g_cur >= 0 ? &F[g_cur] : NULL;
    const int st = P && P->wf >= 0 ? __atomic_load_n(&WF[P->wf].state, __ATOMIC_ACQUIRE) : WF_FREE;
    char nm[140], ln[520];
    int n;
    mj_escape(P && P->sup == 1 ? P->name : "", nm, sizeof nm);
    mj_escape(P && P->sup == 1 ? P->link : "", ln, sizeof ln);
    n = snprintf(out, (size_t)cap, ",\"fmr_slot\":%d,\"fmr_name\":\"%s\",\"fmr_link\":\"%s\",\"fmr_words\":\"%s\","
                 "\"fmr_said\":%u", P && P->sup == 1 ? g_cur : -1, nm, ln,
                 !P || P->sup != 1 ? "none" : st == WF_LOADED ? "loaded" : st == WF_FAILED || (P->link[0] && P->wf < 0)
                 ? "failed" : st == WF_PENDING ? "fetching" : "none", g_said);
    return n < cap ? n : cap - 1;
}
