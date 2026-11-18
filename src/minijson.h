/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RFM14_MINIJSON_H
#define RFM14_MINIJSON_H
#include <stddef.h>

/* the value of a top-level key of a JSON object (a pointer into json), NULL when not there */
const char *mj_find(const char *json, const char *key);
/* a top-level string field (escapes decoded, cut to cap - 1): 0, or -1 when missing or not a string */
int mj_string(const char *json, const char *key, char *out, size_t cap);
int mj_int(const char *json, const char *key, long *out);
/* in as a JSON string's contents (no quotes); the length written */
size_t mj_escape(const char *in, char *out, size_t cap);

#endif
