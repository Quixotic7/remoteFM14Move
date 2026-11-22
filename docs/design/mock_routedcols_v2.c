/* SPDX-License-Identifier: MPL-2.0 */
/* Session view: other ways to show the columns whose track does not reach the firmware played (the owner on the
 * built look, dotted cell outlines, 2026-11-22: "Don't love the look of this"). Mock-ups, before any code, with the
 * core's own v2 primitives. The grid is the build's test picture (columns 2 and 5 not routed).
 * Writes sessview_rc_<name>.pbm into the working directory.
 *   cc -std=c99 -I<core>/src -I<core>/third_party/moveremote/include mock_routedcols_v2.c <core>/src/v2.c \
 *      <core>/third_party/moveremote/host/mr_gfx.c <core>/third_party/moveremote/common/font5x7.c -o mock && ./mock */
#include <stdio.h>
#include <string.h>
#include "v2.h"

/* [row][column]: P playing, C a clip, Q queued, . empty */
static const char *const GRID[4] = { "PPPPQ..", "CC.CP..", "C.C....", "......." };
static const int ROUTED[7] = { 1, 0, 1, 1, 0, 1, 1 };
enum { SEL = 4 };

static void dump(const mr_canvas_t *cv, const char *name)
{
    char path[96];
    FILE *f;
    int x, y;
    snprintf(path, sizeof path, "sessview_rc_%s.pbm", name);
    f = fopen(path, "w");
    fprintf(f, "P1\n128 64\n");
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 128; x++)
            fputc(cv->px[y][x] ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
}

/* a small cell (a column that is not routed): 10 x 9 in the middle of the cell's place */
static void small(mr_canvas_t *b, int c, int r, char s)
{
    const int x = c * 16 + 3, y = 4 + r * 15 + 2;
    if (s == '.')
        v2_R(b, x + 4, y + 3, 2, 2, 1);
    else if (s == 'P') {
        int i;
        v2_R(b, x, y, 10, 9, 1);
        for (i = 0; i < 3; i++)                         /* a small play mark, black */
            v2_R(b, x + 3 + i, y + 2 + i, 1, 5 - 2 * i, 0);
    } else if (s == 'Q')
        v2_O(b, x, y, 10, 9, 1), v2_O(b, x + 2, y + 2, 6, 5, 1);
    else
        v2_O(b, x, y, 10, 9, 1);
}

/* mode: 0 the cells plain (the pads alone say it), 1 as built (dotted outlines), 2 a line over each routed column,
 * 3 small cells for the columns that are not routed */
static void board(const char *name, int mode)
{
    mr_canvas_t cv;
    int c, r;
    mr_gfx_clear(&cv, 0);
    for (c = 0; c < 7; c++) {
        const int x = c * 16 + 1;
        if (c == SEL)
            v2_R(&cv, x, 0, 14, 2, 1);
        else if (mode == 2 && ROUTED[c])
            v2_R(&cv, x + 4, 0, 6, 1, 1);
    }
    for (r = 0; r < 4; r++) {
        char n[2] = { (char)('1' + r), 0 };
        for (c = 0; c < 7; c++) {
            if (mode == 3 && !ROUTED[c])
                small(&cv, c, r, GRID[r][c]);
            else
                v2_cell(&cv, c, r, GRID[r][c]);
            if (mode == 1 && !ROUTED[c] && GRID[r][c] != '.')
                v2_cellDots(&cv, c, r);
        }
        if (r == 0)
            v2_R(&cv, 7 * 16 + 5, 4 + 3, 8, 8, 1);       /* (scene 1 plays) */
        else
            v2_T(&cv, 7 * 16 + 6, 4 + r * 15 + 3, n, 1, 1);
    }
    dump(&cv, name);
}

int main(void)
{
    board("plain", 0);
    board("dotted", 1);
    board("line", 2);
    board("small", 3);
    return 0;
}
