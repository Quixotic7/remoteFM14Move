/* SPDX-License-Identifier: MPL-2.0 */
/* The sampler's stage 3, the kit (Track 4's Kit view, 16 sampled pads): the mock-up's renderer, round 1, written
 * BEFORE any code (a proposal: the owner answers it card by card on the design page). The screens are drawn with the
 * core's own v2 primitives and font and the sampler's look as built in 0.15.0 (11 x 11 icons, tabs above the rows, one
 * row at rest, three while the jog turns, a full-screen popup while a value changes, no boxes); the lights by the
 * proposal's rules. Writes kit1_<board>.pbm and .leds into the working directory (the core's test dump format: 4 pad
 * rows from the top, the 16 steps, the 8 knobs, the 16 icons, the 4 Track buttons) and one more row, the buttons:
 * sample record play loop mute delete copy undo.
 *   cc -std=c99 -I<core>/src -I<core>/third_party/moveremote/include mock_kit_v1.c <core>/src/v2.c \
 *      <core>/third_party/moveremote/host/mr_gfx.c <core>/third_party/moveremote/common/font5x7.c -o mock && ./mock
 * (<core>: third_party/rfm1cs-abletonmove at v0.15.0) */
#include <stdio.h>
#include <string.h>
#include "v2.h"

typedef struct { int r, g, b; } rgb;
static const rgb SLOT[3] = { { 255, 120, 0 }, { 40, 120, 255 }, { 255, 200, 0 } };   /* orange, blue, yellow */
static const rgb WHITE = { 255, 255, 255 }, RED = { 255, 0, 0 }, GREEN = { 0, 255, 60 }, MENU = { 255, 72, 0 };
static const rgb KIT = { 255, 60, 160 };                /* the kit (Track 4's Kit view) */
static const rgb INPUT = { 0, 220, 200 };
/* the four kit tracks 13..16, a colour each (the proposal's: a pad row a track) */
static const rgb ROWC[4] = { { 255, 60, 160 }, { 255, 140, 40 }, { 120, 220, 255 }, { 170, 120, 255 } };
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
    snprintf(path, sizeof path, "kit1_%s.pbm", name);
    f = fopen(path, "w");
    fprintf(f, "P1\n128 64\n");
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 128; x++)
            fputc(b->cv.px[y][x] ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
    snprintf(path, sizeof path, "kit1_%s.leds", name);
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

/* ---- the icons: the sampler's (as built), and the kit's own in their style */
typedef const char *const icon_t[11];
static icon_t I_SRC = { "........##.", "....#...##.", ".....#..##.", "......#.##.", "#######.##.", "#######.##.",
                        "......#.##.", ".....#..##.", "....#...##.", "........##.", "..........." };
static icon_t I_MODE = { "###.....###", "#.........#", "#.........#", "#....#....#", "#...###...#", "#...###...#",
                         "#....#....#", "#.........#", "#.........#", "###.....###", "..........." };
static icon_t I_TO = { ".....#.....", ".....#.....", ".....#.....", "..#..#..#..", "...#.#.#...", "....###....",
                       ".....#.....", "#.........#", "#.........#", "###########", "..........." };
static icon_t I_LIST = { "...........", "##..#######", "##..#######", "...........", "##..#######", "##..#######",
                         "...........", "##..#######", "##..#######", "...........", "..........." };
static icon_t I_DOT = { "...........", "...#####...", "..#######..", ".#########.", ".#########.", ".#########.",
                        ".#########.", ".#########.", "..#######..", "...#####...", "..........." };
static icon_t I_PLAY = { "...........", "..##.......", "..####.....", "..######...", "..########.", "..########.",
                         "..######...", "..####.....", "..##.......", "...........", "..........." };
static icon_t I_X = { "...........", ".##.....##.", "..##...##..", "...##.##...", "....###....", "....###....",
                      "...##.##...", "..##...##..", ".##.....##.", "...........", "..........." };
static icon_t I_TRIM = { "##.......##", "#.........#", "#.........#", "#..#...#..#", "#.##...##.#", "#.##...##.#",
                         "#..#...#..#", "#.........#", "#.........#", "##.......##", "..........." };
static icon_t I_DIR = { "...........", ".####......", "#....#####.", "#.........#", "#.........#", "#.........#",
                        "#.........#", "#.........#", "###########", "...........", "..........." };
static icon_t I_REV = { "......#....", "......##...", "##########.", "......##...", "......#....", "....#......",
                        "...##......", ".##########", "...##......", "....#......", "..........." };
static icon_t I_NORM = { "###########", "...........", "..#.....#..", ".###...###.", "#####.#####", "..#.....#..",
                         "..#..#..#..", "..#..#..#..", "..#..#..#..", "..#..#..#..", "..........." };
static icon_t I_LOOP = { "...#####...", "..#.....#..", ".#.......#.", ".#.......#.", ".#.......#.", ".#.....####",
                         ".#......###", "..#......#.", "...###.....", "...........", "..........." };
/* (round A, 2026-11-25) Toggle: play, then stop */
static icon_t I_TOGGLE = { "...........", "#.....####.", "##....####.", "###...####.", "####..####.", "#####.####.",
                           "####..####.", "###...####.", "##....####.", "#.....####.", "..........." };
/* the kit's own */
static icon_t I_KIT = { "...........", "####..####.", "####..####.", "####..####.", "####..####.", "...........",
                        "####..####.", "####..####.", "####..####.", "####..####.", "..........." };
static icon_t I_PAD = { "...........", "#########..", "#########..", "##.....##..", "##.....##..", "##.....##..",
                        "##.....##..", "#########..", "#########..", "...........", "..........." };
static icon_t I_PITCH = { "....#......", "...###.....", "..#.#.#....", "....#......", "....#......", "....#...#..",
                          "....#...#..", "........#..", "......#.#.#", ".......###.", "........#.." };
static icon_t I_STRETCH = { "#.........#", "#.........#", "#..#...#..#", "#.##...##.#", "###########", "#.##...##.#",
                            "#..#...#..#", "#.........#", "#.........#", "...........", "..........." };
static icon_t I_ENV = { "...........", "...#####...", "..#.....#..", "..#......#.", ".#........#", ".#........#",
                        "#.........#", "#..........", "#..........", "###########", "..........." };
static icon_t I_CHOKE = { "##.....##..", "##.....##..", "##.....##..", "##..#..##..", "##...#.##..", "##....###..",
                          "##.....##..", "##.....##..", "##.....##..", "##.....##..", "..........." };
static icon_t I_SEND = { "...........", "......#....", "......##...", "###########", "......##...", "......#....",
                         "#..........", "#..........", "#..........", "#..........", "..........." };
static icon_t I_LVL = { "...........", "........##.", "........##.", ".....##.##.", ".....##.##.", "..##.##.##.",
                        "..##.##.##.", "..##.##.##.", "..##.##.##.", "##########.", "..........." };
static icon_t I_PAN = { "...........", "...........", ".#.......#.", "##.......##", "###########", "##.......##",
                        ".#..###..#.", "....###....", "....###....", "...........", "..........." };
static icon_t I_COPY = { "#######....", "#.....#....", "#.....#....", "#...#######", "#...#.....#", "#####.....#",
                         "....#.....#", "....#.....#", "....#.....#", "....#######", "..........." };
static icon_t I_MIX = { "#.........#", ".#.......#.", "..#.....#..", "...#...#...", "....#.#....", ".....#.....",
                        ".....#.....", ".....#.....", ".....#.....", ".....#.....", "..........." };
static icon_t I_JOIN = { "...........", "...........", "####.######", "####.######", "####.######", "####.######",
                         "####.######", "...........", "...........", "...........", "..........." };
static icon_t I_SLICE = { "..#..#..#..", "..#..#..#..", "#.#..#..#.#", "#.##.#.##.#", "####.####.#",
                          "#.##.#.##.#", "#.#..#..#.#", "..#..#..#..", "..#..#..#..", "...........", "..........." };
static icon_t I_SAVE = { "###########", "#.........#", "#..#####..#", "#.........#", "#.........#", "#..#####..#",
                         "#..#####..#", "#..#####..#", "#..#####..#", "###########", "..........." };
static icon_t I_KEYS = { "###########", "#.#.#.#.#.#", "#.#.#.#.#.#", "#.#.#.#.#.#", "#.#.#.#.#.#", "#..#...#..#",
                         "#..#...#..#", "#..#...#..#", "#..#...#..#", "###########", "..........." };
static void ic(mr_canvas_t *cv, icon_t i, int x, int y, int v)
{
    int r, c;
    for (r = 0; r < 11; r++)
        for (c = 0; c < 11 && i[r][c]; c++)
            if (i[r][c] == '#')
                v2_px(cv, x + c, y + r, v);
}

/* ---- the kit (a stand-in): which pads hold a sample, kit pad n (1..16) bottom-left first */
static const signed char HAS[16] = { 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0 };
static int kp(int n)                                    /* kit pad n (1..16) -> the LED index (0 = top-left) */
{
    const int r = (n - 1) / 4, c = (n - 1) % 4;
    return (3 - r) * 8 + c;
}
static int rp(int n)                                    /* right-half pad n (1..16) -> the LED index */
{
    const int r = (n - 1) / 4, c = (n - 1) % 4;
    return (3 - r) * 8 + 4 + c;
}
/* the kit's lights: a pad with a sample dim in its row's (track's) colour, the chosen one white, an empty one dark;
 * the right half: the chosen pad's 16 pitches (its own pitch, pad 1, and the octaves bright) */
static void kit_pads(board_t *b, int chosen, int right)
{
    int n;
    for (n = 1; n <= 16; n++)
        if (HAS[n - 1])
            b->pad[kp(n)] = sc(ROWC[(n - 1) / 4], 80);
    if (chosen)
        b->pad[kp(chosen)] = WHITE;
    for (n = 1; n <= 16 && right == 1; n++)             /* 16 pitches: semitones up from the pad's own */
        b->pad[rp(n)] = (n - 1) % 12 == 0 ? sc(ROWC[(chosen - 1) / 4], 230) : sc(WHITE, 40);
    for (n = 1; n <= 16 && right == 2; n++) {           /* in key, rows a fourth apart: 4 notes of the scale a row */
        const int deg = (n - 1) % 4 + ((n - 1) / 4) * 3;
        b->pad[rp(n)] = deg % 7 == 0 ? sc(ROWC[(chosen - 1) / 4], 230) : sc(WHITE, 40);
    }
}
/* the kit view's common lights: Track 4 the kit's, the chosen pad's steps (and only its), the knobs */
static const unsigned short STEPS[16] = { 0x1111, 0x1010, 0x0404, 0x4444, 0x0808, 0x2002, 0x0101, 0, 0x8888, 0x0880,
                                          0x0220, 0, 0x0001, 0x0010, 0, 0 };
static void kit_view(board_t *b, int chosen, int right, int knobs)
{
    int i;
    kit_pads(b, chosen, right);
    for (i = 0; i < 3; i++)
        b->track[i] = sc(SLOT[i], 60);
    b->track[3] = KIT;
    for (i = 0; i < 16; i++)
        b->step[i] = STEPS[chosen - 1] >> i & 1 ? sc(ROWC[(chosen - 1) / 4], 220) : sc(WHITE, 16);
    for (i = 0; i < knobs; i++)
        b->knob[i] = sc(KIT, 150);
    b->btn[B_PLAY] = GREEN;
    b->btn[B_SAMPLE] = sc(WHITE, 60);
    b->btn[B_COPY] = sc(WHITE, 60), b->btn[B_DELETE] = sc(WHITE, 60), b->btn[B_MUTE] = sc(WHITE, 60);
}

/* a wave about y0 (h each way) from x0 over w px; the part [xa, xb) a white block with black columns */
static void wavec(mr_canvas_t *cv, int x0, int w, int y0, int h, unsigned seed, int xa, int xb, int decay)
{
    int x;
    if (xb > xa)
        v2_R(cv, xa, y0 - h - 1, xb - xa, 2 * h + 2, 1);
    for (x = 0; x < w; x += 2) {
        const int beat = (x * 16 / w) % 4, in = x0 + x >= xa && x0 + x < xb;
        int a;
        seed = seed * 1103515245u + 12345u;
        a = 1 + (int)((seed >> 16) % (unsigned)h) * (beat == 0 ? 4 : beat == 2 ? 3 : 2) / 4;
        if (decay)                                      /* a drum hit: loud at once, dying away */
            a = 1 + (h - 1) * (w - x) * (w - x) / (w * w) + (int)((seed >> 20) % 2u);
        if (a > h)
            a = h;
        v2_R(cv, x0 + x, y0 - a, 1, a * 2, in ? 0 : 1);
    }
}
/* the tabs: Pad, Kit, Files (the shown one underlined; `on`: the jog on the tabs, a white block); the chosen pad's
 * number at the right */
static void top(mr_canvas_t *cv, int tab, int on, int pad)
{
    static icon_t *const T[3] = { &I_PAD, &I_KIT, &I_DIR };
    char n[8];
    int i;
    for (i = 0; i < 3; i++) {
        const int x = 2 + i * 18;
        if (i == tab && on)
            v2_R(cv, x - 2, 0, 15, 14, 1);
        ic(cv, *T[i], x, 1, !(i == tab && on));
        if (i == tab && !on)
            v2_R(cv, x, 13, 11, 1, 1);
    }
    if (pad) {
        snprintf(n, sizeof n, "%d", pad);
        v2_T(cv, 128 - v2_tw(n, 2), 0, n, 2, 1);
    }
}
static void rowi(mr_canvas_t *cv, int y, icon_t i, const char *val, int chosen)
{
    if (chosen)
        v2_R(cv, 0, y, 128, 15, 1);
    ic(cv, i, 3, y + 2, !chosen);
    v2_T(cv, 20, y + 1, val, 2, !chosen);
}
static void marks(mr_canvas_t *cv, int n, int cur, int y)
{
    int i;
    for (i = 0; i < n; i++)
        v2_R(cv, 3 + i * 9, i == cur ? y - 1 : y, 6, i == cur ? 3 : 1, 1);
}
/* the steps the sample lasts, under the wave: n steps of 16, a tick a step, the bar's quarters taller */
static void grid(mr_canvas_t *cv, int y, int n)
{
    int i;
    for (i = 0; i <= 16; i++) {
        const int x = i * 127 / 16;
        v2_R(cv, x, y, 1, i % 4 == 0 ? 4 : 2, 1);
    }
    v2_R(cv, 0, y + 5, n * 127 / 16, 2, 1);
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
/* the AHR envelope drawn over w px: attack a, hold h, release r (fractions of w, in 1/100) */
static void ahr(mr_canvas_t *cv, int x0, int y0, int w, int ht, int a, int h, int r, int thick)
{
    const int xa = x0 + w * a / 100, xh = xa + w * h / 100, xr = xh + w * r / 100;
    v2_L(cv, x0, y0, xa, y0 - ht);
    v2_L(cv, xa, y0 - ht, xh, y0 - ht);
    v2_L(cv, xh, y0 - ht, xr, y0);
    v2_dotH(cv, x0, y0, w);
    if (thick == 1)
        v2_L(cv, x0 + 1, y0, xa + 1, y0 - ht), v2_L(cv, x0 + 2, y0, xa + 2, y0 - ht);
    if (thick == 2)
        v2_R(cv, xa, y0 - ht + 1, xh - xa, 2, 1);
    if (thick == 3)
        v2_L(cv, xh - 1, y0 - ht, xr - 1, y0), v2_L(cv, xh - 2, y0 - ht, xr - 2, y0);
}

int main(void)
{
    static board_t b;
    mr_canvas_t *cv = &b.cv;
    int i;

    /* A. the Kit view at rest (Pad tab): the chosen pad's sample, the steps it lasts, one row */
    memset(&b, 0, sizeof b);
    kit_view(&b, 5, 1, 8);
    top(cv, 0, 0, 5);
    wavec(cv, 0, 128, 28, 11, 55u, 0, 0, 1);
    grid(cv, 41, 4);
    rowi(cv, 49, I_PITCH, "+3", 1);
    dump(&b, "rest");

    /* B. the jog turning: three rows, the chosen in the middle */
    memset(&b, 0, sizeof b);
    kit_view(&b, 5, 1, 8);
    top(cv, 0, 0, 5);
    rowi(cv, 16, I_DIR, "Snare Rim", 0);
    rowi(cv, 32, I_PITCH, "+3", 1);
    rowi(cv, 48, I_STRETCH, "Off", 0);
    dump(&b, "three");

    /* C. a knob touched / a row clicked: the full-screen popup, its name and value */
    memset(&b, 0, sizeof b);
    kit_view(&b, 5, 1, 8);
    b.knob[0] = WHITE;
    v2_titleBox(cv, "Pitch", "+3", 64, 128);
    grid(cv, 54, 4);
    dump(&b, "pitch");

    /* D. Stretch to steps: the sample fitted to 16 steps (repitched); the steps it lasts lit */
    memset(&b, 0, sizeof b);
    kit_view(&b, 9, 1, 8);
    b.knob[2] = WHITE;
    v2_titleBox(cv, "Stretch", "16 Steps", 64, 128);
    grid(cv, 54, 16);
    for (i = 0; i < 16; i++)
        b.step[i] = sc(ROWC[2], i == 0 ? 255 : 110);
    dump(&b, "stretch");

    /* E. how it plays: Loop, with its envelope (attack, hold, release); (round A) four modes, Toggle the fourth */
    memset(&b, 0, sizeof b);
    kit_view(&b, 9, 1, 8);
    b.knob[3] = WHITE;
    v2_TC(cv, 64, 0, "Play", 2, 1);
    {
        static icon_t *const P[4] = { &I_PLAY, &I_PAD, &I_LOOP, &I_TOGGLE };
        icon_row(cv, P, 4, 2, NULL, 18);
    }
    v2_TC(cv, 64, 32, "Loop", 2, 1);
    ahr(cv, 14, 63, 100, 12, 15, 45, 35, 0);
    dump(&b, "mode");

    /* F. the envelope's attack while knob 5 turns */
    memset(&b, 0, sizeof b);
    kit_view(&b, 9, 1, 8);
    b.knob[4] = WHITE;
    v2_TC(cv, 64, 0, "Attack", 2, 1);
    v2_TC(cv, 64, 17, "20 ms", 2, 1);
    ahr(cv, 4, 60, 120, 22, 18, 45, 35, 1);
    dump(&b, "attack");

    /* G. choke group: the pads of group 2 lit in the group's sign (white), the others dark */
    memset(&b, 0, sizeof b);
    kit_view(&b, 11, 1, 8);
    for (i = 1; i <= 16; i++)
        if (HAS[i - 1] && i != 11 && i != 10 && i != 9)
            b.pad[kp(i)] = sc(WHITE, 14);
    b.pad[kp(9)] = sc(ROWC[2], 220), b.pad[kp(10)] = sc(ROWC[2], 220);
    v2_titleBox(cv, "Choke", "Group 2", 64, 128);
    dump(&b, "choke");

    /* H. FX sends: send 1 of the three audio FX */
    memset(&b, 0, sizeof b);
    kit_view(&b, 6, 1, 8);
    b.knob[7] = WHITE;
    v2_titleBox(cv, "Send 1", "35%", 64, 128);
    v2_dotH(cv, 8, 58, 112), v2_R(cv, 8, 56, 39, 5, 1);
    dump(&b, "send");

    /* I. Files tab: places (takes, Move's samples, kits); the jog's click on a sound loads it onto the chosen pad */
    memset(&b, 0, sizeof b);
    kit_view(&b, 5, 0, 0);
    b.pad[kp(5)] = sc(WHITE, (0 + 1) * 120);             /* (the pad it loads onto: blinking white) */
    top(cv, 2, 0, 5);
    rowi(cv, 16, I_LIST, "Takes", 0);
    rowi(cv, 32, I_DIR, "Samples", 1);
    rowi(cv, 48, I_KIT, "Kits", 0);
    dump(&b, "places");

    /* J. in a folder: a file a row, its wave as a small picture; each plays as the jog lands on it */
    memset(&b, 0, sizeof b);
    kit_view(&b, 5, 0, 0);
    top(cv, 2, 0, 5);
    {
        static const char *const F[3] = { "Kick 808", "Rim 2", "Snare 4" };
        for (i = 0; i < 3; i++) {
            const int y = 16 + i * 16, ch = i == 1;
            if (ch)
                v2_R(cv, 0, y, 128, 15, 1);
            v2_T(cv, 3, y + 1, F[i], 2, !ch);
            if (ch)
                wavec(cv, 99, 28, y + 7, 5, 9u + i, 99, 127, 1);
            else
                wavec(cv, 99, 28, y + 7, 5, 9u + i, 0, 0, 1);
        }
    }
    b.btn[B_PLAY] = GREEN;
    dump(&b, "files");

    /* K. a folder's actions (Shift + the jog's click on a folder): its first 16 sounds onto the pads */
    memset(&b, 0, sizeof b);
    kit_view(&b, 1, 0, 0);
    for (i = 1; i <= 16; i++)
        b.pad[kp(i)] = sc(ROWC[(i - 1) / 4], 200);
    wavec(cv, 0, 128, 10, 8, 3u, 0, 0, 0);
    {
        static icon_t *const A[2] = { &I_DIR, &I_KIT };
        icon_row(cv, A, 2, 1, "All To Pads", 26);
    }
    dump(&b, "folder");

    /* L. pad 7 held + Sample: armed (the threshold), the pad blinking red */
    memset(&b, 0, sizeof b);
    kit_view(&b, 7, 1, 0);
    b.pad[kp(7)] = RED;
    b.btn[B_SAMPLE] = RED;
    ic(cv, I_DOT, 2, 1, 1);
    v2_T(cv, 20, 0, "7", 2, 1);
    ic(cv, I_SRC, 117, 1, 1);
    for (i = 0; i < 58; i++)
        v2_R(cv, i * 2, 34, 1, 1, 1);
    v2_R(cv, 121, 45 - 6, 6, 6, 1), v2_dotV(cv, 124, 17, 28), v2_R(cv, 117, 45 - 14, 11, 1, 1);
    v2_TC(cv, 58, 48, "Waiting", 2, 1);
    dump(&b, "armed");

    /* M. the take runs: the wave grows on the kit's screen until the pad is let go */
    memset(&b, 0, sizeof b);
    kit_view(&b, 7, 1, 0);
    b.pad[kp(7)] = RED;
    b.btn[B_SAMPLE] = RED;
    ic(cv, I_DOT, 2, 1, 1);
    v2_T(cv, 20, 0, "7", 2, 1);
    v2_T(cv, 40, 0, "0:02", 2, 1);
    wavec(cv, 0, 70, 34, 15, 41u, 0, 0, 0);
    for (i = 35; i < 58; i++)
        v2_R(cv, i * 2, 34, 1, 1, 1);
    v2_R(cv, 121, 45 - 22, 6, 22, 1), v2_dotV(cv, 124, 17, 28);
    dump(&b, "recording");

    /* N. Copy held, pad 2 then pad 5: copy over, mix the two, or join them end to end */
    memset(&b, 0, sizeof b);
    kit_view(&b, 5, 0, 0);
    b.pad[kp(2)] = sc(WHITE, 140);                       /* (the first: blinking) */
    b.pad[kp(5)] = WHITE;
    b.btn[B_COPY] = WHITE;
    v2_T(cv, 0, 0, "2", 2, 1);
    v2_T(cv, 128 - v2_tw("5", 2), 0, "5", 2, 1);
    v2_L(cv, 18, 7, 108, 7), v2_L(cv, 104, 3, 108, 7), v2_L(cv, 104, 11, 108, 7);   /* (2 onto 5) */
    {
        static icon_t *const A[3] = { &I_COPY, &I_MIX, &I_JOIN };
        icon_row(cv, A, 3, 2, "Join", 26);
    }
    dump(&b, "merge");

    /* O. the result of a join: pad 5 holds 2 + 5 end to end (a new file), the cut shown */
    memset(&b, 0, sizeof b);
    kit_view(&b, 5, 1, 8);
    top(cv, 0, 0, 5);
    wavec(cv, 0, 62, 28, 11, 12u, 0, 0, 1);
    wavec(cv, 64, 64, 28, 11, 55u, 0, 0, 1);
    v2_dotV(cv, 63, 16, 26);
    grid(cv, 41, 8);
    rowi(cv, 49, I_DIR, "Join 2+5", 1);
    dump(&b, "joined");

    /* P. a take sliced over the pads (a take's "to the kit"): 16 slices, by its hits; the pads play the slices */
    memset(&b, 0, sizeof b);
    for (i = 1; i <= 16; i++)
        b.pad[kp(i)] = sc(SLOT[1], i == 6 ? 255 : 110);
    b.pad[kp(6)] = WHITE;
    for (i = 0; i < 3; i++)
        b.track[i] = sc(SLOT[i], 60);
    b.track[3] = KIT;
    b.knob[0] = sc(MENU, 150), b.knob[1] = sc(MENU, 150);
    wavec(cv, 0, 128, 18, 12, 77u, 40, 48, 0);
    {
        static const int CUT[17] = { 0, 9, 16, 24, 31, 40, 48, 55, 63, 72, 80, 87, 95, 104, 111, 119, 127 };
        for (i = 1; i < 16; i++)
            v2_dotV(cv, CUT[i], 5, 26);
    }
    rowi(cv, 33, I_SLICE, "16 Hits", 1);
    rowi(cv, 49, I_KIT, "Pads 1-16", 0);
    dump(&b, "slice");

    /* Q. Kit tab: the kit's own rows (load, save, a folder, the right half) */
    memset(&b, 0, sizeof b);
    kit_view(&b, 5, 1, 0);
    top(cv, 1, 0, 0);
    v2_T(cv, 128 - v2_tw("Kit 3", 2), 0, "Kit 3", 2, 1);
    rowi(cv, 16, I_DIR, "Load", 0);
    rowi(cv, 32, I_SAVE, "Save", 1);
    rowi(cv, 48, I_KEYS, "Pitches", 0);
    dump(&b, "kittab");

    /* R. saved: a kit is a folder of its sounds; its name given, said once */
    memset(&b, 0, sizeof b);
    kit_view(&b, 5, 1, 0);
    v2_msg(cv, "Saved", "Kit 3");
    dump(&b, "saved");

    /* S. the right half in key: rows a fourth apart, the scale's notes only; the pad's own note the root */
    memset(&b, 0, sizeof b);
    kit_view(&b, 5, 2, 8);
    v2_titleBox(cv, "In Key", "C Minor", 64, 128);
    dump(&b, "inkey");

    /* T. a firmware Track held: steps 13..16 are the kit's tracks (routed to the kit: its colour) */
    memset(&b, 0, sizeof b);
    for (i = 0; i < 3; i++)
        b.track[i] = sc(SLOT[i], i == 0 ? 255 : 60);
    b.track[3] = sc(KIT, 60);
    for (i = 0; i < 16; i++)
        b.step[i] = i >= 12 ? sc(ROWC[i - 12], 200) : i == 0 || i == 4 ? SLOT[0] : sc(WHITE, 0);
    b.step[0] = WHITE;
    for (i = 0; i < 32; i++)
        b.pad[i] = sc(SLOT[0], 40);
    v2_T(cv, 0, 0, "Felucca", 2, 1);
    for (i = 0; i < 16; i++) {
        const int x = 4 + i * 7 + (i / 4) * 3;
        if (i >= 12)
            v2_R(cv, x, 30, 5, 10, 1);              /* (the kit's: a block and the kit's sign under them) */
        else if (i == 0 || i == 4)
            v2_R(cv, x, 30, 5, 10, 1);
        else
            v2_R(cv, x + 2, 34, 1, 2, 1);
    }
    v2_R(cv, 4, 42, 5, 1, 1);
    ic(cv, I_KIT, 4 + 12 * 7 + 9 + 7, 46, 1);
    dump(&b, "route");

    /* U. the sampler's Record tab with Source Kit and To Kit (the next empty pad) */
    memset(&b, 0, sizeof b);
    for (i = 0; i < 3; i++)
        b.track[i] = sc(SLOT[i], 60);
    b.track[3] = KIT;
    kit_pads(&b, 0, 0);
    b.pad[kp(8)] = sc(WHITE, 160);                       /* (the next empty pad: where the take lands) */
    b.btn[B_SAMPLE] = WHITE;
    {
        static icon_t *const T[3] = { &I_DOT, &I_LIST, &I_TRIM };
        for (i = 0; i < 3; i++) {
            ic(cv, *T[i], 2 + i * 18, 1, 1);
            if (i == 0)
                v2_R(cv, 2, 13, 11, 1, 1);
        }
    }
    v2_T(cv, 128 - v2_tw("K", 2), 0, "K", 2, 1);
    rowi(cv, 16, I_SRC, "Kit", 0);
    rowi(cv, 32, I_MODE, "Bars", 0);
    rowi(cv, 48, I_TO, "Kit Pad 8", 1);
    dump(&b, "sampler");

    /* V. Delete held + a pad: the pad's steps go first, then (again) its sample; the pad's actions on the jog: */
    memset(&b, 0, sizeof b);
    kit_view(&b, 5, 0, 0);
    wavec(cv, 0, 128, 12, 10, 55u, 0, 0, 1);
    {
        static icon_t *const A[5] = { &I_PLAY, &I_TRIM, &I_NORM, &I_REV, &I_X };
        icon_row(cv, A, 5, 1, "Trim", 28);
    }
    dump(&b, "padact");

    /* W. further down the Pad tab's list: the envelope, the choke group, the level and pan (marks: where the jog is) */
    memset(&b, 0, sizeof b);
    kit_view(&b, 5, 1, 8);
    top(cv, 0, 0, 5);
    rowi(cv, 16, I_ENV, "20/0/300", 0);
    rowi(cv, 32, I_CHOKE, "Group 2", 1);
    rowi(cv, 48, I_LVL, "-3 dB", 0);
    dump(&b, "rows2");
    (void)INPUT, (void)I_PAN, (void)I_SEND, (void)marks;
    return 0;
}
