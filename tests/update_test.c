/* SPDX-License-Identifier: MPL-2.0 */
/* The page's Update's parts (src/update.c, src/gz.c), natively: gzip both ways, the .rfm1u's signature (good, tampered,
 * the catalog's key or message, no domain, the release key alone), its payload's rules (.., absolute, links, devices,
 * FIFOs, "." parts, .update-, long names, skipped AppleDouble), a whole module package's remotefm1/ dropped, and the
 * data backup unpacked again. The fixtures: tests/update_fixtures.py (tests/update_test.sh makes them).
 *   UPD_FIX=build/update-test UPD_WORK=build/update-test/work build/update-test/update_test */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gz.h"
#include "update.h"
#include "update_key.h"
#include "update_test_key.h"
#include "catalog_test_key.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (c) printf("  ok    " __VA_ARGS__); else { fails++; printf("  FAIL  " __VA_ARGS__); } printf("\n"); } while (0)

static const char *FIX, *WORK;
static char g_p[1024];
static const char *fx(const char *n) { snprintf(g_p, sizeof g_p, "%s/%s", FIX, n); return g_p; }

static uint8_t *g_buf;
static size_t g_n, g_cap;
static int collect(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    if (g_n + n > g_cap) {
        g_cap = (g_n + n) * 2;
        g_buf = realloc(g_buf, g_cap);
    }
    memcpy(g_buf + g_n, p, n);
    g_n += n;
    return 0;
}
static int gz_round(const uint8_t *data, size_t n, size_t *packed)
{
    char path[1100], why[200];
    FILE *f;
    gz_writer_t *w;
    int fd, rc;
    struct stat st;
    snprintf(path, sizeof path, "%s/round.gz", WORK);
    f = fopen(path, "wb");
    w = gzw_open(f);
    /* (in odd pieces: the window slides across them) */
    for (size_t at = 0; at < n;) {
        size_t k = 1 + (at * 7919u) % 100000u;
        if (k > n - at)
            k = n - at;
        gzw_write(w, data + at, k);
        at += k;
    }
    if (gzw_close(w) != 0)
        return -1;
    fclose(f);
    stat(path, &st);
    *packed = (size_t)st.st_size;
    fd = open(path, O_RDONLY);
    g_n = 0;
    rc = gz_inflate_fd(fd, (uint64_t)st.st_size, collect, NULL, why, sizeof why);
    close(fd);
    if (rc != 0)
        printf("        (%s)\n", why);
    return rc == 0 && g_n == n && (n == 0 || !memcmp(g_buf, data, n)) ? 0 : -1;
}

static const uint8_t (*K_TEST)[32] = (const uint8_t(*)[32])UPD_TEST_KEY;
static int check(const char *name, const uint8_t (*keys)[32], int nk, upd_manifest_t *m, char *why, size_t cap)
{
    int rc = upd_check_file(fx(name), keys, nk, m, why, cap);
    return rc;
}
static int scan(const char *name, int *strip, int *files, char *why, size_t cap)
{
    upd_manifest_t m;
    uint64_t bytes = 0;
    int rc;
    if (upd_check_file(fx(name), K_TEST, 1, &m, why, cap) != 0)
        return -2;
    rc = upd_payload(fx(name), &m, NULL, strip, files, &bytes, why, cap);
    upd_manifest_free(&m);
    return rc;
}
static int unpack(const char *name, const char *dest, char *why, size_t cap)
{
    upd_manifest_t m;
    int strip = 0, files = 0, rc;
    uint64_t bytes = 0;
    if (upd_check_file(fx(name), K_TEST, 1, &m, why, cap) != 0)
        return -2;
    rc = upd_payload(fx(name), &m, NULL, &strip, &files, &bytes, why, cap);
    if (rc == 0) {
        mkdir(dest, 0755);
        rc = upd_payload(fx(name), &m, dest, &strip, NULL, NULL, why, cap);
    }
    upd_manifest_free(&m);
    return rc;
}
static int exists(const char *dir, const char *rel)
{
    char p[1400];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    return lstat(p, &st) == 0;
}
static long fsize(const char *dir, const char *rel, mode_t *mode)
{
    char p[1400];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    if (lstat(p, &st) != 0)
        return -1;
    if (mode)
        *mode = st.st_mode;
    return (long)st.st_size;
}
static void put_file(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    fwrite(p, 1, n, f);
    fclose(f);
}

int main(void)
{
    char why[400], d[1100];
    upd_manifest_t m;
    int strip, files, rc;
    FIX = getenv("UPD_FIX");
    WORK = getenv("UPD_WORK");
    if (!FIX || !WORK) {
        fprintf(stderr, "UPD_FIX and UPD_WORK, please\n");
        return 2;
    }
    upd_rmtree(WORK);
    mkdir(WORK, 0755);

    printf("gzip\n");
    {
        const size_t N = 3u << 20;
        uint8_t *data = malloc(N);
        size_t packed = 0, i;
        uint32_t x = 12345;
        CHECK(gz_round((const uint8_t *)"", 0, &packed) == 0, "nothing, round trip");
        CHECK(gz_round((const uint8_t *)"a", 1, &packed) == 0, "one byte, round trip");
        memset(data, 0, N);
        CHECK(gz_round(data, N, &packed) == 0 && packed < N / 100, "3 MB of zeros: round trip, %zu bytes", packed);
        memset(data, 0xff, N);
        for (i = 0; i < N; i += 4096)
            memcpy(data + i, "a state header..", 16);
        CHECK(gz_round(data, N, &packed) == 0 && packed < N / 20, "3 MB of flash image: round trip, %zu bytes", packed);
        for (i = 0; i < N; i++) {
            x = x * 1103515245u + 12345u;
            data[i] = (uint8_t)(x >> 24);
        }
        CHECK(gz_round(data, N, &packed) == 0, "3 MB of noise: round trip, %zu bytes", packed);
        for (i = 0; i < N; i++)
            data[i] = "the quick brown fox jumps over the lazy dog, "[(i * 3 + i / 977) % 45];
        CHECK(gz_round(data, N, &packed) == 0 && packed < N / 4, "3 MB of text: round trip, %zu bytes", packed);
        free(data);
    }

    printf("the signature\n");
    rc = check("good.rfm1u", K_TEST, 1, &m, why, sizeof why);
    CHECK(rc == 0 && !strcmp(m.version, "9.9.9-test") && !strcmp(m.kind, "overlay") && m.nfiles > 3,
          "good: checks (version %s, %s, %d files)", m.version, m.kind, m.nfiles);
    upd_manifest_free(&m);
    rc = check("good.rfm1u", (const uint8_t(*)[32])UPD_SIGNING_KEY, 1, &m, why, sizeof why);
    CHECK(rc != 0 && strstr(why, "signature"), "good: refused by the release key alone (%s)", why);
    {
        static const char *const BAD[] = { "tampered.rfm1u", "tamper-man.rfm1u", "catsig.rfm1u", "catdom.rfm1u",
                                           "nodomain.rfm1u" };
        for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
            uint8_t keys[2][32];
            memcpy(keys[0], UPD_TEST_KEY, 32);
            memcpy(keys[1], UPD_SIGNING_KEY, 32);
            rc = check(BAD[i], (const uint8_t(*)[32])keys, 2, &m, why, sizeof why);
            CHECK(rc != 0 && strstr(why, "signature"), "%s: refused (%s)", BAD[i], why);
        }
    }
    {   /* the catalog's key is never an update's */
        rc = check("catdom.rfm1u", (const uint8_t(*)[32])CAT_TEST_KEY, 1, &m, why, sizeof why);
        CHECK(rc == 0, "catdom: good with the catalog key itself (so the key is what refuses it above)");
        upd_manifest_free(&m);
    }
    {   /* cut short, one byte added, not an update */
        char p[1100];
        FILE *f = fopen(fx("good.rfm1u"), "rb");
        long n;
        uint8_t *b;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        b = malloc((size_t)n + 1);
        fread(b, 1, (size_t)n, f);
        fclose(f);
        snprintf(p, sizeof p, "%s/cut.rfm1u", WORK);
        put_file(p, b, (size_t)n - 1);
        rc = upd_check_file(p, K_TEST, 1, &m, why, sizeof why);
        CHECK(rc != 0 && strstr(why, "sizes"), "cut short by a byte: refused (%s)", why);
        b[n] = 0;
        put_file(p, b, (size_t)n + 1);
        rc = upd_check_file(p, K_TEST, 1, &m, why, sizeof why);
        CHECK(rc != 0 && strstr(why, "sizes"), "a byte added: refused (%s)", why);
        put_file(p, "PK\3\4 not an update file at all, a zip perhaps ...........................................", 90);
        rc = upd_check_file(p, K_TEST, 1, &m, why, sizeof why);
        CHECK(rc != 0 && strstr(why, "not an update"), "a zip: refused (%s)", why);
        b[8] = 2;
        put_file(p, b, (size_t)n);
        rc = upd_check_file(p, K_TEST, 1, &m, why, sizeof why);
        CHECK(rc != 0 && strstr(why, "format"), "format 2: refused (%s)", why);
        free(b);
    }

    printf("the payload\n");
    rc = scan("good.rfm1u", &strip, &files, why, sizeof why);
    CHECK(rc == 0 && !strip && files > 3, "good: %d files, nothing to drop", files);
    snprintf(d, sizeof d, "%s/good", WORK);
    rc = unpack("good.rfm1u", d, why, sizeof why);
    {
        mode_t mode = 0;
        const long hs = fsize(d, "remotefm1-host", &mode);
        CHECK(rc == 0 && hs > 100000 && (mode & 0111) && exists(d, "help.json") && exists(d, "module.json") &&
              exists(d, "site/index.html") && exists(d, "site/js/moveupdate.js") && exists(d, "update-marker.txt"),
              "good: unpacked (remotefm1-host %ld bytes, executable; help.json, module.json, site/, the marker)", hs);
    }
    {
        static const char *const BAD[][2] = {
            { "dotdot.rfm1u", "\"..\"" }, { "abs.rfm1u", "absolute" }, { "symlink.rfm1u", "link" },
            { "hardlink.rfm1u", "link" }, { "chardev.rfm1u", "device" }, { "fifo.rfm1u", "FIFO" },
            { "dotpart.rfm1u", "\".\"" }, { "reserved.rfm1u", ".update-" },
        };
        for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
            char dd[1200];
            rc = scan(BAD[i][0], &strip, &files, why, sizeof why);
            CHECK(rc == -1 && strstr(why, BAD[i][1]), "%s: refused by the scan (%s)", BAD[i][0], why);
            /* and by the unpack itself, nothing written past what came before it */
            snprintf(dd, sizeof dd, "%s/bad-%zu", WORK, i);
            mkdir(dd, 0755);
            {
                upd_manifest_t mm;
                int st = 0;
                upd_check_file(fx(BAD[i][0]), K_TEST, 1, &mm, why, sizeof why);
                rc = upd_payload(fx(BAD[i][0]), &mm, dd, &st, NULL, NULL, why, sizeof why);
                upd_manifest_free(&mm);
            }
            CHECK(rc == -1 && !exists(WORK, "evil.txt") && !exists(dd, "site/link") && !exists(dd, "site/hard") &&
                  !exists(dd, "site/dev") && !exists(dd, "site/fifo"), "%s: refused by the unpack too, nothing outside",
                  BAD[i][0]);
        }
    }
    {
        static const char *const LONG[] = { "gnulong.rfm1u", "paxlong.rfm1u" };
        for (size_t i = 0; i < 2; i++) {
            char dd[1200], rel[400];
            snprintf(dd, sizeof dd, "%s/long-%zu", WORK, i);
            snprintf(rel, sizeof rel, "site/%0*d/%0*d/long-name.txt", 60, 0, 70, 0);
            memset(rel + 5, 'a', 60);
            memset(rel + 66, 'b', 70);
            rc = unpack(LONG[i], dd, why, sizeof why);
            CHECK(rc == 0 && fsize(dd, rel, NULL) == 5, "%s: a 150-character name unpacked", LONG[i]);
        }
    }
    snprintf(d, sizeof d, "%s/skips", WORK);
    rc = scan("skips.rfm1u", &strip, &files, why, sizeof why);
    CHECK(rc == 0 && files == 2, "skips: ._ and .DS_Store left out (%d files)", files);
    rc = unpack("skips.rfm1u", d, why, sizeof why);
    CHECK(rc == 0 && exists(d, "site/x.txt") && !exists(d, "._help.json") && !exists(d, "site/.DS_Store"),
          "skips: unpacked without them");
    rc = scan("full.rfm1u", &strip, &files, why, sizeof why);
    CHECK(rc == 0 && strip == 1 && files == 4, "full: every name under remotefm1/ (%d files)", files);
    snprintf(d, sizeof d, "%s/full", WORK);
    rc = unpack("full.rfm1u", d, why, sizeof why);
    CHECK(rc == 0 && exists(d, "remotefm1-host") && exists(d, "firmwares/full-marker.txt") && !exists(d, "remotefm1"),
          "full: unpacked with remotefm1/ dropped");
    {   /* into a copy of good: an overlay replaces, keeps the rest; a file where a folder is refuses */
        char g[1100];
        snprintf(g, sizeof g, "%s/good", WORK);
        rc = unpack("skips.rfm1u", g, why, sizeof why);
        CHECK(rc == 0 && fsize(g, "help.json", NULL) == 2 && exists(g, "site/index.html") && exists(g, "site/x.txt"),
              "an overlay over good: help.json replaced, the rest kept, site/x.txt added");
    }

    printf("names\n");
    {
        static const struct { const char *in, *out; int rc; } N[] = {
            { "site/a.js", "site/a.js", 0 }, { "./site/a.js", "site/a.js", 0 }, { "site/", "site", 0 },
            { "./", "", 1 }, { ".", "", 1 }, { "/etc/x", "", -1 }, { "a/../b", "", -1 }, { "..", "", -1 },
            { "a//b", "", -1 }, { "a/./b", "", -1 }, { "a\nb", "", -1 }, { "site/._x", "", 1 },
            { ".update-new/x", "", -1 }, { "site/.DS_Store", "", 1 }, { "a/.hidden", "a/.hidden", 0 },
        };
        for (size_t i = 0; i < sizeof N / sizeof N[0]; i++) {
            char o[200] = "";
            const char *w = "";
            const int r = upd_name(N[i].in, o, sizeof o, &w);
            CHECK(r == N[i].rc && (r != 0 || !strcmp(o, N[i].out)), "name \"%s\": %d %s%s", N[i].in, r, o, r < 0 ? w : "");
        }
    }

    printf("the data backup\n");
    {
        char root[1100], p[1500], out[1200], dd[1100];
        static uint8_t big[1u << 20];
        snprintf(root, sizeof root, "%s/data", WORK);
        mkdir(root, 0755);
        snprintf(p, sizeof p, "%s/move", root), mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/move/projects", root), mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/move/settings.bin", root), put_file(p, "settings", 8);
        memset(big, 0xff, sizeof big);
        snprintf(p, sizeof p, "%s/move/projects/move-project-01.fw1.bin", root), put_file(p, big, sizeof big);
        snprintf(p, sizeof p, "%s/move/projects/%0120d.json", root, 7), put_file(p, "{\"long\":1}", 10);
        snprintf(p, sizeof p, "%s/move/projects/%0130d/%0130d.json", root, 1, 2);
        {
            char q[1500];
            snprintf(q, sizeof q, "%s/move/projects/%0130d", root, 1), mkdir(q, 0755);
        }
        put_file(p, "deep", 4);
        snprintf(out, sizeof out, "%s/backup.tar.gz", WORK);
        rc = upd_backup_dir(root, "move", out, why, sizeof why);
        {
            upd_manifest_t bm;
            struct stat st;
            int s2 = 0, nf = 0;
            stat(out, &st);
            memset(&bm, 0, sizeof bm);
            bm.poff = 0, bm.plen = (uint64_t)st.st_size;
            snprintf(dd, sizeof dd, "%s/restored", WORK);
            mkdir(dd, 0755);
            CHECK(rc == 0 && st.st_size < 20000, "move/ backed up (%lld bytes for 1 MB of flash image)", (long long)st.st_size);
            rc = upd_payload(out, &bm, NULL, &s2, &nf, NULL, why, sizeof why);
            CHECK(rc == 0 && nf == 4, "the backup reads back (%d files)", nf);
            rc = upd_payload(out, &bm, dd, &s2, NULL, NULL, why, sizeof why);
            snprintf(p, sizeof p, "move/projects/%0130d/%0130d.json", 1, 2);
            CHECK(rc == 0 && fsize(dd, "move/settings.bin", NULL) == 8 &&
                  fsize(dd, "move/projects/move-project-01.fw1.bin", NULL) == (long)sizeof big && fsize(dd, p, NULL) == 4,
                  "the backup unpacks to the same files (a 1 MB one, a 270-character name)");
        }
        snprintf(p, sizeof p, "%s/nothing", WORK);
        mkdir(p, 0755);
        CHECK(upd_backup_dir(p, "move", out, why, sizeof why) == 1, "no move/: nothing to back up");
    }

    printf("update_test: %d checks, %d failed\n", checks, fails);
    if (!fails)
        printf("ALL UPDATE TESTS PASSED\n");
    return fails ? 1 : 0;
}
