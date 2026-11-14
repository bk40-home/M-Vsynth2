/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 build switches (after M-vance's mv_build.h). The defaults are the standalone FM-1 firmware. Each switch
 * is 0 or 1 unless it says a count or milliseconds. Included first by mvsynth2.c and vs_voice.h (later: vs_ui.c,
 * vs_prog.h, vs_fx.h). Constants only.
 *   ./build.sh                                       the standalone firmware
 *   DINGHY_DEFS="-DVS_NV=6u -DVS_LPDL=0" ./build.sh  a trial of the switches (flash.ps1 -Defs "...") */
#pragma once

#ifndef VS_NV
#define VS_NV 8u            /* voices built in (Synth1: up to 32). 8 by the gate's measurement on the FM-1 (proposal 3.5).
                               POLY (parameter 94) and UNISON VOICES (93) are played as min(value, VS_NV); the patch
                               keeps what it says, so a .sy1 goes back to the computer unchanged */
#endif

#ifndef VS_DEBUG
#define VS_DEBUG 1          /* the DEBUG page (GLO's last) and the render-time meter behind it */
#endif

#ifndef VS_XFER
#define VS_XFER 1           /* the patch transfer over USB SysEx (vs_xfer.h, docs/bank.html) and its RECEIVING /
                               SENDING screen. Off: its SysEx is ignored (APP_SYSEX not set) */
#endif

#ifndef VS_LPDL
#define VS_LPDL 1           /* the LPDL filter (type 4: the diode ladder, 2 x oversampled; the dearest thing a voice
                               can do). 0: type 4 plays LP24, the page shows LPDL*, the cheat sheet says so */
#endif

#ifndef VS_NBANK
#define VS_NBANK 8u         /* user banks of 128 patches, one flash slot each (vs_prog.h): 1..8 (NSLOT) */
#endif

#ifndef VS_DLY_MS
#define VS_DLY_MS 1000u     /* the delay lines' length (ms): 2 beats at 120 BPM (Kris 13 Nov: the oscillator tables are read
                               from RAM, 107 KB of the pool, proposal 3.5). A longer time (TIME x the tempo, + SPREAD) is
                               held at the line's end. RAM 2 x 2 B x (44.1 x ms + 3): 176 KB of the pool */
#endif

#if (VS_DEBUG != 0 && VS_DEBUG != 1) || (VS_XFER != 0 && VS_XFER != 1) || (VS_LPDL != 0 && VS_LPDL != 1)
#error "vs_build.h: VS_DEBUG, VS_XFER and VS_LPDL are 0 or 1"
#endif
#if VS_NV < 1u || VS_NV > 16u
#error "vs_build.h: VS_NV is 1..16"
#endif
#if VS_NBANK < 1u || VS_NBANK > 8u
#error "vs_build.h: VS_NBANK is 1..8 (the platform has 8 slots)"
#endif
#if VS_DLY_MS < 200u || VS_DLY_MS > 1000u
#error "vs_build.h: VS_DLY_MS is 200..1000 (the pool holds the oscillator tables, 1000 ms and the chorus lines)"
#endif
