/* SPDX-License-Identifier: MPL-2.0 */
/* The page's Update: see update.h. */
#define _GNU_SOURCE
#include "update.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "ed25519.h"
#include "gz.h"
#include "minijson.h"
#include "sha256.h"
#include "update_key.h"
#ifdef RFM14_TEST_BUILD
#include "update_test_key.h"   /* build/testkey (the Makefile): UPD_TEST_KEY */
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/* ------------------------------------------------------------------------------------------- small helpers --- */
static void say(char *why, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(char *why, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!why || !cap)
        return;
    va_start(ap, fmt);
    vsnprintf(why, cap, fmt, ap);
    va_end(ap);
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static int read_all(int fd, void *p, size_t n)
{
    uint8_t *b = p;
    while (n) {
        const ssize_t r = read(fd, b, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        b += r, n -= (size_t)r;
    }
    return 0;
}
static int write_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        const ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        b += w, n -= (size_t)w;
    }
    return 0;
}
static int is_dir(const char *p)
{
    struct stat st;
    return lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)st, (void)f;
    return (flag == FTW_DP ? rmdir(p) : unlink(p)) != 0 && errno != ENOENT ? -1 : 0;
}
int upd_rmtree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    return nftw(path, rm_one, 16, FTW_DEPTH | FTW_PHYS);
}
static uint64_t g_du;
static int du_one(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)p, (void)f;
    if (flag == FTW_F && S_ISREG(st->st_mode))
        g_du += (uint64_t)st->st_size;
    return 0;
}
static uint64_t du(const char *path)   /* (the worker's, or the web thread's at an upload's start: g_mu held) */
{
    g_du = 0;
    nftw(path, du_one, 16, FTW_PHYS);
    return g_du;
}
static uint64_t disk_free(const char *dir)
{
    struct statvfs v;
#ifdef RFM14_TEST_BUILD
    {
        const char *e = getenv("RFM14_TEST_DISK_FREE_MB");
        if (e && *e)
            return (uint64_t)atoll(e) << 20;
    }
#endif
    return statvfs(dir, &v) == 0 ? (uint64_t)v.f_bavail * v.f_frsize : 0;
}

/* ------------------------------------------------------------------------------------------- the file --- */
void upd_manifest_free(upd_manifest_t *m)
{
    if (m) {
        free(m->json);
        m->json = NULL;
    }
}

int upd_check_file(const char *path, const uint8_t (*keys)[32], int nkeys, upd_manifest_t *m, char *why, size_t cap)
{
    uint8_t head[UPD_HEAD], sig[64], digest[32], msg[14 + 32], buf[65536];
    struct stat st;
    sha256_t h;
    uint64_t left;
    int fd, i, ok = 0;
    const char *it;
    memset(m, 0, sizeof *m);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) != 0) {
        say(why, cap, "cannot read the file");
        if (fd >= 0)
            close(fd);
        return -1;
    }
    if (st.st_size < (off_t)(UPD_HEAD + 64) || read_all(fd, head, UPD_HEAD) != 0 || memcmp(head, UPD_MAGIC, 8)) {
        say(why, cap, "not an update file (.rfm1u)");
        close(fd);
        return -1;
    }
    if (le32(head + 8) != UPD_FORMAT) {
        say(why, cap, "an update file of another format (%u): this host reads format %u", le32(head + 8), UPD_FORMAT);
        close(fd);
        return -1;
    }
    m->mlen = le32(head + 12);
    m->plen = le64(head + 16);
    if (m->mlen == 0 || m->mlen > UPD_MAN_MAX || m->plen == 0 || m->plen > UPD_MAX ||
        (uint64_t)st.st_size != UPD_HEAD + (uint64_t)m->mlen + 64u + m->plen) {
        say(why, cap, "a damaged update file (its sizes do not add up)");
        close(fd);
        return -1;
    }
    m->poff = UPD_HEAD + (uint64_t)m->mlen + 64u;
    if (!(m->json = malloc(m->mlen + 1u)) || read_all(fd, m->json, m->mlen) != 0 || read_all(fd, sig, 64) != 0) {
        say(why, cap, "cannot read the file");
        upd_manifest_free(m);
        close(fd);
        return -1;
    }
    m->json[m->mlen] = 0;
    sha256_init(&h);
    sha256_update(&h, head, UPD_HEAD);
    sha256_update(&h, m->json, m->mlen);
    for (left = m->plen; left;) {
        const size_t k = left > sizeof buf ? sizeof buf : (size_t)left;
        if (read_all(fd, buf, k) != 0) {
            say(why, cap, "cannot read the file");
            upd_manifest_free(m);
            close(fd);
            return -1;
        }
        sha256_update(&h, buf, k);
        left -= k;
    }
    close(fd);
    sha256_final(&h, digest);
    memcpy(msg, UPD_DOMAIN, 14);
    memcpy(msg + 14, digest, 32);
    for (i = 0; i < nkeys && !ok; i++)
        ok = ed25519_verify(sig, msg, sizeof msg, keys[i]) == 0;
    if (!ok) {
        say(why, cap, "the signature is not good: not an update signed for this Move (nothing was changed)");
        upd_manifest_free(m);
        return -1;
    }
    if (mj_string(m->json, "version", m->version, sizeof m->version) != 0 || !m->version[0]) {
        say(why, cap, "the update's manifest names no version");
        upd_manifest_free(m);
        return -1;
    }
    mj_string(m->json, "host_commit", m->commit, sizeof m->commit);
    mj_string(m->json, "core_commit", m->core, sizeof m->core);
    mj_string(m->json, "built", m->built, sizeof m->built);
    mj_string(m->json, "kind", m->kind, sizeof m->kind);
    if ((it = mj_find(m->json, "files")) && *it == '[')
        while (mj_elem(&it))
            m->nfiles++;
    return 0;
}

/* ------------------------------------------------------------------------------------------- the payload --- */
int upd_name(const char *in, char *out, size_t cap, const char **why)
{
    const char *p = in, *c;
    size_t n;
    while (p[0] == '.' && p[1] == '/')
        p += 2;
    n = strlen(p);
    if (n && p[n - 1] == '/')
        n--;
    if (!n || (n == 1 && p[0] == '.'))
        return 1;
    if (p[0] == '/') {
        *why = "an absolute name";
        return -1;
    }
    if (n + 1 > cap || n > 900) {
        *why = "a name too long";
        return -1;
    }
    memcpy(out, p, n);
    out[n] = 0;
    for (c = out; *c; c++)
        if ((unsigned char)*c < 0x20 || *c == 0x7f) {
            *why = "a control character in a name";
            return -1;
        }
    for (c = out;;) {   /* each part */
        const char *e = strchr(c, '/');
        const size_t k = e ? (size_t)(e - c) : strlen(c);
        if (k == 0 || (k == 1 && c[0] == '.')) {
            *why = "an empty or \".\" part in a name";
            return -1;
        }
        if (k == 2 && c[0] == '.' && c[1] == '.') {
            *why = "a \"..\" in a name";
            return -1;
        }
        if (k >= 8 && !strncmp(c, ".update-", 8)) {
            *why = "a name under .update- (reserved)";
            return -1;
        }
        if ((k >= 2 && c[0] == '.' && c[1] == '_') || (k == 9 && !strncmp(c, ".DS_Store", 9)))
            return 1;
        if (!e)
            break;
        c = e + 1;
    }
    return 0;
}

typedef struct {
    const char *dest;          /* NULL: a scan */
    int strip;                 /* (unpack) the remotefm1/ dropped; (scan) every name under it so far */
    int seen;                  /* (scan) a name seen */
    uint8_t hdr[512];
    size_t hn;
    uint64_t data_left, pad_left;
    int kind;                  /* the data now: 0 skipped, 1 a file's (fd), 2 a long name, 3 pax records */
    int fd;
    char lname[1024];
    size_t ln;
    int have_long;
    char pax[4096];
    size_t pn;
    int ended, entries, files;
    uint64_t bytes;
    char why[256];
} tarx_t;

static uint64_t octal(const uint8_t *p, size_t n, int *bad)
{
    uint64_t v = 0;
    size_t i = 0;
    if (p[0] & 0x80) {
        *bad = 1;
        return 0;
    }
    while (i < n && p[i] == ' ')
        i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++)
        v = v * 8 + (uint64_t)(p[i] - '0');
    if (i < n && p[i] != 0 && p[i] != ' ')
        *bad = 1;
    return v;
}
/* the folders of rel (and rel itself when it is a folder) made under dest; a non-folder in the way refuses */
static int mkparents(tarx_t *t, const char *rel, int self)
{
    char p[PATH_MAX];
    const size_t base = (size_t)snprintf(p, sizeof p, "%s/", t->dest);
    size_t i;
    const size_t n = strlen(rel);
    if (base + n + 1 > sizeof p) {
        say(t->why, sizeof t->why, "a name too long: %s", rel);
        return -1;
    }
    memcpy(p + base, rel, n + 1);
    for (i = base; i <= base + n; i++) {
        struct stat st;
        if (p[i] != '/' && !(self && p[i] == 0))
            continue;
        if (p[i] == '/' && i == base)
            continue;
        {
            const char c = p[i];
            p[i] = 0;
            if (lstat(p, &st) != 0) {
                if (errno != ENOENT || mkdir(p, 0755) != 0) {
                    say(t->why, sizeof t->why, "cannot make the folder %s", p + base);
                    return -1;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                say(t->why, sizeof t->why, "%s is in the way of a folder", p + base);
                return -1;
            }
            p[i] = c;
        }
    }
    return 0;
}
static int entry_name(tarx_t *t, char *name, size_t cap)
{
    if (t->have_long) {
        t->have_long = 0;
        snprintf(name, cap, "%s", t->lname);
    } else if (!memcmp(t->hdr + 257, "ustar", 5) && t->hdr[345])
        snprintf(name, cap, "%.155s/%.100s", (const char *)t->hdr + 345, (const char *)t->hdr);
    else
        snprintf(name, cap, "%.100s", (const char *)t->hdr);
    return 0;
}
/* a whole header: what follows it set up; -1 refused (t->why) */
static int tar_header(tarx_t *t)
{
    char raw[1200], rel[1024];
    const char *why = NULL;
    uint64_t size, sum = 0, want;
    int bad = 0, i, r;
    const uint8_t type = t->hdr[156];
    for (i = 0; i < 512 && !t->hdr[i]; i++) {}
    if (i == 512) {   /* the end */
        t->ended = 1;
        return 0;
    }
    for (i = 0; i < 512; i++)
        sum += i >= 148 && i < 156 ? ' ' : t->hdr[i];
    want = octal(t->hdr + 148, 8, &bad);
    size = octal(t->hdr + 124, 12, &bad);
    if (bad || want != sum) {
        say(t->why, sizeof t->why, "a damaged tar header in the payload");
        return -1;
    }
    if (++t->entries > UPD_ENTRIES_MAX) {
        say(t->why, sizeof t->why, "more than %d entries in the payload", UPD_ENTRIES_MAX);
        return -1;
    }
    t->data_left = size;
    t->pad_left = (512u - size % 512u) % 512u;
    t->kind = 0;
    if (type == 'L' || type == 'x') {   /* a long name, or pax records: for the next entry */
        if (size >= (type == 'L' ? sizeof t->lname : sizeof t->pax)) {
            say(t->why, sizeof t->why, "a name too long in the payload");
            return -1;
        }
        t->kind = type == 'L' ? 2 : 3;
        t->ln = t->pn = 0;
        return 0;
    }
    if (type == 'g' || type == 'K')   /* (global pax records, a long link name: nothing for us) */
        return 0;
    entry_name(t, raw, sizeof raw);
    if (type != '0' && type != 0 && type != '7' && type != '5') {
        say(t->why, sizeof t->why, "the payload holds a %s (%.200s): refused",
            type == '1' || type == '2' ? "link" : type == '3' || type == '4' ? "device" : type == '6' ? "FIFO"
                                                                                       : "special entry", raw);
        return -1;
    }
    r = upd_name(raw, rel, sizeof rel, &why);
    if (r < 0) {
        say(t->why, sizeof t->why, "%s (%.200s): refused", why, raw);
        return -1;
    }
    if (r > 0)
        return 0;   /* (skipped: its data too) */
    if (!t->dest) {   /* a scan: the names */
        const int under = !strcmp(rel, "remotefm1") || !strncmp(rel, "remotefm1/", 10);
        t->strip = (t->seen ? t->strip : 1) && under;
        t->seen = 1;
        if (type != '5') {
            t->files++;
            t->bytes += size;
            if (t->bytes > UPD_UNPACKED_MAX) {
                say(t->why, sizeof t->why, "the payload unpacks to more than 1 GB");
                return -1;
            }
        }
        return 0;
    }
    {
        const char *name = rel;
        char full[PATH_MAX];
        struct stat st;
        int mode;
        if (t->strip) {
            if (!strcmp(rel, "remotefm1"))
                return 0;
            name = rel + 10;
        }
        if (type == '5')
            return mkparents(t, name, 1);
        if (mkparents(t, name, 0) != 0)
            return -1;
        if ((size_t)snprintf(full, sizeof full, "%s/%s", t->dest, name) >= sizeof full) {
            say(t->why, sizeof t->why, "a name too long: %.200s", name);
            return -1;
        }
        if (lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                say(t->why, sizeof t->why, "%.200s is a folder in the module", name);
                return -1;
            }
            unlink(full);
        }
        mode = octal(t->hdr + 100, 8, &bad) & 0111 ? 0755 : 0644;
        t->fd = open(full, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
        if (t->fd < 0) {
            t->fd = 0;
            say(t->why, sizeof t->why, "cannot write %.200s", name);
            return -1;
        }
        fchmod(t->fd, (mode_t)mode);
        t->kind = 1;
        t->files++;
        t->bytes += size;
        if (!size) {
            close(t->fd);
            t->fd = 0;
        }
        return 0;
    }
}
static void pax_path(tarx_t *t)
{
    size_t i = 0;
    while (i < t->pn) {   /* records: "<len> key=value\n" */
        size_t len = 0, j = i;
        while (j < t->pn && t->pax[j] >= '0' && t->pax[j] <= '9')
            len = len * 10 + (size_t)(t->pax[j++] - '0');
        if (!len || i + len > t->pn || j >= t->pn || t->pax[j] != ' ')
            return;
        if (len > (j + 1 - i) + 5 && !strncmp(t->pax + j + 1, "path=", 5)) {
            const size_t vs = j + 6, vn = i + len - 1 - vs;
            if (vn < sizeof t->lname) {
                memcpy(t->lname, t->pax + vs, vn);
                t->lname[vn] = 0;
                t->have_long = 1;
            }
        }
        i += len;
    }
}
static int tar_sink(void *ctx, const uint8_t *p, size_t n)
{
    tarx_t *t = ctx;
    while (n && !t->ended) {
        if (t->data_left) {
            size_t k = n < t->data_left ? n : (size_t)t->data_left;
            if (t->kind == 1 && t->fd > 0 && write_all(t->fd, p, k) != 0) {
                say(t->why, sizeof t->why, "a write failed (the disk full?)");
                return -1;
            }
            if (t->kind == 2) {
                memcpy(t->lname + t->ln, p, k);
                t->ln += k;
            } else if (t->kind == 3) {
                memcpy(t->pax + t->pn, p, k);
                t->pn += k;
            }
            p += k, n -= k, t->data_left -= k;
            if (!t->data_left) {
                if (t->kind == 1 && t->fd > 0) {
                    if (close(t->fd) != 0) {
                        t->fd = 0;
                        say(t->why, sizeof t->why, "a write failed (the disk full?)");
                        return -1;
                    }
                    t->fd = 0;
                } else if (t->kind == 2) {
                    t->lname[t->ln < sizeof t->lname ? t->ln : sizeof t->lname - 1] = 0;
                    t->have_long = 1;
                } else if (t->kind == 3)
                    pax_path(t);
                t->kind = 0;
            }
            continue;
        }
        if (t->pad_left) {
            const size_t k = n < t->pad_left ? n : (size_t)t->pad_left;
            p += k, n -= k, t->pad_left -= k;
            continue;
        }
        {
            const size_t k = n < 512 - t->hn ? n : 512 - t->hn;
            memcpy(t->hdr + t->hn, p, k);
            t->hn += k, p += k, n -= k;
            if (t->hn == 512) {
                t->hn = 0;
                if (tar_header(t) != 0)
                    return -1;
            }
        }
    }
    return 0;
}

int upd_payload(const char *path, const upd_manifest_t *m, const char *dest, int *strip, int *files, uint64_t *bytes,
                char *why, size_t cap)
{
    tarx_t *t = calloc(1, sizeof *t);
    int fd, rc;
    if (!t) {
        say(why, cap, "no memory");
        return -1;
    }
    t->dest = dest;
    t->strip = dest ? *strip : 0;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || lseek(fd, (off_t)m->poff, SEEK_SET) != (off_t)m->poff) {
        say(why, cap, "cannot read the file");
        if (fd >= 0)
            close(fd);
        free(t);
        return -1;
    }
    rc = gz_inflate_fd(fd, m->plen, tar_sink, t, why, cap);
    close(fd);
    if (t->fd > 0)
        close(t->fd);
    if (rc != 0 && t->why[0])
        say(why, cap, "%s", t->why);
    if (rc == 0 && !t->ended && (t->hn || t->data_left || t->pad_left)) {
        say(why, cap, "the payload's tar is cut short");
        rc = -1;
    }
    if (rc == 0 && !t->files) {
        say(why, cap, "the payload holds no files");
        rc = -1;
    }
    if (rc == 0) {
        if (!dest && strip)
            *strip = t->seen && t->strip;
        if (files)
            *files = t->files;
        if (bytes)
            *bytes = t->bytes;
    }
    free(t);
    return rc;
}

/* ------------------------------------------------------------------------------------------- the backup --- */
typedef struct {
    gz_writer_t *w;
    const char *root;
    int err;
} tarw_t;

static void tar_put_header(tarw_t *t, const char *name, int type, uint64_t size, unsigned mode, time_t mtime)
{
    uint8_t h[512];
    const size_t n = strlen(name);
    unsigned sum = 0;
    int i;
    memset(h, 0, sizeof h);
    if (n <= 100)
        memcpy(h, name, n);
    else {
        size_t cut = 0, k;
        for (k = n - 1; k > 0; k--)   /* the prefix's last '/' that leaves a name of 100 or less */
            if (name[k] == '/' && n - k - 1 <= 100 && k <= 155) {
                cut = k;
                break;
            }
        if (cut) {
            memcpy(h + 345, name, cut);
            memcpy(h, name + cut + 1, n - cut - 1);
        } else {   /* a GNU long name first */
            uint8_t blk[512];
            size_t at;
            tar_put_header(t, "././@LongLink", 'L', n + 1, 0644, 0);
            for (at = 0; at < n + 1; at += 512) {
                memset(blk, 0, sizeof blk);
                memcpy(blk, name + at, n + 1 - at < 512 ? n - at : 512);
                if (gzw_write(t->w, blk, 512) != 0)
                    t->err = 1;
            }
            memcpy(h, name, 100);
        }
    }
    snprintf((char *)h + 100, 8, "%07o", mode & 07777);
    snprintf((char *)h + 108, 8, "%07o", 0);
    snprintf((char *)h + 116, 8, "%07o", 0);
    snprintf((char *)h + 124, 12, "%011llo", (unsigned long long)size);
    snprintf((char *)h + 136, 12, "%011llo", (unsigned long long)(mtime > 0 ? mtime : 0));
    h[156] = (uint8_t)type;
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8);
    for (i = 0; i < 512; i++)
        sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    h[155] = ' ';
    if (gzw_write(t->w, h, 512) != 0)
        t->err = 1;
}
static void tar_walk(tarw_t *t, const char *rel)
{
    char abs_[PATH_MAX];
    DIR *d;
    struct dirent *e;
    snprintf(abs_, sizeof abs_, "%s/%s", t->root, rel);
    if (!(d = opendir(abs_)))
        return;
    while (!t->err && (e = readdir(d))) {
        char r2[PATH_MAX], a2[PATH_MAX + 8];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(r2, sizeof r2, "%s/%s", rel, e->d_name);
        snprintf(a2, sizeof a2, "%s/%s", t->root, r2);
        if (lstat(a2, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            char dn[PATH_MAX + 2];
            snprintf(dn, sizeof dn, "%s/", r2);
            tar_put_header(t, dn, '5', 0, st.st_mode, st.st_mtime);
            tar_walk(t, r2);
        } else if (S_ISREG(st.st_mode)) {
            uint8_t buf[65536];
            uint64_t left = (uint64_t)st.st_size;
            const int fd = open(a2, O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                continue;
            tar_put_header(t, r2, '0', left, st.st_mode, st.st_mtime);
            while (left && !t->err) {   /* (exactly the size the header gave: a file cut meanwhile padded) */
                size_t k = left > sizeof buf ? sizeof buf : (size_t)left;
                const ssize_t r = read(fd, buf, k);
                if (r <= 0)
                    memset(buf, 0, k);
                else
                    k = (size_t)r;
                if (gzw_write(t->w, buf, k) != 0)
                    t->err = 1;
                left -= k;
            }
            close(fd);
            if (st.st_size % 512) {
                memset(buf, 0, 512);
                if (gzw_write(t->w, buf, 512 - (size_t)(st.st_size % 512)) != 0)
                    t->err = 1;
            }
        }
    }
    closedir(d);
}
int upd_backup_dir(const char *root, const char *sub, const char *out, char *why, size_t cap)
{
    char top[PATH_MAX], part[PATH_MAX + 8];
    struct stat st;
    tarw_t t;
    FILE *f;
    uint8_t z[1024];
    snprintf(top, sizeof top, "%s/%s", root, sub);
    if (lstat(top, &st) != 0 || !S_ISDIR(st.st_mode))
        return 1;
    snprintf(part, sizeof part, "%s.part", out);
    if (!(f = fopen(part, "wb"))) {
        say(why, cap, "cannot write the data backup beside the RemoteFM1 folder");
        return -1;
    }
    memset(&t, 0, sizeof t);
    t.root = root;
    if (!(t.w = gzw_open(f))) {
        fclose(f);
        unlink(part);
        say(why, cap, "no memory");
        return -1;
    }
    {
        char dn[PATH_MAX + 2];
        snprintf(dn, sizeof dn, "%s/", sub);
        tar_put_header(&t, dn, '5', 0, st.st_mode, st.st_mtime);
    }
    tar_walk(&t, sub);
    memset(z, 0, sizeof z);
    if (gzw_write(t.w, z, sizeof z) != 0)
        t.err = 1;
    if (gzw_close(t.w) != 0)
        t.err = 1;
    if (fflush(f) != 0 || fsync(fileno(f)) != 0)
        t.err = 1;
    if (fclose(f) != 0)
        t.err = 1;
    if (t.err || rename(part, out) != 0) {
        unlink(part);
        say(why, cap, "the data backup could not be written (the disk full?)");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------- the state --- */
enum { S_IDLE, S_RECEIVING, S_CHECKING, S_INSTALLING, S_UNDOING, S_DONE, S_FAILED };
static const char *const STATE[] = { "idle", "receiving", "checking", "installing", "undoing", "done", "failed" };

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int ready, state, side_ok;   /* side_ok: the copies go in `beside` (writable, the module's own file system) */
    uint32_t seq;
    char msg[320], backup[128];
    char mod[PATH_MAX], parent[PATH_MAX], data[PATH_MAX], beside[PATH_MAX];   /* beside: where the upload and backups go */
    char part[PATH_MAX];
    char version[64], commit[64], core[64], built[64];
    char staged_version[64], staged_commit[64];
} G;

static void set_state(int state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_state(int state, const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&g_mu);
    G.state = state;
    G.seq++;
    va_start(ap, fmt);
    vsnprintf(G.msg, sizeof G.msg, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "update: %s: %s\n", STATE[state], G.msg);
}

/* The copies (the staged build, the previous one, one undone) never sit beside the module in its own folder: Schwung
 * lists every folder of modules/tools with a module.json as a tool, so a "remotefm1.prev" there is a second RemoteFM1
 * in its menu. They go in `beside` (where the upload and backups go: /data/UserData on the Move) under hidden names,
 * when that can be written and is the module's file system (the swap is renames); else inside the module's folder.
 * RFM14_TEST_UPDATE_INSIDE forces the inside layout in tests. */
static int inside(void)
{
#ifdef RFM14_TEST_BUILD
    const char *e = getenv("RFM14_TEST_UPDATE_INSIDE");
    if (e && *e == '1')
        return 1;
#endif
    return !G.side_ok || access(G.parent, W_OK) != 0;
}
static void paths(char *n, char *p, char *u, size_t cap)   /* the staged copy, the previous build, the one going */
{
    if (inside()) {
        snprintf(n, cap, "%s/.update-new", G.mod);
        snprintf(p, cap, "%s/.update-prev", G.mod);
        snprintf(u, cap, "%s/.update-undone", G.mod);
    } else {
        snprintf(n, cap, "%s/.remotefm1-update-new", G.beside);
        snprintf(p, cap, "%s/.remotefm1-update-prev", G.beside);
        snprintf(u, cap, "%s/.remotefm1-update-undone", G.beside);
    }
}
static int undo_dir(char *out, size_t cap)   /* the build an Undo brings back: 0 (out), -1 none */
{
    snprintf(out, cap, "%s/.update-prev", G.mod);
    if (is_dir(out))
        return 0;
    snprintf(out, cap, "%s/.remotefm1-update-prev", G.beside);
    return G.side_ok && is_dir(out) ? 0 : -1;
}

/* a module folder's build: its update-manifest.json (an install's), else its module.json's version */
static void build_of(const char *dir, char *ver, char *commit, char *built, size_t cap)
{
    char p[PATH_MAX + 32], *j = NULL;
    FILE *f;
    long n;
    ver[0] = commit[0] = built[0] = 0;
    snprintf(p, sizeof p, "%s/update-manifest.json", dir);
    if (!(f = fopen(p, "rb"))) {
        snprintf(p, sizeof p, "%s/module.json", dir);
        f = fopen(p, "rb");
    }
    if (!f)
        return;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && n < (long)UPD_MAN_MAX && fseek(f, 0, SEEK_SET) == 0 &&
        (j = malloc((size_t)n + 1)) && fread(j, 1, (size_t)n, f) == (size_t)n) {
        j[n] = 0;
        mj_string(j, "version", ver, cap);
        mj_string(j, "host_commit", commit, cap);
        mj_string(j, "built", built, cap);
    }
    free(j);
    fclose(f);
}

void upd_init(const upd_cfg_t *c)
{
    char *slash;
    char n[PATH_MAX], p[PATH_MAX], u[PATH_MAX];
    memset(&G, 0, sizeof G);
    if (!c->module_dir || !c->module_dir[0] || !realpath(c->module_dir, G.mod)) {
        fprintf(stderr, "update: no module folder: the page's Update is off\n");
        return;
    }
    snprintf(G.parent, sizeof G.parent, "%s", G.mod);
    if ((slash = strrchr(G.parent, '/')))
        *(slash == G.parent ? slash + 1 : slash) = 0;
    if (!c->data_root || !realpath(c->data_root, G.data)) {
        fprintf(stderr, "update: no RemoteFM1 folder: the page's Update is off\n");
        return;
    }
    snprintf(G.beside, sizeof G.beside, "%s", G.data);
    if ((slash = strrchr(G.beside, '/')))
        *(slash == G.beside ? slash + 1 : slash) = 0;
    if (access(G.beside, W_OK) != 0)   /* (beside the folder when that can be written, else in it) */
        snprintf(G.beside, sizeof G.beside, "%s", G.data);
    {
        struct stat a, b;
        G.side_ok = access(G.beside, W_OK) == 0 && stat(G.beside, &a) == 0 && stat(G.mod, &b) == 0 &&
                    a.st_dev == b.st_dev;
    }
    snprintf(G.part, sizeof G.part, "%s/.remotefm1-update.part", G.beside);
    snprintf(G.version, sizeof G.version, "%s", c->version ? c->version : "");
    snprintf(G.commit, sizeof G.commit, "%s", c->commit ? c->commit : "");
    snprintf(G.core, sizeof G.core, "%s", c->core ? c->core : "");
    snprintf(G.built, sizeof G.built, "%s", c->built ? c->built : "");
    /* (what a stop in the middle left: an upload, a staged copy, a build undone) */
    unlink(G.part);
    paths(n, p, u, sizeof n);
    upd_rmtree(n), upd_rmtree(u);
    snprintf(n, sizeof n, "%s/.remotefm1-update-new", G.beside), upd_rmtree(n);
    snprintf(n, sizeof n, "%s/.remotefm1-update-undone", G.beside), upd_rmtree(n);
    snprintf(n, sizeof n, "%s/.update-new", G.mod), upd_rmtree(n);
    snprintf(n, sizeof n, "%s/.update-undone", G.mod), upd_rmtree(n);
    G.ready = 1;
    fprintf(stderr, "update: the module %s (%s), uploads and backups in %s\n", G.mod,
            inside() ? "swapped inside it" : "swapped beside it", G.beside);
}

static size_t jstr(char *o, size_t cap, const char *s)   /* a JSON string, quoted */
{
    char e[700];
    mj_escape(s ? s : "", e, sizeof e);
    return (size_t)snprintf(o, cap, "\"%s\"", e);
}
#define CAT(...) do { if (k < cap) k += (size_t)snprintf(out + k, cap - k, __VA_ARGS__); } while (0)
#define CATS(s) do { if (k < cap) k += jstr(out + k, cap - k, s); } while (0)
int upd_status_json(char *out, size_t cap)
{
    char v[64], cm[64], bt[64], ud[PATH_MAX];
    const char *why = NULL;
    size_t k = 0;
    int undo;
    pthread_mutex_lock(&g_mu);
    if (!G.ready)
        why = "no module folder";
    else if (access(G.mod, W_OK) != 0)
        why = "the module's folder cannot be written";
    CAT("{\"state\":\"%s\",\"seq\":%u,\"msg\":", STATE[G.state], G.seq);
    CATS(G.msg);
    CAT(",\"running\":{\"version\":");
    CATS(G.version);
    CAT(",\"commit\":");
    CATS(G.commit);
    CAT(",\"core\":");
    CATS(G.core);
    CAT(",\"built\":");
    CATS(G.built);
    CAT("},\"installed\":");
    if (G.ready) {
        build_of(G.mod, v, cm, bt, sizeof v);
        CAT("{\"version\":");
        CATS(v);
        CAT(",\"commit\":");
        CATS(cm);
        CAT(",\"built\":");
        CATS(bt);
        CAT("}");
    } else
        CAT("null");
    CAT(",\"staged\":");
    if (G.staged_version[0]) {
        CAT("{\"version\":");
        CATS(G.staged_version);
        CAT(",\"commit\":");
        CATS(G.staged_commit);
        CAT("}");
    } else
        CAT("null");
    undo = G.ready && G.state != S_INSTALLING && G.state != S_UNDOING && undo_dir(ud, sizeof ud) == 0;
    CAT(",\"undo\":%s,\"undo_to\":", undo ? "true" : "false");
    if (undo) {
        build_of(ud, v, cm, bt, sizeof v);
        CAT("{\"version\":");
        CATS(v);
        CAT(",\"commit\":");
        CATS(cm);
        CAT("}");
    } else
        CAT("null");
    CAT(",\"backup\":");
    CATS(G.backup);
    CAT(",\"layout\":\"%s\",\"can\":%s,\"why\":", G.ready && inside() ? "inside" : "beside", why ? "false" : "true");
    CATS(why ? why : "");
    CAT(",\"max\":%llu}", (unsigned long long)UPD_MAX);
    pthread_mutex_unlock(&g_mu);
    return k < cap ? (int)k : -1;
}

/* ------------------------------------------------------------------------------------------- the install --- */
static int copy_file(const char *from, const char *to, mode_t mode)
{
    uint8_t buf[65536];
    const int in = open(from, O_RDONLY | O_CLOEXEC);
    int out, rc = 0;
    if (in < 0)
        return -1;
    out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode & 0777);
    if (out < 0) {
        close(in);
        return -1;
    }
    for (;;) {
        const ssize_t r = read(in, buf, sizeof buf);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            rc = -1;
        if (r <= 0)
            break;
        if (write_all(out, buf, (size_t)r) != 0) {
            rc = -1;
            break;
        }
    }
    fchmod(out, mode & 0777);   /* (a setuid file's copy is a plain one: the launcher installs bin/heal again) */
    close(in);
    if (close(out) != 0)
        rc = -1;
    return rc;
}
/* from's tree as to (made here): files, folders and links as they are; at the top, this feature's own left out */
static int copy_tree(const char *from, const char *to, int top, char *why, size_t cap)
{
    DIR *d;
    struct dirent *e;
    struct stat st;
    int rc = 0;
    if (lstat(from, &st) != 0 || mkdir(to, (st.st_mode & 0777) | 0700) != 0) {
        say(why, cap, "cannot make %s", to);
        return -1;
    }
    if (!(d = opendir(from))) {
        say(why, cap, "cannot read %s", from);
        return -1;
    }
    while (!rc && (e = readdir(d))) {
        char a[PATH_MAX], b[PATH_MAX];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (top && !strncmp(e->d_name, ".update-", 8)))
            continue;
        if ((size_t)snprintf(a, sizeof a, "%s/%s", from, e->d_name) >= sizeof a ||
            (size_t)snprintf(b, sizeof b, "%s/%s", to, e->d_name) >= sizeof b || lstat(a, &st) != 0) {
            say(why, cap, "cannot copy %s", e->d_name);
            rc = -1;
        } else if (S_ISDIR(st.st_mode))
            rc = copy_tree(a, b, 0, why, cap);
        else if (S_ISREG(st.st_mode)) {
            if (copy_file(a, b, st.st_mode) != 0) {
                say(why, cap, "cannot copy %s (the disk full?)", a);
                rc = -1;
            }
        } else if (S_ISLNK(st.st_mode)) {
            char l[PATH_MAX];
            const ssize_t n = readlink(a, l, sizeof l - 1);
            if (n < 0 || (l[n] = 0, symlink(l, b) != 0)) {
                say(why, cap, "cannot copy the link %s", a);
                rc = -1;
            }
        }   /* (anything else: not copied) */
    }
    closedir(d);
    return rc;
}
/* every top-level entry of from (but this feature's own) renamed into to: 0; -1 (those moved, moved back) */
static int move_all(const char *from, const char *to)
{
    DIR *d = opendir(from);
    struct dirent *e;
    char done[256][NAME_MAX + 1];
    int n = 0, rc = 0;
    if (!d)
        return -1;
    while ((e = readdir(d))) {
        char a[PATH_MAX], b[PATH_MAX];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strncmp(e->d_name, ".update-", 8))
            continue;
        snprintf(a, sizeof a, "%s/%s", from, e->d_name);
        snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        if (n == 256 || rename(a, b) != 0) {
            rc = -1;
            break;
        }
        snprintf(done[n++], sizeof done[0], "%s", e->d_name);
    }
    closedir(d);
    if (rc)
        while (n--) {
            char a[PATH_MAX], b[PATH_MAX];
            snprintf(a, sizeof a, "%s/%s", from, done[n]);
            snprintf(b, sizeof b, "%s/%s", to, done[n]);
            rename(b, a);
        }
    return rc;
}
/* the staged copy n in place of the module's folder, the folder kept as p (an older p moved to u first) */
static int swap_in(const char *n, const char *p, const char *u, int in)
{
    const int had = is_dir(p);
    if (had && rename(p, u) != 0)
        return -1;
    if (!in) {
        if (rename(G.mod, p) != 0) {
            if (had)
                rename(u, p);
            return -1;
        }
        if (rename(n, G.mod) != 0) {
            rename(p, G.mod);
            if (had)
                rename(u, p);
            return -1;
        }
    } else {
        if (mkdir(p, 0755) != 0) {
            if (had)
                rename(u, p);
            return -1;
        }
        if (move_all(G.mod, p) != 0) {
            rmdir(p);
            if (had)
                rename(u, p);
            return -1;
        }
        if (move_all(n, G.mod) != 0) {
            move_all(p, G.mod);
            rmdir(p);
            if (had)
                rename(u, p);
            return -1;
        }
        rmdir(n);
    }
    return 0;
}
/* the module's folder's build out (to u), p back in its place */
static int swap_back(const char *p, const char *u, int in)
{
    if (!in) {
        if (rename(G.mod, u) != 0)
            return -1;
        if (rename(p, G.mod) != 0) {
            rename(u, G.mod);
            return -1;
        }
        return 0;
    }
    if (mkdir(u, 0755) != 0 || move_all(G.mod, u) != 0) {
        rmdir(u);
        return -1;
    }
    if (move_all(p, G.mod) != 0) {
        move_all(u, G.mod);
        return -1;
    }
    rmdir(p);
    return 0;
}
/* the program in the staged copy: a plain executable file, an ELF for the machine this host runs on */
static const char *host_ok(const char *dir)
{
    char p[PATH_MAX + 32];
    uint8_t a[20], b[20];
    struct stat st;
    int fd;
    snprintf(p, sizeof p, "%s/remotefm1-host", dir);
    if (lstat(p, &st) != 0 || !S_ISREG(st.st_mode))
        return "the update leaves no remotefm1-host";
    if (access(p, X_OK) != 0 || !(st.st_mode & 0100))
        return "the update's remotefm1-host is not executable";
    if ((fd = open(p, O_RDONLY | O_CLOEXEC)) < 0)
        return "cannot read the update's remotefm1-host";
    if (read_all(fd, a, sizeof a) != 0 || memcmp(a, "\177ELF", 4)) {
        close(fd);
        return "the update's remotefm1-host is not a program for the Move (not an ELF)";
    }
    close(fd);
    if ((fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC)) >= 0) {
        if (read_all(fd, b, sizeof b) == 0 && (a[4] != b[4] || a[5] != b[5] || a[18] != b[18] || a[19] != b[19])) {
            close(fd);
            return "the update's remotefm1-host is for another machine";
        }
        close(fd);
    }
    return NULL;
}
/* one of this feature's own backups: exactly remotefm1-data-backup-YYYYMMDD-HHMMSS.tar.gz. Hand-made backups with
 * other names (remotefm1-data-backup-20261125-before-x.tar.gz) are never counted or deleted: by name they sorted
 * before the new one, which was then pruned as the oldest (the Move, 2026-11-25). */
static int own_backup(const char *s)
{
    int i;
    if (strlen(s) != 44 || strncmp(s, "remotefm1-data-backup-", 22) || strcmp(s + 37, ".tar.gz") || s[30] != '-')
        return 0;
    for (i = 22; i < 37; i++)
        if (i != 30 && (s[i] < '0' || s[i] > '9'))
            return 0;
    return 1;
}
static void prune_backups(void)
{
    DIR *d = opendir(G.beside);
    struct dirent *e;
    char names[64][128];
    int n = 0, i, j;
    if (!d)
        return;
    while ((e = readdir(d)) && n < 64)
        if (own_backup(e->d_name))
            snprintf(names[n++], sizeof names[0], "%s", e->d_name);
    closedir(d);
    for (i = 1; i < n; i++)   /* (by name: the date and time in it) */
        for (j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char t[128];
            memcpy(t, names[j], sizeof t);
            memcpy(names[j], names[j - 1], sizeof t);
            memcpy(names[j - 1], t, sizeof t);
        }
    for (i = 0; i + UPD_BACKUPS < n; i++) {
        char p[PATH_MAX + 130];
        snprintf(p, sizeof p, "%s/%s", G.beside, names[i]);
        unlink(p);
    }
}
static void write_manifest(const char *dir, const upd_manifest_t *m)
{
    char p[PATH_MAX + 32];
    const int fd = (snprintf(p, sizeof p, "%s/update-manifest.json", dir), open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
    if (fd >= 0) {
        write_all(fd, m->json, m->mlen);
        close(fd);
    }
}

static void *install_main(void *arg)
{
    uint8_t keys[2][32];
    char why[320], n[PATH_MAX], p[PATH_MAX], u[PATH_MAX], bk[PATH_MAX + 128], name[128];
    upd_manifest_t m;
    int strip = 0, files = 0, nk = 0, in;
    uint64_t bytes = 0, need;
    (void)arg;
#ifdef __linux__
    pthread_setname_np(pthread_self(), "rfm1-update");
#endif
    memcpy(keys[nk++], UPD_SIGNING_KEY, 32);
#ifdef RFM14_TEST_BUILD
    memcpy(keys[nk++], UPD_TEST_KEY, 32);
#endif
    set_state(S_CHECKING, "Checking the signature");
    if (upd_check_file(G.part, (const uint8_t(*)[32])keys, nk, &m, why, sizeof why) != 0) {
        unlink(G.part);
        set_state(S_FAILED, "%s", why);
        return NULL;
    }
    pthread_mutex_lock(&g_mu);
    snprintf(G.staged_version, sizeof G.staged_version, "%s", m.version);
    snprintf(G.staged_commit, sizeof G.staged_commit, "%s", m.commit);
    pthread_mutex_unlock(&g_mu);
    if (upd_payload(G.part, &m, NULL, &strip, &files, &bytes, why, sizeof why) != 0) {
        unlink(G.part);
        upd_manifest_free(&m);
        set_state(S_FAILED, "%s (nothing was changed)", why);
        return NULL;
    }
    need = du(G.mod) + bytes + UPD_RESERVE;
    if (disk_free(G.beside) < need) {
        unlink(G.part);
        upd_manifest_free(&m);
        set_state(S_FAILED, "not enough room on the Move (nothing was changed)");
        return NULL;
    }
    /* the staged copy */
    in = inside();
    paths(n, p, u, sizeof n);
    set_state(S_INSTALLING, "Unpacking %s (%d file%s)", m.version, files, files == 1 ? "" : "s");
    upd_rmtree(n);
    if (copy_tree(G.mod, n, 1, why, sizeof why) != 0 ||
        upd_payload(G.part, &m, n, &strip, NULL, NULL, why, sizeof why) != 0) {
        upd_rmtree(n);
        unlink(G.part);
        upd_manifest_free(&m);
        set_state(S_FAILED, "%s (nothing was changed)", why);
        return NULL;
    }
    {
        const char *bad = host_ok(n);
        if (bad) {
            upd_rmtree(n);
            unlink(G.part);
            upd_manifest_free(&m);
            set_state(S_FAILED, "%s (nothing was changed)", bad);
            return NULL;
        }
    }
    /* the data backup (once the staged copy is good: a refused update leaves none) */
    set_state(S_INSTALLING, "Backing up the RemoteFM1 folder's move/");
    {
        const time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(name, sizeof name, "remotefm1-data-backup-%04d%02d%02d-%02d%02d%02d.tar.gz", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        snprintf(bk, sizeof bk, "%s/%s", G.beside, name);
        switch (upd_backup_dir(G.data, "move", bk, why, sizeof why)) {
        case 0:
            pthread_mutex_lock(&g_mu);
            snprintf(G.backup, sizeof G.backup, "%s", name);
            pthread_mutex_unlock(&g_mu);
            prune_backups();
            break;
        case 1:
            break;   /* (no move/ yet) */
        default:
            upd_rmtree(n);
            unlink(G.part);
            upd_manifest_free(&m);
            set_state(S_FAILED, "%s (nothing was changed)", why);
            return NULL;
        }
    }
    write_manifest(n, &m);
    sync();
    set_state(S_INSTALLING, "Putting %s in place", m.version);
    if (swap_in(n, p, u, in) != 0) {
        const int e = errno;
        upd_rmtree(n);
        unlink(G.part);
        upd_manifest_free(&m);
        set_state(S_FAILED, "the module's folder could not be swapped (%s; nothing was changed)", strerror(e));
        return NULL;
    }
    sync();
    upd_rmtree(u);
    unlink(G.part);
    set_state(S_DONE, "Installed %s. Relaunch RemoteFM1 on the Move to start it.", m.version);
    upd_manifest_free(&m);
    return NULL;
}
static void *undo_main(void *arg)
{
    char p[PATH_MAX], u[PATH_MAX], v[64], cm[64], bt[64];
    int in;
    (void)arg;
#ifdef __linux__
    pthread_setname_np(pthread_self(), "rfm1-update");
#endif
    in = is_dir((snprintf(p, sizeof p, "%s/.update-prev", G.mod), p));
    if (in) {
        snprintf(u, sizeof u, "%s/.update-undone", G.mod);
    } else {
        snprintf(p, sizeof p, "%s/.remotefm1-update-prev", G.beside);
        snprintf(u, sizeof u, "%s/.remotefm1-update-undone", G.beside);
    }
    build_of(p, v, cm, bt, sizeof v);
    upd_rmtree(u);
    if (!is_dir(p) || swap_back(p, u, in) != 0) {
        set_state(S_FAILED, "the previous build could not be put back (nothing was changed)");
        return NULL;
    }
    sync();
    upd_rmtree(u);
    pthread_mutex_lock(&g_mu);
    G.staged_version[0] = G.staged_commit[0] = 0;
    pthread_mutex_unlock(&g_mu);
    set_state(S_DONE, "Put back %s. Relaunch RemoteFM1 on the Move to start it.", v[0] ? v : "the previous build");
    return NULL;
}
static int spawn(void *(*fn)(void *))
{
    pthread_t t;
    pthread_attr_t a;
    int rc;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    rc = pthread_create(&t, &a, fn, NULL);
    pthread_attr_destroy(&a);
    return rc;
}
static int busy(void)
{
    return G.state == S_RECEIVING || G.state == S_CHECKING || G.state == S_INSTALLING || G.state == S_UNDOING;
}

int upd_recv_begin(uint64_t size, char *tmp, size_t cap, char *why, size_t wcap)
{
    int rc = 0;
    pthread_mutex_lock(&g_mu);
    if (!G.ready) {
        say(why, wcap, "updates are off on this Move (no module folder)");
        rc = 500;
    } else if (busy()) {
        say(why, wcap, "an update is under way");
        rc = 409;
    } else if (access(G.mod, W_OK) != 0) {
        say(why, wcap, "the module's folder cannot be written");
        rc = 500;
    } else if (size < UPD_HEAD + 64 + 2) {
        say(why, wcap, "not an update file (.rfm1u)");
        rc = 400;
    } else if (size > UPD_MAX) {
        say(why, wcap, "at most 256 MB");
        rc = 413;
    } else if (size * 5 + du(G.mod) + UPD_RESERVE > disk_free(G.beside)) {
        say(why, wcap, "not enough room on the Move");
        rc = 507;
    }
    if (!rc) {
        snprintf(tmp, cap, "%s", G.part);
        G.state = S_RECEIVING;
        G.seq++;
        G.staged_version[0] = G.staged_commit[0] = 0;
        G.backup[0] = 0;
        snprintf(G.msg, sizeof G.msg, "Receiving");
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}
void upd_recv_abort(const char *why)
{
    unlink(G.part);
    set_state(S_FAILED, "%s", why ? why : "the upload stopped");
}
int upd_recv_done(void)
{
    if (spawn(install_main) != 0) {
        unlink(G.part);
        set_state(S_FAILED, "cannot start the install");
        return -1;
    }
    return 0;
}
int upd_undo_start(char *why, size_t wcap)
{
    char ud[PATH_MAX];
    int rc = 0;
    pthread_mutex_lock(&g_mu);
    if (!G.ready) {
        say(why, wcap, "updates are off on this Move");
        rc = 500;
    } else if (busy()) {
        say(why, wcap, "an update is under way");
        rc = 409;
    } else if (undo_dir(ud, sizeof ud) != 0) {
        say(why, wcap, "there is no previous build to put back");
        rc = 409;
    } else {
        G.state = S_UNDOING;
        G.seq++;
        snprintf(G.msg, sizeof G.msg, "Putting the previous build back");
    }
    pthread_mutex_unlock(&g_mu);
    if (!rc && spawn(undo_main) != 0) {
        set_state(S_FAILED, "cannot start the undo");
        rc = 500;
        say(why, wcap, "cannot start the undo");
    }
    return rc;
}
