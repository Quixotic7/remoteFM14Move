/* SPDX-License-Identifier: MPL-2.0 */
/* The firmware catalog and its downloads: see catalog.h. */
#define _GNU_SOURCE
#include "catalog.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "catsig.h"
#include "https.h"
#include "minijson.h"
#include "sha256.h"

#define INDEX_MAX (8u << 20)   /* index.json's size cap */
#define SIG_MAX 1024u

static char g_bundled[ST_PATH];
static char g_url[512];
static volatile int g_cancel;

void cat_config(const char *bundled_index, const char *base_url)
{
    snprintf(g_bundled, sizeof g_bundled, "%s", bundled_index ? bundled_index : "");
    snprintf(g_url, sizeof g_url, "%s", base_url && *base_url ? base_url : CAT_URL_DEFAULT);
}

void cat_base_url(char *out, int cap)
{
    char p[ST_PATH];
    size_t n = 0;
    uint8_t *b;
    st_path(p, sizeof p, "catalog-url");
    if ((b = st_read(p, &n))) {   /* its first line, trimmed */
        size_t i = 0, e;
        while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n'))
            i++;
        for (e = i; e < n && b[e] != '\r' && b[e] != '\n'; e++)
            ;
        while (e > i && (b[e - 1] == ' ' || b[e - 1] == '\t' || b[e - 1] == '/'))
            e--;
        if (e > i && (!strncmp((char *)b + i, "http://", 7) || !strncmp((char *)b + i, "https://", 8))) {
            snprintf(out, (size_t)cap, "%.*s", (int)(e - i), (char *)b + i);
            free(b);
            return;
        }
        free(b);
    }
    snprintf(out, (size_t)cap, "%s", g_url[0] ? g_url : CAT_URL_DEFAULT);
    n = strlen(out);
    while (n > 0 && out[n - 1] == '/')
        out[--n] = 0;
}

/* ------------------------------------------------------------------ small things --- */
static uint32_t fnv(const uint8_t *b, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h ? h : 1;
}

int cat_b64(const char *in, uint8_t *out, int cap)
{
    int n = 0, bits = 0;
    uint32_t acc = 0;
    for (; *in && *in != '"'; in++) {
        const char c = *in;
        int v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            v = c - '0' + 52;
        else if (c == '+')
            v = 62;
        else if (c == '/')
            v = 63;
        else if (c == '=')
            break;
        else
            return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap)
                return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

/* a file name the index may give: one plain name, no path */
static int safe_name(const char *s)
{
    if (!*s || *s == '.' || strlen(s) > 200)
        return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '.' ||
              *s == '-' || *s == '_' || *s == '+'))
            return 0;
    return 1;
}

static int is_hex64(const char *s)
{
    int i;
    for (i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return s[64] == 0;
}

static int ends_with(const char *s, const char *suf)
{
    const size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

/* ------------------------------------------------------------------ the index --- */
static const char *CATS[5] = { "Synth", "Groovebox", "Chords", "Drums", "Other" };

/* one index entry into e: 0, -1 when it is not one */
static int parse_entry(const char *o, cat_ent_t *e)
{
    const char *v, *it;
    char tmp[256];
    long x;
    size_t cap = 0, n = 0;
    memset(e, 0, sizeof *e);
    e->cat = 255;
    if (mj_string(o, "id", e->id, sizeof e->id) || !e->id[0] || strlen(e->id) >= RFM_MOVE_FW_ID_MAX - 1 ||
        mj_string(o, "version", e->version, sizeof e->version) || !e->version[0])
        return -1;
    if (mj_string(o, "name", e->name, sizeof e->name) || !e->name[0])
        snprintf(e->name, sizeof e->name, "%s", e->id);
    mj_string(o, "description", e->desc, sizeof e->desc);
    mj_string(o, "license", e->license, sizeof e->license);
    mj_string(o, "source_url", e->source_url, sizeof e->source_url);
    if ((it = mj_find(o, "categories")) && *it == '[')
        while ((v = mj_elem(&it))) {
            int k;
            if (*v != '"')
                continue;
            {   /* (the string decoded: as an object's field) */
                char wrap[64];
                const size_t span = mj_span(v);
                if (span >= sizeof wrap - 8)
                    continue;
                snprintf(wrap, sizeof wrap, "{\"c\":%.*s}", (int)span, v);
                if (mj_string(wrap, "c", tmp, sizeof tmp))
                    continue;
            }
            for (k = 0; k < 5; k++)
                if (!strcmp(tmp, CATS[k]))
                    e->cats |= (uint8_t)(1u << k);
        }
    if (!mj_int(o, "cat", &x) && x >= 0 && x < 5)
        e->cat = (uint8_t)x;
    if (e->cat < 5 && !(e->cats >> e->cat & 1))
        e->cats |= (uint8_t)(1u << e->cat);
    if (e->cat == 255 && e->cats) {
        int k;
        for (k = 0; k < 5 && !(e->cats >> k & 1); k++)
            ;
        e->cat = (uint8_t)k;
    }
    if ((v = mj_find(o, "thumb")) && *v == '"' &&
        cat_b64(v + 1, e->thumb, sizeof e->thumb) == RFM_MOVE_FW_THUMB_BYTES)
        e->has_thumb = 1;
    if ((it = mj_find(o, "files")) && *it == '[') {
        int nf = 0, manifest_last = 0;
        while ((v = mj_elem(&it))) {
            char name[256], sha[80];
            long size = -1;
            char line[400];
            size_t k;
            if (mj_string(v, "name", name, sizeof name) || mj_string(v, "sha256", sha, sizeof sha) ||
                mj_int(v, "size", &size) || size < 0 || !safe_name(name) || !is_hex64(sha))
                goto bad;
            k = (size_t)snprintf(line, sizeof line, "%s %s %ld\n", name, sha, size);
            if (n + k + 1 > cap) {
                char *b = realloc(e->files, cap = (n + k + 1) * 2);
                if (!b)
                    goto bad;
                e->files = b;
            }
            memcpy(e->files + n, line, k + 1);
            n += k;
            e->bytes += (uint64_t)size;
            nf++;
            manifest_last = ends_with(name, ".json");
        }
        if (nf && manifest_last)
            e->indexed = 1;
        else {
            free(e->files);
            e->files = NULL;
        }
    }
    return 0;
bad:
    free(e->files);
    e->files = NULL;
    e->indexed = 0;
    return 0;   /* (listed, not downloadable) */
}

/* an index file read, its signature (sig: its .sig file) checked (catsig.h): its entries (malloc'd), how many (-1:
 * not a signed index; why, when not NULL); generated into gen */
static int read_index(const char *path, const char *sig, cat_ent_t **out, char *gen, int gcap, uint32_t *hash,
                      char *why, int wcap)
{
    size_t n = 0, sn = 0;
    char *b = (char *)st_read(path, &n), w[160];
    uint8_t *sb;
    const char *it, *o;
    long schema = 0;
    int k = 0, cap = 0;
    cat_ent_t *e = NULL;
    *out = NULL;
    if (!b) {
        if (why)
            snprintf(why, (size_t)wcap, "cannot read %s", path);
        return -1;
    }
    sb = st_read(sig, &sn);
    if (catsig_check((const uint8_t *)b, n, sn ? sb : NULL, sn, w, sizeof w) != 0) {
        fprintf(stderr, "catalog: %s refused: %s\n", path, w);
        if (why)
            snprintf(why, (size_t)wcap, "%s", w);
        free(sb);
        free(b);
        return -1;
    }
    free(sb);
    if (why)
        snprintf(why, (size_t)wcap, "not a Move catalog (schema 1)");
    if (mj_int(b, "schema", &schema) || schema != 1 || !(it = mj_find(b, "firmwares")) || *it != '[') {
        free(b);
        return -1;
    }
    if (gen)
        mj_string(b, "generated", gen, (size_t)gcap);
    if (hash)
        *hash = fnv((const uint8_t *)b, n);
    while ((o = mj_elem(&it))) {
        if (*o != '{')
            continue;
        if (k == cap) {
            cat_ent_t *x = realloc(e, sizeof *e * (size_t)(cap = cap ? cap * 2 : 128));
            if (!x)
                break;
            e = x;
        }
        if (parse_entry(o, &e[k]) == 0)
            k++;
    }
    free(b);
    *out = e;
    return k;
}

/* ------------------------------------------------------------------ the installed firmwares --- */
/* every manifest in firmwares/: the older versions of an id go (their manifest first), unless keep names their object */
static void sweep(const char *keep)
{
    char dirp[ST_PATH];
    DIR *d;
    struct dirent *de;
    struct m { char id[RFM_MOVE_FW_ID_MAX], version[32], base[256]; } *m = NULL;
    int n = 0, cap = 0, i, j;
    st_path(dirp, sizeof dirp, "firmwares");
    if (!(d = opendir(dirp)))
        return;
    while ((de = readdir(d))) {
        char p[ST_PATH], *json;
        size_t len;
        const char *man;
        if (!ends_with(de->d_name, ".json") || de->d_name[0] == '.' || strlen(de->d_name) >= 250)
            continue;
        snprintf(p, sizeof p, "%s/%s", dirp, de->d_name);
        if (!(json = (char *)st_read(p, &len)))
            continue;
        if ((man = mj_find(json, "manifest"))) {
            if (n == cap) {
                struct m *x = realloc(m, sizeof *m * (size_t)(cap = cap ? cap * 2 : 64));
                if (!x) {
                    free(json);
                    break;
                }
                m = x;
            }
            if (!mj_string(man, "id", m[n].id, sizeof m[n].id) && !mj_string(man, "version", m[n].version,
                                                                             sizeof m[n].version)) {
                snprintf(m[n].base, sizeof m[n].base, "%.*s", (int)(strlen(de->d_name) - 5), de->d_name);
                n++;
            }
        }
        free(json);
    }
    closedir(d);
    for (i = 0; i < n; i++) {
        int older = 0;
        for (j = 0; j < n && !older; j++)
            older = j != i && !strcmp(m[i].id, m[j].id) && st_vercmp(m[j].version, m[i].version) > 0;
        if (older) {
            static const char *SUF[] = { ".json", ".rfm1m", ".licenses.txt", ".rfm1skin", ".rfm1skin.bg",
                                         ".rfm1skin.bgl" };
            char so[ST_PATH];
            size_t k;
            snprintf(so, sizeof so, "%s/%s.rfm1m", dirp, m[i].base);
            if (keep) {   /* a slot runs it: it stays until a later sweep */
                const char *p = keep;
                const size_t sl = strlen(so);
                int kept = 0;
                while (*p && !kept) {
                    const char *e = strchr(p, '\n');
                    const size_t l = e ? (size_t)(e - p) : strlen(p);
                    kept = l == sl && !strncmp(p, so, l);
                    p += l + (e ? 1 : 0);
                }
                if (kept)
                    continue;
            }
            for (k = 0; k < sizeof SUF / sizeof SUF[0]; k++) {
                char p[ST_PATH];
                snprintf(p, sizeof p, "%s/%s%s", dirp, m[i].base, SUF[k]);
                unlink(p);
            }
            fprintf(stderr, "catalog: %s %s removed (a later version is installed)\n", m[i].id, m[i].version);
        }
    }
    free(m);
}

/* ------------------------------------------------------------------ the catalog --- */
static int by_name(const void *a, const void *b)
{
    const cat_ent_t *x = a, *y = b;
    const int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->id, y->id);
}

void cat_free(cat_t *c)
{
    int i;
    if (!c)
        return;
    for (i = 0; i < c->n; i++)
        free(c->e[i].files);
    free(c->e);
    free(c->list);
    free(c->fw);
    free(c);
}

int cat_find(const cat_t *c, const char *id)
{
    int i;
    for (i = 0; c && i < c->n; i++)
        if (!strcmp(c->e[i].id, id))
            return i;
    return -1;
}

int cat_fw_find(const cat_t *c, const char *id)
{
    int i;
    for (i = 0; c && i < c->nfw; i++)
        if (!strcmp(c->fw[i].id, id))
            return i;
    return -1;
}

int cat_same_ids(const cat_t *a, const cat_t *b)
{
    int i;
    if (!a || !b || a->n != b->n)
        return 0;
    for (i = 0; i < a->n; i++)
        if (strcmp(a->e[i].id, b->e[i].id))
            return 0;
    return 1;
}

cat_t *cat_build(const char *keep)
{
    cat_t *c = calloc(1, sizeof *c);
    cat_ent_t *idx = NULL;
    int ni = -1, i;
    char p[ST_PATH], gen_root[40] = "", gen_b[40] = "";
    uint32_t h_root = 0, h_b = 0;
    if (!c)
        return NULL;
    sweep(keep);
    if (!(c->fw = calloc(ST_FW_MAX, sizeof *c->fw))) {
        free(c);
        return NULL;
    }
    c->nfw = st_fw_scan(c->fw, ST_FW_MAX);
    /* the index: the downloaded one or the bundled snapshot, the later generated */
    {
        cat_ent_t *a = NULL, *b = NULL;
        int na, nb;
        char sig[ST_PATH + 8];
        st_path(p, sizeof p, "catalog/index.json");
        st_path(sig, sizeof sig, "catalog/index.json.sig");
        na = read_index(p, sig, &a, gen_root, sizeof gen_root, &h_root, NULL, 0);
        snprintf(sig, sizeof sig, "%s.sig", g_bundled);
        nb = g_bundled[0] ? read_index(g_bundled, sig, &b, gen_b, sizeof gen_b, &h_b, NULL, 0) : -1;
        if (na >= 0 && (nb < 0 || strcmp(gen_root, gen_b) >= 0)) {
            idx = a, ni = na, c->from = 2, c->index_hash = h_root;
            snprintf(c->generated, sizeof c->generated, "%s", gen_root);
            for (i = 0; i < nb; i++)
                free(b[i].files);
            free(b);
        } else if (nb >= 0) {
            idx = b, ni = nb, c->from = 1, c->index_hash = h_b;
            snprintf(c->generated, sizeof c->generated, "%s", gen_b);
            for (i = 0; i < na; i++)
                free(a[i].files);
            free(a);
        }
    }
    if (ni < 0)
        ni = 0;
    /* the entries: the index's, then the installed ones it does not list */
    if (!(c->e = calloc((size_t)(ni + c->nfw) + 1, sizeof *c->e))) {
        for (i = 0; i < ni; i++)
            free(idx[i].files);
        free(idx);
        cat_free(c);
        return NULL;
    }
    for (i = 0; i < ni; i++) {
        int k;
        for (k = 0; k < c->n && strcmp(c->e[k].id, idx[i].id); k++)
            ;
        if (k < c->n) {   /* (an id twice: the first) */
            free(idx[i].files);
            continue;
        }
        c->e[c->n++] = idx[i];
    }
    free(idx);
    for (i = 0; i < c->nfw; i++) {
        const st_fw_t *f = &c->fw[i];
        int k = cat_find(c, f->id);
        cat_ent_t *e;
        if (k < 0) {
            e = &c->e[k = c->n++];
            memset(e, 0, sizeof *e);
            snprintf(e->id, sizeof e->id, "%s", f->id);
            snprintf(e->name, sizeof e->name, "%s", f->name);
            snprintf(e->version, sizeof e->version, "%s", f->version);
            snprintf(e->license, sizeof e->license, "%s", f->license);
            snprintf(e->source_url, sizeof e->source_url, "%s", f->source_url);
            snprintf(e->desc, sizeof e->desc, "%s %s. %s. Source: %s", f->name, f->version, f->license,
                     f->source_url);
            e->cat = 255;
        }
        e = &c->e[k];
        snprintf(e->installed, sizeof e->installed, "%s", f->version);
        /* the catalog's version is the one offered; an installed one as new or newer is not an update */
        if (st_vercmp(f->version, e->version) >= 0)
            snprintf(e->version, sizeof e->version, "%s", f->version);
    }
    qsort(c->e, (size_t)c->n, sizeof *c->e, by_name);
    if (!(c->list = calloc((size_t)c->n + 1, sizeof *c->list))) {
        cat_free(c);
        return NULL;
    }
    for (i = 0; i < c->n; i++) {
        cat_ent_t *e = &c->e[i];
        rfm_move_fw_t *l = &c->list[i];
        l->id = e->id;
        l->name = e->name;
        l->version = e->version;
        l->installed = e->installed[0] ? e->installed : NULL;
        l->desc = e->desc[0] ? e->desc : NULL;
        l->thumb = e->has_thumb ? e->thumb : NULL;
        l->cats = e->cats;
        l->cat = e->cat;
    }
    return c;
}

/* ------------------------------------------------------------------ downloads --- */
void cat_cancel(void)
{
    g_cancel = 1;
}

typedef struct {
    uint64_t done, total;
    void (*progress)(void *, int);
    void *ctx;
    int last;
} prog_t;

static void on_bytes(void *ctx, uint64_t got, uint64_t len)
{
    prog_t *p = ctx;
    int pct;
    (void)len;
    if (!p->progress || !p->total)
        return;
    pct = (int)((p->done + got) * 99u / p->total);
    pct = pct < 1 ? 1 : pct > 99 ? 99 : pct;
    if (pct != p->last)
        p->progress(p->ctx, p->last = pct);
}

/* url -> out (https.h; plain http:// only when the base URL is: one the owner set, as the default is https).
 * 0 ok; -1 err */
static int fetch(const char *base, const char *url, const char *out, uint64_t max, prog_t *p, char *err, int cap)
{
    return https_get(url, out, max, !strncmp(base, "http://", 7), p ? on_bytes : NULL, p, &g_cancel, err, cap);
}

int cat_fetch_index(uint32_t old_hash, int *changed, char *err, int cap)
{
    char base[512], url[640], part[ST_PATH], spart[ST_PATH], path[ST_PATH], spath[ST_PATH], gen[40], why[160];
    cat_ent_t *e = NULL;
    uint32_t h = 0;
    int n, i;
    *changed = 0;
    g_cancel = 0;
    cat_base_url(base, sizeof base);
    st_path(path, sizeof path, "catalog");
    st_mkdirs(path);
    st_path(part, sizeof part, "catalog/index.json.part");
    st_path(spart, sizeof spart, "catalog/index.json.sig.part");
    st_path(path, sizeof path, "catalog/index.json");
    st_path(spath, sizeof spath, "catalog/index.json.sig");
    snprintf(url, sizeof url, "%s/index.json", base);
    if (fetch(base, url, part, INDEX_MAX, NULL, err, cap) != 0)
        goto fail;
    snprintf(url, sizeof url, "%s/index.json.sig", base);
    if (fetch(base, url, spart, SIG_MAX, NULL, err, cap) != 0)
        goto fail;
    n = read_index(part, spart, &e, gen, sizeof gen, &h, why, sizeof why);
    for (i = 0; i < n; i++)
        free(e[i].files);
    free(e);
    if (n < 0) {   /* (unsigned, badly signed, or not an index: not kept) */
        snprintf(err, (size_t)cap, "%s/index.json refused: %s", base, why);
        goto fail;
    }
    /* the signature first: a crash between the two leaves a pair that does not match, refused, the bundled used */
    if (rename(spart, spath) != 0 || rename(part, path) != 0) {
        snprintf(err, (size_t)cap, "cannot write %s: %s", path, strerror(errno));
        goto fail;
    }
    *changed = h != old_hash;
    return 0;
fail:
    unlink(part);
    unlink(spart);
    return -1;
}

int cat_download(const char *id, const char *files, uint64_t total, void (*progress)(void *ctx, int pct), void *ctx,
                 char *err, int cap)
{
    char base[512], dl[ST_PATH], names[32][256];
    const char *p = files;
    uint64_t done = 0;
    int n = 0, i, rc = -1;
    g_cancel = 0;
    cat_base_url(base, sizeof base);
    st_path(dl, sizeof dl, "firmwares/.dl");
    st_mkdirs(dl);
    if (!files || !*files) {
        snprintf(err, (size_t)cap, "%s: the catalog has no files for it", id);
        return -1;
    }
    while (*p && n < 32) {   /* each file: fetched, its size and SHA-256 checked */
        char name[256], sha[80], url[1024], part[ST_PATH], got[65];
        unsigned long long size = 0;
        uint64_t have = 0;
        const char *e = strchr(p, '\n');
        if (sscanf(p, "%255s %79s %llu", name, sha, &size) != 3 || !safe_name(name)) {
            snprintf(err, (size_t)cap, "%s: a damaged file list", id);
            goto out;
        }
        p = e ? e + 1 : p + strlen(p);
        snprintf(names[n++], sizeof names[0], "%s", name);
        snprintf(url, sizeof url, "%s/%s", base, name);
        snprintf(part, sizeof part, "%s/%s.part", dl, name);
        {
            prog_t pr = { done, total, progress, ctx, -1 };
            if (fetch(base, url, part, size, &pr, err, cap) != 0)
                goto out;
        }
        if (sha256_file(part, got, &have) != 0) {
            snprintf(err, (size_t)cap, "%s: cannot read the download", name);
            goto out;
        }
        if (have != size || strcmp(got, sha)) {
            snprintf(err, (size_t)cap, "%s: refused: %s (%llu bytes, SHA-256 %.12s.., the catalog says %llu, %.12s..)",
                     name, have != size ? "not the catalog's size" : "not the catalog's SHA-256", (unsigned long long)have, got, size,
                     sha);
            goto out;
        }
        done += size;
    }
    /* all there and right: into firmwares/, in the index's order (the manifest last) */
    for (i = 0; i < n; i++) {
        char from[ST_PATH], to[ST_PATH];
        snprintf(from, sizeof from, "%s/%s.part", dl, names[i]);
        st_path(to, sizeof to, "firmwares/%s", names[i]);
        chmod(from, ends_with(names[i], ".rfm1m") ? 0755 : 0644);
        if (rename(from, to) != 0) {
            snprintf(err, (size_t)cap, "cannot install %s: %s", to, strerror(errno));
            goto out;
        }
    }
    rc = 0;
out:
    for (i = 0; i < n; i++) {   /* (what is left of this download: nothing, once installed) */
        char part[ST_PATH];
        snprintf(part, sizeof part, "%s/%s.part", dl, names[i]);
        unlink(part);
    }
    return rc;
}
