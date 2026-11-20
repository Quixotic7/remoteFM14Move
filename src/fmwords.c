/* SPDX-License-Identifier: MPL-2.0 */
/* The screen's text in words: fmwords.h. The rules are fm1-reader's web/words.js (MIT, Charles Vestal), in its order. */
#include "fmwords.h"

#include <stdlib.h>
#include <string.h>

#include "minijson.h"

#define CH(i) ((i) >= 0 && (i) < n ? (unsigned char)s[i] : 0)
static int al(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int dg(int c) { return c >= '0' && c <= '9'; }
static int an(int c) { return al(c) || dg(c); }
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

void fmw_free(fmw_t *w)
{
    free(w->k), free(w->v), free(w->pool), free(w->first);
    memset(w, 0, sizeof *w);
}

int fmw_load(fmw_t *w, const char *json)
{
    const char *it = mj_find(json, "words"), *e;
    long fmt = 0;
    size_t len = strlen(json), used = 0;
    int cnt = 0;
    memset(w, 0, sizeof *w);
    if (mj_int(json, "format", &fmt) || fmt != 1 || !it || *it != '[')
        return -1;
    for (e = it; mj_elem(&e);)
        if (++cnt > FMW_WORDS_MAX)
            return -1;
    w->k = calloc((size_t)cnt + 1, sizeof *w->k);
    w->v = calloc((size_t)cnt + 1, sizeof *w->v);
    w->first = calloc((size_t)cnt + 1, 1);
    w->pool = malloc(len + 1);   /* (the decoded strings are never longer than the JSON) */
    if (!w->k || !w->v || !w->first || !w->pool) {
        fmw_free(w);
        return -1;
    }
    while ((e = mj_elem(&it))) {
        char a[256], b[256];
        const char *p = e;
        const char *x = *p == '[' ? mj_elem(&p) : NULL, *y = x ? mj_elem(&p) : NULL;
        size_t na, nb;
        if (!y || mj_str(x, a, sizeof a) || mj_str(y, b, sizeof b) || !a[0] || (na = strlen(a)) > FMW_WORD_MAX ||
            (nb = strlen(b)) > FMW_WORD_MAX || used + na + nb + 2 > len + 1)
            continue;   /* (as wordsOf: a bad entry left out) */
        w->k[w->n] = memcpy(w->pool + used, a, na + 1), used += na + 1;
        w->v[w->n] = memcpy(w->pool + used, b, nb + 1), used += nb + 1;
        w->first[w->n] = a[0] == '#' || a[0] == '*' ? 0 : (unsigned char)lc((unsigned char)a[0]);
        w->n++;
    }
    return 0;
}

/* how much of s from i the entry k matches (-1 none); its numbers (up to 4) as offsets and lengths */
static int match(const char *s, int n, int i, const char *k, int cap_at[4], int cap_n[4], int *ncap)
{
    int p = i, j;
    *ncap = 0;
    for (j = 0; k[j]; j++) {
        const int c = (unsigned char)k[j];
        if (c == '#') {
            const int b = p;
            if (CH(p) == '-' || CH(p) == '+')
                p++;
            while (dg(CH(p)) || (CH(p) == '.' && dg(CH(p + 1))))
                p++;
            if (p == b || !dg(CH(p - 1)))
                return -1;
            if (*ncap < 4)
                cap_at[*ncap] = b, cap_n[*ncap] = p - b, ++*ncap;
        } else if (c == '*') {
            const int b = p;
            while (an(CH(p)))
                p++;
            if (p == b)
                return -1;
        } else {
            if (!CH(p) || lc(CH(p)) != lc(c))
                return -1;
            p++;
        }
    }
    if (an(CH(p)) && an(CH(p - 1)))
        return -1;   /* (whole: not the start of a longer word) */
    return p - i;
}

typedef struct { char *o; int n, cap; } out_t;
static void put(out_t *o, const char *t, int len)
{
    while (len-- > 0 && *t)
        if (o->n < o->cap - 1)
            o->o[o->n++] = *t++;
        else
            return;
}
static void puts_(out_t *o, const char *t) { put(o, t, (int)strlen(t)); }
static int last(const out_t *o) { return o->n ? (unsigned char)o->o[o->n - 1] : 0; }

/* after a capital A, these make it the note, not the article (case as written: words.js's NOTE_WORD) */
static int note_word(const char *t)
{
    static const char *const W[] = { "MINOR", "MAJOR", "MIN", "MAJ", "SHARP", "FLAT", "NATURAL", "DORIAN", "PHRYGIAN",
        "LYDIAN", "MIXOLYDIAN", "LOCRIAN", "AEOLIAN", "IONIAN", "PENTATONIC", "BLUES", "HARMONIC", "MELODIC",
        "CHROMATIC", "SEVENTH", "SIXTH", "NINTH", "ELEVENTH", "THIRTEENTH", "DIMINISHED", "AUGMENTED", "SUSPENDED",
        "ADD", "DOMINANT", "POWER", "minor", "major", "sharp", "flat", "seventh", "diminished", "augmented",
        "suspended" };
    size_t i;
    for (i = 0; i < sizeof W / sizeof W[0]; i++) {
        const size_t l = strlen(W[i]);
        const int c = (unsigned char)t[l];
        if (!strncmp(t, W[i], l) && !an(c) && c != '_')
            return 1;
    }
    return 0;
}

int fmw_spoken(const char *s, const fmw_t *w, char *buf, int cap)
{
    static const char *const NOTE[33] = { [2] = "half", [4] = "quarter", [8] = "eighth", [16] = "sixteenth",
                                          [32] = "thirty-second" };
    const int n = (int)strlen(s);
    out_t o = { buf, 0, cap };
    int i = 0;
    if (cap <= 0)
        return 0;
    while (i < n) {
        const int c = CH(i), start = i == 0 || !an(CH(i - 1));
        if (start && an(c) && w && w->n) {   /* a word: the list */
            int best = -1, blen = 0, at[4], ln[4], nc = 0, e;
            int bat[4], bln[4], bnc = 0;
            for (e = 0; e < w->n; e++) {
                int m;
                if (w->first[e] && w->first[e] != lc(c))
                    continue;
                if ((m = match(s, n, i, w->k[e], at, ln, &nc)) > blen) {
                    best = e, blen = m, bnc = nc;
                    memcpy(bat, at, sizeof at), memcpy(bln, ln, sizeof ln);
                }
            }
            if (best >= 0) {
                const char *v = w->v[best];
                int k = 0;
                for (; *v; v++)
                    if (*v == '#') {
                        if (k < bnc)
                            put(&o, s + bat[k], bln[k]);
                        k++;
                    } else
                        put(&o, v, 1);
                i += blen;
                continue;
            }
        }
        if (start && c == '1' && CH(i + 1) == '/') {   /* 1/16, 1/8T (a triplet), 1/8. (dotted: the dot ends it) */
            int q = i + 2, d = 0, nd = 0;
            while (dg(CH(q)) && nd < 3)
                d = d * 10 + CH(q) - '0', q++, nd++;
            if (!dg(CH(q)) && !(nd > 1 && CH(i + 2) == '0') && d <= 32 && NOTE[d]) {
                const int trip = CH(q) == 'T',
                          dot = CH(q) == '.' && (q + 1 >= n || CH(q + 1) == ',' || CH(q + 1) == '.');
                if ((trip || dot) ? !an(CH(q + 1)) : !an(CH(q))) {
                    if (dot)
                        puts_(&o, "dotted ");
                    puts_(&o, NOTE[d]);
                    if (trip)
                        puts_(&o, " triplet");
                    i = q + (trip || dot ? 1 : 0);
                    continue;
                }
            }
        }
        if (start && dg(c)) {   /* 4:1 a ratio, 1-16 a range, 1/3 a share */
            int q = i;
            while (dg(CH(q)) || (CH(q) == '.' && dg(CH(q + 1))))
                q++;
            if ((CH(q) == ':' || CH(q) == '-' || CH(q) == '/') && dg(CH(q + 1))) {
                int e = q + 1;
                while (dg(CH(e)))
                    e++;
                if (!an(CH(e))) {
                    put(&o, s + i, q - i);
                    puts_(&o, CH(q) == '/' ? " of " : " to ");
                    i = q + 1;
                    continue;
                }
            }
        }
        if (start && c >= 'A' && c <= 'G' && CH(i + 1) == '#') {   /* C#3: C sharp 3 */
            put(&o, s + i, 1);
            puts_(&o, dg(CH(i + 2)) || CH(i + 2) == '-' ? " sharp " : " sharp");
            i += 2;
            continue;
        }
        if (start && c == 'x' && dg(CH(i + 1))) {   /* x2: 2 times */
            int q = i + 1;
            while (dg(CH(q)))
                q++;
            if (!an(CH(q))) {
                put(&o, s + i + 1, q - i - 1);
                puts_(&o, q - i - 1 == 1 && CH(i + 1) == '1' ? " time" : " times");
                i = q;
                continue;
            }
        }
        if (start && c == 'A' && CH(i + 1) == ' ' && al(CH(i + 2)) && al(CH(i + 3)) && !note_word(s + i + 2)) {
            puts_(&o, "a");   /* A NEW LINE: the article (A MINOR, A FLAT: the note) */
            i++;
            continue;
        }
        if (start && al(c)) {   /* a word in capitals: a word, not letters */
            int q = i, caps = 0, lower = 0, k;
            while (al(CH(q)) || CH(q) == '\'') {
                caps += CH(q) >= 'A' && CH(q) <= 'Z';
                lower += CH(q) >= 'a' && CH(q) <= 'z';
                q++;
            }
            if (caps >= 2 && !lower && !dg(CH(q)))
                for (k = i; k < q; k++) {
                    const char l = (char)lc(CH(k));
                    put(&o, &l, 1);
                }
            else
                put(&o, s + i, q - i);
            i = q;
            continue;
        }
        if (c == '(' && dg(CH(i + 1))) {   /* (1/2): page 1 of 2 */
            int q = i + 1, a0, a1, b0;
            while (dg(CH(q)))
                q++;
            a1 = q;
            if (CH(q) == '/' && dg(CH(q + 1))) {
                b0 = ++q;
                while (dg(CH(q)))
                    q++;
                if (CH(q) == ')') {
                    a0 = i + 1;
                    while (o.n && o.o[o.n - 1] == ' ')
                        o.n--;
                    puts_(&o, ", page ");
                    put(&o, s + a0, a1 - a0);
                    puts_(&o, " of ");
                    put(&o, s + b0, q - b0);
                    i = q + 1;
                    continue;
                }
            }
        }
        if (c == '%') {
            puts_(&o, o.n && last(&o) != ' ' ? " percent" : "percent");
            i++;
            continue;
        }
        if (c == '+' && (i == 0 || CH(i - 1) == ' ' || CH(i - 1) == '(') && CH(i + 1) == ' ') {
            puts_(&o, "plus");
            i++;
            continue;
        }
        if (c == '/' && i > 0 && CH(i - 1) == ' ' && CH(i + 1) == ' ') {
            puts_(&o, "or");
            i++;
            continue;
        }
        if (c == ' ') {   /* one space; none before a mark or at the end */
            while (CH(i + 1) == ' ')
                i++;
            if (!o.n || last(&o) == ' ' || !CH(i + 1) || strchr(",.:", CH(i + 1))) {
                i++;
                continue;
            }
        }
        if (c == '.' && o.n && strchr(",:?!.", last(&o))) {   /* "DELAY,." / "PATTERN?.": one mark */
            if (last(&o) == ',' || last(&o) == ':')
                o.o[o.n - 1] = '.';
            i++;
            continue;
        }
        {
            const char t = (char)(c < 0x20 ? ' ' : c);   /* (what the voice is given: no control characters) */
            put(&o, &t, 1);
        }
        i++;
    }
    o.o[o.n] = 0;
    return o.n;
}
