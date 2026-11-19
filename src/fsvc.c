/* SPDX-License-Identifier: MPL-2.0 */
/* fsvc: a Fantasy CFW's file service (rfm1_host_1): see fsvc.h. */
#define _GNU_SOURCE
#include "fsvc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "minijson.h"

static char g_root[PATH_MAX];           /* the folder (its real path), "" none */
static void (*g_dispatch)(void *io);
static volatile uint32_t g_changed = 1;

enum { PH_FREE, PH_NEW, PH_IO, PH_READY };

typedef struct fsv_io {
    struct fsvc *svc;
    int op, area, slot;
    char path[PATH_MAX];
    int64_t off;
    uint32_t len;
    uint8_t *data;                      /* a write's bytes; else what the request answers (len bytes at most) */
    uint32_t n;                         /* bytes in data to copy into the module */
    int32_t result;
} fsv_io_t;

typedef struct {
    int32_t id;                         /* the cart's id (0: none visible) */
    int phase;                          /* PH_*: NEW / READY change only on the engine's thread, IO -> READY on the
                                         * I/O thread (under mu) */
    int done, canceled;                 /* the result is visible (fs_poll gives it once); cancelled */
    int32_t result;
    uint32_t buf, due;
    fsv_io_t *io;
} entry_t;

struct fsvc {
    char id[64];
    uint8_t access[3];
    pthread_mutex_t mu;
    int refs;                           /* the instance's + one per request on the I/O thread (under mu) */
    int dead;                           /* released: what the I/O thread still has is dropped */
    uint32_t now_ms;
    int32_t next_id;
    entry_t e[FSV_TABLE];
    fsvc_stats_t st;
};

/* ------------------------------------------------------------------ the folder --- */
void fsvc_set_root(const char *root)
{
    g_root[0] = 0;
    if (root && root[0] && !realpath(root, g_root))
        snprintf(g_root, sizeof g_root, "%s", root);
}
const char *fsvc_root(void) { return g_root; }
void fsvc_set_dispatch(void (*fn)(void *io)) { g_dispatch = fn; }
void fsvc_bump_changed(void) { __atomic_add_fetch(&g_changed, 1, __ATOMIC_RELAXED); }

static int32_t from_errno(int e)
{
    switch (e) {
    case ENOENT:
    case ENOTDIR: return FSV_ENOENT;
    case EACCES:
    case EPERM:
    case EROFS:
    case ELOOP: return FSV_EACCES;
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
        return FSV_ENOSPC;
    default: return FSV_EIO;
    }
}

static const char *const AREA[3] = { "audio", "code", "data" };

static void area_base(const char *root, const char *id, int area, char *out, size_t cap)
{
    if (area == FSV_AUDIO)
        snprintf(out, cap, "%s/audio", root);
    else
        snprintf(out, cap, "%s/%s/%s", root, AREA[area], id);
}

/* strict UTF-8: no overlong forms, no surrogates, nothing past U+10FFFF (as the iOS and Android hosts) */
static int good_utf8(const uint8_t *s, uint32_t n)
{
    uint32_t i = 0;
    while (i < n) {
        const uint8_t c = s[i];
        uint32_t k, u, j;
        if (c < 0x80) {
            i++;
            continue;
        } else if (c >= 0xC2 && c < 0xE0)
            k = 1, u = c & 0x1Fu;
        else if (c >= 0xE0 && c < 0xF0)
            k = 2, u = c & 0x0Fu;
        else if (c >= 0xF0 && c < 0xF5)
            k = 3, u = c & 0x07u;
        else
            return 0;
        for (j = 1; j <= k; j++) {
            if (i + j >= n || (s[i + j] & 0xC0) != 0x80)
                return 0;
            u = u << 6 | (s[i + j] & 0x3Fu);
        }
        if ((k == 2 && u < 0x800) || (k == 3 && (u < 0x10000 || u > 0x10FFFF)) || (u >= 0xD800 && u < 0xE000))
            return 0;
        i += k + 1;
    }
    return 1;
}

int32_t fsvc_map(const char *root, const char *id, const uint8_t *v, uint32_t n, char *out, size_t cap, int *area,
                 int *top)
{
    char seg[256];
    uint32_t i, k = 0;
    int a = -1, nseg = 0;
    size_t at = 0;
    if (n < 2 || n >= FSV_PATH || v[0] != '/')
        return FSV_EACCES;
    for (i = 0; i < n; i++)
        if (!v[i] || v[i] == '\\')
            return FSV_EACCES;
    if (!good_utf8(v, n))
        return FSV_EACCES;
    /* each segment after the leading '/': none empty (a trailing '/' too), none "." or "..", each <= 255 bytes */
    seg[0] = 0;
    for (i = 1; i <= n; i++) {
        if (i < n && v[i] != '/') {
            if (k >= sizeof seg - 1)
                return FSV_EACCES;
            seg[k++] = (char)v[i];
            seg[k] = 0;
            continue;
        }
        if (!k || !strcmp(seg, ".") || !strcmp(seg, ".."))
            return FSV_EACCES;
        if (nseg == 0) {
            int j;
            for (j = 0; j < 3; j++)
                if (!strcmp(seg, AREA[j]))
                    a = j;
            if (a < 0)
                return FSV_EACCES;
            area_base(root, id, a, out, cap);
            at = strlen(out);
        } else {
            if (at + 1 + k >= cap)
                return FSV_EACCES;
            at += (size_t)snprintf(out + at, cap - at, "/%s", seg);
        }
        nseg++;
        k = 0;
        seg[0] = 0;
    }
    *area = a;
    *top = nseg == 1;
    return 0;
}

int fsvc_inside(const char *root, const char *id, const char *path, int area)
{
    char base[PATH_MAX], rb[PATH_MAX], rp[PATH_MAX], tmp[PATH_MAX];
    size_t rl, bl;
    area_base(root, id, area, base, sizeof base);
    if (!realpath(base, rb))
        return 0;
    rl = strlen(root);   /* the area itself inside the folder (a symlinked code/<id> pointing out is refused too) */
    if (strncmp(rb, root, rl) || (rb[rl] && rb[rl] != '/'))
        return 0;
    snprintf(tmp, sizeof tmp, "%s", path);
    while (!realpath(tmp, rp)) {
        char *sl = strrchr(tmp, '/');
        if (errno != ENOENT || !sl || sl == tmp)
            return 0;
        *sl = 0;                        /* (not there yet: its folder) */
    }
    bl = strlen(rb);
    return !strncmp(rp, rb, bl) && (rp[bl] == 0 || rp[bl] == '/');
}

/* ------------------------------------------------------------------ an instance --- */
void fsvc_access(const char *json, uint8_t access[3])
{
    const char *m = json ? mj_find(json, "manifest") : NULL, *f;
    int a;
    access[0] = access[1] = access[2] = 0;
    if (!json)
        return;
    f = mj_find(m ? m : json, "folders");
    if (!f || *f != '{')
        return;
    for (a = 0; a < 3; a++) {
        char v[8];
        if (!mj_string(f, AREA[a], v, sizeof v))
            access[a] = (uint8_t)((strchr(v, 'r') ? FSV_R : 0) | (strchr(v, 'w') ? FSV_W : 0));
    }
}

fsvc_t *fsvc_new(const char *id, const uint8_t access[3])
{
    fsvc_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    snprintf(s->id, sizeof s->id, "%s", id);
    memcpy(s->access, access, 3);
    pthread_mutex_init(&s->mu, NULL);
    s->refs = 1;
    if (g_root[0]) {   /* its own folders, so the user sees where its files go (made on demand by the other hosts) */
        char p[PATH_MAX];
        int a;
        for (a = 0; a < 3; a++)
            if (access[a]) {
                if (a != FSV_AUDIO) {
                    snprintf(p, sizeof p, "%s/%s", g_root, AREA[a]);
                    mkdir(p, 0755);
                }
                area_base(g_root, id, a, p, sizeof p);
                mkdir(p, 0755);
            }
    }
    return s;
}

static void unref(fsvc_t *s)   /* (mu held; unlocks) */
{
    const int last = --s->refs == 0;
    pthread_mutex_unlock(&s->mu);
    if (last) {
        int i;
        for (i = 0; i < FSV_TABLE; i++)
            if (s->e[i].io) {
                free(s->e[i].io->data);
                free(s->e[i].io);
            }
        pthread_mutex_destroy(&s->mu);
        free(s);
    }
}

void fsvc_release(fsvc_t *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    s->dead = 1;
    unref(s);
}

void fsvc_stats(const fsvc_t *s, fsvc_stats_t *out)
{
    int i;
    memset(out, 0, sizeof *out);
    if (!s)
        return;
    *out = s->st;
    for (i = 0; i < FSV_TABLE; i++)
        out->inflight += s->e[i].id != 0;
}

/* ------------------------------------------------------------------ the imports --- */
int32_t fsvc_request(fsvc_t *s, uint8_t *mem, uint64_t size, int32_t op, uint32_t path, int32_t path_len, int64_t off,
                     uint32_t buf, int32_t len)
{
    char host[PATH_MAX];
    int i, slot = -1, inflight = 0, area = 0, top = 0;
    int32_t rc;
    entry_t *e;
    fsv_io_t *io;
    if (!s || !g_root[0])
        return FSV_ENOFOLDER;
    for (i = 0; i < FSV_TABLE; i++) {
        inflight += s->e[i].id != 0;
        if (slot < 0 && !s->e[i].id && __atomic_load_n(&s->e[i].phase, __ATOMIC_ACQUIRE) == PH_FREE)
            slot = i;
    }
    if (inflight >= FSV_INFLIGHT || slot < 0) {
        s->st.busy++;
        return FSV_EBUSY;
    }
    if (op < FSV_STAT || op > FSV_MKDIR || path_len <= 0 || len < 0 || len > FSV_MAXLEN ||
        (uint64_t)path + (uint32_t)path_len > size || (uint64_t)buf + (uint32_t)len > size ||
        (op == FSV_STAT && len < 16) || (op == FSV_WRITE && off < -1) || ((op == FSV_READ || op == FSV_LIST) && off < 0) ||
        (op == FSV_LIST && off > 0x7FFFFFFF)) {
        s->st.refused++;
        return FSV_EIO;
    }
    rc = fsvc_map(g_root, s->id, mem + path, (uint32_t)path_len, host, sizeof host, &area, &top);
    if (!rc) {   /* the manifest's "folders": read where it says r, write only in /data (below it) where rw */
        const int writes = op == FSV_WRITE || op == FSV_DELETE || op == FSV_MKDIR;
        if (writes ? area != FSV_DATA || !(s->access[area] & FSV_W) || top : !(s->access[area] & FSV_R))
            rc = FSV_EACCES;
    }
    if (rc) {
        s->st.refused++;
        return rc;
    }
    if (!(io = calloc(1, sizeof *io)))
        return FSV_EIO;
    io->svc = s;
    io->op = op;
    io->area = area;
    io->slot = slot;
    io->off = off;
    io->len = op == FSV_STAT ? 16u : op == FSV_DELETE || op == FSV_MKDIR ? 0u : (uint32_t)len;
    snprintf(io->path, sizeof io->path, "%s", host);
    if (io->len && !(io->data = malloc(io->len))) {
        free(io);
        return FSV_EIO;
    }
    if (op == FSV_WRITE && io->len)   /* (the bytes leave the module now: its buffer is the host's until answered) */
        memcpy(io->data, mem + buf, io->len);
    if (++s->next_id <= 0 || s->next_id > 0x7FFFFFF0)
        s->next_id = 1;
    e = &s->e[slot];
    e->id = s->next_id;
    e->done = e->canceled = 0;
    e->result = FSV_PENDING;
    e->buf = buf;
    e->due = s->now_ms + 1u;
    e->io = io;
    __atomic_store_n(&e->phase, PH_NEW, __ATOMIC_RELEASE);
    s->st.made++;
    return e->id;
}

int32_t fsvc_poll(fsvc_t *s, int32_t id)
{
    int i;
    if (!s || !g_root[0])
        return FSV_ENOFOLDER;
    for (i = 0; i < FSV_TABLE && id > 0; i++)
        if (s->e[i].id == id) {
            if (!s->e[i].done)
                return FSV_PENDING;
            s->e[i].id = 0;              /* (the result once: the id is free) */
            return s->e[i].result;
        }
    return FSV_EIO;
}

void fsvc_cancel(fsvc_t *s, int32_t id)
{
    int i;
    for (i = 0; s && i < FSV_TABLE && id > 0; i++)
        if (s->e[i].id == id && !s->e[i].done) {
            s->e[i].done = s->e[i].canceled = 1;
            s->e[i].result = FSV_ECANCELED;
        }
}

int32_t fsvc_changed(fsvc_t *s)
{
    (void)s;
    return (int32_t)__atomic_load_n(&g_changed, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ the work (the I/O thread, or inline) --- */
static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void own_data(const char *id)   /* the cart's own data folder, made at its first write (as rfm1_run) */
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/data", g_root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/data/%s", g_root, id);
    mkdir(p, 0755);
}

static int32_t do_io(fsv_io_t *io, const char *id)
{
    struct stat st;
    const char *path = io->path;
    if (io->area == FSV_DATA && (io->op == FSV_WRITE || io->op == FSV_MKDIR))
        own_data(id);
    if (!fsvc_inside(g_root, id, path, io->area))
        return FSV_EACCES;
    switch (io->op) {
    case FSV_STAT: {
        uint64_t sz;
        uint32_t mt, dir;
        if (stat(path, &st))
            return from_errno(errno);
        sz = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
        mt = (uint32_t)st.st_mtime, dir = S_ISDIR(st.st_mode) ? 1u : 0u;
        for (int i = 0; i < 8; i++)
            io->data[i] = (uint8_t)(sz >> (8 * i));
        for (int i = 0; i < 4; i++) {
            io->data[8 + i] = (uint8_t)(mt >> (8 * i));
            io->data[12 + i] = (uint8_t)(dir >> (8 * i));
        }
        io->n = 16;
        return 16;
    }
    case FSV_LIST: {
        DIR *d = opendir(path);
        struct dirent *de;
        char **names = NULL, **nn;
        size_t n = 0, cap = 0, i;
        int32_t written = 0;
        uint32_t at = 0;
        if (!d)
            return from_errno(errno);
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.' || strlen(de->d_name) > 255)
                continue;                /* (hidden files are not listed) */
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                if (!(nn = realloc(names, cap * sizeof *names)))
                    break;
                names = nn;
            }
            if (!(names[n] = strdup(de->d_name)))
                break;
            n++;
        }
        closedir(d);
        if (n)
            qsort(names, n, sizeof *names, name_cmp);
        for (i = (size_t)io->off; i < n; i++) {
            char full[PATH_MAX + 260];
            const size_t nl = strlen(names[i]);
            if (at + 2u + nl > io->len)
                break;                   /* (an entry is never split) */
            snprintf(full, sizeof full, "%s/%s", path, names[i]);
            io->data[at] = (uint8_t)(!stat(full, &st) && S_ISDIR(st.st_mode));
            io->data[at + 1] = (uint8_t)nl;
            memcpy(io->data + at + 2, names[i], nl);
            at += 2u + (uint32_t)nl;
            written++;
        }
        const int none_fit = !written && (size_t)io->off < n;
        for (i = 0; i < n; i++)
            free(names[i]);
        free(names);
        io->n = at;
        return none_fit ? FSV_EIO : written;   /* (a buffer too small for the next name: not the end) */
    }
    case FSV_READ: {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        uint32_t got = 0;
        if (fd < 0)
            return from_errno(errno);
        if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
            close(fd);
            return FSV_EIO;
        }
        while (got < io->len) {
            const ssize_t r = pread(fd, io->data + got, io->len - got, (off_t)io->off + got);
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0) {
                const int e = errno;
                close(fd);
                return from_errno(e);
            }
            if (r == 0)
                break;
            got += (uint32_t)r;
        }
        close(fd);
        io->n = got;
        return (int32_t)got;
    }
    case FSV_WRITE: {
        int fd;
        uint32_t put = 0;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return FSV_EIO;
        fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | (io->off < 0 ? O_TRUNC : 0), 0644);
        if (fd < 0)
            return from_errno(errno);
        while (put < io->len) {
            const ssize_t w = pwrite(fd, io->data + put, io->len - put, (off_t)(io->off < 0 ? 0 : io->off) + put);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                const int e = w < 0 ? errno : EIO;
                close(fd);
                return from_errno(e);
            }
            put += (uint32_t)w;
        }
        close(fd);
        io->n = 0;
        return (int32_t)put;
    }
    case FSV_DELETE:
        if (lstat(path, &st))
            return from_errno(errno);
        if (S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path))
            return errno == ENOTEMPTY || errno == EEXIST ? FSV_EIO : from_errno(errno);
        return 0;
    case FSV_MKDIR:
        if (mkdir(path, 0755) && !(errno == EEXIST && !stat(path, &st) && S_ISDIR(st.st_mode)))
            return from_errno(errno);
        return 0;
    }
    return FSV_EIO;
}

void fsvc_serve(void *p)
{
    fsv_io_t *io = p;
    fsvc_t *s = io->svc;
    int dead;
    pthread_mutex_lock(&s->mu);
    dead = s->dead;
    pthread_mutex_unlock(&s->mu);
    io->result = dead ? FSV_EIO : do_io(io, s->id);
    pthread_mutex_lock(&s->mu);
    __atomic_store_n(&s->e[io->slot].phase, PH_READY, __ATOMIC_RELEASE);
    unref(s);
}

/* ------------------------------------------------------------------ between two calls --- */
void fsvc_pump(fsvc_t *s, uint8_t *mem, uint64_t size, uint32_t ms)
{
    uint32_t copied = 0;
    int i;
    if (!s)
        return;
    s->now_ms = ms;
    for (i = 0; i < FSV_TABLE; i++) {
        entry_t *e = &s->e[i];
        int ph = __atomic_load_n(&e->phase, __ATOMIC_ACQUIRE);
        if (ph == PH_NEW && e->canceled) {   /* (never sent) */
            free(e->io->data);
            free(e->io);
            e->io = NULL;
            __atomic_store_n(&e->phase, PH_FREE, __ATOMIC_RELEASE);
            continue;
        }
        if (ph == PH_NEW) {
            if (!g_dispatch) {           /* inline: done one device ms after it was made (rfm1_run --folder) */
                if (e->due > ms)
                    continue;
                e->io->result = do_io(e->io, s->id);
                ph = PH_READY;
            } else {
                pthread_mutex_lock(&s->mu);   /* (the I/O thread's reference) */
                s->refs++;
                pthread_mutex_unlock(&s->mu);
                __atomic_store_n(&e->phase, PH_IO, __ATOMIC_RELEASE);
                g_dispatch(e->io);
                continue;
            }
        }
        if (ph != PH_READY)
            continue;
        if (copied >= FSV_PUMP_BYTES && e->io->n && !e->canceled)
            continue;                    /* (the next pump: the engine's time per ms stays bounded) */
        if (!e->canceled) {
            fsv_io_t *io = e->io;
            int32_t r = io->result;
            if (r >= 0 && io->n) {
                if ((uint64_t)e->buf + io->n <= size) {
                    memcpy(mem + e->buf, io->data, io->n);
                    copied += io->n;
                    s->st.bytes_in += io->n;
                } else
                    r = FSV_EIO;
            }
            if (r >= 0 && io->op == FSV_WRITE)
                s->st.bytes_out += (uint32_t)r;
            e->result = r;
            e->done = 1;
            s->st.served++;
        }
        free(e->io->data);
        free(e->io);
        e->io = NULL;
        __atomic_store_n(&e->phase, PH_FREE, __ATOMIC_RELEASE);
    }
}
