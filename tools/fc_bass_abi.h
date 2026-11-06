// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

#ifndef FC_BASS_ABI_H
#define FC_BASS_ABI_H

// fc_bass — THE BASS AMP behind a C ABI: felitronics::bass::BassChain with the pack player as its AMP, for a page that
// runs it in an AudioWorklet (fcbass.*, tools/wasm-bass/build.sh). It compiles natively too, which is how the ctest
// suite holds it to the chain driven directly.
//
// ====================================================================================
// WHAT IT IS
// ====================================================================================
// One instance is the bass amp's whole chain, as OrbitAmp's bass amps play it (RedGrooveBass, OrbitBass):
//
//   input cut → GATE → ENVELOPE's key → COMP → cut → OCTAVE → cut → ENVELOPE
//     → THE AMP: the pack player (an fc_rig inside the instance, prepared for one channel), then its TONE (eq::EqLink,
//       the bass's standard: BASS, MIDDLE at its swept frequency, TREBLE)
//     → cut → the gate's END mute → the seam (one signal to two planes) → CHORUS → CAB with the DI mixed in → the bass's cuts
//
// Mono in (channel 0), two planes out. The echo, the room, the output trim and the limiter are not here — a page plays
// its own lid, as the guitar heroes do. Nothing of the chain's arithmetic lives here: every stage is felitronics-core's,
// wired by felitronics::bass::BassChain, the same class the plug-in plays.
//
// THE AMP IS AN fc_rig. fc_bass_rig() answers the fc_rig* inside the instance, and every fc_rig entry point works on it
// as on an instance of its own — the pack (fc_rig_load), the load jobs and the files up front, the preload, the gain
// dial (DRIVE: fc_rig_set_gain_dial, degrees of the pack's sweep), the switches (the gear's Voice: fc_rig_set_switch),
// the feed, the scope, PARK, the crossover (BLEND: fc_rig_set_crossover_*, ABI 6) — except prepare, buffer, process,
// reset and destroy, which are fc_bass's: the instance prepares, runs and frees its player itself. A page drives the
// player exactly as it drives fcrig's; fcbass-worklet.js is fcrig's processor (rig-port.js) around this instance.
//
// THE KNOBS ARE THE APP'S PARAMETERS, by the app's ids and in the app's units — the ids a face kit names them by
// (places.json: world.knobs[].param, world.columns[].param, world.switches[].param, …). fc_bass_set (id, value): a
// switch is 0 or 1 (> 0.5 on), a choice its index, a number clamped onto the parameter's range; a non-finite value or
// an unknown id is refused (0) and changes nothing. A fresh instance stands at the app's parameter DEFAULTS — not at a
// product's opening: a page sets what its product holds (RedGrooveBass: src/redgroove/RedGrooveLineup.cpp's pinned
// values, the export's opening). The ids:
//
//   GATE      gate_on 0|1 (0) · gate_threshold dB -80 … -10 (-50) · gate_pos 0 START | 1 END (1)
//   COMP      comp_on 0|1 (1) · comp BLEND 0 … 1 (0.5) · comp_threshold dB -48 … 0 (-24) · comp_ratio 1.5 … 10 (4)
//             comp_attack 0 slow | 1 med | 2 fast (1) · comp_release 0 auto | 1 fast | 2 slow (0)
//             comp_character 0 smooth | 1 punchy | 2 opto | 3 fet | 4 squash (2)
//   OCTAVE    octave_on 0|1 (1) · octave_present 0|1 (1) · octave 0 … 1 (0) · octave_tone_hz 30 … 2000 (120)
//   ENVELOPE  filter_on 0|1 (1) · filter_present 0|1 (1) · filter 0 … 1 (0) · filter_mode 0 LP | 1 BP (0)
//             filter_direction 0 up | 1 down (0) · filter_resonance 0.5 … 12 (4) · filter_range 0 low | 1 mid | 2 high (1)
//             filter_keep_lows 0|1 (1) · filter_keep_hz 80 … 300 (150) · filter_release_ms 5 … 2000 (120)
//   CHORUS    chorus_on 0|1 (1) · chorus_present 0|1 (1) · chorus 0 … 1 (0) · chorus_rate_hz 0.05 … 10 (0.7)
//             chorus_crossover_hz 200 … 500 (200)
//   TONE      preamp_eq_lo_db -15 … 15 (0) · preamp_eq_lo_hz 30 … 500 (100) · preamp_eq_b1_db -15 … 15 (0)
//             preamp_eq_b1_hz 60 … 3000 (400; the sound holds it to 200 … 3000) · preamp_eq_b1_q 0.3 … 6 (1)
//             preamp_eq_hi_db -15 … 15 (0) · preamp_eq_hi_hz 1500 … 16000 (8000)
//   BLEND     amp_blend_wet 0 … 1 (1: the capture alone) · amp_blend_hz 80 … 250 (150) · amp_blend_invert 0|1 (0)
//             — told to the player as its crossover: amount 1 - amp_blend_wet (the app's tellBlend)
//   CAB       cab_on 0|1 (1) · cab_mix percent 0 … 100, the cabinet's share against the DI (70)
//   CUTS      bass_hpf_on 0|1 (1) · bass_hpf 0 5 STR | 1 4 STR (0) · bass_hpf_hz_4 20 … 60 (32) · bass_hpf_hz_5 15 … 45 (22)
//             bass_lpf_on 0|1 (0) · bass_lpf 0 FINGERS | 1 SLAP (0) · bass_lpf_hz_fingers 2000 … 8000 (4500)
//             bass_lpf_hz_slap 5000 … 16000 (9500)
//
// A knob lands at the next fc_bass_process, every stage reading the block's knobs at its top as the app does; the
// stages glide where the app's glide. The app quantizes a parameter to its own step (0.1 dB, 1 Hz …); this does not.
//
// THE CABINET comes in as taps — the IR at its own rate, one channel: fc_bass_cab_taps (length) is module memory for
// them, fc_bass_cab_load (length, rate) takes them (the post baked, the level taken against the bass's reference,
// resampled to the session's rate, crossfaded in over 50 ms; refused — 0, the cabinet it had kept — for a length or a
// rate cabinet::CabinetIr::takes refuses: 8 kHz … 768 kHz, at most 5 s). The bass amps' cabinets are Cab IR
// Generator's: a page renders a preset's TOML with its own module (cabir_render, then the 24-bit WAV at cabir_wav) and
// hands in its samples AS THE PLUG-IN READS THEM — the 24-bit integers over 2^23 — so the taps are the plug-in's.
//
// ====================================================================================
// THE CONTRACT
// ====================================================================================
//   · create() allocates the instance and its player; prepare(), reset(), fc_bass_cab_taps() and fc_bass_cab_load()
//     allocate; destroy() frees. process() and fc_bass_set() never allocate, lock or throw — process() plays the player,
//     whose own contract holds (fc_rig_abi.h: no allocation while the captures sounding are Linear or WaveNet).
//   · ONE thread owns an instance — in an AudioWorklet, the audio thread.
//   · prepare() answers 1 or 0: a rate outside 1 kHz … 768 kHz or a maxBlock outside 1 … 65536 is refused and DISARMS
//     the instance (process() refuses until a prepare succeeds); a stage of the chain that refuses its prepare refuses
//     the whole. A pack may be loaded before or after; the cabinet loaded before a prepare is loaded again after it.
//   · process() answers 1 when it processed all n frames, 0 when it refused (unprepared, n < 0, n > maxBlock, left
//     NULL) without touching a sample. n == 0 is a no-op that answers 1. `left` carries the input in and the left plane
//     out; `right`, the right plane out (its contents in are ignored); a NULL `right` plays a one-plane bus (the chorus
//     one voice).
//   · reset() is a stream restart: the chain prepared again at its rate and block with the knobs it has, the cabinet
//     loaded again, the player reset (fc_rig_reset).
//
// ====================================================================================
// THE COMPATIBILITY RULE — APPEND-ONLY
// ====================================================================================
// FC_BASS_ABI_VERSION moves when the surface grows AND when what an existing entry point does changes; an existing
// entry point's name, arguments and return type never change, nothing is removed. A page asks fc_bass_abi_version()
// first and refuses a module older than itself; the player inside answers fc_rig_abi_version() as fcrig does (6).
//
//   version   the surface
//   1         everything below (v0.9.0)

#include <stdint.h>

#define FC_BASS_ABI_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fc_bass fc_bass;
typedef struct fc_rig  fc_rig;

uint32_t fc_bass_abi_version (void);

// ---- the instance --------------------------------------------------------------------------------------------------
fc_bass* fc_bass_create (void);                                       // NULL only when an allocation fails
void     fc_bass_destroy (fc_bass* bass);                             // the player with it; NULL is a no-op
int      fc_bass_prepare (fc_bass* bass, double sampleRate, int maxBlock);   // 1 prepared; 0 refused, disarmed
float*   fc_bass_buffer (fc_bass* bass, int channel);                 // 0: the input and the left plane, 1: the right
int      fc_bass_process (fc_bass* bass, float* left, float* right, int n);  // in place; right NULL = one plane
void     fc_bass_reset (fc_bass* bass);                               // a stream restart (see THE CONTRACT)

// ---- the AMP's player --------------------------------------------------------------------------------------------
fc_rig*  fc_bass_rig (fc_bass* bass);                                 // the fc_rig* every fc_rig_* call goes to

// ---- the knobs -----------------------------------------------------------------------------------------------------
int      fc_bass_set (fc_bass* bass, const char* id, double value);   // 1 taken; 0 unknown id, non-finite, NULL

// ---- the cabinet ---------------------------------------------------------------------------------------------------
float*   fc_bass_cab_taps (fc_bass* bass, int length);                // module memory for `length` taps; NULL out of range
int      fc_bass_cab_load (fc_bass* bass, int length, double rate);   // the taps written there → the cabinet; 1 / 0

// ---- read-outs -----------------------------------------------------------------------------------------------------
float    fc_bass_comp_gr_db (fc_bass* bass);                          // COMP's squeeze now, dB (0 or below)
int      fc_bass_latency_samples (fc_bass* bass);                     // the player's (a capture at another rate); the chain adds none

#ifdef __cplusplus
}
#endif

#endif // FC_BASS_ABI_H
