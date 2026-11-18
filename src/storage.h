/* SPDX-License-Identifier: MPL-2.0 */
/* The RemoteFM1 root folder (default /data/UserData/RemoteFM1), and the storage thread.
 *
 *   firmwares/   <id>-<version>.rfm1m (the compiled firmware), .json (its manifest: the .rfm1's, with the SHA-256 of
 *                that file), .licenses.txt (the .rfm1's credits and licence sections)
 *   states/<id>/ flash.bin (the firmware's flash as it was left), state-NN.bin (the 32 state slots), each with a
 *                .json beside it (id, version, sha256, saved)
 *   move/        settings.bin (rfm1cs's settings image), library.rmvl (its library blob), open-project (the slot),
 *                projects/move-project-NN.rfmmove (rfm1cs's project files, RMVP)
 *
 * Writes go through a temporary file and a rename, so a power cut leaves the old file or the new one. */
#ifndef RFM14_STORAGE_H
#define RFM14_STORAGE_H
#include <stddef.h>
#include <stdint.h>

#include "rfm1cs/move.h"

#define ST_PATH 512
#define ST_FW_MAX 64

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

/* ---- the storage thread: jobs in, results out (the engine drains them on its own thread) */
enum {
    J_WRITE = 1,          /* path, data/len: written (no result) */
    J_FW_OPEN,            /* slot, id, path (the .rfm1m), aux_path (flash.bin): obj + data (the flash, or none) back */
    J_STATE_SAVE,         /* slot, index, id, data/len (the image), text (its .json): back with used bits */
    J_STATE_LOAD,         /* slot, index, id: back with data (the image; none: a failure) */
    J_STATE_DELETE,       /* slot, index, id: back with used bits */
    J_STATE_SCAN,         /* slot, id: back with used bits */
    J_PROJECT,            /* kind (the act), index, value, data/len: rfm1cs_store_request */
    R_PROJECT_LOADED,     /* (results) index, data/len */
    R_PROJECT_STORED,     /* index, data/len, value: used bits */
};
typedef struct st_job {
    int kind, slot, index, value, act;
    char id[RFM_MOVE_FW_ID_MAX];
    char path[ST_PATH], aux_path[ST_PATH];
    char *text;
    uint8_t *data;
    uint32_t len;
    uint32_t used;
    void *obj;                        /* J_FW_OPEN: the fw_obj_t, NULL with err */
    char err[200];
    struct st_job *next;
} st_job_t;

/* used: the project slots that hold a project (st_projects_scan) */
int st_worker_start(uint32_t project_used);
void st_worker_stop(void);            /* every job done first */
void st_post(st_job_t *j);            /* the worker owns it now (and its data / text) */
st_job_t *st_take(void);              /* a result, or NULL; free with st_job_free */
st_job_t *st_job_new(int kind);
void st_job_free(st_job_t *j);
/* the projects at start (the main thread, before the worker): the used slots; the open slot's blob into *blob */
uint32_t st_projects_scan(int open_slot, uint8_t **blob, uint32_t *n);
/* the states a firmware has stored (bit k: state-k), any thread */
uint32_t st_states_used(const char *id);

#endif
