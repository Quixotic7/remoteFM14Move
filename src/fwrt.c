/* SPDX-License-Identifier: MPL-2.0 */
/* fwrt: see fwrt.h. The WASI imports of RFM1-FORMAT.md section 3 are defined here, once for every firmware object:
 * the objects resolve them (and wasm-rt-impl.c's runtime) against this executable at dlopen. */
#define _GNU_SOURCE
#include "fwrt.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wasm-rt.h"
#include "wasm-rt-impl.h"
#include "fsvc.h"
#include "minijson.h"

/* ------------------------------------------------------------------ the layout (RFM1-FORMAT.md section 5) --- */
/* fm1panel_t (fm1core.h, ABI 5/6) and fm1core_t as wasm32 lays them out: what rfm1_layout must say */
static const uint32_t WANT[34] = {
    115384,                                                          /* sizeof(fm1panel_t) */
    0, 4, 8, 12, 16, 44, 48, 75, 102, 146, 148, 115348, 115352, 115366, 115373, 115380,
    192,                                                             /* sizeof(fm1core_t), wasm32 */
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 92, 96, 100, 104, 108, 188,
};
enum { PL_SIZE, PL_KEYS, PL_BUTTONS, PL_KEYS_TAP, PL_BUTTONS_TAP, PL_ENC, PL_MASTER, PL_KEY_VEL, PL_KEY_LEAN, PL_LED,
       PL_PLAY_GREEN, PL_LCD, PL_LCD_WRITES, PL_BTN_ID, PL_ENC_ID, PL_ENC_DIR, PL_READY,
       DL_SIZE, DL_ABI, DL_ID, DL_NAME, DL_VERSION, DL_SOURCE, DL_FAMILY, DL_LICENSE, DL_GATED, DL_FLASH_SIZE,
       DL_BUTTON_NAMES, DL_PARAMS, DL_NPARAMS, DL_PANEL, DL_KEYMAP, DL_OPTIONS, DL_CREDITS };

#define SCRATCH 8192
#define S_STR 0           /* strings in (the flash path), ui_info out: 1024 bytes */
#define S_AUDIO 1024      /* one block out: 512 bytes */
#define S_WORD 1536       /* a word out (midi_out_take) */
#define S_STATS 1544

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int in_mem(const fw_t *fw, uint32_t a, uint32_t n) { return (uint64_t)a + n <= fw->mem_size; }
static uint32_t rd32(const fw_t *fw, uint32_t a)
{
    uint32_t v = 0;
    if (in_mem(fw, a, 4))
        memcpy(&v, fw->mem + a, 4);
    return v;
}
static void wr32(fw_t *fw, uint32_t a, uint32_t v)
{
    if (in_mem(fw, a, 4))
        memcpy(fw->mem + a, &v, 4);
}
static uint8_t rd8(const fw_t *fw, uint32_t a) { return in_mem(fw, a, 1) ? fw->mem[a] : 0; }
static void wr8(fw_t *fw, uint32_t a, uint8_t v)
{
    if (in_mem(fw, a, 1))
        fw->mem[a] = v;
}
/* a C string in memory, copied (cut) into out */
static void rdstr(const fw_t *fw, uint32_t a, char *out, size_t cap)
{
    size_t n = 0;
    if (a)
        while (n + 1 < cap && in_mem(fw, a + (uint32_t)n, 1) && fw->mem[a + n])
            out[n] = (char)fw->mem[a + n], n++;
    out[n] = 0;
}
#define P(i) (fw->panel + fw->L[i])

/* ------------------------------------------------------------------ the WASI imports --- */
static fw_t *of(struct Z_wasi_snapshot_preview1_instance_t *w) { return w ? w->fw : NULL; }

/* the firmware's stdout / stderr into the log: whole lines, one write each, rate-limited per instance (a token bucket:
 * FW_LOG_RATE lines a second, bursts of FW_LOG_BURST); what the limit holds back is counted, and said in one line a
 * second at most (fw_log_flush: the engine's housekeeping). A firmware that prints on every key (ChoralRoot: "touch: key 15
 * vel 100 fixed") would otherwise write the Move's flash storage from the engine's thread all the time. */
static void log_line(fw_t *fw)
{
    const uint64_t t = now_ns();
    if (fw->log_ns)
        fw->log_tokens += (double)(t - fw->log_ns) * 1e-9 * FW_LOG_RATE;
    else
        fw->log_tokens = FW_LOG_BURST;
    if (fw->log_tokens > FW_LOG_BURST)
        fw->log_tokens = FW_LOG_BURST;
    fw->log_ns = t;
    if (fw->log_tokens < 1.0) {
        fw->log_dropped++;
        fw->line_n = 0;
        return;
    }
    fw->log_tokens -= 1.0;
    fprintf(stderr, "[%s] %.*s\n", fw->id[0] ? fw->id : "fw", fw->line_n, fw->line);
    fw->log_said++;
    fw->line_n = 0;
}

void fw_log_flush(fw_t *fw)
{
    if (fw->log_dropped) {
        fprintf(stderr, "[%s] (%u lines dropped: at most %d a second)\n", fw->id[0] ? fw->id : "fw", fw->log_dropped,
                FW_LOG_RATE);
        fw->log_dropped = 0;
    }
}

uint32_t Z_wasi_snapshot_preview1Z_fd_write(struct Z_wasi_snapshot_preview1_instance_t *w, uint32_t fd, uint32_t iov, uint32_t n, uint32_t out)
{
    fw_t *fw = of(w);
    uint32_t total = 0, i;
    if (!fw)
        return 8;
    for (i = 0; i < n; i++) {
        const uint32_t p = rd32(fw, iov + 8 * i), l = rd32(fw, iov + 8 * i + 4);
        if ((fd == 1 || fd == 2) && in_mem(fw, p, l)) {
            uint32_t k;
            for (k = 0; k < l; k++) {
                const char ch = (char)fw->mem[p + k];
                if (ch == '\n')
                    log_line(fw);
                else if (fw->line_n < (int)sizeof fw->line)
                    fw->line[fw->line_n++] = ch == '\r' || ch == '\t' ? ' ' : ch;
            }
        }
        total += l;
    }
    wr32(fw, out, total);
    return fd == 1 || fd == 2 ? 0 : 8;   /* 8: EBADF */
}
uint32_t Z_wasi_snapshot_preview1Z_fd_read(struct Z_wasi_snapshot_preview1_instance_t *w, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    (void)w, (void)a, (void)b, (void)c, (void)d;
    return 8;
}
uint32_t Z_wasi_snapshot_preview1Z_fd_close(struct Z_wasi_snapshot_preview1_instance_t *w, uint32_t a)
{
    (void)w, (void)a;
    return 8;
}
uint32_t Z_wasi_snapshot_preview1Z_fd_seek(struct Z_wasi_snapshot_preview1_instance_t *w, uint32_t a, uint64_t b, uint32_t c, uint32_t d)
{
    (void)w, (void)a, (void)b, (void)c, (void)d;
    return 8;
}
uint32_t Z_wasi_snapshot_preview1Z_clock_time_get(struct Z_wasi_snapshot_preview1_instance_t *w, uint32_t id, uint64_t prec, uint32_t out)
{
    fw_t *fw = of(w);
    const uint64_t ns = now_ns();
    (void)id, (void)prec;
    if (fw && in_mem(fw, out, 8))
        memcpy(fw->mem + out, &ns, 8);
    return 0;
}
void Z_wasi_snapshot_preview1Z_proc_exit(struct Z_wasi_snapshot_preview1_instance_t *w, uint32_t code)
{
    fw_t *fw = of(w);
    if (fw)
        fw->exit_code = (int)(code & 0xFF);
    wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE);   /* unwinds to the call's guard: the instance halts (0x100 | code) */
}

/* ------------------------------------------------------------------ rfm1_host_1 (fsvc.h) --- */
/* a Fantasy CFW's file API: checked and noted during the call; answered between calls (fsvc_pump in run_ms) */
static struct fsvc *fsof(struct Z_rfm1_host_1_instance_t *h, fw_t **fw)
{
    *fw = h ? h->w.fw : NULL;
    return *fw ? h->w.fs : NULL;
}
uint32_t Z_rfm1_host_1Z_fs_request(struct Z_rfm1_host_1_instance_t *h, uint32_t op, uint32_t path, uint32_t path_len,
                                   uint64_t off, uint32_t buf, uint32_t len)
{
    fw_t *fw;
    struct fsvc *s = fsof(h, &fw);
    if (!s)
        return (uint32_t)FSV_ENOFOLDER;
    return (uint32_t)fsvc_request(s, fw->mem, fw->mem_size, (int32_t)op, path, (int32_t)path_len, (int64_t)off, buf,
                                  (int32_t)len);
}
uint32_t Z_rfm1_host_1Z_fs_poll(struct Z_rfm1_host_1_instance_t *h, uint32_t id)
{
    fw_t *fw;
    struct fsvc *s = fsof(h, &fw);
    return (uint32_t)(s ? fsvc_poll(s, (int32_t)id) : FSV_ENOFOLDER);
}
void Z_rfm1_host_1Z_fs_cancel(struct Z_rfm1_host_1_instance_t *h, uint32_t id)
{
    fw_t *fw;
    struct fsvc *s = fsof(h, &fw);
    if (s)
        fsvc_cancel(s, (int32_t)id);
}
uint32_t Z_rfm1_host_1Z_fs_changed(struct Z_rfm1_host_1_instance_t *h)
{
    fw_t *fw;
    struct fsvc *s = fsof(h, &fw);
    return (uint32_t)(s ? fsvc_changed(s) : 0);
}

/* ------------------------------------------------------------------ the object --- */
/* the manifest beside the object (<base>.json): what a Fantasy CFW needs; 0, or -1 with err (a need this host lacks) */
static int obj_manifest(fw_obj_t *o, char *err, size_t cap)
{
    char p[sizeof o->path + 8];
    const size_t n = strlen(o->path);
    FILE *f;
    long len;
    char *j;
    const char *m, *it, *v;
    if (n < 6 || strcmp(o->path + n - 6, ".rfm1m"))
        return 0;
    snprintf(p, sizeof p, "%.*s.json", (int)(n - 6), o->path);
    if (!(f = fopen(p, "rb")))
        return 0;   /* (no manifest beside it: an object run alone, as before) */
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (1 << 20) || !(j = calloc(1, (size_t)len + 1)) || fread(j, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        return 0;
    }
    fclose(f);
    m = mj_find(j, "manifest");
    m = m ? m : j;
    {
        long pages = 0, mib = 0;
        if (!mj_int(j, "memory_pages", &pages) && pages > 0)
            o->mem_bytes = (uint64_t)pages * 65536u;
        else if (!mj_int(m, "memory_mib", &mib) && mib > 0)
            o->mem_bytes = (uint64_t)mib << 20;
    }
    v = mj_find(m, "virtual");
    o->virt = v && !strncmp(v, "true", 4);
    if ((it = mj_find(m, "needs")) && *it == '[') {
        const char *e;
        while ((e = mj_elem(&it))) {
            char need[32];
            if (*e != '"' || mj_span(e) < 2 || mj_span(e) - 2 >= sizeof need) {
                snprintf(err, cap, "%s: needs something this host does not know: a newer RemoteFM1", o->path);
                free(j);
                return -1;
            }
            memcpy(need, e + 1, mj_span(e) - 2);
            need[mj_span(e) - 2] = 0;
            if (!strcmp(need, "fs1"))
                o->fs1 = 1;
            else {
                snprintf(err, cap, "needs \"%s\", which this host does not give yet: a newer RemoteFM1", need);
                free(j);
                return -1;
            }
        }
    }
    if (o->fs1)
        fsvc_access(m, o->access);
    free(j);
    return 0;
}


fw_obj_t *fw_obj_open(const char *path, char *err, size_t cap)
{
    fw_obj_t *o = calloc(1, sizeof *o);
    rfm1m_get_fn get;
    if (!o) {
        snprintf(err, cap, "no memory");
        return NULL;
    }
    o->so = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!o->so) {
        snprintf(err, cap, "%s", dlerror());
        free(o);
        return NULL;
    }
    get = (rfm1m_get_fn)(uintptr_t)dlsym(o->so, RFM1M_SYMBOL);
    if (!get || !(o->vt = get()) || o->vt->abi != RFM1M_ABI) {
        snprintf(err, cap, "%s: not a compiled firmware (rfm1m ABI %u)", path, RFM1M_ABI);
        dlclose(o->so);
        free(o);
        return NULL;
    }
    snprintf(o->path, sizeof o->path, "%s", path);
    if (obj_manifest(o, err, cap) != 0) {
        dlclose(o->so);
        free(o);
        return NULL;
    }
    o->vt->init_module();
    o->refs = 1;
    return o;
}

void fw_obj_ref(fw_obj_t *o) { __atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL); }

void fw_obj_close(fw_obj_t *o)
{
    if (!o || __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) > 0)
        return;
    /* the object stays mapped: wasm2c's init_module registered its function types in the runtime, which keeps them */
    free(o);
}

/* ------------------------------------------------------------------ guarded calls --- */
static void trapped(fw_t *fw, wasm_rt_trap_t code, const char *what)
{
    fw->dead = 1;
    if (fw->exit_code >= 0) {
        fw->halted = 0x100u | (uint32_t)fw->exit_code;
        snprintf(fw->err, sizeof fw->err, "%s: exit(%d) in %s", fw->id, fw->exit_code, what);
    } else
        snprintf(fw->err, sizeof fw->err, "%s: trap %d (%s) in %s", fw->id, (int)code, wasm_rt_strerror(code), what);
    fprintf(stderr, "fwrt: %s\n", fw->err);
}

/* call: an expression on fw->obj->vt and fw->inst; on a trap the instance is dead and the expression's value is 0 */
#define CALL(fw, what, expr)                                                                                        \
    do {                                                                                                            \
        wasm_rt_trap_t code_ = (wasm_rt_trap_t)wasm_rt_impl_try();                                                  \
        if (code_ == WASM_RT_TRAP_NONE) {                                                                           \
            expr;                                                                                                   \
        } else                                                                                                      \
            trapped((fw), code_, (what));                                                                           \
    } while (0)

/* ------------------------------------------------------------------ an instance --- */
static __thread char g_why[160];
const char *fw_inst_why(void) { return g_why[0] ? g_why : "no memory for an instance"; }

long fw_mem_free_mb(void)
{
    const char *t = getenv("RFM14_MEM_AVAIL_MB");
    long kb = -1;
    FILE *f;
    char line[128];
    if (t && *t)
        return atol(t) - FW_MEM_RESERVE_MB;
    if (!(f = fopen("/proc/meminfo", "r")))
        return -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1)
            break;
    fclose(f);
    return kb < 0 ? -1 : kb / 1024 - FW_MEM_RESERVE_MB;
}

fw_inst_t *fw_instantiate(fw_obj_t *obj)
{
    fw_inst_t *p;
    g_why[0] = 0;
    if (obj->mem_bytes > (64u << 20)) {   /* (a big one: wasm2c's calloc of all of it, locked: refused, not killed) */
        const long free_mb = fw_mem_free_mb(), need = (long)((obj->mem_bytes + (1u << 20) - 1) >> 20);
        if (free_mb >= 0 && need > free_mb) {
            snprintf(g_why, sizeof g_why, "%s needs %ld MB of memory, %ld MB free", obj->vt->id, need,
                     free_mb > 0 ? free_mb : 0);
            return NULL;
        }
    }
    p = calloc(1, sizeof *p);
    if (!p || !(p->wasi = calloc(1, sizeof *p->wasi))) {
        free(p);
        return NULL;
    }
    p->obj = obj;
    fw_obj_ref(obj);
    if (obj->fs1 && !(p->wasi->fs = fsvc_new(obj->vt->id, obj->access))) {
        fw_inst_free(p);
        return NULL;
    }
    if (!(p->inst = obj->vt->create(p->wasi))) {   /* (wasm2c's instantiate: allocations and copies, no wasm code) */
        fw_inst_free(p);
        return NULL;
    }
    return p;
}

void fw_inst_free(fw_inst_t *p)
{
    if (!p)
        return;
    if (p->inst)
        p->obj->vt->destroy(p->inst);
    fw_obj_close(p->obj);
    if (p->wasi)
        fsvc_release(p->wasi->fs);
    free(p->wasi);
    free(p);
}

int fw_create(fw_t *fw, fw_obj_t *obj)
{
    fw_inst_t *p = fw_instantiate(obj);
    if (!p) {
        memset(fw, 0, sizeof *fw);
        snprintf(fw->err, sizeof fw->err, "no memory for an instance");
        return -1;
    }
    return fw_create_from(fw, p);
}

int fw_create_from(fw_t *fw, fw_inst_t *pre)
{
    const rfm1m_t *vt = pre->obj->vt;
    volatile uint32_t ver = 0, desc = 0, i;
    memset(fw, 0, sizeof *fw);
    fw->exit_code = -1;
    fw->obj = pre->obj;   /* (the instance's reference is fw's now) */
    snprintf(fw->id, sizeof fw->id, "%s", vt->id);
    fw->inst = pre->inst;
    fw->wasi = pre->wasi;
    fw->wasi->fw = fw;
    free(pre);
    fw->mem = vt->memory(fw->inst, &fw->mem_size);
    if (vt->initialize)
        CALL(fw, "_initialize", vt->initialize(fw->inst));
    if (fw->dead)
        return -1;
    CALL(fw, "rfm1_version", ver = vt->version(fw->inst));
    if (!fw->dead && ver != 1) {
        snprintf(fw->err, sizeof fw->err, "rfm1_version %u, not 1", (unsigned)ver);
        fw->dead = 1;
    }
    if (!fw->dead)
        CALL(fw, "rfm1_get", desc = vt->get(fw->inst, 6));
    if (!fw->dead && !desc)   /* an ABI 5 module: its descriptor is ABI 6's without credits (the layout the same) */
        CALL(fw, "rfm1_get", desc = vt->get(fw->inst, 5));
    if (!fw->dead && !desc) {
        snprintf(fw->err, sizeof fw->err, "rfm1_get(6) and (5) are 0: another ABI");
        fw->dead = 1;
    }
    for (i = 0; i < 34 && !fw->dead; i++) {
        volatile uint32_t v = 0;
        CALL(fw, "rfm1_layout", v = vt->layout(fw->inst, i));
        fw->L[i] = v;
        if (!fw->dead && v != WANT[i] && !(i == DL_CREDITS && v == (uint32_t)-1)) {
            snprintf(fw->err, sizeof fw->err, "rfm1_layout(%u) = %u, this host expects %u", (unsigned)i, (unsigned)v,
                     (unsigned)WANT[i]);
            fw->dead = 1;
        }
    }
    if (fw->dead)
        return -1;
    fw->desc = desc;
    fw->panel = rd32(fw, desc + fw->L[DL_PANEL]);
    if (!fw->panel || !in_mem(fw, fw->panel, fw->L[PL_SIZE])) {
        snprintf(fw->err, sizeof fw->err, "the panel is outside the module's memory");
        fw->dead = 1;
        return -1;
    }
    {
        char id[32];
        rdstr(fw, rd32(fw, desc + fw->L[DL_ID]), id, sizeof id);
        if (strcmp(id, fw->id)) {
            snprintf(fw->err, sizeof fw->err, "the descriptor's id \"%s\" is not the manifest's \"%s\"", id, fw->id);
            fw->dead = 1;
            return -1;
        }
    }
    rdstr(fw, rd32(fw, desc + fw->L[DL_NAME]), fw->name, sizeof fw->name);
    rdstr(fw, rd32(fw, desc + fw->L[DL_VERSION]), fw->version, sizeof fw->version);
    fw->flash_size = rd32(fw, desc + fw->L[DL_FLASH_SIZE]);
    for (i = 0; i < FW_NBTN; i++)
        rdstr(fw, rd32(fw, desc + fw->L[DL_BUTTON_NAMES] + 4 * i), fw->btn_name[i], sizeof fw->btn_name[i]);
    {
        const uint32_t km = rd32(fw, desc + fw->L[DL_KEYMAP]);
        if (km && in_mem(fw, km, sizeof fw->keymap)) {
            memcpy(fw->keymap, fw->mem + km, sizeof fw->keymap);
            fw->has_keymap = 1;
        }
    }
    {
        volatile uint32_t s = 0;
        CALL(fw, "rfm1_alloc", s = vt->alloc(fw->inst, SCRATCH));
        if (fw->dead || !s || !in_mem(fw, s, SCRATCH)) {
            snprintf(fw->err, sizeof fw->err, "rfm1_alloc(%d) failed", SCRATCH);
            fw->dead = 1;
            return -1;
        }
        fw->scratch = s;
    }
    return 0;
}

int fw_boot(fw_t *fw, const uint8_t *flash, uint32_t n)
{
    const rfm1m_t *vt = fw->obj->vt;
    static const char staged[] = "fm1core:staged";
    volatile uint32_t buf = 0;
    const uint64_t t0 = now_ns();
    if (fw->dead || fw->booted)
        return -1;
    if (flash && n) {   /* the saved image into the module's memory, staged (copied by the core) */
        CALL(fw, "rfm1_alloc", buf = vt->alloc(fw->inst, n));
        if (fw->dead || !buf || !in_mem(fw, buf, n)) {
            snprintf(fw->err, sizeof fw->err, "no room in the module for a %u-byte flash image", (unsigned)n);
            fw->dead = 1;
            return -1;
        }
        memcpy(fw->mem + buf, flash, n);
        CALL(fw, "rfm1_flash_stage", vt->flash_stage(fw->inst, buf, n));
        CALL(fw, "rfm1_free", vt->free_(fw->inst, buf));
    } else
        CALL(fw, "rfm1_flash_stage", vt->flash_stage(fw->inst, 0, 0));
    memcpy(fw->mem + fw->scratch + S_STR, staged, sizeof staged);
    CALL(fw, "rfm1_options", vt->options(fw->inst, fw->scratch + S_STR, 0, 0, 1));
    CALL(fw, "rfm1_boot_options", vt->boot_options(fw->inst, (uint32_t)-1, 0, (uint32_t)-1));
    wr8(fw, P(PL_READY), 2);
    CALL(fw, "rfm1_init", vt->init(fw->inst, 0));
    fw->ns_total += now_ns() - t0;
    if (fw->dead)
        return -1;
    fw->booted = 1;
    fw->ms = 0;
    fw->frames_done = fw->due = 0;
    return 0;
}

fw_inst_t *fw_release(fw_t *fw)
{
    fw_inst_t *p = NULL;
    if (fw->inst) {
        if (fw->booted && !fw->dead)
            CALL(fw, "rfm1_shutdown", fw->obj->vt->shutdown(fw->inst));
        fw_log_flush(fw);
        if ((p = calloc(1, sizeof *p))) {
            p->obj = fw->obj, p->inst = fw->inst, p->wasi = fw->wasi;
            p->wasi->fw = NULL;   /* (no firmware calls from here) */
            fw->obj = NULL;
        } else {
            fw->obj->vt->destroy(fw->inst);
            fsvc_release(fw->wasi->fs);
            free(fw->wasi);
        }
    }
    fw->inst = NULL;
    fw->wasi = NULL;
    if (fw->obj)
        fw_obj_close(fw->obj);
    fw->obj = NULL;
    free(fw->log);
    fw->log = NULL;
    fw->booted = 0;
    fw->dead = 1;
    return p;
}

void fw_destroy(fw_t *fw)
{
    fw_inst_free(fw_release(fw));
}

/* ------------------------------------------------------------------ input --- */
void fw_log_start(fw_t *fw, uint32_t cap)
{
    free(fw->log);
    fw->log = calloc(cap, sizeof *fw->log);
    fw->log_cap = fw->log ? cap : 0;
    fw->log_n = 0;
}

static void apply(fw_t *fw, int op, int a, int b, uint32_t c)
{
    const rfm1m_t *vt = fw->obj->vt;
    if (fw->log && fw->log_n < fw->log_cap) {
        fw_log_t *l = &fw->log[fw->log_n++];
        l->ms = fw->ms, l->op = (uint8_t)op, l->a = (uint8_t)a, l->b = (int16_t)b, l->c = c;
    }
    switch (op) {
    case FWI_KEY:
        if (a < 0 || a >= FW_NKEY)
            break;
        if (b > 0) {   /* the velocity first; a key newly down also taps (Device::keys) */
            const uint32_t bit = 1u << a;
            wr8(fw, P(PL_KEY_VEL) + (uint32_t)a, (uint8_t)(b > 127 ? 127 : b));
            if (!(rd32(fw, P(PL_KEYS)) & bit))
                wr32(fw, P(PL_KEYS_TAP), rd32(fw, P(PL_KEYS_TAP)) | bit);
            wr32(fw, P(PL_KEYS), rd32(fw, P(PL_KEYS)) | bit);
            fw->keys_held |= bit;
        } else {
            wr32(fw, P(PL_KEYS), rd32(fw, P(PL_KEYS)) & ~(1u << a));
            wr8(fw, P(PL_KEY_LEAN) + (uint32_t)a, 0);
            fw->keys_held &= ~(1u << a);
        }
        break;
    case FWI_LEAN:
        if (a >= 0 && a < FW_NKEY)
            wr8(fw, P(PL_KEY_LEAN) + (uint32_t)a, (uint8_t)(b < 0 ? 0 : b > 100 ? 100 : b));
        break;
    case FWI_BTN:
        if (a >= 0 && a < FW_NBTN) {
            const uint32_t bit = 1u << a;
            if (b) {
                if (!(rd32(fw, P(PL_BUTTONS)) & bit))
                    wr32(fw, P(PL_BUTTONS_TAP), rd32(fw, P(PL_BUTTONS_TAP)) | bit);
                wr32(fw, P(PL_BUTTONS), rd32(fw, P(PL_BUTTONS)) | bit);
                fw->btns_held |= bit;
            } else {
                wr32(fw, P(PL_BUTTONS), rd32(fw, P(PL_BUTTONS)) & ~bit);
                fw->btns_held &= ~bit;
            }
        }
        break;
    case FWI_ENC:
        if (a >= 0 && a < FW_NENC) {
            const uint32_t at = P(PL_ENC) + 4u * (uint32_t)a;
            wr32(fw, at, (uint32_t)((int32_t)rd32(fw, at) + b));
        }
        break;
    case FWI_MASTER:
        wr32(fw, P(PL_MASTER), (uint32_t)(b < 0 ? 0 : b > 1023 ? 1023 : b));
        break;
    case FWI_MIDI: {
        volatile uint32_t ok = 1;
        CALL(fw, "rfm1_midi_in", ok = vt->midi_in(fw->inst, c));
        (void)ok;   /* (0: no room; the message is dropped, as a full USB FIFO would) */
        break;
    }
    case FWI_SYNC:
        CALL(fw, "rfm1_flash_sync", vt->flash_sync(fw->inst));
        break;
    case FWI_RELEASE: {
        int k;
        wr32(fw, P(PL_KEYS), 0);
        wr32(fw, P(PL_BUTTONS), 0);
        for (k = 0; k < FW_NKEY; k++)
            wr8(fw, P(PL_KEY_LEAN) + (uint32_t)k, 0);
        fw->keys_held = fw->btns_held = 0;
        break;
    }
    case FWI_TAKE: {   /* (the replay: what the host took) */
        volatile int k;
        for (k = 0; k < a && !fw->dead; k++)
            CALL(fw, "rfm1_midi_out_take", vt->midi_out_take(fw->inst, fw->scratch + S_WORD));
        break;
    }
    default:
        break;
    }
}

void fw_in_now(fw_t *fw, int op, int a, int b, uint32_t c)
{
    if (fw->inst && !fw->dead)
        apply(fw, op, a, b, c);
}

int fw_midi_out(fw_t *fw, uint32_t *pkts, int max)
{
    int n = 0;
    while (n < max && fw->booted && !fw->dead) {
        volatile uint32_t got = 0;
        CALL(fw, "rfm1_midi_out_take", got = fw->obj->vt->midi_out_take(fw->inst, fw->scratch + S_WORD));
        if (!got || fw->dead)
            break;
        pkts[n++] = rd32(fw, fw->scratch + S_WORD);
    }
    if (n && fw->log && fw->log_n < fw->log_cap) {
        fw_log_t *l = &fw->log[fw->log_n++];
        l->ms = fw->ms, l->op = FWI_TAKE, l->a = (uint8_t)n, l->b = 0, l->c = 0;
    }
    return n;
}

void fw_panel_ids(const fw_t *fw, uint8_t btn_id[FW_NBTN], uint8_t enc_id[FW_NENC], int8_t enc_dir[FW_NENC])
{
    int i;
    for (i = 0; i < FW_NBTN; i++)
        btn_id[i] = rd8(fw, P(PL_BTN_ID) + (uint32_t)i);
    for (i = 0; i < FW_NENC; i++) {
        enc_id[i] = rd8(fw, P(PL_ENC_ID) + (uint32_t)i);
        enc_dir[i] = (int8_t)rd8(fw, P(PL_ENC_DIR) + (uint32_t)i);
    }
}

void fw_in(fw_t *fw, int op, int a, int b, uint32_t c, uint64_t due_frame)
{
    if (!fw->inst || fw->dead)
        return;
    if (fw->qn == (int)(sizeof fw->q / sizeof fw->q[0])) {   /* full: the oldest goes in now */
        apply(fw, fw->q[0].op, fw->q[0].a, fw->q[0].b, fw->q[0].c);
        memmove(fw->q, fw->q + 1, sizeof fw->q[0] * (size_t)--fw->qn);
    }
    fw->q[fw->qn].due = due_frame;
    fw->q[fw->qn].op = (uint8_t)op;
    fw->q[fw->qn].a = (uint8_t)a;
    fw->q[fw->qn].b = (int16_t)b;
    fw->q[fw->qn].c = c;
    fw->qn++;
}

int fw_midi(fw_t *fw, const uint8_t *m, int n, uint64_t due_frame)
{
    const uint8_t st = n > 0 ? m[0] : 0;
    int len;
    if (st < 0x80 || st == 0xF0 || st == 0xF7)
        return -1;
    if (st >= 0xF8)   /* real time: CIN 0xF, one byte */
        len = 1;
    else if (st < 0xF0)
        len = (st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0 ? 2 : 3;
    else if (st == 0xF1 || st == 0xF3)
        len = 2;
    else if (st == 0xF2)
        len = 3;
    else
        len = 1;
    if (n < len)
        return -1;
    {
        const uint32_t cin = st >= 0xF8 ? 0xF : st < 0xF0 ? (uint32_t)st >> 4 : len == 3 ? 3 : len == 2 ? 2 : 5;
        const uint32_t pkt = cin | (uint32_t)m[0] << 8 | (uint32_t)(len > 1 ? m[1] : 0) << 16 |
                             (uint32_t)(len > 2 ? m[2] : 0) << 24;
        fw_in(fw, FWI_MIDI, 0, 0, pkt, due_frame);
    }
    return 0;
}

/* ------------------------------------------------------------------ the clock --- */
static void run_ms(fw_t *fw)
{
    const rfm1m_t *vt = fw->obj->vt;
    const uint32_t ms = fw->ms;
    const uint64_t due = (uint64_t)ms * FW_FS / 1000u;
    int i, k = 0;
    for (i = 0; i < fw->qn; i++) {   /* the input due by this ms, in order */
        if (fw->q[i].due <= due)
            apply(fw, fw->q[i].op, fw->q[i].a, fw->q[i].b, fw->q[i].c);
        else
            fw->q[k++] = fw->q[i];
    }
    fw->qn = k;
    if (fw->wasi->fs)   /* (between two calls: the file requests answered so far, into the module's memory) */
        fsvc_pump(fw->wasi->fs, fw->mem, fw->mem_size, ms);
    CALL(fw, "rfm1_tick", vt->tick(fw->inst, ms));
    if (ms == 0 || ms - fw->last_frame >= FW_FRAME_MS) {
        fw->last_frame = ms;
        CALL(fw, "rfm1_frame", vt->frame(fw->inst));
    } else
        CALL(fw, "rfm1_idle", vt->idle(fw->inst));
    if (ms % 50u == 0 && !fw->dead) {   /* (as rfm1_run: a firmware that rebooted or exited) */
        volatile uint32_t h = 0;
        CALL(fw, "rfm1_halted", h = vt->halted(fw->inst));
        if (h) {
            fw->halted = h;
            fw->dead = 1;
            snprintf(fw->err, sizeof fw->err, "%s halted (0x%x)", fw->id, (unsigned)h);
        }
    }
    fw->due = due;
    fw->ms++;
}

int fw_render(fw_t *fw, int16_t out[FW_BLOCK * 2])
{
    const uint64_t t0 = now_ns();
    uint64_t dt;
    if (!fw->booted || fw->dead) {
        memset(out, 0, FW_BLOCK * 4);
        return -1;
    }
    while (fw->frames_done + FW_BLOCK > fw->due && !fw->dead)
        run_ms(fw);
    if (!fw->dead)
        CALL(fw, "rfm1_audio", fw->obj->vt->audio(fw->inst, fw->scratch + S_AUDIO, FW_BLOCK));
    if (fw->dead)
        memset(out, 0, FW_BLOCK * 4);
    else
        memcpy(out, fw->mem + fw->scratch + S_AUDIO, FW_BLOCK * 4);
    fw->frames_done += FW_BLOCK;
    dt = now_ns() - t0;
    fw->ns_total += dt;
    if (dt / 1000u > fw->worst_us)
        fw->worst_us = (uint32_t)(dt / 1000u);
    return fw->dead ? -1 : 0;
}

/* ------------------------------------------------------------------ what the panel shows --- */
const uint8_t *fw_leds(const fw_t *fw) { return fw->mem + P(PL_LED); }
uint8_t fw_play_green(const fw_t *fw) { return rd8(fw, P(PL_PLAY_GREEN)); }
const uint8_t *fw_lcd(const fw_t *fw) { return fw->mem + P(PL_LCD); }
uint32_t fw_lcd_writes(const fw_t *fw) { return rd32(fw, P(PL_LCD_WRITES)); }
int32_t fw_master(const fw_t *fw) { return (int32_t)rd32(fw, P(PL_MASTER)); }

const uint8_t *fw_flash(fw_t *fw)
{
    volatile uint32_t p = 0;
    if (!fw->booted || (fw->dead && !fw->halted) || !fw->flash_size)
        return NULL;   /* (a halted firmware's flash is still read: what it saved before it rebooted) */
    CALL(fw, "rfm1_flash", p = fw->obj->vt->flash(fw->inst));
    return p && in_mem(fw, p, fw->flash_size) ? fw->mem + p : NULL;
}

uint32_t fw_flash_dirty(fw_t *fw)
{
    volatile uint32_t d = 0;
    if (!fw->booted || fw->dead)
        return 0;
    CALL(fw, "rfm1_flash_dirty", d = fw->obj->vt->flash_dirty(fw->inst));
    return d;
}

uint32_t fw_labels_epoch(fw_t *fw)
{
    volatile uint32_t e = 0;
    if (!fw->booted || fw->dead || !fw->obj->vt->labels_epoch)
        return 0;
    CALL(fw, "rfm1_labels_epoch", e = fw->obj->vt->labels_epoch(fw->inst));
    return e;
}

const char *fw_label(fw_t *fw, int which)
{
    static char buf[16];
    volatile uint32_t p = 0;
    if (!fw->booted || fw->dead || !fw->obj->vt->label)
        return NULL;
    CALL(fw, "rfm1_label", p = fw->obj->vt->label(fw->inst, (uint32_t)which));
    if (fw->dead || !p)
        return NULL;
    rdstr(fw, p, buf, 11);
    return buf;
}

void fw_ui_info(fw_t *fw, char *buf_, uint32_t n_)
{
    volatile uint32_t n = n_;
    char *volatile buf = buf_;
    if (n)
        buf[0] = 0;
    if (!fw->booted || fw->dead || n == 0)
        return;
    if (n > 1024)
        n = 1024;
    memset(fw->mem + fw->scratch + S_STR, 0, n);
    CALL(fw, "rfm1_ui_info", fw->obj->vt->ui_info(fw->inst, fw->scratch + S_STR, n));
    rdstr(fw, fw->scratch + S_STR, buf, n);
}
