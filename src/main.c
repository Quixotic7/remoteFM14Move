/* SPDX-License-Identifier: MPL-2.0 */
/* remotefm1-host: FM-1 firmwares on the Ableton Move. Move Remote's device code owns the hardware (plat_move.c: the
 * SPI link and the real-time audio thread on core 3; plat_sim.c on a computer), in this process, with no network
 * listener of its own; the engine (engine.c) is its one host, through a socketpair, and its audio source.
 *
 *   remotefm1-host [--root DIR] [--port N] [--no-mdns] [--lead BLOCKS] [--fw ID]
 *                  [--sim-dir DIR] [--capture DIR --capture-seconds S]       (the simulator's tests)
 *     --root        the RemoteFM1 folder (default /data/UserData/RemoteFM1; the simulator: $TMPDIR/remotefm1-sim)
 *     --port        the page's port (default 7730)
 *     --no-mdns     no remotefm1.local answers (the simulator: off unless --mdns)
 *     --lead        blocks rendered ahead, 4..8 (default 5)
 *     --fw          the firmware slot 1 gets when nothing was loaded before (default felucca)
 *     --firmwares   the bundled firmwares to install from (default: firmwares/ beside this program)
 *     --frame-ms A[,B]  frame() every A device ms for the slot on screen, B for the others (default 33,200; 15,15:
 *                   the ABI's every 15 ms: README "Frames")
 *     --dev-firmwares  development builds to install, over the same <id>-<version> when they differ (default:
 *                   firmwares-dev/ beside this program: make package DEV_FIRMWARES=..)
 *     --site        the page's files (scripts/build_site.sh; default: site/ beside this program)
 *     --catalog-index   the catalog's snapshot (default: catalog/index.json beside this program)
 *     --catalog-url     the catalog's base URL when the root's catalog-url file names none (default: catalog.h's)
 *     --ca-dir      (tests; RFM14_TEST_BUILD only) the CA certificates' directory instead of /etc/ssl/certs
 *     --log-cap     stderr's file is moved to <log>.1 past this many bytes (default 1 MB; -1 never)
 *     --fx-dir      the Schwung modules hosted as MIDI FX and audio FX (midi_fx/, audio_fx/; default
 *                   /data/UserData/schwung/modules on the Move, none on the simulator)
 *     --fx-budget   an FX module's CPU budget, permille of a core (default 200); past it the module is refused
 *     --test-api    (tests) POST /api/test/midi: sequencer MIDI from outside
 *     --fx-probe KIND DIR SO   (internal) load one FX module in this child process and exit 0 (fx.c's crash probe)
 *     --threads 0   every slot rendered on the engine's thread (default 1: a render thread per slot, README "Threads")
 *     --no-cpu2     no core-1 thread for the firmwares with cpu2 (they run their core-1 jobs themselves)
 *     --no-rt-threads   the engine's, the slots' and core 1's threads at normal priority (default: bin/heal asked for
 *                   SCHED_FIFO, as Move Remote asks for its audio thread; the Move only)
 *     --no-fm1      no FM-1 link on USB-A (fm1link.c; USB-A stays Move Remote's)
 *     --fm1-bps     the FM-1's screen, TILES body bytes a second (default fm1link.h FM1LINK_BPS_DEFAULT)
 *     --fm1-cap     USB-A packets per SPI transfer (default FM1LINK_CAP_DEFAULT; the rest of a transfer is the LEDs')
 *     --fm1-usba-screen the firmware's screen over USB-A too (default: the FM-1 shows a plug on black; USB-A is ~8 KB/s)
 *     --no-fm1-wifi no search for an FM-1 over Wi-Fi (RemoteFM1 Wi-Fi; fm1link.c: USB-A only)
 *     --fm1-addr    an FM-1 over Wi-Fi to ask and connect to, HOST[:PORT] (default: the broadcast on UDP 7741 and the
 *                   address last linked, ROOT/fm1-wifi.txt)
 *     --no-audio-in no input for the Fantasy CFWs with ain1 (Move Remote's input stays its own)
 *     --sim-audio-in F.wav   (the simulator) the Move's input: the WAV, looped (wav.h), in place of its 440 Hz tone
 *     --no-audio-in no input for the Fantasy CFWs with ain1 (Move Remote's own input path stays as it was)
 *     --sim-audio-in F.wav   (the simulator) the Move's input: the WAV (wav.h), looped, in place of its 440 Hz tone
 *     --screen-reader on|off|auto   Schwung's screen reader (announce.h): auto (the Move's default): its sidecar runs,
 *                   RemoteFM1's Speech mode (Global Settings) says when, Schwung's setting its default; on: the
 *                   default On; off: no sidecar at all; the simulator: off unless on or auto
 *     --tts-sidecar Schwung's speech program (default /data/UserData/schwung/schwung-tts; tests: tests/fake_tts.c)
 *     --tts-shm     its shared memory (default /schwung-tts)   --tts-config  Schwung's config folder (the setting, tts.json)
 *     --rtp-port N  network MIDI (midiout.h: RTP-MIDI "RemoteFM1 Move", control N, data N + 1; default 5004, the
 *                   simulator: none unless given)   --no-rtp  none
 *     --midi-out    the active slot's MIDI out to Out (USB-A and network MIDI; until the routing says who goes there)
 *     --midi-out-clock   Out sends the real-time bytes too (F8 clock, start, stop ...; default: dropped)
 *     --fake-fm1    (the simulator) a fake FM-1 on the simulated USB-A (fm1sim.c; the tests' /api/test/fm1)
 *     --sim-dir     the simulator's test files (mr-cmd, mr-state, mr-screen.pbm)
 *     --capture     the audio thread's output (out.raw), the mix (mix.raw), the first firmware's own blocks (raw.raw)
 *                   and its input log (inputs.log), written at exit */
#define _GNU_SOURCE
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "announce.h"
#include "engine.h"
#include "fwrt.h"
#include "fm1link.h"
#ifdef RFM14_PLAT_SIM
#include "fm1sim.h"
#endif
#include "fx.h"
#include "https.h"
#include "mdns.h"
#include "midiout.h"
#include "mr_core.h"
#include "wav.h"
#include "web.h"

int mr_plat_main(int argc, char **argv);

#define VERSION "0.2.0"

int main(int argc, char **argv)
{
    static char dev[PATH_MAX];
    const char *dev_arg = NULL;
    static char root[PATH_MAX], bundled[PATH_MAX], site[PATH_MAX], cap[4][PATH_MAX], index[PATH_MAX];
    const char *sim_dir = NULL, *capture = NULL, *fw = "felucca", *firmwares = NULL, *site_arg = NULL;
    const char *index_arg = NULL, *url = NULL, *fx_dir = NULL;
    int frame_active = 0, frame_bg = 0;
    int fx_budget = 0, test_api = 0, fm1_off = 0, fm1_cap = 0, fake_fm1 = 0, no_ain = 0, fm1_wifi_off = 0, fm1_usba_screen = 0;
    int threads = 1, cpu2 = 1, rt_threads = 1, midi_out = 0, midi_clock = 0;
    char heal[PATH_MAX];
    const char *fm1_addr = NULL;
    const char *sim_ain = NULL;
    long fm1_bps = 0;
    long log_cap = 0;
    char *pargv[4];
    int pargc = 0, i, port = 7730, lead = 5, cap_s = 0, rc;
    static char banner_title[32], banner_name[64];
    ann_cfg_t ann;
    if (argc == 5 && !strcmp(argv[1], "--fx-probe"))   /* (fx.c: a module loaded here first, in a child) */
        return fx_probe_main(atoi(argv[2]), argv[3], argv[4]);
    memset(&ann, 0, sizeof ann);
#ifdef RFM14_PLAT_SIM
    ann.mode = -1;   /* (the simulator: no Schwung) */
#endif
#ifdef RFM14_PLAT_SIM
    int mdns = 0, splash = 0, rtp_port = -1;
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/remotefm1-sim", tmp && *tmp ? tmp : "/tmp");
#else
    int mdns = 1, splash = 3000;   /* the address on the Move's screen before the controls take it */
    int rtp_port = MIDIOUT_RTP_PORT;
    snprintf(root, sizeof root, "/data/UserData/RemoteFM1");
    fx_dir = "/data/UserData/schwung/modules";
#endif
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc)
            snprintf(root, sizeof root, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lead") && i + 1 < argc)
            lead = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fw") && i + 1 < argc)
            fw = argv[++i];
        else if (!strcmp(argv[i], "--firmwares") && i + 1 < argc)
            firmwares = argv[++i];
        else if (!strcmp(argv[i], "--dev-firmwares") && i + 1 < argc)
            dev_arg = argv[++i];
        else if (!strcmp(argv[i], "--site") && i + 1 < argc)
            site_arg = argv[++i];
        else if (!strcmp(argv[i], "--catalog-index") && i + 1 < argc)
            index_arg = argv[++i];
        else if (!strcmp(argv[i], "--catalog-url") && i + 1 < argc)
            url = argv[++i];
#ifdef RFM14_TEST_BUILD
        else if (!strcmp(argv[i], "--ca-dir") && i + 1 < argc)
            https_ca_dir(argv[++i]);
#endif
        else if (!strcmp(argv[i], "--log-cap") && i + 1 < argc)
            log_cap = atol(argv[++i]);
        else if (!strcmp(argv[i], "--splash") && i + 1 < argc)
            splash = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fx-dir") && i + 1 < argc)
            fx_dir = argv[++i];
        else if (!strcmp(argv[i], "--fx-budget") && i + 1 < argc)
            fx_budget = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frame-ms") && i + 1 < argc) {   /* ACTIVE[,BACKGROUND] */
            const char *v = argv[++i], *cm = strchr(v, ',');
            frame_active = atoi(v);
            frame_bg = cm ? atoi(cm + 1) : frame_active;
        }
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
            threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-cpu2"))
            cpu2 = 0;
        else if (!strcmp(argv[i], "--no-rt-threads"))
            rt_threads = 0;
        else if (!strcmp(argv[i], "--no-fm1"))
            fm1_off = 1;
        else if (!strcmp(argv[i], "--fm1-bps") && i + 1 < argc)
            fm1_bps = atol(argv[++i]);
        else if (!strcmp(argv[i], "--fm1-cap") && i + 1 < argc)
            fm1_cap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-fm1-wifi"))
            fm1_wifi_off = 1;
        else if (!strcmp(argv[i], "--fm1-usba-screen"))
            fm1_usba_screen = 1;
        else if (!strcmp(argv[i], "--fm1-addr") && i + 1 < argc)
            fm1_addr = argv[++i];
        else if (!strcmp(argv[i], "--no-audio-in"))
            no_ain = 1;
        else if (!strcmp(argv[i], "--sim-audio-in") && i + 1 < argc)
            sim_ain = argv[++i];
        else if (!strcmp(argv[i], "--screen-reader") && i + 1 < argc) {
            const char *v = argv[++i];
            ann.mode = !strcmp(v, "on") ? 1 : !strcmp(v, "off") ? -1 : 0;
        } else if (!strcmp(argv[i], "--tts-sidecar") && i + 1 < argc)
            ann.sidecar = argv[++i];
        else if (!strcmp(argv[i], "--tts-shm") && i + 1 < argc)
            ann.shm = argv[++i];
        else if (!strcmp(argv[i], "--tts-config") && i + 1 < argc)
            ann.config_dir = argv[++i];
        else if (!strcmp(argv[i], "--rtp-port") && i + 1 < argc)
            rtp_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-rtp"))
            rtp_port = -1;
        else if (!strcmp(argv[i], "--midi-out"))
            midi_out = 1;
        else if (!strcmp(argv[i], "--midi-out-clock"))
            midi_clock = 1;
        else if (!strcmp(argv[i], "--fake-fm1"))
            fake_fm1 = 1;
        else if (!strcmp(argv[i], "--test-api"))
            test_api = 1;
        else if (!strcmp(argv[i], "--no-mdns"))
            mdns = 0;
        else if (!strcmp(argv[i], "--mdns"))
            mdns = 1;
        else if (!strcmp(argv[i], "--sim-dir") && i + 1 < argc)
            sim_dir = argv[++i];
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc)
            capture = argv[++i];
        else if (!strcmp(argv[i], "--capture-seconds") && i + 1 < argc)
            cap_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--version")) {
            printf("remotefm1-host %s (Move Remote %s)\n", VERSION, MR_VERSION);
            return 0;
        } else {
            fprintf(stderr, "usage: remotefm1-host [--root DIR] [--port N] [--no-mdns] [--splash MS] [--lead N] [--fw ID] "
                            "[--firmwares DIR] [--site DIR] [--catalog-index FILE] [--catalog-url URL] [--log-cap BYTES] "
                            "[--dev-firmwares DIR] [--frame-ms A[,B]] [--fx-dir DIR] [--fx-budget PERMILLE] [--test-api] [--threads 0|1] [--no-cpu2] [--no-rt-threads] [--no-fm1] [--fm1-bps N] [--fm1-cap N] [--no-fm1-wifi] [--fm1-addr HOST[:PORT]] [--fm1-usba-screen] [--fake-fm1] "
                            "[--rtp-port N] [--no-rtp] [--midi-out] [--midi-out-clock] "
                            "[--screen-reader on|off|auto] [--tts-sidecar PATH] [--tts-shm NAME] [--tts-config DIR] "
                            "[--no-audio-in] [--sim-audio-in F.wav] [--sim-dir DIR] [--capture DIR --capture-seconds S]\n");
            return 2;
        }
    }
    /* the bundled firmwares: the module's firmwares/ beside this program */
    if (firmwares)
        snprintf(bundled, sizeof bundled, "%s", firmwares);
    else if (realpath(argv[0], bundled)) {
        char *slash = strrchr(bundled, '/');
        if (slash)
            snprintf(slash, sizeof bundled - (size_t)(slash - bundled), "/firmwares");
    } else
        bundled[0] = 0;
    /* development builds to try: the module's firmwares-dev/ beside this program (make package DEV_FIRMWARES=..) */
    if (dev_arg)
        snprintf(dev, sizeof dev, "%s", dev_arg);
    else if (realpath(argv[0], dev)) {
        char *slash = strrchr(dev, '/');
        if (slash)
            snprintf(slash, sizeof dev - (size_t)(slash - dev), "/firmwares-dev");
    } else
        dev[0] = 0;
    /* the catalog's snapshot: the module's catalog/index.json beside this program */
    if (index_arg)
        snprintf(index, sizeof index, "%s", index_arg);
    else if (realpath(argv[0], index)) {
        char *slash = strrchr(index, '/');
        if (slash)
            snprintf(slash, sizeof index - (size_t)(slash - index), "/catalog/index.json");
    } else
        index[0] = 0;
    /* the page's files: the module's site/ beside this program */
    if (site_arg)
        snprintf(site, sizeof site, "%s", site_arg);
    else if (realpath(argv[0], site)) {
        char *slash = strrchr(site, '/');
        if (slash)
            snprintf(slash, sizeof site - (size_t)(slash - site), "/site");
    } else
        site[0] = 0;
    {
        engine_cfg_t c;
        memset(&c, 0, sizeof c);
        c.root = root;
        c.bundled = bundled[0] ? bundled : NULL;
        c.dev_bundled = dev[0] ? dev : NULL;
        c.default_fw = fw;
        c.lead_blocks = lead;
        c.splash_ms = (uint32_t)(splash < 0 ? 0 : splash);
        c.bundled_index = index[0] ? index : NULL;
        c.catalog_url = url;
        c.log_cap = log_cap;
        c.fx_dir = fx_dir;
        c.fx_budget = fx_budget;
        c.frame_ms_active = frame_active;
        c.frame_ms_bg = frame_bg;
        c.test_api = test_api;
        c.fm1_off = fm1_off;
        c.fm1_bps = (uint32_t)(fm1_bps > 0 ? fm1_bps : 0);
        c.fm1_cap = fm1_cap;
        c.fm1_wifi_off = fm1_wifi_off;
        c.fm1_usba_screen = fm1_usba_screen;
        c.fm1_addr = fm1_addr;
        c.serial = !threads;
        c.midi_out = midi_out;
        fw_cpu2_enable(cpu2);
#ifdef RFM14_PLAT_MOVE
        c.cpu_mask = 0x7;   /* cores 0..2: core 3 is the SPI's */
        if (rt_threads && realpath(argv[0], heal)) {   /* (the module's bin/heal, beside this program, as Move Remote's) */
            char *slash = strrchr(heal, '/');
            if (slash) {
                snprintf(slash, sizeof heal - (size_t)(slash - heal), "/bin/heal");
                c.heal = heal;
            }
        }
#else
        (void)rt_threads, (void)heal;
#endif
        if (capture) {
            snprintf(cap[0], sizeof cap[0], "%s/out.raw", capture);
            snprintf(cap[1], sizeof cap[1], "%s/mix.raw", capture);
            snprintf(cap[2], sizeof cap[2], "%s/raw.raw", capture);
            snprintf(cap[3], sizeof cap[3], "%s/inputs.log", capture);
            c.capture_out = cap[0], c.capture_mix = cap[1], c.capture_raw = cap[2], c.log_inputs = cap[3];
            c.capture_seconds = (uint32_t)(cap_s > 0 ? cap_s : 60);
        }
        fprintf(stderr, "remotefm1-host %s: root %s\n", VERSION, root);
        {   /* Out (midiout.h): before the engine, whose block sends to it; its service named for mDNS */
            midiout_cfg_t mo;
            memset(&mo, 0, sizeof mo);
            mo.rtp_port = rtp_port > 0 && rtp_port < 65535 ? rtp_port : -1;
            mo.usba = !fm1_off;
            mo.usba_busy = fm1link_usba_busy;
            mo.realtime = midi_clock;
            if (midiout_start(&mo) != 0)
                fprintf(stderr, "remotefm1-host: no MIDI out\n");
        }
        ann_start(&ann);   /* (before the engine: its sidecar spawned while the process is small, no real-time threads) */
        signal(SIGPIPE, SIG_IGN);
        if (engine_start(&c) != 0) {
            ann_stop();
            fprintf(stderr, "remotefm1-host: the engine did not start\n");
            return 1;
        }
    }
    if (web_start(port, site) != 0)
        fprintf(stderr, "web: cannot listen on port %d: no page\n", port);
    if (mdns)
        mdns_start("remotefm1.local");
    /* Move Remote's platform: no listener of its own, our audio; it runs until the exit gesture, a power off or a
     * signal */
    snprintf(banner_title, sizeof banner_title, "REMOTEFM1 %s", VERSION);
    snprintf(banner_name, sizeof banner_name, "remotefm1.local:%d", port);
    mr_set_banner(banner_title, banner_name, port);
    mr_set_port(-1);
    mr_set_audio_source(engine_audio, NULL);
    if (!no_ain)
        mr_set_audio_in_sink(engine_audio_in, NULL);   /* (ain1: the Move's input for the carts that take it) */
    pargv[pargc++] = argv[0];
#ifdef RFM14_PLAT_SIM
    if (sim_dir) {
        pargv[pargc++] = "--dir";
        pargv[pargc++] = (char *)sim_dir;
    }
#else
    (void)sim_dir;
#endif
    pargv[pargc] = NULL;
#ifdef RFM14_PLAT_SIM
    if (sim_ain) {   /* (the input pcm stays until the end: Move Remote's simulator plays it looped) */
        const char *why = NULL;
        uint32_t nf = 0;
        int16_t *pcm = wav_read(sim_ain, &nf, &why);
        if (!pcm)
            fprintf(stderr, "remotefm1-host: --sim-audio-in %s: %s\n", sim_ain, why);
        else {
            mr_sim_set_audio_in(pcm, nf);
            fprintf(stderr, "remotefm1-host: the simulator's input: %s, %u frames, looped\n", sim_ain, nf);
        }
    }
#else
    (void)sim_ain;
#endif
#ifdef RFM14_PLAT_SIM
    if (fake_fm1 && fm1sim_start() != 0)
        fprintf(stderr, "remotefm1-host: the fake FM-1 did not start\n");
#else
    (void)fake_fm1;
#endif
    rc = mr_plat_main(pargc, pargv);
#ifdef RFM14_PLAT_SIM
    fm1sim_stop();
#endif
    web_stop();
    mdns_stop();
    engine_stop();
    midiout_stop();
    ann_stop();
    fprintf(stderr, "remotefm1-host: done\n");
    return rc;
}
