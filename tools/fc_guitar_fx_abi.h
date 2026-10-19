// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

#ifndef FC_GUITAR_FX_ABI_H
#define FC_GUITAR_FX_ABI_H

// fc_guitar_fx — the amp's echo and room behind a C ABI: felitronics-core's delay::TapeDelay and
// reverb::AmpReverb, driven the way the amp plugins drive them, for a page that runs them in an AudioWorklet
// (fcguitarfx.*, tools/wasm/build.sh). It compiles natively too, which is how the ctest suite holds it to the
// core classes driven directly.
//
// ====================================================================================
// WHAT IT IS
// ====================================================================================
// One instance is the amp's last two additive links, in the app's order: the DELAY, then the REVERB (OrbitAmp's
// processBlock: gate -> preamp -> EQ -> delay -> reverb). Both only ADD: the dry passes at unity whatever the
// knobs say. Stereo in place; a NULL right channel runs the one channel the stages' mono path takes.
//
// The KNOBS are the app's parameters, in the app's units — percent where the face reads percent — and they reach
// the stages through the app's own mapping (OrbitAmp's updateDelaySettings / updateReverbSettings), with the
// knob law felitronics-core carries for it (delay/DelayLaw.h, reverb::Character). A fresh instance stands at the
// app's parameter defaults. A knob is clamped onto the app parameter's range; a non-finite value is ignored.
//
// WHEN A KNOB LANDS. A setter only records the value. The next process() call hands every knob to the stages,
// in the app's order, before it runs them — as the app does at the top of every block — so a knob moved between
// two calls is heard from the first sample of the second. prepare() hands them over before the stages prepare
// (a room then starts AT its character) and again after, as the app's constructor and prepareToPlay do.
//
// ====================================================================================
// THE CONTRACT
// ====================================================================================
//   · create() allocates the instance; prepare() allocates every line and the two channel buffers; destroy()
//     frees. process(), reset() and the setters never allocate, lock or throw.
//   · ONE thread owns an instance: create, prepare, the setters, process, reset and destroy are not to be called
//     concurrently. In an AudioWorklet that thread is the audio thread — the port's message handler runs there,
//     between render quanta.
//   · prepare() answers 1 or 0. It refuses a rate outside 1 kHz … 768 kHz or a maxBlock outside 1 … 65536, and
//     a refused prepare DISARMS the instance: process() refuses until a prepare succeeds.
//   · process() answers 1 when it processed all n frames and 0 when it refused (unprepared, n < 0, left NULL)
//     without touching a sample. n == 0 is a no-op that answers 1. Any n is processed in full; the module's own
//     buffers (fc_guitar_fx_buffer) hold maxBlock frames, so a caller using them keeps n <= maxBlock.
//   · reset() is a stream restart: the delay line and the room cleared, the knobs kept.
//   · Denormals are flushed in software inside the stages (law 8), so nothing here depends on the FPU's mode.
//
// ====================================================================================
// THE COMPATIBILITY RULE — APPEND-ONLY
// ====================================================================================
// FC_GUITAR_FX_ABI_VERSION moves when the surface grows: an entry point is added, or a knob's range or meaning
// widens in a way a caller can observe. An existing entry point's name, arguments, return type and meaning never
// change, and nothing is removed. A page asks fc_guitar_fx_abi_version() first and refuses a module older than
// itself.
//
//   version   the surface
//   -------   ----------------------------------------------------------------------------------------------
//   1         create/destroy, prepare, buffer, process, reset, abi_version; the delay's nine knobs and the
//             reverb's six (below).

#define FC_GUITAR_FX_ABI_VERSION 1u

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fc_guitar_fx fc_guitar_fx;

/** The surface this module speaks — FC_GUITAR_FX_ABI_VERSION. Reads nothing, cannot fail. */
uint32_t fc_guitar_fx_abi_version (void);

/** A new instance at the app's defaults, unprepared; NULL only when the allocation fails. */
fc_guitar_fx* fc_guitar_fx_create (void);
/** Frees the instance and its buffers. NULL is a no-op. */
void fc_guitar_fx_destroy (fc_guitar_fx* fx);

/** Allocates for `sampleRate` (1 kHz … 768 kHz) and blocks of up to `maxBlock` (1 … 65536) frames, two channels.
    1 = prepared; 0 = refused, and the instance is disarmed. Clears the stream. */
int fc_guitar_fx_prepare (fc_guitar_fx* fx, double sampleRate, int maxBlock);

/** The module's own buffer for channel 0 (left) or 1 (right): maxBlock floats inside the module's memory, valid
    from a successful prepare until the next prepare or destroy. A wasm caller writes its input here, passes
    the two pointers to process() and reads the result from the same place. NULL when unprepared. */
float* fc_guitar_fx_buffer (fc_guitar_fx* fx, int channel);

/** Delay, then reverb, over `n` frames of `left` and `right`, in place. `right` NULL = one channel. */
int fc_guitar_fx_process (fc_guitar_fx* fx, float* left, float* right, int n);

/** A stream restart: the delay line and the room cleared; knobs and preparation kept. */
void fc_guitar_fx_reset (fc_guitar_fx* fx);

// ---- THE DELAY — the app's nine; units and defaults are the app parameter's ------------------------------------
/** The delay is ON (1, default) or standing by (0): standing by stops feeding the line, the repeats ring out. */
void fc_guitar_fx_set_delay_on (fc_guitar_fx* fx, int on);
/** SYNC on (1, default): the time is the division at the tempo; off (0): the free TIME knob. */
void fc_guitar_fx_set_delay_sync (fc_guitar_fx* fx, int on);
/** The division, an index onto delay::kDivisionNames — 0 "1/1" … 12 "1/16T"; default 5, "1/4". */
void fc_guitar_fx_set_delay_division (fc_guitar_fx* fx, int division);
/** The tempo the sync counts in, BPM, 40 … 240; default 120. In a page nobody else conducts: this is the app's
    own BPM field, the one that conducts in its standalone. */
void fc_guitar_fx_set_delay_bpm (fc_guitar_fx* fx, float bpm);
/** The free TIME, ms, 20 … 2000; default 350. Heard only with sync off. The head glides there — a move bends. */
void fc_guitar_fx_set_delay_time_ms (fc_guitar_fx* fx, float ms);
/** REPEATS, percent 0 … 100; default 35. */
void fc_guitar_fx_set_delay_repeats (fc_guitar_fx* fx, float percent);
/** DARK, percent of darkness 0 … 100 — clockwise darkens; default 45 (~4 kHz). */
void fc_guitar_fx_set_delay_dark (fc_guitar_fx* fx, float percent);
/** OFFSET, ms -30 … +30; positive holds the RIGHT repeats back. Default 0. */
void fc_guitar_fx_set_delay_offset_ms (fc_guitar_fx* fx, float ms);
/** MIX, percent 0 … 100 — the repeats added over an untouched dry; default 0, as the app ships it. */
void fc_guitar_fx_set_delay_mix (fc_guitar_fx* fx, float percent);

// ---- THE REVERB — the app's six ---------------------------------------------------------------------------------
/** The reverb is ON (1, default) or standing by (0): nothing new enters the room, the tail rings out. */
void fc_guitar_fx_set_reverb_on (fc_guitar_fx* fx, int on);
/** The character, an index onto reverb::kCharacterNames — 0 Ambience, 1 Room, 2 Hall, 3 Plate, 4 Spring,
    5 Modulated; default 3, Plate. */
void fc_guitar_fx_set_reverb_character (fc_guitar_fx* fx, int character);
/** MIX, percent 0 … 100 — the tail added over an untouched dry; default 30. */
void fc_guitar_fx_set_reverb_mix (fc_guitar_fx* fx, float percent);
/** DECAY, a scale of the character's own decay, 0.5 … 2; default 1. */
void fc_guitar_fx_set_reverb_decay (fc_guitar_fx* fx, float scale);
/** PREDELAY, ms 0 … 100; default 0. */
void fc_guitar_fx_set_reverb_predelay_ms (fc_guitar_fx* fx, float ms);
/** The tail's own high-pass, Hz 40 … 500; default 120. Always in. */
void fc_guitar_fx_set_reverb_hpf_hz (fc_guitar_fx* fx, float hz);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // FC_GUITAR_FX_ABI_H
