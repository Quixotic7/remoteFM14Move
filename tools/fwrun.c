/* SPDX-License-Identifier: MPL-2.0 */
/* fwrun: a compiled firmware (.rfm1m) run offline through the host's own runtime (src/fwrt.c).
 *
 *   fwrun FW.rfm1m [--pcm OUT.raw] [--frame-ms 15]
 *       FM1VST rfm1_run's default run, call for call: a fresh flash, init, 4 s idle, keys 9 13 16 (D4 F#4 A4) at
 *       velocity 100 for 5 s, 200 ms more. OUT gets every block (s16 stereo interleaved), as rfm1_run --wav's data;
 *       --settle MS and --seconds S move the chord as rfm1_run's do.
 *   fwrun FW.rfm1m ... [--folder DIR] [--press LABEL@MS[+D]].. [--ui]
 *       --folder: a Fantasy CFW's RemoteFM1 folder (rfm1_host_1, src/fsvc.c), as FM1VST rfm1_run's --folder: audio/,
 *       code/<id>/, data/<id>/ under DIR, each request answered one device ms after it was made (so the run is the
 *       same as rfm1_run --folder's, byte for byte). Without it the file API answers FS_ENOFOLDER.
 *       --press: the firmware's button LABEL (its own name, else rfm1_run's: PLAY, SAVE, OCT+ ..) held from device ms MS
 *       for D ms (60).
 *       --ui: the firmware's ui_info at the end. The line then says what the file API did.
 *   fwrun FW.rfm1m ... [--audio-in F.wav [--audio-in-latency N] [--audio-in-once]]
 *       ain1 (FM1VST docs/VIRTUAL-CARTS.md section 8), as rfm1_run --audio-in: the WAV fed through rfm1_audio_in right
 *       before each rfm1_audio, the same frames, looped (once: then no more calls), after rfm1_audio_in_info(1, N)
 *       (the host's runtime: fw_ain_info, fw_render_in). Only when the manifest lists ain1 and the module exports
 *       rfm1_audio_in (else said, and the run is without input).
 *   fwrun FW.rfm1m --replay LOG --frames N [--flash-in IMG] --pcm OUT.raw
 *       the host's input log (remotefm1-host --log-inputs) replayed: N frames rendered with every logged input put
 *       in before the same device ms. The host's own capture of that instance must be the same bytes.
 * Prints one line: id version, the chord's RMS and peak, LCD writes, x real time, worst block, load and boot. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "../src/fsvc.h"
#include "../src/fwrt.h"
#include "../src/wav.h"
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

/* the next input block of the looped (or once-played) WAV; NULL: no more input (--audio-in-once, played) */
static const int16_t *ain_next(const int16_t *pcm, uint32_t n, uint32_t *pos, int once, int16_t blk[FW_BLOCK * 2])
{
    int i;
    if (!pcm || (once && *pos >= n))
        return NULL;
    for (i = 0; i < FW_BLOCK; i++) {
        int16_t l = 0, r = 0;
        if (*pos < n) {
            l = pcm[2 * *pos];
            r = pcm[2 * *pos + 1];
            if (++*pos >= n && !once)
                *pos = 0;
        }
        blk[2 * i] = l, blk[2 * i + 1] = r;
    }
    return blk;
}

int main(int argc, char **argv)
{
    const char *path = NULL, *pcm_path = NULL, *replay = NULL, *flash_in = NULL, *folder = NULL;
    struct { char label[16]; uint32_t at, dur; int b; } press[16];
    int npress = 0, want_ui = 0, ain_once = 0;
    uint32_t settle = 4000, seconds = 5;
    const char *ain_path = NULL;
    uint32_t ain_lat = 0, ain_frames = 0, ain_pos = 0;
    int16_t *ain = NULL;
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
        else if (!strcmp(argv[i], "--folder") && i + 1 < argc)
            folder = argv[++i];
        else if (!strcmp(argv[i], "--settle") && i + 1 < argc)
            settle = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--audio-in") && i + 1 < argc)
            ain_path = argv[++i];
        else if (!strcmp(argv[i], "--audio-in-latency") && i + 1 < argc)
            ain_lat = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--audio-in-once"))
            ain_once = 1;
        else if (!strcmp(argv[i], "--ui"))
            want_ui = 1;
        else if (!strcmp(argv[i], "--press") && i + 1 < argc && npress < 16) {
            const char *a = argv[++i], *at = strchr(a, '@'), *plus;
            if (!at || at == a || at - a >= 16)
                return 2;
            snprintf(press[npress].label, sizeof press[npress].label, "%.*s", (int)(at - a), a);
            press[npress].at = (uint32_t)strtoul(at + 1, NULL, 10);
            press[npress].dur = (plus = strchr(at, '+')) ? (uint32_t)strtoul(plus + 1, NULL, 10) : 60u;
            npress++;
        }
        else if (!path && argv[i][0] != '-')
            path = argv[i];
        else {
            fprintf(stderr, "usage: fwrun FW.rfm1m [--pcm OUT] [--replay LOG --frames N [--flash-in IMG]] [--folder DIR]"
                            " [--press LABEL@MS[+D]].. [--ui] [--audio-in F.wav [--audio-in-latency N] [--audio-in-once]]\n");
            return 2;
        }
    }
    if (!path)
        return 2;
    wasm_rt_init();
    if (folder) {
        struct stat dst;
        if (stat(folder, &dst) != 0 || !S_ISDIR(dst.st_mode)) {
            fprintf(stderr, "fwrun: --folder %s: not a folder\n", folder);
            return 2;
        }
        fsvc_set_root(folder);   /* (no dispatcher: each request done one device ms late, in the pump) */
    }
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
    if (ain_path) {   /* (right after init, as rfm1_run: the input buffer, then rfm1_audio_in_info(1, latency)) */
        const char *why = NULL;
        if (!(ain = wav_read(ain_path, &ain_frames, &why))) {
            fprintf(stderr, "fwrun: --audio-in %s: %s\n", ain_path, why);
            return 2;
        }
        if (!fw_has_ain(&fw)) {
            fprintf(stderr, "fwrun: --audio-in %s ignored: %s\n", ain_path,
                    !fw.obj->ain1 ? "the manifest has no ain1 in wants or needs" : "the module exports no rfm1_audio_in");
            free(ain);
            ain = NULL;
        } else if (fw_ain_info(&fw, 1, ain_lat) != 0) {
            fprintf(stderr, "fwrun: rfm1_audio_in_info: %s\n", fw.err);
            return 1;
        }
    }
    for (i = 0; i < npress; i++) {   /* a label -> its matrix button (fw_panel_ids: label order -> matrix) */
        uint8_t btn_id[FW_NBTN], enc_id[FW_NENC];
        int8_t enc_dir[FW_NENC];
        int k;
        fw_panel_ids(&fw, btn_id, enc_id, enc_dir);
        press[i].b = -1;
        static const char *const STD[FW_NBTN] = { "FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP",
                                                  "SEQ", "PLAY", "REC", "OCT-", "OCT+" };   /* (rfm1_run's labels) */
        for (k = 0; k < FW_NBTN; k++)
            if (!strcasecmp(fw.btn_name[k], press[i].label))
                press[i].b = btn_id[k];
        for (k = 0; k < FW_NBTN && press[i].b < 0; k++)
            if (!strcasecmp(STD[k], press[i].label))
                press[i].b = btn_id[k];
        if (press[i].b < 0) {
            fprintf(stderr, "fwrun: --press %s: the firmware has no such button\n", press[i].label);
            return 2;
        }
        fw_in(&fw, FWI_BTN, press[i].b, 1, 0, (uint64_t)press[i].at * FW_FS / 1000u);
        fw_in(&fw, FWI_BTN, press[i].b, 0, 0, (uint64_t)(press[i].at + press[i].dur) * FW_FS / 1000u);
    }
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
            int16_t ib[FW_BLOCK * 2];
            fw_render_in(&fw, blk, ain_next(ain, ain_frames, &ain_pos, ain_once, ib));
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
        /* rfm1_run: the chord at --settle (4000 ms: velocity, then the taps for keys newly down, then the keys), up
         * --seconds later (5), the run ends 200 ms after: the blocks due by then */
        static const int keys[3] = { 9, 13, 16 };
        const uint32_t up = settle + seconds * 1000u;
        const uint64_t end = (uint64_t)(up + 199u) * FW_FS / 1000u;
        for (i = 0; i < 3; i++)
            fw_in(&fw, FWI_KEY, keys[i], 100, 0, (uint64_t)settle * FW_FS / 1000u);
        for (i = 0; i < 3; i++)
            fw_in(&fw, FWI_KEY, keys[i], 0, 0, (uint64_t)up * FW_FS / 1000u);
        while (frames + FW_BLOCK <= end) {
            int16_t blk[FW_BLOCK * 2];
            const int on = fw.ms > settle && fw.ms <= up;   /* (the chord's blocks: rendered after a ms in it) */
            int16_t ib[FW_BLOCK * 2];
            fw_render_in(&fw, blk, ain_next(ain, ain_frames, &ain_pos, ain_once, ib));
            if (on || (fw.ms - 1 >= settle && fw.ms - 1 < up)) {
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
    if (frames)   /* where a block's time went: the sound, the clock, the screen (frame: every FW_FRAME_MS) */
        printf("  per block of %d frames: %.1f us audio, %.1f us tick + idle, %.1f us frame (%.0f us a frame call)\n",
               FW_BLOCK, (double)fw.ns_audio / 1000.0 / (double)(frames / FW_BLOCK),
               (double)fw.ns_tick / 1000.0 / (double)(frames / FW_BLOCK),
               (double)fw.ns_frame / 1000.0 / (double)(frames / FW_BLOCK),
               fw.n_frame ? (double)fw.ns_frame / 1000.0 / fw.n_frame : 0.0);
    {   /* core 1 (cpu2): its jobs, those the module ran itself, the time */
        fw_c1_stats_t c1;
        fw_c1_stats(&fw, &c1);
        if (c1.state || c1.jobs || c1.inline_jobs)
            printf("  core1: state %d, %llu jobs (%.1f us each, worst %u us), %llu inline, core 0 waited %.1f us a job\n",
                   c1.state, (unsigned long long)c1.jobs, c1.jobs ? (double)c1.job_ns / 1000.0 / (double)c1.jobs : 0.0,
                   c1.job_worst_us, (unsigned long long)c1.inline_jobs,
                   c1.jobs ? (double)c1.wait_ns / 1000.0 / (double)c1.jobs : 0.0);
    }
    if (fw.wasi && fw.wasi->fs) {
        fsvc_stats_t st;
        fsvc_stats(fw.wasi->fs, &st);
        printf("  files: %u requests, %u answered, %u refused, %u busy, %u in flight; %llu bytes read, %llu written\n",
               st.made, st.served, st.refused, st.busy, st.inflight, (unsigned long long)st.bytes_in,
               (unsigned long long)st.bytes_out);
    }
    if (ain_path)
        printf("  audio in: %u blocks fed, latency %u frames\n", fw.ain_blocks, fw.ain_lat);
    if (want_ui) {
        char ui[1024];
        fw_ui_info(&fw, ui, sizeof ui);
        printf("  ui: %s\n", ui);
    }
    if (fw.dead)
        fprintf(stderr, "fwrun: %s\n", fw.err);
    fw_destroy(&fw);
    free(ain);
    return fw.halted || chord_frames == 0 ? 1 : 0;
}
