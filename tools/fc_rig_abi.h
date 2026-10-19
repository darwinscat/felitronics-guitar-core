// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

#ifndef FC_RIG_ABI_H
#define FC_RIG_ABI_H

// fc_rig — felitronics::rigplayer::RigPlayer behind a C ABI: one device of a .orbitrig pack, playing, for a page that
// runs it in an AudioWorklet (fcrig.*, tools/wasm-rig/build.sh). It compiles natively too, which is how the ctest
// suite holds it to the player driven directly.
//
// ====================================================================================
// WHAT IT IS
// ====================================================================================
// The player as the app plays it (OrbitAmp's CapturedBlock): the pack's manifest (rig.json) in, the knobs by the
// pack's own names, the captures' bytes handed in when the player asks for them, audio in place. Everything the
// player applies by itself stays the player's: the pack's two levels, the files' input trims, the loudness
// normalisation (to the player's own reference, -18 dB), the pack's tone and blend knobs, the crossfade along the
// gain dial — or, with fc_rig_set_max_models(1), one capture at a time with a dip between them. Nothing here is DSP
// of its own: every entry point is one RigPlayer call, or the load job carried across the boundary.
//
// ====================================================================================
// THE LOAD JOB, ACROSS THE BOUNDARY
// ====================================================================================
// RigPlayer never loads by itself: when its law wants a capture it hands out ONE job, the host runs it and brings
// it back. Here the host is the caller, the bytes come from it, and the job lives inside the instance:
//
//   fc_rig_take_job()        service() and then takeLoadJob(): 1 = a job is out now (call it a few times a second
//                            at least — a worklet calls it after every render quantum)
//   fc_rig_job_file()        which `files[].id` it wants (UTF-8, NUL-terminated, fc_rig_job_file_length() bytes)
//   fc_rig_job_needs_bytes() 1 = the player has never had this file: the host fetches it and copies it into
//                            fc_rig_job_bytes(size); 0 = the player kept the bytes from an earlier visit
//   fc_rig_job_run()         the heavy half (NamStage::prepareModel — parsing, two instances, the prewarm):
//                            1 = a model, 0 = not one (the bytes were not a capture, or none were handed in)
//   fc_rig_job_deliver()     the light half, installs it: 1 = a model landed, 2 = a failure was reported to the law
//
// A JOB TAKEN MUST COME BACK — run and delivered, a failure included (run without handing bytes in is how a host
// says "could not fetch it"; nothing of such a job is kept, so the file is asked for again — fc_rig_job_needs_bytes()
// 1 — the next time the law wants it), or the law waits for it forever and no other capture is ever loaded. A job that is out
// when fc_rig_load() or fc_rig_unload() is called is dropped by the instance itself, so a host never delivers into
// the wrong pack: from that call on fc_rig_job_file() answers NULL and the bytes buffer is gone.
//
// ====================================================================================
// THE CONTRACT
// ====================================================================================
//   · create() allocates the instance; prepare() allocates the player and the channel buffers; load(), the job
//     calls and the knob setters allocate (they are the player's message-thread half). destroy() frees.
//     process() and reset() never lock or throw, and never allocate while the captures sounding are Linear or WaveNet
//     — the architectures the pinned NAM preallocates for (gated by felitronics_nam_rtalloc_tests). An LSTM or ConvNet
//     capture DOES allocate inside process() at this pin (an owning Eigen vector per sample, dynamic temporaries;
//     NamStage.h), which in wasm may grow the memory mid-render: a caller re-reads its views of the module's memory
//     after process() as well. An allocation that fails is a trap in wasm, std::bad_alloc natively.
//   · ONE thread owns an instance: no two calls concurrently. In an AudioWorklet that is the audio thread — the
//     port's message handler runs there between render quanta — so the player's message-thread half runs on the
//     audio thread too, and a job run there stalls the quantum it runs in (some tens of milliseconds for a
//     WaveNet). Accepted for now (Oleh, 19.10.2026).
//   · prepare() answers 1 or 0. It refuses a rate outside 1 kHz … 768 kHz, a maxBlock outside 1 … 65536 or a
//     channel count other than 1 or 2, and a refused prepare DISARMS the instance: process() refuses until a prepare
//     succeeds. A pack may be loaded before or after prepare.
//   · process() answers 1 when it processed all n frames and 0 when it refused (unprepared, n < 0, left NULL, a
//     right channel on an instance prepared for one) without touching a sample. n == 0 is a no-op that answers 1.
//     A NULL right channel is a mono call: ONE network per sounding capture — prepare for 1 channel when the
//     source is a guitar. Without a pack, or before the first capture lands, the player is what RigPlayer is then.
//   · Strings are UTF-8, NUL-terminated, in the module's memory (fc_rig_alloc); a NULL name is refused.
//   · A non-finite number given to a setter is refused (0) or ignored (void), never stored.
//
// ====================================================================================
// THE COMPATIBILITY RULE — APPEND-ONLY
// ====================================================================================
// FC_RIG_ABI_VERSION moves when the surface grows AND when what an existing entry point does changes. An existing
// entry point's name, arguments and return type never change, and nothing is removed; a change of meaning is never
// made under the old number. A page asks fc_rig_abi_version() first and refuses a module older than itself.
//
//   version   the surface
//   1         everything below but the ABI-2 block
//   2         + fc_rig_scope_read, fc_rig_scope_frames, fc_rig_set_standby, fc_rig_set_feed_db
//   3         no entry point added: fc_rig_scope_read draws OrbitAmp's two points around the whole player (v0.5.1)
//
// ====================================================================================
// ABI 2 — THE SCOPE, PARK AND THE FEED (OrbitAmp's ScopeTap, its PARK, its block IN)
// ====================================================================================
//   fc_rig_scope_read(dry, wet, n)  copies the newest min(n, 2048, written) samples of the player's scope tap into two
//                     float arrays of the caller (module memory): channel 0, oldest first, at OrbitAmp's own two
//                     points, around the whole player (src/core/CapturedBlock.h:543-556): dry = the input as handed
//                     in, times the feed (fc_rig_set_feed_db — the app's block IN, PluginProcessor.cpp:1497) and
//                     nothing else — before the pack's input level and its pre tone, UNDELAYED; wet = the player's
//                     output — pre tone, models, post tone, blend knob, the pack's output level — before the PARK
//                     fade, at the dry's RMS (RigPlayer::scopeRead). As in the app, the wet trails the dry by a
//                     capture's own lag and, at a host rate other than the pack's, by fc_rig_latency_samples().
//                     Answers how many: 0 = nothing written yet (or n < 1, a NULL array). A block processed with no
//                     input channel writes nothing.
//                     THE ENTRY POINT IS ABI 2's; WHAT IT DRAWS IS ABI 3's (v0.5.1). ABI 2 (v0.5.0) tapped around the models —
//                     the dry after the pack's input level and held back by the latency, the wet before the post tone,
//                     the blend knob and the output level (the app's web spec, src/core/Monitor.cpp:189-191, and the
//                     site's tap around TONE3000's node) — so a pack with a post tone, a blend knob or levels drew
//                     another SHAPE there. Same arguments, same count, same level match; the sound is untouched.
//   fc_rig_scope_frames()  samples written to the tap since prepare(), an integer in a double: it moves when the tap
//                     was written, so a reader knows its frame is FRESH. It does not move while the device stands by.
//   fc_rig_set_standby(on)  PARK: the output fades over 15 ms to the input (at the latency, unity, no pack levels) and
//                     the device stops running — no models, no tone, no scope; off fades it back in over 15 ms. Knobs
//                     set meanwhile are taken on the way back (RigPlayer::setStandby).
//   fc_rig_set_feed_db(db)  how hard the captures are fed ON TOP of the pack's own input level — the app's block IN
//                     trim (TURBO's push); 0 = the pack's own. Applied inside the device (RigPlayer::setFeedDb): after
//                     the stand-by wire (a parked device hands the input on untrimmed) and before the scope's dry tap,
//                     which is the input times the feed alone. Kept across loads.
//                     A non-finite db is refused (0). A change GLIDES, as the player's every level does (rampInto,
//                     about 10 ms); the app's IN trim ramps over one host block instead (PluginProcessor.cpp:1497 —
//                     2.7 ms at 128 samples, 10.7 ms at 512), and on the return from PARK it arrives, here as there.

#include <stdint.h>

#define FC_RIG_ABI_VERSION 3u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fc_rig fc_rig;

uint32_t fc_rig_abi_version (void);

// ---- the instance --------------------------------------------------------------------------------------------------
fc_rig*  fc_rig_create (void);
void     fc_rig_destroy (fc_rig* rig);
int      fc_rig_prepare (fc_rig* rig, double sampleRate, int maxBlock, int channels);
float*   fc_rig_buffer (fc_rig* rig, int channel);           // maxBlock floats per prepared channel; NULL otherwise
void*    fc_rig_alloc (int bytes);                            // module memory for the caller (strings, the manifest)
void     fc_rig_free (void* p);

// ---- the pack ------------------------------------------------------------------------------------------------------
int      fc_rig_load (fc_rig* rig, const char* manifest, int length);   // rig.json; 1 = a NAM stage to play
void     fc_rig_unload (fc_rig* rig);

// ---- the load job (see THE LOAD JOB above) -------------------------------------------------------------------------
int         fc_rig_take_job (fc_rig* rig);
const char* fc_rig_job_file (fc_rig* rig);                    // NULL = no job out
int         fc_rig_job_file_length (fc_rig* rig);
int         fc_rig_job_needs_bytes (fc_rig* rig);
uint8_t*    fc_rig_job_bytes (fc_rig* rig, int size);         // 1 … 256 MiB; NULL = no job, or it needs none
int         fc_rig_job_run (fc_rig* rig);                     // 1 a model, 0 not one, -1 no job to run
int         fc_rig_job_deliver (fc_rig* rig);                 // 1 a model, 2 a failure, 0 nothing run

// ---- the knobs, by the pack's names (RigPlayer::setDial / setSwitch …) ---------------------------------------------
int      fc_rig_set_dial (fc_rig* rig, const char* name, double degrees);
int      fc_rig_set_switch (fc_rig* rig, const char* name, const char* value);
int      fc_rig_set_gain_dial (fc_rig* rig, double degrees);  // the crossfade dial, whatever the pack names it
double   fc_rig_gain_dial_sweep (fc_rig* rig);                // its sweep in degrees; 0 = the pack has none
int      fc_rig_set_max_models (fc_rig* rig, int n);          // 1 or 2 (the default)
void     fc_rig_set_normalize (fc_rig* rig, int on);          // the loudness tag; on by default
void     fc_rig_set_input_trims (fc_rig* rig, int on);        // the files' input_db; on by default
void     fc_rig_set_blend_shape (fc_rig* rig, double point, double width);   // each clamped onto 0 … 1

// ---- the audio -----------------------------------------------------------------------------------------------------
int      fc_rig_process (fc_rig* rig, float* left, float* right, int n);    // in place; right NULL = mono
void     fc_rig_reset (fc_rig* rig);

// ---- read-outs -----------------------------------------------------------------------------------------------------
double   fc_rig_sounding_db (fc_rig* rig);                    // RigPlayer::soundingLoudness — the tag in dB…
int      fc_rig_sounding_tagged (fc_rig* rig);                // …whether it carries one…
int      fc_rig_sounding_blended (fc_rig* rig);               // …and whether a second capture is audible beside it
int      fc_rig_held (fc_rig* rig, int slot);                 // 1 = the slot holds a capture
int      fc_rig_latency_samples (fc_rig* rig);
int      fc_rig_run_blocks (fc_rig* rig, int slot);           // blocks a slot's network ran, since clear_counters
int      fc_rig_pair_blocks (fc_rig* rig);                    // …and both did
int      fc_rig_warm_blocks (fc_rig* rig);                    // …and a slot was still warming
void     fc_rig_clear_counters (fc_rig* rig);

// ---- ABI 2: the scope, PARK, the feed (see ABI 2 above) ------------------------------------------------------------
int      fc_rig_scope_read (fc_rig* rig, float* dry, float* wet, int n);   // samples copied; 0 = none written yet
double   fc_rig_scope_frames (fc_rig* rig);                    // samples written to the tap since prepare
void     fc_rig_set_standby (fc_rig* rig, int on);             // PARK: 1 stands by, 0 plays
int      fc_rig_set_feed_db (fc_rig* rig, double db);          // the block's IN on top of the pack's input level

#ifdef __cplusplus
}
#endif

#endif // FC_RIG_ABI_H
