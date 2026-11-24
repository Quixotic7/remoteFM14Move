/* SPDX-License-Identifier: MPL-2.0 */
/* fwrt: see fwrt.h. The WASI imports of RFM1-FORMAT.md section 3 are defined here, once for every firmware object:
 * the objects resolve them (and wasm-rt-impl.c's runtime) against this executable at dlopen. */
#define _GNU_SOURCE
#include "fwrt.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#endif

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
static struct fw_lg *lg_for(fw_obj_t *o);
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
            else if (!strcmp(need, "ain1"))
                o->ain1 = 1;
            else {
                snprintf(err, cap, "needs \"%s\", which this host does not give yet: a newer RemoteFM1", need);
                free(j);
                return -1;
            }
        }
    }
    if ((it = mj_find(m, "wants")) && *it == '[') {   /* (unknown names in wants are ignored) */
        const char *e;
        while ((e = mj_elem(&it)))
            if (*e == '"' && mj_span(e) == 6 && !strncmp(e + 1, "ain1", 4))
                o->ain1 = 1;
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
    {   /* (through a link of its own: a rebuild installed over a path an older object still runs from (the same
         * <id>-<version>) is another file, which dlopen would not load under the name it already has loaded) */
        char alias[PATH_MAX];
        struct stat st;
        if (stat(path, &st) == 0 &&
            snprintf(alias, sizeof alias, "%s.ld%llx", path, (unsigned long long)st.st_ino) < (int)sizeof alias &&
            (link(path, alias) == 0 || errno == EEXIST)) {
            o->so = dlopen(alias, RTLD_NOW | RTLD_LOCAL);
            unlink(alias);
        } else
            o->so = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }
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
    {   /* rd1: rfm1m_rd1 (an object built with RFM1M_RD1: its module exports rfm1_readout) */
        const rfm1m_rd1_fn rg = (rfm1m_rd1_fn)(uintptr_t)dlsym(o->so, RFM1M_RD1_SYMBOL);
        const rfm1m_rd1_t *r = rg ? rg() : NULL;
        o->rd1 = r && r->abi == RFM1M_RD1_ABI && r->readout ? r : NULL;
    }
    {   /* tk1: rfm1m_tk1 (an object built with RFM1M_TK1: its module exports rfm1_tk, rfm1_tk_input, rfm1_tk_state) */
        const rfm1m_tk1_fn tg = (rfm1m_tk1_fn)(uintptr_t)dlsym(o->so, RFM1M_TK1_SYMBOL);
        const rfm1m_tk1_t *t = tg ? tg() : NULL;
        o->tk1 = t && t->abi == RFM1M_TK1_ABI && t->tk && t->input && t->state ? t : NULL;
    }
    {   /* ep1: rfm1m_ep1 (an object built with RFM1M_EP1: its module exports rfm1_ep, rfm1_ep_set, rfm1_ep_watch) */
        const rfm1m_ep1_fn eg = (rfm1m_ep1_fn)(uintptr_t)dlsym(o->so, RFM1M_EP1_SYMBOL);
        const rfm1m_ep1_t *e = eg ? eg() : NULL;
        o->ep1 = e && e->abi == RFM1M_EP1_ABI && e->ep && e->set && e->watch ? e : NULL;
    }
    {   /* ain1: rfm1m_ain (an object built with RFM1M_AIN1: its module exports rfm1_audio_in) */
        const rfm1m_ain_fn ag = (rfm1m_ain_fn)(uintptr_t)dlsym(o->so, RFM1M_AIN_SYMBOL);
        const rfm1m_ain_t *a = ag ? ag() : NULL;
        o->ain = a && a->abi == RFM1M_AIN_ABI && a->audio_in ? a : NULL;
    }
    {   /* threads: a thread-local call depth (else every call under the legacy lock); core 1 */
        const rfm1m_caps_fn cf = (rfm1m_caps_fn)(uintptr_t)dlsym(o->so, RFM1M_CAPS_SYMBOL);
        const rfm1m_cpu2_fn c2 = (rfm1m_cpu2_fn)(uintptr_t)dlsym(o->so, RFM1M_CPU2_SYMBOL);
        const rfm1m_cpu2_t *c = c2 ? c2() : NULL;
        o->mt = cf && (cf() & RFM1M_CAP_MT);
        o->cpu2 = o->mt && c && (c->abi == 1 || c->abi == RFM1M_CPU2_ABI) && c->twin && c->twin_free && c->stack &&
                  c->call ? c : NULL;   /* (ABI 1: an importing module's object, without hook) */
    }
    snprintf(o->path, sizeof o->path, "%s", path);
    if (!o->mt)
        o->lg = lg_for(o);   /* (an old object: its own call depth, if it can be had) */
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

/* An object built before the threads (fw_obj_t.mt 0: no rfm1m_caps) counts its call depth in wasm2c's process global
 * wasm_rt_call_stack_depth, ++ and -- on every call: two threads in such objects would corrupt it. Two ways out:
 *   - its own counter (Linux): the object's GOT entry for that symbol (its R_AARCH64_GLOB_DAT relocation, found in its
 *     dynamic section by name, checked to hold the global's address) is pointed at a counter of that loaded object's
 *     own (fw_obj_t.lg: one per loaded file, shared by every fw_obj_t of it). Its instances are serialised by that
 *     object's lock only, so different old objects run on different threads at once, as new ones do; the same object
 *     in two slots takes turns. A trap restores the counter from the value saved before the call (the runtime's
 *     restore is for the global);
 *   - otherwise (no such relocation, another libc): every call into it under one process-wide lock, the global counter
 *     restored on a trap only on the thread holding it (tools/w2c_mt.sh: rfm14_w2c_legacy_held). */
typedef struct fw_lg {
    const void *lm;                   /* the loaded object (its link_map) */
    uint32_t depth;                   /* its call depth (the GOT points here) */
    pthread_mutex_t mu;
} fw_lg_t;
#define LG_MAX 64
static fw_lg_t *g_lg[LG_MAX];
static pthread_mutex_t g_lg_mu = PTHREAD_MUTEX_INITIALIZER;
#if defined(__linux__) && defined(__aarch64__)
#include <link.h>
#include <sys/mman.h>
struct relro_q { ElfW(Addr) base; uintptr_t lo, hi; };
static int relro_cb(struct dl_phdr_info *info, size_t size, void *arg)
{
    struct relro_q *q = arg;
    int i;
    (void)size;
    if (info->dlpi_addr != q->base)
        return 0;
    for (i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_GNU_RELRO) {
            q->lo = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
            q->hi = q->lo + info->dlpi_phdr[i].p_memsz;
        }
    return 1;
}
/* the object's GOT slots for sym (holding old) pointed at neu: how many (0: none, -1: a slot held something else) */
static int got_redirect(void *so, const char *sym, void *old, void *neu)
{
    struct link_map *lm = NULL;
    ElfW(Dyn) *d;
    const ElfW(Rela) *rela = NULL;
    const ElfW(Sym) *symtab = NULL;
    const char *strtab = NULL;
    size_t relasz = 0, i;
    int n = 0;
    struct relro_q q;
    const long pg = sysconf(_SC_PAGESIZE);
    if (dlinfo(so, RTLD_DI_LINKMAP, &lm) != 0 || !lm || !lm->l_ld)
        return 0;
#define LGPTR(v) ((uintptr_t)(v) < (uintptr_t)lm->l_addr ? (uintptr_t)(v) + lm->l_addr : (uintptr_t)(v))
    for (d = lm->l_ld; d->d_tag != DT_NULL; d++)
        switch (d->d_tag) {
        case DT_RELA: rela = (const ElfW(Rela) *)LGPTR(d->d_un.d_ptr); break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_SYMTAB: symtab = (const ElfW(Sym) *)LGPTR(d->d_un.d_ptr); break;
        case DT_STRTAB: strtab = (const char *)LGPTR(d->d_un.d_ptr); break;
        default: break;
        }
#undef LGPTR
    if (!rela || !symtab || !strtab)
        return 0;
    q.base = lm->l_addr, q.lo = q.hi = 0;
    dl_iterate_phdr(relro_cb, &q);
    for (i = 0; i < relasz / sizeof *rela; i++) {   /* (first: every use of sym is a GOT slot that holds old) */
        const ElfW(Rela) *r = &rela[i];
        if (ELF64_R_SYM(r->r_info) && !strcmp(strtab + symtab[ELF64_R_SYM(r->r_info)].st_name, sym)
            && (ELF64_R_TYPE(r->r_info) != R_AARCH64_GLOB_DAT || *(void **)(lm->l_addr + r->r_offset) != old))
            return -1;
    }
    for (i = 0; i < relasz / sizeof *rela; i++) {
        const ElfW(Rela) *r = &rela[i];
        void **slot;
        uintptr_t page;
        if (ELF64_R_TYPE(r->r_info) != R_AARCH64_GLOB_DAT || strcmp(strtab + symtab[ELF64_R_SYM(r->r_info)].st_name, sym))
            continue;
        slot = (void **)(lm->l_addr + r->r_offset);
        page = (uintptr_t)slot & ~(uintptr_t)(pg - 1);
        if (mprotect((void *)page, (size_t)pg, PROT_READ | PROT_WRITE) != 0)
            return -1;
        *slot = neu;
        if ((uintptr_t)slot >= q.lo && (uintptr_t)slot < q.hi)   /* (RELRO: read-only again) */
            mprotect((void *)page, (size_t)pg, PROT_READ);
        n++;
    }
    return n;
}
#endif
/* o's own counter and lock (an old object), or NULL: the global lock then */
static fw_lg_t *lg_for(fw_obj_t *o)
{
#if defined(__linux__) && defined(__aarch64__)
    struct link_map *lm = NULL;
    fw_lg_t *lg = NULL;
    int i, free_i = -1;
    if (getenv("RFM14_LEGACY_LOCK") || dlinfo(o->so, RTLD_DI_LINKMAP, &lm) != 0 || !lm)
        return NULL;
    pthread_mutex_lock(&g_lg_mu);
    for (i = 0; i < LG_MAX && !lg; i++)
        if (g_lg[i] && g_lg[i]->lm == lm)
            lg = g_lg[i];
        else if (!g_lg[i] && free_i < 0)
            free_i = i;
    if (!lg && free_i >= 0 && (lg = calloc(1, sizeof *lg))) {
        lg->lm = lm;
        pthread_mutex_init(&lg->mu, NULL);
        if (got_redirect(o->so, "wasm_rt_call_stack_depth", &wasm_rt_call_stack_depth, &lg->depth) > 0)
            g_lg[free_i] = lg;   /* (kept while the process runs: the object stays mapped, fw_obj_close) */
        else {
            fprintf(stderr, "fwrt: %s: an object from before the threads, its call depth not redirected: every call "
                            "under the one lock\n", o->path);
            pthread_mutex_destroy(&lg->mu);
            free(lg);
            lg = NULL;
        }
    }
    pthread_mutex_unlock(&g_lg_mu);
    return lg;
#else
    (void)o;
    return NULL;
#endif
}

static pthread_mutex_t g_legacy = PTHREAD_MUTEX_INITIALIZER;
/* before a call: 0 (a new object), 1 (the object's own lock, *saved its depth), 2 (the global lock) */
static int legacy_enter(const fw_t *fw, uint32_t *saved)
{
    const fw_obj_t *o = fw->obj;
    if (!o || o->mt)
        return 0;
    if (o->lg) {
        pthread_mutex_lock(&o->lg->mu);
        *saved = o->lg->depth;
        return 1;
    }
    pthread_mutex_lock(&g_legacy);
    rfm14_w2c_legacy_held = 1;
    return 2;
}
static void legacy_leave(const fw_t *fw, int held, int trapped_, uint32_t saved)
{
    if (held == 1) {
        if (trapped_)
            fw->obj->lg->depth = saved;   /* (the runtime restored the global's, not this) */
        pthread_mutex_unlock(&fw->obj->lg->mu);
    } else if (held == 2) {
        rfm14_w2c_legacy_held = 0;
        pthread_mutex_unlock(&g_legacy);
    }
}

/* call: an expression on fw->obj->vt and fw->inst; on a trap the instance is dead and the expression's value is 0.
 * The jump buffer is this thread's (tools/w2c_mt.sh) */
#define CALL(fw, what, expr)                                                                                        \
    do {                                                                                                            \
        uint32_t saved_ = 0;                                                                                        \
        const volatile int held_ = legacy_enter(fw, &saved_);                                                       \
        const volatile uint32_t sv_ = saved_;                                                                       \
        wasm_rt_trap_t code_ = (wasm_rt_trap_t)wasm_rt_impl_try();                                                  \
        if (code_ == WASM_RT_TRAP_NONE) {                                                                           \
            expr;                                                                                                   \
        } else                                                                                                      \
            trapped((fw), code_, (what));                                                                           \
        legacy_leave((fw), held_, code_ != WASM_RT_TRAP_NONE, sv_);                                                 \
    } while (0)


/* ------------------------------------------------------------------ core 1 (cpu2) --- */
/* README "Threads". The module asks for core 1 inside its own calls (rfm1_audio, typically once per control block):
 *   rfm1_cpu2_1.run(fn, arg): 1 handed over (core 1 runs fn(arg) now), 0 not (the module runs it itself);
 *   rfm1_cpu2_1.wait(): 0 the job is done; else it was not (the module runs it itself).
 * The job runs on the core-1 thread, on the twin: the same module instantiated again (its own globals, so its own
 * stack pointer, set into a region rfm1_alloc'd from the module: rfm1_c1_stack; its own table) with the module's
 * memory. Between run and wait the module's core 0 must leave the job's data alone, as on the FM-1. A trap in a job:
 * the job is reported not done (wait answers -2), and core 1 is off for this instance from then on.
 * The hand-over: sequence numbers, a short spin on each side, then a futex. One job at a time. */
#define C1_STACK (256u * 1024u)        /* the twin's stack, in the module's memory */
#define C1_IDLE_SPIN_NS 200000u         /* core 1 spins this long for the next job before it sleeps */
#define C1_WAIT_SPIN_NS 100000u         /* core 0 spins this long in wait before it sleeps */

struct fw_c1 {
    fw_t *fw;
    const rfm1m_cpu2_t *vt;
    void *twin;
    struct Z_wasi_snapshot_preview1_instance_t *twin_wasi;
    pthread_t thr;
    int started;
    uint32_t stack;                    /* (rfm1_alloc'd) */
    /* the hand-over: post (core 0) and done (core 1) move by one per job; quit ends the thread */
    uint32_t post, done, quit;
    uint32_t fn, arg;
    int result;                        /* the last job: 0 ran, -2 trapped */
    int state;                         /* fw_c1_stats_t.state */
    int pending;                       /* (core 0's) a job handed over and not waited for yet */
    int no_hook;                       /* the module's rfm1_cpu2 record was not usable: never started */
    int tid;
    uint64_t jobs, inline_jobs, job_ns, wait_ns;
    uint32_t job_worst_us;
};

void (*fw_on_c1_thread)(fw_t *fw);
static int g_cpu2_off = -1;
void fw_cpu2_enable(int on) { g_cpu2_off = !on; }
static int cpu2_on(void)
{
    if (g_cpu2_off < 0) {
        const char *e = getenv("RFM14_CPU2");
        g_cpu2_off = e && *e == '0';
    }
    return !g_cpu2_off;
}

static void fu_wait(uint32_t *addr, uint32_t val)
{
#ifdef __linux__
    syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, NULL, NULL, 0);
#else
    (void)addr, (void)val;
    sched_yield();
#endif
}
static void fu_wake(uint32_t *addr)
{
#ifdef __linux__
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
#else
    (void)addr;
#endif
}

/* memories a twin shares never move: wasm_rt_grow_memory (the objects call it for memory.grow; the runtime's own is
 * w2c_rt_grow_memory, Makefile) refuses to grow them (.rfm1 memories are fixed in size anyway: RFM1.md MEMORY_MIB) */
uint32_t w2c_rt_grow_memory(wasm_rt_memory_t *memory, uint32_t delta);
#define SHARED_MAX 16
static pthread_mutex_t g_shared_mu = PTHREAD_MUTEX_INITIALIZER;
static const void *g_shared[SHARED_MAX];
static int shared_set(const void *data, int on)
{
    int i, ok = 0;
    pthread_mutex_lock(&g_shared_mu);
    for (i = 0; i < SHARED_MAX && !ok; i++)
        if (on ? !g_shared[i] : g_shared[i] == data) {
            g_shared[i] = on ? data : NULL;
            ok = 1;
        }
    pthread_mutex_unlock(&g_shared_mu);
    return ok ? 0 : -1;
}
__attribute__((visibility("default"))) uint32_t wasm_rt_grow_memory(wasm_rt_memory_t *memory, uint32_t delta);
uint32_t wasm_rt_grow_memory(wasm_rt_memory_t *memory, uint32_t delta)
{
    int i, shared = 0;
    if (delta) {
        pthread_mutex_lock(&g_shared_mu);
        for (i = 0; i < SHARED_MAX; i++)
            shared |= g_shared[i] && g_shared[i] == memory->data;
        pthread_mutex_unlock(&g_shared_mu);
        if (shared)
            return (uint32_t)-1;
    }
    return w2c_rt_grow_memory(memory, delta);
}

static void *c1_main(void *arg)
{
    struct fw_c1 *c = arg;
    uint32_t seen = 0;
#ifdef __linux__
    c->tid = (int)syscall(SYS_gettid);
#endif
    if (fw_on_c1_thread)
        fw_on_c1_thread(c->fw);
    for (;;) {
        uint32_t p;
        const uint64_t t0 = now_ns();
        while ((p = __atomic_load_n(&c->post, __ATOMIC_ACQUIRE)) == seen && !__atomic_load_n(&c->quit, __ATOMIC_ACQUIRE)) {
            if (now_ns() - t0 > C1_IDLE_SPIN_NS)
                fu_wait(&c->post, seen);
        }
        if (__atomic_load_n(&c->quit, __ATOMIC_ACQUIRE) && p == seen)
            break;
        seen = p;
        {
            const uint64_t t1 = now_ns();
            uint64_t dt;
            wasm_rt_trap_t code = (wasm_rt_trap_t)wasm_rt_impl_try();
            if (code == WASM_RT_TRAP_NONE) {
                c->vt->call(c->twin, c->fn, c->arg);
                c->result = 0;
            } else {
                c->result = -2;
                fprintf(stderr, "fwrt: %s: core 1's job trapped (%s): core 1 off\n", c->fw->id, wasm_rt_strerror(code));
            }
            dt = now_ns() - t1;
            c->job_ns += dt;
            if (dt / 1000u > c->job_worst_us)
                c->job_worst_us = (uint32_t)(dt / 1000u);
            c->jobs++;
        }
        __atomic_store_n(&c->done, seen, __ATOMIC_RELEASE);
        fu_wake(&c->done);
    }
    return NULL;
}

uint32_t Z_rfm1_cpu2_1Z_run(struct Z_rfm1_cpu2_1_instance_t *h, uint32_t fn, uint32_t arg)
{
    fw_t *fw = h ? h->w.fw : NULL;
    struct fw_c1 *c = fw ? fw->c1 : NULL;
    if (!fw)
        return 0;   /* (the twin: a job asking for core 1 runs its part itself) */
    if (!c || c->state != 1 || c->pending) {
        if (c)
            c->inline_jobs++;
        return 0;
    }
    c->fn = fn, c->arg = arg;
    c->pending = 1;
    __atomic_store_n(&c->post, c->post + 1, __ATOMIC_RELEASE);
    fu_wake(&c->post);
    return 1;
}

static void c1_join(struct fw_c1 *c)   /* core 0: the job handed over is done */
{
    const uint32_t want = c->post;
    uint32_t d;
    const uint64_t t0 = now_ns();
    while ((d = __atomic_load_n(&c->done, __ATOMIC_ACQUIRE)) != want)
        if (now_ns() - t0 > C1_WAIT_SPIN_NS)
            fu_wait(&c->done, d);
    c->wait_ns += now_ns() - t0;
    c->pending = 0;
}

uint32_t Z_rfm1_cpu2_1Z_wait(struct Z_rfm1_cpu2_1_instance_t *h)
{
    fw_t *fw = h ? h->w.fw : NULL;
    struct fw_c1 *c = fw ? fw->c1 : NULL;
    if (!c || !c->pending)
        return 0;   /* (nothing handed over: nothing to wait for) */
    c1_join(c);
    if (c->result != 0) {
        c->state = 2;
        return (uint32_t)-2;
    }
    return 0;
}

/* core 1 for fw (fw->c1: the twin fw_instantiate made): its stack in the module's memory, its thread. 0, or -1 (off) */
static int c1_start(fw_t *fw)
{
    struct fw_c1 *c = fw->c1;
    void *const twin = c->twin;
    volatile uint32_t st = 0;
    pthread_attr_t at;
    CALL(fw, "rfm1_alloc", st = fw->obj->vt->alloc(fw->inst, C1_STACK));
    if (fw->dead || !st || !in_mem(fw, st, C1_STACK)) {
        fprintf(stderr, "fwrt: %s: no room for core 1's stack: core 1 off\n", fw->id);
        c->state = 2;
        return -1;
    }
    c->stack = st;
    {
        wasm_rt_trap_t code = (wasm_rt_trap_t)wasm_rt_impl_try();
        if (code == WASM_RT_TRAP_NONE)
            c->vt->stack(twin, (st + C1_STACK) & ~15u);
        else {
            fprintf(stderr, "fwrt: %s: rfm1_c1_stack trapped (%s): core 1 off\n", fw->id, wasm_rt_strerror(code));
            c->state = 2;
            return -1;
        }
    }
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 2u << 20);   /* (wasm2c functions keep their locals in C frames) */
    if (pthread_create(&c->thr, &at, c1_main, c) != 0) {
        pthread_attr_destroy(&at);
        c->state = 2;
        return -1;
    }
    pthread_attr_destroy(&at);
    c->started = 1;
    c->state = 1;
    return 0;
}

/* the thread stopped (a job in flight finished first), the twin handed back to be freed with the instance */
static void c1_stop(fw_t *fw, void **twin, struct Z_wasi_snapshot_preview1_instance_t **tw)
{
    struct fw_c1 *c = fw->c1;
    if (!c)
        return;
    if (c->started) {
        if (c->pending)
            c1_join(c);
        __atomic_store_n(&c->quit, 1, __ATOMIC_RELEASE);
        fu_wake(&c->post);
        pthread_join(c->thr, NULL);
    }
    *twin = c->twin, *tw = c->twin_wasi;
    fw->c1 = NULL;
    free(c);
}

void fw_c1_stats(const fw_t *fw, fw_c1_stats_t *st)
{
    const struct fw_c1 *c = fw ? fw->c1 : NULL;
    memset(st, 0, sizeof *st);
    if (!c)
        return;
    st->state = c->state;
    st->jobs = c->jobs, st->inline_jobs = c->inline_jobs;
    st->job_ns = c->job_ns, st->wait_ns = c->wait_ns;
    st->job_worst_us = c->job_worst_us;
    st->tid = c->tid;
}

/* ------------------------------------------------------------------ an instance --- */
static __thread char g_why[160];
const char *fw_inst_why(void) { return g_why[0] ? g_why : "no memory for an instance"; }

long fw_mem_free_mb(void)
{
    long kb = -1;
    FILE *f;
    char line[128];
#ifdef RFM14_TEST_BUILD
    {   /* (the tests: a Move with less memory) */
        const char *t = getenv("RFM14_MEM_AVAIL_MB");
        if (t && *t)
            return atol(t) - FW_MEM_RESERVE_MB;
    }
#endif
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
    if (obj->cpu2 && cpu2_on() && (p->twin_wasi = calloc(1, sizeof *p->twin_wasi))) {   /* core 1's twin */
        uint64_t sz;
        const void *data = obj->vt->memory(p->inst, &sz);
        if (shared_set(data, 1) == 0 && !(p->twin = obj->cpu2->twin(p->inst, p->twin_wasi)))
            shared_set(data, 0);
        if (!p->twin) {
            free(p->twin_wasi);
            p->twin_wasi = NULL;
        }
    }
    return p;
}

void fw_inst_free(fw_inst_t *p)
{
    if (!p)
        return;
    if (p->twin) {   /* (first: the memory is the instance's) */
        uint64_t sz;
        p->obj->cpu2->twin_free(p->twin);
        shared_set(p->obj->vt->memory(p->inst, &sz), 0);
    }
    free(p->twin_wasi);
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
    if (pre->twin && (fw->c1 = calloc(1, sizeof *fw->c1))) {   /* (core 1: started once the module is up) */
        fw->c1->fw = fw, fw->c1->vt = pre->obj->cpu2, fw->c1->twin = pre->twin, fw->c1->twin_wasi = pre->twin_wasi;
        pre->twin = NULL, pre->twin_wasi = NULL;
    }
    if (pre->twin) {   /* (no memory for its record: the twin goes, the module runs its jobs itself) */
        uint64_t sz;
        pre->obj->cpu2->twin_free(pre->twin);
        shared_set(pre->obj->vt->memory(pre->inst, &sz), 0);
        free(pre->twin_wasi);
    }
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
    if (fw->c1 && fw->c1->vt->abi >= 2 && fw->c1->vt->hook) {   /* (FM1VST's shim: run / wait into its table) */
        volatile int h = -1;
        CALL(fw, "rfm1_cpu2", h = fw->c1->vt->hook(fw->inst, fw->wasi));
        if (fw->dead)
            return -1;
        if (h != 0) {   /* (a record not as FM1VST's docs/RFM1.md says: the module keeps its one-core answers) */
            fprintf(stderr, "fwrt: %s: rfm1_cpu2's record is not usable: core 1 off\n", fw->id);
            fw->c1->state = 0;
            fw->c1->no_hook = 1;
        }
    }
    if (fw->c1 && !fw->c1->no_hook)
        c1_start(fw);   /* (failing: core 1 off, the module runs its jobs itself) */
    return 0;
}

/* tk1: the record's place, once after rfm1_init (it never moves); refused (no takeover) unless version 1, size at
 * least FW_TK_SIZE, in memory, its name NUL-terminated printable ASCII */
static void tk_find(fw_t *fw)
{
    volatile uint32_t p = 0;
    const uint8_t *r;
    uint16_t ver, size;
    int i;
    fw->tk_ptr = 0;
    if (!fw->obj->tk1 || fw->dead)
        return;
    CALL(fw, "rfm1_tk", p = fw->obj->tk1->tk(fw->inst));
    if (fw->dead || !p || !in_mem(fw, p, FW_TK_SIZE))
        return;
    r = fw->mem + p;
    memcpy(&ver, r, 2);
    memcpy(&size, r + 2, 2);
    if (ver != 1 || size < FW_TK_SIZE) {
        fprintf(stderr, "fwrt: %s: tk1 record version %u size %u refused\n", fw->id, ver, size);
        return;
    }
    for (i = 0; i < 16 && r[FW_TK_NAME + i]; i++)
        if (r[FW_TK_NAME + i] < 32 || r[FW_TK_NAME + i] > 126)
            break;
    if (i == 16 || r[FW_TK_NAME + i]) {
        fprintf(stderr, "fwrt: %s: tk1 record's name is not printable ASCII: refused\n", fw->id);
        return;
    }
    fw->tk_ptr = p;
}

/* ep1: the record's place, once after rfm1_init (it never moves); refused (no engine page) unless version 1, size at
 * least FW_EP_SIZE, in memory, nlayers 1..8, its description inside the memory and at most 64 KiB */
static void ep_find(fw_t *fw)
{
    volatile uint32_t p = 0;
    const uint8_t *r;
    uint16_t ver, size;
    uint32_t dp, dl;
    fw->ep_ptr = 0;
    if (!fw->obj->ep1 || fw->dead)
        return;
    CALL(fw, "rfm1_ep", p = fw->obj->ep1->ep(fw->inst));
    if (fw->dead || !p || !in_mem(fw, p, FW_EP_SIZE))
        return;
    r = fw->mem + p;
    memcpy(&ver, r, 2);
    memcpy(&size, r + 2, 2);
    memcpy(&dp, r + FW_EP_DESC_PTR, 4);
    memcpy(&dl, r + FW_EP_DESC_LEN, 4);
    if (ver != 1 || size < FW_EP_SIZE || r[FW_EP_NLAYERS] < 1 || r[FW_EP_NLAYERS] > 8 || !dl || dl > 65536u ||
        !in_mem(fw, dp, dl)) {
        fprintf(stderr, "fwrt: %s: ep1 record version %u size %u layers %u description %u bytes refused\n", fw->id, ver,
                size, r[FW_EP_NLAYERS], dl);
        return;
    }
    fw->ep_ptr = p;
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
    tk_find(fw);
    ep_find(fw);
    return 0;
}

fw_inst_t *fw_release(fw_t *fw)
{
    fw_inst_t *p = NULL;
    void *tv = NULL;
    struct Z_wasi_snapshot_preview1_instance_t *twv = NULL;
    c1_stop(fw, &tv, &twv);   /* (before the module's last calls: a job in flight finished, the thread gone) */
    void *volatile twin = tv;
    struct Z_wasi_snapshot_preview1_instance_t *volatile tw = twv;
    if (fw->inst) {
        if (fw->booted && !fw->dead && fw->ain_state)   /* (the input off first: VIRTUAL-CARTS.md 8) */
            fw_ain_info(fw, 0, 0);
        if (fw->booted && !fw->dead)
            CALL(fw, "rfm1_shutdown", fw->obj->vt->shutdown(fw->inst));
        fw_log_flush(fw);
        if ((p = calloc(1, sizeof *p))) {
            p->obj = fw->obj, p->inst = fw->inst, p->wasi = fw->wasi;
            p->twin = twin, p->twin_wasi = tw;
            p->wasi->fw = NULL;   /* (no firmware calls from here) */
            fw->obj = NULL;
        } else {
            if (twin) {
                uint64_t sz;
                fw->obj->cpu2->twin_free(twin);
                shared_set(fw->obj->vt->memory(fw->inst, &sz), 0);
            }
            free(tw);
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
    free(fw->mq);
    fw->mq = NULL;
    fw->mq_head = fw->mq_tail = 0;
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
    if (op == FWI_KEY || op == FWI_LEAN || op == FWI_BTN || op == FWI_ENC || op == FWI_TK_INPUT || op == FWI_EP ||
        (op == FWI_MIDI && (c & 0xF) != 0xF))   /* (a player's input: not the clock, the master, the screen's rate) */
        fw->n_in++;
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
    case FWI_TK_INPUT:
        fw_tk_input(fw, c);
        break;
    case FWI_TK_STATE:
        fw_tk_state(fw, (uint32_t)a, (uint32_t)(uint16_t)b, c);
        break;
    case FWI_EP:       /* ep1: a parameter's value (engine-params/SPEC.md 5) */
        (void)fw_ep_set(fw, (uint32_t)a, c >> 16, c & 0xFFFFu, b);
        break;
    case FWI_EP_WATCH:
        fw_ep_watch(fw, a, b);
        break;
    case FWI_FRAMES:   /* the screen's rate (the next frame due a ms from the last one) */
        fw->frame_ms = a > 0 ? (uint32_t)a : FW_FRAME_MS;
        break;
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

int fw_midi_feed(fw_t *fw, const uint8_t *m, int n)
{
    uint32_t pk[64], np = 0, i;
    int k;
    if (!fw->inst || fw->dead || n <= 0 || m[0] < 0x80)
        return -1;
    if (!fw->mq && !(fw->mq = malloc(FW_MQ * sizeof *fw->mq)))
        return -1;
    if (m[0] == 0xF0) {   /* SysEx: CIN 4 for each 3 bytes, then 5 / 6 / 7 for the last 1 / 2 / 3 */
        uint32_t need = ((uint32_t)n + 2u) / 3u;
        if (m[n - 1] != 0xF7 || FW_MQ - (fw->mq_head - fw->mq_tail) < need)
            return -1;
        for (k = 0; k < n; k += 3) {
            const int left = n - k, take = left < 3 ? left : 3;
            const uint32_t cin = left > 3 ? 0x4 : take == 1 ? 0x5 : take == 2 ? 0x6 : 0x7;
            uint32_t p = cin;
            for (i = 0; i < (uint32_t)take; i++)
                p |= (uint32_t)m[k + (int)i] << (8 * (i + 1));
            fw->mq[fw->mq_head++ % FW_MQ] = p;
        }
        return 0;
    }
    {   /* one or more short messages (running status not taken) */
        const uint8_t st = m[0];
        const int len = st >= 0xF8 ? 1 : st < 0xF0 ? ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0 ? 2 : 3)
                        : st == 0xF1 || st == 0xF3 ? 2 : st == 0xF2 ? 3 : 1;
        uint32_t cin, p;
        if (n < len || st == 0xF7)
            return -1;
        cin = st >= 0xF8 ? 0xF : st < 0xF0 ? (uint32_t)st >> 4 : len == 3 ? 3 : len == 2 ? 2 : 5;
        p = cin;
        for (i = 0; i < (uint32_t)len; i++)
            p |= (uint32_t)m[i] << (8 * (i + 1));
        pk[np++] = p;
        if (FW_MQ - (fw->mq_head - fw->mq_tail) < np)
            return -1;
        fw->mq[fw->mq_head++ % FW_MQ] = pk[0];
        return n > len ? fw_midi_feed(fw, m + len, n - len) : 0;
    }
}

int fw_pkt_bytes(uint32_t pkt, uint8_t out[3])
{
    static const int8_t LEN[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };
    const int n = LEN[pkt & 0xF];
    int i;
    for (i = 0; i < n; i++)
        out[i] = (uint8_t)(pkt >> (8 * (i + 1)));
    return n;
}

/* the fed messages' packets, as many as the module takes now (between two calls, before the tick) */
static void feed_ms(fw_t *fw)
{
    while (fw->mq && fw->mq_tail != fw->mq_head && !fw->dead) {
        const uint32_t p = fw->mq[fw->mq_tail % FW_MQ];
        volatile uint32_t ok = 0;
        CALL(fw, "rfm1_midi_in", ok = fw->obj->vt->midi_in(fw->inst, p));
        if (!ok || fw->dead)
            break;   /* (no room: the rest at the next ms) */
        fw->mq_tail++;
        fw->mq_fed++;
        fw->n_in++;
        if (fw->log && fw->log_n < fw->log_cap) {   /* (the replay gives it as an FWI_MIDI at the same ms) */
            fw_log_t *l = &fw->log[fw->log_n++];
            l->ms = fw->ms, l->op = FWI_MIDI, l->a = 0, l->b = 0, l->c = p;
        }
    }
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
    if (fw->mq)
        feed_ms(fw);
    if (fw->wasi->fs)   /* (between two calls: the file requests answered so far, into the module's memory) */
        fsvc_pump(fw->wasi->fs, fw->mem, fw->mem_size, ms);
    {
        const uint64_t t0 = now_ns();
        uint64_t t1;
        CALL(fw, "rfm1_tick", vt->tick(fw->inst, ms));
        t1 = now_ns();
        fw->ns_tick += t1 - t0;
        if (ms == 0 || ms - fw->last_frame >= (fw->frame_ms ? fw->frame_ms : FW_FRAME_MS)) {
            fw->last_frame = ms;
            CALL(fw, "rfm1_frame", vt->frame(fw->inst));
            fw->ns_frame += now_ns() - t1;
            fw->n_frame++;
        } else {
            CALL(fw, "rfm1_idle", vt->idle(fw->inst));
            fw->ns_tick += now_ns() - t1;
        }
    }
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

int fw_has_ain(const fw_t *fw) { return fw->obj && fw->obj->ain1 && fw->obj->ain; }

static int ain_alloc(fw_t *fw)   /* rfm1_alloc'd like the output's */
{
    volatile uint32_t b = 0;
    CALL(fw, "rfm1_alloc", b = fw->obj->vt->alloc(fw->inst, FW_BLOCK * 4u));
    if (fw->dead || !b || !in_mem(fw, b, FW_BLOCK * 4u))
        return -1;
    fw->ain_buf = b;
    return 0;
}

static void ain_info_call(fw_t *fw, const rfm1m_ain_t *a, uint32_t st, uint32_t lt)
{
    volatile uint32_t v[2];
    v[0] = st, v[1] = lt;
    CALL(fw, "rfm1_audio_in_info", a->audio_in_info(fw->inst, v[0], v[1]));
}

int fw_ain_info(fw_t *fw, uint32_t state_, uint32_t lat_)
{
    const rfm1m_ain_t *a;
    const uint32_t state = state_ ? 1u : 0u, lat = state_ ? lat_ : 0u;   /* (copies: CALL's longjmp) */
    if (!fw->booted || fw->dead || !fw_has_ain(fw))
        return -1;
    a = fw->obj->ain;
    if (state && !fw->ain_buf && ain_alloc(fw) != 0)   /* (the input buffer: as rfm1_run, after init) */
        return -1;
    if (fw->ain_said ? state == fw->ain_state && lat == fw->ain_lat : !state)
        return 0;   /* (told only when something changed; never told: nothing to turn off) */
    if (a->audio_in_info)
        ain_info_call(fw, a, state, lat);
    fw->ain_state = state, fw->ain_lat = lat, fw->ain_said = 1;
    return fw->dead ? -1 : 0;
}

int fw_render(fw_t *fw, int16_t out[FW_BLOCK * 2]) { return fw_render_in(fw, out, NULL); }

int fw_render_in(fw_t *fw, int16_t out[FW_BLOCK * 2], const int16_t *in)
{
    const uint64_t t0 = now_ns();
    uint64_t dt;
    if (!fw->booted || fw->dead) {
        memset(out, 0, FW_BLOCK * 4);
        return -1;
    }
    while (fw->frames_done + FW_BLOCK > fw->due && !fw->dead)
        run_ms(fw);
    if (in && fw->ain_state && fw->ain_buf && !fw->dead) {   /* ain1: the input block, then the same frames out */
        int i, pk = 0;
        memcpy(fw->mem + fw->ain_buf, in, FW_BLOCK * 4u);
        for (i = 0; i < FW_BLOCK * 2; i++) {
            const int v = in[i] < 0 ? -in[i] : in[i];
            if (v > pk)
                pk = v;
        }
        fw->ain_peak = pk;
        CALL(fw, "rfm1_audio_in", fw->obj->ain->audio_in(fw->inst, fw->ain_buf, FW_BLOCK));
        fw->ain_blocks++;
    }
    if (!fw->dead) {
        const uint64_t ta = now_ns();
        CALL(fw, "rfm1_audio", fw->obj->vt->audio(fw->inst, fw->scratch + S_AUDIO, FW_BLOCK));
        fw->ns_audio += now_ns() - ta;
    }
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

int fw_readout(fw_t *fw, fw_readout_t *out, int take)
{
    const uint8_t *r;
    uint32_t seq;
    uint16_t ver, size;
    int16_t frac;
    if (!fw->booted || fw->dead || !fw->obj->rd1)
        return 0;
    if (!fw->rd_asked) {   /* (the record's place: once, after boot; it never moves) */
        volatile uint32_t p = 0;
        fw->rd_asked = 1;
        CALL(fw, "rfm1_readout", p = fw->obj->rd1->readout(fw->inst));
        fw->rd_ptr = !fw->dead && p && in_mem(fw, p, 48) ? p : 0;
    }
    if (!fw->rd_ptr)
        return 0;
    r = fw->mem + fw->rd_ptr;
    memcpy(&ver, r, 2);
    memcpy(&size, r + 2, 2);
    memcpy(&seq, r + 4, 4);
    if (ver != 1 || size < 48 || seq == fw->rd_seen)
        return 0;
    fw->rd_seen = seq;
    if (!take)
        return 0;
    memcpy(&frac, r + 12, 2);
    out->seq = seq;
    out->control = (int8_t)r[8];
    out->kind = r[9];
    out->flags = r[10];
    out->ms = r[11] * 10;
    out->frac = frac;
    memcpy(out->title, r + 14, 16);
    out->title[16] = 0;
    memcpy(out->value, r + 31, 16);
    out->value[16] = 0;
    return 1;
}

const uint8_t *fw_tk(const fw_t *fw)
{
    return fw->booted && !fw->dead && fw->tk_ptr ? fw->mem + fw->tk_ptr : NULL;
}

void fw_tk_input(fw_t *fw, uint32_t pkt_)
{
    volatile uint32_t pkt = pkt_, r = 0;
    if (!fw->booted || fw->dead || !fw->tk_ptr)
        return;
    CALL(fw, "rfm1_tk_input", r = fw->obj->tk1->input(fw->inst, pkt));
    (void)r;   /* (0: reserved) */
}

void fw_tk_state(fw_t *fw, uint32_t on_, uint32_t claims_, uint32_t flags_)
{
    volatile uint32_t on = on_, claims = claims_, flags = flags_;
    if (!fw->booted || fw->dead || !fw->tk_ptr)
        return;
    CALL(fw, "rfm1_tk_state", fw->obj->tk1->state(fw->inst, on, claims, flags));
}

const uint8_t *fw_ep(const fw_t *fw)
{
    return fw->booted && !fw->dead && fw->ep_ptr ? fw->mem + fw->ep_ptr : NULL;
}

const uint8_t *fw_ep_bytes(const fw_t *fw, uint32_t off, uint32_t n)
{
    return fw->booted && !fw->dead && fw->ep_ptr && n && in_mem(fw, off, n) ? fw->mem + off : NULL;
}

int fw_ep_set(fw_t *fw, uint32_t op_, uint32_t layer_, uint32_t pid_, int32_t value_)
{
    volatile uint32_t op = op_, layer = layer_, pid = pid_, value = (uint32_t)value_;
    volatile int32_t r = -1;
    if (!fw->booted || fw->dead || !fw->ep_ptr)
        return -1;
    CALL(fw, "rfm1_ep_set", r = fw->obj->ep1->set(fw->inst, op, layer, pid, value));
    fw->ep_sets++;
    if (r)
        fw->ep_refused++;
    return r;
}

void fw_ep_watch(fw_t *fw, int32_t layer_, int32_t page_)
{
    volatile uint32_t layer = (uint32_t)layer_, page = (uint32_t)page_;
    if (!fw->booted || fw->dead || !fw->ep_ptr)
        return;
    CALL(fw, "rfm1_ep_watch", fw->obj->ep1->watch(fw->inst, layer, page));
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

/* ------------------------------------------------------------------ snapshots (README "The working memory") --- */
#include "snapsafe.h"

/* the module's entry in snapsafe.h, NULL: not known */
static const snapsafe_t *snap_entry(const fw_obj_t *o)
{
    size_t i;
    if (!o || !o->vt->sha256 || !o->vt->wasm2c || strcmp(o->vt->wasm2c, "1.0.32") || o->fs1 || o->virt)
        return NULL;
    for (i = 0; i < sizeof SNAPSAFE / sizeof SNAPSAFE[0]; i++)
        if (!strcmp(SNAPSAFE[i].sha256, o->vt->sha256))
            return &SNAPSAFE[i];
    return NULL;
}

/* the globals of a wasm2c instance (inst: the module's instance struct) at g_off, g_len bytes, checked: the memory's
 * own record (its data pointer, its size) follows them where wasm2c puts it. NULL: not as the table says */
static uint8_t *snap_globals(const fw_t *fw, void *inst, const snapsafe_t *e)
{
    const uint32_t mo = ((uint32_t)e->g_off + e->g_len + 7u) & ~7u;
    uint8_t *const b = inst;
    uint8_t *data;
    uint32_t size;
    if (!inst || e->g_len > FW_SNAP_GMAX)
        return NULL;
    memcpy(&data, b + mo, sizeof data);
    memcpy(&size, b + mo + sizeof data + 8, 4);   /* (wasm_rt_memory_t: data, pages, max_pages, size) */
    return data == fw->mem && size == fw->mem_size ? b + e->g_off : NULL;
}

int fw_snap_able(fw_t *fw)
{
    const snapsafe_t *e = fw->obj ? snap_entry(fw->obj) : NULL;
    if (!e || !fw->booted || fw->dead || fw->wasi->fs || !snap_globals(fw, fw->inst, e))
        return 0;
    if (fw->c1 && !snap_globals(fw, fw->c1->twin, e))
        return 0;
    return 1;
}

int fw_snap_take(fw_t *fw, fw_snap_t *h, uint8_t *mem)
{
    const snapsafe_t *e;
    struct fw_c1 *c = fw->c1;
    int i;
    if (!fw_snap_able(fw))
        return -1;
    e = snap_entry(fw->obj);
    if (c && c->pending) {   /* (core 1's job in flight: done first; the module's wait still finds it done) */
        const uint32_t want = c->post;
        uint32_t d;
        while ((d = __atomic_load_n(&c->done, __ATOMIC_ACQUIRE)) != want)
            fu_wait(&c->done, d);
    }
    memset(h, 0, sizeof *h);
    snprintf(h->sha256, sizeof h->sha256, "%s", fw->obj->vt->sha256);
    h->mem_size = fw->mem_size;
    h->g_off = e->g_off, h->g_len = e->g_len;
    memcpy(h->g, snap_globals(fw, fw->inst, e), e->g_len);
    if (c) {
        h->tg_len = e->g_len;
        memcpy(h->tg, snap_globals(fw, c->twin, e), e->g_len);
        h->c1_stack = c->stack;
    }
    h->scratch = fw->scratch;
    h->ms = fw->ms, h->last_frame = fw->last_frame, h->frame_ms = fw->frame_ms;
    h->frames_done = fw->frames_done, h->due = fw->due;
    h->tk_ptr = fw->tk_ptr, h->rd_ptr = fw->rd_ptr, h->rd_asked = (uint32_t)fw->rd_asked;
    h->ain_buf = fw->ain_buf, h->ain_state = fw->ain_state, h->ain_lat = fw->ain_lat, h->ain_said = (uint32_t)fw->ain_said;
    h->keys_held = fw->keys_held, h->btns_held = fw->btns_held;
    h->qn = (uint32_t)fw->qn;
    for (i = 0; i < fw->qn; i++) {
        h->q[i].due = fw->q[i].due, h->q[i].op = fw->q[i].op, h->q[i].a = fw->q[i].a;
        h->q[i].b = fw->q[i].b, h->q[i].c = fw->q[i].c;
    }
    memcpy(mem, fw->mem, fw->mem_size);
    return 0;
}

int fw_snap_restore(fw_t *fw, const fw_snap_t *h, const uint8_t *mem)
{
    const snapsafe_t *e = fw->obj ? snap_entry(fw->obj) : NULL;
    uint8_t *g, *tg = NULL;
    uint32_t i;
    const char *why = NULL;
    if (fw->dead || fw->booted)
        why = "not a new instance";
    else if (!e || strcmp(h->sha256, fw->obj->vt->sha256))
        why = "another module";
    else if (h->mem_size != fw->mem_size)
        why = "another memory size";
    else if (h->g_off != e->g_off || h->g_len != e->g_len || !(g = snap_globals(fw, fw->inst, e)))
        why = "its globals are not where the table says";
    else if (!!h->tg_len != !!fw->c1 || (fw->c1 && (!(tg = snap_globals(fw, fw->c1->twin, e)) || h->tg_len != e->g_len)))
        why = "core 1 on in one and not the other";
    else if (h->scratch != fw->scratch || h->c1_stack != (fw->c1 ? fw->c1->stack : 0))
        why = "the host's allocations moved";
    else if (h->qn > sizeof fw->q / sizeof fw->q[0])
        why = "a bad input queue";
    if (why) {
        snprintf(fw->err, sizeof fw->err, "snapshot refused: %s", why);
        return -1;
    }
    memcpy(fw->mem, mem, fw->mem_size);   /* (no module code runs: the twin has no job before the first call) */
    memcpy(g, h->g, h->g_len);
    if (tg)
        memcpy(tg, h->tg, h->tg_len);
    fw->ms = h->ms, fw->last_frame = h->last_frame, fw->frame_ms = h->frame_ms;
    fw->frames_done = h->frames_done, fw->due = h->due;
    fw->tk_ptr = h->tk_ptr && in_mem(fw, h->tk_ptr, FW_TK_SIZE) && fw->obj->tk1 ? h->tk_ptr : 0;
    fw->ep_ptr = 0;   /* (ep1: asked again once booted, below: the record does not move, the snapshot does not say it) */
    fw->rd_ptr = h->rd_ptr && in_mem(fw, h->rd_ptr, 48) ? h->rd_ptr : 0;
    fw->rd_asked = (int)h->rd_asked;
    fw->rd_seen = 0;
    fw->ain_buf = h->ain_buf && in_mem(fw, h->ain_buf, FW_BLOCK * 4u) ? h->ain_buf : 0;
    fw->ain_state = h->ain_state, fw->ain_lat = h->ain_lat, fw->ain_said = (int)h->ain_said;
    fw->keys_held = h->keys_held, fw->btns_held = h->btns_held;
    fw->qn = (int)h->qn;
    for (i = 0; i < h->qn; i++) {
        fw->q[i].due = h->q[i].due, fw->q[i].op = h->q[i].op, fw->q[i].a = h->q[i].a;
        fw->q[i].b = h->q[i].b, fw->q[i].c = h->q[i].c;
    }
    fw->booted = 1;
    ep_find(fw);   /* (ep1: rfm1_ep() is the module's own once open; a snapshot from before it opens it here) */
    return 0;
}
