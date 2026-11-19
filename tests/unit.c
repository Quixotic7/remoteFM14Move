/* SPDX-License-Identifier: MPL-2.0 */
/* Unit tests: the manifest reader (and the catalog index's arrays), SHA-256, the output stage (mix and limiter), the
 * mDNS answers, the FX stage (pan, the send buses, Schwung's freeverb and arp through fx.c: build/fxmods). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine_dsp.h"
#include "fx.h"
#include "mdns.h"
#include "minijson.h"
#include "sha256.h"

static int g_fail, g_n;
#define CHECK(c, ...)                                                                                               \
    do {                                                                                                            \
        g_n++;                                                                                                      \
        printf((c) ? "  ok    " : "  FAIL  ");                                                                      \
        if (!(c))                                                                                                   \
            g_fail++;                                                                                               \
        printf(__VA_ARGS__);                                                                                        \
        putchar('\n');                                                                                              \
    } while (0)

static void json(void)
{
    const char *j = "{\n \"rfm1m\": 1, \"sha256\": \"ab\\u0063\",\n \"manifest\": {\"id\": \"felucca\", \"authors\": "
                    "[\"Leo \\\"K\\\"\"], \"name\": \"Felucca\", \"version\": \"1.8.2\", \"abi\": 6}}";
    char v[64];
    long n = 0;
    const char *m = mj_find(j, "manifest");
    CHECK(m && !mj_string(m, "id", v, sizeof v) && !strcmp(v, "felucca"), "manifest.id = %s", v);
    CHECK(!mj_string(m, "version", v, sizeof v) && !strcmp(v, "1.8.2"), "manifest.version past an array");
    CHECK(!mj_string(j, "sha256", v, sizeof v) && !strcmp(v, "abc"), "a \\u escape");
    CHECK(!mj_int(m, "abi", &n) && n == 6, "an integer");
    CHECK(mj_string(j, "id", v, sizeof v) == -1, "top level only: no id outside the manifest");
    CHECK(mj_escape("a\"b\n", v, sizeof v) && !strcmp(v, "a\\\"b\\u000a"), "escape");
}

static void arrays(void)
{
    const char *j = "{\"firmwares\": [ {\"id\":\"a\",\"files\":[{\"name\":\"x\"},{\"name\":\"y.json\"}]} , "
                    "{\"id\":\"b\"}, 3 ], \"empty\": []}";
    const char *it = mj_find(j, "firmwares"), *e;
    char ids[8] = "", v[16];
    int n = 0;
    while ((e = mj_elem(&it)))
        if (*e == '{' && !mj_string(e, "id", v, sizeof v))
            strcat(ids, v), n++;
        else
            n++;
    CHECK(n == 3 && !strcmp(ids, "ab"), "an array's elements in turn: %d, ids %s", n, ids);
    it = mj_find(j, "empty");
    CHECK(mj_elem(&it) == NULL, "an empty array");
}

static void sha(void)
{
    sha256_t s;
    uint8_t d[32];
    static char big[1000001];
    char hex[65];
    int i;
    sha256_init(&s);
    sha256_update(&s, "abc", 3);
    sha256_final(&s, d);
    for (i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 of abc");
    memset(big, 'a', 1000000);
    sha256_init(&s);
    for (i = 0; i < 1000000; i += 999)
        sha256_update(&s, big + i, (size_t)(1000000 - i < 999 ? 1000000 - i : 999));
    sha256_final(&s, d);
    for (i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
    CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
          "SHA-256 of a million a's, in uneven pieces");
}

static void dsp(void)
{
    static mixer_t m;
    int16_t a[256], b[256], out[256];
    int i, same = 1, over = 0;
    for (i = 0; i < 256; i++)
        a[i] = (int16_t)(i * 37 - 4000), b[i] = (int16_t)(i % 2 ? 30000 : -30000);
    memset(&m, 0, sizeof m);
    mix_clear(&m, 128);
    mix_add(&m, a, 128, 127);
    mix_out(&m, out, 128);
    for (i = 0; i < 256; i++)
        same &= out[i] == a[i];
    CHECK(same && m.limited == 0, "one slot at 127, under the threshold: bit for bit");
    mix_clear(&m, 128);
    mix_add(&m, b, 128, 127);
    mix_add(&m, b, 128, 127);
    mix_out(&m, out, 128);
    for (i = 0; i < 256; i++)
        over |= abs(out[i]) > (int)(LIM_THRESH * 32768.0f) + 1;
    CHECK(!over && m.limited > 0, "two loud slots: held at -1 dBFS (%u samples limited)", m.limited);
    {   /* the release: quiet again, the gain comes back */
        int k;
        for (k = 0; k < 400; k++) {
            mix_clear(&m, 128);
            mix_add(&m, a, 128, 127);
            mix_out(&m, out, 128);
        }
        same = 1;
        for (i = 0; i < 256; i++)
            same &= out[i] == a[i];
        CHECK(same, "after 1.2 s of quiet: unity again");
    }
    mix_clear(&m, 128);
    mix_add(&m, a, 128, 0);
    mix_out(&m, out, 128);
    CHECK(out[10] == 0, "level 0: silent");
}

static void mdns(void)
{
    /* a query for remotefm1.local A from port 5353, and the same from a plain resolver (port 50000, id 0x1234) */
    static const uint8_t q[] = { 0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 9, 'r', 'e', 'm', 'o', 't', 'e', 'f', 'm',
                                 '1', 5, 'l', 'o', 'c', 'a', 'l', 0, 0, 1, 0, 1 };
    static const uint8_t other[] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 4, 'm', 'o', 'v', 'e', 5, 'l', 'o', 'c', 'a',
                                     'l', 0, 0, 1, 0, 1 };
    const uint8_t addr[4] = { 172, 16, 254, 1 };
    uint8_t out[512];
    int uni = -1;
    size_t n = mdns_answer(q, sizeof q, "remotefm1.local", addr, 5353, out, sizeof out, &uni);
    CHECK(n == 12 + 17 + 10 + 4 && out[0] == 0 && out[1] == 0 && out[2] == 0x84 && out[7] == 1 && uni == 0 &&
              !memcmp(out + n - 4, addr, 4) && out[12 + 17 + 2] == 0x80,
          "an mDNS query: a multicast answer, id 0, cache-flush, the address (%zu bytes)", n);
    n = mdns_answer(q, sizeof q, "RemoteFM1.local", addr, 50000, out, sizeof out, &uni);
    CHECK(n > 0 && out[0] == 0x12 && out[1] == 0x34 && out[5] == 1 && uni == 1, "a plain resolver: unicast, its id and question");
    n = mdns_answer(other, sizeof other, "remotefm1.local", addr, 5353, out, sizeof out, &uni);
    CHECK(n == 0, "move.local: not ours, no answer");
    n = mdns_answer(out, 5, "remotefm1.local", addr, 5353, out, sizeof out, &uni);
    CHECK(n == 0, "a short packet: nothing");
    {   /* an AAAA query for our name: NSEC (A only) as the answer at once, the address as additional */
        uint8_t q6[sizeof q];
        memcpy(q6, q, sizeof q);
        q6[sizeof q - 3] = 28;
        n = mdns_answer(q6, sizeof q6, "remotefm1.local", addr, 5353, out, sizeof out, &uni);
        CHECK(n > 0 && out[7] == 1 && out[11] == 1 && out[12 + 17 + 1] == 47 && out[12 + 17 + 9] == 17 + 3 &&
                  out[12 + 17 + 10 + 17 + 2] == 0x40 && !memcmp(out + n - 4, addr, 4),
              "an AAAA query: NSEC saying A only, the A record additional (%zu bytes)", n);
    }
    n = mdns_announce("remotefm1.local", addr, out, sizeof out);
    CHECK(n == 43 && out[2] == 0x84, "the announcement");
}

/* a firmware's block, made up: a few sines */
static void tone(int16_t *b, int blk, int seed)
{
    int i;
    for (i = 0; i < 128; i++) {
        const double t = (blk * 128 + i) / 44100.0;
        b[2 * i] = (int16_t)(9000 * sin(2 * M_PI * (220 + 30 * seed) * t));
        b[2 * i + 1] = (int16_t)(7000 * sin(2 * M_PI * (330 + 20 * seed) * t));
    }
}
/* the engine's mix for n blocks: two slots (levels, pans), optionally a send bus through an audio FX, then the limiter */
static void run_mix(int16_t *out, int nblk, int pan0, fx_inst_t *fx, int send)
{
    static mixer_t m;
    static bus_t bus;
    int16_t a[256], b[256], r[256];
    int k;
    memset(&m, 0, sizeof m);
    for (k = 0; k < nblk; k++) {
        mix_clear(&m, 128);
        if (fx)
            bus_clear(&bus, 128);
        tone(a, k, 0), tone(b, k, 1);
        mix_add_lp(&m, a, 128, 100, pan0);
        mix_add_lp(&m, b, 128, 127, 64);
        if (fx) {
            bus_add(&bus, a, 128, 100, pan0, send);
            bus_add(&bus, b, 128, 127, 64, send);
            bus_out(&bus, r, 128);
            fx_audio(fx, r, 128);
            mix_add_ret(&m, r, 128);
        }
        mix_out(&m, out + 256 * k, 128);
    }
}
static void fxstage(void)
{
    enum { N = 200 };
    static int16_t plain[256 * N], old[256 * N], with[256 * N], sent[256 * N];
    fx_lib_t *L = fx_scan("build/fxmods");
    fx_inst_t *fv = NULL, *arp = NULL;
    char err[300] = "";
    int i, fvi = -1, arpi = -1, diff = 0;
    {   /* pan 64 is mix_add exactly (the old mixer, bit for bit) */
        static mixer_t m;
        int16_t a[256], b[256];
        int k;
        memset(&m, 0, sizeof m);
        for (k = 0; k < N; k++) {
            mix_clear(&m, 128);
            tone(a, k, 0), tone(b, k, 1);
            mix_add(&m, a, 128, 100);
            mix_add(&m, b, 128, 127);
            mix_out(&m, old + 256 * k, 128);
        }
    }
    run_mix(plain, N, 64, NULL, 0);
    CHECK(!memcmp(plain, old, sizeof old), "pan 64 (centre): the mix is the old one, bit for bit");
    {
        float gl, gr;
        pan_gains(127, 0, &gl, &gr);
        CHECK(gl == 1.0f && gr == 0.0f, "pan 0: all left (%g %g)", gl, gr);
        pan_gains(127, 127, &gl, &gr);
        CHECK(gl == 0.0f && gr == 1.0f, "pan 127: all right (%g %g)", gl, gr);
    }
    for (i = 0; i < L->n[1]; i++)
        if (!strcmp(L->mod[1][i].id, "freeverb"))
            fvi = i;
    for (i = 0; i < L->n[0]; i++)
        if (!strcmp(L->mod[0][i].id, "arp"))
            arpi = i;
    CHECK(fvi >= 0 && arpi >= 0 && L->mod[1][fvi].np == 5 && L->mod[0][arpi].np == 4,
          "build/fxmods: freeverb (5 params) and arp (4) listed from their module.json (%d MIDI, %d audio)", L->n[0], L->n[1]);
    if (fvi < 0 || arpi < 0)
        return;
    {
        const rfm_move_fx_module_t *c = &L->core[1][fvi];
        int wet = -1, dry = -1;
        for (i = 0; i < c->nparams; i++) {
            if (!strcmp(L->mod[1][fvi].p[i].key, "wet"))
                wet = i;
            if (!strcmp(L->mod[1][fvi].p[i].key, "dry"))
                dry = i;
        }
        CHECK(wet >= 0 && dry >= 0 && c->params[wet].dry_wet && c->params[wet].def == 127 && c->params[dry].def == 0,
              "the core's view: freeverb's Wet defaults to 127 (100 %% wet for a send), Dry to 0");
        fv = fx_open(&L->mod[1][fvi], err, sizeof err);
        CHECK(fv != NULL, "freeverb opened (the probe in a child, dlopen, create_instance) %s", err);
        if (!fv)
            return;
        for (i = 0; i < c->nparams && i < 8; i++)
            fx_set(fv, i, c->params[i].def);
    }
    run_mix(with, N, 64, fv, 0);
    CHECK(!memcmp(with, plain, sizeof plain), "freeverb on a bus with every send at 0: the output is bit for bit the one without FX");
    run_mix(sent, N, 64, fv, 80);
    for (i = 0; i < 256 * N; i++)
        diff += sent[i] != plain[i];
    CHECK(diff > 256 * N / 2, "sends at 80: freeverb's return changes the output (%d of %d samples)", diff, 256 * N);
    fx_close(fv);
    arp = fx_open(&L->mod[0][arpi], err, sizeof err);
    CHECK(arp != NULL, "arp opened %s", err);
    if (arp) {
        const uint8_t on[3] = { 0x90, 60, 100 }, on2[3] = { 0x90, 64, 100 };
        uint8_t o[FX_MAX_OUT][3];
        int l[FX_MAX_OUT], k, notes = 0, st;
        char state[4096];
        fx_midi(arp, on, 3, o, l, FX_MAX_OUT);
        fx_midi(arp, on2, 3, o, l, FX_MAX_OUT);
        for (k = 0; k < 344; k++) {   /* one second */
            const int m = fx_tick(arp, 128, o, l, FX_MAX_OUT);
            for (i = 0; i < m; i++)
                notes += (o[i][0] & 0xF0) == 0x90 && o[i][2];
        }
        CHECK(notes >= 6, "arp: a held chord, ticked for a second: %d notes", notes);
        st = fx_state_get(arp, state, sizeof state);
        CHECK(st > 0, "arp: its state (get_param \"state\"): %d bytes", st);
        fx_close(arp);
    }
    {   /* a crash in create_instance: refused by the probe, this process unharmed */
        for (i = 0; i < L->n[1]; i++)
            if (!strcmp(L->mod[1][i].id, "crashfx")) {
                fx_inst_t *x = fx_open(&L->mod[1][i], err, sizeof err);
                CHECK(!x && strstr(err, "crashed"), "crashfx: refused, the host alive (%s)", err);
            }
    }
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "--fx-probe"))   /* (fx.c's crash probe runs this program) */
        return fx_probe_main(atoi(argv[2]), argv[3], argv[4]);
    printf("unit\n");
    json();
    arrays();
    sha();
    dsp();
    mdns();
    fxstage();
    printf("unit: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
