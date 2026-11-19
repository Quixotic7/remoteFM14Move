/* SPDX-License-Identifier: MPL-2.0 */
/* The firmware catalog the Move's browser shows (rfm1cs-abletonmove's rfm_move_fw_list), and its downloads.
 *
 * The catalog is the Move catalog's index.json (tools/build_move_catalog.py: every published .rfm1 of RemoteFM1's
 * catalog, compiled for the Move) merged with what is installed in the root's firmwares/:
 *   - the index: <root>/catalog/index.json (the last one downloaded) or the module's bundled snapshot, whichever was
 *     generated later, each used only with a good signature beside it (index.json.sig: Ed25519 by the catalog's
 *     signing key, catsig.h); none: only the installed firmwares are listed;
 *   - each entry: the catalog's id, name, version, categories, description and thumbnail, and the version installed
 *     (or none); an installed firmware the index does not list is an entry of its own. A-Z by name.
 *
 * Downloads (the storage module's network thread, never the engine's or the audio thread): https.c (HTTPS through the
 * system's OpenSSL 3, certificates and host names verified) fetches <base>/index.json, <base>/index.json.sig and a
 * firmware's files from the base URL: <root>/catalog-url's first line, else --catalog-url, else CAT_URL_DEFAULT
 * (https). Plain http:// only when the owner's base URL says so (https.h). The index is kept only when its signature
 * is good. Each file goes to firmwares/.dl/<name>.part, its size and SHA-256 are checked against the (signed) index,
 * then every file is renamed into firmwares/, the manifest (.json) last: a firmware is listed only once all of it is
 * there. An update leaves the older version's files until no slot runs it (cat_build's sweep).
 *
 * THREADS: cat_build, cat_fetch_index, cat_download on the network (or main, at start) thread; the engine owns a
 * cat_t once it has it (and frees it with cat_free). */
#ifndef RFM14_CATALOG_H
#define RFM14_CATALOG_H
#include <stdint.h>

#include "rfm1cs/move.h"
#include "storage.h"

#define CAT_URL_DEFAULT "https://github.com/Quixotic7/remoteFM14Move/releases/download/move-catalog"
#define CAT_DESC 1024

typedef struct {
    char id[RFM_MOVE_FW_ID_MAX], name[48], version[32];
    char installed[32];              /* the version in firmwares/ ("" none): rfm_move_fw_installed points here */
    char license[64], source_url[256];
    char desc[CAT_DESC];
    uint8_t thumb[RFM_MOVE_FW_THUMB_BYTES];
    int has_thumb;
    uint8_t cats, cat;
    uint8_t virt;                    /* a Fantasy CFW (the index's / its manifest's "virtual": rfm_move_fw_t.virt) */
    int indexed;                     /* listed by the index (downloadable) */
    char *files;                     /* the index's files to install, "name sha256 size\n" each, the manifest last */
    uint64_t bytes;                  /* .. their sizes added */
} cat_ent_t;

typedef struct {
    int n;
    cat_ent_t *e;                    /* A-Z by name */
    rfm_move_fw_t *list;             /* n entries, pointing into e: rfm_move_fw_list's array */
    int nfw;
    st_fw_t *fw;                     /* the installed firmwares (the newest of each id), A-Z */
    char generated[40];              /* the index's "generated" ("" none) */
    uint32_t index_hash;             /* FNV-1a of the index's bytes (0 none) */
    int from;                        /* 0 no index, 1 the bundled snapshot, 2 the downloaded one */
} cat_t;

/* the module's index snapshot (or NULL), the base URL when the root names none (or NULL: the default) */
void cat_config(const char *bundled_index, const char *base_url);
/* the catalog now: the index and firmwares/. keep: the .rfm1m paths in use ("\n" between; NULL none), whose files the
 * sweep of older versions leaves. NULL only when out of memory */
cat_t *cat_build(const char *keep);
void cat_free(cat_t *c);
int cat_find(const cat_t *c, const char *id);            /* the entry's index, -1 */
int cat_fw_find(const cat_t *c, const char *id);         /* the installed firmware's index, -1 */
/* the same entries (ids, A-Z) in both: a new installed scan can be taken without a new list */
int cat_same_ids(const cat_t *a, const cat_t *b);

/* the base URL now (the root's catalog-url, the configured one, the default) */
void cat_base_url(char *out, int cap);
/* <base>/index.json -> <root>/catalog/index.json when it is a catalog: 0 fetched (*changed: its hash is not old_hash),
 * -1 not (err) */
int cat_fetch_index(uint32_t old_hash, int *changed, char *err, int cap);
/* a firmware's files (an entry's files, total bytes) downloaded, checked and installed. progress(pct 0..99) as it goes.
 * 0 installed; -1 not (err; nothing installed, the .part files gone) */
int cat_download(const char *id, const char *files, uint64_t total, void (*progress)(void *ctx, int pct), void *ctx,
                 char *err, int cap);
/* a download under way is stopped: any thread */
void cat_cancel(void);
/* base64 (up to its end or a '"') -> at most cap bytes: how many, -1 damaged */
int cat_b64(const char *in, uint8_t *out, int cap);

#endif
