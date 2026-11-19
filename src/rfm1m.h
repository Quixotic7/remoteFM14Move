/* SPDX-License-Identifier: MPL-2.0 */
/* A compiled firmware object (".rfm1m"): an .rfm1 translated by wasm2c 1.0.32 and compiled for the Move as a shared
 * object, plus fw/fwglue.c. It exports one symbol, rfm1m_get(), giving this table: the .rfm1's exports
 * (RFM1-FORMAT.md section 2) as plain C calls on an instance. The host gives the runtime the firmware links against
 * (wasm-rt-impl.c, built into the host and exported) and the WASI imports (section 3).
 *
 * Same file, same sound: an .rfm1m is made from one .rfm1, whose SHA-256 it carries (and its manifest beside it, in
 * <id>-<version>.json). Every call must come from the thread that runs the instance, inside the host's trap guard
 * (fwrt.c). */
#ifndef RFM14_RFM1M_H
#define RFM14_RFM1M_H
#include <stddef.h>
#include <stdint.h>

#define RFM1M_ABI 1
#define RFM1M_SYMBOL "rfm1m_get"

struct Z_wasi_snapshot_preview1_instance_t;   /* the host's (fwrt.c) */
struct wasm_rt_memory_t;

typedef struct {
    uint32_t abi;                       /* RFM1M_ABI */
    const char *id;                     /* the manifest's id */
    const char *sha256;                 /* of the .rfm1 it was made from (hex) */
    const char *wasm2c;                 /* "1.0.32" */
    void (*init_module)(void);          /* once per loaded object, before the first create */
    void *(*create)(struct Z_wasi_snapshot_preview1_instance_t *wasi);   /* an instance (its memory, data loaded) */
    void (*destroy)(void *inst);
    uint8_t *(*memory)(void *inst, uint64_t *size);
    /* the .rfm1's exports; the optional ones NULL when the module has none */
    void (*initialize)(void *inst);
    uint32_t (*version)(void *inst);
    uint32_t (*get)(void *inst, uint32_t abi);
    uint32_t (*layout)(void *inst, uint32_t i);
    uint32_t (*alloc)(void *inst, uint32_t n);
    void (*free_)(void *inst, uint32_t p);
    void (*options)(void *inst, uint32_t path, uint32_t no_flash, uint32_t save, uint32_t headless);
    uint32_t (*boot_options)(void *inst, uint32_t fail, uint32_t reason, uint32_t stage);
    void (*init)(void *inst, uint32_t demo);
    void (*tick)(void *inst, uint32_t ms);
    void (*frame)(void *inst);
    void (*idle)(void *inst);
    void (*audio)(void *inst, uint32_t out, uint32_t frames);
    uint32_t (*midi_in)(void *inst, uint32_t pkt);
    uint32_t (*midi_out_take)(void *inst, uint32_t pkt_out);
    uint32_t (*halted)(void *inst);
    void (*shutdown)(void *inst);
    uint32_t (*flash_stage)(void *inst, uint32_t bytes, uint32_t n);
    uint32_t (*flash)(void *inst);
    uint32_t (*flash_dirty)(void *inst);
    uint32_t (*flash_sync)(void *inst);
    void (*stats)(void *inst, uint32_t shed_out, uint32_t cpu_out);
    void (*ui_info)(void *inst, uint32_t buf, uint32_t n);
    uint32_t (*labels_epoch)(void *inst);   /* optional (DYNAMIC-LABELS) */
    uint32_t (*label)(void *inst, uint32_t which);
} rfm1m_t;

typedef const rfm1m_t *(*rfm1m_get_fn)(void);

/* ain1 (FM1VST docs/VIRTUAL-CARTS.md section 8): an object made from a module that exports rfm1_audio_in also exports
 * RFM1M_AIN_SYMBOL, this table (fwglue.c, built with -DRFM1M_AIN1 by tools/build_fw.sh only for such a module, so
 * every other object stays byte for byte what it was). audio_in_info NULL: the module has no rfm1_audio_in_info */
#define RFM1M_AIN_ABI 1
#define RFM1M_AIN_SYMBOL "rfm1m_ain"
typedef struct {
    uint32_t abi;                       /* RFM1M_AIN_ABI */
    void (*audio_in)(void *inst, uint32_t stereo, uint32_t frames);
    void (*audio_in_info)(void *inst, uint32_t state, uint32_t latency_frames);
} rfm1m_ain_t;
typedef const rfm1m_ain_t *(*rfm1m_ain_fn)(void);

#endif
