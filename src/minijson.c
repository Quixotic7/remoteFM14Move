/* SPDX-License-Identifier: MPL-2.0 */
/* minijson: just enough JSON to read an .rfm1 manifest's top-level fields (RFM1-FORMAT.md section 4). */
#include "minijson.h"

#include <stdlib.h>
#include <string.h>

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* past one JSON string starting at p ('"'); copies its text (escapes decoded; \u: ASCII only, else '?') when out */
static const char *str_(const char *p, char *out, size_t cap)
{
    size_t n = 0;
    if (*p != '"')
        return NULL;
    for (p++; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\') {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned v = 0;
                int k;
                for (k = 1; k <= 4; k++) {
                    const char h = p[k];
                    if (!h)
                        return NULL;
                    v = v * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                }
                p += 4;
                c = v < 128 ? (char)v : '?';
                break;
            }
            case 0: return NULL;
            default: c = *p; break;
            }
        }
        if (out && n + 1 < cap)
            out[n++] = c;
    }
    if (out && cap)
        out[n] = 0;
    return *p == '"' ? p + 1 : NULL;
}

/* past one JSON value */
static const char *skip(const char *p)
{
    int depth = 0;
    p = ws(p);
    do {
        if (*p == '"') {
            if (!(p = str_(p, NULL, 0)))
                return NULL;
            continue;
        }
        if (*p == '{' || *p == '[')
            depth++;
        else if (*p == '}' || *p == ']')
            depth--;
        else if (!*p)
            return NULL;
        else if (depth == 0) {   /* a number, true, false, null */
            while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t')
                p++;
            return p;
        }
        p++;
    } while (depth > 0);
    return p;
}

size_t mj_span(const char *v)
{
    const char *e = v ? skip(v) : NULL;
    return e ? (size_t)(e - v) : 0;
}

const char *mj_elem(const char **it)
{
    const char *p = *it ? ws(*it) : NULL, *e;
    if (!p || (*p != '[' && *p != ','))
        return NULL;
    p = ws(p + 1);
    if (*p == ']' || !*p || !(e = skip(p)))
        return NULL;
    *it = ws(e);
    return p;
}

const char *mj_find(const char *json, const char *key)
{
    const char *p = ws(json);
    char k[64];
    if (*p != '{')
        return NULL;
    p = ws(p + 1);
    while (*p == '"') {
        if (!(p = str_(p, k, sizeof k)))
            return NULL;
        p = ws(p);
        if (*p != ':')
            return NULL;
        p = ws(p + 1);
        if (!strcmp(k, key))
            return p;
        if (!(p = skip(p)))
            return NULL;
        p = ws(p);
        if (*p == ',')
            p = ws(p + 1);
    }
    return NULL;
}

int mj_string(const char *json, const char *key, char *out, size_t cap)
{
    const char *v = mj_find(json, key);
    if (cap)
        out[0] = 0;
    if (!v || *v != '"')
        return -1;
    return str_(v, out, cap) ? 0 : -1;
}

int mj_str(const char *v, char *out, size_t cap)
{
    if (cap)
        out[0] = 0;
    return v && *v == '"' && str_(v, out, cap) ? 0 : -1;
}

int mj_int(const char *json, const char *key, long *out)
{
    const char *v = mj_find(json, key);
    char *end;
    long x;
    if (!v)
        return -1;
    x = strtol(v, &end, 10);
    if (end == v)
        return -1;
    *out = x;
    return 0;
}

size_t mj_escape(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    for (; *in; in++) {
        const unsigned char c = (unsigned char)*in;
        char e[7];
        size_t k = 0, i;
        if (c == '"' || c == '\\')
            e[k++] = '\\', e[k++] = (char)c;
        else if (c < 0x20)
            e[k++] = '\\', e[k++] = 'u', e[k++] = '0', e[k++] = '0', e[k++] = hex[c >> 4], e[k++] = hex[c & 15];
        else
            e[k++] = (char)c;
        if (n + k + 1 > cap)
            break;
        for (i = 0; i < k; i++)
            out[n++] = e[i];
    }
    if (cap)
        out[n] = 0;
    return n;
}
