/* SPDX-License-Identifier: MPL-2.0 */
/* Schwung FX hosting: see fx.h. */
#define _GNU_SOURCE
#include "fx.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"
#include "host/midi_fx_api_v1.h"
#include "minijson.h"

extern char **environ;

/* ------------------------------------------------------------------ the host API the modules get --- */
static volatile float g_bpm = 120.0f;
static volatile int g_running;
static volatile double g_beat = -1.0;
static void h_log(const char *msg) { fprintf(stderr, "fx: %s\n", msg ? msg : ""); }
static float h_bpm(void) { return g_bpm; }
static int h_clock(void) { return g_running ? MOVE_CLOCK_STATUS_RUNNING : MOVE_CLOCK_STATUS_STOPPED; }
static double h_beat(void) { return g_running ? g_beat : -1.0; }
/* one per process, zeroed by construction (static), with a tail of NULLs past it (Schwung's "reserved tail": a module
 * whose header is newer reads NULL there) */
static struct {
    host_api_v1_t api;
    void *tail[16];
} g_host;
static void host_init(void)
{
    if (g_host.api.api_version)
        return;
    g_host.api.api_version = 1;
    g_host.api.sample_rate = 44100;
    g_host.api.frames_per_block = 128;
    g_host.api.log = h_log;
    g_host.api.get_clock_status = h_clock;
    g_host.api.get_bpm = h_bpm;
    g_host.api.get_beat_position = h_beat;
}
void fx_clock(float bpm, int running, double beat) { g_bpm = bpm > 0 ? bpm : 120.0f, g_running = running, g_beat = beat; }

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ module.json --- */
static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *b;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 1 << 20 || !(b = malloc((size_t)n + 1))) {
        fclose(f);
        return NULL;
    }
    n = (long)fread(b, 1, (size_t)n, f);
    b[n] = 0;
    fclose(f);
    return b;
}
static int num(const char *obj, const char *key, double *out)
{
    const char *v = mj_find(obj, key);
    char *e;
    double d;
    if (!v)
        return -1;
    if (*v == 't' || *v == 'f') {
        *out = *v == 't';
        return 0;
    }
    d = strtod(v, &e);
    if (e == v)
        return -1;
    *out = d;
    return 0;
}
/* a JSON string element (an option) into out, plain: escapes dropped */
static void str_elem(const char *v, char *out, size_t cap)
{
    size_t k = 0;
    if (*v != '"') {   /* (a number as an option) */
        while (*v && *v != ',' && *v != ']' && *v != ' ' && k + 1 < cap)
            out[k++] = *v++;
        out[k] = 0;
        return;
    }
    for (v++; *v && *v != '"' && k + 1 < cap; v++) {
        if (*v == '\\' && v[1])
            v++;
        out[k++] = *v;
    }
    out[k] = 0;
}
static void cpy(char *d, size_t cap, const char *s) { const size_t n = strnlen(s, cap - 1); memmove(d, s, n); d[n] = 0; }
static int by_name(const void *a, const void *b) { return strcasecmp(((const fx_mod_t *)a)->name, ((const fx_mod_t *)b)->name); }
static void param_parse(fx_param_t *p, const char *o)
{
    char t[16] = "", s[64];
    double d;
    memset(p, 0, sizeof *p);
    mj_string(o, "key", p->key, sizeof p->key);
    if (mj_string(o, "name", p->name, sizeof p->name) || !p->name[0])
        cpy(p->name, sizeof p->name, p->key);
    mj_string(o, "type", t, sizeof t);
    p->type = !strcmp(t, "enum") ? FXP_ENUM : !strcmp(t, "int") ? FXP_INT : FXP_FLOAT;
    p->min = 0, p->max = p->type == FXP_FLOAT ? 1.0 : 127.0;
    num(o, "min", &p->min);
    num(o, "max", &p->max);
    p->def = p->min;
    if (p->type == FXP_ENUM) {
        const char *it = mj_find(o, "options"), *e;
        int n = 0;
        p->opt_str = num(o, "options_as_string", &d) == 0 && d != 0;
        if (it && *it == '[') {
            const char *it2 = it;
            while (mj_elem(&it2))
                n++;
        }
        p->opt = calloc((size_t)(n ? n : 1), sizeof *p->opt);
        for (it = mj_find(o, "options"); it && (e = mj_elem(&it)) && p->nopt < n;) {
            str_elem(e, s, sizeof s);
            p->opt[p->nopt++] = strdup(s);
        }
        p->min = 0, p->max = p->nopt ? p->nopt - 1 : 0;
        {   /* the default: an index, or an option's text */
            const char *dv = mj_find(o, "default");
            p->def = 0;
            if (dv && *dv == '"') {
                int i;
                str_elem(dv, s, sizeof s);
                for (i = 0; i < p->nopt; i++)
                    if (!strcmp(p->opt[i], s))
                        p->def = i;
            } else if (dv)
                p->def = strtod(dv, NULL);
        }
    } else
        num(o, "default", &p->def);
    if (p->max < p->min)
        p->max = p->min;
}
/* a parameter's value in the core's 0..127 (an enum: its index) and back */
static int to127(const fx_param_t *p, double v)
{
    if (p->type == FXP_ENUM)
        return (int)(v < 0 ? 0 : v > p->max ? p->max : v);
    if (p->max <= p->min)
        return 0;
    v = (v - p->min) / (p->max - p->min) * 127.0;
    return (int)lrint(v < 0 ? 0 : v > 127 ? 127 : v);
}
static int is_wet(const char *k) { return strstr(k, "wet") || !strcmp(k, "mix"); }
static int is_dry(const char *k) { return strstr(k, "dry") != NULL; }

static void scan_kind(fx_lib_t *L, const char *root, int kind)
{
    char d[600];
    DIR *dir;
    struct dirent *de;
    snprintf(d, sizeof d, "%s/%s", root, kind ? "audio_fx" : "midi_fx");
    if (!(dir = opendir(d)))
        return;
    while ((de = readdir(dir)) && L->n[kind] < FX_MAX_MODS) {
        fx_mod_t *m = &L->mod[kind][L->n[kind]];
        char p[1200], dsp[128] = "";
        char *json;
        const char *caps, *params, *e;
        struct stat st;
        if (de->d_name[0] == '.')
            continue;
        memset(m, 0, sizeof *m);
        m->kind = kind;
        snprintf(m->dir, sizeof m->dir, "%s/%s", d, de->d_name);
        snprintf(p, sizeof p, "%s/module.json", m->dir);
        if (!(json = read_all(p)))
            continue;
        if (mj_string(json, "id", m->id, sizeof m->id) || !m->id[0])
            snprintf(m->id, sizeof m->id, "%s", de->d_name);
        if (mj_string(json, "name", m->name, sizeof m->name) || !m->name[0])
            cpy(m->name, sizeof m->name, m->id);
        mj_string(json, "abbrev", m->abbrev, sizeof m->abbrev);
        mj_string(json, "version", m->version, sizeof m->version);
        {
            double b = 0;
            m->builtin = (num(json, "builtin", &b) == 0 && b != 0) || !strcmp(m->id, "arp") || !strcmp(m->id, "chord") ||
                         !strcmp(m->id, "freeverb");
        }
        mj_string(json, "dsp", dsp, sizeof dsp);
        if (!dsp[0] || strchr(dsp, '/'))
            snprintf(dsp, sizeof dsp, kind ? "%s.so" : "dsp.so", m->id);
        {
            char t[sizeof m->so];
            snprintf(t, sizeof t, "%s/%s", m->dir, dsp);
            cpy(m->so, sizeof m->so, t);
        }
        if (stat(m->so, &st) != 0 && kind == 0) {   /* (a MIDI FX keeps its dsp in dsp/ in the sources; installed: beside) */
            snprintf(p, sizeof p, "%s/dsp/%s", m->dir, dsp);
            if (stat(p, &st) == 0)
                cpy(m->so, sizeof m->so, p);
        }
        if (stat(m->so, &st) != 0) {
            fprintf(stderr, "fx: %s: no %s\n", m->id, m->so);
            free(json);
            continue;
        }
        caps = mj_find(json, "capabilities");
        params = caps ? mj_find(caps, "chain_params") : NULL;
        if (!params && caps) {   /* (Schwung's own modules: their ui_hierarchy's root page) */
            const char *h = mj_find(caps, "ui_hierarchy"), *lv = h ? mj_find(h, "levels") : NULL;
            const char *r = lv ? mj_find(lv, "root") : NULL;
            params = r ? mj_find(r, "params") : NULL;
        }
        if (params && *params != '[')
            params = NULL;
        for (e = NULL; params && (e = mj_elem(&params)) && m->np < FX_MAX_PARAMS;)
            if (*e == '{') {
                param_parse(&m->p[m->np], e);
                if (m->p[m->np].key[0])
                    m->np++;
            }
        free(json);
        L->n[kind]++;
    }
    closedir(dir);
    qsort(L->mod[kind], (size_t)L->n[kind], sizeof L->mod[kind][0], by_name);
}

fx_lib_t *fx_scan(const char *dir)
{
    fx_lib_t *L = calloc(1, sizeof *L);
    int kind, i, k;
    if (!L)
        abort();
    host_init();
    for (kind = 0; kind < FX_KINDS && dir && *dir; kind++) {
        scan_kind(L, dir, kind);
        for (i = 0; i < L->n[kind]; i++) {
            const fx_mod_t *m = &L->mod[kind][i];
            for (k = 0; k < m->np; k++) {
                const fx_param_t *p = &m->p[k];
                rfm_move_fx_param_t *c = &L->cpar[kind][i][k];
                c->name = p->name;
                c->nwords = p->type == FXP_ENUM ? p->nopt : 0;
                c->words = p->type == FXP_ENUM ? (const char *const *)p->opt : NULL;
                c->def = (uint8_t)to127(p, p->def);
                if (kind == 1 && (is_wet(p->key) || is_dry(p->key)) && p->type != FXP_ENUM) {
                    c->dry_wet = 1;   /* a send's FX: 100 % wet, no dry (rfm1cs: "100 % wet for a send") */
                    c->def = is_dry(p->key) && !is_wet(p->key) ? 0 : 127;
                }
            }
            L->core[kind][i].name = m->name;
            L->core[kind][i].nparams = m->np;
            L->core[kind][i].params = L->cpar[kind][i];
        }
        fprintf(stderr, "fx: %d %s module(s) in %s\n", L->n[kind], kind ? "audio FX" : "MIDI FX", dir);
    }
    return L;
}

const fx_mod_t *fx_mod(const fx_lib_t *lib, int kind, int i)
{
    return lib && kind >= 0 && kind < FX_KINDS && i >= 0 && i < lib->n[kind] ? &lib->mod[kind][i] : NULL;
}

char *fx_lib_json(const fx_lib_t *L)
{
    size_t cap = 4096, n = 0;
    char *o = malloc(cap), esc[200];
    int kind, i, k, j;
#define PUT(...) do { int w_; for (;;) { w_ = snprintf(o + n, cap - n, __VA_ARGS__); if (w_ >= 0 && (size_t)w_ < cap - n) break; \
    cap *= 2; o = realloc(o, cap); if (!o) return NULL; } n += (size_t)w_; } while (0)
    if (!o)
        return NULL;
    PUT("{");
    for (kind = 0; kind < FX_KINDS; kind++) {
        PUT("%s\"%s\":[", kind ? "," : "", kind ? "audio" : "midi");
        for (i = 0; L && i < L->n[kind]; i++) {
            const fx_mod_t *m = &L->mod[kind][i];
            mj_escape(m->name, esc, sizeof esc);
            PUT("%s{\"i\":%d,\"id\":\"%s\",\"name\":\"%s\",\"abbrev\":\"%s\",\"version\":\"%s\",\"builtin\":%s,\"params\":[",
                i ? "," : "", i, m->id, esc, m->abbrev, m->version, m->builtin ? "true" : "false");
            for (k = 0; k < m->np; k++) {
                const fx_param_t *p = &m->p[k];
                const rfm_move_fx_param_t *c = &L->cpar[kind][i][k];
                mj_escape(p->name, esc, sizeof esc);
                PUT("%s{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"min\":%g,\"max\":%g,\"def\":%d,\"wet\":%d",
                    k ? "," : "", p->key, esc, p->type == FXP_ENUM ? "enum" : p->type == FXP_INT ? "int" : "float", p->min,
                    p->max, c->def, c->dry_wet ? (is_dry(p->key) && !is_wet(p->key) ? 2 : 1) : 0);
                if (p->type == FXP_ENUM) {
                    PUT(",\"options\":[");
                    for (j = 0; j < p->nopt; j++) {
                        mj_escape(p->opt[j], esc, sizeof esc);
                        PUT("%s\"%s\"", j ? "," : "", esc);
                    }
                    PUT("]");
                }
                PUT("}");
            }
            PUT("]}");
        }
        PUT("]");
    }
    PUT("}");
#undef PUT
    return o;
}

/* ------------------------------------------------------------------ instances --- */
struct fx_inst {
    fx_mod_t *mod;
    void *dl, *inst;
    midi_fx_api_v1_t *mapi;
    audio_fx_api_v2_t *aapi;
    uint64_t ns;
};

/* the module's dlopen, init and create_instance in a child first: a crash there refuses the module, nothing more */
static int probe(fx_mod_t *m, char *err, size_t cap)
{
    char exe[600], kind[4];
    char *argv[7];
    pid_t pid;
    int st = 0, i;
    ssize_t k;
    if (m->probed)
        return m->probed > 0 ? 0 : -1;
    k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k <= 0)
        return 0;   /* (no /proc: no probe) */
    exe[k] = 0;
    snprintf(kind, sizeof kind, "%d", m->kind);
    argv[0] = exe, argv[1] = "--fx-probe", argv[2] = kind, argv[3] = m->dir, argv[4] = m->so, argv[5] = NULL;
    if (posix_spawn(&pid, exe, NULL, NULL, argv, environ) != 0)
        return 0;
    for (i = 0; i < 300; i++) {   /* (3 s at most) */
        const pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid)
            break;
        if (r < 0) {
            st = 0;
            break;
        }
        usleep(10000);
    }
    if (i == 300) {
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        snprintf(err, cap, "%s: its probe hung", m->id);
        m->probed = -1;
        return -1;
    }
    if (WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) != 0)) {
        if (WIFSIGNALED(st))
            snprintf(err, cap, "%s crashed as it loaded (signal %d): refused", m->id, WTERMSIG(st));
        else
            snprintf(err, cap, "%s could not load (probe exit %d): refused", m->id, WEXITSTATUS(st));
        m->probed = -1;
        return -1;
    }
    m->probed = 1;
    return 0;
}

static fx_inst_t *open_now(fx_mod_t *m, char *err, size_t cap)
{
    fx_inst_t *x = calloc(1, sizeof *x);
    if (!x)
        return NULL;
    host_init();
    x->mod = m;
    if (!(x->dl = dlopen(m->so, RTLD_NOW | RTLD_LOCAL))) {
        snprintf(err, cap, "%s: %s", m->id, dlerror());
        free(x);
        return NULL;
    }
    if (m->kind == RFM_MOVE_FX_MIDI) {
        midi_fx_init_fn init = (midi_fx_init_fn)dlsym(x->dl, MIDI_FX_INIT_SYMBOL);
        x->mapi = init ? init(&g_host.api) : NULL;
        if (!x->mapi || !x->mapi->create_instance || !x->mapi->process_midi) {
            snprintf(err, cap, "%s: no %s", m->id, MIDI_FX_INIT_SYMBOL);
            goto fail;
        }
        x->inst = x->mapi->create_instance(m->dir, NULL);
    } else {
        audio_fx_init_v2_fn init = (audio_fx_init_v2_fn)dlsym(x->dl, AUDIO_FX_INIT_V2_SYMBOL);
        x->aapi = init ? init(&g_host.api) : NULL;
        if (!x->aapi || !x->aapi->create_instance || !x->aapi->process_block) {
            snprintf(err, cap, "%s: no %s (a v1 module?)", m->id, AUDIO_FX_INIT_V2_SYMBOL);
            goto fail;
        }
        x->inst = x->aapi->create_instance(m->dir, NULL);
    }
    if (!x->inst) {
        snprintf(err, cap, "%s: create_instance failed", m->id);
        goto fail;
    }
    return x;
fail:
    dlclose(x->dl);
    free(x);
    return NULL;
}

fx_inst_t *fx_open(fx_mod_t *m, char *err, size_t cap)
{
    if (!m) {
        snprintf(err, cap, "no such module");
        return NULL;
    }
    if (probe(m, err, cap) != 0)
        return NULL;
    return open_now(m, err, cap);
}

void fx_close(fx_inst_t *x)
{
    if (!x)
        return;
    if (x->mapi && x->mapi->destroy_instance)
        x->mapi->destroy_instance(x->inst);
    if (x->aapi && x->aapi->destroy_instance)
        x->aapi->destroy_instance(x->inst);
    /* (no dlclose: a module may keep threads or atexit hooks; its code stays mapped, a second open reuses it) */
    free(x);
}

const fx_mod_t *fx_inst_mod(const fx_inst_t *x) { return x ? x->mod : NULL; }
uint64_t fx_ns(const fx_inst_t *x) { return x ? x->ns : 0; }

static void set_param(fx_inst_t *x, const char *k, const char *v)
{
    const uint64_t t0 = now_ns();
    if (x->mapi && x->mapi->set_param)
        x->mapi->set_param(x->inst, k, v);
    if (x->aapi && x->aapi->set_param)
        x->aapi->set_param(x->inst, k, v);
    x->ns += now_ns() - t0;
}

void fx_set(fx_inst_t *x, int k, int v)
{
    const fx_param_t *p;
    char s[64];
    if (!x || k < 0 || k >= x->mod->np)
        return;
    p = &x->mod->p[k];
    if (p->type == FXP_ENUM) {
        if (v < 0 || v >= p->nopt)
            return;
        if (p->opt_str)
            snprintf(s, sizeof s, "%s", p->opt[v]);
        else
            snprintf(s, sizeof s, "%d", v);
    } else {
        const double d = p->min + (p->max - p->min) * (v < 0 ? 0 : v > 127 ? 127 : v) / 127.0;
        if (p->type == FXP_INT)
            snprintf(s, sizeof s, "%ld", lrint(d));
        else
            snprintf(s, sizeof s, "%.4f", d);
    }
    set_param(x, p->key, s);
}

int fx_state_get(fx_inst_t *x, char *buf, int cap)
{
    int n = -1;
    if (!x || cap < 2)
        return -1;
    buf[0] = 0;
    if (x->mapi && x->mapi->get_param)
        n = x->mapi->get_param(x->inst, "state", buf, cap);
    if (x->aapi && x->aapi->get_param)
        n = x->aapi->get_param(x->inst, "state", buf, cap);
    if (n < 0 || n >= cap)
        return -1;
    buf[n] = 0;
    return n;
}

void fx_state_set(fx_inst_t *x, const char *s)
{
    if (x && s && *s)
        set_param(x, "state", s);
}

int fx_midi(fx_inst_t *x, const uint8_t *in, int n, uint8_t out[][3], int lens[], int max)
{
    uint64_t t0;
    int r;
    if (!x || !x->mapi)
        return 0;
    t0 = now_ns();
    r = x->mapi->process_midi(x->inst, in, n, out, lens, max);
    x->ns += now_ns() - t0;
    return r < 0 ? 0 : r > max ? max : r;
}

int fx_tick(fx_inst_t *x, int frames, uint8_t out[][3], int lens[], int max)
{
    uint64_t t0;
    int r;
    if (!x || !x->mapi || !x->mapi->tick)
        return 0;
    t0 = now_ns();
    r = x->mapi->tick(x->inst, frames, 44100, out, lens, max);
    x->ns += now_ns() - t0;
    return r < 0 ? 0 : r > max ? max : r;
}

void fx_audio(fx_inst_t *x, int16_t *lr, int frames)
{
    uint64_t t0;
    if (!x || !x->aapi)
        return;
    t0 = now_ns();
    x->aapi->process_block(x->inst, lr, frames);
    x->ns += now_ns() - t0;
}

int fx_probe_main(int kind, const char *dir, const char *so)
{
    fx_mod_t m;
    fx_inst_t *x;
    char err[300];
    memset(&m, 0, sizeof m);
    m.kind = kind;
    snprintf(m.id, sizeof m.id, "probe");
    snprintf(m.dir, sizeof m.dir, "%s", dir);
    snprintf(m.so, sizeof m.so, "%s", so);
    if (!(x = open_now(&m, err, sizeof err))) {
        fprintf(stderr, "fx probe: %s\n", err);
        return 1;
    }
    if (kind == RFM_MOVE_FX_AUDIO) {
        int16_t b[256] = { 0 };
        fx_audio(x, b, 128);
    } else {
        uint8_t o[FX_MAX_OUT][3];
        int l[FX_MAX_OUT];
        fx_tick(x, 128, o, l, FX_MAX_OUT);
    }
    fx_close(x);
    return 0;
}
