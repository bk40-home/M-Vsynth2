/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 DSP: the engine's parts in ONE translation unit of their own, compiled at -O2 (tools/build.py), while
 * the app and the platform (mvsynth2.c: USB, flash, the screen) stay at -Os. As M-vance's mv_dsp.c, and for its
 * reason: -O2 on the unity file with usb.c broke USB-MIDI (OberVave's finding); the DSP touches no hardware.
 * mvsynth2.c includes only the headers and calls vs_engine_* / vs_param_* / vs_cc_* / vs_patch_*; platform.c starts
 * the second core through vs_core1_start. */
#include <stdint.h>                              /* uint32_t for the vs_core1_* accessors on every build path */
#if !defined(DINGHY_HOST) && !defined(DINGHY_WEBSIM)     /* (a computer, a browser: no second core, no hardware) */
#include "fm1_time.h"
#define VS_TICKS() fm1_ticks()                  /* the engine's own meter: voices / bus (vs_voice.h t_voice, t_bus) */
#define VS_SHED_TICKS(n) ((n) * 435u)          /* the overload guard (vs_voice.c voice_shed): 80 % of n frames at 44.1 kHz,
                                                   24 ticks a us (544 a frame); the rest is the ISR's own and the main loop's */
#include "fm1_dual.h"
#if MELODEE_DUAL_CORE
/* The second core (Melodee's, hal/fm1_multicore.h + audio_worker.h): CPU1 polls a mailbox from RAM and renders the
 * voices CPU0 hands it each 16 frames (vs_voice.c vs_engine_render). The only hardware this file touches */
#include "fm1_multicore.h"
#define VS_PAR_SUBMIT(fn, ctx) audio_worker_submit((fn), (ctx))   /* 1: CPU1 took the job */
#define VS_PAR_JOIN() audio_worker_join()      /* 1: its results are in; 0: CPU1 retired (one core from now) */
#define VS_PAR_CHECK() audio_worker_check()    /* between blocks: retire a CPU1 that faulted or misread */
int vs_core1_start(uint32_t power_ok, uint32_t early)   /* platform.c, at boot: 1 online */
{
    return fm1_core1_boot_allowed(power_ok, early) ? fm1_multicore_start() : 0;
}
int vs_core1_online(void) { return audio_worker_online; }
#if MELODEE_AUDIO_PROFILE
uint32_t vs_core1_job_ticks(void)  { return audio_worker.job_ticks; }   /* CPU1 render time, wrapping tick sum */
uint32_t vs_core1_wait_ticks(void) { return audio_worker.wait_ticks; }  /* CPU0 time spent waiting at the join */
#else
uint32_t vs_core1_job_ticks(void)  { return 0u; }
uint32_t vs_core1_wait_ticks(void) { return 0u; }
#endif
#endif
#endif
#ifndef VS_PAR_SUBMIT
int vs_core1_online(void) { return 0; }
uint32_t vs_core1_job_ticks(void)  { return 0u; }
uint32_t vs_core1_wait_ticks(void) { return 0u; }
#endif
#include "vs_patch.c"
#include "vs_osc.c"
#include "vs_env.c"
#include "vs_lfo.c"
#include "vs_bus.c"
#include "vs_lim.c"
#include "vs_voice.c"
