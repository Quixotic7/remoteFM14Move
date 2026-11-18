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
#include "mdns.h"
#include "mr_core.h"
#include "web.h"

int mr_plat_main(int argc, char **argv);

#define VERSION "0.1.0"

int main(int argc, char **argv)
{
    static char root[PATH_MAX], bundled[PATH_MAX], site[PATH_MAX], cap[4][PATH_MAX];
    const char *sim_dir = NULL, *capture = NULL, *fw = "felucca", *firmwares = NULL, *site_arg = NULL;
    char *pargv[4];
    int pargc = 0, i, port = 7730, lead = 5, cap_s = 0, rc;
    static char banner_title[32], banner_name[64];
#ifdef RFM14_PLAT_SIM
    int mdns = 0, splash = 0;
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/remotefm1-sim", tmp && *tmp ? tmp : "/tmp");
#else
    int mdns = 1, splash = 3000;   /* the address on the Move's screen before the controls take it */
    snprintf(root, sizeof root, "/data/UserData/RemoteFM1");
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
        else if (!strcmp(argv[i], "--splash") && i + 1 < argc)
            splash = atoi(argv[++i]);
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
                            "[--firmwares DIR] [--site DIR] [--sim-dir DIR] [--capture DIR --capture-seconds S]\n");
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
    rc = mr_plat_main(pargc, pargv);
    web_stop();
    mdns_stop();
    engine_stop();
    fprintf(stderr, "remotefm1-host: done\n");
    return rc;
}
