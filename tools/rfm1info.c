/* SPDX-License-Identifier: MPL-2.0 */
/* rfm1info: an .rfm1 file's manifest and licence texts, and the checks a host makes before running it
 * (RFM1-FORMAT.md sections 1, 3, 4): the wasm magic, the rfm1 section, the imports (wasi_snapshot_preview1's six
 * only), the required manifest fields.
 *
 *   rfm1info FILE.rfm1 [--field KEY] [--manifest] [--licenses OUT.txt] [--check]
 *     --field KEY      a manifest string field (id, name, version, license, source_url, ..) on stdout
 *     --manifest       the manifest JSON on stdout
 *     --licenses OUT   the rfm1.credits text and every rfm1.license.<n> section, one after another
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

int main(int argc, char **argv)
{
    const char *path = NULL, *field = NULL, *lic = NULL;
    int want_manifest = 0, check = 0, i, bad = 0;
    char *manifest = NULL;
    FILE *f, *lf = NULL;
    size_t at;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--field") && i + 1 < argc)
            field = argv[++i];
        else if (!strcmp(argv[i], "--licenses") && i + 1 < argc)
            lic = argv[++i];
        else if (!strcmp(argv[i], "--manifest"))
            want_manifest = 1;
        else if (!strcmp(argv[i], "--check"))
            check = 1;
        else if (!path && argv[i][0] != '-')
            path = argv[i];
        else {
            fprintf(stderr, "usage: rfm1info FILE.rfm1 [--field KEY] [--manifest] [--licenses OUT] [--check]\n");
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
                if (!ok) {
                    fprintf(stderr, "rfm1info: imports %s.%s, which a host does not give\n", mod, fn);
                    bad = 1;
                }
            }
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
