/* SPDX-License-Identifier: MPL-2.0 */
/* Session view, proposal 2 (the owner's review of 2026-11-21): the mock-up's renderer, written BEFORE the core's code
 * (the core then draws the same, pixel for pixel). The screens with the core's own v2 primitives and font, the pads'
 * and steps' colours by the proposal's rules. Writes sessview2_<board>.pbm and .leds into the working directory.
 *   cc -std=c99 -I<core>/src -I<core>/third_party/moveremote/include mock_session_v2.c <core>/src/v2.c \
 *      <core>/third_party/moveremote/host/mr_gfx.c <core>/third_party/moveremote/common/font5x7.c -o mock && ./mock */
#include <stdio.h>
#include <string.h>
#include "v2.h"

typedef struct { int r, g, b; } rgb;
static const rgb TRACK[16] = {
    { 255, 42, 42 },  { 255, 128, 0 },  { 255, 208, 0 },   { 200, 255, 42 }, { 42, 255, 80 },   { 0, 255, 190 },
    { 26, 228, 255 }, { 40, 120, 255 }, { 120, 60, 255 },  { 196, 58, 255 }, { 255, 58, 200 },  { 255, 110, 140 },
    { 255, 170, 110 }, { 180, 255, 160 }, { 140, 220, 255 }, { 220, 200, 255 },
};
static const rgb WHITE = { 255, 255, 255 }, GREEN = { 0, 255, 0 }, OFF = { 0, 0, 0 };
static rgb sc(rgb c, int f) { rgb o = { c.r * f / 256, c.g * f / 256, c.b * f / 256 }; return o; }

enum { NONE = -2, STOP = -1 };
typedef struct {
    int clip[16][16];                                   /* [track][scene]: 0 no clip, 1 a clip with notes, 2 an empty clip */
    int active[16], queued[16], sel[16], mute[16];
    int sel_track, playing, track_page, scene_off;
    int bl;                                             /* the blink's phase (1: on) */
    int pu;                                             /* the pulse's step, 0 (the beat) .. 5 */
    int info;                                           /* a knob touched: its column + 1 (the track's popup), 0 none */
    const char *fw[16];                                 /* the firmwares a track reaches (the popup's second row) */
} S;

enum { CS_EMPTY, CS_CONTENT, CS_HOLLOW, CS_ARMED, CS_PLAYING, CS_QUEUED, CS_STOPPING };
static int clip_state(const S *s, int t, int sn)
{
    const int c = s->clip[t][sn];
    if (s->queued[t] == sn)
        return CS_QUEUED;
    if (s->active[t] == sn && c && s->queued[t] == STOP && s->playing)
        return CS_STOPPING;
    if (s->active[t] == sn && c)
        return s->playing ? CS_PLAYING : CS_ARMED;
    return c == 1 ? CS_CONTENT : c ? CS_HOLLOW : CS_EMPTY;
}
/* a scene: 0 no clip in its row, 1 clips, 2 the one playing (stopped: the one Play starts): every track with a clip
 * in the row plays it and the others play nothing, 3 on its way (launched, waiting for the bar) */
static int scene_state(const S *s, int sn)
{
    int t, any = 0, is = 1, will = 1, wait = 0;
    for (t = 0; t < 16; t++) {
        const int has = s->clip[t][sn] != 0;
        const int now = s->active[t], then = s->queued[t] == NONE ? now : s->queued[t] == STOP ? -1 : s->queued[t];
        any |= has;
        wait |= s->queued[t] != NONE;
        if (has ? now != sn : now >= 0)
            is = 0;
        if (has ? then != sn : then >= 0)
            will = 0;
    }
    return !any ? 0 : wait && will ? 3 : !wait && is ? 2 : 1;
}

/* ---- the new cells (v2.c gets them): an armed clip (stopped: Play starts it) an outline with the play icon; an empty
 * clip its corners only; an empty clip that plays (or is armed): the corners and the icon */
static void corners(mr_canvas_t *b, int x, int y)
{
    v2_R(b, x, y, 3, 1, 1), v2_R(b, x, y, 1, 3, 1), v2_R(b, x + 11, y, 3, 1, 1), v2_R(b, x + 13, y, 1, 3, 1);
    v2_R(b, x, y + 12, 3, 1, 1), v2_R(b, x, y + 10, 1, 3, 1), v2_R(b, x + 11, y + 12, 3, 1, 1), v2_R(b, x + 13, y + 10, 1, 3, 1);
}
static void cell(mr_canvas_t *b, int c, int r, char s)
{
    const int x = c * 16 + 1, y = 4 + r * 15;
    if (s == 'A')
        v2_O(b, x, y, 14, 13, 1), v2_ic(b, V2I_PLAY, x + 2, y + 1, 1);
    else if (s == 'H' || s == 'h') {
        corners(b, x, y);
        if (s == 'h')
            v2_ic(b, V2I_PLAY, x + 2, y + 1, 1);
    } else
        v2_cell(b, c, r, s);
}

static void screen(const S *s, mr_canvas_t *cv)
{
    int c, r;
    mr_gfx_clear(cv, 0);
    if (s->info) {                                      /* a knob touched: its column's track, the whole screen */
        const int t = s->track_page + s->info - 1, sn = s->sel[t];
        char a[24], b[32];
        snprintf(a, sizeof a, "Track %d", t + 1);
        v2_TC(cv, 64, 2, a, 2, 1);
        if (s->mute[t])
            v2_ic(cv, V2I_MUTE, 114, 4, 1);
        if (v2_tw(s->fw[t], 2) <= 124)
            v2_TC(cv, 64, 21, s->fw[t], 2, 1);
        else
            v2_TC(cv, 64, 25, s->fw[t], 1, 1);
        if (s->clip[t][sn] == 1)
            snprintf(b, sizeof b, "Clip %d, 2 bars", sn + 1);
        else
            snprintf(b, sizeof b, "Clip %d, empty", sn + 1);
        v2_TC(cv, 64, 43, b, 1, 1);
        snprintf(b, sizeof b, "Channel %d", t + 1);
        v2_TC(cv, 64, 54, b, 1, 1);
        return;
    }
    for (c = 0; c < 7; c++) {
        const int t = s->track_page + c, x = c * 16 + 1;
        if (t == s->sel_track)
            v2_R(cv, x, 0, 14, 2, 1);
        else if (s->mute[t])
            v2_dotH(cv, x, 1, 14);
    }
    for (r = 0; r < 4; r++) {
        const int sn = s->scene_off + r, ss = scene_state(s, sn);
        char n[4];
        for (c = 0; c < 7; c++) {
            const int t = s->track_page + c, hollow = s->clip[t][sn] == 2;
            char ch = '.';
            switch (clip_state(s, t, sn)) {
            case CS_CONTENT: ch = 'C'; break;
            case CS_HOLLOW: ch = 'H'; break;
            case CS_ARMED: ch = hollow ? 'h' : 'A'; break;
            case CS_QUEUED: ch = s->bl ? (hollow ? 'H' : 'C') : 'Q'; break;
            case CS_PLAYING: ch = hollow ? 'h' : 'P'; break;
            case CS_STOPPING: ch = s->bl ? (hollow ? 'H' : 'C') : hollow ? 'h' : 'P'; break;
            default: break;
            }
            cell(cv, c, r, ch);
            if (t == s->sel_track && s->sel[t] == sn) {
                const int x = c * 16 + 1, y = 4 + r * 15;
                if (ch == 'P')
                    v2_O(cv, x, y, 14, 13, 0);
                v2_O(cv, x - 1, y - 1, 16, 15, 1);
            }
        }
        /* the 8th column: the scene's number (the one playing underlined; on its way: blinking) */
        snprintf(n, sizeof n, "%d", sn + 1);
        if (ss != 3 || s->bl)
            v2_TC(cv, 7 * 16 + 8, 4 + r * 15 + 3, n, 1, 1);
        if (ss == 2)
            v2_R(cv, 7 * 16 + 3, 4 + r * 15 + 11, 10, 1, 1);
    }
}

static void leds(const S *s, rgb pad[32], rgb step[16])
{
    const int pu = 256 - s->pu * (160 / 6);
    int i, t, anyplay = 0, allstop = 1;
    for (i = 0; i < 32; i++) {
        const int c = i % 8, sn = s->scene_off + (3 - i / 8);
        rgb v = OFF;
        if (c == 7) {                                   /* the scenes: green */
            switch (scene_state(s, sn)) {
            case 3: v = sc(GREEN, s->bl ? 256 : 40); break;
            case 2: v = s->playing ? sc(GREEN, pu) : GREEN; break;
            case 1: v = sc(GREEN, 70); break;
            default: v = sc(GREEN, 12); break;
            }
        } else {
            const int tr = s->track_page + c, col = s->mute[tr] ? 96 : 256, sel = tr == s->sel_track && s->sel[tr] == sn;
            const int hollow = s->clip[tr][sn] == 2;
            switch (clip_state(s, tr, sn)) {
            case CS_QUEUED: v = s->bl ? GREEN : sel ? sc(WHITE, 60) : sc(GREEN, 40); break;
            case CS_STOPPING: v = sc(TRACK[tr], s->bl ? col : 24); break;
            case CS_PLAYING:                            /* green, pulsing; the selected one white on the beat */
                v = sel && s->pu == 0 ? WHITE : sc(GREEN, pu * (hollow ? 70 : col) / 256);
                break;
            case CS_ARMED: v = sel ? sc(WHITE, 150) : sc(GREEN, hollow ? 40 : col); break;
            case CS_CONTENT: v = sel ? sc(WHITE, 150) : sc(TRACK[tr], 80 * col / 256); break;
            case CS_HOLLOW: v = sel ? sc(WHITE, 60) : sc(TRACK[tr], 22); break;
            default: v = sel ? sc(WHITE, 60) : OFF; break;
            }
        }
        pad[i] = v;
    }
    for (t = 0; t < 16; t++) {
        if ((s->active[t] >= 0 && s->playing) || s->queued[t] >= 0)
            anyplay = 1;
        if (s->active[t] >= 0 && s->playing && s->queued[t] != STOP)
            allstop = 0;
    }
    for (i = 0; i < 16; i++) {
        rgb v = OFF;
        if (i < 14) {
            const int tr = s->track_page + i / 2;
            const int plays = (s->active[tr] >= 0 && s->playing && s->clip[tr][s->active[tr]]) || s->queued[tr] >= 0;
            if (!(i & 1))                               /* select: the track's colour (the selected one full; muted: red) */
                v = s->mute[tr] ? sc((rgb){ 255, 0, 0 }, tr == s->sel_track ? 256 : 60) : sc(TRACK[tr], tr == s->sel_track ? 256 : 50);
            else                                        /* stop: white while it plays something to stop */
                v = plays ? (s->queued[tr] == STOP ? sc(WHITE, s->bl ? 256 : 30) : WHITE) : sc(WHITE, 8);
        } else if (i == 15)                             /* stop every clip */
            v = anyplay ? (allstop ? sc(WHITE, s->bl ? 256 : 30) : WHITE) : sc(WHITE, 8);
        step[i] = v;
    }
}

static void board(const char *name, const S *s)
{
    mr_canvas_t cv;
    rgb pad[32], step[16];
    char path[64];
    FILE *f;
    int x, y, i;
    screen(s, &cv);
    leds(s, pad, step);
    snprintf(path, sizeof path, "sessview2_%s.pbm", name);
    f = fopen(path, "w");
    fprintf(f, "P1\n128 64\n");
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 128; x++)
            fputc(cv.px[y][x] ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
    snprintf(path, sizeof path, "sessview2_%s.leds", name);
    f = fopen(path, "w");
    for (y = 3; y >= 0; y--) {                          /* (the top row first) */
        for (x = 0; x < 8; x++)
            fprintf(f, "%02x%02x%02x ", pad[y * 8 + x].r, pad[y * 8 + x].g, pad[y * 8 + x].b);
        fputc('\n', f);
    }
    for (i = 0; i < 16; i++)
        fprintf(f, "%02x%02x%02x ", step[i].r, step[i].g, step[i].b);
    fputc('\n', f);
    fclose(f);
}

int main(void)
{
    S s;
    int t;
    memset(&s, 0, sizeof s);
    for (t = 0; t < 16; t++)
        s.active[t] = -1, s.queued[t] = NONE, s.fw[t] = "Not routed";
    s.fw[0] = "ChoralRoot", s.fw[1] = "Felucca", s.fw[2] = "Felucca", s.fw[3] = "OberWave";
    /* the owner's grid of 2026-11-21, with notes: track 1: clips 1 2 3, track 2: 1 2, track 3: 1 3, track 4: 1 and an
     * empty clip 2 */
    s.clip[0][0] = s.clip[0][1] = s.clip[0][2] = 1, s.clip[1][0] = s.clip[1][1] = 1, s.clip[2][0] = s.clip[2][2] = 1;
    s.clip[3][0] = 1, s.clip[3][1] = 2;
    s.sel_track = 1;

    /* A: at rest. Row 1 is what Play starts */
    s.active[0] = s.active[1] = s.active[2] = s.active[3] = 0;
    board("rest", &s);
    /* B: playing */
    s.playing = 1, s.pu = 2;
    board("playing", &s);
    /* C: a clip pressed: track 1's clip 2 waits for the bar, selected at once */
    s.sel_track = 0, s.sel[0] = 1, s.queued[0] = 1, s.bl = 0;
    board("queued", &s);
    /* D: Shift + a clip: track 3's clip 3 selected, nothing launched */
    s.queued[0] = NONE, s.active[0] = 1, s.sel_track = 2, s.sel[2] = 2, s.pu = 0;
    board("shiftsel", &s);
    /* E: an empty slot pressed (track 2, row 3): selected, no clip made; the track's clip stops at the bar */
    s.sel_track = 1, s.sel[1] = 2, s.queued[1] = STOP, s.bl = 1, s.pu = 3;
    board("empty", &s);
    /* F: scene 2 pressed: on its way */
    s.queued[1] = NONE, s.active[1] = -1, s.sel[1] = 0;
    s.queued[0] = NONE, s.queued[1] = 1, s.queued[2] = STOP, s.queued[3] = 1, s.bl = 1;
    board("scene", &s);
    /* G: stop all (step 16): everything on its way out */
    s.queued[1] = NONE, s.queued[3] = NONE, s.active[1] = 1, s.active[2] = -1, s.active[3] = 1, s.queued[2] = NONE;
    s.queued[0] = s.queued[1] = s.queued[3] = STOP, s.bl = 1;
    board("stopall", &s);
    /* H: a knob touched: its track */
    s.queued[0] = s.queued[1] = s.queued[3] = NONE, s.info = 3, s.sel_track = 2;
    board("info", &s);
    return 0;
}
