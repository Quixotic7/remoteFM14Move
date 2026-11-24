/* SPDX-License-Identifier: MPL-2.0 */
/* The recorder, the recordings and the looper on the Move (the owner's request of 2026-11-24): the mock-up's
 * renderer, round 2 (the sampler as an app, the kit on Track 4), written BEFORE any code. The screens with the core's own v2 primitives and font, the lights by the
 * proposal's rules. Writes rec2_<board>.pbm and .leds into the working directory (the core's test dump format: 4 pad
 * rows from the top, the 16 steps, the 8 knobs, the 16 icons, the 4 Track buttons) and one more row, the buttons:
 * sample record play loop mute delete copy undo.
 *   cc -std=c99 -I<core>/src -I<core>/third_party/moveremote/include mock_recorder_v2.c <core>/src/v2.c \
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
    snprintf(path, sizeof path, "rec2_%s.pbm", name);
    f = fopen(path, "w");
    fprintf(f, "P1\n128 64\n");
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 128; x++)
            fputc(b->cv.px[y][x] ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
    snprintf(path, sizeof path, "rec2_%s.leds", name);
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
/* the app's tabs, top right: the shown one long */
static void tabs(mr_canvas_t *cv, int cur)
{
    v2_R(cv, 106, cur == 0 ? 0 : 1, 9, cur == 0 ? 3 : 1, 1);
    v2_R(cv, 118, cur == 1 ? 0 : 1, 9, cur == 1 ? 3 : 1, 1);
}
/* the level now, a bar up the right edge, the threshold's mark across it */
static void meter(mr_canvas_t *cv, int level, int mark)
{
    v2_R(cv, 121, 44 - level, 6, level, 1);
    v2_dotV(cv, 124, 18, 26);
    if (mark >= 0)
        v2_R(cv, 117, 44 - mark, 11, 1, 1);
}
/* the settings' places, the chosen one long; then its value, the row inverted (edit: framed, between arrows) */
static void setting(mr_canvas_t *cv, int cur, const char *val, int edit)
{
    int i;
    for (i = 0; i < 7; i++)
        v2_R(cv, 3 + i * 18, i == cur ? 45 : 46, 12, i == cur ? 3 : 1, 1);
    if (!edit) {
        sel(cv, 49, val);
        return;
    }
    {
        const int w = v2_tw(val, 2), x = 64 - w / 2;
        v2_O(cv, 0, 49, 128, 15, 1);
        v2_R(cv, x - 10, 50, w + 20, 13, 1);
        v2_T(cv, x, 50, val, 2, 0);
        v2_T(cv, x - 8, 53, "<", 1, 0), v2_T(cv, x + w + 3, 53, ">", 1, 0);
    }
}
static void app_under(board_t *b, int s, rgb sample)
{
    note_layer(b, s);
    b->btn[B_SAMPLE] = sample;
}
static void kit_pads(board_t *b, int chosen)
{
    static const signed char HAS[16] = { 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0 };   /* from the bottom-left */
    int r, c, i;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            if (HAS[(3 - r) * 4 + c])
                b->pad[r * 8 + c] = sc(KIT, 90);
    if (chosen >= 0)
        b->pad[(3 - chosen / 4) * 8 + chosen % 4] = WHITE;
    for (i = 0; i < 3; i++)
        b->track[i] = sc(SLOT[i], 60);
    b->track[3] = KIT;
    for (i = 0; i < 16; i++)
        b->step[i] = i % 4 == 0 ? sc(KIT, 200) : i == 10 ? sc(KIT, 200) : sc(WHITE, 16);
    for (i = 0; i < 8; i++)
        b->knob[i] = sc(KIT, 120);
}

int main(void)
{
    static board_t b;
    mr_canvas_t *cv = &b.cv;

    /* A. the sampler app, Record: the source's sound now (wave, level), the jog on a setting */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    rec_line(cv, "Slot 2", NULL), tabs(cv, 0);
    wave(cv, 0, 116, 31, 12, 116, 31u);
    meter(cv, 17, 9);
    setting(cv, 1, "Bars", 0);
    dump(&b, "app");

    /* B. the jog's click: the setting edited */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    rec_line(cv, "Slot 2", NULL), tabs(cv, 0);
    wave(cv, 0, 116, 31, 12, 116, 31u);
    meter(cv, 12, 9);
    setting(cv, 2, "4 Bars", 1);
    dump(&b, "app_edit");

    /* C. the input: its level set in the app */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    rec_line(cv, "Input", NULL), tabs(cv, 0);
    wave(cv, 0, 116, 31, 7, 116, 77u);
    meter(cv, 21, 9);
    setting(cv, 4, "In +6 dB", 1);
    dump(&b, "app_input");

    /* D. Sample in the app: the take, its bars */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, RED);
    rec_line(cv, "Slot 2", "2/4");
    wave(cv, 0, 116, 34, 14, 52, 7u);
    v2_dotV(cv, 29, 20, 30), v2_dotV(cv, 58, 20, 30), v2_dotV(cv, 87, 20, 30);
    meter(cv, 19, -1);
    v2_strip(cv, 4, 1u << 1, 52.0 / 116.0, 0, 116);
    dump(&b, "app_rec");

    /* E. the app's other tab: the recordings, one a jog detent */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    v2_T(cv, 0, 1, "3/14", 2, 1), tabs(cv, 1);
    v2_T(cv, 0, 19, "4 Bars", 2, 1);
    v2_T(cv, 128 - v2_tw("120", 2), 19, "120", 2, 1);
    wave(cv, 0, 128, 44, 8, 128, 21u);
    v2_dotV(cv, 32, 36, 17), v2_dotV(cv, 64, 36, 17), v2_dotV(cv, 96, 36, 17);
    v2_T(cv, 0, 56, "ChoralRoot", 1, 1);
    v2_T(cv, 128 - v2_tw("14:32", 1), 56, "14:32", 1, 1);
    dump(&b, "app_takes");

    /* F. the kit on Track 4: a pad chosen, its sample */
    memset(&b, 0, sizeof b);
    kit_pads(&b, 4);
    v2_T(cv, 0, 1, "Pad 5", 2, 1);
    wave(cv, 0, 128, 34, 14, 86, 55u);
    v2_R(cv, 4, 18, 1, 34, 1), v2_R(cv, 4, 18, 4, 2, 1);
    v2_R(cv, 86, 18, 1, 34, 1), v2_R(cv, 83, 18, 4, 2, 1);
    v2_T(cv, 0, 56, "kick tape 02", 1, 1);
    dump(&b, "kit");

    /* G. the jog's click on a pad: its sample picked from a list, heard as the list moves */
    memset(&b, 0, sizeof b);
    kit_pads(&b, 4);
    v2_TC(cv, 64, 0, "Pad 5", 2, 1);
    v2_T(cv, 3, 20, "hat 08", 2, 1);
    sel(cv, 34, "kick tape");
    v2_T(cv, 3, 50, "snare 120", 2, 1);
    dump(&b, "kit_pick");
    return 0;
}
