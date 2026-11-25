/* SPDX-License-Identifier: MPL-2.0 */
/* The page's file browser over the RemoteFM1 folder (web.c /api/fs/...): the path rules, what may change and what
 * may not, the listing, and a folder as a store-only .zip streamed piece by piece. Pure functions over a root path
 * (the tests' unit checks call them directly); no threads of its own.
 *
 * Paths are relative to the root, '/' between segments, "" the root itself. A segment: 1..255 bytes of valid UTF-8,
 * no control characters, no '\', not "." or "..", not hidden (a leading '.': the host's own temporary files are);
 * at most 1024 bytes in all. A name the page makes (an upload, a new folder, a rename's target) also has none of
 * : * ? " < > | and no leading or trailing space and no trailing '.' (it must survive a zip on any computer). No
 * segment on the way may be a symbolic link (nothing reaches outside the root), but the one files_follow_link names
 * (audio/rec, the sampler's takes), and then only inside its target.
 *
 * What may change (files_refuse): only audio/, code/, data/, states/ and move/projects/; the five folders themselves
 * stay. firmwares/ is read-only (a firmware is native code the Move runs: only the signed catalog installs one),
 * catalog/ too (the signed index), and the root's own files and move/'s settings and library (the running core's).
 * While a firmware runs in a slot: its states/<id>/ is left alone, and its data/<id>/ and code/<id>/ take new files
 * but no delete, rename or replace; the open project's move/projects/move-project-NN.* neither. */
#ifndef RFM14_FILES_H
#define RFM14_FILES_H
#include <stddef.h>
#include <stdint.h>

#define FILES_PATH_MAX 1024
#define FILES_SEG_MAX 255

/* the path rules: 0 ok, -1 not (why says); new: the last segment is a name the page makes */
int files_rel_ok(const char *rel, int is_new, const char **why);
/* rel's segments, from the root, are no symbolic links (and exist as far as they go): 0, -1 a link on the way, or a
 * "." / ".." segment. The one followed link (files_follow_link) may be on the way, once: then rel must resolve inside
 * its target (as far as it exists), with no other link after it */
int files_no_links(const char *root, const char *rel);
/* (README "The sampler") the one symbolic link the browser follows: rel under root (audio/rec, the sampler's takes in
 * the Move's sample library). Listed as a folder ("link": true), listed, read, renamed and deleted inside; itself it
 * stays (no delete, rename or replace). NULL / "": none */
void files_follow_link(const char *root, const char *rel);
/* lstat, but the followed link (to a folder) as the folder it points to */
struct stat;
int files_lstat(const char *path, struct stat *st);
/* %XX-decoded (a query value: '+' a space) into out: 0, or -1 (a bad escape, %00, too long) */
int files_unescape(const char *in, size_t n, char *out, size_t cap);

/* what runs now (the engine's slots, the open project) */
typedef struct {
    char ids[3][32];          /* the firmware ids in the slots ("" none) */
    int open_project;         /* 0..31, -1 unknown */
} files_use_t;
enum { FILES_PUT = 1, FILES_REPLACE, FILES_MKDIR, FILES_DELETE, FILES_RENAME };
/* whether op on rel may happen: NULL yes, else why not (into why) */
const char *files_refuse(const char *rel, int op, const files_use_t *u, char *why, size_t cap);
/* rel may change at all (files_refuse with no slot running): 1, 0 */
int files_writable(const char *rel);

/* the listing of a folder as JSON ({"path", "entries": [{"name", "size", "mtime", "dir"}], ...}: folders first, then
 * A-Z; hidden names, links and names the rules refuse are left out): malloc'd, NULL (not a folder; errno) */
char *files_list_json(const char *root, const char *rel);

/* ---- a folder as a .zip (store only, a data descriptor after each file, UTF-8 names; no zip64: 4 GB and 65535
 * entries at most): planned at once (each file's size and time), its length known before the first byte; then read
 * out piece by piece. A file that changed size since is cut or padded to the planned size (the CRC is of the bytes
 * sent). Names inside start with the folder's own name ("states/..."; the root: "RemoteFM1/") */
typedef struct files_zip files_zip_t;
files_zip_t *files_zip_plan(const char *root, const char *rel, uint64_t *length, const char **why);
/* the same for the files directly in rel whose names start with prefix (a state's, a project's: web.c /api/store),
 * inside a folder named top; NULL with why ("nothing there": none) */
files_zip_t *files_zip_plan_sel(const char *root, const char *rel, const char *prefix, const char *top,
                                uint64_t *length, const char **why);
/* a .zip as files_zip_* writes it (store only; folders inside ignored, each file by its own name, __MACOSX/ and
 * hidden names skipped) unpacked into the folder dir (made by the caller, empty): at most max files and max_bytes in
 * all, each CRC checked, no name twice. The count, or -1 with why (the page's words) */
int files_unzip(const char *zip, const char *dir, int max, uint64_t max_bytes, const char **why);
/* up to cap bytes of the archive into buf: how many; 0 the end; -1 a read failed (the archive is cut) */
long files_zip_read(files_zip_t *z, uint8_t *buf, size_t cap);
void files_zip_free(files_zip_t *z);
uint32_t files_crc32(uint32_t crc, const uint8_t *b, size_t n);

/* a folder and everything in it removed (links unlinked, not followed): 0, -1 */
int files_rmtree(const char *path);

#endif
