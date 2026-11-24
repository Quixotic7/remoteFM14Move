/* SPDX-License-Identifier: MPL-2.0 */
/* The recorder, the recordings and the looper on the Move (the owner's request of 2026-11-24): the mock-up's
 * renderer, round 4 (the owner: the screen is very small, so icons, as little text as can be, one or three rows of
 * settings, the tabs above the first row, text in a popup while a value changes), written BEFORE any code. The screens with the core's own v2 primitives and font, the lights by the
 * proposal's rules. Writes rec4_<board>.pbm and .leds into the working directory (the core's test dump format: 4 pad
 * rows from the top, the 16 steps, the 8 knobs, the 16 icons, the 4 Track buttons) and one more row, the buttons:
 * sample record play loop mute delete copy undo.
 *   cc -std=c99 -I<core>/src -I<core>/third_party/moveremote/include mock_recorder_v4.c <core>/src/v2.c \
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
    snprintf(path, sizeof path, "rec4_%s.pbm", name);
    f = fopen(path, "w");
    fprintf(f, "P1\n128 64\n");
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 128; x++)
            fputc(b->cv.px[y][x] ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
    snprintf(path, sizeof path, "rec4_%s.leds", name);
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

/* ---- the sampler's own icons, 11 x 11 */
typedef const char *const icon_t[11];
static icon_t I_SRC = { "........##.", "....#...##.", ".....#..##.", "......#.##.", "#######.##.", "#######.##.", "......#.##.",
                        ".....#..##.", "....#...##.", "........##.", "..........." };
static icon_t I_MODE = { "###.....###", "#.........#", "#.........#", "#....#....#", "#...###...#", "#...###...#",
                         "#....#....#", "#.........#", "#.........#", "###.....###", "..........." };
static icon_t I_LEN = { "...........", "...........", "#.........#", "#.........#", "#.........#", "###########",
                        "#.........#", "#.........#", "#.........#", "...........", "..........." };
static icon_t I_THR = { "......##...", "......##...", "......##...", "####..##...", "####..##...", "####..##...",
                        "####..##...", "......##...", "......##...", "......##...", "..........." };
static icon_t I_IN = { "....###....", "....###....", "....###....", "....###....", "..#.###.#..", "..#.###.#..",
                       "..#.....#..", "...#####...", ".....#.....", ".....#.....", "...#####..." };
static icon_t I_MON = { "...#####...", "..#.....#..", ".#.......#.", ".#.......#.", ".#.......#.", "###.....###",
                        "###.....###", "###.....###", "###.....###", "...........", "..........." };
static icon_t I_TO = { ".....#.....", ".....#.....", ".....#.....", "..#..#..#..", "...#.#.#...", "....###....",
                       ".....#.....", "#.........#", "#.........#", "###########", "..........." };
static icon_t I_LIST = { "...........", "##..#######", "##..#######", "...........", "##..#######", "##..#######",
                         "...........", "##..#######", "##..#######", "...........", "..........." };
static icon_t I_DOT = { "...........", "...#####...", "..#######..", ".#########.", ".#########.", ".#########.",
                        ".#########.", ".#########.", "..#######..", "...#####...", "..........." };
static icon_t I_START = { "##.........", "#..........", "#....#.....", "#...##.....", "#..########", "#..########",
                          "#...##.....", "#....#.....", "#..........", "##.........", "..........." };
static icon_t I_END = { ".........##", "..........#", ".....#....#", ".....##...#", "########..#", "########..#",
                        ".....##...#", ".....#....#", "..........#", ".........##", "..........." };
static icon_t I_PLAY = { "...........", "..##.......", "..####.....", "..######...", "..########.", "..########.",
                         "..######...", "..####.....", "..##.......", "...........", "..........." };
static icon_t I_OK = { "...........", "..........#", ".........##", "........##.", ".......##..", "##....##...",
                       ".##..##....", "..####.....", "...##......", "...........", "..........." };
static icon_t I_X = { "...........", ".##.....##.", "..##...##..", "...##.##...", "....###....", "....###....",
                      "...##.##...", "..##...##..", ".##.....##.", "...........", "..........." };
static icon_t I_TRIM = { "##.......##", "#.........#", "#.........#", "#..#...#..#", "#.##...##.#", "#.##...##.#",
                         "#..#...#..#", "#.........#", "#.........#", "##.......##", "..........." };
static icon_t I_KIT = { "...........", "####..####.", "####..####.", "####..####.", "####..####.", "...........",
                        "####..####.", "####..####.", "####..####.", "####..####.", "..........." };
static icon_t I_LOOP = { "...#####...", "..#.....#..", ".#.......#.", ".#.......#.", ".#.......#.", ".#.....#####",
                         ".#......###.", "..#......#..", "...###.....", "...........", "..........." };
static icon_t I_DIR = { "...........", ".####......", "#....#####.", "#.........#", "#.........#", "#.........#",
                        "#.........#", "#.........#", "###########", "...........", "..........." };
static void ic(mr_canvas_t *cv, icon_t i, int x, int y, int v)
{
    int r, c;
    for (r = 0; r < 11; r++)
        for (c = 0; c < 11 && i[r][c]; c++)
            if (i[r][c] == '#')
                v2_px(cv, x + c, y + r, v);
}

/* the top line: the tabs as icons (the shown one underlined; `on`: the jog is on the tabs, it is a white block), the
 * source at the right as one sign */
static void top(mr_canvas_t *cv, int tab, int on, const char *src)
{
    static icon_t *const T[2] = { &I_DOT, &I_LIST };
    int i;
    for (i = 0; i < 2; i++) {
        const int x = 2 + i * 18;
        if (i == tab && on)
            v2_R(cv, x - 2, 0, 15, 14, 1);
        ic(cv, *T[i], x, 1, !(i == tab && on));
        if (i == tab && !on)
            v2_R(cv, x, 13, 11, 1, 1);
    }
    if (src)
        v2_T(cv, 128 - v2_tw(src, 2), 0, src, 2, 1);
}
/* a setting's row: its icon and its value, 15 pixels; chosen: a white row */
static void rowi(mr_canvas_t *cv, int y, icon_t i, const char *val, int chosen)
{
    if (chosen)
        v2_R(cv, 0, y, 128, 15, 1);
    ic(cv, i, 3, y + 2, !chosen);
    v2_T(cv, 20, y + 1, val, 2, !chosen);
}
/* where the jog is in the list: a mark a setting, the chosen one long */
static void marks(mr_canvas_t *cv, int n, int cur, int y)
{
    int i;
    for (i = 0; i < n; i++)
        v2_R(cv, 3 + i * 18, i == cur ? y - 1 : y, 12, i == cur ? 3 : 1, 1);
}
static void meter(mr_canvas_t *cv, int level, int mark)
{
    v2_R(cv, 121, 45 - level, 6, level, 1);
    v2_dotV(cv, 124, 17, 28);
    if (mark >= 0)
        v2_R(cv, 117, 45 - mark, 11, 1, 1);
}
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
static void app_under(board_t *b, int s, rgb sample)
{
    note_layer(b, s);
    b->btn[B_SAMPLE] = sample;
}
/* a take's row: its source's sign, its length, its wave as a small picture */
static void take(mr_canvas_t *cv, int y, const char *src, const char *len, unsigned seed, int chosen)
{
    if (chosen)
        v2_R(cv, 0, y, 128, 15, 1);
    v2_T(cv, 3, y + 1, src, 2, !chosen);
    v2_T(cv, 22, y + 1, len, 2, !chosen);
    if (chosen)
        wavec(cv, 99, 28, y + 7, 5, seed, 99, 127);
    else
        wave(cv, 99, 28, y + 7, 5, 28, seed);
}

int main(void)
{
    static board_t b;
    mr_canvas_t *cv = &b.cv;
    int i;

    /* A. one row: the source's sound and its level, and the one setting the jog is on */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    top(cv, 0, 0, "2");
    wave(cv, 0, 116, 31, 12, 116, 31u);
    meter(cv, 17, 9);
    marks(cv, 7, 1, 47);
    rowi(cv, 49, I_MODE, "Bars", 1);
    dump(&b, "one");

    /* B. three rows: no wave until a take runs; the level a line on top */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    top(cv, 0, 0, "2");
    v2_dotH(cv, 44, 7, 56), v2_R(cv, 44, 5, 30, 5, 1), v2_R(cv, 84, 2, 2, 11, 1);
    rowi(cv, 16, I_SRC, "Slot 2", 0);
    rowi(cv, 32, I_MODE, "Bars", 1);
    rowi(cv, 48, I_LEN, "4 Bars", 0);
    dump(&b, "three");

    /* C. scrolled up past the first setting: the jog is on the tabs */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    top(cv, 0, 1, "2");
    wave(cv, 0, 116, 31, 12, 116, 31u);
    meter(cv, 14, 9);
    marks(cv, 7, -1, 47);
    rowi(cv, 49, I_SRC, "Slot 2", 0);
    dump(&b, "tabs");

    /* D. the jog's click on a setting: its name and its value, the only text, while it changes */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    v2_titleBox(cv, "Threshold", "-24", 64, 128);
    v2_dotH(cv, 8, 58, 112), v2_R(cv, 8, 56, 40, 5, 1), v2_R(cv, 72, 52, 2, 12, 1);
    dump(&b, "popup");

    /* E. a take runs */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, RED);
    ic(cv, I_DOT, 2, 1, 1);
    v2_T(cv, 20, 0, "2/4", 2, 1);
    v2_T(cv, 128 - v2_tw("2", 2), 0, "2", 2, 1);
    wave(cv, 0, 116, 34, 15, 52, 7u);
    v2_dotV(cv, 29, 18, 34), v2_dotV(cv, 58, 18, 34), v2_dotV(cv, 87, 18, 34);
    meter(cv, 19, -1);
    v2_strip(cv, 4, 1u << 1, 52.0 / 116.0, 0, 116);
    dump(&b, "run");

    /* F. the takes: three rows, no names: the source, the length, the wave's picture */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    top(cv, 1, 0, NULL);
    take(cv, 16, "2", "4 Bars", 21u, 0);
    take(cv, 32, "2", "4 Bars", 77u, 1);
    take(cv, 48, "M", "8 Bars", 5u, 0);
    dump(&b, "takes");

    /* G. the jog's click on a take: what to do with it, as icons; the chosen one's name the only word */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    wavec(cv, 0, 128, 12, 10, 77u, 0, 0);
    {
        static icon_t *const A[5] = { &I_PLAY, &I_TRIM, &I_KIT, &I_LOOP, &I_X };
        for (i = 0; i < 5; i++) {
            const int x = 10 + i * 25;
            if (i == 1)
                v2_R(cv, x - 2, 26, 15, 15, 1);
            ic(cv, *A[i], x, 28, i != 1);
        }
    }
    v2_TC(cv, 64, 47, "Trim", 2, 1);
    dump(&b, "act");

    /* H. trim: what is kept is the white block; start, end, hear it, keep it; the value while a marker moves */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    v2_T(cv, 0, 0, "1.84 s", 2, 1);
    wavec(cv, 0, 128, 31, 13, 61u, 9, 101);
    {
        static icon_t *const A[4] = { &I_START, &I_END, &I_PLAY, &I_OK };
        for (i = 0; i < 4; i++) {
            const int x = 12 + i * 31;
            if (i == 1)
                v2_R(cv, x - 2, 48, 15, 15, 1);
            ic(cv, *A[i], x, 50, i != 1);
        }
    }
    for (i = 0; i < 2; i++)
        b.knob[i] = sc(MENU, i == 1 ? 255 : 90);
    dump(&b, "trim");

    /* I. a folder above the takes: a day */
    memset(&b, 0, sizeof b);
    app_under(&b, 1, WHITE);
    top(cv, 1, 0, NULL);
    rowi(cv, 16, I_DIR, "Today", 1);
    rowi(cv, 32, I_DIR, "23 Nov", 0);
    rowi(cv, 48, I_DIR, "22 Nov", 0);
    dump(&b, "days");

    /* J. a kit pad: its number, its sample; a touched knob says its name and value as everywhere */
    memset(&b, 0, sizeof b);
    {
        static const signed char HAS[16] = { 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0 };
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
            b.knob[i] = sc(KIT, 120);
        ic(cv, I_KIT, 2, 1, 1);
        v2_T(cv, 20, 0, "5", 2, 1);
        wavec(cv, 0, 128, 36, 16, 55u, 4, 88);
    }
    dump(&b, "kit");
    return 0;
}
