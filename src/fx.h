/* SPDX-License-Identifier: MPL-2.0 */
/* Schwung FX hosting (PLAN.md, phase 2b): the user's installed Schwung modules, loaded into this process.
 *
 *   MIDI FX:  <modules>/midi_fx/<id>/module.json + its dsp (module.json "dsp", else dsp.so): move_midi_fx_init ->
 *             midi_fx_api_v1_t (process_midi per message, tick per 128-frame block)
 *   audio FX: <modules>/audio_fx/<id>/module.json + its dsp ("dsp", else <id>.so): move_audio_fx_init_v2 ->
 *             audio_fx_api_v2_t (process_block in place, int16 stereo, 128 frames)
 *
 * The headers are Schwung's own (third_party/schwung-api, MIT). <modules> is /data/UserData/schwung/modules on the Move.
 * Each module's parameters are its module.json's capabilities.chain_params (else its ui_hierarchy's root params): key,
 * name, type float / int / enum, min, max, default, options. rfm1cs-abletonmove's FX setup (rfm_move_fx_modules) sees
 * them as 0..127 values or word lists; fx_set maps a 0..127 value back to the module's own range and sets it by key.
 *
 * THREADS: fx_scan once at start (the main thread, before the engine). fx_open / fx_close on the storage thread (the
 * dlopen, the init, create_instance: a module's own allocation, file reads); everything else on the engine thread.
 * A module that crashes in its dlopen, init or create_instance is caught first by a probe in a child process
 * (`remotefm1-host --fx-probe KIND DIR SO`): it is refused, the host keeps running. A crash later (in process_block or
 * tick) still takes the host down, as any in-process plug-in would. */
#ifndef RFM14_FX_H
#define RFM14_FX_H
#include <stddef.h>
#include <stdint.h>

#include "rfm1cs/move.h"

#define FX_KINDS 2              /* RFM_MOVE_FX_MIDI 0, RFM_MOVE_FX_AUDIO 1 */
#define FX_MAX_MODS 64          /* modules of a kind listed */
#define FX_MAX_PARAMS 16        /* parameters of a module kept (the core uses the first 8) */
#define FX_MAX_OUT 16           /* MIDI messages a call may give (MIDI_FX_MAX_OUT_MSGS) */

enum { FXP_FLOAT, FXP_INT, FXP_ENUM };
typedef struct {
    char key[48], name[48];
    int type;                   /* FXP_* */
    double min, max, def;
    int nopt, opt_str;          /* enum: its options (words); opt_str: set by the option's text, else by its index */
    char **opt;
} fx_param_t;

typedef struct {
    int kind;
    char id[64], name[RFM_MOVE_FX_NAME], abbrev[8], version[24];
    char dir[512], so[600];
    int builtin;                /* Schwung's own (module.json "builtin", or arp / chord / freeverb) */
    int np;
    fx_param_t p[FX_MAX_PARAMS];
    int probed;                 /* (the storage thread's) 1 the probe passed, -1 it failed */
} fx_mod_t;

typedef struct {
    fx_mod_t mod[FX_KINDS][FX_MAX_MODS];
    int n[FX_KINDS];
    /* the core's view (rfm_move_fx_modules): the same order, A-Z by name */
    rfm_move_fx_module_t core[FX_KINDS][FX_MAX_MODS];
    rfm_move_fx_param_t cpar[FX_KINDS][FX_MAX_MODS][FX_MAX_PARAMS];
} fx_lib_t;

/* the modules under dir (its midi_fx/ and audio_fx/), sorted A-Z by name; never NULL (empty when there is none) */
fx_lib_t *fx_scan(const char *dir);
const fx_mod_t *fx_mod(const fx_lib_t *lib, int kind, int i);
/* the modules as JSON for the page: {"midi":[{id,name,abbrev,version,builtin,params:[{key,name,type,min,max,def,
 * options}]}],"audio":[...]} (malloc'd) */
char *fx_lib_json(const fx_lib_t *lib);

/* the host's clock, as modules ask for it (host_api_v1 get_bpm, get_clock_status, get_beat_position) */
void fx_clock(float bpm, int running, double beat);

typedef struct fx_inst fx_inst_t;
/* (the storage thread) the probe, then dlopen, init and create_instance; NULL with err */
fx_inst_t *fx_open(fx_mod_t *m, char *err, size_t cap);
void fx_close(fx_inst_t *x);                             /* (the storage thread) destroy_instance, dlclose */
const fx_mod_t *fx_inst_mod(const fx_inst_t *x);
/* (the engine thread) parameter k (chain_params order) to the core's 0..127 value (an enum: the option's index) */
void fx_set(fx_inst_t *x, int k, int v);
/* its "state" (get_param): the length, or -1 (none); set_param("state", s) */
int fx_state_get(fx_inst_t *x, char *buf, int cap);
void fx_state_set(fx_inst_t *x, const char *s);
/* MIDI FX: one message in, the messages out (their count); tick: the block's generated messages */
int fx_midi(fx_inst_t *x, const uint8_t *in, int n, uint8_t out[][3], int lens[], int max);
int fx_tick(fx_inst_t *x, int frames, uint8_t out[][3], int lens[], int max);
/* audio FX: the bus processed in place */
void fx_audio(fx_inst_t *x, int16_t *lr, int frames);
uint64_t fx_ns(const fx_inst_t *x);                      /* the time spent in its calls so far */

/* the child's side of the probe (main.c: --fx-probe KIND DIR SO): dlopen, init, create, one call, destroy; 0 ok */
int fx_probe_main(int kind, const char *dir, const char *so);

#endif
