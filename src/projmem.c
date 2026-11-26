/* SPDX-License-Identifier: MPL-2.0 */
/* A project's copies of its firmwares' memory, the states' files: see projmem.h. */
#include "projmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minijson.h"
#include "sha256.h"

void pm_copy_path(char *out, size_t cap, const char *root, int proj, int slot)
{
    snprintf(out, cap, "%s/move/projects/move-project-%02d.fw%d.bin", root, proj + 1, slot + 1);
}

void pm_clip_path(char *out, size_t cap, const char *root, int slot)
{
    snprintf(out, cap, "%s/move/projects/.clip.fw%d.bin", root, slot + 1);
}

void pm_json_path(char *out, size_t cap, const char *bin)
{
    const size_t n = strlen(bin);
    if (n > 4 && !strcmp(bin + n - 4, ".bin"))
        snprintf(out, cap, "%.*s.json", (int)(n - 4), bin);
    else
        snprintf(out, cap, "%s.json", bin);
}

void pm_hash_hex(const uint8_t *b, uint32_t n, char hex[65])
{
    sha256_t s;
    uint8_t d[32];
    int i;
    sha256_init(&s);
    sha256_update(&s, b, n);
    sha256_final(&s, d);
    for (i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
}

char *pm_json_hash(const char *text, const uint8_t *b, uint32_t n)
{
    const char *end;
    char hex[65], *o;
    size_t k, cap;
    if (!text)
        return NULL;
    if (!(end = strrchr(text, '}')))
        return strdup(text);
    pm_hash_hex(b, n, hex);
    k = (size_t)(end - text);
    cap = strlen(text) + 96;
    if (!(o = malloc(cap)))
        return NULL;
    snprintf(o, cap, "%.*s%s\"hash\":\"%s\"%s", (int)k, text, k > 1 && text[k - 1] != '{' ? "," : "", hex, end);
    return o;
}

char *pm_json_add(const char *text, const char *key, const char *val)
{
    const char *end;
    char *o;
    size_t k, cap;
    if (!text)
        return NULL;
    if (!(end = strrchr(text, '}')))
        return strdup(text);
    k = (size_t)(end - text);
    cap = strlen(text) + strlen(key) + strlen(val) + 16;
    if (!(o = malloc(cap)))
        return NULL;
    snprintf(o, cap, "%.*s%s\"%s\":\"%s\"%s", (int)k, text, k > 1 && text[k - 1] != '{' ? "," : "", key, val, end);
    return o;
}

static uint8_t *read_all(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long len;
    *n = 0;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 || !(b = malloc((size_t)len + 1))) {
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

int pm_json_id(const char *json_path, char *id, size_t cap)
{
    size_t n;
    char *t = (char *)read_all(json_path, &n);
    int r = t ? mj_string(t, "id", id, cap) : -1;
    free(t);
    if (r)
        id[0] = 0;
    return r ? -1 : 0;
}

int pm_json_str(const char *json_path, const char *key, char *out, size_t cap)
{
    size_t n;
    char *t = (char *)read_all(json_path, &n);
    int r = t ? mj_string(t, key, out, cap) : -1;
    free(t);
    if (r)
        out[0] = 0;
    return r ? -1 : 0;
}

int pm_state_match(const char *root, const char *id, const uint8_t *b, uint32_t n, const char *mem)
{
    char hex[65], p[1024], h[80], m[80];
    int k;
    if (!b || !n || !id || !id[0])
        return -1;
    pm_hash_hex(b, n, hex);
    for (k = 0; k < RFM_MOVE_FW_STATES; k++) {
        size_t mn;
        char *t;
        uint8_t *s;
        int same;
        snprintf(p, sizeof p, "%s/states/%s/state-%02d.json", root, id, k);
        m[0] = 0;
        if ((t = (char *)read_all(p, &mn))) {
            const int has = mj_string(t, "hash", h, sizeof h) == 0;
            if (mj_string(t, "mem", m, sizeof m))
                m[0] = 0;
            free(t);
            if (has && strcmp(h, hex))
                continue;   /* (its hash says another image: not read) */
        }
        if (strcmp(m, mem ? mem : ""))
            continue;   /* (another working memory, or one where the other has none) */
        snprintf(p, sizeof p, "%s/states/%s/state-%02d.bin", root, id, k);
        if (!(s = read_all(p, &mn)))
            continue;
        same = mn == n && !memcmp(s, b, n);
        free(s);
        if (same)
            return k;
    }
    return -1;
}

/* (rfm1cs project.c kit_bytes) the kit part after the firmwares' from version 11: its size by version. A version
 * this file does not know stops the build: its layout must be added here first. (2026-11-26) Version 16: notes of 8
 * bytes (before the parts here, which are read from the end), the kit part as 15's */
typedef char pm_version_check[RFM_MOVE_PROJECT_VERSION == 17 ? 1 : -1];   /* (17, 2026-11-26: Scan, 46 settings a pad) */
static uint32_t kit_bytes(int ver)
{
    return ver >= 17 ? RFM_MOVE_KIT_BYTES : ver >= 15 ? RFM_MOVE_KIT_BYTES_V15 : ver == 14 ? RFM_MOVE_KIT_BYTES_V14 : ver >= 12 ? RFM_MOVE_KIT_BYTES_V12
         : ver == 11 ? RFM_MOVE_KIT_BYTES_V11 : 0;
}
int pm_blob_ids(const uint8_t *b, uint32_t n, char ids[PM_SLOTS][RFM_MOVE_FW_ID_MAX])
{
    const uint8_t *p;
    uint32_t kb;
    int s;
    memset(ids, 0, PM_SLOTS * RFM_MOVE_FW_ID_MAX);
    /* "RMSQ", the version, ..., the version 9 part, (version 11 and up) the kit, a 4-byte check (rfm1cs project.c
     * pfw_part: b + n - 4 - kit_bytes - RFM_MOVE_PFW_BYTES; 2026-11-26: the kit's part was read as the firmwares',
     * so a save of a project with a kit dropped its firmware copies) */
    if (!b || n < 5 || memcmp(b, "RMSQ", 4) || b[4] < 9 || b[4] > RFM_MOVE_PROJECT_VERSION)
        return 0;
    kb = kit_bytes(b[4]);
    if (n < 5u + 4u + RFM_MOVE_PFW_BYTES + kb)
        return 0;
    p = b + n - 4 - kb - RFM_MOVE_PFW_BYTES;
    for (s = 0; s < PM_SLOTS; s++, p += RFM_MOVE_PFW_SLOT_BYTES) {
        memcpy(ids[s], p, RFM_MOVE_FW_ID_MAX);
        ids[s][RFM_MOVE_FW_ID_MAX - 1] = 0;
    }
    return 1;
}
