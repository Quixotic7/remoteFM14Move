/* SPDX-License-Identifier: MPL-2.0 */
/* A firmware's screen text in words, for the screen reader protocol (charlesvestal/fm1-reader, PROTOCOL.md): its
 * words file ({"format":1,"words":[[screen, said], ...]}) first, then the common rules. A C port of fm1-reader's
 * web/words.js (MIT), rule for rule:
 *   - an entry matches whole words, capitals or not; "#" takes a number (digits, a sign, a decimal point) and says it
 *     ("#" in what is said, in order); "*" takes a word and drops it; the longest match wins, an earlier entry a tie;
 *   - then: 1/16, 1/8T, 1/8. (note lengths), 4:1 and 1-16 ("to"), 2/3 ("of"), C#3 ("C sharp 3"), x2 ("2 times"),
 *     "A" the article (not before MINOR, FLAT, ..), a word in capitals made an ordinary word, "(1/2)" (", page 1 of 2"),
 *     "%", " + ", " / ", one space, one mark.
 * A file is held to WORDS_MAX entries of at most WORD_MAX characters a side (the reader's bounds). No threads of its
 * own: a table is read-only once loaded. */
#ifndef RFM14_FMWORDS_H
#define RFM14_FMWORDS_H

#define FMW_WORDS_MAX 5000
#define FMW_WORD_MAX 120

typedef struct {
    int n;
    const char **k, **v;   /* the screen's text, what to say (into pool) */
    char *pool;
    unsigned char *first;  /* each key's first character, lower case (0: '#' or '*', any) */
} fmw_t;

/* a words file's JSON into w (empty on failure): 0, or -1 not a words file (format 1, at most FMW_WORDS_MAX entries) */
int fmw_load(fmw_t *w, const char *json);
void fmw_free(fmw_t *w);
/* s said (w NULL or empty: the common rules only) into out (cut to cap - 1); the length */
int fmw_spoken(const char *s, const fmw_t *w, char *out, int cap);

#endif
