/* SPDX-License-Identifier: MPL-2.0 */
/* The recorder, the recordings and the looper on the Move (the owner's request of 2026-11-24): the mock-up's
 * renderer, written BEFORE any code. The screens with the core's own v2 primitives and font, the lights by the
 * proposal's rules. Writes rec_<board>.pbm and .leds into the working directory (the core's test dump format: 4 pad
 * rows from the top, the 16 steps, the 8 knobs, the 16 icons, the 4 Track buttons) and one more row, the buttons:
 * sample record play loop mute delete copy undo.
 *   cc -std=c99 -I<core>/src -I<core>/third_party/moveremote/include mock_recorder_v1.c <core>/src/v2.c \
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
    snprintf(path, sizeof path, "rec_%s.pbm", name);
    f = fopen(path, "w");
    fprintf(f, "P1\n128 64\n");
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 128; x++)
            fputc(b->cv.px[y][x] ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
    snprintf(path, sizeof path, "rec_%s.leds", name);
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

int main(void)
{
    static board_t b;
    mr_canvas_t *cv = &b.cv;

    /* A. Shift + Sample: the recorder's menu (a Shift menu: a row a knob, the jog selects, its click edits) */
    memset(&b, 0, sizeof b);
    note_layer(&b, 1);
    v2_TC(cv, 64, 0, "Recorder", 2, 1);
    sel(cv, 19, "Slot 2");
    v2_T(cv, 3, 35, "Bars", 2, 1);
    v2_T(cv, 3, 50, "4 Bars", 2, 1);
    menu_knobs(&b, 6, 0);
    b.btn[B_SAMPLE] = WHITE;
    dump(&b, "menu");

    /* B. the menu's level row: the source's level now, the threshold's mark on it */
    memset(&b, 0, sizeof b);
    note_layer(&b, 1);
    v2_TC(cv, 64, 0, "Recorder", 2, 1);
    sel(cv, 19, "-24 dB");
    v2_R(cv, 4, 42, 38, 12, 1);                         /* the level now */
    v2_dotH(cv, 4, 48, 120);
    v2_R(cv, 70, 38, 2, 20, 1);                         /* the threshold */
    menu_knobs(&b, 6, 3);
    b.btn[B_SAMPLE] = WHITE;
    dump(&b, "level");

    /* C. armed (Bars): the top line only, the view under it as it was; Sample blinks red */
    memset(&b, 0, sizeof b);
    note_layer(&b, 1);
    rec_line(cv, "Slot 2", "4");
    track_view(cv);
    b.btn[B_SAMPLE] = sc(RED, 120);
    dump(&b, "armed");

    /* D. recording, the top line (option 1): bars done / wanted */
    memset(&b, 0, sizeof b);
    note_layer(&b, 1);
    rec_line(cv, "Slot 2", "2/4");
    track_view(cv);
    b.btn[B_SAMPLE] = RED;
    dump(&b, "line");

    /* E. recording, the wave (option 2): the take's levels, the bar lines, the strip its bars */
    memset(&b, 0, sizeof b);
    note_layer(&b, 1);
    rec_line(cv, "Slot 2", "2/4");
    wave(cv, 0, 128, 38, 16, 54, 7u);
    v2_dotV(cv, 32, 20, 36), v2_dotV(cv, 64, 20, 36), v2_dotV(cv, 96, 20, 36);
    v2_strip(cv, 4, 1u << 1, 54.0 / 128.0, 0, 128);
    b.btn[B_SAMPLE] = RED;
    dump(&b, "wave");

    /* F. Capture: the last 4 bars kept, said for a second; Sample flashes green, then dim white (listening) */
    memset(&b, 0, sizeof b);
    note_layer(&b, 1);
    v2_msg(cv, "Captured", "4 Bars");
    b.btn[B_SAMPLE] = GREEN;
    dump(&b, "captured");

    /* G. the recordings: a take chosen */
    memset(&b, 0, sizeof b);
    take_pads(&b, 1, -1);
    v2_T(cv, 0, 1, "4 Bars", 2, 1);
    v2_T(cv, 128 - v2_tw("120", 2), 1, "120", 2, 1);
    wave(cv, 0, 128, 34, 14, 128, 21u);
    v2_dotV(cv, 32, 20, 30), v2_dotV(cv, 64, 20, 30), v2_dotV(cv, 96, 20, 30);
    v2_T(cv, 0, 55, "ChoralRoot", 1, 1);
    v2_T(cv, 128 - v2_tw("14:32", 1), 55, "14:32", 1, 1);
    dump(&b, "takes");

    /* H. the recordings: it plays (the pad green, the place a line over the wave) */
    memset(&b, 0, sizeof b);
    take_pads(&b, -1, 1);
    v2_ic(cv, V2I_PLAY, 0, 2, 1);
    v2_T(cv, 16, 1, "2/4", 2, 1);
    v2_T(cv, 128 - v2_tw("120", 2), 1, "120", 2, 1);
    wave(cv, 0, 128, 34, 14, 128, 21u);
    v2_dotV(cv, 32, 20, 30), v2_dotV(cv, 64, 20, 30), v2_dotV(cv, 96, 20, 30);
    v2_R(cv, 47, 18, 1, 34, 1), v2_R(cv, 46, 18, 3, 2, 1);
    v2_T(cv, 0, 55, "ChoralRoot", 1, 1);
    v2_T(cv, 128 - v2_tw("14:32", 1), 55, "14:32", 1, 1);
    dump(&b, "takes_play");

    /* I. the looper: 8 columns of 4 loops, a loop chosen */
    memset(&b, 0, sizeof b);
    loop_pads(&b, 9, -1);
    v2_T(cv, 0, 1, "Loop 2", 2, 1);
    v2_T(cv, 128 - v2_tw("3/4", 2), 1, "3/4", 2, 1);
    wave(cv, 0, 128, 36, 14, 128, 99u);
    v2_dotV(cv, 32, 20, 34), v2_dotV(cv, 64, 20, 34), v2_dotV(cv, 96, 20, 34);
    v2_strip(cv, 4, 1u << 2, 78.0 / 128.0, 0, 128);
    dump(&b, "looper");

    /* J. the looper: an empty pad pressed, its loop records (the pad red), from the recorder's source */
    memset(&b, 0, sizeof b);
    loop_pads(&b, -1, 10);
    rec_line(cv, "Loop 3", "1/4");
    wave(cv, 0, 128, 36, 14, 30, 5u);
    v2_dotV(cv, 32, 20, 34), v2_dotV(cv, 64, 20, 34), v2_dotV(cv, 96, 20, 34);
    v2_strip(cv, 4, 1u << 0, 30.0 / 128.0, 0, 128);
    b.btn[B_SAMPLE] = RED;
    dump(&b, "looper_rec");

    /* K. the looper: Record is the overdub into the chosen loop */
    memset(&b, 0, sizeof b);
    loop_pads(&b, 9, -1);
    v2_ic(cv, V2I_OVERDUB, 0, 2, 1);
    v2_T(cv, 16, 1, "Loop 2", 2, 1);
    v2_T(cv, 128 - v2_tw("x2", 2), 1, "x2", 2, 1);
    wave(cv, 0, 128, 36, 16, 128, 99u);
    v2_dotV(cv, 32, 20, 34), v2_dotV(cv, 64, 20, 34), v2_dotV(cv, 96, 20, 34);
    v2_strip(cv, 4, 1u << 1, 40.0 / 128.0, 0, 128);
    b.btn[B_RECORD] = RED;
    b.pad[9] = RED;
    dump(&b, "looper_dub");
    return 0;
}
