// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// fc_guitar_fx — the translation unit of the fcguitarfx module (tools/wasm/build.sh), and of the native library the
// ctest suite links (tools/wasm/CMakeLists.txt). The contract is in tools/fc_guitar_fx_abi.h; this file is the
// app's mapping from its knobs to the two core stages, and nothing else: no DSP lives here.
//
// THE MAPPING IS THE APP'S. Each knob below names the OrbitAmp parameter it is (src/Parameters.cpp, the layout —
// its range and its default) and the line of updateDelaySettings / updateReverbSettings (src/PluginProcessor.cpp)
// that hands it to the stage. Where felitronics-core carries the app's law (delay/DelayLaw.h: the sync ladder and
// DARK's curve; reverb::Character: the six rooms in the app's order), the law is called rather than restated.

#include "fc_guitar_fx_abi.h"

#include <felitronics/delay/DelayLaw.h>
#include <felitronics/delay/TapeDelay.h>
#include <felitronics/reverb/AmpReverb.h>

#include <cmath>
#include <cstdint>
#include <new>
#include <vector>

#if defined(__EMSCRIPTEN__)
  #include <emscripten/emscripten.h>
  #define FC_EXPORT extern "C" EMSCRIPTEN_KEEPALIVE
#else
  #define FC_EXPORT extern "C"
#endif

namespace
{
namespace delay  = felitronics::delay;
namespace reverb = felitronics::reverb;

constexpr int kMaxBlock = 65536;   // the module's own buffers are maxBlock floats per channel

/** The app's parameters, in the app's units, at the app's defaults (OrbitAmp src/Parameters.cpp, createLayout). */
struct Knobs
{
    bool  delayOn          = true;                      // delay_on, Bool, true
    bool  delaySync        = true;                      // delay_sync, Bool, true
    int   delayDivision    = delay::kDivisionDefault;   // delay_div, Choice over the ladder, 5 = 1/4
    float delayBpm         = delay::kBpmDefault;        // delay_bpm, 40..240, 120
    float delayTimeMs      = 350.0f;                    // delay_time_ms, 20..2000, 350
    float delayRepeats     = 35.0f;                     // delay_repeats, percent, 35
    float delayDark        = delay::kDarkDefaultPct;    // delay_dark, percent, 45
    float delayOffsetMs    = 0.0f;                      // delay_offset, -30..30 ms, 0
    float delayMix         = 0.0f;                      // delay_mix, percent, 0 — on with nothing in its mix
    bool  reverbOn         = true;                      // reverb_on, Bool, true
    int   reverbCharacter  = (int) reverb::Character::plate;   // reverb_type, Choice, "Plate"
    float reverbMix        = 30.0f;                     // reverb_mix, percent, 30
    float reverbDecay      = 1.0f;                      // reverb_decay, 0.5..2, 1
    float reverbPredelayMs = 0.0f;                      // reverb_predelay, 0..100 ms, 0
    float reverbHpfHz      = 120.0f;                    // reverb_hpf_hz, 40..500 Hz, 120
};

inline float clampf (float lo, float hi, float v) noexcept { return v < lo ? lo : (hi < v ? hi : v); }
inline int   clampi (int lo, int hi, int v) noexcept { return v < lo ? lo : (hi < v ? hi : v); }

} // namespace

struct fc_guitar_fx
{
    delay::TapeDelay  delay;
    reverb::AmpReverb reverb;
    Knobs knobs;
    bool  dirty    = true;   // a knob moved since the stages last heard them
    bool  prepared = false;
    int   maxBlock = 0;
    std::vector<float> io[2];

    /** Every knob to the stages, in the app's order. The delay as updateDelaySettings has it: the time first —
        the division at the tempo when SYNC is on, the free knob otherwise (DelayLaw's timeMsFor, the app's sync
        branch) — then REPEATS ×0.01 (the face reads percent), DARK through its log-even curve
        (darkHzForPercent, the app's ceiling·(floor/ceiling)^(pct·0.01)), OFFSET as it is, MIX ×0.01. Then the
        room as updateReverbSettings has it: the character by index, MIX ×0.01, DECAY, PREDELAY, HPF as they are.
        The stages' setters skip a value they already hold, so handing over an unmoved knob changes nothing. */
    void apply() noexcept
    {
        const Knobs& k = knobs;
        delay.setTimeMs (delay::timeMsFor (k.delaySync, k.delayDivision, (double) k.delayBpm, k.delayTimeMs));
        delay.setRepeats (k.delayRepeats * 0.01f);
        delay.setDarkHz (delay::darkHzForPercent (k.delayDark));
        delay.setOffsetMs (k.delayOffsetMs);
        delay.setMix (k.delayMix * 0.01f);

        reverb.setCharacter (static_cast<reverb::Character> (k.reverbCharacter));
        reverb.setMix (k.reverbMix * 0.01f);
        reverb.setDecay (k.reverbDecay);
        reverb.setPredelayMs (k.reverbPredelayMs);
        reverb.setHpfHz (k.reverbHpfHz);
        dirty = false;
    }

    template <typename T>
    void set (T& knob, T value) noexcept
    {
        knob  = value;
        dirty = true;
    }

    /** A float knob onto its parameter's range; a non-finite value leaves it where it stands. */
    void setf (float& knob, float lo, float hi, float value) noexcept
    {
        if (std::isfinite (value))
            set (knob, clampf (lo, hi, value));
    }
};

FC_EXPORT std::uint32_t fc_guitar_fx_abi_version (void) { return FC_GUITAR_FX_ABI_VERSION; }

FC_EXPORT fc_guitar_fx* fc_guitar_fx_create (void)
{
    auto* fx = new (std::nothrow) fc_guitar_fx();
    if (fx != nullptr)
        fx->apply();   // the app's constructor hands its knobs over before anyone prepares
    return fx;
}

FC_EXPORT void fc_guitar_fx_destroy (fc_guitar_fx* fx) { delete fx; }

FC_EXPORT int fc_guitar_fx_prepare (fc_guitar_fx* fx, double sampleRate, int maxBlock)
{
    if (fx == nullptr)
        return 0;

    fx->prepared = false;   // whatever happens next, the old preparation no longer stands
    if (! (sampleRate >= delay::TapeDelay::kMinSampleRate && sampleRate <= delay::TapeDelay::kMaxSampleRate)
        || maxBlock < 1 || maxBlock > kMaxBlock)
        return 0;

    // The knobs as they stand now are the state the instance was set up with: handed over BEFORE the stages
    // prepare, as the app's constructor does with the state it restores, so the room starts at its character.
    // Then the stages' prepare and the knobs again, as the app's prepareToPlay has it — a no-op here.
    fx->apply();
    if (! fx->delay.prepare (sampleRate, maxBlock, 2) || ! fx->reverb.prepare (sampleRate, maxBlock, 2))
        return 0;
    fx->apply();

    for (auto& b : fx->io)
        b.assign ((std::size_t) maxBlock, 0.0f);

    fx->maxBlock = maxBlock;
    fx->prepared = true;
    return 1;
}

FC_EXPORT float* fc_guitar_fx_buffer (fc_guitar_fx* fx, int channel)
{
    if (fx == nullptr || ! fx->prepared || channel < 0 || channel > 1)
        return nullptr;
    return fx->io[channel].data();
}

FC_EXPORT int fc_guitar_fx_process (fc_guitar_fx* fx, float* left, float* right, int n)
{
    if (fx == nullptr || ! fx->prepared || left == nullptr || n < 0)
        return 0;
    if (n == 0)
        return 1;

    if (fx->dirty)
        fx->apply();   // the app's updateDelaySettings / updateReverbSettings, at the top of the block

    float* io[2] = { left, right };
    const int nch = right != nullptr ? 2 : 1;

    // The app's order: the echo, then the room that rooms it. Each is fed by its own ON switch; standing by,
    // its tail rings out into the dry.
    if (! fx->delay.process (io, nch, n, fx->knobs.delayOn))
        return 0;
    if (! fx->reverb.process (io, nch, n, fx->knobs.reverbOn))
        return 0;
    return 1;
}

FC_EXPORT void fc_guitar_fx_reset (fc_guitar_fx* fx)
{
    if (fx == nullptr)
        return;
    // Knobs moved since the last block reach the stages FIRST: a reset lands every glide on its target, and the
    // targets must be the knobs as they stand now, not as the last block heard them — or what follows the restart
    // would depend on what came before it (law 11a).
    if (fx->dirty)
        fx->apply();
    fx->delay.reset();
    fx->reverb.reset();
}

// ---- the knobs: each clamped onto its OrbitAmp parameter's range (src/Parameters.cpp) ----------------------------

FC_EXPORT void fc_guitar_fx_set_delay_on (fc_guitar_fx* fx, int on)
{
    if (fx != nullptr) fx->set (fx->knobs.delayOn, on != 0);
}

FC_EXPORT void fc_guitar_fx_set_delay_sync (fc_guitar_fx* fx, int on)
{
    if (fx != nullptr) fx->set (fx->knobs.delaySync, on != 0);
}

FC_EXPORT void fc_guitar_fx_set_delay_division (fc_guitar_fx* fx, int division)
{
    if (fx != nullptr) fx->set (fx->knobs.delayDivision, clampi (0, delay::kNumDivisions - 1, division));
}

FC_EXPORT void fc_guitar_fx_set_delay_bpm (fc_guitar_fx* fx, float bpm)
{
    if (fx != nullptr) fx->setf (fx->knobs.delayBpm, delay::kBpmMin, delay::kBpmMax, bpm);
}

FC_EXPORT void fc_guitar_fx_set_delay_time_ms (fc_guitar_fx* fx, float ms)
{
    if (fx != nullptr) fx->setf (fx->knobs.delayTimeMs, delay::kTimeMinMs, delay::kTimeMaxMs, ms);
}

FC_EXPORT void fc_guitar_fx_set_delay_repeats (fc_guitar_fx* fx, float percent)
{
    if (fx != nullptr) fx->setf (fx->knobs.delayRepeats, 0.0f, 100.0f, percent);
}

FC_EXPORT void fc_guitar_fx_set_delay_dark (fc_guitar_fx* fx, float percent)
{
    if (fx != nullptr) fx->setf (fx->knobs.delayDark, 0.0f, 100.0f, percent);
}

FC_EXPORT void fc_guitar_fx_set_delay_offset_ms (fc_guitar_fx* fx, float ms)
{
    if (fx != nullptr) fx->setf (fx->knobs.delayOffsetMs, -delay::kOffsetMaxMs, delay::kOffsetMaxMs, ms);
}

FC_EXPORT void fc_guitar_fx_set_delay_mix (fc_guitar_fx* fx, float percent)
{
    if (fx != nullptr) fx->setf (fx->knobs.delayMix, 0.0f, 100.0f, percent);
}

FC_EXPORT void fc_guitar_fx_set_reverb_on (fc_guitar_fx* fx, int on)
{
    if (fx != nullptr) fx->set (fx->knobs.reverbOn, on != 0);
}

FC_EXPORT void fc_guitar_fx_set_reverb_character (fc_guitar_fx* fx, int character)
{
    if (fx != nullptr) fx->set (fx->knobs.reverbCharacter, clampi (0, reverb::kNumCharacters - 1, character));
}

FC_EXPORT void fc_guitar_fx_set_reverb_mix (fc_guitar_fx* fx, float percent)
{
    if (fx != nullptr) fx->setf (fx->knobs.reverbMix, 0.0f, 100.0f, percent);
}

FC_EXPORT void fc_guitar_fx_set_reverb_decay (fc_guitar_fx* fx, float scale)
{
    if (fx != nullptr) fx->setf (fx->knobs.reverbDecay, 0.5f, 2.0f, scale);
}

FC_EXPORT void fc_guitar_fx_set_reverb_predelay_ms (fc_guitar_fx* fx, float ms)
{
    if (fx != nullptr) fx->setf (fx->knobs.reverbPredelayMs, 0.0f, 100.0f, ms);
}

FC_EXPORT void fc_guitar_fx_set_reverb_hpf_hz (fc_guitar_fx* fx, float hz)
{
    if (fx != nullptr) fx->setf (fx->knobs.reverbHpfHz, 40.0f, 500.0f, hz);
}
