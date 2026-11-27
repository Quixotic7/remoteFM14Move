/* SPDX-License-Identifier: MPL-2.0 */
/* (looper_test) every allocation src/looper.c makes, counted by thread: looper_test marks the "engine's" thread
 * (la_engine) and checks the count made there stays 0. Included by looper.c only with -DLOOPER_TEST_ALLOC. */
#ifndef RFM14_LOOPER_ALLOC_H
#define RFM14_LOOPER_ALLOC_H
#include <pthread.h>
#include <stdlib.h>

extern pthread_t la_engine;
extern int la_engine_set;
extern long la_engine_allocs;
static inline void la_note(void)
{
    if (la_engine_set && pthread_equal(pthread_self(), la_engine))
        __atomic_add_fetch(&la_engine_allocs, 1, __ATOMIC_RELAXED);
}
static inline void *la_malloc(size_t n) { la_note(); return malloc(n); }
static inline void *la_calloc(size_t a, size_t b) { la_note(); return calloc(a, b); }
static inline void *la_realloc(void *p, size_t n) { la_note(); return realloc(p, n); }
static inline void la_free(void *p) { if (p) la_note(); free(p); }
#define malloc la_malloc
#define calloc la_calloc
#define realloc la_realloc
#define free la_free
#endif
