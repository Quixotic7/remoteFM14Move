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
#include "memsnap.h"
#include "minijson.h"
#include "projmem.h"
#include "rfm1cs/store.h"
#include "kit.h"

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
/* (rfm1cs 0.14.0, the owner's 2026-11-24: pad 1 is the open project's own sound, the library is state files 1..31)
 * a state file 0 from before is moved once to the first free index 1..31 (none of its three files there), by renames:
 * a journal (state-00.moving, the index) first, then the working memory, the .json and the image last, then the journal
 * gone; a stop half way finishes at the next scan (the journal's index), nothing lost or overwritten. No free index:
 * left as it is, never shown or written (the scan's bit 0 is always clear), said once */
static void state0_paths(const char *id, int k, char bin[ST_PATH], char mem[ST_PATH + 8], char js[ST_PATH + 8])
{
    st_path(bin, ST_PATH, "states/%s/state-%02d.bin", id, k);
    ms_path(mem, ST_PATH + 8, bin);
    pm_json_path(js, ST_PATH + 8, bin);
}
static void state0_move(const char *id)
{
    static char said[8][RFM_MOVE_FW_ID_MAX];
    char b0[ST_PATH], m0[ST_PATH + 8], j0[ST_PATH + 8], bk[ST_PATH], mk[ST_PATH + 8], jk[ST_PATH + 8], jr[ST_PATH];
    char txt[16];
    struct stat st;
    size_t n = 0;
    uint8_t *t;
    int k = -1, i;
    st_path(jr, sizeof jr, "states/%s/state-00.moving", id);
    if ((t = st_read(jr, &n))) {   /* (a move begun before a stop: its index) */
        snprintf(txt, sizeof txt, "%.*s", (int)(n < 15 ? n : 15), (const char *)t);
        free(t);
        k = atoi(txt);
        if (k < 1 || k >= RFM_MOVE_FW_STATES)
            k = -1;
    }
    state0_paths(id, 0, b0, m0, j0);
    if (k < 0) {
        if (stat(b0, &st) != 0)
            return;
        for (i = 1; i < RFM_MOVE_FW_STATES && k < 0; i++) {
            state0_paths(id, i, bk, mk, jk);
            if (stat(bk, &st) != 0 && stat(mk, &st) != 0 && stat(jk, &st) != 0)
                k = i;
        }
        if (k < 0) {
            for (i = 0; i < 8 && said[i][0] && strcmp(said[i], id); i++) {}
            if (i < 8 && !said[i][0]) {
                snprintf(said[i], sizeof said[i], "%s", id);
                fprintf(stderr, "storage: %s: state-00 (made before pad 1 was the project's sound) left in place: no "
                        "free state 2..32 to move it to; it is not shown or written\n", id);
            }
            return;
        }
        snprintf(txt, sizeof txt, "%d\n", k);
        if (st_write(jr, txt, strlen(txt)) != 0) {
            fprintf(stderr, "storage: %s: cannot write %s: %s\n", id, jr, strerror(errno));
            return;
        }
    }
    state0_paths(id, k, bk, mk, jk);
    if ((stat(m0, &st) == 0 && stat(mk, &st) != 0 && rename(m0, mk) != 0) ||
        (stat(j0, &st) == 0 && stat(jk, &st) != 0 && rename(j0, jk) != 0) ||
        (stat(b0, &st) == 0 && stat(bk, &st) != 0 && rename(b0, bk) != 0)) {
        fprintf(stderr, "storage: %s: moving state-00 to state-%02d: %s (finished at the next scan)\n", id, k,
                strerror(errno));
        return;
    }
    unlink(jr);
    fprintf(stderr, "storage: %s: state-00 (made before pad 1 was the project's sound) moved to state %d (its pad %d)\n",
            id, k, k + 1);
}
uint32_t st_states_used(const char *id)
{
    uint32_t used = 0;
    int k;
    state0_move(id);
    for (k = 1; k < RFM_MOVE_FW_STATES; k++) {
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
    {   /* a project file this host cannot read (damaged, or a newer one) is kept as stored: the grid and the page say
         * so, and nothing is made or pasted over it */
        int k;
        for (k = 0; k < RFM1CS_SLOTS; k++) {
            char p[ST_PATH];
            proj_path(p, sizeof p, k);
            if (!(used >> k & 1) && access(p, F_OK) == 0)
                used |= 1u << k;
        }
    }
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
    ms_free(j->snap);
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

/* (rfm1cs 0.13.0) the state buffer: one state's bytes and its firmware's id (STATE_COPY, STATE_DELETE; STATE_PASTE),
 * and its working memory's file as it was (memsnap.h; NULL none) */
static uint8_t *g_sbuf, *g_sbuf_mem;
static uint32_t g_sbuf_n;
static size_t g_sbuf_mem_n;
static char g_sbuf_id[RFM_MOVE_FW_ID_MAX];

static void unlink_copy(const char *bin)   /* a copy, its .json and its working memory */
{
    char p[ST_PATH + 8];
    unlink(bin);
    pm_json_path(p, sizeof p, bin);
    unlink(p);
    ms_path(p, sizeof p, bin);
    unlink(p);
}

/* ---- the working memory beside an image (memsnap.h, README "The working memory") */
/* a .mem file's bytes (b, n) for an instance of the module sha (its .rfm1's SHA-256) booting the image img: decoded,
 * or NULL with why (not this module's, not this image's: the .bin was written without it since, damaged) */
static ms_snap_t *mem_take(const uint8_t *b, size_t n, const char *sha, const uint8_t *img, uint32_t in, char *why,
                           size_t cap)
{
    char msha[65], fhex[65], ihex[65];
    ms_snap_t *m = NULL;
    if (!b)
        snprintf(why, cap, "no working memory saved with it");
    else if (ms_peek(b, n, msha, fhex) != 0)
        snprintf(why, cap, "its .mem is not a working memory this host reads");
    else if (!sha || strcmp(msha, sha))
        snprintf(why, cap, "its working memory is another build's (%.12s)", msha);
    else if (pm_hash_hex(img, in, ihex), strcmp(fhex, ihex))
        snprintf(why, cap, "its working memory goes with another flash image");
    else if (!(m = ms_decode(b, n, fhex)))
        snprintf(why, cap, "its working memory is damaged");
    else
        why[0] = 0;
    return m;
}
/* the .mem beside bin, read and taken (mem_take) */
static ms_snap_t *mem_read(const char *bin, const char *sha, const uint8_t *img, uint32_t in, char *why, size_t cap)
{
    char p[ST_PATH + 8];
    size_t n = 0;
    uint8_t *b;
    ms_snap_t *m;
    ms_path(p, sizeof p, bin);
    b = st_read(p, &n);
    m = mem_take(b, n, sha, img, in, why, cap);
    free(b);
    return m;
}
/* an image and its working memory's file (enc NULL: none, an older .mem there removed) written as bin and .mem, and
 * the .json: js (with the image's hash; NULL none), jm the same with the .mem's (used when the .mem was written). 0, -1
 * (the image not written) */
static int image_write(const char *bin, const uint8_t *img, uint32_t n, const uint8_t *enc, size_t en, const char *js,
                       const char *jm)
{
    char p[ST_PATH + 8];
    int with = 0;
    ms_path(p, sizeof p, bin);
    if (enc && st_write(p, enc, en) == 0)
        with = 1;
    else
        unlink(p);
    if (st_write(bin, img, n) != 0) {
        fprintf(stderr, "storage: cannot write %s: %s\n", bin, strerror(errno));
        return -1;
    }
    if (js) {
        const char *t = with && jm ? jm : js;
        pm_json_path(p, sizeof p, bin);
        st_write(p, t, strlen(t));
    }
    return 0;
}
/* the .json texts for image_write: text with the image's hash (*js), and with the .mem's too (*jm, when enc) */
static void image_json(const char *text, const uint8_t *img, uint32_t n, const uint8_t *enc, size_t en, char **js,
                       char **jm)
{
    char mh[65];
    *js = text ? pm_json_hash(text, img, n) : NULL;
    *jm = NULL;
    if (*js && enc) {
        pm_hash_hex(enc, en, mh);
        *jm = pm_json_add(*js, "mem", mh);
    }
}
/* a snapshot as its file, for the image img (NULL snapshot: none) */
static uint8_t *mem_encode(const ms_snap_t *m, const uint8_t *img, uint32_t n, size_t *en)
{
    char ih[65];
    *en = 0;
    if (!m)
        return NULL;
    pm_hash_hex(img, n, ih);
    return ms_encode(m, ih, en);
}
/* ---- (the kit, round C) a project's own sound folder (Kits/Projects/NN) goes with the project */
static char g_kits[ST_PATH];
void st_set_kits_dir(const char *dir) { snprintf(g_kits, sizeof g_kits, "%s", dir ? dir : ""); }
static int kdir_ok(const char *d)   /* a project's folder (or the buffer's): never anything else */
{
    return g_kits[0] && d[0] && !strncmp(d, g_kits, strlen(g_kits)) && strstr(d + strlen(g_kits), "/Projects/");
}
static void kdir_drop(const char *d)   /* its files deleted one by one, then the folder */
{
    DIR *x;
    struct dirent *e;
    if (!kdir_ok(d) || !(x = opendir(d)))
        return;
    while ((e = readdir(x))) {
        char f[ST_PATH + 256];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(f, sizeof f, "%s/%s", d, e->d_name);
        if (!lstat(f, &st) && S_ISREG(st.st_mode))
            unlink(f);
    }
    closedir(x);
    rmdir(d);
}
static int copy_file(const char *from, const char *to);
static void kdir_copy(const char *a, const char *b)   /* b made a's copy (its files; none: b gone) */
{
    DIR *x;
    struct dirent *e;
    kdir_drop(b);
    if (!kdir_ok(a) || !kdir_ok(b) || !(x = opendir(a)))
        return;
    st_mkdirs(b);
    while ((e = readdir(x))) {
        char f[ST_PATH + 256], t[ST_PATH + 256];
        struct stat st;
        if (e->d_name[0] == '.')
            continue;
        snprintf(f, sizeof f, "%s/%s", a, e->d_name);
        snprintf(t, sizeof t, "%s/%s", b, e->d_name);
        if (!lstat(f, &st) && S_ISREG(st.st_mode) && copy_file(f, t) != 0)
            fprintf(stderr, "storage: %s not copied to %s\n", f, b);
    }
    closedir(x);
}
static void kdir_move(const char *a, const char *b)
{
    kdir_drop(b);
    if (kdir_ok(a) && kdir_ok(b) && access(a, F_OK) == 0 && rename(a, b) != 0)
        kdir_copy(a, b), kdir_drop(a);
}
static void kit_files(int act, int slot, int to)   /* as proj_files does the firmwares' copies */
{
    char a[ST_PATH], b[ST_PATH], p[ST_PATH];
    if (!g_kits[0])
        return;
    kit_proj_dir(g_kits, slot, a, sizeof a);
    kit_proj_clip(g_kits, b, sizeof b);
    switch (act) {
    case RFM_MOVE_ACT_PROJECT_DELETE: kdir_drop(a); break;
    case RFM_MOVE_ACT_PROJECT_CUT:
    case RFM_MOVE_ACT_PROJECT_COPY:
        proj_path(p, sizeof p, slot);
        if (access(p, F_OK) != 0)
            return;   /* (no project there: the buffer as it was) */
        if (act == RFM_MOVE_ACT_PROJECT_CUT)
            kdir_move(a, b);
        else
            kdir_copy(a, b);
        break;
    case RFM_MOVE_ACT_PROJECT_PASTE:
        if (g_store.clip_len)
            kdir_copy(b, a);   /* (a copy: the buffer pastes again) */
        break;
    case -1:   /* (the page's duplicate: slot's onto to) */
        kit_proj_dir(g_kits, to, b, sizeof b);
        kdir_copy(a, b);
        break;
    case -2: kdir_drop(a); break;   /* (an import: an empty folder) */
    default: break;
    }
}
static int copy_file(const char *from, const char *to)   /* (st_copy without its chmod) 0, -1 */
{
    size_t n;
    uint8_t *b = st_read(from, &n);
    int rc;
    if (!b)
        return -1;
    rc = st_write(to, b, n);
    free(b);
    return rc;
}
static void copy_pair(const char *from, const char *to)   /* a copy, its .json and .mem: from replaces to (none: to gone) */
{
    char fj[ST_PATH + 8], tj[ST_PATH + 8];
    pm_json_path(fj, sizeof fj, from);
    pm_json_path(tj, sizeof tj, to);
    if (copy_file(from, to) != 0 || copy_file(fj, tj) != 0)
        unlink_copy(to);
    else {   /* (the working memory, when the copy has one) */
        ms_path(fj, sizeof fj, from);
        ms_path(tj, sizeof tj, to);
        if (copy_file(fj, tj) != 0)
            unlink(tj);
    }
}
/* a project's firmware copies (projmem.h) with the project buffer: a delete drops them, a cut moves them into the
 * buffer, a copy copies them there, a paste writes the buffer's for the target; a save drops the copy of a slot the
 * saved blob holds empty (a missing firmware's copy stays: the blob still names it) */
static void proj_files(const st_job_t *j)
{
    char a[ST_PATH], b[ST_PATH], p[ST_PATH];
    int s;
    kit_files(j->act, j->index, 0);   /* (round C: the kit's own sounds' folder too) */
    proj_path(p, sizeof p, j->index);
    for (s = 0; s < PM_SLOTS; s++) {
        pm_copy_path(a, sizeof a, g_root, j->index, s);
        pm_clip_path(b, sizeof b, g_root, s);
        switch (j->act) {
        case RFM_MOVE_ACT_PROJECT_DELETE:
            unlink_copy(a);
            break;
        case RFM_MOVE_ACT_PROJECT_CUT:
        case RFM_MOVE_ACT_PROJECT_COPY:
            if (access(p, F_OK) != 0)
                return;   /* (no project there: the buffer stays as it was, as the store's) */
            copy_pair(a, b);
            if (j->act == RFM_MOVE_ACT_PROJECT_CUT)
                unlink_copy(a);
            break;
        case RFM_MOVE_ACT_PROJECT_PASTE:
            if (!g_store.clip_len)
                return;   /* (an empty buffer: the store pastes nothing either) */
            copy_pair(b, a);
            break;
        case RFM_MOVE_ACT_PROJECT_SAVE: {
            char ids[PM_SLOTS][RFM_MOVE_FW_ID_MAX];
            if (pm_blob_ids(j->data, j->len, ids) && !ids[s][0])
                unlink_copy(a);
            break;
        }
        default:
            return;
        }
    }
}

/* ---- the page's States and projects (J_STATE_IMPORT, J_PROJECT_DUP, J_PROJECT_IMPORT): a file the page sent waits in
 * a hidden folder of the root (web.c), so each of its files goes into place by a rename */
/* dir/name moved to to: 1 moved, 0 none there, -1 a failure */
static int move_in(const char *dir, const char *name, const char *to)
{
    char from[ST_PATH + 64];
    snprintf(from, sizeof from, "%s/%s", dir, name);
    if (access(from, F_OK) != 0)
        return 0;
    if (rename(from, to) != 0) {
        fprintf(stderr, "storage: cannot put %s in place as %s: %s\n", from, to, strerror(errno));
        return -1;
    }
    return 1;
}
/* a stored project's blob (malloc'd, its length in *n), NULL none or not readable */
static uint8_t *proj_blob(int slot, uint32_t *n)
{
    uint8_t *b = malloc(RFM_MOVE_PROJECT_MAX);
    const int len = b ? io_read(NULL, slot, b, RFM_MOVE_PROJECT_MAX) : -1;
    if (len <= 0) {
        free(b);
        return NULL;
    }
    *n = (uint32_t)len;
    return b;
}
static void state_import(st_job_t *j)
{
    char p[ST_PATH], dir[ST_PATH], to[ST_PATH + 8], from[ST_PATH + 16];
    size_t n = 0;
    uint8_t *img;
    snprintf(from, sizeof from, "%s/s.bin", j->path);
    img = st_read(from, &n);
    j->ok = 0;
    if (!img)
        snprintf(j->err, sizeof j->err, "the file holds no flash image");
    else if (j->total && n != j->total)
        snprintf(j->err, sizeof j->err, "its flash image is %zu bytes, the firmware's flash %llu", n,
                 (unsigned long long)j->total);
    else if (j->index == 0) {   /* (rfm1cs 0.14.0) pad 1, the project's sound: no state file, loaded from the file */
        j->ok = 1;
        if (j->obj) {
            j->data = img, j->len = (uint32_t)n, img = NULL;
            j->snap = mem_read(from, ((fw_obj_t *)j->obj)->vt->sha256, j->data, j->len, j->memwhy, sizeof j->memwhy);
            if (!(j->ptr = fw_instantiate(j->obj)))
                snprintf(j->err, sizeof j->err, "%s: %s", j->id, fw_inst_why());
        }
    } else {
        st_path(dir, sizeof dir, "states/%s", j->id);
        st_mkdirs(dir);
        st_path(p, sizeof p, "states/%s/state-%02d.bin", j->id, j->index);
        unlink_copy(p);   /* (the state there gone first: a stop half way leaves none, never a mix of two) */
        ms_path(to, sizeof to, p);
        move_in(j->path, "s.mem", to);
        pm_json_path(to, sizeof to, p);
        move_in(j->path, "s.json", to);
        if (move_in(j->path, "s.bin", p) != 1) {   /* (the image last: the state is there from now on) */
            unlink_copy(p);
            snprintf(j->err, sizeof j->err, "cannot write state %d", j->index + 1);
        } else {
            j->ok = 1;
            if (j->value && j->obj) {   /* (loaded too, as a paste onto the active one) */
                j->data = img, j->len = (uint32_t)n, img = NULL;
                j->snap = mem_read(p, ((fw_obj_t *)j->obj)->vt->sha256, j->data, j->len, j->memwhy, sizeof j->memwhy);
                if (!(j->ptr = fw_instantiate(j->obj)))
                    snprintf(j->err, sizeof j->err, "%s: %s", j->id, fw_inst_why());
            }
        }
    }
    free(img);
    if (j->obj)
        fw_obj_close(j->obj);
    j->obj = NULL;
    j->used = st_states_used(j->id);
}
static void project_dup(st_job_t *j)
{
    char a[ST_PATH], b[ST_PATH];
    uint32_t n = 0;
    uint8_t *blob = proj_blob(j->index, &n);
    int s;
    if (!blob || rfm_move_project_check(blob, n) != 0) {
        snprintf(j->err, sizeof j->err, "project %d could not be read", j->index + 1);
        free(blob);
        return;
    }
    st_fx_state_path(a, sizeof a, j->index);
    st_fx_state_path(b, sizeof b, j->value);
    if (copy_file(a, b) != 0)
        unlink(b);
    kit_files(-1, j->index, j->value);   /* (round C: its kit's own sounds with it) */
    for (s = 0; s < PM_SLOTS; s++) {   /* (its firmwares' copies with it: a copy restores the same sound) */
        pm_copy_path(a, sizeof a, g_root, j->index, s);
        pm_copy_path(b, sizeof b, g_root, j->value, s);
        copy_pair(a, b);
    }
    if (rfm1cs_store_request(&g_store, RFM_MOVE_ACT_PROJECT_SAVE, j->value, 0, blob, n) < 0)
        snprintf(j->err, sizeof j->err, "project %d could not be written", j->value + 1);
    free(blob);
}
static void project_import(st_job_t *j)
{
    char f[ST_PATH + 16], to[ST_PATH + 8], p[ST_PATH];
    size_t n = 0;
    uint8_t *raw;
    rfm1cs_file_entry_t e[RFM1CS_SLOTS];
    rfm1cs_file_error_t fe;
    int kind = 0, s, k;
    snprintf(f, sizeof f, "%s/p.rfmmove", j->path);
    raw = st_read(f, &n);
    memset(&fe, 0, sizeof fe);
    if (!raw || (k = rfm1cs_file_unpack(raw, (uint32_t)n, &kind, e, &fe)) < 1 || kind != RFM1CS_FILE_ONE) {
        char why[200] = "";
        if (raw && fe.code)
            rfm1cs_file_error_text(&fe, NULL, why, sizeof why);
        snprintf(j->err, sizeof j->err, "not a project this Move reads%s%s", why[0] ? ": " : "", why);
        free(raw);
        return;
    }
    proj_path(p, sizeof p, j->index);
    if (access(p, F_OK) == 0)   /* (the project there gone first, its blob before its copies) */
        rfm1cs_store_request(&g_store, RFM_MOVE_ACT_PROJECT_DELETE, j->index, 0, NULL, 0);
    kit_files(-2, j->index, 0);   /* (round C: no kit sounds of its own: an imported kit's paths stay as they are) */
    for (s = 0; s < PM_SLOTS; s++) {
        char nm[32];
        pm_copy_path(p, sizeof p, g_root, j->index, s);
        unlink_copy(p);
        snprintf(nm, sizeof nm, "p.fw%d.mem", s + 1);
        ms_path(to, sizeof to, p);
        move_in(j->path, nm, to);
        snprintf(nm, sizeof nm, "p.fw%d.json", s + 1);
        pm_json_path(to, sizeof to, p);
        move_in(j->path, nm, to);
        snprintf(nm, sizeof nm, "p.fw%d.bin", s + 1);
        if (move_in(j->path, nm, p) < 0)
            unlink_copy(p);
    }
    st_fx_state_path(p, sizeof p, j->index);
    unlink(p);
    move_in(j->path, "p.fx.json", p);
    if (rfm1cs_store_request(&g_store, RFM_MOVE_ACT_PROJECT_SAVE, j->index, 0, e[0].bytes, e[0].len) < 0)
        snprintf(j->err, sizeof j->err, "project %d could not be written", j->index + 1);
    free(raw);
}

static void run_job(st_job_t *j)
{
    switch (j->kind) {
    case J_WRITE: {
        const char *to[3] = { j->path, j->aux_path, j->path2 };
        size_t en = 0;
        uint8_t *enc = j->act & 7 ? mem_encode(j->snap, j->data, j->len, &en) : NULL;   /* (one file for each) */
        char *js, *jm;
        int k;
        image_json(j->text, j->data, j->len, enc, en, &js, &jm);
        for (k = 0; k < 3; k++) {
            if (!to[k][0])
                continue;
            if (j->act >> k & 1)   /* (the image and its working memory: a state, a project's copy) */
                image_write(to[k], j->data, j->len, enc, en, js, jm);
            else if (st_write(to[k], j->data, j->len) != 0)
                fprintf(stderr, "storage: cannot write %s: %s\n", to[k], strerror(errno));
            else if (js) {   /* (a .json beside it, with the image's hash) */
                char p[ST_PATH + 8];
                pm_json_path(p, sizeof p, to[k]);
                st_write(p, js, strlen(js));
            }
        }
        free(js);
        free(jm);
        free(enc);
        st_job_free(j);
        return;
    }
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
        size_t en = 0;
        uint8_t *enc = mem_encode(j->snap, j->data, j->len, &en);
        char *js, *jm;
        image_json(j->text, j->data, j->len, enc, en, &js, &jm);
        st_path(p, sizeof p, "states/%s/state-%02d.bin", j->id, j->index);
        if (image_write(p, j->data, j->len, enc, en, js, jm) != 0)
            snprintf(j->err, sizeof j->err, "cannot write %s", p);
        free(js);
        free(jm);
        j->ok = enc != NULL;   /* (with its working memory) */
        j->total = en;
        free(enc);
        free(j->data);
        j->data = NULL, j->len = 0;
        ms_free(j->snap);
        j->snap = NULL;
        j->used = st_states_used(j->id);
        break;
    }
    case J_STATE_LOAD: {
        char p[ST_PATH];
        size_t n = 0;
        if (j->act == STA_LOAD) {
            st_path(p, sizeof p, "states/%s/state-%02d.bin", j->id, j->index);
            j->data = st_read(p, &n);
            j->len = (uint32_t)n;
            if (j->data && j->obj)   /* (its working memory, for this very module and image) */
                j->snap = mem_read(p, ((fw_obj_t *)j->obj)->vt->sha256, j->data, j->len, j->memwhy, sizeof j->memwhy);
        }
        if (!j->data && j->act != STA_NEW)
            snprintf(j->err, sizeof j->err, "no state %d for %s", j->index + 1, j->id);
        else if (j->obj && !(j->ptr = fw_instantiate(j->obj)))
            snprintf(j->err, sizeof j->err, "%s: %s", j->id, fw_inst_why());
        if (j->obj)
            fw_obj_close(j->obj);
        j->obj = NULL;
        break;
    }
    case J_STATE_COPY:
    case J_STATE_DELETE: {   /* (a delete is a cut: the state into the buffer first) */
        char p[ST_PATH];
        size_t n = 0;
        uint8_t *b;
        if (j->kind == J_STATE_COPY && j->data) {   /* (rfm1cs 0.14.0, index 0) the memory now, synced, with its working
                                                    * memory: the engine's image and snapshot */
            free(g_sbuf);
            free(g_sbuf_mem);
            g_sbuf_mem = mem_encode(j->snap, j->data, j->len, &g_sbuf_mem_n);
            g_sbuf = j->data, g_sbuf_n = j->len, j->data = NULL, j->len = 0;
            ms_free(j->snap);
            j->snap = NULL;
            snprintf(g_sbuf_id, sizeof g_sbuf_id, "%s", j->id);
            j->used = st_states_used(j->id);
            break;
        }
        st_path(p, sizeof p, "states/%s/state-%02d.bin", j->id, j->index);
        if (!(j->kind == J_STATE_DELETE && j->value) && (b = st_read(p, &n))) {   /* (the page's: not into it) */
            char mp[ST_PATH + 8];
            free(g_sbuf);
            free(g_sbuf_mem);
            g_sbuf = b, g_sbuf_n = (uint32_t)n;
            snprintf(g_sbuf_id, sizeof g_sbuf_id, "%s", j->id);
            ms_path(mp, sizeof mp, p);   /* (its working memory, as it is) */
            g_sbuf_mem = st_read(mp, &g_sbuf_mem_n);
        }
        if (j->kind == J_STATE_COPY) {
            j->used = st_states_used(j->id);
            break;
        }
        unlink_copy(p);
        j->used = st_states_used(j->id);
        break;
    }
    case J_STATE_SCAN:
        j->used = st_states_used(j->id);
        break;
    case J_STATE_PASTE: {
        char p[ST_PATH];
        j->ok = 0;
        if (g_sbuf && !strcmp(g_sbuf_id, j->id)) {
            char *js, *jm;
            int w = 0;
            st_path(p, sizeof p, "states/%s/state-%02d.bin", j->id, j->index);
            if (j->index > 0) {   /* (rfm1cs 0.14.0) index 0 is the project's sound: loaded only, no state file */
                image_json(j->text, g_sbuf, g_sbuf_n, g_sbuf_mem, g_sbuf_mem_n, &js, &jm);
                w = image_write(p, g_sbuf, g_sbuf_n, g_sbuf_mem, g_sbuf_mem_n, js, jm);   /* (and its .mem) */
                free(js);
                free(jm);
            }
            if (w == 0) {
                j->ok = 1;
                if (j->index == 0 && j->obj && (j->data = malloc(g_sbuf_n))) {   /* (into the project: loaded) */
                    memcpy(j->data, g_sbuf, g_sbuf_n);
                    j->len = g_sbuf_n;
                    j->snap = mem_take(g_sbuf_mem, g_sbuf_mem_n, ((fw_obj_t *)j->obj)->vt->sha256, g_sbuf, g_sbuf_n,
                                       j->memwhy, sizeof j->memwhy);
                    if (!(j->ptr = fw_instantiate(j->obj)))
                        snprintf(j->err, sizeof j->err, "%s: %s", j->id, fw_inst_why());
                }
            } else
                snprintf(j->err, sizeof j->err, "cannot write %s", p);
        }
        if (j->obj)
            fw_obj_close(j->obj);
        j->obj = NULL;
        j->used = st_states_used(j->id);
        break;
    }
    case J_PFW_OPEN: {
        size_t n = 0;
        char jp[ST_PATH + 8], id[RFM_MOVE_FW_ID_MAX], sha[65] = "";
        if (j->obj) {   /* (the same firmware: a new instance of its object) */
            snprintf(sha, sizeof sha, "%s", ((fw_obj_t *)j->obj)->vt->sha256);
            if (!(j->ptr = fw_instantiate(j->obj)))
                snprintf(j->err, sizeof j->err, "%s: %s", j->id, fw_inst_why());
            fw_obj_close(j->obj);
            j->obj = NULL;
        } else {
            fw_obj_t *obj = fw_obj_open(j->path, j->err, sizeof j->err);
            if (obj) {
                snprintf(sha, sizeof sha, "%s", obj->vt->sha256);
                if (!(j->ptr = fw_instantiate(obj)))
                    snprintf(j->err, sizeof j->err, "%s: %s", j->id, fw_inst_why());
                fw_obj_close(obj);
            }
        }
        j->ok = 0, j->match = -1;
        if (j->ptr) {
            pm_json_path(jp, sizeof jp, j->aux_path);   /* the project's copy, when its json names this firmware */
            if (pm_json_id(jp, id, sizeof id) == 0 && !strcmp(id, j->id) && (j->data = st_read(j->aux_path, &n))) {
                j->ok = 2;
                j->snap = mem_read(j->aux_path, sha, j->data, (uint32_t)n, j->memwhy, sizeof j->memwhy);
            } else if (j->path2[0] && (j->data = st_read(j->path2, &n)))   /* (else its last memory anywhere) */
                j->ok = 1;
            j->len = (uint32_t)n;   /* (rfm1cs 0.14.0: no state is active, none is matched) */
        }
        break;
    }
    case J_PROJECT_INFO: {   /* (the page's and the grid's brief of a stored project, at start) */
        uint8_t *b = malloc(RFM_MOVE_PROJECT_MAX);
        const int len = b ? io_read(NULL, j->index, b, RFM_MOVE_PROJECT_MAX) : -1;
        j->value = 0;
        if (len > 0)
            j->data = b, j->len = (uint32_t)len, b = NULL;
        else if (len < 0) {   /* (the container itself: a newer one is a word, a broken one another) */
            char p[ST_PATH];
            size_t n = 0;
            uint8_t *raw;
            proj_path(p, sizeof p, j->index);
            raw = st_read(p, &n);
            /* (the file's version, or its one project's: "RMVP", its version, ..., the blob at 18 with its at 22) */
            j->value = raw && n > 5 && !memcmp(raw, "RMVP", 4) &&
                       (raw[4] > 1 || (n > 23 && !memcmp(raw + 18, "RMSQ", 4) && raw[22] > RFM_MOVE_PROJECT_VERSION)) ? -2 : -1;
            free(raw);
        }
        free(b);
        break;
    }
    case J_SYNC:
        break;
    case J_STATE_IMPORT:
        state_import(j);
        break;
    case J_PROJECT_DUP:
        project_dup(j);
        break;
    case J_PROJECT_IMPORT:
        project_import(j);
        break;
    case J_PROJECT:
        proj_files(j);   /* (the firmwares' copies travel with the project: before the store's own work) */
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
