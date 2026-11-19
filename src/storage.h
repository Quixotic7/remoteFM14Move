/* SPDX-License-Identifier: MPL-2.0 */
/* The RemoteFM1 root folder (default /data/UserData/RemoteFM1), and the storage thread.
 *
 *   firmwares/   <id>-<version>.rfm1m (the compiled firmware), .json (its manifest: the .rfm1's, with the SHA-256 of
 *                that file), .licenses.txt (the .rfm1's credits and licence sections)
 *   states/<id>/ flash.bin (the firmware's flash as it was left), state-NN.bin (the 32 state slots), each with a
 *                .json beside it (id, version, sha256, saved)
 *   move/        settings.bin (rfm1cs's settings image), library.rmvl (its library blob), open-project (the slot),
 *                projects/move-project-NN.rfmmove (rfm1cs's project files, RMVP), and beside each
 *                move-project-NN.fx.json: its FX modules' own state (get_param "state"; R_PROJECT_LOADED's text)
 *
 * Writes go through a temporary file and a rename, so a power cut leaves the old file or the new one. */
#ifndef RFM14_STORAGE_H
#define RFM14_STORAGE_H
#include <stddef.h>
#include <stdint.h>

#include "rfm1cs/move.h"

#define ST_PATH 512
#define ST_FW_MAX 256

int st_init(const char *root);
const char *st_root(void);
/* root-relative path into out */
void st_path(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int st_mkdirs(const char *path);                         /* every directory on the way (the last one too) */
int st_write(const char *path, const void *b, size_t n);   /* atomic; 0 ok */
uint8_t *st_read(const char *path, size_t *n);           /* malloc'd (one spare 0 byte after), NULL: none */
int st_copy(const char *from, const char *to);

/* ---- the installed firmwares (firmwares/<id>-<version>.json) */
typedef struct {
    char id[RFM_MOVE_FW_ID_MAX], name[48], version[32], license[64];
    char source_url[256], sha256[65];
    char so[ST_PATH];                /* the .rfm1m */
    char json[ST_PATH];
} st_fw_t;
/* copy the bundled firmwares (a module's firmwares/ folder) that the root lacks; then list the root's. Sorted A-Z by
 * name. Returns how many (at most max) */
int st_fw_install_bundled(const char *bundled_dir);
int st_fw_scan(st_fw_t *out, int max);
/* versions compared as dotted numbers: < 0, 0, > 0 */
int st_vercmp(const char *a, const char *b);

/* ---- the storage thread: jobs in, results out (the engine drains them on its own thread) */
enum {
    J_WRITE = 1,          /* path, data/len: written (no result) */
    J_FW_OPEN,            /* slot, id, path (the .rfm1m), aux_path (flash.bin): back with ptr (the instance made, its
                           * memory and data: fw_instantiate) + data (the flash, or none) */
    J_STATE_SAVE,         /* slot, index, id, data/len (the image), text (its .json): back with used bits */
    J_STATE_LOAD,         /* slot, index, id, obj: back with data (the image; none: a failure) and ptr (an instance) */
    J_STATE_DELETE,       /* slot, index, id: back with used bits */
    J_STATE_SCAN,         /* slot, id: back with used bits */
    J_PROJECT,            /* kind (the act), index, value, data/len: rfm1cs_store_request */
    J_FW_FREE,            /* ptr: a stopped instance (fw_inst_t) freed (no result) */
    J_LOG_ROTATE,         /* value: the cap: the log (stderr's file) moved to <log>.1 when past it (no result) */
    R_PROJECT_LOADED,     /* (results) index, data/len */
    R_PROJECT_STORED,     /* index, data/len, value: used bits */
    /* the network thread's (catalog.h): one job at a time, in order */
    J_NET_INDEX,          /* used: the index hash in use, text: the keep list -> back as R_INDEX: ok (err), ptr: a new
                           * cat_t when the index changed */
    J_NET_DOWNLOAD,       /* id, text: the entry's files, total: bytes, data: the keep list -> R_DL_PROGRESS (id,
                           * value: 1..99) as it goes, then back as R_DL_DONE: ok (err), ptr: the new cat_t */
    J_NET_SCAN,           /* text: the keep list -> back as R_CATALOG, ptr: the new cat_t */
    R_INDEX, R_DL_PROGRESS, R_DL_DONE, R_CATALOG,
    /* (the storage thread's again) Schwung FX (fx.h) */
    J_FX_OPEN,            /* obj: the module (fx_mod_t), slot, index (kind), value (a generation): back with ptr (the
                           * fx_inst_t made, NULL with err) */
    J_FX_FREE,            /* ptr: an fx_inst_t closed (no result) */
};
typedef struct st_job {
    int kind, slot, index, value, act;
    char id[RFM_MOVE_FW_ID_MAX];
    char path[ST_PATH], aux_path[ST_PATH];
    char *text;
    uint8_t *data;
    uint32_t len;
    uint32_t used;
    void *obj;                        /* J_STATE_LOAD: the running instance's fw_obj_t (a reference, given up by the job) */
    void *ptr;                        /* J_FW_OPEN, J_STATE_LOAD: back with the instance made (fw_inst_t; NULL with
                                       * err); J_FW_FREE's; R_*: the cat_t */
    uint64_t total;
    int ok;
    char err[300];
    struct st_job *next;
} st_job_t;

/* used: the project slots that hold a project (st_projects_scan) */
int st_worker_start(uint32_t project_used);
void st_worker_stop(void);            /* every job done first (a download under way is stopped) */
void st_post(st_job_t *j);            /* the worker owns it now (and its data / text); J_NET_* go to the network thread */
st_job_t *st_take(void);              /* a result, or NULL; free with st_job_free */
st_job_t *st_job_new(int kind);
void st_job_free(st_job_t *j);
/* the projects at start (the main thread, before the worker): the used slots; the open slot's blob into *blob */
uint32_t st_projects_scan(int open_slot, uint8_t **blob, uint32_t *n);
/* a project's FX state file (move/projects/move-project-NN.fx.json), slot 0..31 */
void st_fx_state_path(char *out, size_t cap, int slot);
/* the states a firmware has stored (bit k: state-k), any thread */
uint32_t st_states_used(const char *id);

#endif
