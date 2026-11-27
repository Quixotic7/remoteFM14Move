/* SPDX-License-Identifier: MPL-2.0 */
/* The looper (Track 4's fourth view, and the sampler app's Loops tab): the mock-up's renderer, round 1, written BEFORE
 * any code (a proposal: the owner answers it card by card on the design page). Drawn with the core's own v2
 * primitives and font, in the kit's look as released in 0.6.0 (the jog turns pages, a page is a picture, the page
 * switcher's 2x icons, a knob's name and value in a full-screen popup, no boxes; the sampler's tabs and rows for the
 * Loops tab). Writes loop1_<board>.pbm and .leds into the working directory (the kit mock's dump format: 4 pad rows
 * from the top, the 16 steps, the 8 knobs, the 16 icons, the 4 Track buttons, then the buttons sample record play
 * loop mute delete copy undo).
 *   cc -std=c99 -I<core>/src -I<core>/third_party/moveremote/include mock_looper_v1.c <core>/src/v2.c \
 *      <core>/third_party/moveremote/host/mr_gfx.c <core>/third_party/moveremote/common/font5x7.c -o mock && ./mock
 * (<core>: rfm1cs-abletonmove 0.16.0) */
#include <stdio.h>
#include <string.h>
#include "v2.h"

typedef struct { int r, g, b; } rgb;
static const rgb SLOT[3] = { { 255, 120, 0 }, { 40, 120, 255 }, { 255, 200, 0 } };   /* orange, blue, yellow */
static const rgb WHITE = { 255, 255, 255 }, RED = { 255, 0, 0 }, GREEN = { 0, 255, 60 };
static const rgb KIT = { 255, 60, 160 };
static const rgb LOOP = { 0, 210, 190 };               /* the looper's colour (a proposal: teal, card lp_colour) */
static rgb sc(rgb c, int f) { rgb o = { c.r * f / 256, c.g * f / 256, c.b * f / 256 }; return o; }

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
    snprintf(path, sizeof path, "loop1_%s.pbm", name);
    f = fopen(path, "w");
    fprintf(f, "P1\n128 64\n");
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 128; x++)
            fputc(b->cv.px[y][x] ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
    snprintf(path, sizeof path, "loop1_%s.leds", name);
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

/* ---- icons: the sampler's and the kit's as built (0.16.0), and the looper's own in their style */
typedef const char *const icon_t[11];
static icon_t I_SRC = { "........##.", "....#...##.", ".....#..##.", "......#.##.", "#######.##.", "#######.##.",
                        "......#.##.", ".....#..##.", "....#...##.", "........##.", "..........." };
static icon_t I_LEN = { "...........", "...........", "#.........#", "#.........#", "#.........#", "###########",
                        "#.........#", "#.........#", "#.........#", "...........", "..........." };
static icon_t I_LIST = { "...........", "##..#######", "##..#######", "...........", "##..#######", "##..#######",
                         "...........", "##..#######", "##..#######", "...........", "..........." };
static icon_t I_DOT = { "...........", "...#####...", "..#######..", ".#########.", ".#########.", ".#########.",
                        ".#########.", ".#########.", "..#######..", "...#####...", "..........." };
static icon_t I_TRIM = { "##.......##", "#.........#", "#.........#", "#..#...#..#", "#.##...##.#", "#.##...##.#",
                         "#..#...#..#", "#.........#", "#.........#", "##.......##", "..........." };
static icon_t I_LOOP = { "...#####...", "..#.....#..", ".#.......#.", ".#.......#.", ".#.......#.", ".#.....####",
                         ".#......###", "..#......#.", "...###.....", "...........", "..........." };
static icon_t I_PLAY = { "...........", "..##.......", "..####.....", "..######...", "..########.", "..########.",
                         "..######...", "..####.....", "..##.......", "...........", "..........." };
static icon_t I_KIT = { "...........", "####..####.", "####..####.", "####..####.", "####..####.", "...........",
                        "####..####.", "####..####.", "####..####.", "####..####.", "..........." };
static icon_t I_IN = { "....###....", "....###....", "....###....", "....###....", "..#.###.#..", "..#.###.#..",
                       "...#...#...", "....###....", ".....#.....", "...#####...", "..........." };
/* the kit's page icons used here: Pad Mix's fader, Kit Mix's faders */
static icon_t PI_MIX = { ".....#.....", ".....#.....", ".....#.....", "...#####...", "...#####...", ".....#.....",
                         ".....#.....", ".....#.....", ".....#.....", ".....#.....", ".....#....." };
static icon_t PI_MIXES = { ".#...#...#.", ".#...#...#.", "###..#...#.", "###..#...#.", ".#...#..###", ".#..###.###",
                           ".#..###..#.", ".#...#...#.", ".#...#...#.", ".#...#...#.", ".#...#...#." };
/* the looper's own: Loops (the lanes as bars of different lengths), Record (the dot over a bar), Routing's arrows */
static icon_t PI_LANES = { "...........", "#######....", "#######....", "...........", "##########.", "##########.",
                           "...........", "####.......", "####.......", "...........", "..........." };
static icon_t PI_REC = { "...........", "...###.....", "..#####....", "..#####....", "...###.....", "...........",
                         "#.........#", "#.........#", "###########", "#.........#", "#.........#" };
static icon_t I_ROUTE = { "...........", "#####......", "....#......", "....#...#..", "....######.", "....#...#..",
                          "....#......", "#####......", "...........", "...........", "..........." };

static void ic(mr_canvas_t *cv, icon_t i, int x, int y, int v)
{
    int r, c;
    for (r = 0; r < 11; r++)
        for (c = 0; c < 11 && i[r][c]; c++)
            if (i[r][c] == '#')
                v2_px(cv, x + c, y + r, v);
}
static void ic2(mr_canvas_t *cv, icon_t i, int x, int y, int v)   /* an icon at 2x */
{
    int r, c;
    for (r = 0; r < 11; r++)
        for (c = 0; c < 11 && i[r][c]; c++)
            if (i[r][c] == '#')
                v2_R(cv, x + 2 * c, y + 2 * r, 2, 2, v);
}
static void icon_row(mr_canvas_t *cv, icon_t *const *icons, int n, int chosen, const char *name, int y)
{
    int i;
    for (i = 0; i < n; i++) {
        const int x = 64 - n * 25 / 2 + 7 + i * 25;
        if (i == chosen)
            v2_R(cv, x - 2, y - 2, 15, 15, 1);
        ic(cv, *icons[i], x, y, i != chosen);
    }
    if (name)
        v2_TC(cv, 64, y + 19, name, 2, 1);
}
static void rowi(mr_canvas_t *cv, int y, icon_t i, const char *val, int chosen)
{
    if (chosen)
        v2_R(cv, 0, y, 128, 15, 1);
    ic(cv, i, 3, y + 2, !chosen);
    v2_T(cv, 20, y + 1, val, 2, !chosen);
}

/* ---- the demo set: 4 lanes (pad rows, lane 1 the top row, beside Track 1) x 8 scenes (columns) */
static const signed char HAS[4][8] = { { 1, 1, 1, 0, 0, 0, 0, 0 }, { 1, 1, 0, 1, 0, 0, 0, 0 }, { 1, 0, 1, 0, 0, 0, 0, 0 },
                                       { 0, 0, 0, 0, 0, 0, 0, 0 } };
static const signed char BARS[4][8] = { { 4, 4, 2, 0 }, { 2, 2, 0, 8 }, { 1, 0, 4, 0 }, { 0 } };
static const signed char PLAYS[4] = { 0, 1, 0, -1 };   /* the scene each lane plays (-1: stopped) */
static int pi(int lane, int sc) { return lane * 8 + sc; }
enum { CH_NONE = -1 };
/* the grid's lights: a loop dim in the looper's colour, playing green, the chosen one white, empty dark */
static void grid_pads(board_t *b, int cl, int cs)
{
    int l, s;
    for (l = 0; l < 4; l++)
        for (s = 0; s < 8; s++)
            if (HAS[l][s])
                b->pad[pi(l, s)] = PLAYS[l] == s ? GREEN : sc(LOOP, 90);
    if (cl >= 0)
        b->pad[pi(cl, cs)] = WHITE;
}
/* the Looper view's common lights: Track 4 the looper's, the scene steps (odd: a scene, its icon dark), step 16 Stop
 * All (a function: its icon lit), the page's knobs */
static void loop_view(board_t *b, int cl, int cs, int knobs, int playing)
{
    int i;
    grid_pads(b, cl, cs);
    for (i = 0; i < 3; i++)
        b->track[i] = sc(SLOT[i], 60);
    b->track[3] = LOOP;
    for (i = 0; i < 16; i += 2)
        b->step[i] = sc(WHITE, 50);
    b->step[15] = sc(RED, 140), b->icon[15] = 127;
    for (i = 0; i < 8; i++)
        if (knobs >> i & 1)
            b->knob[i] = sc(LOOP, 150);
    b->btn[B_PLAY] = playing ? GREEN : sc(GREEN, 50);
    b->btn[B_SAMPLE] = sc(WHITE, 60), b->btn[B_COPY] = sc(WHITE, 60), b->btn[B_DELETE] = sc(WHITE, 60);
    b->btn[B_UNDO] = sc(WHITE, 60);
}

/* a wave about y0 (h each way) from x0 over w px (a column every 2 px) */
static void wave(mr_canvas_t *cv, int x0, int w, int y0, int h, unsigned seed, int dim_from)
{
    int x;
    for (x = 0; x < w; x += 2) {
        const int beat = (x * 16 / (w > 0 ? w : 1)) % 4;
        int a;
        seed = seed * 1103515245u + 12345u;
        a = 1 + (int)((seed >> 16) % (unsigned)h) * (beat == 0 ? 4 : beat == 2 ? 3 : 2) / 4;
        if (a > h)
            a = h;
        if (dim_from >= 0 && x0 + x >= dim_from) {      /* (past the playhead: dotted, the part to come) */
            int yy;
            for (yy = y0 - a; yy < y0 + a; yy += 2)
                v2_px(cv, x0 + x, yy, 1);
        } else
            v2_R(cv, x0 + x, y0 - a, 1, a * 2, 1);
    }
}
/* the slot's place: the 8 x 4 grid in small (3 x 2 cells, a px apart), the chosen cell solid, a loop a dot */
static void locator(mr_canvas_t *cv, int x0, int y0, int cl, int cs)
{
    int l, s;
    for (l = 0; l < 4; l++)
        for (s = 0; s < 8; s++) {
            const int x = x0 + s * 4, y = y0 + l * 3;
            if (l == cl && s == cs)
                v2_R(cv, x, y, 3, 2, 1);
            else if (HAS[l][s])
                v2_px(cv, x + 1, y, 1), v2_px(cv, x + 1, y + 1, 1);
        }
}
/* the bars of a loop: n cells over w px at y (h tall), the first k filled, the playhead's bar half */
static void bars(mr_canvas_t *cv, int x0, int w, int y, int h, int n, int k)
{
    int i;
    for (i = 0; i < n; i++) {
        const int a = x0 + i * w / n, e = x0 + (i + 1) * w / n - 2;
        if (i < k)
            v2_R(cv, a, y, e - a, h, 1);
        else
            v2_O(cv, a, y, e - a, h, 1);
    }
}
/* the Loops page: a lane a row (16 px): the scene playing at 2x, then the loop's bars, filled up to the playhead.
 * state: 0 playing, 1 queued (dashed), 2 stopped (a line), 3 recording (the dot, the wave growing) */
static void lane_row(mr_canvas_t *cv, int lane, int scene, int state, int n, double pos)
{
    const int y = lane * 16, x0 = 18, w = 110;
    char s[4];
    if (state == 3)
        ic(cv, I_DOT, 2, y + 2, 1);
    else if (state != 2) {
        if (scene < 0)
            snprintf(s, sizeof s, "-");
        else
            snprintf(s, sizeof s, "%d", scene + 1);
        v2_T(cv, 2, y + 1, s, 2, 1);
    }
    if (state == 2) {
        v2_dotH(cv, x0, y + 7, w);
        return;
    }
    if (state == 1) {
        v2_dashO(cv, x0, y + 2, w - 2, 11);
        return;
    }
    if (state == 3) {
        wave(cv, x0, (int)(w * pos), y + 7, 6, 31u + lane, -1);
        v2_dotH(cv, x0 + (int)(w * pos), y + 7, w - (int)(w * pos));
        return;
    }
    {
        int i;
        const int fill = (int)(w * pos);
        for (i = 0; i < n; i++) {                       /* bar ticks above, the progress as a solid bar */
            const int a = x0 + i * w / n;
            v2_R(cv, a, y + 2, 1, 11, 1);
        }
        v2_R(cv, x0 + w - 1, y + 2, 1, 11, 1);
        v2_R(cv, x0, y + 5, fill, 5, 1);
        v2_R(cv, x0, y + 7, w, 1, 1);
    }
}
/* the page switcher, as the kit's (2x icons in 28 px cells, the shown page a white cell, its name under) */
#define PG_CELL 28
static icon_t *const PG_ICON[5] = { &PI_LANES, &I_LOOP, &PI_REC, &PI_MIX, &PI_MIXES };
static const char *const PG_NAME[5] = { "Loops", "Loop", "Record", "Loop Mix", "Looper Mix" };
static void page_switch(mr_canvas_t *cv, int pg)
{
    const int y = 7;
    int i;
    for (i = 0; i < 5; i++) {
        const int x = 64 - PG_CELL / 2 + (i - pg) * PG_CELL;
        if (x <= -PG_CELL || x >= 128)
            continue;
        if (i == pg)
            v2_R(cv, x, y, PG_CELL, PG_CELL, 1);
        ic2(cv, *PG_ICON[i], x + 3, y + 3, i != pg);
    }
    v2_TC(cv, 64, y + PG_CELL + 6, PG_NAME[pg], 2, 1);
}
/* the sampler app's tabs: Record, Takes, Tools, Loops (the shown one underlined) */
static void tabs(mr_canvas_t *cv, int tab)
{
    static icon_t *const T[4] = { &I_DOT, &I_LIST, &I_TRIM, &I_LOOP };
    int i;
    for (i = 0; i < 4; i++) {
        const int x = 2 + i * 18;
        ic(cv, *T[i], x, 1, 1);
        if (i == tab)
            v2_R(cv, x, 13, 11, 1, 1);
    }
}
/* the Loops tab's grid: 8 x 4 cells of 13 x 7, 3 px apart, lanes from y0: a loop an outline, playing solid with its
 * progress, queued dashed, recording a solid cell with a black dot, empty a dot; the jog's cell underlined */
static void tab_grid(mr_canvas_t *cv, int y0, int cl, int cs, int rec_l, int rec_s, int q_l, int q_s)
{
    int l, s;
    for (l = 0; l < 4; l++)
        for (s = 0; s < 8; s++) {
            const int x = s * 16 + 1, y = y0 + l * 8;
            if (l == rec_l && s == rec_s) {
                v2_R(cv, x, y, 13, 6, 1);
                v2_R(cv, x + 5, y + 2, 3, 2, 0);
            } else if (l == q_l && s == q_s)
                v2_dashO(cv, x, y, 13, 6);
            else if (HAS[l][s] && PLAYS[l] == s)
                v2_R(cv, x, y, 13, 6, 1), v2_R(cv, x + 9, y + 2, 3, 2, 0);
            else if (HAS[l][s])
                v2_O(cv, x, y, 13, 6, 1);
            else
                v2_px(cv, x + 6, y + 3, 1);
            if (l == cl && s == cs)
                v2_R(cv, x, y + 7, 13, 1, 1);
        }
}

int main(void)
{
    static board_t b;
    mr_canvas_t *cv = &b.cv;
    int i;

    /* A. the Looper view at rest, the Loops page: a lane a row, what each plays and where it is */
    memset(&b, 0, sizeof b);
    loop_view(&b, 1, 1, 0x0F, 1);
    b.step[0] = GREEN, b.step[2] = GREEN;
    lane_row(cv, 0, 0, 0, 4, 0.62);
    lane_row(cv, 1, 1, 0, 2, 0.24);
    lane_row(cv, 2, 0, 0, 1, 0.48);
    lane_row(cv, 3, 0, 2, 0, 0);
    dump(&b, "loops");

    /* B. the jog turning: the page switcher (Loops, Loop, Record, Loop Mix, Looper Mix) */
    memset(&b, 0, sizeof b);
    loop_view(&b, 1, 1, 0x3F, 1);
    page_switch(cv, 1);
    dump(&b, "switch");

    /* C. the Loop page: the chosen loop (lane 2, scene 2) over its bars, the playhead, its place top right */
    memset(&b, 0, sizeof b);
    loop_view(&b, 1, 1, 0x3F, 1);
    wave(cv, 0, 128, 31, 13, 77u, 46);
    v2_R(cv, 46, 16, 1, 31, 1);                          /* (the playhead) */
    v2_R(cv, 0, 45, 1, 4, 1);
    bars(cv, 0, 130, 53, 9, 2, 0);
    v2_R(cv, 2, 55, 44, 5, 1);                          /* (the bar it plays: filled so far) */
    locator(cv, 96, 0, 1, 1);
    dump(&b, "loop");

    /* D. knob 3 turned on the Loop page: Pitch (a bar from the middle: repitched, the tempo kept) */
    memset(&b, 0, sizeof b);
    loop_view(&b, 1, 1, 0x3F, 1);
    b.knob[2] = WHITE;
    v2_titleBox(cv, "Pitch", "+2 st", 64, 128);
    v2_vbar(cv, 2.0 / 24, 1, 8, 112, 55);
    dump(&b, "pitch");

    /* E. knob 2: Length (the bars it plays, of the 2 recorded: halve to 1, 1/2, 1/4 ..) */
    memset(&b, 0, sizeof b);
    loop_view(&b, 1, 1, 0x3F, 1);
    b.knob[1] = WHITE;
    v2_titleBox(cv, "Length", "1 Bar", 64, 128);
    bars(cv, 8, 114, 54, 7, 2, 1);
    dump(&b, "length");

    /* F. knob 1: Start (the loop's start moved by steps, still bar-locked: the playhead's phase shifts) */
    memset(&b, 0, sizeof b);
    loop_view(&b, 1, 1, 0x3F, 1);
    b.knob[0] = WHITE;
    v2_titleBox(cv, "Start", "+3 Steps", 64, 128);
    for (i = 0; i <= 16; i++)
        v2_R(cv, 4 + i * 120 / 16, 52, 1, i % 4 == 0 ? 5 : 3, 1);
    v2_R(cv, 4 + 3 * 120 / 16, 58, 120 - 3 * 120 / 16, 4, 1);
    dump(&b, "start");

    /* G. an empty slot chosen (Shift + pad: lane 4, scene 1): the Record page, the lane's Source and Length */
    memset(&b, 0, sizeof b);
    loop_view(&b, 3, 0, 0x03, 1);
    b.step[0] = GREEN;
    ic2(cv, PI_REC, 2, 2, 1);
    locator(cv, 96, 0, 3, 0);
    rowi(cv, 31, I_SRC, "Track 2", 0);
    rowi(cv, 48, I_LEN, "Open", 0);
    dump(&b, "recpage");

    /* H. knob 1 on the Record page: Source (Track 2 lights bright: the firmware it records) */
    memset(&b, 0, sizeof b);
    loop_view(&b, 3, 0, 0x03, 1);
    b.knob[0] = WHITE;
    b.track[1] = SLOT[1];
    v2_titleBox(cv, "Source", "Track 2", 64, 128);
    dump(&b, "source");

    /* I. an empty slot pressed while playing: armed for the next bar (the pad blinking red), the beats to go */
    memset(&b, 0, sizeof b);
    loop_view(&b, CH_NONE, 0, 0x0F, 1);
    b.pad[pi(3, 0)] = sc(RED, 150);
    b.btn[B_SAMPLE] = sc(RED, 150);
    ic(cv, I_DOT, 2, 1, 1);
    ic(cv, I_SRC, 18, 1, 1);
    v2_T(cv, 32, 0, "2", 2, 1);
    for (i = 0; i < 4; i++) {                            /* (beat 3 of 4: the beats gone solid, the rest outlined) */
        if (i < 2)
            v2_R(cv, 8 + i * 30, 26, 22, 22, 1);
        else
            v2_O(cv, 8 + i * 30, 26, 22, 22, 1);
    }
    locator(cv, 96, 0, 3, 0);
    dump(&b, "armed");

    /* J. recording (Open): the dot, the bars so far at 4x, the wave growing over the bar being recorded, the level */
    memset(&b, 0, sizeof b);
    loop_view(&b, CH_NONE, 0, 0x0F, 1);
    b.pad[pi(3, 0)] = RED;
    b.btn[B_SAMPLE] = RED;
    ic(cv, I_DOT, 2, 1, 1);
    ic(cv, I_SRC, 18, 1, 1);
    v2_T(cv, 32, 0, "2", 2, 1);
    v2_TC(cv, 64, 0, "3", 2, 1);                        /* (the bar being recorded: the third) */
    for (i = 0; i < 4; i++)                             /* (the beat: a mark a beat, the one now solid) */
        v2_R(cv, 84 + i * 6, i == 1 ? 3 : 5, 4, i == 1 ? 6 : 2, 1);
    wave(cv, 0, 74, 36, 14, 41u, -1);
    v2_dotH(cv, 74, 36, 40);
    v2_R(cv, 121, 45 - 20, 6, 20, 1), v2_dotV(cv, 124, 17, 28);
    bars(cv, 0, 116, 56, 6, 3, 2);
    dump(&b, "recording");

    /* K. Fixed length (2 bars) recording: the bars to fill, the second running; it ends by itself and plays */
    memset(&b, 0, sizeof b);
    loop_view(&b, CH_NONE, 0, 0x0F, 1);
    b.pad[pi(3, 0)] = RED;
    b.btn[B_SAMPLE] = RED;
    ic(cv, I_DOT, 2, 1, 1);
    ic(cv, I_SRC, 18, 1, 1);
    v2_T(cv, 32, 0, "2", 2, 1);
    v2_TC(cv, 70, 0, "2/2", 2, 1);
    wave(cv, 0, 84, 36, 14, 43u, -1);
    v2_dotH(cv, 84, 36, 30);
    v2_R(cv, 121, 45 - 14, 6, 14, 1), v2_dotV(cv, 124, 17, 28);
    bars(cv, 0, 116, 56, 6, 2, 1);
    dump(&b, "fixed");

    /* L. a scene launched while playing (step 5: scene 3): what waits flashes green; the lanes that will change
     * dashed, a beat count to the bar */
    memset(&b, 0, sizeof b);
    loop_view(&b, CH_NONE, 0, 0x0F, 1);
    b.step[0] = GREEN;
    b.step[4] = sc(GREEN, 140);                          /* (blinking: waiting) */
    b.pad[pi(0, 2)] = sc(GREEN, 140), b.pad[pi(2, 2)] = sc(GREEN, 140);   /* (blinking green: lined up) */
    b.pad[pi(1, 1)] = sc(LOOP, 160);                     /* (blinking in its colour: about to stop) */
    lane_row(cv, 0, 2, 1, 2, 0);
    lane_row(cv, 1, -1, 1, 2, 0);
    lane_row(cv, 2, 2, 1, 4, 0);
    lane_row(cv, 3, 0, 2, 0, 0);
    {                                                    /* (in each dashed lane: the beats to the bar, 3 gone of 4) */
        int l;
        for (l = 0; l < 3; l++)
            for (i = 0; i < 4; i++)
                v2_R(cv, 26 + i * 8, l * 16 + 6, 4, 3, i < 3 ? 1 : 0);
    }
    dump(&b, "pending");

    /* M. the Loop Mix page (as the kit's Pad Mix): the wave, the level, the pan, three sends; no text */
    memset(&b, 0, sizeof b);
    loop_view(&b, 1, 1, 0x3F, 1);
    wave(cv, 0, 82, 32, 13, 77u, -1);
    v2_O(cv, 88, 4, 12, 50, 1);
    v2_R(cv, 90, 52 - 34, 8, 34, 1);
    v2_R(cv, 87, 60, 14, 1, 1);
    v2_R(cv, 91, 57, 2, 5, 1);
    for (i = 0; i < 3; i++) {
        v2_O(cv, 105, 8 + i * 16, 22, 7, 1);
        v2_R(cv, 107, 10 + i * 16, (i == 0 ? 40 : i == 1 ? 0 : 15) * 18 / 100, 3, 1);
    }
    dump(&b, "loopmix");

    /* N. the Looper Mix page: the four lanes' levels (knobs 1-4), then the Loops strip (the Mixer's column 5):
     * its level, pan, sends (knobs 5-8) */
    memset(&b, 0, sizeof b);
    loop_view(&b, 1, 1, 0xFF, 1);
    for (i = 0; i < 4; i++) {
        const int x = 4 + i * 18, h = i == 3 ? 0 : 20 + i * 7;
        v2_R(cv, x, 56, 12, 1, 1);
        v2_R(cv, x + 3, 56 - h, 6, h, 1);
        if (i < 3)
            v2_R(cv, x + 1, 56 - h - 4, 10, 2, 1);      /* (a cap: the lane sounds) */
        if (i == 1)
            v2_R(cv, x + 1, 60, 10, 2, 1);              /* (the chosen loop's lane) */
    }
    v2_dotV(cv, 78, 2, 60);
    v2_O(cv, 84, 4, 12, 50, 1);
    v2_R(cv, 86, 52 - 40, 8, 40, 1);
    v2_R(cv, 83, 60, 14, 1, 1);
    v2_R(cv, 89, 57, 2, 5, 1);
    for (i = 0; i < 3; i++) {
        v2_O(cv, 102, 8 + i * 16, 22, 7, 1);
        v2_R(cv, 104, 10 + i * 16, (i == 0 ? 25 : 0) * 18 / 100, 3, 1);
    }
    dump(&b, "loopermix");

    /* O. the sampler app's Loops tab (no pads needed: the pads stay the firmware's): the grid in small, the jog's
     * slot underlined, one row; knobs 1-4 the lanes' levels */
    memset(&b, 0, sizeof b);
    for (i = 0; i < 3; i++)
        b.track[i] = sc(SLOT[i], i == 1 ? 255 : 60);
    b.track[3] = sc(LOOP, 60);
    for (i = 0; i < 32; i++)
        b.pad[i] = sc(SLOT[1], 40);                      /* (the pads: Track 2's firmware, as before the app) */
    for (i = 0; i < 4; i++)
        b.knob[i] = sc(LOOP, 150);
    b.btn[B_SAMPLE] = WHITE, b.btn[B_PLAY] = GREEN;
    tabs(cv, 3);
    tab_grid(cv, 16, 3, 0, -1, -1, -1, -1);
    rowi(cv, 49, I_DOT, "Track 2  Open", 1);
    dump(&b, "tab");

    /* P. the Loops tab while a loop records (the jog's click or Sample on an empty slot): its cell solid with a dot */
    memset(&b, 0, sizeof b);
    for (i = 0; i < 3; i++)
        b.track[i] = sc(SLOT[i], i == 1 ? 255 : 60);
    b.track[3] = sc(LOOP, 60);
    for (i = 0; i < 32; i++)
        b.pad[i] = sc(SLOT[1], 40);
    b.pad[9] = WHITE, b.pad[12] = WHITE;                 /* (two notes being played on the firmware meanwhile) */
    for (i = 0; i < 4; i++)
        b.knob[i] = sc(LOOP, 150);
    b.btn[B_SAMPLE] = RED, b.btn[B_PLAY] = GREEN;
    tabs(cv, 3);
    tab_grid(cv, 16, 3, 0, 3, 0, 0, 2);
    rowi(cv, 49, I_DOT, "Bar 3", 1);
    dump(&b, "tabrec");

    /* Q. Track 4 held: pads 1-5 = Mixer, Routing, Kit, Looper, Projects; the screen names the one pressed */
    memset(&b, 0, sizeof b);
    for (i = 0; i < 3; i++)
        b.track[i] = sc(SLOT[i], 60);
    b.track[3] = WHITE;
    b.pad[24] = sc(WHITE, 90), b.pad[25] = sc(WHITE, 90), b.pad[26] = sc(KIT, 150), b.pad[27] = LOOP;
    b.pad[28] = sc(WHITE, 90);
    {
        static icon_t *const V[4] = { &PI_MIXES, &I_ROUTE, &I_KIT, &PI_LANES };
        for (i = 0; i < 4; i++) {
            const int x = 64 - 5 * 25 / 2 + 7 + i * 25;
            if (i == 3)
                v2_R(cv, x - 2, 16, 15, 15, 1);
            ic(cv, *V[i], x, 18, i != 3);
        }
        v2_ic(cv, V2I_PROJ, 64 - 5 * 25 / 2 + 7 + 4 * 25, 18, 1);
        v2_TC(cv, 64, 37, "Looper", 2, 1);
    }
    dump(&b, "held4");

    /* R. Undo after a recording: the slot empty again, said once */
    memset(&b, 0, sizeof b);
    loop_view(&b, CH_NONE, 0, 0x0F, 1);
    b.btn[B_UNDO] = WHITE;
    v2_msg(cv, "Undo", "Loop 4-1");
    dump(&b, "undo");

    /* S. Copy held, loop 1-1 pressed: the slots it can go to; an empty slot pressed: pasted (as session view) */
    memset(&b, 0, sizeof b);
    loop_view(&b, CH_NONE, 0, 0x0F, 1);
    b.pad[pi(0, 0)] = sc(WHITE, 160);
    b.btn[B_COPY] = WHITE;
    ic(cv, I_LOOP, 2, 1, 1);
    v2_T(cv, 18, 0, "1-1", 2, 1);
    v2_L(cv, 60, 7, 108, 7), v2_L(cv, 104, 3, 108, 7), v2_L(cv, 104, 11, 108, 7);
    v2_TC(cv, 64, 30, "Copy", 2, 1);
    dump(&b, "copy");

    /* T. To Kit (stage 2): a loop's jog click, To Kit: the next empty pad lit white in the Kit view */
    memset(&b, 0, sizeof b);
    loop_view(&b, 1, 1, 0, 1);
    {
        static icon_t *const A[3] = { &I_PLAY, &I_KIT, &I_LIST };
        icon_row(cv, A, 3, 1, "To Kit", 22);
    }
    dump(&b, "tokit");
    (void)I_IN, (void)BARS;
    return 0;
}
