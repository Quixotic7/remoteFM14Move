/* SPDX-License-Identifier: MPL-2.0 */
/* The root folder and the storage thread: see storage.h. */
#define _GNU_SOURCE
#include "storage.h"

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

int st_fw_install_bundled(const char *dir)
{
    DIR *d = dir ? opendir(dir) : NULL;
    struct dirent *e;
    int n = 0;
    if (!d)
        return 0;
    while ((e = readdir(d))) {
        char base[256], from[ST_PATH], to[ST_PATH];
        struct stat st;
        static const char *SUF[] = { ".rfm1m", ".json", ".licenses.txt", ".rfm1skin", ".rfm1skin.bg", ".rfm1skin.bgl" };
        size_t k;
        if (!ends_with(e->d_name, ".json") || strlen(e->d_name) >= sizeof base)
            continue;
        snprintf(base, sizeof base, "%.*s", (int)(strlen(e->d_name) - 5), e->d_name);
        snprintf(to, sizeof to, "%s/firmwares/%s.json", g_root, base);
        if (stat(to, &st) == 0)
            continue;   /* there already (the user's, or an earlier install) */
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
    return n;
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
                int a[4] = { 0 }, b[4] = { 0 }, k, newer = 0;
                sscanf(out[i].version, "%d.%d.%d.%d", &a[0], &a[1], &a[2], &a[3]);
                sscanf(out[j].version, "%d.%d.%d.%d", &b[0], &b[1], &b[2], &b[3]);
                for (k = 0; k < 4 && !newer; k++)
                    newer = b[k] > a[k] ? 1 : b[k] < a[k] ? -1 : 0;
                if (newer > 0)
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
static void io_loaded(void *ctx, int slot, const uint8_t *b, uint32_t n)
{
    st_job_t *r = st_job_new(R_PROJECT_LOADED);
    (void)ctx;
    r->index = slot;
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

/* ------------------------------------------------------------------ the worker --- */
static pthread_t g_thread;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static st_job_t *g_jobs, *g_jobs_tail, *g_res, *g_res_tail;
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
    j->next = NULL;
    pthread_mutex_lock(&g_mu);
    if (g_jobs_tail)
        g_jobs_tail->next = j;
    else
        g_jobs = j;
    g_jobs_tail = j;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
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
        j->obj = fw_obj_open(j->path, j->err, sizeof j->err);
        j->data = j->aux_path[0] ? st_read(j->aux_path, &n) : NULL;
        j->len = (uint32_t)n;
        break;
    }
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
    default:
        st_job_free(j);
        return;
    }
    post_result(j);
}

static void *worker_main(void *arg)
{
    (void)arg;
#ifdef __linux__
    pthread_setname_np(pthread_self(), "rfm1-store");
#endif
    pthread_mutex_lock(&g_mu);
    for (;;) {
        st_job_t *j;
        while (!g_jobs && !g_stop)
            pthread_cond_wait(&g_cv, &g_mu);
        if (!g_jobs && g_stop)
            break;
        j = g_jobs;
        g_jobs = j->next;
        if (!g_jobs)
            g_jobs_tail = NULL;
        pthread_mutex_unlock(&g_mu);
        run_job(j);
        pthread_mutex_lock(&g_mu);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

int st_worker_start(uint32_t project_used)
{
    rfm1cs_store_init(&g_store, &IO, project_used);
    g_stop = 0;
    if (pthread_create(&g_thread, NULL, worker_main, NULL) != 0)
        return -1;
    g_running = 1;
    return 0;
}

void st_worker_stop(void)
{
    if (!g_running)
        return;
    pthread_mutex_lock(&g_mu);
    g_stop = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_join(g_thread, NULL);
    g_running = 0;
}
