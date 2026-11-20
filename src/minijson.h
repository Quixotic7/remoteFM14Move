/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RFM14_MINIJSON_H
#define RFM14_MINIJSON_H
#include <stddef.h>

/* the value of a top-level key of a JSON object (a pointer into json), NULL when not there */
const char *mj_find(const char *json, const char *key);
/* the length of the JSON value at v (as mj_find gives it), 0 when it is damaged */
size_t mj_span(const char *v);
/* a top-level string field (escapes decoded, cut to cap - 1): 0, or -1 when missing or not a string */
int mj_string(const char *json, const char *key, char *out, size_t cap);
/* an array's elements in turn: *it at the array ('['), each call the next element (a pointer into the JSON) and *it past
 * it; NULL at the end */
const char *mj_elem(const char **it);
int mj_int(const char *json, const char *key, long *out);
/* the JSON string at v ('"', e.g. an array's element) into out (escapes decoded, cut to cap - 1): 0, or -1 not a string */
int mj_str(const char *v, char *out, size_t cap);
/* in as a JSON string's contents (no quotes); the length written */
size_t mj_escape(const char *in, char *out, size_t cap);

#endif
