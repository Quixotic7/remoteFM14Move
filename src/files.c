/* SPDX-License-Identifier: MPL-2.0 */
/* The page's file browser: see files.h. */
#define _GNU_SOURCE
#include "files.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "minijson.h"

/* ------------------------------------------------------------------ the path rules --- */
/* one UTF-8 sequence at s (n bytes left): its length, 0 not valid (overlong, surrogates, past U+10FFFF) */
static int utf8_len(const unsigned char *s, size_t n)
{
    unsigned c = s[0];
    int k, i;
    unsigned v;
    if (c < 0x80)
        return 1;
    if (c >= 0xC2 && c <= 0xDF)
        k = 2, v = c & 0x1F;
    else if (c >= 0xE0 && c <= 0xEF)
        k = 3, v = c & 0x0F;
    else if (c >= 0xF0 && c <= 0xF4)
        k = 4, v = c & 0x07;
    else
        return 0;
    if (n < (size_t)k)
        return 0;
    for (i = 1; i < k; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return 0;
        v = v << 6 | (s[i] & 0x3F);
    }
    if ((k == 3 && (v < 0x800 || (v >= 0xD800 && v <= 0xDFFF))) || (k == 4 && (v < 0x10000 || v > 0x10FFFF)))
        return 0;
    return k;
}

static int seg_ok(const char *s, size_t n, int is_new, const char **why)
{
    size_t i;
    if (!n) {
        *why = "an empty name (// or a trailing /)";
        return -1;
    }
    if (n > FILES_SEG_MAX) {
        *why = "a name longer than 255 bytes";
        return -1;
    }
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) {
        *why = "a . or .. in the path";
        return -1;
    }
    if (s[0] == '.') {
        *why = "a hidden name (starting with .)";
        return -1;
    }
    for (i = 0; i < n;) {
        const unsigned char c = (unsigned char)s[i];
        int k;
        if (c < 0x20 || c == 0x7F) {
            *why = "a control character in the name";
            return -1;
        }
        if (c == '\\') {
            *why = "a \\ in the name";
            return -1;
        }
        if (is_new && strchr(":*?\"<>|", c)) {
            *why = "one of : * ? \" < > | in the name";
            return -1;
        }
        if (!(k = utf8_len((const unsigned char *)s + i, n - i))) {
            *why = "a name that is not UTF-8";
            return -1;
        }
        i += (size_t)k;
    }
    if (is_new && (s[0] == ' ' || s[n - 1] == ' ' || s[n - 1] == '.')) {
        *why = "a name that starts or ends with a space, or ends with .";
        return -1;
    }
    return 0;
}

int files_rel_ok(const char *rel, int is_new, const char **why)
{
    const char *p = rel;
    const size_t len = strlen(rel);
    static const char *dummy;
    if (!why)
        why = &dummy;
    if (len > FILES_PATH_MAX) {
        *why = "a path longer than 1024 bytes";
        return -1;
    }
    if (!len) {
        if (is_new) {
            *why = "no name";
            return -1;
        }
        return 0;   /* (the root) */
    }
    if (rel[0] == '/') {
        *why = "an absolute path";
        return -1;
    }
    for (;;) {
        const char *e = strchr(p, '/');
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        if (seg_ok(p, n, is_new && !e, why) != 0)
            return -1;
        if (!e)
            return 0;
        p = e + 1;
    }
}

int files_no_links(const char *root, const char *rel)
{
    char p[FILES_PATH_MAX + 600];
    size_t at;
    const size_t rl = strlen(root);
    struct stat st;
    if (rl + 2 + strlen(rel) >= sizeof p)
        return -1;
    memcpy(p, root, rl);
    at = rl;
    while (*rel) {
        const char *e = strchr(rel, '/');
        const size_t n = e ? (size_t)(e - rel) : strlen(rel);
        p[at++] = '/';
        memcpy(p + at, rel, n);
        at += n;
        p[at] = 0;
        if (lstat(p, &st) != 0)
            return 0;   /* (the rest does not exist: nothing further to follow) */
        if (S_ISLNK(st.st_mode))
            return -1;
        if (!e)
            break;
        rel = e + 1;
    }
    return 0;
}

int files_unescape(const char *in, size_t n, char *out, size_t cap)
{
    size_t i, k = 0;
    for (i = 0; i < n; i++) {
        int c = (unsigned char)in[i];
        if (c == '%') {
            int v = 0, j;
            if (i + 2 >= n)
                return -1;
            for (j = 1; j <= 2; j++) {
                const char x = in[i + (size_t)j];
                v = v * 16 + (x >= '0' && x <= '9' ? x - '0' : x >= 'a' && x <= 'f' ? x - 'a' + 10 : x >= 'A' && x <= 'F' ? x - 'A' + 10 : -1000);
            }
            if (v <= 0 || v > 255)
                return -1;
            c = v;
            i += 2;
        } else if (c == '+')
            c = ' ';
        if (k + 1 >= cap)
            return -1;
        out[k++] = (char)c;
    }
    out[k] = 0;
    return 0;
}

/* ------------------------------------------------------------------ what may change --- */
static const char *const AREAS[] = { "audio", "code", "data", "states", "move" };

/* the first segment of rel into seg (its length), and whether rel is that segment alone */
static size_t first(const char *rel, char *seg, size_t cap, int *alone)
{
    const char *e = strchr(rel, '/');
    size_t n = e ? (size_t)(e - rel) : strlen(rel);
    if (n >= cap)
        n = cap - 1;
    memcpy(seg, rel, n);
    seg[n] = 0;
    *alone = !e;
    return n;
}

/* rel is p or inside p (p a path), or (for a delete or a rename) p is inside rel */
static int touches(const char *rel, const char *p, int op)
{
    const size_t n = strlen(p), r = strlen(rel);
    if (!strncmp(rel, p, n) && (rel[n] == 0 || rel[n] == '/'))
        return 1;
    return (op == FILES_DELETE || op == FILES_RENAME) && r < n && !strncmp(p, rel, r) && p[r] == '/';
}

const char *files_refuse(const char *rel, int op, const files_use_t *u, char *why, size_t cap)
{
    char seg[64], p[128];
    int alone, s, i;
    if (!*rel) {
        snprintf(why, cap, "the RemoteFM1 folder itself does not change");
        return why;
    }
    first(rel, seg, sizeof seg, &alone);
    for (i = 0; i < 5 && strcmp(seg, AREAS[i]); i++) {}
    if (i == 5) {
        if (!strcmp(seg, "firmwares"))
            snprintf(why, cap, "firmwares/ is read-only from the page: a firmware is native code the Move runs, so only "
                               "the signed catalog installs one (Mute in the projects view)");
        else if (!strcmp(seg, "catalog"))
            snprintf(why, cap, "catalog/ is the catalog's signed index, as the Move keeps it: read-only");
        else if (alone && op != FILES_MKDIR)
            snprintf(why, cap, "the RemoteFM1 folder's own files are the Move's: read-only");
        else
            snprintf(why, cap, "only audio/, code/, data/, states/ and move/projects/ take changes from the page");
        return why;
    }
    if (alone && op != FILES_PUT && op != FILES_REPLACE) {   /* (a put into the area itself has rel = the area) */
        if (op == FILES_MKDIR)
            snprintf(why, cap, "%s/ is there already", seg);
        else
            snprintf(why, cap, "%s/ is a folder the Move needs", seg);
        return why;
    }
    if (!strcmp(seg, "move")) {
        if (strncmp(rel, "move/projects", 13) || (rel[13] && rel[13] != '/')) {
            snprintf(why, cap, "move/'s settings and library are the running Move's own: read-only (projects: move/projects/)");
            return why;
        }
        if (!rel[13] && op != FILES_PUT && op != FILES_REPLACE) {
            snprintf(why, cap, "move/projects/ is a folder the Move needs");
            return why;
        }
        if (u && u->open_project >= 0 && op != FILES_PUT && op != FILES_MKDIR) {
            const char *suf[2] = { "rfmmove", "fx.json" };
            for (i = 0; i < 2; i++) {
                snprintf(p, sizeof p, "move/projects/move-project-%02d.%s", u->open_project + 1, suf[i]);
                if (touches(rel, p, op)) {
                    snprintf(why, cap, "project %d is the open project on the Move: open another one first", u->open_project + 1);
                    return why;
                }
            }
        }
        return NULL;
    }
    for (s = 0; u && s < 3; s++) {
        const char *id = u->ids[s];
        if (!id[0])
            continue;
        snprintf(p, sizeof p, "states/%s", id);
        if (touches(rel, p, op)) {
            snprintf(why, cap, "%s runs in slot %d and keeps its states there: unload it first", id, s + 1);
            return why;
        }
        if (op == FILES_PUT || op == FILES_MKDIR)
            continue;
        for (i = 0; i < 2; i++) {
            snprintf(p, sizeof p, "%s/%s", i ? "code" : "data", id);
            if (touches(rel, p, op)) {
                snprintf(why, cap, "%s runs in slot %d and may have these files open: unload it first (new files are fine)", id, s + 1);
                return why;
            }
        }
    }
    return NULL;
}

int files_writable(const char *rel)
{
    char why[256];
    return !files_refuse(rel, FILES_PUT, NULL, why, sizeof why);
}

/* ------------------------------------------------------------------ the listing --- */
typedef struct { char *name; uint64_t size; long mtime; int dir; } ent_t;
static int by_ent(const void *a, const void *b)
{
    const ent_t *x = a, *y = b;
    if (x->dir != y->dir)
        return y->dir - x->dir;
    return strcmp(x->name, y->name);
}

/* the entries of dir (no hidden, no links, no names the rules refuse), sorted: count, -1 not a folder */
static int scan(const char *dir, ent_t **out)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    ent_t *v = NULL;
    int n = 0, cap = 0;
    *out = NULL;
    if (!d)
        return -1;
    while ((e = readdir(d))) {
        char p[FILES_PATH_MAX + 900];
        struct stat st;
        const char *why;
        if (e->d_name[0] == '.' || seg_ok(e->d_name, strlen(e->d_name), 0, &why) != 0)
            continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (lstat(p, &st) != 0 || !(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)))
            continue;   /* (links, sockets, devices: not shown) */
        if (n == cap) {
            ent_t *nv = realloc(v, (size_t)(cap = cap ? cap * 2 : 32) * sizeof *v);
            if (!nv)
                break;
            v = nv;
        }
        v[n].name = strdup(e->d_name);
        v[n].size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
        v[n].mtime = (long)st.st_mtime;
        v[n].dir = S_ISDIR(st.st_mode);
        if (v[n].name)
            n++;
        if (n >= 100000)
            break;
    }
    closedir(d);
    if (n)
        qsort(v, (size_t)n, sizeof *v, by_ent);
    *out = v;
    return n;
}
static void scan_free(ent_t *v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        free(v[i].name);
    free(v);
}

char *files_list_json(const char *root, const char *rel)
{
    char dir[FILES_PATH_MAX + 600], esc[FILES_SEG_MAX * 6 + 8], why[256];
    ent_t *v;
    int n, i;
    size_t o = 0, cap = 4096;
    char *out;
    snprintf(dir, sizeof dir, "%s%s%s", root, *rel ? "/" : "", rel);
    if ((n = scan(dir, &v)) < 0)
        return NULL;
    if (!(out = malloc(cap))) {
        scan_free(v, n);
        return NULL;
    }
    {
        char *erel = malloc(strlen(rel) * 6 + 8);
        const char *no = files_refuse(*rel ? rel : "x", FILES_PUT, NULL, why, sizeof why);
        if (!erel) {
            free(out);
            scan_free(v, n);
            return NULL;
        }
        mj_escape(rel, erel, strlen(rel) * 6 + 8);
        mj_escape(no ? why : "", esc, sizeof esc);
        o += (size_t)snprintf(out + o, cap - o, "{\"path\":\"%s\",\"writable\":%s,\"why\":\"%s\",\"entries\":[", erel,
                              no || !*rel ? "false" : "true", !*rel ? "the RemoteFM1 folder's own files are the Move's: read-only" : esc);
        free(erel);
    }
    for (i = 0; i < n; i++) {
        mj_escape(v[i].name, esc, sizeof esc);
        if (o + strlen(esc) + 128 > cap) {
            char *b = realloc(out, cap = cap * 2 + strlen(esc) + 128);
            if (!b)
                break;
            out = b;
        }
        o += (size_t)snprintf(out + o, cap - o, "%s{\"name\":\"%s\",\"size\":%llu,\"mtime\":%ld,\"dir\":%s}", i ? "," : "",
                              esc, (unsigned long long)v[i].size, v[i].mtime, v[i].dir ? "true" : "false");
    }
    if (o + 4 > cap) {
        char *b = realloc(out, cap = o + 4);
        if (b)
            out = b;
    }
    snprintf(out + o, cap - o, "]");
    scan_free(v, n);
    return out;   /* (the caller closes it: "]" then its own fields and "}") */
}

/* ------------------------------------------------------------------ the zip --- */
static uint32_t CRC[256];
uint32_t files_crc32(uint32_t crc, const uint8_t *b, size_t n)
{
    size_t i;
    if (!CRC[1]) {
        uint32_t k, c;
        int j;
        for (k = 0; k < 256; k++) {
            for (c = k, j = 0; j < 8; j++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            CRC[k] = c;
        }
    }
    crc = ~crc;
    for (i = 0; i < n; i++)
        crc = CRC[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

typedef struct {
    char *zname;              /* the name inside ("states/x/flash.bin", a folder's with a trailing /) */
    char *path;               /* on disk */
    uint32_t size, crc, off;  /* planned size; the CRC of what was sent; its local header's offset */
    uint16_t dtime, ddate;
    int dir;
} zent_t;

struct files_zip {
    zent_t *e;
    int n, cap;
    int i, phase;             /* the entry; 0 its local header, 1 its data, 2 its descriptor; then 3 the central
                               * directory (i again), 4 the end record, 5 done */
    int fd;
    uint32_t left, done;      /* (phase 1) bytes of the entry to send, sent */
    uint32_t pos;             /* bytes of the archive sent */
    uint32_t cd_start, cd_size;
    uint8_t hdr[46 + 65536];  /* the record being sent (a header with its name) */
    uint32_t hn, ho;
};

static void dos_time(long t, uint16_t *tm, uint16_t *dt)
{
    struct tm lt;
    time_t tt = (time_t)t;
    localtime_r(&tt, &lt);
    if (lt.tm_year < 80)
        lt.tm_year = 80, lt.tm_mon = 0, lt.tm_mday = 1, lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    *tm = (uint16_t)(lt.tm_hour << 11 | lt.tm_min << 5 | lt.tm_sec / 2);
    *dt = (uint16_t)((lt.tm_year - 80) << 9 | (lt.tm_mon + 1) << 5 | lt.tm_mday);
}

static int zadd(files_zip_t *z, const char *zname, const char *path, uint64_t size, long mtime, int dir)
{
    zent_t *x;
    if (z->n == z->cap) {
        zent_t *ne = realloc(z->e, (size_t)(z->cap = z->cap ? z->cap * 2 : 64) * sizeof *ne);
        if (!ne)
            return -1;
        z->e = ne;
    }
    x = &z->e[z->n];
    memset(x, 0, sizeof *x);
    if (!(x->zname = strdup(zname)) || !(x->path = strdup(path)))
        return -1;
    x->size = (uint32_t)size;
    x->dir = dir;
    dos_time(mtime, &x->dtime, &x->ddate);
    z->n++;
    return 0;
}

/* everything under dir (zname its name inside, with a trailing /), depth first; total: the bytes so far */
static int zwalk(files_zip_t *z, const char *dir, const char *zname, uint64_t *total, int depth, const char **why)
{
    ent_t *v;
    int n = scan(dir, &v), i, rc = 0;
    if (n < 0)
        return 0;
    if (depth > 32) {
        scan_free(v, n);
        *why = "folders nested too deep";
        return -1;
    }
    for (i = 0; i < n && !rc; i++) {
        char *p = malloc(strlen(dir) + strlen(v[i].name) + 2), *zn = malloc(strlen(zname) + strlen(v[i].name) + 2);
        if (!p || !zn) {
            free(p), free(zn);
            rc = -1;
            break;
        }
        sprintf(p, "%s/%s", dir, v[i].name);
        sprintf(zn, "%s%s%s", zname, v[i].name, v[i].dir ? "/" : "");
        if (v[i].size > 0xFFFFFFFFull - 65536) {
            *why = "a file over 4 GB (a zip without zip64)";
            rc = -1;
        } else if (zadd(z, zn, p, v[i].size, v[i].mtime, v[i].dir) != 0)
            rc = -1;
        else {
            *total += 30 + 46 + 2 * strlen(zn) + v[i].size + (v[i].dir ? 0 : 16);
            if (*total > 0xFFFFFFFFull - 65536 || z->n > 65000) {
                *why = "more than 4 GB or 65000 files (a zip without zip64): pick a smaller folder";
                rc = -1;
            } else if (v[i].dir)
                rc = zwalk(z, p, zn, total, depth + 1, why);
        }
        free(p), free(zn);
    }
    scan_free(v, n);
    return rc;
}

static void record(files_zip_t *z);
files_zip_t *files_zip_plan(const char *root, const char *rel, uint64_t *length, const char **why)
{
    files_zip_t *z = calloc(1, sizeof *z);
    char dir[FILES_PATH_MAX + 600], top[FILES_SEG_MAX + 2];
    const char *slash = strrchr(rel, '/');
    struct stat st;
    uint64_t total = 22;
    if (!z) {
        *why = "no memory";
        return NULL;
    }
    z->fd = -1;
    snprintf(dir, sizeof dir, "%s%s%s", root, *rel ? "/" : "", rel);
    if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        *why = "no such folder";
        free(z);
        return NULL;
    }
    snprintf(top, sizeof top, "%s/", *rel ? (slash ? slash + 1 : rel) : "RemoteFM1");
    total += 30 + 46 + 2 * strlen(top);
    if (zadd(z, top, dir, 0, (long)st.st_mtime, 1) != 0 || zwalk(z, dir, top, &total, 0, why) != 0) {
        if (!*why)
            *why = "no memory";
        files_zip_free(z);
        return NULL;
    }
    *length = total;
    *why = NULL;
    record(z);   /* (the first local header) */
    return z;
}

static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { le16(p, v & 0xFFFF), le16(p + 2, v >> 16); }

/* the next record into z->hdr (z->hn bytes) for the phase that needs one */
static void record(files_zip_t *z)
{
    zent_t *x = z->i < z->n ? &z->e[z->i] : NULL;
    uint8_t *h = z->hdr;
    const uint16_t flags = (uint16_t)(0x0800 | (x && !x->dir ? 0x0008 : 0));   /* UTF-8; a descriptor follows the data */
    size_t nl = x ? strlen(x->zname) : 0;
    z->ho = 0;
    if (z->phase == 0) {   /* local header: CRC and sizes in the descriptor (folders: zero, none) */
        x->off = z->pos;
        le32(h, 0x04034b50), le16(h + 4, x->dir ? 10 : 20), le16(h + 6, flags), le16(h + 8, 0);
        le16(h + 10, x->dtime), le16(h + 12, x->ddate), le32(h + 14, 0), le32(h + 18, 0), le32(h + 22, 0);
        le16(h + 26, (uint32_t)nl), le16(h + 28, 0);
        memcpy(h + 30, x->zname, nl);
        z->hn = (uint32_t)(30 + nl);
    } else if (z->phase == 2) {
        le32(h, 0x08074b50), le32(h + 4, x->crc), le32(h + 8, x->size), le32(h + 12, x->size);
        z->hn = 16;
    } else if (z->phase == 3) {
        le32(h, 0x02014b50), le16(h + 4, 0x031E), le16(h + 6, x->dir ? 10 : 20), le16(h + 8, flags), le16(h + 10, 0);
        le16(h + 12, x->dtime), le16(h + 14, x->ddate), le32(h + 16, x->crc), le32(h + 20, x->size), le32(h + 24, x->size);
        le16(h + 28, (uint32_t)nl), le16(h + 30, 0), le16(h + 32, 0), le16(h + 34, 0), le16(h + 36, 0);
        le32(h + 38, x->dir ? 040755u << 16 | 0x10 : 0100644u << 16), le32(h + 42, x->off);
        memcpy(h + 46, x->zname, nl);
        z->hn = (uint32_t)(46 + nl);
    } else {   /* the end of the central directory */
        le32(h, 0x06054b50), le16(h + 4, 0), le16(h + 6, 0), le16(h + 8, (uint32_t)z->n), le16(h + 10, (uint32_t)z->n);
        le32(h + 12, z->cd_size), le32(h + 16, z->cd_start), le16(h + 20, 0);
        z->hn = 22;
    }
}

long files_zip_read(files_zip_t *z, uint8_t *buf, size_t cap)
{
    size_t got = 0;
    while (got < cap && z->phase < 5) {
        if (z->phase == 1) {   /* a file's bytes: as planned (cut, or padded with zeros) */
            zent_t *x = &z->e[z->i];
            size_t want = cap - got;
            ssize_t r = 0;
            if (want > z->left)
                want = z->left;
            if (z->fd >= 0 && want) {
                r = read(z->fd, buf + got, want);
                if (r < 0)
                    return -1;
                if (r == 0) {
                    close(z->fd);
                    z->fd = -1;
                }
            }
            if (z->fd < 0 && want && r == 0) {   /* (shorter than planned: zeros) */
                memset(buf + got, 0, want);
                r = (ssize_t)want;
            }
            x->crc = files_crc32(x->crc, buf + got, (size_t)r);
            got += (size_t)r, z->left -= (uint32_t)r, z->pos += (uint32_t)r;
            if (!z->left) {
                if (z->fd >= 0)
                    close(z->fd);
                z->fd = -1;
                z->phase = 2;
                record(z);
            }
            continue;
        }
        if (z->ho < z->hn) {   /* a record under way */
            size_t k = z->hn - z->ho;
            if (k > cap - got)
                k = cap - got;
            memcpy(buf + got, z->hdr + z->ho, k);
            z->ho += (uint32_t)k, got += k, z->pos += (uint32_t)k;
            if (z->phase == 3)
                z->cd_size += (uint32_t)k;
            continue;
        }
        /* the record is out: the next step */
        if (z->phase == 0) {
            zent_t *x = &z->e[z->i];
            if (x->dir) {
                if (++z->i >= z->n)
                    z->phase = 3, z->i = 0, z->cd_start = z->pos;
                record(z);
                continue;
            }
            z->fd = open(x->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            z->left = x->size;
            x->crc = 0;
            z->phase = 1;
            if (!z->left) {
                if (z->fd >= 0)
                    close(z->fd);
                z->fd = -1;
                z->phase = 2;
                record(z);
            }
            continue;
        }
        if (z->phase == 2) {
            if (++z->i >= z->n)
                z->phase = 3, z->i = 0, z->cd_start = z->pos;
            else
                z->phase = 0;
            record(z);
            continue;
        }
        if (z->phase == 3) {
            if (++z->i >= z->n)
                z->phase = 4;
            record(z);
            continue;
        }
        z->phase = 5;   /* (phase 4's record went out) */
    }
    return (long)got;
}

void files_zip_free(files_zip_t *z)
{
    int i;
    if (!z)
        return;
    if (z->fd >= 0)
        close(z->fd);
    for (i = 0; i < z->n; i++)
        free(z->e[i].zname), free(z->e[i].path);
    free(z->e);
    free(z);
}

int files_rmtree(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    int rc = 0;
    if (lstat(path, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    if (!(d = opendir(path)))
        return -1;
    while ((e = readdir(d)) && !rc) {
        char *p;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (!(p = malloc(strlen(path) + strlen(e->d_name) + 2))) {
            rc = -1;
            break;
        }
        sprintf(p, "%s/%s", path, e->d_name);
        rc = files_rmtree(p);
        free(p);
    }
    closedir(d);
    return rc ? rc : rmdir(path);
}
