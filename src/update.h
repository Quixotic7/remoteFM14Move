/* SPDX-License-Identifier: MPL-2.0 */
/* The page's Update (README "Updating from the page"): a signed .rfm1u sent to POST /api/update is checked and put in
 * place of the module's folder (the host's own: remotefm1-host, help.json, site/ ...), with the old one kept for one
 * Undo. Nothing restarts: the next launch of RemoteFM1 on the Move runs it.
 *
 * The file (.rfm1u, tools/make_update.py), little-endian:
 *   "RFM1UPD\0", u32 format (1), u32 the manifest's bytes M (<= 1 MB), u64 the payload's bytes P
 *   the manifest: JSON {"format": 1, "version", "host_commit", "core_commit", "built", "kind": "overlay" | "full",
 *                 "files": [{"path", "size"}]}
 *   the signature: 64 bytes, Ed25519 (update_key.h) over "RFM1-UPDATE-1\0" (14 bytes) + the SHA-256 of the 24 + M
 *                  header bytes and the payload (the signature itself left out)
 *   the payload: P bytes, a gzip'd tar of files relative to the module's folder: an overlay (the files it carries
 *                replace those of the same name; nothing else changes or goes), or a whole module package (every name
 *                under remotefm1/, as dist/remotefm1-module.tar.gz: that top folder is dropped)
 * The file is exactly 24 + M + 64 + P bytes. In the payload: plain files and folders only (a link, a device, a FIFO
 * refuse the whole file), no absolute name, no "." or ".." part, no empty part, no control characters, no name under
 * ".update-" (this feature's own); AppleDouble "._*" and ".DS_Store" entries are skipped. ustar, GNU long names and
 * pax "path" records are read.
 *
 * The install (a worker thread; the web thread only streams the body into a file beside the RemoteFM1 folder):
 *   1. the signature (a bad one: refused, nothing changed), then the payload read through once (its names);
 *   2. a copy of the module's folder staged with the payload over it (everything the folder holds is copied: the
 *      launcher's bin/ with it), its remotefm1-host checked (a plain executable file, an ELF for this machine);
 *   3. the RemoteFM1 folder's move/ backed up: remotefm1-data-backup-YYYYMMDD-HHMMSS.tar.gz beside the RemoteFM1
 *      folder (the newest UPD_BACKUPS kept, older ones of this feature deleted);
 *   4. swapped in by renames: the folder -> <dir>.prev (an older .prev deleted), the copy -> <dir>. When the folder
 *      above the module's cannot be written, the same happens inside it (<dir>/.update-new, <dir>/.update-prev: each
 *      top-level entry renamed in turn).
 * Undo: the newest .prev back in place of the folder (the build undone is deleted; one level only). The running host
 * keeps the program it started with (renames do not touch open files). */
#ifndef RFM14_UPDATE_H
#define RFM14_UPDATE_H
#include <stddef.h>
#include <stdint.h>

#define UPD_MAGIC "RFM1UPD"          /* (8 bytes with its NUL) */
#define UPD_DOMAIN "RFM1-UPDATE-1"   /* (14 bytes with its NUL) */
#define UPD_FORMAT 1u
#define UPD_HEAD 24u
#define UPD_MAN_MAX (1024u * 1024u)
#define UPD_MAX (256ull * 1024 * 1024)       /* an .rfm1u's size cap */
#define UPD_UNPACKED_MAX (1024ull * 1024 * 1024)
#define UPD_ENTRIES_MAX 20000
#define UPD_RESERVE (64ull * 1024 * 1024)    /* left free on the disk */
#define UPD_BACKUPS 3

typedef struct {
    const char *module_dir;   /* the module's folder (the host's own; main.c: beside the program, or --module-dir) */
    const char *data_root;    /* the RemoteFM1 folder: move/ is backed up; the upload and backups go beside it */
    const char *version, *commit, *core, *built;   /* this build (main.c VERSION, build/gen/buildinfo.h) */
} upd_cfg_t;

void upd_init(const upd_cfg_t *c);
int upd_status_json(char *out, size_t cap);   /* GET /api/update: the length written */

/* the upload (web.c): begin with the body's size: 0 and the file to write (tmp), or the HTTP status (409 busy, 413,
 * 507, 500) with why; then done (the worker starts: 0) or abort */
int upd_recv_begin(uint64_t size, char *tmp, size_t cap, char *why, size_t wcap);
void upd_recv_abort(const char *why);
int upd_recv_done(void);
/* POST /api/update/undo: 0 started, or the HTTP status (409) with why */
int upd_undo_start(char *why, size_t wcap);

/* ------------------------------------------------------------------ the parts (tests/update_test.c) --- */
typedef struct {
    char version[64], commit[64], core[64], built[64], kind[16];
    int nfiles;
    uint32_t mlen;
    uint64_t plen, poff;      /* the payload's bytes and where it starts */
    char *json;               /* the manifest (malloc'd, NUL-ended; upd_manifest_free) */
} upd_manifest_t;
void upd_manifest_free(upd_manifest_t *m);
/* the file's header, manifest and signature against keys: 0 good (m filled), -1 why */
int upd_check_file(const char *path, const uint8_t (*keys)[32], int nkeys, upd_manifest_t *m, char *why, size_t cap);
/* a payload name: 0 good (out: normalized), 1 skipped, -1 refused (why) */
int upd_name(const char *in, char *out, size_t cap, const char **why);
/* the payload read through (dest NULL: names and types only) or unpacked into dest (an existing folder):
 * 0, -1 why. *files / *bytes: what it holds; strip: every name was under remotefm1/ (scan) / drop it (unpack) */
int upd_payload(const char *path, const upd_manifest_t *m, const char *dest, int *strip, int *files, uint64_t *bytes,
                char *why, size_t cap);
/* root's move/ as a .tar.gz at out: 0, 1 nothing to back up, -1 why */
int upd_backup_dir(const char *root, const char *sub, const char *out, char *why, size_t cap);
int upd_rmtree(const char *path);

#endif
