/* SPDX-License-Identifier: MPL-2.0 */
/* The root folder and the storage thread: see storage.h. */
#define _GNU_SOURCE
#include "storage.h"
#include "fx.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "catalog.h"
#include "fsvc.h"
#include "fwrt.h"
#include "minijson.h"
#include "rfm1cs/store.h"

static char g_root[ST_PATH];

/* ------------------------------------------------------------------ files --- */
int st_mkdirs(const char *path)
{
    char p[ST_PATH];
    size_t i;
    snprintf(p, sizeof p, "%s", path);
    for (i = 1; p[i]; i++)
        if (p[i] == '/') {
            p[i] = 0;
            if (mkdir(p, 0755) != 0 && errno != EEXIST)
                return -1;
            p[i] = '/';
        }
    return mkdir(p, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

int st_init(const char *root)
{
    char p[ST_PATH];
    snprintf(g_root, sizeof g_root, "%s", root);
    if (st_mkdirs(g_root) != 0)
        return -1;
    st_path(p, sizeof p, "firmwares");
    st_mkdirs(p);
    st_path(p, sizeof p, "states");
    st_mkdirs(p);
    st_path(p, sizeof p, "move/projects");
    st_mkdirs(p);
    st_path(p, sizeof p, "catalog");
    st_mkdirs(p);
    /* the Fantasy CFW folder (FM1VST docs/VIRTUAL-CARTS.md section 3): audio/ (every cart reads it), code/<id>/ and
     * data/<id>/ (made as a cart loads); its file API is served from here (fsvc.c, the file thread below) */
    st_path(p, sizeof p, "audio");
    st_mkdirs(p);
    st_path(p, sizeof p, "code");
    st_mkdirs(p);
    st_path(p, sizeof p, "data");
    st_mkdirs(p);
    fsvc_set_root(g_root);
    {   /* a download the last run left unfinished */
        DIR *d;
        struct dirent *e;
        st_path(p, sizeof p, "firmwares/.dl");
        if ((d = opendir(p))) {
            while ((e = readdir(d)))
                if (e->d_name[0] != '.') {
                    char f[ST_PATH + 260];
                    snprintf(f, sizeof f, "%s/%s", p, e->d_name);
                    unlink(f);
                }
            closedir(d);
        }
    }
    return 0;
}

const char *st_root(void) { return g_root; }

void st_path(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n = snprintf(out, cap, "%s/", g_root);
    va_start(ap, fmt);
    if (n > 0 && (size_t)n < cap)
        vsnprintf(out + n, cap - (size_t)n, fmt, ap);
    va_end(ap);
}

int st_write(const char *path, const void *b, size_t n)
{
    char tmp[ST_PATH + 8];
    int fd;
    ssize_t w = 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        char dir[ST_PATH], *slash;
        snprintf(dir, sizeof dir, "%s", path);
        if ((slash = strrchr(dir, '/'))) {
            *slash = 0;
            st_mkdirs(dir);
        }
        fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0)
            return -1;
    }
    while ((size_t)w < n) {
        const ssize_t k = write(fd, (const uint8_t *)b + w, n - (size_t)w);
        if (k <= 0) {
            close(fd);
            unlink(tmp);
            return -1;
        }
        w += k;
    }
    fsync(fd);
    close(fd);
    return rename(tmp, path) == 0 ? 0 : -1;
}

uint8_t *st_read(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long len;
    *n = 0;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || !(b = malloc((size_t)len + 1))) {
        fclose(f);
        return NULL;
    }
    if (fread(b, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    b[len] = 0;
    *n = (size_t)len;
    return b;
}

int st_copy(const char *from, const char *to)
{
    size_t n;
    uint8_t *b = st_read(from, &n);
    int rc;
    if (!b)
        return -1;
    rc = st_write(to, b, n);
    free(b);
    if (rc == 0)
        chmod(to, 0755);
    return rc;
}

/* ------------------------------------------------------------------ firmwares --- */
static int ends_with(const char *s, const char *suf)
{
    const size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

static int install_dir(const char *dir, int dev);
int st_fw_install_bundled(const char *dir) { return install_dir(dir, 0); }
int st_fw_install_dev(const char *dir) { return install_dir(dir, 1); }
static int install_dir(const char *dir, int dev)
{
    DIR *d = dir ? opendir(dir) : NULL;
    struct dirent *e;
    st_fw_t *have;
    int n = 0, nh;
    if (!d)
        return 0;
    if (!(have = calloc(ST_FW_MAX, sizeof *have))) {
        closedir(d);
        return 0;
    }
    nh = st_fw_scan(have, ST_FW_MAX);
    while ((e = readdir(d))) {
        char base[256], from[ST_PATH], to[ST_PATH], id[RFM_MOVE_FW_ID_MAX], ver[32];
        struct stat st;
        static const char *SUF[] = { ".rfm1m", ".json", ".licenses.txt", ".rfm1skin", ".rfm1skin.bg", ".rfm1skin.bgl" };
        size_t k, len;
        int i, newer = 0;
        char *json;
        const char *man;
        if (!ends_with(e->d_name, ".json") || strlen(e->d_name) >= sizeof base)
            continue;
        snprintf(base, sizeof base, "%.*s", (int)(strlen(e->d_name) - 5), e->d_name);
        /* a manifest, of a firmware the root has no newer (or the same) version of under another name */
        snprintf(from, sizeof from, "%s/%s", dir, e->d_name);
        if (!(json = (char *)st_read(from, &len)))
            continue;
        man = mj_find(json, "manifest");
        if (!man || mj_string(man, "id", id, sizeof id) || mj_string(man, "version", ver, sizeof ver)) {
            free(json);
            continue;
        }
        if (dev) {   /* (a dev build over the same <id>-<version>: when the installed manifest names another .rfm1) */
            char sha[80], isha[80], *ij;
            size_t il;
            snprintf(to, sizeof to, "%s/firmwares/%s.json", g_root, base);
            sha[0] = isha[0] = 0;
            mj_string(json, "sha256", sha, sizeof sha);
            if ((ij = (char *)st_read(to, &il))) {
                mj_string(ij, "sha256", isha, sizeof isha);
                free(ij);
                if (sha[0] && strcmp(sha, isha)) {
                    unlink(to);   /* (the manifest first: half a replacement is not listed) */
                    fprintf(stderr, "storage: %s: the development build (%.12s) in place of %.12s\n", base, sha, isha);
                }
            }
        }
        free(json);
        for (i = 0; i < nh && !newer; i++)
            newer = !strcmp(have[i].id, id) && st_vercmp(have[i].version, ver) > 0;
        if (newer)
            continue;   /* (an update was downloaded: the bundled one is older) */
        snprintf(to, sizeof to, "%s/firmwares/%s.json", g_root, base);
        if (stat(to, &st) == 0) {
            /* there already (the user's, or an earlier install): the same <id>-<version>, so only what an older
             * install did not have (its licence texts, a skin) is added, and the manifest that names them */
            int added = 0;
            for (k = 2; k < sizeof SUF / sizeof SUF[0]; k++) {
                snprintf(from, sizeof from, "%s/%s%s", dir, base, SUF[k]);
                snprintf(to, sizeof to, "%s/firmwares/%s%s", g_root, base, SUF[k]);
                if (stat(from, &st) == 0 && stat(to, &st) != 0 && st_copy(from, to) == 0)
                    added++;
            }
            if (added) {
                snprintf(from, sizeof from, "%s/%s.json", dir, base);
                snprintf(to, sizeof to, "%s/firmwares/%s.json", g_root, base);
                st_copy(from, to);
            }
            continue;
        }
        for (k = 0; k < sizeof SUF / sizeof SUF[0]; k++) {
            snprintf(from, sizeof from, "%s/%s%s", dir, base, SUF[k]);
            snprintf(to, sizeof to, "%s/firmwares/%s%s", g_root, base, SUF[k]);
            if (stat(from, &st) == 0 && k != 1)
                st_copy(from, to);
        }
        /* the manifest last: a firmware is listed only once its object is there */
        snprintf(from, sizeof from, "%s/%s.json", dir, base);
        snprintf(to, sizeof to, "%s/firmwares/%s.json", g_root, base);
        if (st_copy(from, to) == 0)
            n++;
    }
    closedir(d);
    free(have);
    return n;
}

/* a rebuild's revision: "<version>-r<N>" (an .rfm1 of the same release with new glue, FM1VST fm1_add_rfm1 VERSION):
 * N, else 0 (no revision, or another suffix: "-beta.19" compares as before) */
static int ver_rev(const char *v)
{
    const char *d = strstr(v, "-r");
    int n = 0;
    if (!d || !d[2])
        return 0;
    for (d += 2; *d >= '0' && *d <= '9'; d++)
        n = n * 10 + (*d - '0');
    return *d ? 0 : n;
}
int st_vercmp(const char *x, const char *y)
{
    int a[4] = { 0 }, b[4] = { 0 }, k;
    sscanf(x, "%d.%d.%d.%d", &a[0], &a[1], &a[2], &a[3]);
    sscanf(y, "%d.%d.%d.%d", &b[0], &b[1], &b[2], &b[3]);
    for (k = 0; k < 4; k++)
        if (a[k] != b[k])
            return a[k] > b[k] ? 1 : -1;
    k = ver_rev(x) - ver_rev(y);   /* (the same numbers: "0.42-r2" is newer than "0.42" and "0.42-r1") */
    return k > 0 ? 1 : k < 0 ? -1 : 0;
}

static int by_name(const void *a, const void *b)
{
    const st_fw_t *x = a, *y = b;
    const int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->version, y->version);
}

int st_fw_scan(st_fw_t *out, int max)
{
    char dirp[ST_PATH];
    DIR *d;
    struct dirent *e;
    int n = 0, i, j;
    st_path(dirp, sizeof dirp, "firmwares");
    if (!(d = opendir(dirp)))
        return 0;
    while ((e = readdir(d)) && n < max) {
        st_fw_t *f = &out[n];
        size_t len;
        char *json;
        const char *man;
        struct stat st;
        if (!ends_with(e->d_name, ".json"))
            continue;
        memset(f, 0, sizeof *f);
        snprintf(f->json, sizeof f->json, "%s/%s", dirp, e->d_name);
        if (!(json = (char *)st_read(f->json, &len)))
            continue;
        man = mj_find(json, "manifest");
        if (man && !mj_string(man, "id", f->id, sizeof f->id) && !mj_string(man, "version", f->version, sizeof f->version)) {
            mj_string(man, "name", f->name, sizeof f->name);
            mj_string(man, "license", f->license, sizeof f->license);
            mj_string(man, "source_url", f->source_url, sizeof f->source_url);
            mj_string(json, "sha256", f->sha256, sizeof f->sha256);
            snprintf(f->so, sizeof f->so, "%s/%.*s.rfm1m", dirp, (int)(strlen(e->d_name) - 5), e->d_name);
            if (!f->name[0])
                snprintf(f->name, sizeof f->name, "%s", f->id);
            if (stat(f->so, &st) == 0)
                n++;
        }
        free(json);
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, by_name);
    /* one entry per id: the newest version wins (versions compared as dotted numbers) */
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (!strcmp(out[i].id, out[j].id)) {
                if (st_vercmp(out[j].version, out[i].version) > 0)
                    out[i] = out[j];
                memmove(&out[j], &out[j + 1], sizeof *out * (size_t)(n - j - 1));
                n--, j--;
            }
    return n;
}

/* ------------------------------------------------------------------ states --- */
uint32_t st_states_used(const char *id)
{
    uint32_t used = 0;
    int k;
    for (k = 0; k < RFM_MOVE_FW_STATES; k++) {
        char p[ST_PATH];
        struct stat st;
        st_path(p, sizeof p, "states/%s/state-%02d.bin", id, k);
        if (stat(p, &st) == 0)
            used |= 1u << k;
    }
    return used;
}

/* ------------------------------------------------------------------ projects (rfm1cs store.h's slot I/O) --- */
static rfm1cs_store_t g_store;
static uint8_t g_pfile[RFM1CS_FILE_HEAD + RFM1CS_FILE_ENTRY + RFM_MOVE_PROJECT_MAX];
static void post_result(st_job_t *j);

static void proj_path(char *out, size_t cap, int slot) { st_path(out, cap, "move/projects/move-project-%02d.rfmmove", slot + 1); }

static int io_read(void *ctx, int slot, uint8_t *out, uint32_t cap)
{
    char p[ST_PATH];
    size_t n;
    uint8_t *b;
    rfm1cs_file_entry_t e[RFM1CS_SLOTS];
    int k, len = 0;
    (void)ctx;
    proj_path(p, sizeof p, slot);
    if (!(b = st_read(p, &n)))
        return 0;
    k = rfm1cs_file_unpack(b, (uint32_t)n, NULL, e, NULL);
    if (k >= 1 && e[0].len <= cap) {
        memcpy(out, e[0].bytes, e[0].len);
        len = (int)e[0].len;
    } else
        len = -1;
    free(b);
    return len;
}
static int io_write(void *ctx, int slot, const uint8_t *b, uint32_t n)
{
    char p[ST_PATH];
    rfm1cs_file_entry_t e;
    uint32_t len;
    (void)ctx;
    e.slot = slot, e.saved = (uint32_t)time(NULL), e.bytes = b, e.len = n;
    if (!(len = rfm1cs_file_pack(&e, 1, RFM1CS_FILE_ONE, g_pfile, sizeof g_pfile)))
        return -1;
    proj_path(p, sizeof p, slot);
    return st_write(p, g_pfile, len);
}
static int io_remove(void *ctx, int slot)
{
    char p[ST_PATH];
    (void)ctx;
    proj_path(p, sizeof p, slot);
    return unlink(p) == 0 || errno == ENOENT ? 0 : -1;
}
void st_fx_state_path(char *out, size_t cap, int slot) { st_path(out, cap, "move/projects/move-project-%02d.fx.json", slot + 1); }
static void io_loaded(void *ctx, int slot, const uint8_t *b, uint32_t n)
{
    st_job_t *r = st_job_new(R_PROJECT_LOADED);
    char p[ST_PATH];
    size_t k;
    (void)ctx;
    r->index = slot;
    st_fx_state_path(p, sizeof p, slot);   /* (its FX modules' state, beside it) */
    r->text = (char *)st_read(p, &k);
    if (b && n && (r->data = malloc(n))) {
        memcpy(r->data, b, n);
        r->len = n;
    }
    post_result(r);
}
static void io_stored(void *ctx, int slot, const uint8_t *b, uint32_t n, uint32_t used)
{
    st_job_t *r = st_job_new(R_PROJECT_STORED);
    (void)ctx;
    r->index = slot;
    r->used = used;
    if (b && n && (r->data = malloc(n))) {
        memcpy(r->data, b, n);
        r->len = n;
    }
    post_result(r);
}
static const rfm1cs_store_io_t IO = { NULL, io_read, io_write, io_remove, io_loaded, io_stored };

uint32_t st_projects_scan(int open_slot, uint8_t **blob, uint32_t *n)
{
    static rfm1cs_store_t s;   /* (a synchronous scan, before the worker: no stored() results wanted) */
    rfm1cs_store_io_t io = IO;
    uint32_t used = 0;
    int len;
    io.loaded = NULL;
    io.stored = NULL;
    rfm1cs_store_init(&s, &io, 0);
    len = rfm1cs_store_scan(&s, open_slot, &used);
    *blob = NULL, *n = 0;
    if (len > 0 && (*blob = malloc((size_t)len))) {
        memcpy(*blob, s.buf, (size_t)len);
        *n = (uint32_t)len;
    }
    return used;
}

/* ------------------------------------------------------------------ the workers --- */
/* three threads, each with its queue: the storage thread (files, firmware objects and instances, projects), the
 * network thread (catalog.h: the index, downloads: slow, so never in front of a load or a save) and the file thread
 * (a Fantasy CFW's file requests, fsvc.c: never behind a firmware load, never on the engine's or the audio thread) */
typedef struct {
    st_job_t *head, *tail;
    pthread_cond_t cv;
    pthread_t thread;
    const char *name;
} queue_t;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
#define NQ 3
static queue_t g_q[NQ] = { { NULL, NULL, PTHREAD_COND_INITIALIZER, 0, "rfm1-store" },
                           { NULL, NULL, PTHREAD_COND_INITIALIZER, 0, "rfm1-net" },
                           { NULL, NULL, PTHREAD_COND_INITIALIZER, 0, "rfm1-files" } };
static st_job_t *g_res, *g_res_tail;
static int g_stop, g_running;

st_job_t *st_job_new(int kind)
{
    st_job_t *j = calloc(1, sizeof *j);
    if (!j)
        abort();
    j->kind = kind;
    return j;
}

void st_job_free(st_job_t *j)
{
    if (!j)
        return;
    free(j->data);
    free(j->text);
    free(j);
}

static void post_result(st_job_t *j)
{
    j->next = NULL;
    pthread_mutex_lock(&g_mu);
    if (g_res_tail)
        g_res_tail->next = j;
    else
        g_res = j;
    g_res_tail = j;
    pthread_mutex_unlock(&g_mu);
}

st_job_t *st_take(void)
{
    st_job_t *j = NULL;
    if (pthread_mutex_trylock(&g_mu) != 0)
        return NULL;   /* (the engine never waits: the next pass takes it) */
    if (g_res) {
        j = g_res;
        g_res = j->next;
        if (!g_res)
            g_res_tail = NULL;
    }
    pthread_mutex_unlock(&g_mu);
    return j;
}

void st_post(st_job_t *j)
{
    queue_t *q = &g_q[j->kind == J_FS_IO ? 2 : j->kind >= J_NET_INDEX && j->kind <= J_NET_SCAN];
    j->next = NULL;
    pthread_mutex_lock(&g_mu);
    if (q->tail)
        q->tail->next = j;
    else
        q->head = j;
    q->tail = j;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&g_mu);
}

/* the log (stderr's file, as the launcher opened it) past cap: moved to <log>.1 (the one before that goes), a new one
 * in its place for stdout and stderr. Together the two stay under twice the cap. */
static void log_rotate(long cap)
{
    char path[ST_PATH], old[ST_PATH + 4];
    struct stat s2, s1;
    ssize_t k;
    int fd;
    if (fstat(2, &s2) != 0 || !S_ISREG(s2.st_mode) || s2.st_size <= cap)
        return;
    k = readlink("/proc/self/fd/2", path, sizeof path - 1);
    if (k <= 0 || path[0] != '/')
        return;
    path[k] = 0;
    if (strstr(path, " (deleted)"))
        return;
    snprintf(old, sizeof old, "%s.1", path);
    if (rename(path, old) != 0)
        return;
    if ((fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_TRUNC | O_CLOEXEC, 0644)) < 0)
        return;
    if (fstat(1, &s1) == 0 && s1.st_dev == s2.st_dev && s1.st_ino == s2.st_ino) {
        fflush(stdout);
        dup2(fd, 1);
    }
    dup2(fd, 2);
    close(fd);
    fprintf(stderr, "log: %s reached %ld bytes; the lines before are in %s\n", path, (long)s2.st_size, old);
}

static void net_progress(void *ctx, int pct)
{
    const st_job_t *j = ctx;
    st_job_t *r = st_job_new(R_DL_PROGRESS);
    snprintf(r->id, sizeof r->id, "%s", j->id);
    r->value = pct;
    post_result(r);
}

static void run_job(st_job_t *j)
{
    switch (j->kind) {
    case J_WRITE:
        if (st_write(j->path, j->data, j->len) != 0)
            fprintf(stderr, "storage: cannot write %s: %s\n", j->path, strerror(errno));
        if (j->text) {   /* (a .json beside it) */
            char p[ST_PATH + 8];
            snprintf(p, sizeof p, "%.*s.json", (int)(strlen(j->path) - 4), j->path);
            st_write(p, j->text, strlen(j->text));
        }
        st_job_free(j);
        return;
    case J_FW_OPEN: {
        size_t n = 0;
        fw_obj_t *obj = fw_obj_open(j->path, j->err, sizeof j->err);
        if (obj) {   /* the instance too: its memory and data, off the engine's thread */
            if (!(j->ptr = fw_instantiate(obj)))
                snprintf(j->err, sizeof j->err, "%s: %s", j->id, fw_inst_why());
            fw_obj_close(obj);
        }
        j->data = j->aux_path[0] ? st_read(j->aux_path, &n) : NULL;
        j->len = (uint32_t)n;
        break;
    }
    case J_FW_FREE:
        fw_inst_free(j->ptr);
        st_job_free(j);
        return;
    case J_FS_IO:
        fsvc_serve(j->ptr);
        st_job_free(j);
        return;
    case J_FX_OPEN:
        j->ptr = fx_open(j->obj, j->err, sizeof j->err);
        break;
    case J_FX_STATE:
        if (j->ptr && j->data)
            fx_state_set(j->ptr, (const char *)j->data);
        break;
    case J_FX_FREE:
        fx_close(j->ptr);
        st_job_free(j);
        return;
    case J_LOG_ROTATE:
        log_rotate(j->value);
        st_job_free(j);
        return;
    case J_STATE_SAVE: {
        char p[ST_PATH];
        st_path(p, sizeof p, "states/%s/state-%02d.bin", j->id, j->index);
        if (st_write(p, j->data, j->len) != 0)
            snprintf(j->err, sizeof j->err, "cannot write %s", p);
        else if (j->text) {
            st_path(p, sizeof p, "states/%s/state-%02d.json", j->id, j->index);
            st_write(p, j->text, strlen(j->text));
        }
        free(j->data);
        j->data = NULL, j->len = 0;
        j->used = st_states_used(j->id);
        break;
    }
    case J_STATE_LOAD: {
        char p[ST_PATH];
        size_t n = 0;
        st_path(p, sizeof p, "states/%s/state-%02d.bin", j->id, j->index);
        j->data = st_read(p, &n);
        j->len = (uint32_t)n;
        if (!j->data)
            snprintf(j->err, sizeof j->err, "no state %d for %s", j->index + 1, j->id);
        else if (j->obj && !(j->ptr = fw_instantiate(j->obj)))
            snprintf(j->err, sizeof j->err, "%s: %s", j->id, fw_inst_why());
        if (j->obj)
            fw_obj_close(j->obj);
        j->obj = NULL;
        break;
    }
    case J_STATE_DELETE: {
        char p[ST_PATH];
        st_path(p, sizeof p, "states/%s/state-%02d.bin", j->id, j->index);
        unlink(p);
        st_path(p, sizeof p, "states/%s/state-%02d.json", j->id, j->index);
        unlink(p);
        j->used = st_states_used(j->id);
        break;
    }
    case J_STATE_SCAN:
        j->used = st_states_used(j->id);
        break;
    case J_PROJECT:
        rfm1cs_store_request(&g_store, j->act, j->index, j->value, j->data, j->len);
        st_job_free(j);
        return;
    case J_NET_INDEX: {
        int changed = 0;
        j->ok = cat_fetch_index(j->used, &changed, j->err, sizeof j->err) == 0;
        if (j->ok && changed)
            j->ptr = cat_build(j->text);
        j->kind = R_INDEX;
        break;
    }
    case J_NET_DOWNLOAD:
        j->ok = cat_download(j->id, j->text, j->total, net_progress, j, j->err, sizeof j->err) == 0;
        j->ptr = cat_build((const char *)j->data);
        j->kind = R_DL_DONE;
        break;
    case J_NET_SCAN:
        j->ptr = cat_build(j->text);
        j->kind = R_CATALOG;
        break;
    default:
        st_job_free(j);
        return;
    }
    post_result(j);
}

static void *worker_main(void *arg)
{
    queue_t *q = arg;
#ifdef __linux__
    pthread_setname_np(pthread_self(), q->name);
#endif
    pthread_mutex_lock(&g_mu);
    for (;;) {
        st_job_t *j;
        while (!q->head && !g_stop)
            pthread_cond_wait(&q->cv, &g_mu);
        if (!q->head && g_stop)
            break;
        j = q->head;
        q->head = j->next;
        if (!q->head)
            q->tail = NULL;
        pthread_mutex_unlock(&g_mu);
        run_job(j);
        pthread_mutex_lock(&g_mu);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

/* fsvc's dispatcher: a file request to the file thread (from the engine's thread, between two calls) */
static void fs_dispatch(void *io)
{
    st_job_t *j = st_job_new(J_FS_IO);
    j->ptr = io;
    st_post(j);
}

int st_worker_start(uint32_t project_used)
{
    int k;
    rfm1cs_store_init(&g_store, &IO, project_used);
    g_stop = 0;
    fsvc_set_dispatch(fs_dispatch);
    for (k = 0; k < NQ; k++)
        if (pthread_create(&g_q[k].thread, NULL, worker_main, &g_q[k]) != 0)
            return -1;
    g_running = 1;
    return 0;
}

void st_worker_stop(void)
{
    int k;
    if (!g_running)
        return;
    pthread_mutex_lock(&g_mu);
    g_stop = 1;
    {   /* the network's queue: dropped (a download is not finished at exit) */
        st_job_t *j = g_q[1].head;
        while (j) {
            st_job_t *n = j->next;
            st_job_free(j);
            j = n;
        }
        g_q[1].head = g_q[1].tail = NULL;
    }
    for (k = 0; k < NQ; k++)
        pthread_cond_signal(&g_q[k].cv);
    pthread_mutex_unlock(&g_mu);
    cat_cancel();
    for (k = 0; k < NQ; k++)
        pthread_join(g_q[k].thread, NULL);
    fsvc_set_dispatch(NULL);
    g_running = 0;
}
