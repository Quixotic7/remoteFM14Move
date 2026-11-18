/* SPDX-License-Identifier: MPL-2.0 */
/* Unit tests: the manifest reader, the output stage (mix and limiter), the mDNS answers. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine_dsp.h"
#include "mdns.h"
#include "minijson.h"

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

int main(void)
{
    printf("unit\n");
    json();
    dsp();
    mdns();
    printf("unit: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
