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
 *     --no-fm1      no FM-1 link on USB-A (fm1link.c; USB-A stays Move Remote's)
 *     --fm1-bps     the FM-1's screen, TILES body bytes a second (default fm1link.h FM1LINK_BPS_DEFAULT)
 *     --fm1-cap     USB-A packets per SPI transfer (default FM1LINK_CAP_DEFAULT; the rest of a transfer is the LEDs')
 *     --no-audio-in no input for the Fantasy CFWs with ain1 (Move Remote's input stays its own)
 *     --sim-audio-in F.wav   (the simulator) the Move's input: the WAV, looped (wav.h), in place of its 440 Hz tone
 *     --no-audio-in no input for the Fantasy CFWs with ain1 (Move Remote's own input path stays as it was)
 *     --sim-audio-in F.wav   (the simulator) the Move's input: the WAV (wav.h), looped, in place of its 440 Hz tone
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

#include "engine.h"
#include "fm1link.h"
#ifdef RFM14_PLAT_SIM
#include "fm1sim.h"
#endif
#include "fx.h"
#include "https.h"
#include "mdns.h"
#include "mr_core.h"
#include "wav.h"
#include "web.h"

int mr_plat_main(int argc, char **argv);

#define VERSION "0.1.0"

int main(int argc, char **argv)
{
    static char root[PATH_MAX], bundled[PATH_MAX], site[PATH_MAX], cap[4][PATH_MAX], index[PATH_MAX];
    const char *sim_dir = NULL, *capture = NULL, *fw = "felucca", *firmwares = NULL, *site_arg = NULL;
    const char *index_arg = NULL, *url = NULL, *fx_dir = NULL;
    int fx_budget = 0, test_api = 0, fm1_off = 0, fm1_cap = 0, fake_fm1 = 0, no_ain = 0;
    const char *sim_ain = NULL;
    long fm1_bps = 0;
    long log_cap = 0;
    char *pargv[4];
    int pargc = 0, i, port = 7730, lead = 5, cap_s = 0, rc;
    static char banner_title[32], banner_name[64];
    if (argc == 5 && !strcmp(argv[1], "--fx-probe"))   /* (fx.c: a module loaded here first, in a child) */
        return fx_probe_main(atoi(argv[2]), argv[3], argv[4]);
#ifdef RFM14_PLAT_SIM
    int mdns = 0, splash = 0;
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/remotefm1-sim", tmp && *tmp ? tmp : "/tmp");
#else
    int mdns = 1, splash = 3000;   /* the address on the Move's screen before the controls take it */
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
        else if (!strcmp(argv[i], "--no-fm1"))
            fm1_off = 1;
        else if (!strcmp(argv[i], "--fm1-bps") && i + 1 < argc)
            fm1_bps = atol(argv[++i]);
        else if (!strcmp(argv[i], "--fm1-cap") && i + 1 < argc)
            fm1_cap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-audio-in"))
            no_ain = 1;
        else if (!strcmp(argv[i], "--sim-audio-in") && i + 1 < argc)
            sim_ain = argv[++i];
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
                            "[--fx-dir DIR] [--fx-budget PERMILLE] [--test-api] [--no-fm1] [--fm1-bps N] [--fm1-cap N] [--fake-fm1] "
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
        c.default_fw = fw;
        c.lead_blocks = lead;
        c.splash_ms = (uint32_t)(splash < 0 ? 0 : splash);
        c.bundled_index = index[0] ? index : NULL;
        c.catalog_url = url;
        c.log_cap = log_cap;
        c.fx_dir = fx_dir;
        c.fx_budget = fx_budget;
        c.test_api = test_api;
        c.fm1_off = fm1_off;
        c.fm1_bps = (uint32_t)(fm1_bps > 0 ? fm1_bps : 0);
        c.fm1_cap = fm1_cap;
#ifdef RFM14_PLAT_MOVE
        c.cpu_mask = 0x7;   /* cores 0..2: core 3 is the SPI's */
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
        signal(SIGPIPE, SIG_IGN);
        if (engine_start(&c) != 0) {
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
    fprintf(stderr, "remotefm1-host: done\n");
    return rc;
}
