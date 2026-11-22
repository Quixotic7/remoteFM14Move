/* SPDX-License-Identifier: MPL-2.0 */
/* Projects that hold their firmwares, and firmware states on the Move (the owner's request of 2026-11-22): the
 * mock-up's renderer, written BEFORE the core's code. The screens with the core's own v2 primitives and font, the
 * lights by the proposal's rules. Writes sessview_ps_<board>.pbm and .leds into the working directory (the format of
 * the core's test dumps: 4 pad rows from the top, the 16 steps, the 8 knobs, the 16 icons, the 4 Track buttons).
 *   cc -std=c99 -I<core>/src -I<core>/third_party/moveremote/include mock_projstates_v1.c <core>/src/v2.c \
 *      <core>/third_party/moveremote/host/mr_gfx.c <core>/third_party/moveremote/common/font5x7.c -o mock && ./mock */
#include <stdio.h>
#include <string.h>
#include "v2.h"

typedef struct { int r, g, b; } rgb;
static const rgb SLOT[3] = { { 255, 120, 0 }, { 40, 120, 255 }, { 255, 200, 0 } };   /* orange, blue, yellow */
static const rgb PROJ[8] = { { 255, 42, 42 }, { 255, 208, 0 }, { 42, 255, 80 }, { 26, 228, 255 },
                             { 120, 60, 255 }, { 255, 58, 200 }, { 255, 170, 110 }, { 180, 255, 160 } };
static const rgb WHITE = { 255, 255, 255 };
static rgb sc(rgb c, int f) { rgb o = { c.r * f / 256, c.g * f / 256, c.b * f / 256 }; return o; }

typedef struct {
    mr_canvas_t cv;
    rgb pad[32];                                        /* 0 = the top-left pad */
    rgb step[16], knob[8], track[4];
    int icon[16];                                       /* 0..127 */
} board_t;

static void clear(board_t *b)
{
    memset(b, 0, sizeof *b);
}
static void dump(const board_t *b, const char *name)
{
    char path[96];
    FILE *f;
    int x, y, i;
    snprintf(path, sizeof path, "sessview_ps_%s.pbm", name);
    f = fopen(path, "w");
    fprintf(f, "P1\n128 64\n");
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 128; x++)
            fputc(b->cv.px[y][x] ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
    snprintf(path, sizeof path, "sessview_ps_%s.leds", name);
    f = fopen(path, "w");
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 8; x++)
            fprintf(f, "%02x%02x%02x ", b->pad[y * 8 + x].r, b->pad[y * 8 + x].g, b->pad[y * 8 + x].b);
        fputc('\n', f);
    }
    for (i = 0; i < 16; i++)
        fprintf(f, "%02x%02x%02x ", b->step[i].r, b->step[i].g, b->step[i].b);
    fputc('\n', f);
    for (i = 0; i < 8; i++)
        fprintf(f, "%02x%02x%02x ", b->knob[i].r, b->knob[i].g, b->knob[i].b);
    fputc('\n', f);
    for (i = 0; i < 16; i++)
        fprintf(f, "%02x ", b->icon[i]);
    fputc('\n', f);
    for (i = 0; i < 4; i++)
        fprintf(f, "%02x%02x%02x ", b->track[i].r, b->track[i].g, b->track[i].b);
    fputc('\n', f);
    fclose(f);
}

/* ---- the project grid's lights: the stored projects in their colours, the open one white */
static const int STORED[] = { 0, 1, 2, 4, 6, 7, 11, 16 };
static void proj_pads(board_t *b, int open, int held)
{
    unsigned i;
    for (i = 0; i < sizeof STORED / sizeof *STORED; i++)
        b->pad[STORED[i]] = sc(PROJ[i % 8], 110);
    if (open >= 0)
        b->pad[open] = WHITE;
    if (held >= 0)
        b->pad[held] = WHITE;
    b->track[3] = sc(WHITE, 200);                       /* (Track 4: the projects) */
}

/* a project's three firmwares, a row a slot: the slot's number, the name (NULL: an empty slot, a dotted line); bar:
 * -1 none, 0..100 a loading bar, 101 waiting (a dotted bar) */
static void fw_rows(mr_canvas_t *cv, const char *const *fw, const int *bar, int y0)
{
    int s;
    for (s = 0; s < 3; s++) {
        const int y = y0 + s * 12;
        char n[2] = { (char)('1' + s), 0 };
        v2_T(cv, 0, y, n, 1, 1);
        if (fw[s])
            v2_T(cv, 12, y, fw[s], 1, 1);
        else
            v2_dotH(cv, 12, y + 3, 40);
        if (bar && fw[s] && bar[s] >= 0) {
            const int x = 84, w = 43;
            if (bar[s] > 100)
                v2_dotH(cv, x, y + 3, w);
            else {
                v2_O(cv, x, y + 1, w, 5, 1);
                v2_R(cv, x, y + 1, w * bar[s] / 100, 5, 1);
            }
        }
    }
}

int main(void)
{
    board_t b;
    static const char *const FW[3] = { "ChoralRoot", "SLOOP", "Felucca" };
    static const char *const FW2[3] = { "ChoralRoot", "SLOOP", NULL };
    int i;

    /* A: a project pad held: its number, tempo and clips as today, and its three firmwares */
    clear(&b);
    v2_T(&b.cv, 0, 0, "07", 3, 1);
    v2_ic(&b.cv, V2I_PLAY, 40, 5, 1);
    v2_T(&b.cv, 53, 3, "120", 2, 1);
    v2_ic(&b.cv, V2I_CLIP, 92, 5, 1);
    v2_T(&b.cv, 105, 3, "12", 2, 1);
    fw_rows(&b.cv, FW, NULL, 28);
    proj_pads(&b, 2, 6);
    b.knob[0] = sc(PROJ[4], 255);                       /* (knob 1: the held project's colour, as today) */
    for (i = 0; i < 3; i++)
        b.track[i] = sc(SLOT[i], 64);
    dump(&b, "proj_held");

    /* B: a project pressed: it loads. Slot 1 was the same firmware (its state swapped: done), slot 2 loads, slot 3
     * waits; a Track button lights when its firmware is ready */
    clear(&b);
    {
        static const int bar[3] = { 100, 60, 101 };
        v2_T(&b.cv, 0, 0, "07", 3, 1);
        v2_T(&b.cv, 41, 4, "Loading", 2, 1);
        fw_rows(&b.cv, FW, bar, 28);
    }
    proj_pads(&b, 6, -1);
    b.track[0] = SLOT[0];
    dump(&b, "proj_loading");

    /* C: a firmware of the project is not on this Move: said for a second, its slot left empty */
    clear(&b);
    v2_TC(&b.cv, 64, 8, "Felucca", 2, 1);
    v2_TC(&b.cv, 64, 32, "not installed", 1, 1);
    v2_TC(&b.cv, 64, 46, "Track 3 empty", 1, 1);
    proj_pads(&b, 6, -1);
    b.track[0] = SLOT[0], b.track[1] = sc(SLOT[1], 64), b.track[2] = sc(WHITE, 40);
    dump(&b, "proj_missing");

    /* D: a held project with an empty slot */
    clear(&b);
    v2_T(&b.cv, 0, 0, "03", 3, 1);
    v2_ic(&b.cv, V2I_PLAY, 40, 5, 1);
    v2_T(&b.cv, 53, 3, "96", 2, 1);
    v2_ic(&b.cv, V2I_CLIP, 92, 5, 1);
    v2_T(&b.cv, 105, 3, "4", 2, 1);
    fw_rows(&b.cv, FW2, NULL, 28);
    proj_pads(&b, 6, 2);
    dump(&b, "proj_held_empty");

    /* E: States, opened from Track 1 held + pad 7: the firmware's name, its 32 states (stored: framed; the one
     * loaded or saved last: filled; empty: a dot). Pads in the slot's colour; step 14 (New) has its icon lit */
    clear(&b);
    {
        static const int used[] = { 0, 1, 4, 15 };
        const int cur = 1;
        unsigned k;
        v2_T(&b.cv, 1, 0, "ChoralRoot", 2, 1);
        for (i = 0; i < 32; i++) {
            const int x = (i % 8) * 16 + 1, y = 16 + (i / 8) * 12;
            int has = 0;
            char n[4];
            for (k = 0; k < sizeof used / sizeof *used; k++)
                has |= used[k] == i;
            if (!has) {
                v2_R(&b.cv, x + 6, y + 5, 2, 2, 1);
                continue;
            }
            snprintf(n, sizeof n, "%d", i + 1);
            if (i == cur)
                v2_R(&b.cv, x, y, 14, 11, 1);
            else
                v2_O(&b.cv, x, y, 14, 11, 1);
            v2_T(&b.cv, x + (14 - v2_tw(n, 1)) / 2, y + 2, n, 1, i != cur);
            b.pad[i] = i == cur ? SLOT[0] : sc(SLOT[0], 108);
        }
    }
    b.track[0] = SLOT[0], b.track[1] = sc(SLOT[1], 64), b.track[2] = sc(SLOT[2], 64);
    b.icon[13] = 127, b.step[13] = sc(WHITE, 64);
    dump(&b, "states");

    /* F: step 14 in States: the firmware restarts as first installed. No state is the current one */
    {
        board_t st = b;
        clear(&b);
        v2_TC(&b.cv, 64, 10, "New state", 2, 1);
        v2_TC(&b.cv, 64, 36, "ChoralRoot", 1, 1);
        memcpy(b.pad, st.pad, sizeof b.pad), memcpy(b.track, st.track, sizeof b.track);
        b.pad[1] = sc(SLOT[0], 108);
        b.icon[13] = 127, b.step[13] = WHITE;
        dump(&b, "state_new");
        /* G: Undo right after: what the firmware held is back */
        clear(&b);
        v2_TC(&b.cv, 64, 10, "Undo", 2, 1);
        v2_TC(&b.cv, 64, 36, "New state", 1, 1);
        memcpy(b.pad, st.pad, sizeof b.pad), memcpy(b.track, st.track, sizeof b.track);
        b.icon[13] = 127, b.step[13] = sc(WHITE, 64);
        dump(&b, "state_undo");
    }
    return 0;
}
