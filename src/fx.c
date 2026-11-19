/* SPDX-License-Identifier: MPL-2.0 */
/* Schwung FX hosting: see fx.h. */
#define _GNU_SOURCE
#include "fx.h"

#include <ctype.h>
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
/* a JSON value (as mj_find gives it) as a number (true / false: 1 / 0): 0, or -1 when it is none */
static int vnum(const char *v, double *out)
{
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
static int num(const char *obj, const char *key, double *out) { return vnum(obj ? mj_find(obj, key) : NULL, out); }
/* a JSON string element (an option) into out, plain: escapes dropped */
static void str_elem(const char *v, char *out, size_t cap)
{
    size_t k = 0;
    if (*v != '"') {   /* (a number as an option) */
        while (*v && *v != ',' && *v != ']' && *v != '}' && *v != ' ' && k + 1 < cap)
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
static void cpy(char *d, size_t cap, const char *s) { size_t n = 0; while (n + 1 < cap && s[n]) n++; memmove(d, s, n); d[n] = 0; }
static int by_name(const void *a, const void *b) { return strcasecmp(((const fx_mod_t *)a)->name, ((const fx_mod_t *)b)->name); }

/* ---- a parameter's metadata: the hierarchy's entry h first, then chain_params' c (Schwung: "Metadata (type, min,
 * max) can come from either the hierarchy or chain_params") */
static const char *fld(const char *h, const char *c, const char *k)
{
    const char *v = h ? mj_find(h, k) : NULL;
    return v ? v : c ? mj_find(c, k) : NULL;
}
static int fstr(const char *h, const char *c, const char *k, char *out, size_t cap)
{
    const char *v = fld(h, c, k);
    out[0] = 0;
    if (!v || *v != '"')
        return -1;
    str_elem(v, out, cap);
    return 0;
}

/* Schwung's per-word mnemonics (its src/shared/param_pages/render_page_movy.mjs WORD_ABBREV, MIT; a subset) */
static const char *const ABBR[][2] = {
    { "attack", "ATK" }, { "decay", "DEC" }, { "sustain", "SUS" }, { "release", "REL" }, { "hold", "HLD" },
    { "envelope", "ENV" }, { "amount", "AMT" }, { "depth", "DPT" }, { "cutoff", "CUT" }, { "frequency", "FRQ" },
    { "resonance", "RES" }, { "filter", "FLT" }, { "oscillator", "OSC" }, { "waveform", "WAV" }, { "shape", "SHP" },
    { "pitch", "PIT" }, { "detune", "DET" }, { "octave", "OCT" }, { "transpose", "TRN" }, { "semitone", "SEM" },
    { "glide", "GLD" }, { "portamento", "GLD" }, { "noise", "NSE" }, { "spread", "SPR" }, { "offset", "OFS" },
    { "position", "POS" }, { "threshold", "THR" }, { "ratio", "RAT" }, { "modulation", "MOD" }, { "velocity", "VEL" },
    { "pressure", "PRS" }, { "sensitivity", "SNS" }, { "amplitude", "AMP" }, { "volume", "VOL" }, { "level", "LEV" },
    { "balance", "BAL" }, { "panning", "PAN" }, { "width", "WID" }, { "phase", "PHS" }, { "feedback", "FBK" },
    { "delay", "DLY" }, { "reverb", "REV" }, { "chorus", "CHO" }, { "flanger", "FLG" }, { "phaser", "PHR" },
    { "tremolo", "TRM" }, { "vibrato", "VIB" }, { "overdrive", "OVR" }, { "distortion", "DST" },
    { "saturation", "SAT" }, { "drive", "DRV" }, { "compressor", "CMP" }, { "limiter", "LIM" }, { "damping", "DMP" },
    { "diffusion", "DIF" }, { "sample", "SMP" }, { "start", "STR" }, { "length", "LEN" }, { "reverse", "RVS" },
    { "speed", "SPD" }, { "random", "RND" }, { "quantize", "QNT" }, { "channel", "CHN" }, { "output", "OUT" },
    { "input", "IN" }, { "voice", "VCE" }, { "voices", "VCES" }, { "trigger", "TRG" }, { "rotation", "ROT" },
    { "rhythm", "RHY" }, { "density", "DNS" }, { "unison", "UNI" }, { "division", "DIV" }, { "color", "CLR" },
    { "colour", "CLR" }, { "stereo", "STO" }, { "polyphony", "POLY" }, { "send", "SND" }, { "number", "NUM" },
    { "advanced", "ADV" }, { "predelay", "PDLY" }, { "humanize", "HUM" }, { "source", "SRC" }, { "grain", "GRN" },
    { "inversion", "INV" }, { "direction", "DIR" }, { "bandwidth", "BW" }, { "lowpass", "LPF" }, { "highpass", "HPF" },
    { "bandpass", "BPF" }, { "character", "CHR" }, { "destination", "DES" }, { "smoothing", "SMO" },
};
#define LABEL_CHARS 5
void fx_abbrev(const char *name, char *out, size_t cap)
{
    char w[8][24], t[64];
    int nw = 0, i, n = 0;
    const char *s = name ? name : "";
    if (cap < 2) {
        if (cap)
            out[0] = 0;
        return;
    }
    if (strlen(s) <= LABEL_CHARS) {   /* (it fits: as written) */
        cpy(out, cap, s);
        return;
    }
    while (*s && nw < 8) {   /* the words (space, '_' or '-' apart), each its mnemonic when the table has it */
        size_t k = 0;
        while (*s == ' ' || *s == '_' || *s == '-')
            s++;
        if (!*s)
            break;
        while (*s && *s != ' ' && *s != '_' && *s != '-' && k + 1 < sizeof w[0])
            w[nw][k++] = *s++;
        while (*s && *s != ' ' && *s != '_' && *s != '-')
            s++;
        w[nw][k] = 0;
        for (i = 0; i < (int)(sizeof ABBR / sizeof ABBR[0]); i++)
            if (!strcasecmp(w[nw], ABBR[i][0])) {
                cpy(w[nw], sizeof w[0], ABBR[i][1]);
                break;
            }
        nw++;
    }
    t[0] = 0;
    for (i = 0; i < nw; i++)   /* the mnemonics joined: "FLT ENV" */
        n += snprintf(t + n, sizeof t - (size_t)n, "%s%s", i ? " " : "", w[i]);
    if (n > LABEL_CHARS && nw > 1) {   /* squeezed: the leading words' initials (numbers whole), the last word */
        char tail[24] = "";
        int last = nw - 1;
        while (last > 0 && isdigit((unsigned char)w[last][0]))   /* (a trailing number stays: "Osc 1 Octave" keeps its 1) */
            last--;
        for (i = last + 1; i < nw; i++)
            strncat(tail, w[i], sizeof tail - strlen(tail) - 1);
        n = 0, t[0] = 0;
        for (i = 0; i < last && n < (int)sizeof t - 1; i++) {
            if (isdigit((unsigned char)w[i][0]))
                n += snprintf(t + n, sizeof t - (size_t)n, "%s", w[i]);
            else
                t[n++] = (char)toupper((unsigned char)w[i][0]), t[n] = 0;
        }
        {   /* the last word, cut to leave room for the trailing number */
            const int room = LABEL_CHARS - n - (int)strlen(tail);
            if (room > 0)
                n += snprintf(t + n, sizeof t - (size_t)n, "%.*s", room, w[last]);
            n += snprintf(t + n, sizeof t - (size_t)n, "%s", tail);
        }
    }
    if (n > LABEL_CHARS)
        t[LABEL_CHARS] = 0;   /* (cut) */
    cpy(out, cap, t);
}

/* decimals of a step (0.05: 2; 0.1: 1; 1: 0) */
static int step_dec(double st)
{
    int d = 0;
    if (st <= 0)
        return -1;
    while (d < 3 && fabs(st * pow(10, d) - lrint(st * pow(10, d))) > 1e-6)
        d++;
    return d;
}
/* a parameter: h its hierarchy entry (or NULL), c its chain_params entry (or NULL), key its key. 0, or -1 when it is
 * no value a knob can turn (a filepath, a string, a canvas, ..) */
static int param_parse(fx_param_t *p, const char *h, const char *c, const char *key, int kind)
{
    char t[16] = "", s[64], fmt[16] = "";
    double d;
    const char *v;
    memset(p, 0, sizeof *p);
    cpy(p->key, sizeof p->key, key);
    if ((fstr(h, c, "name", p->name, sizeof p->name) || !p->name[0]) && (fstr(h, c, "label", p->name, sizeof p->name) || !p->name[0]))
        cpy(p->name, sizeof p->name, p->key);
    fstr(h, c, "type", t, sizeof t);
    if (!t[0])
        cpy(t, sizeof t, fld(h, c, "options") ? "enum" : "float");
    if (!strcmp(t, "enum") || !strcmp(t, "mode"))
        p->type = FXP_ENUM;
    else if (!strcmp(t, "int"))
        p->type = FXP_INT;
    else if (!strcmp(t, "float"))
        p->type = FXP_FLOAT;
    else if (!strcmp(t, "bool") || !strcmp(t, "boolean"))
        p->type = FXP_ENUM;
    else
        return -1;
    p->min = 0, p->max = p->type == FXP_FLOAT ? 1.0 : 127.0;
    vnum(fld(h, c, "min"), &p->min);
    vnum(fld(h, c, "max"), &p->max);
    vnum(fld(h, c, "step"), &p->step);
    if (p->step < 0)
        p->step = 0;
    fstr(h, c, "unit", p->unit, sizeof p->unit);
    fstr(h, c, "display_format", fmt, sizeof fmt);
    if (fstr(h, c, "short_name", p->sname, sizeof p->sname) || !p->sname[0])
        fx_abbrev(p->name, p->sname, sizeof p->sname);
    p->def = p->min;
    if (p->type == FXP_ENUM) {
        const char *it = fld(h, c, "options"), *e;
        int n = 0;
        p->opt_str = vnum(fld(h, c, "options_as_string"), &d) == 0 && d != 0;
        if (it && *it == '[') {
            const char *it2 = it;
            while (mj_elem(&it2))
                n++;
        }
        if (!n && (!strcmp(t, "bool") || !strcmp(t, "boolean"))) {   /* (a bool: Off / On, by its index) */
            p->opt = calloc(2, sizeof *p->opt);
            p->opt[0] = strdup("Off"), p->opt[1] = strdup("On"), p->nopt = 2;
        } else {
            p->opt = calloc((size_t)(n ? n : 1), sizeof *p->opt);
            for (; n && (e = mj_elem(&it)) && p->nopt < n;) {
                str_elem(e, s, sizeof s);
                p->opt[p->nopt++] = strdup(s);
            }
        }
        p->min = 0, p->max = p->nopt ? p->nopt - 1 : 0, p->step = 0, p->unit[0] = 0;
        if ((v = fld(h, c, "default")) && *v == '"') {   /* the default: an index, or an option's text */
            int i;
            str_elem(v, s, sizeof s);
            for (i = 0; i < p->nopt; i++)
                if (!strcmp(p->opt[i], s))
                    p->def = i;
        } else
            vnum(v, &p->def);
        return 0;
    }
    vnum(fld(h, c, "default"), &p->def);
    if (p->max < p->min)
        p->max = p->min;
    p->def = p->def < p->min ? p->min : p->def > p->max ? p->max : p->def;
    /* how the value reads on the Move (Schwung's formatter, src/shared/param_format.mjs: "%" in 0..1 is x100, sec, ms,
     * dB and Hz their decimals; display_format "%.Nf" wins) */
    if (p->max > p->min) {
        int dec = -1;
        const char *f = strstr(fmt, "%.");
        double mul = 1;
        if (!strcmp(p->unit, "%") && p->max <= 1.0)
            mul = 100;
        if (!strcmp(p->unit, "sec"))
            cpy(p->unit, sizeof p->unit, "s");
        if (f && isdigit((unsigned char)f[2]))
            dec = f[2] - '0';
        else if (p->type == FXP_INT)
            dec = 0;
        else if ((dec = step_dec(p->step * mul)) >= 0)
            ;
        else if (!strcmp(p->unit, "%") || !strcmp(p->unit, "Hz") || !strcmp(p->unit, "BPM") || !strcmp(p->unit, "st"))
            dec = 0;
        else {
            const double r = (p->max - p->min) * mul;
            dec = !strcmp(p->unit, "s") ? 2 : !strcmp(p->unit, "dB") || !strcmp(p->unit, "ms") ? 1 : r <= 10 ? 2 : r <= 100 ? 1 : 0;
        }
        p->lo = (float)(p->min * mul), p->hi = (float)(p->max * mul), p->dec = dec > 3 ? 3 : dec;
    }
    (void)kind;
    return 0;
}

int fx_to127(const fx_param_t *p, double v)
{
    if (!p)
        return -1;
    if (p->type == FXP_ENUM)
        return (int)(v < 0 ? 0 : v > p->max ? p->max : v);
    if (p->max <= p->min)
        return 0;
    v = (v - p->min) / (p->max - p->min) * 127.0;
    return (int)lrint(v < 0 ? 0 : v > 127 ? 127 : v);
}
int fx_wire(const fx_param_t *p, int v, char *s, size_t cap)
{
    double d;
    if (!p || v < 0 || v == RFM_MOVE_FX_UNSET)
        return -1;
    if (p->type == FXP_ENUM) {
        if (v >= p->nopt)
            return -1;
        return p->opt_str ? snprintf(s, cap, "%s", p->opt[v]) : snprintf(s, cap, "%d", v);
    }
    v = v > 127 ? 127 : v;
    d = p->min + (p->max - p->min) * v / 127.0;
    if (p->step > 0)   /* (snapped to its step, from min) */
        d = p->min + floor((d - p->min) / p->step + 0.5) * p->step;
    d = d < p->min ? p->min : d > p->max ? p->max : d;
    if (p->type == FXP_INT)
        return snprintf(s, cap, "%ld", lrint(d));
    return snprintf(s, cap, "%.4f", d);
}
static int is_wet(const char *k) { return strstr(k, "wet") || !strcmp(k, "mix"); }
static int is_dry(const char *k) { return strstr(k, "dry") != NULL; }

/* ---- the plan: the hierarchy's levels walked from root (fx.h) */
typedef struct {
    fx_mod_t *m;
    const char *levels, *chain;
    char seen[32][48];
    int nseen, kind;
} plan_t;
/* an array's object element whose "key" is key (NULL none) */
static const char *by_key(const char *arr, const char *key)
{
    const char *it = arr, *e;
    char k[48];
    if (!arr || *arr != '[')
        return NULL;
    while ((e = mj_elem(&it)))
        if (*e == '{' && !mj_string(e, "key", k, sizeof k) && !strcmp(k, key))
            return e;
    return NULL;
}
/* the parameter with that key: its index (one already planned: the same one), or -1 when it is no knob's */
static int intern(plan_t *P, const char *key, const char *h)
{
    fx_mod_t *m = P->m;
    int i;
    if (!key[0])
        return -1;
    for (i = 0; i < m->np; i++)
        if (!strcmp(m->p[i].key, key))
            return i;
    if (m->np >= FX_MAX_PARAMS)
        return -1;
    if (param_parse(&m->p[m->np], h, by_key(P->chain, key), key, P->kind) != 0) {
        memset(&m->p[m->np], 0, sizeof m->p[0]);
        return -1;
    }
    return m->np++;
}
static void add_pages(fx_mod_t *m, const char *name, const int *cell, int n)
{
    int pg, k;
    for (pg = 0; pg * RFM_MOVE_FX_KNOBS < n && m->npg < FX_MAX_PAGES; pg++) {
        fx_page_t *g = &m->pg[m->npg];
        int any = 0;
        for (k = 0; k < RFM_MOVE_FX_KNOBS; k++) {
            const int c = pg * RFM_MOVE_FX_KNOBS + k;
            g->knob[k] = (int16_t)(c < n ? cell[c] : -1);
            any |= g->knob[k] >= 0;
        }
        if (!any)
            continue;   /* (a page of gaps: none) */
        if (pg)
            snprintf(g->name, sizeof g->name, "%.26s %d", name, pg + 1);
        else
            cpy(g->name, sizeof g->name, name);
        m->npg++;
    }
}
static void plan_level(plan_t *P, const char *lname, int depth)
{
    const char *lv = mj_find(P->levels, lname), *it, *e, *params;
    char name[32], key[48], sel[5][48], links[16][48];
    int cell[FX_MAX_PAGES * RFM_MOVE_FX_KNOBS], n = 0, nk, nlinks = 0, i;
    static const char *const SEL[5] = { "list_param", "count_param", "name_param", "items_param", "select_param" };
    if (!lv || *lv != '{' || depth > 16 || P->nseen >= 32)
        return;
    for (i = 0; i < P->nseen; i++)
        if (!strcmp(P->seen[i], lname))
            return;
    cpy(P->seen[P->nseen++], sizeof P->seen[0], lname);
    if ((mj_string(lv, "name", name, sizeof name) || !name[0]) && (mj_string(lv, "label", name, sizeof name) || !name[0]))
        cpy(name, sizeof name, strcmp(lname, "root") ? lname : P->m->name);
    for (i = 0; i < 5; i++)   /* (a preset browser's keys: never a knob's) */
        if (mj_string(lv, SEL[i], sel[i], sizeof sel[i]))
            sel[i][0] = 0;
    params = mj_find(lv, "params");
    if (params && *params != '[')
        params = NULL;
#define SELECTOR(k) (!strcmp(k, sel[0]) || !strcmp(k, sel[1]) || !strcmp(k, sel[2]) || !strcmp(k, sel[3]) || !strcmp(k, sel[4]))
    /* the knobs, by position (null a gap) */
    if ((it = mj_find(lv, "knobs")) && *it == '[')
        while ((e = mj_elem(&it)) && n < (int)(sizeof cell / sizeof cell[0])) {
            int idx = -1;
            if (*e == '"') {
                str_elem(e, key, sizeof key);
                if (key[0] && !SELECTOR(key))
                    idx = intern(P, key, by_key(params, key));
            }
            cell[n++] = idx;
        }
    while (n > 0 && cell[n - 1] < 0)   /* (a trailing null describes nothing) */
        n--;
    nk = n;
    /* then the params on no knob, after the last knob; the links, for after this level */
    for (it = params; it && (e = mj_elem(&it));) {
        int idx, j, on = 0;
        if (*e == '"')
            str_elem(e, key, sizeof key);
        else if (*e == '{' && mj_string(e, "key", key, sizeof key) == 0 && key[0])
            ;
        else {
            if (*e == '{' && !mj_string(e, "level", key, sizeof key) && key[0] && nlinks < 16)
                cpy(links[nlinks++], sizeof links[0], key);
            continue;
        }
        if (SELECTOR(key))
            continue;
        if ((idx = intern(P, key, *e == '{' ? e : NULL)) < 0)
            continue;
        for (j = 0; j < nk; j++)
            on |= cell[j] == idx;
        if (!on && n < (int)(sizeof cell / sizeof cell[0]))
            cell[n++] = idx;
    }
#undef SELECTOR
    add_pages(P->m, name, cell, n);
    for (i = 0; i < nlinks; i++)
        plan_level(P, links[i], depth + 1);
}
/* the module's params and pages from its module.json */
static void plan(fx_mod_t *m, const char *json, int kind)
{
    const char *caps = mj_find(json, "capabilities"), *h, *e, *it;
    plan_t *P = calloc(1, sizeof *P);
    m->p = calloc(FX_MAX_PARAMS, sizeof *m->p);
    if (!P || !m->p)
        abort();
    P->m = m, P->kind = kind;
    P->chain = caps ? mj_find(caps, "chain_params") : NULL;
    if (P->chain && *P->chain != '[')
        P->chain = NULL;
    h = caps ? mj_find(caps, "ui_hierarchy") : NULL;
    if (!h)
        h = mj_find(json, "ui_hierarchy");
    P->levels = h && *h == '{' ? mj_find(h, "levels") : NULL;
    if (P->levels && *P->levels == '{')
        plan_level(P, "root", 0);
    if (!m->np && P->chain) {   /* (no hierarchy, or nothing on it: chain_params in order, pages of 8) */
        int cell[FX_MAX_PARAMS], n = 0;
        char key[48];
        m->npg = 0;
        for (it = P->chain; (e = mj_elem(&it)) && n < FX_MAX_PARAMS;)
            if (*e == '{' && !mj_string(e, "key", key, sizeof key)) {
                const int idx = intern(P, key, NULL);
                if (idx >= 0)
                    cell[n++] = idx;
            }
        add_pages(m, m->name, cell, n);
    }
    if (kind == RFM_MOVE_FX_AUDIO) {   /* (a wet / dry / mix: its send's default, 100 % wet, no dry) */
        int i;
        for (i = 0; i < m->np; i++)
            if ((is_wet(m->p[i].key) || is_dry(m->p[i].key)) && m->p[i].type != FXP_ENUM)
                m->p[i].def = is_dry(m->p[i].key) && !is_wet(m->p[i].key) ? m->p[i].min : m->p[i].max;
    }
    if (m->np) {
        fx_param_t *r = realloc(m->p, (size_t)m->np * sizeof *m->p);
        if (r)
            m->p = r;
    } else {
        free(m->p);
        m->p = NULL;
    }
    free(P);
}

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
        plan(m, json, kind);
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
        for (i = 0; i < L->n[kind]; i++) {   /* (after the sort: the core's view points into the modules) */
            fx_mod_t *m = &L->mod[kind][i];
            rfm_move_fx_param_t *cp = L->cpar[kind][i] = calloc((size_t)(m->np ? m->np : 1), sizeof *cp);
            if (!cp)
                abort();
            for (k = 0; k < m->np; k++) {
                const fx_param_t *p = &m->p[k];
                rfm_move_fx_param_t *c = &cp[k];
                c->name = p->name;
                c->short_name = p->sname[0] ? p->sname : NULL;
                c->nwords = p->type == FXP_ENUM ? p->nopt : 0;
                c->words = p->type == FXP_ENUM ? (const char *const *)p->opt : NULL;
                c->def = (uint8_t)fx_to127(p, p->def);
                c->lo = p->lo, c->hi = p->hi, c->decimals = (uint8_t)p->dec, c->unit = p->unit[0] ? p->unit : NULL;
                if (kind == 1 && (is_wet(p->key) || is_dry(p->key)) && p->type != FXP_ENUM)
                    c->dry_wet = 1;   /* a send's FX: 100 % wet, no dry (rfm1cs: "100 % wet for a send") */
            }
            for (k = 0; k < m->npg; k++) {
                L->cpg[kind][i][k].name = m->pg[k].name;
                memcpy(L->cpg[kind][i][k].knob, m->pg[k].knob, sizeof m->pg[k].knob);
            }
            L->core[kind][i].name = m->name;
            L->core[kind][i].nparams = m->np;
            L->core[kind][i].params = cp;
            L->core[kind][i].npages = m->npg;
            L->core[kind][i].pages = L->cpg[kind][i];
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
    char *o = malloc(cap), esc[200], esc2[64];
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
                mj_escape(p->sname, esc2, sizeof esc2);
                PUT("%s{\"key\":\"%s\",\"name\":\"%s\",\"short\":\"%s\",\"type\":\"%s\",\"min\":%g,\"max\":%g,\"step\":%g,"
                    "\"def\":%d,\"wet\":%d",
                    k ? "," : "", p->key, esc, esc2, p->type == FXP_ENUM ? "enum" : p->type == FXP_INT ? "int" : "float",
                    p->min, p->max, p->step, c->def, c->dry_wet ? (is_dry(p->key) && !is_wet(p->key) ? 2 : 1) : 0);
                if (p->lo != p->hi) {
                    mj_escape(p->unit, esc2, sizeof esc2);
                    PUT(",\"lo\":%g,\"hi\":%g,\"dec\":%d,\"unit\":\"%s\"", p->lo, p->hi, p->dec, esc2);
                }
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
            PUT("],\"pages\":[");
            for (k = 0; k < m->npg; k++) {
                mj_escape(m->pg[k].name, esc, sizeof esc);
                PUT("%s{\"name\":\"%s\",\"knobs\":[", k ? "," : "", esc);
                for (j = 0; j < RFM_MOVE_FX_KNOBS; j++)
                    PUT("%s%d", j ? "," : "", m->pg[k].knob[j]);
                PUT("]}");
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
    char s[64];
    if (!x || k < 0 || k >= x->mod->np || fx_wire(&x->mod->p[k], v, s, sizeof s) < 0)
        return;
    set_param(x, x->mod->p[k].key, s);
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
