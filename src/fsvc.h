/* SPDX-License-Identifier: MPL-2.0 */
/* fsvc: a Fantasy CFW's file service, the import module rfm1_host_1 (capability fs1; FM1VST docs/VIRTUAL-CARTS.md
 * sections 3-4, core-glue/common/rfm1_host.h), one per instance, served from the RemoteFM1 folder:
 *
 *   /audio/..  <root>/audio/..       every cart reads it ("folders": {"audio": "r"})
 *   /code/..   <root>/code/<id>/..   the cart's own scripts and assets, read ("code": "r")
 *   /data/..   <root>/data/<id>/..   the cart's own files, read and write ("data": "rw"); the only place it writes
 *
 * Only what the manifest's "folders" lists; "..", ".", empty segments, '\' and NUL refused; a path (or, for one being
 * made, its folder) that resolves out of its area through a symlink refused; at most FSV_INFLIGHT requests in flight
 * and FSV_MAXLEN bytes a request. The semantics are FM1VST tools/rfm1_run's --folder (fs_map, fs_inside, fs_do), with
 * the stricter path rules of the iOS and Android hosts (RFMFileApi.cpp): no empty segment (a trailing '/' too), strict
 * UTF-8, no write to /data itself, a LIST that cannot fit its next entry is FS_EIO (not the end).
 *
 * THREADS AND TIMING. The imports (fsvc_request, _poll, _cancel, _changed) run on the engine's thread, inside a call
 * into the module: they check a request and note it, nothing else (no lock, no file, no syscall). Between two calls
 * (before each rfm1_tick: fwrt.c run_ms) the engine's thread calls fsvc_pump: requests made since go to the I/O
 * thread (fsvc_set_dispatch: the storage module's file thread, storage.c), and the requests it has finished are copied
 * into the module's memory and become visible to fs_poll. So a request is never answered during a call, and a buffer
 * is only ever written between calls. Without a dispatcher (tools/fwrun) each request is done in the pump one device
 * ms after it was made, exactly as rfm1_run --folder does it, so the two can be compared byte for byte.
 * The I/O thread reads into its own staging buffer (a write's bytes are copied out of the module when it is made);
 * an instance freed while its requests run is kept alive until the last one returns (a reference count). */
#ifndef RFM14_FSVC_H
#define RFM14_FSVC_H
#include <stddef.h>
#include <stdint.h>

enum { FSV_STAT = 1, FSV_LIST, FSV_READ, FSV_WRITE, FSV_DELETE, FSV_MKDIR };
enum { FSV_PENDING = -1, FSV_ENOENT = -2, FSV_EACCES = -3, FSV_EIO = -4, FSV_ENOSPC = -5, FSV_EBUSY = -6,
       FSV_ECANCELED = -7, FSV_ENOFOLDER = -8 };
#define FSV_INFLIGHT 32                /* requests the cart may have in flight */
#define FSV_TABLE 64                   /* (and room for cancelled ones the I/O thread still holds) */
#define FSV_MAXLEN (1024 * 1024)       /* bytes per request */
#define FSV_PUMP_BYTES (1024 * 1024)   /* at most this much copied into a module per pump (one more if alone) */
#define FSV_PATH 1024

enum { FSV_AUDIO, FSV_CODE, FSV_DATA };
#define FSV_R 1
#define FSV_W 2

typedef struct fsvc fsvc_t;

/* ---- the folder (any thread; set before instances are made) */
void fsvc_set_root(const char *root);  /* the RemoteFM1 folder; NULL / "": none (every request FS_ENOFOLDER) */
const char *fsvc_root(void);
/* the I/O thread: fn(io) must, on another thread, call fsvc_serve(io). NULL: done inline in the pump (one ms late) */
void fsvc_set_dispatch(void (*fn)(void *io));
void fsvc_serve(void *io);
/* (the recorder's takes) audio/<name> may be a symbolic link out of the folder: what resolves inside its target is
 * the audio area's too (one name; NULL / "": none). Any other link out stays refused */
void fsvc_set_audio_link(const char *name);
/* the folder changed (files added or removed outside the carts: the page): every cart's fs_changed moves */
void fsvc_bump_changed(void);

/* ---- an instance (made where the instance is made; released where it is freed) */
/* access[FSV_AUDIO..FSV_DATA]: FSV_R | FSV_W bits (the manifest's "folders"); its own code/<id>, data/<id> and
 * audio/ are made in the folder when they are missing */
fsvc_t *fsvc_new(const char *id, const uint8_t access[3]);
/* the manifest's "folders" object (JSON text holding it: the manifest or the .rfm1m's .json) -> access bits */
void fsvc_access(const char *manifest_json, uint8_t access[3]);
void fsvc_release(fsvc_t *s);          /* (NULL: nothing) */

/* ---- the imports (the engine's thread, during a call); mem / size: the module's linear memory */
int32_t fsvc_request(fsvc_t *s, uint8_t *mem, uint64_t size, int32_t op, uint32_t path, int32_t path_len, int64_t off,
                     uint32_t buf, int32_t len);
int32_t fsvc_poll(fsvc_t *s, int32_t id);
void fsvc_cancel(fsvc_t *s, int32_t id);
int32_t fsvc_changed(fsvc_t *s);

/* ---- between two calls (the engine's thread): the device ms about to run */
void fsvc_pump(fsvc_t *s, uint8_t *mem, uint64_t size, uint32_t ms);

typedef struct {
    uint32_t made, served, refused, busy, inflight;
    uint64_t bytes_in, bytes_out;
} fsvc_stats_t;
void fsvc_stats(const fsvc_t *s, fsvc_stats_t *out);

/* ---- the rules alone (tests/unit.c) */
/* a virtual path (n bytes, not NUL-terminated) -> the host path under root and its area; 0, or FSV_EACCES /
 * FSV_ENOENT. top: 1 when it is the area itself (/audio, /code, /data) */
int32_t fsvc_map(const char *root, const char *id, const uint8_t *v, uint32_t n, char *out, size_t cap, int *area,
                 int *top);
/* the host path (or, for one not there yet, its nearest folder) resolves inside the area's base under root (or, in
 * the audio area, inside fsvc_set_audio_link's target) */
int fsvc_inside(const char *root, const char *id, const char *path, int area);

#endif
