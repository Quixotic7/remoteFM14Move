/* SPDX-License-Identifier: MPL-2.0 */
/* The recorder, the recordings and the looper on the Move (the owner's request of 2026-11-24): the mock-up's
 * renderer, round 3 (the sampler's screens after the owner's references: the Digitakt's and Tonverk's recorder,
 * trim and browser, the Torso S-4), written BEFORE any code. The screens with the core's own v2 primitives and font, the lights by the
 * proposal's rules. Writes rec3_<board>.pbm and .leds into the working directory (the core's test dump format: 4 pad
 * rows from the top, the 16 steps, the 8 knobs, the 16 icons, the 4 Track buttons) and one more row, the buttons:
 * sample record play loop mute delete copy undo.
 *   cc -std=c99 -I<core>/src -I<core>/third_party/moveremote/include mock_recorder_v3.c <core>/src/v2.c \
 *      <core>/third_party/moveremote/host/mr_gfx.c <core>/third_party/moveremote/common/font5x7.c -o mock && ./mock */
#include <stdio.h>
#include <string.h>
#include "v2.h"

typedef struct { int r, g, b; } rgb;
static const rgb SLOT[3] = { { 255, 120, 0 }, { 40, 120, 255 }, { 255, 200, 0 } };   /* orange, blue, yellow */
static const rgb WHITE = { 255, 255, 255 }, RED = { 255, 0, 0 }, GREEN = { 0, 255, 60 }, MENU = { 255, 72, 0 };
static const rgb INPUT = { 0, 220, 200 }, STEMS = { 190, 90, 255 };                  /* the input, a set of stems */
static rgb sc(rgb c, int f) { rgb o = { c.r * f / 256, c.g * f / 256, c.b * f / 256 }; return o; }
/* a take's source: 0 the master (white), 1..3 a slot, 4 the input, 5 stems */
static rgb src(int s) { return s == 0 ? WHITE : s <= 3 ? SLOT[s - 1] : s == 4 ? INPUT : STEMS; }

enum { B_SAMPLE, B_RECORD, B_PLAY, B_LOOP, B_MUTE, B_DELETE, B_COPY, B_UNDO, B_N };
typedef struct {
    mr_canvas_t cv;
    rgb pad[32];                                        /* 0 = the top-left pad */
    rgb step[16], knob[8], track[4], btn[B_N];
    int icon[16];                                       /* 0..127 */
} board_t;

static void row(FILE *f, const rgb *c, int n)
{
    int i;
    for (i = 0; i < n; i++)
        fprintf(f, "%02x%02x%02x ", c[i].r, c[i].g, c[i].b);
    fputc('\n', f);
}
static void dump(const board_t *b, const char *name)
{
    char path[96];
    FILE *f;
    int x, y, i;
    snprintf(path, sizeof path, "rec3_%s.pbm", name);
    f = fopen(path, "w");
    fprintf(f, "P1\n128 64\n");
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 128; x++)
            fputc(b->cv.px[y][x] ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
    snprintf(path, sizeof path, "rec3_%s.leds", name);
    f = fopen(path, "w");
    for (y = 0; y < 4; y++)
        row(f, b->pad + y * 8, 8);
    row(f, b->step, 16);
    row(f, b->knob, 8);
    for (i = 0; i < 16; i++)
        fprintf(f, "%02x ", b->icon[i]);
    fputc('\n', f);
    row(f, b->track, 4);
    row(f, b->btn, B_N);
    fclose(f);
}

/* ---- the layer under the recorder: a note layout of slot s (0..2), its clip's steps, the transport running */
static void note_layer(board_t *b, int s)
{
    int i;
    for (i = 0; i < 32; i++)
        b->pad[i] = sc(SLOT[s], (31 - i) % 7 == 3 ? 255 : 70);   /* (the roots full) */
    for (i = 0; i < 16; i++)
        b->step[i] = i % 4 == 0 || i == 6 || i == 11 ? sc(SLOT[s], 200) : sc(WHITE, 16);
    for (i = 0; i < 3; i++)
        b->track[i] = sc(SLOT[i], i == s ? 255 : 60);
    b->track[3] = sc(WHITE, 40);
    b->btn[B_PLAY] = GREEN;
}
static void track_view(mr_canvas_t *cv)                 /* (a stand-in for the track view's lower part) */
{
    v2_steps(cv, 0x0851 | 0x1111, -1, -1, -1);
    v2_T(cv, 0, 24, "Track 5", 2, 1);
    v2_T(cv, 0, 41, "1/16", 1, 1);
}

/* a take's levels as bars about the line y0 (h each way), from x0 over w pixels, the first `upto` of them drawn */
static void wave(mr_canvas_t *cv, int x0, int w, int y0, int h, int upto, unsigned seed)
{
    int x;
    for (x = 0; x < w; x += 2) {
        const int beat = (x * 16 / w) % 4;
        int a;
        seed = seed * 1103515245u + 12345u;
        a = 1 + (int)((seed >> 16) % (unsigned)h) * (beat == 0 ? 4 : beat == 2 ? 3 : 2) / 4;
        if (x >= upto)
            a = 0;
        v2_R(cv, x0 + x, y0 - a, 1, a ? a * 2 : 1, 1);
    }
}

/* ---- the top line while the recorder is armed or records: the wave icon, the source, the bars done / wanted */
static void rec_line(mr_canvas_t *cv, const char *source, const char *count)
{
    v2_ic(cv, V2I_WAVE, 0, 2, 1);
    v2_T(cv, 16, 1, source, 2, 1);
    if (count)
        v2_T(cv, 128 - v2_tw(count, 2), 1, count, 2, 1);
}
static void sel(mr_canvas_t *cv, int y, const char *s) { v2_R(cv, 0, y, 128, 15, 1), v2_T(cv, 3, y + 1, s, 2, 0); }
static void menu_knobs(board_t *b, int n, int touched)
{
    int i;
    for (i = 0; i < n; i++)
        b->knob[i] = sc(MENU, i == touched ? 255 : 90);
}

/* ---- the recordings' pads: n takes, the newest first (pad 1), each in its source's colour */
static const int TAKE_SRC[] = { 2, 2, 0, 4, 1, 1, 5, 0, 3, 2, 4, 0, 1, 2 };
#define TAKES ((int)(sizeof TAKE_SRC / sizeof *TAKE_SRC))
static void take_pads(board_t *b, int chosen, int playing)
{
    int i;
    for (i = 0; i < TAKES; i++)
        b->pad[i] = sc(src(TAKE_SRC[i]), 90);
    if (chosen >= 0)
        b->pad[chosen] = WHITE;
    if (playing >= 0)
        b->pad[playing] = GREEN;
    b->track[3] = sc(WHITE, 200);
    b->step[0] = sc(WHITE, 200);                        /* the filters: all, the master, the slots, the input */
    b->step[1] = sc(WHITE, 40);
    for (i = 0; i < 3; i++)
        b->step[2 + i] = sc(SLOT[i], 60);
    b->step[5] = sc(INPUT, 60);
    b->btn[B_DELETE] = sc(WHITE, 60), b->btn[B_COPY] = sc(WHITE, 60);
}

/* ---- the looper's pads: 8 columns of 4 loops; a loop in its source's colour, the playing ones green */
static const signed char LOOPS[4][8] = {                /* -1 empty, else the source */
    { 1, 2, 2, 0, 4, -1, -1, -1 },
    { 1, 2, -1, 0, -1, -1, -1, -1 },
    { -1, 2, -1, -1, -1, -1, -1, -1 },
    { -1, -1, -1, -1, -1, -1, -1, -1 },
};
static void loop_pads(board_t *b, int chosen, int recording)
{
    static const signed char PLAY[8] = { 0, 1, 0, -1, 0, -1, -1, -1 };   /* the row each column plays */
    int r, c;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 8; c++)
            if (LOOPS[r][c] >= 0)
                b->pad[r * 8 + c] = PLAY[c] == r ? GREEN : sc(src(LOOPS[r][c]), 90);
    if (chosen >= 0)
        b->pad[chosen] = WHITE;
    if (recording >= 0)
        b->pad[recording] = RED;
    for (c = 0; c < 8; c++) {
        const int has = LOOPS[0][c] >= 0;
        b->knob[c] = has ? sc(src(LOOPS[0][c]), 160) : sc(WHITE, 0);
        b->step[c * 2] = has ? sc(src(LOOPS[0][c]), 120) : sc(WHITE, 12);
        b->step[c * 2 + 1] = PLAY[c] >= 0 ? sc(WHITE, 90) : sc(WHITE, 0);
    }
    b->track[3] = sc(WHITE, 200);
    b->btn[B_PLAY] = GREEN;
    b->btn[B_RECORD] = sc(RED, 40);
    b->btn[B_DELETE] = sc(WHITE, 60), b->btn[B_COPY] = sc(WHITE, 60), b->btn[B_UNDO] = sc(WHITE, 60);
}

static const rgb KIT = { 255, 60, 160 };                 /* the kit on Track 4 */

/* the title, inverted, top left; returns the x after it */
static int title(mr_canvas_t *cv, const char *s)
{
    const int w = v2_tw(s, 1) + 3;
    v2_R(cv, 0, 0, w, 9, 1);
    v2_T(cv, 2, 1, s, 1, 0);
    return w + 3;
}
/* the level now along the top, from x0 to the right edge: the bar, the range a dotted line, the threshold's mark */
static void hmeter(mr_canvas_t *cv, int x0, int level, int mark)
{
    v2_dotH(cv, x0, 4, 128 - x0);
    v2_R(cv, x0, 2, level, 5, 1);
    if (mark >= 0)
        v2_R(cv, x0 + mark, 0, 2, 9, 1);
}
/* a cell of the 4 x 2 grid: its value over its name; st 0 plain, 1 the jog's (the value inverted), 2 edited (all of it) */
static void cell(mr_canvas_t *cv, int col, int y, const char *val, const char *name, int st)
{
    const int x = col * 32;
    if (st == 1)
        v2_R(cv, x + 1, y - 1, 30, 9, 1);
    else if (st == 2)
        v2_R(cv, x + 1, y - 1, 30, 18, 1);
    v2_TC(cv, x + 16, y, val, 1, st ? 0 : 1);
    v2_TC(cv, x + 16, y + 9, name, 1, st != 2);
}
/* a take's levels, the part kept (xa..xb) as a white block with the wave cut out of it, the rest plain */
static void wavec(mr_canvas_t *cv, int x0, int w, int y0, int h, unsigned seed, int xa, int xb)
{
    int x;
    if (xb > xa)
        v2_R(cv, xa, y0 - h - 1, xb - xa, 2 * h + 2, 1);
    for (x = 0; x < w; x += 2) {
        const int beat = (x * 16 / w) % 4, in = x0 + x >= xa && x0 + x < xb;
        int a;
        seed = seed * 1103515245u + 12345u;
        a = 1 + (int)((seed >> 16) % (unsigned)h) * (beat == 0 ? 4 : beat == 2 ? 3 : 2) / 4;
        v2_R(cv, x0 + x, y0 - a, 1, a * 2, in ? 0 : 1);
    }
}
/* a list: the folder's name across the top, six rows, the chosen one inverted, a tag at the right, the bar */
static void list(mr_canvas_t *cv, const char *folder, const char *const *row, const char *const *tag, int n, int cur,
                 int at, int of)
{
    int i;
    v2_R(cv, 0, 0, 128, 9, 1);
    v2_TC(cv, 64, 1, folder, 1, 0);
    for (i = 0; i < n && i < 6; i++) {
        const int y = 10 + i * 9;
        if (i == cur)
            v2_R(cv, 0, y, 122, 9, 1);
        v2_T(cv, 2, y + 1, row[i], 1, i != cur);
        if (tag && tag[i] && *tag[i])
            v2_T(cv, 120 - v2_tw(tag[i], 1), y + 1, tag[i], 1, i != cur);
    }
    v2_dotV(cv, 126, 10, 54);
    v2_R(cv, 125, 10 + 54 * at / of, 3, 54 * 6 / of > 6 ? 54 * 6 / of : 6, 1);
}
static void app_under(board_t *b, int s, rgb sample)
{
    note_layer(b, s);
    b->btn[B_SAMPLE] = sample;
}
static void recorder(mr_canvas_t *cv, int cur, int edit, int level)
{
    static const char *const V[8] = { "FW 2", "BARS", "4", "-24", "+0", "OFF", "FILES", "14" };
    static const char *const N[8] = { "SRC", "MODE", "LEN", "THRES", "IN", "MON", "TO", "TAKES" };
    int i;
    hmeter(cv, title(cv, "RECORDER"), level, 46);
    for (i = 0; i < 8; i++)
        cell(cv, i % 4, i < 4 ? 12 : 31, V[i], N[i], i == cur ? (edit ? 2 : 1) : 0);
    wave(cv, 0, 128, 56, 6, 128, 31u);
}

int main(void)
{
    static board_t b;
    mr_canvas_t *cv = &b.cv;
    int i;

    /* A. the recorder: the level on top, eight settings in a grid, the source's sound along the bottom */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    recorder(cv, 1, 0, 30);
    dump(&b, "rec");

    /* B. the jog's click: the cell edited */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    recorder(cv, 3, 1, 22);
    dump(&b, "rec_edit");

    /* C. Sample: the take as it grows */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, RED);
    i = title(cv, "REC");
    v2_T(cv, i, 1, "FW 2  2/4", 1, 1);
    hmeter(cv, 82, 26, -1);
    wave(cv, 0, 128, 33, 20, 58, 7u);
    v2_dotV(cv, 32, 11, 44), v2_dotV(cv, 64, 11, 44), v2_dotV(cv, 96, 11, 44);
    v2_strip(cv, 4, 1u << 1, 58.0 / 128.0, 0, 128);
    dump(&b, "rec_run");

    /* D. the take, trimmed: what is kept is the white block; start, end, hear it, save */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    i = title(cv, "TRIM");
    v2_T(cv, i, 1, "FW 2", 1, 1);
    v2_T(cv, 128 - v2_tw("1.72s", 1), 1, "1.72s", 1, 1);
    wavec(cv, 0, 128, 27, 15, 61u, 9, 101);
    cell(cv, 0, 46, "0.12", "START", 0);
    cell(cv, 1, 46, "1.84", "END", 2);
    cell(cv, 2, 46, ">", "PLAY", 0);
    cell(cv, 3, 46, "OK", "SAVE", 0);
    for (i = 0; i < 2; i++)
        b.knob[i] = sc(MENU, i == 1 ? 255 : 90);
    dump(&b, "trim");

    /* E. the takes: a list, the newest first */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    {
        static const char *const R[6] = { "14:32 ChoralRoot", "14:31 ChoralRoot", "14:28 Master", "14:20 Input",
                                          "14:17 FIZZ", "14:09 Stems" };
        static const char *const T[6] = { "4b", "4b", "8b", "12s", "2b", "4b" };
        list(cv, "TODAY", R, T, 6, 1, 0, 14);
    }
    dump(&b, "list");

    /* F. the folders above it */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    {
        static const char *const R[5] = { "Today", "23 Nov", "22 Nov", "Move library", "audio" };
        static const char *const T[5] = { "14", "6", "21", ">", ">" };
        list(cv, "SAMPLES", R, T, 5, 0, 0, 6);
    }
    dump(&b, "list_root");

    /* G. a take's menu */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    wave(cv, 0, 44, 12, 9, 44, 21u);
    v2_T(cv, 0, 27, "4 BARS", 1, 1);
    v2_T(cv, 0, 37, "120 BPM", 1, 1);
    v2_T(cv, 0, 47, "FW 2", 1, 1);
    v2_dotV(cv, 47, 0, 64);
    {
        static const char *const M[6] = { "PLAY", "TRIM", "TO KIT PAD", "TO LOOPER", "RENAME", "DELETE" };
        for (i = 0; i < 6; i++) {
            if (i == 2)
                v2_R(cv, 50, 2 + i * 10, 78, 10, 1);
            v2_T(cv, 53, 4 + i * 10, M[i], 1, i != 2);
        }
    }
    dump(&b, "menu");

    /* H. a kit pad: its sample, eight settings, a knob each */
    memset(&b, 0, sizeof b);
    {
        static const signed char HAS[16] = { 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0 };
        static const char *const V[8] = { "100", "C", "+0", "SHOT", "0.00", "0.41", "OFF", "0" };
        static const char *const N[8] = { "LEV", "PAN", "TUNE", "MODE", "STRT", "LEN", "DCY", "SEND" };
        int r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 4; c++)
                if (HAS[(3 - r) * 4 + c])
                    b.pad[r * 8 + c] = sc(KIT, 90);
        b.pad[2 * 8 + 0] = WHITE;
        for (i = 0; i < 3; i++)
            b.track[i] = sc(SLOT[i], 60);
        b.track[3] = KIT;
        for (i = 0; i < 16; i++)
            b.step[i] = i % 4 == 0 || i == 10 ? sc(KIT, 200) : sc(WHITE, 16);
        for (i = 0; i < 8; i++)
            b.knob[i] = sc(KIT, i == 4 ? 255 : 120);
        i = title(cv, "K05");
        v2_T(cv, i, 1, "kick tape 02", 1, 1);
        wavec(cv, 0, 128, 19, 8, 55u, 4, 88);
        for (i = 0; i < 8; i++)
            cell(cv, i % 4, i < 4 ? 30 : 48, V[i], N[i], i == 4);
    }
    dump(&b, "kit");
    return 0;
}
