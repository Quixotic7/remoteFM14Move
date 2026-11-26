/* SPDX-License-Identifier: MPL-2.0 */
#define _GNU_SOURCE
/* Unit tests: the manifest reader (and the catalog index's arrays), SHA-256, the output stage (mix and limiter), the
 * mDNS answers, the FX stage (pan, the send buses, Schwung's freeverb and arp through fx.c: build/fxmods), the page's
 * file browser (files.c: the path rules, what may change, the zip's records and CRC), the screen reader protocol's words
 * (fmwords.c: fm1-reader's tests/words_test.mjs, case for case), a project's firmware copies and the states' files
 * (projmem.c: the names, the json's hash, the compare that finds the active state, a blob's firmware ids). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine_dsp.h"
#include "fx.h"
#include "mdns.h"
#include "minijson.h"
#include "sha256.h"
#include "catalog_key.h"
#include "catalog_test_key.h"
#include "catsig.h"
#include "ed25519.h"
#include "fsvc.h"
#include "files.h"
#include "fmwords.h"
#include "projmem.h"
#include "memsnap.h"

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

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
    {   /* DNS-SD: a browse for _apple-midi._udp.local (PTR), multicast; the PTR, then SRV, TXT and A as additional */
        static const uint8_t qp[] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 11, '_', 'a', 'p', 'p', 'l', 'e', '-', 'm', 'i',
                                      'd', 'i', 4, '_', 'u', 'd', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0, 0, 12, 0, 1 };
        const mdns_svc_t sv = { "RemoteFM1 Move", "_apple-midi._udp.local", 5004 };
        static const uint8_t inst[] = { 14, 'R', 'e', 'm', 'o', 't', 'e', 'F', 'M', '1', ' ', 'M', 'o', 'v', 'e' };
        size_t at;
        n = mdns_answer_svc(qp, sizeof qp, &sv, "remotefm1.local", addr, 5353, out, sizeof out, &uni);
        at = 12 + 24 + 10;   /* (the PTR's data: the instance's labels) */
        CHECK(n > 0 && uni == 0 && out[7] == 1 && out[11] == 3 && out[12 + 24 + 1] == 12 && out[12 + 24 + 2] == 0 &&
                  !memcmp(out + at, inst, sizeof inst) && !memcmp(out + n - 4, addr, 4),
              "DNS-SD: a browse for _apple-midi._udp: the PTR to \"RemoteFM1 Move\", SRV, TXT and A additional (%zu bytes)", n);
        {   /* the SRV right after the PTR: port 5004, the target remotefm1.local */
            const size_t srv = at + sizeof inst + 24, rd = srv + sizeof inst + 24 + 10;
            CHECK(n > rd + 6 && out[srv + sizeof inst + 24 + 1] == 33 && out[rd + 4] == 5004 >> 8 && out[rd + 5] == (5004 & 0xFF) &&
                      out[rd + 6] == 9 && !memcmp(out + rd + 7, "remotefm1", 9),
                  "DNS-SD: the SRV gives port 5004 on remotefm1.local");
        }
        n = mdns_answer_svc(qp, sizeof qp, &sv, "remotefm1.local", addr, 50000, out, sizeof out, &uni);
        CHECK(n > sizeof qp && uni == 1 && out[5] == 1 && !memcmp(out + 12, qp + 12, sizeof qp - 12),
              "DNS-SD: a plain resolver's browse: unicast, its question back");
        n = mdns_answer_svc(q, sizeof q, &sv, "remotefm1.local", addr, 5353, out, sizeof out, &uni);
        CHECK(n == 0, "DNS-SD: an A query is not the service's");
        n = mdns_announce_svc(&sv, "remotefm1.local", addr, out, sizeof out);
        CHECK(n > 0 && out[7] == 4 && out[11] == 1, "DNS-SD: the announcement (enum, PTR, SRV, TXT; A)");
    }
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
    {   /* (rfm1cs 0.5.0) the page plan of a multi-level ui_hierarchy (tests/fxmods/audio_fx/pagesfx/module.json) */
        const fx_mod_t *pm = NULL;
        const rfm_move_fx_module_t *pc = NULL;
        static const int16_t want[6][8] = { { 0, 1, -1, 2, 3, 4, 5, 6 }, { 7, 8, 9, 10, -1, -1, -1, -1 },
                                            { 11, 12, 0, -1, -1, -1, -1, -1 }, { 13, 14, -1, -1, -1, -1, -1, -1 },
                                            { 15, 16, 17, 18, 19, 20, 21, 22 }, { 23, -1, -1, -1, -1, -1, -1, -1 } };
        static const char *const names[6] = { "Pages", "Pages 2", "Advanced", "Sub", "Deep", "Deep 2" };
        int same = 1, k, ndrive = 0;
        for (i = 0; i < L->n[1]; i++)
            if (!strcmp(L->mod[1][i].id, "pagesfx"))
                pm = &L->mod[1][i], pc = &L->core[1][i];
        CHECK(pm && pm->np == 24 && pm->npg == 6 && pc->npages == 6 && pc->nparams == 24,
              "pagesfx: 24 parameters (a filepath left out) on 6 pages (%d, %d)", pm ? pm->np : -1, pm ? pm->npg : -1);
        if (pm && pm->npg == 6) {
            for (k = 0; k < 6; k++) {
                same &= !strcmp(pm->pg[k].name, names[k]) && !strcmp(pc->pages[k].name, names[k]);
                for (i = 0; i < 8; i++) {
                    same &= pm->pg[k].knob[i] == want[k][i] && pc->pages[k].knob[i] == want[k][i];
                    ndrive += pm->pg[k].knob[i] == 0;
                }
            }
            CHECK(same, "the plan: root's knobs with the null gap at knob 3, its knobs 9..10 then its params on no knob on "
                  "\"Pages 2\" (no trailing gaps), Advanced, then Sub (Advanced's link) before Deep, Deep's 9 on two pages");
            CHECK(ndrive == 2 && !strcmp(pm->p[0].key, "drive") && !strcmp(pm->p[8].key, "rate") && !strcmp(pm->p[14].key, "subB"),
                  "a key on two levels (drive, root and Advanced) is one parameter (index 0) on both pages; rate (a knob "
                  "named only in chain_params) is 8, Sub's subB 14");
            CHECK(pm->p[1].def == 2000 && pm->p[1].max == 20000 && !strcmp(pm->p[1].unit, "Hz") && pm->p[14].max == 1 &&
                  pm->p[14].def == 0.25,
                  "metadata from either: tone's default from the hierarchy (2000), its range and unit from chain_params");
            CHECK(!strcmp(pm->p[2].sname, "Mode") && !strcmp(pm->p[0].sname, "Drive") && !strcmp(pm->p[4].sname, "SWID") &&
                  !strcmp(pm->p[1].sname, "TFRQ") && !strcmp(pm->p[15].sname, "Deep1") && !strcmp(pm->p[11].sname, "DTime"),
                  "short labels: short_name as given (Mode), a short name as is (Drive), else squeezed (%s %s %s %s)",
                  pm->p[4].sname, pm->p[1].sname, pm->p[15].sname, pm->p[11].sname);
            CHECK(pc->params[0].lo == 0 && pc->params[0].hi == 100 && !strcmp(pc->params[0].unit, "%") && pc->params[0].decimals == 0 &&
                  pc->params[11].hi == 2 && pc->params[11].decimals == 2 && !strcmp(pc->params[11].unit, "s") &&
                  pc->params[8].decimals == 1 && pc->params[3].lo == -24 && !strcmp(pc->params[3].unit, "dB") &&
                  pc->params[2].nwords == 3 && pc->params[2].def == 1 && pc->params[13].nwords == 2,
                  "how values read: Drive 0..100 %%, Delay Time 0..2.00 s, Rate 1 decimal (its step), Gain -24..24 dB; "
                  "Filter Mode's words (default HP: 1), Sub On a bool (Off / On) (%g..%g %s %d; %g %d %s; %d; %g %s; %d %d %d)",
                  pc->params[0].lo, pc->params[0].hi, pc->params[0].unit, pc->params[0].decimals, pc->params[11].hi,
                  pc->params[11].decimals, pc->params[11].unit, pc->params[8].decimals, pc->params[3].lo, pc->params[3].unit,
                  pc->params[2].nwords, pc->params[2].def, pc->params[13].nwords);
        }
        if (pm && pm->np == 24) {   /* the value conversion, per type */
            char s[64];
            fx_wire(&pm->p[2], 2, s, sizeof s);
            CHECK(!strcmp(s, "BP") && fx_wire(&pm->p[2], 3, s, sizeof s) < 0, "an enum by text: 2 -> BP, 3 none");
            fx_wire(&pm->p[5], 3, s, sizeof s);
            CHECK(!strcmp(s, "3"), "an enum by index: 3 -> \"3\" (%s)", s);
            fx_wire(&pm->p[3], 0, s, sizeof s);
            same = !strcmp(s, "-24");
            fx_wire(&pm->p[3], 127, s, sizeof s);
            same &= !strcmp(s, "24");
            fx_wire(&pm->p[3], 64, s, sizeof s);
            CHECK(same && !strcmp(s, "0"), "an int: 0 / 64 / 127 -> -24 / 0 / 24 (%s)", s);
            fx_wire(&pm->p[8], 64, s, sizeof s);
            CHECK(!strcmp(s, "5.1000"), "a float snapped to its step: Rate 64 -> 5.1 (0.1..10, step 0.1: %s)", s);
            fx_wire(&pm->p[6], 127, s, sizeof s);
            CHECK(!strcmp(s, "1.0000") && fx_wire(&pm->p[6], RFM_MOVE_FX_UNSET, s, sizeof s) < 0,
                  "a float without a step: Color 127 -> 1.0; 255 (unset) nothing");
            CHECK(fx_to127(&pm->p[1], 2000) == 13 && fx_to127(&pm->p[2], 1) == 1 && fx_to127(&pm->p[3], 24) == 127,
                  "and back: Tone 2000 Hz -> 13, Filter Mode HP -> 1, Gain 24 -> 127");
        }
        {
            char a[16];
            fx_abbrev("Filter Envelope", a, sizeof a);
            same = !strcmp(a, "FENV");
            fx_abbrev("Osc 1 Octave", a, sizeof a);
            same &= !strcmp(a, "O1OCT");
            fx_abbrev("Resonance", a, sizeof a);
            CHECK(same && !strcmp(a, "RES"), "abbreviations: Filter Envelope FENV, Osc 1 Octave O1OCT (its 1 kept), Resonance RES");
        }
    }
    {   /* a crash in create_instance: refused by the probe, this process unharmed */
        for (i = 0; i < L->n[1]; i++)
            if (!strcmp(L->mod[1][i].id, "crashfx")) {
                fx_inst_t *x = fx_open(&L->mod[1][i], err, sizeof err);
                CHECK(!x && strstr(err, "crashed"), "crashfx: refused, the host alive (%s)", err);
            }
    }
}

/* ------------------------------------------------------------------ Ed25519 and the index's signature --- */
static int unhex(const char *h, uint8_t *out, int cap)
{
    int n = 0;
    while (h[0] && h[1] && n < cap) {
        unsigned v;
        sscanf(h, "%2x", &v);
        out[n++] = (uint8_t)v;
        h += 2;
    }
    return n;
}

static void ed25519(void)
{
    /* RFC 8032 7.1: TEST 1, 2, 3 and TEST SHA(abc) (whose message is SHA-512("abc"): ours, checked) */
    static const char *V[4][3] = {
        { "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
          "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
        { "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
          "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
        { "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
          "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
        { "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf", NULL,
          "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704" },
    };
    uint8_t pk[32], sig[64], msg[64], h[64], want[64];
    sha512_t s;
    int i, n;
    sha512_init(&s);
    sha512_update(&s, "abc", 3);
    sha512_final(&s, h);
    unhex("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
          want, 64);
    CHECK(!memcmp(h, want, 64), "SHA-512 of abc");
    {   /* 1,000,000 a's, fed in odd pieces (FIPS 180-2's vector) */
        static uint8_t a[1001];
        memset(a, 'a', sizeof a);
        sha512_init(&s);
        for (i = 0; i < 1000; i++)
            sha512_update(&s, a, i % 2 ? 999 : 1001);
        sha512_final(&s, h);
        unhex("e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b",
              want, 64);
        CHECK(!memcmp(h, want, 64), "SHA-512 of a million a's, in uneven pieces");
    }
    for (i = 0; i < 4; i++) {
        unhex(V[i][0], pk, 32);
        unhex(V[i][2], sig, 64);
        if (V[i][1])
            n = unhex(V[i][1], msg, 64);
        else {
            sha512_init(&s);
            sha512_update(&s, "abc", 3);
            sha512_final(&s, msg);
            n = 64;
        }
        CHECK(ed25519_verify(sig, msg, (size_t)n, pk) == 0, "RFC 8032 %s: verified", i < 3 ? (const char *[]){ "TEST 1", "TEST 2", "TEST 3" }[i] : "TEST SHA(abc)");
        sig[5] ^= 1;
        CHECK(ed25519_verify(sig, msg, (size_t)n, pk) != 0, "  .. one bit of R changed: refused");
        sig[5] ^= 1;
        sig[40] ^= 0x10;
        CHECK(ed25519_verify(sig, msg, (size_t)n, pk) != 0, "  .. one bit of S changed: refused");
        sig[40] ^= 0x10;
        if (n) {
            msg[0] ^= 0x80;
            CHECK(ed25519_verify(sig, msg, (size_t)n, pk) != 0, "  .. another message: refused");
            msg[0] ^= 0x80;
        }
        pk[3] ^= 4;
        CHECK(ed25519_verify(sig, msg, (size_t)n, pk) != 0, "  .. another key: refused");
        pk[3] ^= 4;
    }
    {   /* S + L (the same point, a non-canonical S): refused */
        static const uint8_t L[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9,
                                       0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };
        int c = 0, k;
        unhex(V[0][0], pk, 32);
        unhex(V[0][2], sig, 64);
        for (k = 0; k < 32; k++) {
            const int x = sig[32 + k] + L[k] + c;
            sig[32 + k] = (uint8_t)x;
            c = x >> 8;
        }
        CHECK(ed25519_verify(sig, msg, 0, pk) != 0, "S + L (malleated): refused (S < L required)");
    }
}

static uint8_t *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    uint8_t *b = NULL;
    long len;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len >= 0 && (b = malloc((size_t)len + 1)) && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *n = (size_t)len;
    return b;
}

static void catsig(void)
{
    const uint8_t *const *keys;
    const int nk = catsig_keys(&keys);
    size_t n = 0, sn = 0;
    uint8_t *idx = slurp("build/testcat/index.json", &n), *sig = slurp("build/testcat/index.json.sig", &sn);
    char why[160] = "";
    CHECK(nk == 2 && !memcmp(keys[0], CAT_SIGNING_KEY, 32) && !memcmp(keys[1], CAT_TEST_KEY, 32) &&
              memcmp(CAT_SIGNING_KEY, CAT_TEST_KEY, 32),
          "the test build trusts the release key and the test key (2 keys, not the same)");
    if (!idx || !sig) {
        CHECK(0, "no build/testcat/index.json(.sig): make testcat");
        free(idx);
        free(sig);
        return;
    }
    CHECK(catsig_check(idx, n, sig, sn, why, sizeof why) == 0, "the test catalog's index: its signature good (%zu bytes)", n);
    CHECK(catsig_check_key(CAT_SIGNING_KEY, idx, n, sig, sn, why, sizeof why) != 0,
          "  .. and not by the release key (the tests never use it): %s", why);
    idx[n / 2] ^= 1;
    CHECK(catsig_check(idx, n, sig, sn, why, sizeof why) != 0 && strstr(why, "bad signature"),
          "one bit of the index changed: refused (%s)", why);
    idx[n / 2] ^= 1;
    CHECK(catsig_check(idx, n - 1, sig, sn, why, sizeof why) != 0, "the index one byte short: refused");
    CHECK(catsig_check(idx, n, NULL, 0, why, sizeof why) != 0 && strstr(why, "no signature"), "no .sig: refused (%s)", why);
    CHECK(catsig_check(idx, n, (const uint8_t *)"AAAA", 4, why, sizeof why) != 0 && strstr(why, "damaged"),
          "a .sig that is not 64 bytes: refused (%s)", why);
    sig[10] = sig[10] == 'A' ? 'B' : 'A';
    CHECK(catsig_check(idx, n, sig, sn, why, sizeof why) != 0, "a .sig changed: refused (%s)", why);
    free(idx);
    free(sig);
}


/* ------------------------------------------------------------------ a Fantasy CFW's file API (fsvc.c) --- */
/* (CHECK evaluates its condition twice: these conditions make requests, so once here) */
#define CK(c, ...)                                                                                                  \
    do {                                                                                                            \
        const int c_ = (c);                                                                                         \
        CHECK(c_, __VA_ARGS__);                                                                                     \
    } while (0)
static uint8_t g_mem[2u << 20];   /* a module's memory: paths at 0x100, buffers at 0x10000 */
#define PATHAT 0x100u
#define BUFAT 0x10000u
static int32_t req(fsvc_t *s, int op, const char *path, int64_t off, uint32_t buf, int32_t len)
{
    const size_t n = strlen(path);
    memcpy(g_mem + PATHAT, path, n);
    return fsvc_request(s, g_mem, sizeof g_mem, op, PATHAT, (int32_t)n, off, buf, len);
}
static uint32_t g_ms;
/* a request made, then pumped until answered (inline: one ms later): its result */
static int32_t run(fsvc_t *s, int op, const char *path, int64_t off, int32_t len)
{
    int32_t id = req(s, op, path, off, BUFAT, len), r = FSV_PENDING;
    int k;
    if (id <= 0)
        return id;
    for (k = 0; k < 4 && r == FSV_PENDING; k++) {
        fsvc_pump(s, g_mem, sizeof g_mem, ++g_ms);
        r = fsvc_poll(s, id);
    }
    return r;
}
static void put(const char *root, const char *rel, const char *text)
{
    char p[512];
    FILE *f;
    snprintf(p, sizeof p, "%s/%s", root, rel);
    f = fopen(p, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}
/* the I/O thread stand-in: requests handed to it, served when the test says */
static void *g_io[64];
static int g_nio;
static void grab(void *io) { g_io[g_nio++] = io; }
static void *serve_all(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < g_nio; i++)
        fsvc_serve(g_io[i]);
    g_nio = 0;
    return NULL;
}

static void fsapi(void)
{
    char tmpl[] = "/tmp/rfm14fs.XXXXXX", root[256], p[512], out[1024];
    const uint8_t elastic[3] = { FSV_R, 0, FSV_R | FSV_W }, ripple[3] = { 0, FSV_R, FSV_R | FSV_W };
    fsvc_t *s;
    int32_t r, id, ids[40];
    int area, top, i;
    printf("fs (rfm1_host_1)\n");
    if (!mkdtemp(tmpl))
        return;
    snprintf(root, sizeof root, "%s/RemoteFM1", tmpl);
    mkdir(root, 0755);
    snprintf(p, sizeof p, "%s/audio", root), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/audio/kit", root), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/code", root), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/code/fscart", root), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/code/fscart/droplets", root), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/code/other", root), mkdir(p, 0755);
    put(root, "audio/kick.wav", "RIFF....WAVEkick");
    put(root, "audio/b.wav", "b");
    put(root, "audio/.hidden.wav", "h");
    put(root, "code/fscart/droplets/a.lua", "print('a')");
    put(root, "code/other/secret.lua", "secret");
    put(tmpl, "outside.txt", "outside!");
    snprintf(p, sizeof p, "%s/outside.txt", tmpl);
    snprintf(out, sizeof out, "%s/code/fscart/escape", root);
    symlink(p, out);
    snprintf(out, sizeof out, "%s/audio/out", root);
    symlink(tmpl, out);

    /* the paths: section 3's rules, the iOS / Android hosts' stricter ones */
    {
        static const char *bad[] = { "/audio/../code/other/secret.lua", "/code/../other/secret.lua", "/code/./droplets/a.lua",
                                     "//audio/kick.wav", "/audio//kick.wav", "/audio/", "/etc/passwd", "/", "audio/kick.wav",
                                     "/audiox/kick.wav", "/audio\\kick.wav", "/data/..", "/Audio/kick.wav" };
        int nbad = 0;
        for (i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++)
            nbad += fsvc_map(root, "fscart", (const uint8_t *)bad[i], (uint32_t)strlen(bad[i]), out, sizeof out, &area,
                             &top) == FSV_EACCES;
        CK(nbad == (int)(sizeof bad / sizeof bad[0]), "13 escapes and malformed paths refused (.., ., //, a trailing /, "
              "outside the three, relative, \\, case): %d", nbad);
        CK(fsvc_map(root, "fscart", (const uint8_t *)"/audio/k\0x", 10, out, sizeof out, &area, &top) == FSV_EACCES &&
              fsvc_map(root, "fscart", (const uint8_t *)"/audio/\xc0\xaf", 9, out, sizeof out, &area, &top) == FSV_EACCES,
              "a NUL inside, an overlong UTF-8 '/': refused");
        CK(!fsvc_map(root, "fscart", (const uint8_t *)"/code/droplets/a.lua", 20, out, sizeof out, &area, &top) &&
              area == FSV_CODE && !top && strstr(out, "/code/fscart/droplets/a.lua"), "/code/droplets/a.lua -> code/fscart/droplets/a.lua");
        CK(!fsvc_map(root, "fscart", (const uint8_t *)"/data", 5, out, sizeof out, &area, &top) && area == FSV_DATA && top,
              "/data: the cart's data/fscart itself");
        CK(!fsvc_map(root, "x", (const uint8_t *)"/audio/kit/\xc3\xa9.wav", 15, out, sizeof out, &area, &top) && area == FSV_AUDIO,
              "UTF-8 names (/audio/kit/\\u00e9.wav)");
    }

    fsvc_set_root(NULL);
    s = fsvc_new("fscart", ripple);
    CK(req(s, FSV_READ, "/code/droplets/a.lua", 0, BUFAT, 16) == FSV_ENOFOLDER, "no folder: FS_ENOFOLDER");
    fsvc_release(s);

    fsvc_set_root(root);
    fsvc_set_dispatch(NULL);   /* inline: answered in the pump one device ms later (rfm1_run --folder) */
    s = fsvc_new("fscart", ripple);
    snprintf(p, sizeof p, "%s/data/fscart", root);
    {
        struct stat st;
        CK(stat(p, &st) == 0 && S_ISDIR(st.st_mode), "the cart's data/fscart made as it is made");
    }
    memset(g_mem + BUFAT, 0, 64);
    r = run(s, FSV_READ, "/code/droplets/a.lua", 0, 100);
    CK(r == 10 && !memcmp(g_mem + BUFAT, "print('a')", 10), "READ /code/droplets/a.lua: its own code/fscart/ (%d)", r);
    r = run(s, FSV_READ, "/code/droplets/a.lua", 6, 100);
    CK(r == 4 && !memcmp(g_mem + BUFAT, "'a')", 4), "READ at an offset (%d)", r);
    CK(run(s, FSV_READ, "/code/droplets/a.lua", 10, 100) == 0, "READ at the end: 0");
    CK(run(s, FSV_READ, "/code/nothing.lua", 0, 16) == FSV_ENOENT, "READ a missing file: FS_ENOENT");
    CK(run(s, FSV_READ, "/code/droplets", 0, 16) == FSV_EIO, "READ a folder: FS_EIO");
    CK(run(s, FSV_READ, "/code/escape", 0, 16) == FSV_EACCES, "a symlink out of the folder (code/fscart/escape): FS_EACCES");
    CK(req(s, FSV_READ, "/audio/kick.wav", 0, BUFAT, 16) == FSV_EACCES, "Ripple's folders (code r, data rw): /audio refused");
    CK(req(s, FSV_WRITE, "/code/x.lua", -1, BUFAT, 4) == FSV_EACCES && req(s, FSV_MKDIR, "/data", 0, 0, 0) == FSV_EACCES &&
          req(s, FSV_DELETE, "/data", 0, 0, 0) == FSV_EACCES, "writes: not in /code, not /data itself");
    memcpy(g_mem + BUFAT, "hello world", 11);
    CK(run(s, FSV_WRITE, "/data/out.txt", -1, 11) == 11, "WRITE /data/out.txt (truncate): 11");
    memcpy(g_mem + BUFAT, "W", 1);
    CK(run(s, FSV_WRITE, "/data/out.txt", 6, 1) == 1, "WRITE at an offset");
    snprintf(p, sizeof p, "%s/data/fscart/out.txt", root);
    {
        FILE *f = fopen(p, "rb");
        char b[32] = "";
        if (f) {
            if (!fgets(b, sizeof b, f))
                b[0] = 0;
            fclose(f);
        }
        CK(!strcmp(b, "hello World"), "data/fscart/out.txt: \"%s\"", b);
    }
    CK(run(s, FSV_MKDIR, "/data/sub", 0, 0) == 0 && run(s, FSV_MKDIR, "/data/sub", 0, 0) == 0, "MKDIR (there already: fine)");
    CK(run(s, FSV_WRITE, "/data/sub/x", -1, 1) == 1 && run(s, FSV_DELETE, "/data/sub", 0, 0) == FSV_EIO,
          "DELETE a folder not empty: FS_EIO");
    CK(run(s, FSV_DELETE, "/data/sub/x", 0, 0) == 0 && run(s, FSV_DELETE, "/data/sub", 0, 0) == 0 &&
          run(s, FSV_DELETE, "/data/sub", 0, 0) == FSV_ENOENT, "DELETE a file, then the empty folder; again: FS_ENOENT");
    r = run(s, FSV_STAT, "/data/out.txt", 0, 16);
    {
        uint64_t sz = 0;
        uint32_t dir = 9;
        memcpy(&sz, g_mem + BUFAT, 8), memcpy(&dir, g_mem + BUFAT + 12, 4);
        CK(r == 16 && sz == 11 && dir == 0, "STAT: 16, size 11, not a folder");
    }
    fsvc_release(s);

    /* Elasticat's folders: audio r, data rw */
    s = fsvc_new("elasticat", elastic);
    CK(req(s, FSV_READ, "/code/x", 0, BUFAT, 4) == FSV_EACCES, "Elasticat's folders (audio r, data rw): /code refused");
    r = run(s, FSV_LIST, "/audio", 0, 256);
    {
        char got[256] = "";
        uint32_t at = 0;
        for (i = 0; i < r; i++) {
            const uint8_t d = g_mem[BUFAT + at], nl = g_mem[BUFAT + at + 1];
            snprintf(got + strlen(got), sizeof got - strlen(got), "%s%.*s%s", i ? " " : "", nl, (char *)g_mem + BUFAT + at + 2, d ? "/" : "");
            at += 2u + nl;
        }
        CK(r == 4 && !strcmp(got, "b.wav kick.wav kit/ out/"), "LIST /audio: sorted, no hidden files, folders marked (%s)", got);
    }
    r = run(s, FSV_LIST, "/audio", 2, 256);
    CK(r == 2 && g_mem[BUFAT + 1] == 3 && !memcmp(g_mem + BUFAT + 2, "kit", 3), "LIST from entry 2 on (%d)", r);
    CK(run(s, FSV_LIST, "/audio", 4, 256) == 0, "LIST past the last: 0 (the end)");
    CK(run(s, FSV_LIST, "/audio", 0, 9) == 1 && run(s, FSV_LIST, "/audio", 1, 4) == FSV_EIO,
          "LIST: an entry never split; a buffer too small for the next one: FS_EIO, not the end");
    CK(run(s, FSV_READ, "/audio/out/outside.txt", 0, 16) == FSV_EACCES, "a symlinked folder out (audio/out): FS_EACCES");
    {   /* the recorder's takes: audio/rec a link to the Move's sample library (out of the folder), followed once it is
         * named (fsvc_set_audio_link); any other link out still refused */
        char lib[600];
        snprintf(lib, sizeof lib, "%s/Samples", tmpl), mkdir(lib, 0755);
        snprintf(lib, sizeof lib, "%s/Samples/RemoteFM1", tmpl), mkdir(lib, 0755);
        put(tmpl, "Samples/RemoteFM1/take.wav", "RIFF....WAVEtake");
        snprintf(out, sizeof out, "%s/audio/rec", root);
        symlink(lib, out);
        CK(run(s, FSV_READ, "/audio/rec/take.wav", 0, 16) == FSV_EACCES, "audio/rec a link out, not named: FS_EACCES");
        fsvc_set_audio_link("rec");
        memset(g_mem + BUFAT, 0, 32);
        r = run(s, FSV_READ, "/audio/rec/take.wav", 0, 16);
        CK(r == 16 && !memcmp(g_mem + BUFAT, "RIFF....WAVEtake", 16), "fsvc_set_audio_link(\"rec\"): READ /audio/rec/take.wav "
              "through the link (%d)", r);
        r = run(s, FSV_LIST, "/audio/rec", 0, 256);
        CK(r == 1 && g_mem[BUFAT + 1] == 8 && !memcmp(g_mem + BUFAT + 2, "take.wav", 8), "LIST /audio/rec: its files (%d)", r);
        CK(run(s, FSV_READ, "/audio/out/outside.txt", 0, 16) == FSV_EACCES, "audio/out (another link out): still FS_EACCES");
        CK(files_no_links(root, "audio/rec") != 0, "the page's browser: audio/rec is a link, not followed (files_no_links)");
        {   /* the page's browser follows that one link once named (files_follow_link): inside its target only */
            char esc[700], *j;
            uint64_t zl = 0;
            const char *zw = NULL;
            files_zip_t *z;
            snprintf(esc, sizeof esc, "%s/Samples/RemoteFM1/2026-11-24", tmpl), mkdir(esc, 0755);
            snprintf(esc, sizeof esc, "%s/Samples/RemoteFM1/esc", tmpl);
            symlink(tmpl, esc);   /* (a link inside the target, pointing out of it) */
            files_follow_link(root, "audio/rec");
            CK(files_no_links(root, "audio/rec") == 0 && files_no_links(root, "audio/rec/take.wav") == 0 &&
                   files_no_links(root, "audio/rec/2026-11-24") == 0 && files_no_links(root, "audio/rec/2026-11-24/new.wav") == 0,
               "files_follow_link(audio/rec): the link, its files, a folder in it, a new name in it: followed (inside its target)");
            CK(files_no_links(root, "audio/out") != 0 && files_no_links(root, "audio/out/outside.txt") != 0,
               "files_follow_link: another link (audio/out) still refused");
            CK(files_no_links(root, "audio/rec/..") != 0 && files_no_links(root, "audio/rec/../../x") != 0 &&
                   files_no_links(root, "audio/rec/2026-11-24/../take.wav") != 0 && files_no_links(root, "audio/../audio") != 0,
               "files_follow_link: no .. anywhere (out of the target through the link's parent)");
            CK(files_no_links(root, "audio/rec/esc") != 0 && files_no_links(root, "audio/rec/esc/Samples") != 0,
               "files_follow_link: a link inside the target pointing out: refused");
            j = files_list_json(root, "audio");
            CK(j && strstr(j, "{\"name\":\"rec\",\"size\":0,") && strstr(strstr(j, "\"name\":\"rec\""), "\"dir\":true,\"link\":true}") &&
                   !strstr(j, "\"name\":\"out\""),
               "files_list_json(audio): rec a folder (\"link\":true); audio/out not listed");
            free(j);
            j = files_list_json(root, "audio/rec");
            CK(j && strstr(j, "\"name\":\"take.wav\",\"size\":16,") && strstr(j, "\"name\":\"2026-11-24\"") && !strstr(j, "\"esc\""),
               "files_list_json(audio/rec): the takes; the link out in it not listed");
            free(j);
            {
                char w[256];
                CK(files_refuse("audio/rec", FILES_DELETE, NULL, w, sizeof w) && files_refuse("audio/rec", FILES_RENAME, NULL, w, sizeof w) &&
                       !files_refuse("audio/rec/take.wav", FILES_DELETE, NULL, w, sizeof w) &&
                       !files_refuse("audio/rec/take.wav", FILES_RENAME, NULL, w, sizeof w),
                   "files_refuse: audio/rec itself stays; a take in it deletes and renames");
            }
            z = files_zip_plan(root, "audio/rec", &zl, &zw);
            CK(z && zl > 16, "files_zip_plan(audio/rec): the takes as a zip (%llu bytes)", (unsigned long long)zl);
            files_zip_free(z);
            files_follow_link(NULL, NULL);
            CK(files_no_links(root, "audio/rec") != 0, "files_follow_link(NULL): no link followed again");
            unlink(esc);
            snprintf(esc, sizeof esc, "%s/Samples/RemoteFM1/2026-11-24", tmpl), rmdir(esc);
        }
        fsvc_set_audio_link(NULL);
        unlink(out);
    }

    /* the limits */
    CK(req(s, FSV_READ, "/audio/kick.wav", 0, BUFAT, FSV_MAXLEN + 1) == FSV_EIO, "more than 1 MiB a request: FS_EIO");
    CK(req(s, FSV_READ, "/audio/kick.wav", 0, (uint32_t)sizeof g_mem - 8, 16) == FSV_EIO, "a buffer past the memory: FS_EIO");
    CK(req(s, FSV_STAT, "/audio/kick.wav", 0, BUFAT, 8) == FSV_EIO, "STAT with less than 16 bytes: FS_EIO");
    CK(req(s, 9, "/audio/kick.wav", 0, BUFAT, 8) == FSV_EIO && req(s, FSV_READ, "/audio/kick.wav", -5, BUFAT, 8) == FSV_EIO,
          "an unknown op, a negative offset: FS_EIO");
    for (i = 0; i < 32; i++)
        ids[i] = req(s, FSV_READ, "/audio/kick.wav", 0, BUFAT + 64u * (uint32_t)i, 16);
    id = req(s, FSV_READ, "/audio/kick.wav", 0, BUFAT, 16);
    CK(ids[0] > 0 && ids[31] > 0 && id == FSV_EBUSY, "32 in flight; the 33rd: FS_EBUSY");
    CK(fsvc_poll(s, ids[0]) == FSV_PENDING, "made in this call: pending (answered between calls only)");
    fsvc_pump(s, g_mem, sizeof g_mem, g_ms);   /* (the same ms: not due yet) */
    CK(fsvc_poll(s, ids[0]) == FSV_PENDING, "inline: not before the next device ms");
    memset(g_mem + BUFAT, 0, 64);
    fsvc_cancel(s, ids[1]);
    fsvc_pump(s, g_mem, sizeof g_mem, ++g_ms);
    CK(fsvc_poll(s, ids[0]) == 16 && !memcmp(g_mem + BUFAT, "RIFF....WAVEkick", 16), "one ms later: answered, its bytes in");
    CK(fsvc_poll(s, ids[0]) == FSV_EIO, "a result is given once: the id is free");
    CK(fsvc_poll(s, ids[1]) == FSV_ECANCELED && g_mem[BUFAT + 64] == 0, "cancelled: FS_ECANCELED, its buffer not written");
    for (i = 2; i < 32; i++)
        fsvc_poll(s, ids[i]);
    CK(req(s, FSV_STAT, "/audio/kick.wav", 0, BUFAT, 16) > 0, "polled: room again");
    {
        const int32_t c0 = fsvc_changed(s);
        fsvc_bump_changed();
        CK(fsvc_changed(s) == c0 + 1, "fs_changed moves when the folder changes (the page's uploads)");
    }
    fsvc_release(s);

    /* on another thread (the host's file thread): answered only in the pump, between two calls */
    fsvc_set_dispatch(grab);
    s = fsvc_new("elasticat", elastic);
    memset(g_mem + BUFAT, 0, 64);
    id = req(s, FSV_READ, "/audio/kick.wav", 0, BUFAT, 16);
    fsvc_pump(s, g_mem, sizeof g_mem, ++g_ms);   /* (sent) */
    CK(g_nio == 1, "the request went to the I/O thread at the pump");
    {
        pthread_t t;
        pthread_create(&t, NULL, serve_all, NULL);
        pthread_join(t, NULL);
    }
    CK(fsvc_poll(s, id) == FSV_PENDING && g_mem[BUFAT] == 0, "done on the I/O thread: still pending, nothing written, "
          "until the next pump");
    fsvc_pump(s, g_mem, sizeof g_mem, ++g_ms);
    CK(fsvc_poll(s, id) == 16 && !memcmp(g_mem + BUFAT, "RIFF", 4), "the pump: answered, its bytes in the memory");
    id = req(s, FSV_READ, "/audio/kick.wav", 0, BUFAT, 16);
    fsvc_pump(s, g_mem, sizeof g_mem, ++g_ms);
    fsvc_release(s);   /* (the instance freed while its request runs: kept until it returns) */
    serve_all(NULL);
    CK(1, "an instance released while the I/O thread holds its request: freed after (no crash)");
    fsvc_set_dispatch(NULL);
    {
        uint8_t acc[3];
        fsvc_access("{\"manifest\":{\"id\":\"x\",\"folders\":{\"audio\":\"r\",\"data\":\"rw\"}}}", acc);
        CK(acc[0] == FSV_R && acc[1] == 0 && acc[2] == (FSV_R | FSV_W), "the manifest's folders -> access");
    }
}

/* the page's file browser (files.h): the path rules, what may change, %-unescaping, the zip */
static void filesapi(void)
{
    static const char *const GOOD[] = { "", "audio", "audio/Amen loop (1).wav", "data/elasticat/rec/rec-0001.wav",
                                        "audio/caf\xc3\xa9 \xe2\x99\xab.wav", "states/choralroot/state-00.bin" };
    static const char *const BAD[] = { "/audio", "audio/", "audio//x", "audio/../x", "..", "audio/./x", "./audio",
                                       "audio/.hidden", "audio/a\\b", "audio/a\x01" "b", "audio/\xc3", "audio/\xc0\xaf",
                                       "audio/\xed\xa0\x80", "audio/\xf4\x90\x80\x80" };
    static const char *const NEWBAD[] = { "audio/a:b", "audio/a*b", "audio/a?b", "audio/a\"b", "audio/a<b", "audio/a|b",
                                          "audio/ x", "audio/x ", "audio/x." };
    const char *why = NULL;
    char longp[1100], seg[300], out[64], w[300];
    size_t i;
    int ok = 1;
    files_use_t u;
    for (i = 0; i < sizeof GOOD / sizeof GOOD[0]; i++)
        ok &= files_rel_ok(GOOD[i], 0, &why) == 0;
    CHECK(ok, "files: good paths (the root, spaces, brackets, UTF-8 names)");
    ok = 1;
    for (i = 0; i < sizeof BAD / sizeof BAD[0]; i++)
        if (files_rel_ok(BAD[i], 0, &why) == 0)
            ok = 0, printf("    (taken: %s)\n", BAD[i]);
    CHECK(ok, "files: refused: absolute, trailing /, //, .., ., hidden, \\, control characters, bad UTF-8 (cut, overlong, surrogate, past U+10FFFF)");
    ok = 1;
    for (i = 0; i < sizeof NEWBAD / sizeof NEWBAD[0]; i++)
        ok &= files_rel_ok(NEWBAD[i], 0, &why) == 0 && files_rel_ok(NEWBAD[i], 1, &why) != 0;
    CHECK(ok && files_rel_ok("audio/a:b/c", 1, &why) == 0 && files_rel_ok("", 1, &why) != 0,
          "files: a new name: no : * ? \" < > |, no leading or trailing space, no trailing . (only the new segment)");
    memset(seg, 'a', 256), seg[256] = 0;
    snprintf(longp, sizeof longp, "audio/%s", seg);
    ok = files_rel_ok(longp, 0, &why) != 0;
    seg[255] = 0;
    snprintf(longp, sizeof longp, "audio/%s", seg);
    ok &= files_rel_ok(longp, 0, &why) == 0;
    memset(longp, 'a', 1030), longp[1030] = 0;
    for (i = 100; i < 1030; i += 100)
        longp[i] = '/';
    CHECK(ok && files_rel_ok(longp, 0, &why) != 0, "files: a name over 255 bytes, a path over 1024: refused");
    CHECK(files_unescape("a%20b+c%2Fd%C3%A9", 17, out, sizeof out) == 0 && !strcmp(out, "a b c/d\xc3\xa9") &&
          files_unescape("a%00b", 5, out, sizeof out) != 0 && files_unescape("a%2", 3, out, sizeof out) != 0 &&
          files_unescape("a%zz", 4, out, sizeof out) != 0, "files: %%-unescaping (UTF-8 kept; %%00, a cut or bad escape refused)");
    /* what may change */
    memset(&u, 0, sizeof u);
    u.open_project = 2;
    snprintf(u.ids[0], 32, "elasticat");
    snprintf(u.ids[2], 32, "choralroot");
#define NO(rel, op) (files_refuse(rel, op, &u, w, sizeof w) != NULL)
    CHECK(!NO("audio/x.wav", FILES_PUT) && !NO("audio/x.wav", FILES_REPLACE) && !NO("audio/sub", FILES_MKDIR) &&
          !NO("audio/x.wav", FILES_DELETE) && !NO("audio/x.wav", FILES_RENAME) && !NO("code/ripple/d.lua", FILES_DELETE) &&
          !NO("states/felucca/state-00.bin", FILES_DELETE) && !NO("move/projects/move-project-01.rfmmove", FILES_DELETE) &&
          !NO("data/ripple", FILES_DELETE), "files: audio/, code/, data/, states/, move/projects/ take changes");
    CHECK(NO("firmwares/felucca-1.8.2.rfm1m", FILES_DELETE) && NO("firmwares/x.rfm1m", FILES_PUT) && NO("firmwares/a", FILES_MKDIR) &&
          NO("catalog/index.json", FILES_REPLACE) && NO("catalog-url", FILES_REPLACE) && NO("newdir", FILES_MKDIR) &&
          NO("", FILES_DELETE) && strstr((files_refuse("firmwares/x.rfm1m", FILES_PUT, &u, w, sizeof w), w), "signed catalog"),
          "files: firmwares/ and catalog/ read-only (a firmware: only the signed catalog installs one), the root too");
    CHECK(NO("audio", FILES_DELETE) && NO("states", FILES_RENAME) && NO("move/projects", FILES_DELETE) && NO("move/settings.bin", FILES_REPLACE) &&
          NO("move/library.rmvl", FILES_DELETE) && NO("move/open-project", FILES_DELETE) && NO("move", FILES_DELETE),
          "files: the five folders stay; move/'s settings, library and open-project are the core's");
    CHECK(NO("move/projects/move-project-03.rfmmove", FILES_DELETE) && NO("move/projects/move-project-03.fx.json", FILES_RENAME) &&
          NO("move/projects/move-project-03.rfmmove", FILES_REPLACE) && !NO("move/projects/move-project-04.rfmmove", FILES_DELETE),
          "files: the open project's files (project 3) stay");
    CHECK(NO("move/projects/move-project-03.fw1.bin", FILES_DELETE) && NO("move/projects/move-project-03.fw3.json", FILES_REPLACE) &&
          NO("move/projects/move-project-03.fw2.bin", FILES_RENAME) && !NO("move/projects/move-project-04.fw1.bin", FILES_DELETE) &&
          !NO("move/projects/move-project-03.fw2.bin", FILES_PUT) && NO("move/projects/move-project-03.fw2.mem", FILES_DELETE) &&
          !NO("move/projects/move-project-04.fw2.mem", FILES_DELETE),
          "files: the open project's firmware copies (rfm1cs 0.13.0) stay too; another project's change");
    CHECK(NO("states/elasticat/state-00.bin", FILES_DELETE) && NO("states/elasticat/new.bin", FILES_PUT) && NO("states/choralroot", FILES_RENAME) &&
          strstr((files_refuse("states/elasticat/x", FILES_PUT, &u, w, sizeof w), w), "slot 1") &&
          NO("data/elasticat/settings.bin", FILES_DELETE) && NO("data/elasticat", FILES_RENAME) && NO("data/elasticat/x", FILES_REPLACE) &&
          !NO("data/elasticat/new.bin", FILES_PUT) && !NO("data/elasticat/new", FILES_MKDIR) && NO("code/choralroot/x", FILES_DELETE),
          "files: a running firmware's states/<id>/ stay; its data/<id>/, code/<id>/ take new files, no delete, rename or replace");
    CHECK(!NO("data/elasticatx/a", FILES_DELETE) && !NO("states/elasticat2", FILES_DELETE),
          "files: a name that only starts like a running id is not it");
#undef NO
    /* the zip: CRC-32's check value; a folder's archive read in odd pieces is the same as in one */
    CHECK(files_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u, "files: CRC-32 (123456789 -> cbf43926)");
    {
        char root[] = "/tmp/rfm14-files-XXXXXX", p[256];
        uint64_t len = 0, len2 = 0;
        files_zip_t *z;
        uint8_t *a = NULL, *b = NULL;
        long n, ta = 0, tb = 0;
        FILE *f;
        if (mkdtemp(root)) {
            snprintf(p, sizeof p, "%s/states", root), mkdir(p, 0755);
            snprintf(p, sizeof p, "%s/states/x", root), mkdir(p, 0755);
            snprintf(p, sizeof p, "%s/states/empty", root), mkdir(p, 0755);
            snprintf(p, sizeof p, "%s/states/x/state-00.bin", root);
            if ((f = fopen(p, "wb"))) { for (i = 0; i < 70000; i++) fputc((int)(i * 7 & 255), f); fclose(f); }
            snprintf(p, sizeof p, "%s/states/.hidden", root);
            if ((f = fopen(p, "wb"))) { fputs("no", f); fclose(f); }
            snprintf(p, sizeof p, "%s/states/link", root);
            if (symlink("/etc", p) != 0) {}
            z = files_zip_plan(root, "states", &len, &why);
            a = malloc(len + 16), b = malloc(len + 16);
            while (z && a && (n = files_zip_read(z, a + ta, 65536)) > 0)
                ta += n;
            files_zip_free(z);
            z = files_zip_plan(root, "states", &len2, &why);
            while (z && b && (n = files_zip_read(z, b + tb, 7)) > 0)
                tb += n;
            files_zip_free(z);
            CHECK(a && b && len == len2 && (uint64_t)ta == len && ta == tb && !memcmp(a, b, (size_t)ta) &&
                  !memcmp(a, "PK\x03\x04", 4) && !memcmp(a + ta - 22, "PK\x05\x06", 4) && a[ta - 22 + 8] == 4 &&
                  !memmem(a, (size_t)ta, ".hidden", 7) && !memmem(a, (size_t)ta, "states/link", 11) &&
                  memmem(a, (size_t)ta, "states/x/state-00.bin", 21) && memmem(a, (size_t)ta, "states/empty/", 13),
                  "files: a zip of states/ (%ld bytes, the length planned first): 4 entries (the folder, x/, empty/, the file; no "
                  "hidden file, no link), the same read in 64 KB or 7-byte pieces", ta);
            free(a), free(b);
            ok = files_rmtree(root) == 0;   /* (once: CHECK reads its condition twice) */
            CHECK(ok && access("/etc/hostname", F_OK) == 0 && access(root, F_OK) != 0,
                  "files: files_rmtree removes a folder (a link in it unlinked, not followed)");
        }
    }
}

/* fmwords.c: fm1-reader's tests/words_test.mjs, its list and its cases */
static void fmwords(void)
{
    static const char *const CASES[][2] = {
        { "BD TUNE 37", "bass drum tune 37" }, { "909 GR", "nine oh nine groove" }, { "909 KIT", "nine oh nine kit" },
        { "P3", "pattern 3" }, { "COPY TO P17-32", "copy to patterns 17 to 32" },
        { "STEP 5: 1:4", "step 5: loop 1 of every 4" }, { "NEW LINE 3FA2", "new line" },
        { "LENGTH 16 st", "length 16 steps" }, { "DIRECTION REV", "direction reverse" },
        { "RATE 1/16T", "rate sixteenth triplet" }, { "TIME 1/8., FEEDBACK 52", "time dotted eighth, feedback 52" },
        { "TIME 1/8.", "time dotted eighth" }, { "RATE 1/16. STEPS 1-16 ON: NONE", "rate sixteenth. steps 1 to 16 on: none" },
        { "RATIO 4:1", "ratio 4 to 1" }, { "BATTERY 2/3", "battery 2 of 3" }, { "ROOT C#", "root C sharp" },
        { "STEP 5 ON C#3", "step 5 on C sharp 3" }, { "REPEAT x1", "repeat 1 time" }, { "REPEAT x4", "repeat 4 times" },
        { "SWING 50 %", "swing 50 percent" }, { "SWING 50%", "swing 50 percent" }, { "LEVEL -5 dB", "level -5 decibels" },
        { "BD (1/2): TUNE 34", "bass drum, page 1 of 2: tune 34" }, { "PLAY / STOP", "play or stop" },
        { "HOME + REC: UNDO", "home plus record: undo" }, { "(+ OCT+: P17-32)", "(plus octave up: patterns 17 to 32)" },
        { "HOLD A STEP", "hold a step" }, { "LOOP A", "loop A" }, { "KEY A MINOR", "key A minor" },
        { "A FLAT MAJOR", "A flat major" }, { "A DORIAN", "A dorian" }, { "THE PART'S SOUND", "the part's sound" },
        { "THE EFFECTS: REVERB, DELAY,. TAPE", "the effects: reverb, delay. tape" },
        { "CLEAR THIS?. SEL: YES", "clear this? sel: yes" }, { "WAVE Saw", "wave Saw" },
        { "BREAK MIDI CH 4", "break MIDI channel 4" }, { "A  B ", "A B" }, { "BDX", "bdx" },
    };
    const char *list = "{\"format\":1,\"words\":[[\"BD\",\"bass drum\"],[\"909\",\"nine oh nine\"],[\"909 GR\",\"nine oh "
        "nine groove\"],[\"P#\",\"pattern #\"],[\"P#-#\",\"patterns # to #\"],[\"STEP #: #:#\",\"step #: loop # of every #\"],"
        "[\"NEW LINE *\",\"new line\"],[\"LENGTH # st\",\"length # steps\"],[\"REV\",\"reverse\"],[\"REV\",\"reverb\"],"
        "[\"OCT+\",\"octave up\"],[\"REC\",\"record\"],[\"MIDI CH #\",\"MIDI channel #\"],[\"dB\",\"decibels\"],"
        "[\"\",\"x\"],[1,2]]}";
    fmw_t w;
    char out[512];
    int i, bad = 0, ld = fmw_load(&w, list);
    CHECK(ld == 0 && w.n == 14, "fmwords: the test list loaded (%d entries; the empty key and the numbers left out)", w.n);
    for (i = 0; i < (int)(sizeof CASES / sizeof CASES[0]); i++) {
        fmw_spoken(CASES[i][0], &w, out, sizeof out);
        if (strcmp(out, CASES[i][1])) {
            printf("        \"%s\" -> \"%s\", want \"%s\"\n", CASES[i][0], out, CASES[i][1]);
            bad++;
        }
    }
    CHECK(!bad, "fmwords: words_test.mjs's %d cases (%d wrong)", (int)(sizeof CASES / sizeof CASES[0]), bad);
    fmw_spoken("BD TUNE 37", NULL, out, sizeof out);
    CHECK(!strcmp(out, "bd tune 37"), "fmwords: no list: the common rules only (\"%s\")", out);
    fmw_spoken("BD TUNE 37", &w, out, 8);
    CHECK(!strcmp(out, "bass dr"), "fmwords: cut to the buffer (\"%s\")", out);
    fmw_free(&w);
    CHECK(fmw_load(&w, "{\"format\":2,\"words\":[]}") == -1, "fmwords: format 2 refused");
    {
        static char big[(FMW_WORDS_MAX + 1) * 10 + 64];
        int n = snprintf(big, sizeof big, "{\"format\":1,\"words\":[");
        for (i = 0; i <= FMW_WORDS_MAX; i++)
            n += snprintf(big + n, sizeof big - (size_t)n, "%s[\"A\",\"b\"]", i ? "," : "");
        snprintf(big + n, sizeof big - (size_t)n, "]}");
        CHECK(fmw_load(&w, big) == -1, "fmwords: more than %d entries refused", FMW_WORDS_MAX);
    }
    {
        char j[600];
        memset(out, 'A', FMW_WORD_MAX + 1);
        out[FMW_WORD_MAX + 1] = 0;
        snprintf(j, sizeof j, "{\"format\":1,\"words\":[[\"%s\",\"b\"],[\"C\",\"%s\"],[\"E\",\"f\"]]}", out, out);
        CHECK(fmw_load(&w, j) == 0 && w.n == 1 && !strcmp(w.k[0], "E"), "fmwords: an entry over %d characters left out",
              FMW_WORD_MAX);
        fmw_free(&w);
    }
}

/* (rfm1cs 0.13.0) projmem.c: a project's copies of its firmwares' memory, the states' files */
static void wr(const char *p, const void *b, size_t n)
{
    FILE *f = fopen(p, "wb");
    if (f) {
        fwrite(b, 1, n, f);
        fclose(f);
    }
}
static void projmem(void)
{
    char p[512], hex[65], ids[PM_SLOTS][RFM_MOVE_FW_ID_MAX], id[32], root[] = "/tmp/rfm14-pm-XXXXXX";
    static uint8_t img[4096], other[4096], blob[200 + RFM_MOVE_PFW_BYTES + 4];
    char *j;
    int i, k;
    pm_copy_path(p, sizeof p, "/r", 0, 0);
    CHECK(!strcmp(p, "/r/move/projects/move-project-01.fw1.bin"), "projmem: project 1's copy of slot 1: %s", p);
    pm_copy_path(p, sizeof p, "/r", 31, 2);
    CHECK(!strcmp(p, "/r/move/projects/move-project-32.fw3.bin"), "projmem: project 32, slot 3: %s", p);
    pm_clip_path(p, sizeof p, "/r", 1);
    CHECK(!strcmp(p, "/r/move/projects/.clip.fw2.bin"), "projmem: the buffer's copy is hidden: %s", p);
    pm_json_path(p, sizeof p, "/r/move/projects/move-project-07.fw2.bin");
    CHECK(!strcmp(p, "/r/move/projects/move-project-07.fw2.json"), "projmem: its .json: %s", p);
    for (i = 0; i < (int)sizeof img; i++)
        img[i] = (uint8_t)(i * 13), other[i] = (uint8_t)(i * 13);
    other[4000] ^= 1;
    pm_hash_hex((const uint8_t *)"abc", 3, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "projmem: the hash (SHA-256 of abc)");
    j = pm_json_hash("{\"id\":\"x\",\"saved\":1}\n", (const uint8_t *)"abc", 3);
    CHECK(j && !strcmp(j, "{\"id\":\"x\",\"saved\":1,\"hash\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}\n"),
          "projmem: the json gets its hash before its last }: %s", j ? j : "(null)");
    free(j);
    j = pm_json_hash("{}", (const uint8_t *)"abc", 3);
    CHECK(j && !strncmp(j, "{\"hash\":\"ba78", 12) && j[strlen(j) - 1] == '}', "projmem: an empty object: %s", j ? j : "(null)");
    free(j);
    if (mkdtemp(root)) {
        /* states: 2 another image with no hash (an older json), 5 this image under a wrong hash, 9 this image with no
         * json, 12 this image with its hash */
        snprintf(p, sizeof p, "%s/states", root), mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/states/fw", root), mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/states/fw/state-02.bin", root), wr(p, other, sizeof other);
        snprintf(p, sizeof p, "%s/states/fw/state-02.json", root), wr(p, "{\"id\":\"fw\"}\n", 12);
        CHECK(pm_state_match(root, "fw", img, sizeof img, "") == -1, "projmem: no stored state holds the image: -1");
        snprintf(p, sizeof p, "%s/states/fw/state-05.bin", root), wr(p, img, sizeof img);
        snprintf(p, sizeof p, "%s/states/fw/state-05.json", root);
        wr(p, "{\"id\":\"fw\",\"hash\":\"00\"}\n", 24);
        CHECK(pm_state_match(root, "fw", img, sizeof img, "") == -1, "projmem: a state whose json hash says otherwise is not read");
        snprintf(p, sizeof p, "%s/states/fw/state-09.bin", root), wr(p, img, sizeof img);
        CHECK((k = pm_state_match(root, "fw", img, sizeof img, "")) == 9, "projmem: a state with no json, the same bytes: %d", k);
        CHECK(pm_state_match(root, "fw", other, sizeof other, "") == 2, "projmem: an older json (no hash): compared byte for byte");
        CHECK(pm_state_match(root, "fw", img, sizeof img - 1, "") == -1 && pm_state_match(root, "fw", NULL, 0, "") == -1 &&
              pm_state_match(root, "nofw", img, sizeof img, "") == -1, "projmem: another size, no image, another firmware: -1");
        snprintf(p, sizeof p, "%s/states/fw/state-09.bin", root), unlink(p);
        j = pm_json_hash("{\"id\":\"fw\"}\n", img, sizeof img);
        snprintf(p, sizeof p, "%s/states/fw/state-12.json", root), wr(p, j, strlen(j));
        free(j);
        snprintf(p, sizeof p, "%s/states/fw/state-12.bin", root), wr(p, img, sizeof img);
        CHECK(pm_state_match(root, "fw", img, sizeof img, "") == 12, "projmem: a state with the image's hash: found");
        CHECK(pm_json_id(p, id, sizeof id) == -1, "projmem: no json: no id");
        snprintf(p, sizeof p, "%s/states/fw/state-12.json", root);
        CHECK(pm_json_id(p, id, sizeof id) == 0 && !strcmp(id, "fw"), "projmem: a copy's json names its firmware");
    }
    /* a blob's firmware ids: the version 9 part right before the 4-byte check */
    memset(blob, 0, sizeof blob);
    memcpy(blob, "RMSQ", 4);
    blob[4] = 9;
    memcpy(blob + sizeof blob - 4 - RFM_MOVE_PFW_BYTES, "felucca", 7);
    memcpy(blob + sizeof blob - 4 - RFM_MOVE_PFW_BYTES + 2 * RFM_MOVE_PFW_SLOT_BYTES, "choralroot", 10);
    CHECK(pm_blob_ids(blob, sizeof blob, ids) == 1 && !strcmp(ids[0], "felucca") && !ids[1][0] && !strcmp(ids[2], "choralroot"),
          "projmem: a version 9 blob's firmwares: '%s' '%s' '%s'", ids[0], ids[1], ids[2]);
    blob[4] = 8;
    CHECK(pm_blob_ids(blob, sizeof blob, ids) == 0 && !ids[0][0], "projmem: a version 8 blob holds no firmwares");
    CHECK(pm_blob_ids(blob, 10, ids) == 0 && pm_blob_ids(NULL, 0, ids) == 0, "projmem: a short blob, none: no firmwares");
    {   /* (2026-11-26) version 11 and up: the kit's part after the firmwares' (its size by version), a pad's
         * settings there never read as a firmware's id */
        static const struct { int ver; uint32_t kb; } KV[] = { { 11, RFM_MOVE_KIT_BYTES_V11 }, { 12, RFM_MOVE_KIT_BYTES_V12 },
            { 13, RFM_MOVE_KIT_BYTES_V12 }, { 14, RFM_MOVE_KIT_BYTES_V14 }, { 15, RFM_MOVE_KIT_BYTES_V15 },
            { 16, RFM_MOVE_KIT_BYTES_V15 }, { 17, RFM_MOVE_KIT_BYTES_V17 }, { 18, RFM_MOVE_KIT_BYTES } };   /* (17, 2026-11-26: Scan;
                                                         * 18: the pads' original places) */
        static uint8_t kb[200 + RFM_MOVE_PFW_BYTES + RFM_MOVE_KIT_BYTES + 4];
        unsigned v;
        for (v = 0; v < sizeof KV / sizeof KV[0]; v++) {
            const uint32_t n = 200 + RFM_MOVE_PFW_BYTES + KV[v].kb + 4;
            uint8_t *f = kb + n - 4 - KV[v].kb - RFM_MOVE_PFW_BYTES;
            memset(kb, 0, sizeof kb);
            memcpy(kb, "RMSQ", 4);
            kb[4] = (uint8_t)KV[v].ver;
            memcpy(f + RFM_MOVE_PFW_SLOT_BYTES, "felucca", 7);
            memset(kb + n - 4 - KV[v].kb, 0, KV[v].kb);   /* (the kit empty: zeros, as a fresh project's paths) */
            CHECK(pm_blob_ids(kb, n, ids) == 1 && !ids[0][0] && !strcmp(ids[1], "felucca") && !ids[2][0],
                  "projmem: a version %d blob's firmwares before its kit (%u bytes): '%s' '%s' '%s'", KV[v].ver,
                  (unsigned)KV[v].kb, ids[0], ids[1], ids[2]);
        }
        kb[4] = RFM_MOVE_PROJECT_VERSION + 1;
        CHECK(pm_blob_ids(kb, sizeof kb, ids) == 0, "projmem: a newer version's blob: no firmwares read (nothing dropped)");
    }
}

/* README "The working memory": a working memory's file (memsnap.h) back as it was written; a damaged one refused */
static void memsnap(void)
{
    const uint64_t sz = 64u * MS_PAGE + 100u;   /* (a last page shorter than the others) */
    ms_snap_t *m = ms_new(sz), *d;
    uint8_t *enc, *bad;
    size_t en = 0, i;
    uint32_t r = 12345;
    char fh[65], sha[65], hex[65];
    int same;
    memset(fh, 'a', 64), fh[64] = 0;
    memset(m->mem, 0, sz);
    for (i = 0; i < 3 * MS_PAGE; i++)   /* pages 1..3: noise */
        m->mem[MS_PAGE + i] = (uint8_t)((r = r * 1103515245u + 12345u) >> 16);
    for (i = 0; i < MS_PAGE; i++)       /* page 5: a pattern, page 7: a run, the last: a few bytes */
        m->mem[5 * MS_PAGE + i] = (uint8_t)(i % 7), m->mem[7 * MS_PAGE + i] = 0x5A;
    memcpy(m->mem + 64u * MS_PAGE + 90, "tail", 4);
    snprintf(m->h.sha256, sizeof m->h.sha256, "%s", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    m->h.g_off = 8, m->h.g_len = 12, m->h.tg_len = 12, m->h.g[0] = 1, m->h.g[11] = 2, m->h.tg[3] = 9;
    m->h.scratch = 4096, m->h.c1_stack = 8192, m->h.ms = 70000, m->h.frames_done = 3087000, m->h.due = 3087000;
    m->h.tk_ptr = 2048, m->h.keys_held = 5, m->h.qn = 2;
    m->h.q[0].due = 3087100, m->h.q[0].op = 4, m->h.q[0].a = 3, m->h.q[0].b = -2;
    m->h.q[1].due = 3087200, m->h.q[1].op = 6, m->h.q[1].c = 0x7F3C9009u;
    enc = ms_encode(m, fh, &en);
    d = enc ? ms_decode(enc, en, hex) : NULL;
    same = d && d->h.mem_size == sz && !memcmp(d->mem, m->mem, sz) && !strcmp(d->h.sha256, m->h.sha256) &&
           !strcmp(hex, fh) && d->h.g_len == 12 && !memcmp(d->h.g, m->h.g, 12) && !memcmp(d->h.tg, m->h.tg, 12) &&
           d->h.scratch == 4096 && d->h.c1_stack == 8192 && d->h.ms == 70000 && d->h.frames_done == 3087000 &&
           d->h.tk_ptr == 2048 && d->h.keys_held == 5 && d->h.qn == 2 && d->h.q[0].b == -2 &&
           d->h.q[1].c == 0x7F3C9009u;
    CHECK(same && en < 4 * MS_PAGE + 4096, "memsnap: %llu bytes of memory (zeros, noise, a pattern, a run) as a %zu-byte "
          "file and back, the header too", (unsigned long long)sz, en);
    CHECK(enc && ms_peek(enc, en, sha, hex) == 0 && !strcmp(sha, m->h.sha256) && !strcmp(hex, fh),
          "memsnap: the module and the image it goes with read without decoding");
    ms_free(d);
    if (enc && (bad = malloc(en))) {
        memcpy(bad, enc, en);
        bad[en / 2] ^= 0x40;
        d = ms_decode(bad, en, hex);
        CHECK(!d, "memsnap: a byte changed: refused");
        ms_free(d);
        d = ms_decode(enc, en - 9, hex);
        CHECK(!d && !ms_decode(enc, 10, hex) && !ms_decode((const uint8_t *)"RFM1MEM0xxxxxxxxxxxxxxxx", 24, hex),
              "memsnap: cut short, another magic: refused");
        ms_free(d);
        free(bad);
    }
    {
        char p[64];
        ms_path(p, sizeof p, "/r/states/fw/state-03.bin");
        CHECK(!strcmp(p, "/r/states/fw/state-03.mem"), "memsnap: a state's working memory beside it: %s", p);
    }
    {
        ms_snap_t *r2 = ms_ref(m);   /* (CHECK evaluates its condition twice: no side effect in it) */
        CHECK(r2 == m && m->refs == 2, "memsnap: a second reference");
        ms_free(r2);
        CHECK(m->refs == 1, "memsnap: given back: one left");
        ms_free(m);
    }
    free(enc);
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
    ed25519();
    catsig();
    fsapi();
    filesapi();
    fmwords();
    projmem();
    memsnap();
    printf("unit: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
