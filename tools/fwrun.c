/* SPDX-License-Identifier: MPL-2.0 */
/* fwrun: a compiled firmware (.rfm1m) run offline through the host's own runtime (src/fwrt.c).
 *
 *   fwrun FW.rfm1m [--pcm OUT.raw] [--frame-ms 15]
 *       FM1VST rfm1_run's default run, call for call: a fresh flash, init, 4 s idle, keys 9 13 16 (D4 F#4 A4) at
 *       velocity 100 for 5 s, 200 ms more. OUT gets every block (s16 stereo interleaved), as rfm1_run --wav's data.
 *   fwrun FW.rfm1m --replay LOG --frames N [--flash-in IMG] --pcm OUT.raw
 *       the host's input log (remotefm1-host --log-inputs) replayed: N frames rendered with every logged input put
 *       in before the same device ms. The host's own capture of that instance must be the same bytes.
 * Prints one line: id version, the chord's RMS and peak, LCD writes, x real time, worst block, load and boot. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/fwrt.h"
#include "wasm-rt.h"

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint8_t *read_all(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    uint8_t *b;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(*n ? *n : 1);
    if (b && fread(b, 1, *n, f) != *n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    const char *path = NULL, *pcm_path = NULL, *replay = NULL, *flash_in = NULL;
    static fw_t fw;
    fw_obj_t *obj;
    char err[256];
    FILE *pcm = NULL;
    double t0, t_load, t_boot, wall, sumsq = 0, peak = 0;
    uint64_t frames = 0, chord_frames = 0, want_frames = 0;
    uint32_t lcd0;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pcm") && i + 1 < argc)
            pcm_path = argv[++i];
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc)
            replay = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            want_frames = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--flash-in") && i + 1 < argc)
            flash_in = argv[++i];
        else if (!path && argv[i][0] != '-')
            path = argv[i];
        else {
            fprintf(stderr, "usage: fwrun FW.rfm1m [--pcm OUT] [--replay LOG --frames N [--flash-in IMG]]\n");
            return 2;
        }
    }
    if (!path)
        return 2;
    wasm_rt_init();
    t0 = now_s();
    if (!(obj = fw_obj_open(path, err, sizeof err))) {
        fprintf(stderr, "fwrun: %s\n", err);
        return 1;
    }
    if (fw_create(&fw, obj) != 0) {
        fprintf(stderr, "fwrun: %s\n", fw.err);
        return 1;
    }
    fw_obj_close(obj);   /* (the instance holds it) */
    t_load = now_s() - t0;
    t0 = now_s();
    {
        size_t fn = 0;
        uint8_t *img = flash_in ? read_all(flash_in, &fn) : NULL;
        if (flash_in && !img) {
            fprintf(stderr, "fwrun: cannot read %s\n", flash_in);
            return 2;
        }
        lcd0 = fw_lcd_writes(&fw);
        if (fw_boot(&fw, img, (uint32_t)fn) != 0) {
            fprintf(stderr, "fwrun: boot: %s\n", fw.err);
            return 1;
        }
        free(img);
    }
    t_boot = now_s() - t0;
    if (pcm_path && !(pcm = fopen(pcm_path, "wb"))) {
        fprintf(stderr, "fwrun: cannot write %s\n", pcm_path);
        return 2;
    }
    t0 = now_s();
    if (replay) {
        size_t n = 0, k = 0;
        fw_log_t *log = (fw_log_t *)read_all(replay, &n);
        if (!log && n) {
            fprintf(stderr, "fwrun: cannot read %s\n", replay);
            return 2;
        }
        n /= sizeof *log;
        while (frames < want_frames) {
            int16_t blk[FW_BLOCK * 2];
            /* every input logged before a device ms this block will run: the ms's own due frame decides */
            while (k < n) {
                const uint64_t due = (uint64_t)log[k].ms * FW_FS / 1000u;
                if (log[k].ms > fw.ms + 4u)   /* (only the next few ms: a block is at most 3) */
                    break;
                fw_in(&fw, log[k].op, log[k].a, log[k].b, log[k].c, due);
                k++;
            }
            fw_render(&fw, blk);
            for (i = 0; i < FW_BLOCK * 2; i++) {
                const double v = blk[i] / 32768.0;
                sumsq += v * v;
                if (fabs(v) > peak)
                    peak = fabs(v);
            }
            chord_frames += FW_BLOCK;
            if (pcm)
                fwrite(blk, sizeof blk, 1, pcm);
            frames += FW_BLOCK;
        }
        free(log);
    } else {
        /* rfm1_run: the chord at 4000 ms (velocity, then the taps for keys newly down, then the keys), up at 9000 ms,
         * the run ends at 9200 ms: the blocks due by then */
        static const int keys[3] = { 9, 13, 16 };
        const uint64_t end = (uint64_t)9199 * FW_FS / 1000u;
        for (i = 0; i < 3; i++)
            fw_in(&fw, FWI_KEY, keys[i], 100, 0, (uint64_t)4000 * FW_FS / 1000u);
        for (i = 0; i < 3; i++)
            fw_in(&fw, FWI_KEY, keys[i], 0, 0, (uint64_t)9000 * FW_FS / 1000u);
        while (frames + FW_BLOCK <= end) {
            int16_t blk[FW_BLOCK * 2];
            const int on = fw.ms > 4000 && fw.ms <= 9000;   /* (the chord's blocks: rendered after a ms in it) */
            fw_render(&fw, blk);
            if (on || (fw.ms - 1 >= 4000 && fw.ms - 1 < 9000)) {
                for (i = 0; i < FW_BLOCK * 2; i++) {
                    const double v = blk[i] / 32768.0;
                    sumsq += v * v;
                    if (fabs(v) > peak)
                        peak = fabs(v);
                }
                chord_frames += FW_BLOCK;
            }
            if (pcm)
                fwrite(blk, sizeof blk, 1, pcm);
            frames += FW_BLOCK;
        }
    }
    wall = now_s() - t0;
    if (pcm)
        fclose(pcm);
    printf("%s %s: rms %.4f (peak %.3f), %u LCD writes, %.1fx real time, worst block %u us, load %.1f ms, boot %.1f ms%s\n",
           fw.id, fw.version, chord_frames ? sqrt(sumsq / (double)(chord_frames * 2)) : 0.0, peak,
           fw_lcd_writes(&fw) - lcd0, (double)frames / FW_FS / wall, fw.worst_us, t_load * 1000, t_boot * 1000,
           fw.dead ? " (DEAD)" : "");
    if (fw.dead)
        fprintf(stderr, "fwrun: %s\n", fw.err);
    fw_destroy(&fw);
    return fw.halted || chord_frames == 0 ? 1 : 0;
}
