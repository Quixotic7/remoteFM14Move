/* SPDX-License-Identifier: MPL-2.0 */
/* rfm1info: an .rfm1 file's manifest and licence texts, and the checks a host makes before running it
 * (RFM1-FORMAT.md sections 1, 3, 4): the wasm magic, the rfm1 section, the imports (wasi_snapshot_preview1's six
 * only, and for a Fantasy CFW rfm1_host_1's four: FM1VST docs/VIRTUAL-CARTS.md, "virtual": true with "fs1" in "needs"),
 * the required manifest fields; a Fantasy CFW's limits (the file under 16 MiB, its memory at most 512 MiB).
 *
 *   rfm1info FILE.rfm1 [--field KEY] [--manifest] [--licenses OUT.txt] [--check]
 *     --field KEY      a manifest string field (id, name, version, license, source_url, ..) on stdout
 *     --manifest       the manifest JSON on stdout
 *     --licenses OUT   the rfm1.credits text and every rfm1.license.<n> section, one after another
 *     --skin PREFIX    a skin's sections (RFM1-FORMAT.md section 8), each as it is: rfm1.skin -> PREFIX.rfm1skin,
 *                      rfm1.skin.background -> PREFIX.rfm1skin.bg, rfm1.skin.background.landscape -> PREFIX.rfm1skin.bgl
 *     --check          exit 1 (said on stderr) when the file is not an .rfm1 this host runs */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/minijson.h"

static uint8_t *g_b;
static size_t g_n;

static int leb(size_t *at, uint32_t *v)
{
    uint32_t r = 0;
    int s = 0;
    for (;;) {
        uint8_t x;
        if (*at >= g_n || s > 28)
            return -1;
        x = g_b[(*at)++];
        r |= (uint32_t)(x & 0x7F) << s;
        s += 7;
        if (x < 0x80)
            break;
    }
    *v = r;
    return 0;
}

static const char *IMPORTS[] = { "fd_write", "fd_read", "fd_close", "fd_seek", "clock_time_get", "proc_exit" };
static const char *HOST1[] = { "fs_request", "fs_poll", "fs_cancel", "fs_changed" };   /* rfm1_host_1 (fs1) */

/* a manifest's array field (top level) lists s */
static int lists(const char *man, const char *key, const char *s)
{
    const char *it = mj_find(man, key), *e;
    const size_t n = strlen(s);
    if (!it || *it != '[')
        return 0;
    while ((e = mj_elem(&it)))
        if (*e == '"' && mj_span(e) == n + 2 && !strncmp(e + 1, s, n))
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL, *field = NULL, *lic = NULL, *skin = NULL;
    int want_manifest = 0, check = 0, i, bad = 0, host1 = 0;
    uint32_t mem_pages = 0;
    char *manifest = NULL;
    FILE *f, *lf = NULL;
    size_t at;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--field") && i + 1 < argc)
            field = argv[++i];
        else if (!strcmp(argv[i], "--licenses") && i + 1 < argc)
            lic = argv[++i];
        else if (!strcmp(argv[i], "--skin") && i + 1 < argc)
            skin = argv[++i];
        else if (!strcmp(argv[i], "--manifest"))
            want_manifest = 1;
        else if (!strcmp(argv[i], "--check"))
            check = 1;
        else if (!path && argv[i][0] != '-')
            path = argv[i];
        else {
            fprintf(stderr, "usage: rfm1info FILE.rfm1 [--field KEY] [--manifest] [--licenses OUT] [--skin PREFIX] [--check]\n");
            return 2;
        }
    }
    if (!path || !(f = fopen(path, "rb"))) {
        fprintf(stderr, "rfm1info: cannot read %s\n", path ? path : "(none)");
        return 2;
    }
    fseek(f, 0, SEEK_END);
    g_n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_b = malloc(g_n ? g_n : 1);
    if (!g_b || fread(g_b, 1, g_n, f) != g_n)
        return 2;
    fclose(f);
    if (g_n < 8 || memcmp(g_b, "\0asm\1\0\0\0", 8)) {
        fprintf(stderr, "rfm1info: %s is not a WebAssembly module\n", path);
        return 1;
    }
    if (lic && !(lf = fopen(lic, "w"))) {
        fprintf(stderr, "rfm1info: cannot write %s\n", lic);
        return 2;
    }
    for (at = 8; at < g_n;) {
        const uint8_t id = g_b[at++];
        uint32_t len;
        size_t end;
        if (leb(&at, &len) || at + len > g_n) {
            fprintf(stderr, "rfm1info: a damaged section\n");
            return 1;
        }
        end = at + len;
        if (id == 0) {   /* custom: name, then the bytes */
            uint32_t nl;
            size_t p = at;
            char name[64];
            if (leb(&p, &nl) || p + nl > end || nl >= sizeof name)
                goto next;
            memcpy(name, g_b + p, nl);
            name[nl] = 0;
            p += nl;
            if (!strcmp(name, "rfm1")) {
                manifest = malloc(end - p + 1);
                memcpy(manifest, g_b + p, end - p);
                manifest[end - p] = 0;
            } else if (skin && (!strcmp(name, "rfm1.skin") || !strcmp(name, "rfm1.skin.background") ||
                                !strcmp(name, "rfm1.skin.background.landscape"))) {
                char out[1024];
                FILE *sf;
                snprintf(out, sizeof out, "%s.rfm1skin%s", skin, !strcmp(name, "rfm1.skin") ? ""
                         : !strcmp(name, "rfm1.skin.background") ? ".bg" : ".bgl");
                if (!(sf = fopen(out, "wb")) || fwrite(g_b + p, 1, end - p, sf) != end - p) {
                    fprintf(stderr, "rfm1info: cannot write %s\n", out);
                    return 2;
                }
                fclose(sf);
            } else if (lf && (!strcmp(name, "rfm1.credits") || !strncmp(name, "rfm1.license.", 13))) {
                fprintf(lf, "==== %s ====\n\n", !strcmp(name, "rfm1.credits") ? "Credits" : name + 5);
                fwrite(g_b + p, 1, end - p, lf);
                fputs("\n\n", lf);
            }
        } else if (id == 2) {   /* imports: wasi_snapshot_preview1's six functions only */
            uint32_t n, k;
            size_t p = at;
            if (leb(&p, &n))
                return 1;
            for (k = 0; k < n; k++) {
                uint32_t ml, fl, idx;
                char mod[64], fn[64];
                uint8_t kind;
                size_t j;
                int ok = 0;
                if (leb(&p, &ml) || ml >= sizeof mod || p + ml > end)
                    return 1;
                memcpy(mod, g_b + p, ml), mod[ml] = 0, p += ml;
                if (leb(&p, &fl) || fl >= sizeof fn || p + fl > end)
                    return 1;
                memcpy(fn, g_b + p, fl), fn[fl] = 0, p += fl;
                kind = g_b[p++];
                if (kind != 0) {   /* a table, memory or global import: not allowed */
                    fprintf(stderr, "rfm1info: imports %s.%s (kind %u)\n", mod, fn, kind);
                    bad = 1;
                    break;
                }
                if (leb(&p, &idx))
                    return 1;
                for (j = 0; j < sizeof IMPORTS / sizeof IMPORTS[0]; j++)
                    ok |= !strcmp(mod, "wasi_snapshot_preview1") && !strcmp(fn, IMPORTS[j]);
                for (j = 0; j < sizeof HOST1 / sizeof HOST1[0]; j++)
                    if (!strcmp(mod, "rfm1_host_1") && !strcmp(fn, HOST1[j]))
                        ok = host1 = 1;   /* (allowed below only for a Fantasy CFW with fs1 in its needs) */
                if (!ok) {
                    fprintf(stderr, "rfm1info: imports %s.%s, which a host does not give\n", mod, fn);
                    bad = 1;
                }
            }
        } else if (id == 5) {   /* memory: its initial size (a Fantasy CFW's limit) */
            uint32_t n, flags = 0;
            size_t p = at;
            if (!leb(&p, &n) && n >= 1 && !leb(&p, &flags))
                leb(&p, &mem_pages);
        }
    next:
        at = end;
    }
    if (lf)
        fclose(lf);
    if (!manifest) {
        fprintf(stderr, "rfm1info: %s has no rfm1 section: not an .rfm1\n", path);
        return 1;
    }
    {
        static const char *REQ[] = { "id", "name", "version", "license", "source_url" };
        char v[256];
        long r = 0, abi = 0;
        size_t k;
        for (k = 0; k < sizeof REQ / sizeof REQ[0]; k++)
            if (mj_string(manifest, REQ[k], v, sizeof v) || !v[0]) {
                fprintf(stderr, "rfm1info: the manifest has no \"%s\"\n", REQ[k]);
                bad = 1;
            }
        if (mj_int(manifest, "rfm1", &r) || r != 1 || mj_int(manifest, "abi", &abi) || abi < 5 || abi > 6) {
            fprintf(stderr, "rfm1info: manifest rfm1 %ld, abi %ld: not version 1 / ABI 5..6\n", r, abi);
            bad = 1;
        }
        {   /* a Fantasy CFW (VIRTUAL-CARTS.md 1-2, 4): its imports, its limits; a "needs" this host lacks refused */
            const char *vv = mj_find(manifest, "virtual");
            const int virt = vv && !strncmp(vv, "true", 4), fs1 = lists(manifest, "needs", "fs1");
            if (host1 && !(virt && fs1)) {
                fprintf(stderr, "rfm1info: imports rfm1_host_1 without \"virtual\": true and \"fs1\" in \"needs\"\n");
                bad = 1;
            }
            if (virt && g_n >= 16u << 20) {
                fprintf(stderr, "rfm1info: a Fantasy CFW of %zu bytes: at most 16 MiB\n", g_n);
                bad = 1;
            }
            if (virt && mem_pages > 8192) {
                fprintf(stderr, "rfm1info: a Fantasy CFW with %u MiB of memory: at most 512 MiB\n", mem_pages / 16);
                bad = 1;
            }
            {
                const char *it = mj_find(manifest, "needs"), *e;
                if (it && *it == '[')
                    while ((e = mj_elem(&it)))
                        if (!(*e == '"' && mj_span(e) == 5 && !strncmp(e, "\"fs1\"", 5))) {
                            fprintf(stderr, "rfm1info: needs %.*s, which this host does not give\n", (int)mj_span(e), e);
                            bad = 1;
                        }
            }
        }
        if (!mj_string(manifest, "id", v, sizeof v)) {
            for (k = 0; v[k]; k++)
                if (!((v[k] >= 'a' && v[k] <= 'z') || (v[k] >= '0' && v[k] <= '9') || v[k] == '_'))
                    break;
            if (v[k] || k == 0 || k > 31) {
                fprintf(stderr, "rfm1info: id \"%s\" is not [a-z0-9_]{1,31}\n", v);
                bad = 1;
            }
        }
    }
    if (field) {
        char v[1024];
        if (mj_string(manifest, field, v, sizeof v)) {
            fprintf(stderr, "rfm1info: no string field \"%s\"\n", field);
            return 1;
        }
        puts(v);
    }
    if (want_manifest)
        fputs(manifest, stdout);
    return check && bad ? 1 : 0;
}
