// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// fc_bass — the translation unit of the fcbass module (tools/wasm-bass/build.sh), and of the native library the ctest
// suite links (tools/wasm-bass/CMakeLists.txt). The contract is in tools/fc_bass_abi.h. No DSP lives here: the chain is
// felitronics::bass::BassChain, the AMP the fc_rig player (tools/wasm-rig/fc_rig.cpp, linked beside this) and
// eq::EqLink; this file is their wiring to a C surface and the app's parameter ids.

#include "fc_bass_abi.h"
#include "fc_rig_abi.h"

#include <felitronics/bass/BassChain.h>
#include <felitronics/eq/EqLink.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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
constexpr int kMaxBlock = 65536;

/** The app's parameters the instance keeps apart before they become the chain's Settings: a removable link works while
    it is switched on AND in the strip. */
struct Links
{
    bool octaveOn = true, octavePresent = true;
    bool filterOn = true, filterPresent = true;
    bool chorusOn = true, chorusPresent = true;
};
} // namespace

struct fc_bass
{
    fc_rig* rig = nullptr;
    felitronics::bass::BassChain chain;
    felitronics::bass::Settings  settings;
    Links links;
    felitronics::eq::EqLink           tone;
    felitronics::eq::EqLink::Settings console;   // the AMP's console as the app's parameters hold it
    float blendWet = 1.0f, blendHz = 150.0f;
    bool  blendInvert = false;
    std::vector<float> io[2];
    std::vector<float> cabTaps;                  // the module memory a page writes an IR into…
    std::vector<float> cabKept;                  // …and the one the cabinet was last given, loaded again after a prepare
    double cabRate = 0.0;
    double rate = 48000.0;
    int    block = 0;
    bool   prepared = false;

    fc_bass()
    {
        console.b1Hz = 400.0;   // the app's parameter defaults (Parameters.cpp, the console's bells and shelves)
    }

    void sync() noexcept
    {
        settings.octaveWorks   = links.octaveOn && links.octavePresent;
        settings.envelopeWorks = links.filterOn && links.filterPresent;
        settings.chorusWorks   = links.chorusOn && links.chorusPresent;
    }

    /** BLEND, told to the player as the app's tellBlend tells it: the crossover's amount is 1 - BLEND. */
    void tellBlend() noexcept
    {
        (void) fc_rig_set_crossover_amount (rig, 1.0 - (double) blendWet);
        (void) fc_rig_set_crossover_hz (rig, (double) blendHz);
        fc_rig_set_crossover_invert (rig, blendInvert ? 1 : 0);
    }

    bool prepareChain() noexcept
    {
        sync();
        const bool ok = chain.prepare (rate, block, 2, settings);
        tone.prepare (rate, 2);
        if (! cabKept.empty())
            (void) chain.cabinet().load (cabKept.data(), (int) cabKept.size(), cabRate);
        return ok;
    }
};

namespace
{
using Setter = bool (*) (fc_bass&, double);

double clampTo (double v, double lo, double hi) { return std::clamp (v, lo, hi); }
int    choice (double v, int count) { return std::clamp ((int) std::lround (v), 0, count - 1); }
bool   on (double v) { return v > 0.5; }

struct Knob { const char* id; Setter set; };

// THE APP'S PARAMETERS, by its ids, in its units and onto its ranges (OrbitAmp src/Parameters.cpp).
const Knob kKnobs[] = {
    { "gate_on",             [] (fc_bass& b, double v) { b.settings.gateWorks = on (v); return true; } },
    { "gate_threshold",      [] (fc_bass& b, double v) { b.settings.gateThresholdDb = (float) clampTo (v, -80.0, -10.0); return true; } },
    { "gate_pos",            [] (fc_bass& b, double v) { b.settings.gateMutesAtStart = choice (v, 2) == 0; return true; } },

    { "comp_on",             [] (fc_bass& b, double v) { b.settings.compWorks = on (v); return true; } },
    { "comp",                [] (fc_bass& b, double v) { b.settings.compBlend = (float) clampTo (v, 0.0, 1.0); return true; } },
    { "comp_threshold",      [] (fc_bass& b, double v) { b.settings.compThresholdDb = (float) clampTo (v, -48.0, 0.0); return true; } },
    { "comp_ratio",          [] (fc_bass& b, double v) { b.settings.compRatio = (float) clampTo (v, 1.5, 10.0); return true; } },
    { "comp_attack",         [] (fc_bass& b, double v) { b.settings.compAttack = choice (v, 3); return true; } },
    { "comp_release",        [] (fc_bass& b, double v) { b.settings.compRelease = choice (v, 3); return true; } },
    { "comp_character",      [] (fc_bass& b, double v) { b.settings.compCharacter = choice (v, 5); return true; } },

    { "octave_on",           [] (fc_bass& b, double v) { b.links.octaveOn = on (v); return true; } },
    { "octave_present",      [] (fc_bass& b, double v) { b.links.octavePresent = on (v); return true; } },
    { "octave",              [] (fc_bass& b, double v) { b.settings.octave = (float) clampTo (v, 0.0, 1.0); return true; } },
    { "octave_tone_hz",      [] (fc_bass& b, double v) { b.settings.octaveToneHz = (float) clampTo (v, 30.0, 2000.0); return true; } },

    { "filter_on",           [] (fc_bass& b, double v) { b.links.filterOn = on (v); return true; } },
    { "filter_present",      [] (fc_bass& b, double v) { b.links.filterPresent = on (v); return true; } },
    { "filter",              [] (fc_bass& b, double v) { b.settings.envelope = (float) clampTo (v, 0.0, 1.0); return true; } },
    { "filter_mode",         [] (fc_bass& b, double v) { b.settings.envelopeMode = choice (v, 2); return true; } },
    { "filter_direction",    [] (fc_bass& b, double v) { b.settings.envelopeDirection = choice (v, 2); return true; } },
    { "filter_resonance",    [] (fc_bass& b, double v) { b.settings.envelopeResonance = (float) clampTo (v, 0.5, 12.0); return true; } },
    { "filter_range",        [] (fc_bass& b, double v) { b.settings.envelopeRange = choice (v, 3); return true; } },
    { "filter_keep_lows",    [] (fc_bass& b, double v) { b.settings.envelopeKeepLows = on (v); return true; } },
    { "filter_keep_hz",      [] (fc_bass& b, double v) { b.settings.envelopeKeepHz = (float) clampTo (v, 80.0, 300.0); return true; } },
    { "filter_release_ms",   [] (fc_bass& b, double v) { b.settings.envelopeReleaseMs = (float) clampTo (v, 5.0, 2000.0); return true; } },

    { "chorus_on",           [] (fc_bass& b, double v) { b.links.chorusOn = on (v); return true; } },
    { "chorus_present",      [] (fc_bass& b, double v) { b.links.chorusPresent = on (v); return true; } },
    { "chorus",              [] (fc_bass& b, double v) { b.settings.chorus = (float) clampTo (v, 0.0, 1.0); return true; } },
    { "chorus_rate_hz",      [] (fc_bass& b, double v) { b.settings.chorusRateHz = (float) clampTo (v, 0.05, 10.0); return true; } },
    { "chorus_crossover_hz", [] (fc_bass& b, double v) { b.settings.chorusCrossoverHz = (float) clampTo (v, 200.0, 500.0); return true; } },

    { "preamp_eq_lo_db",     [] (fc_bass& b, double v) { b.console.loDb = (double) (float) clampTo (v, -15.0, 15.0); return true; } },
    { "preamp_eq_lo_hz",     [] (fc_bass& b, double v) { b.console.loHz = (double) (float) clampTo (v, 30.0, 500.0); return true; } },
    { "preamp_eq_b1_db",     [] (fc_bass& b, double v) { b.console.b1Db = (double) (float) clampTo (v, -15.0, 15.0); return true; } },
    { "preamp_eq_b1_hz",     [] (fc_bass& b, double v) { b.console.b1Hz = (double) (float) clampTo (v, 60.0, 3000.0); return true; } },
    { "preamp_eq_b1_q",      [] (fc_bass& b, double v) { b.console.b1Q = (double) (float) clampTo (v, 0.3, 6.0); return true; } },
    { "preamp_eq_hi_db",     [] (fc_bass& b, double v) { b.console.hiDb = (double) (float) clampTo (v, -15.0, 15.0); return true; } },
    { "preamp_eq_hi_hz",     [] (fc_bass& b, double v) { b.console.hiHz = (double) (float) clampTo (v, 1500.0, 16000.0); return true; } },

    { "amp_blend_wet",       [] (fc_bass& b, double v) { b.blendWet = (float) clampTo (v, 0.0, 1.0); b.tellBlend(); return true; } },
    { "amp_blend_hz",        [] (fc_bass& b, double v) { b.blendHz = (float) clampTo (v, 80.0, 250.0); b.tellBlend(); return true; } },
    { "amp_blend_invert",    [] (fc_bass& b, double v) { b.blendInvert = on (v); b.tellBlend(); return true; } },

    { "cab_on",              [] (fc_bass& b, double v) { b.settings.cabWorks = on (v); return true; } },
    { "cab_mix",             [] (fc_bass& b, double v) { b.settings.cabMix = (float) clampTo (v, 0.0, 100.0); return true; } },

    { "bass_hpf_on",         [] (fc_bass& b, double v) { b.settings.hpfOn = on (v); return true; } },
    { "bass_hpf",            [] (fc_bass& b, double v) { b.settings.hpf = choice (v, 2); return true; } },
    { "bass_hpf_hz_4",       [] (fc_bass& b, double v) { b.settings.hpfHz4 = (float) clampTo (v, 20.0, 60.0); return true; } },
    { "bass_hpf_hz_5",       [] (fc_bass& b, double v) { b.settings.hpfHz5 = (float) clampTo (v, 15.0, 45.0); return true; } },
    { "bass_lpf_on",         [] (fc_bass& b, double v) { b.settings.lpfOn = on (v); return true; } },
    { "bass_lpf",            [] (fc_bass& b, double v) { b.settings.lpf = choice (v, 2); return true; } },
    { "bass_lpf_hz_fingers", [] (fc_bass& b, double v) { b.settings.lpfHzFingers = (float) clampTo (v, 2000.0, 8000.0); return true; } },
    { "bass_lpf_hz_slap",    [] (fc_bass& b, double v) { b.settings.lpfHzSlap = (float) clampTo (v, 5000.0, 16000.0); return true; } },
};
} // namespace

FC_EXPORT uint32_t fc_bass_abi_version (void) { return FC_BASS_ABI_VERSION; }

FC_EXPORT fc_bass* fc_bass_create (void)
{
    auto* b = new (std::nothrow) fc_bass();
    if (b == nullptr) return nullptr;
    b->rig = fc_rig_create();
    if (b->rig == nullptr) { delete b; return nullptr; }
    b->tellBlend();   // the app's BLEND default: the capture alone — the player without the crossover
    return b;
}

FC_EXPORT void fc_bass_destroy (fc_bass* bass)
{
    if (bass == nullptr) return;
    fc_rig_destroy (bass->rig);
    delete bass;
}

FC_EXPORT int fc_bass_prepare (fc_bass* bass, double sampleRate, int maxBlock)
{
    if (bass == nullptr) return 0;
    bass->prepared = false;   // disarm first: a refusal leaves nothing half-prepared playing
    if (! (sampleRate >= 1000.0 && sampleRate <= 768000.0) || maxBlock < 1 || maxBlock > kMaxBlock) return 0;
    if (fc_rig_prepare (bass->rig, sampleRate, maxBlock, 1) != 1) return 0;   // a bass: one network per capture
    bass->rate  = sampleRate;
    bass->block = maxBlock;
    if (! bass->prepareChain()) return 0;
    for (auto& plane : bass->io)
        plane.assign ((std::size_t) maxBlock, 0.0f);
    bass->prepared = true;
    return 1;
}

FC_EXPORT float* fc_bass_buffer (fc_bass* bass, int channel)
{
    if (bass == nullptr || ! bass->prepared || channel < 0 || channel > 1) return nullptr;
    return bass->io[channel].data();
}

FC_EXPORT int fc_bass_process (fc_bass* bass, float* left, float* right, int n)
{
    if (bass == nullptr || ! bass->prepared || n < 0 || n > bass->block || left == nullptr) return 0;
    if (n == 0) return 1;
    // The app at the top of every block: the AMP's console told its settings (updateEqSettings), the bass's own.
    bass->tone.setSettings (felitronics::bass::toneSettings (bass->console));
    bass->chain.process (left, right, n, bass->settings, [bass] (float* mono, int m)
    {
        (void) fc_rig_process (bass->rig, mono, nullptr, m);
        float* io[] { mono };
        bass->tone.process (io, 1, m);
    });
    bass->chain.cabinet().flushPending();   // a cabinet handed over mid-crossfade gets its next chance (the app's pump)
    return 1;
}

FC_EXPORT void fc_bass_reset (fc_bass* bass)
{
    if (bass == nullptr) return;
    fc_rig_reset (bass->rig);
    if (bass->prepared)
        bass->prepared = bass->prepareChain();
}

FC_EXPORT fc_rig* fc_bass_rig (fc_bass* bass) { return bass != nullptr ? bass->rig : nullptr; }

FC_EXPORT int fc_bass_set (fc_bass* bass, const char* id, double value)
{
    if (bass == nullptr || id == nullptr || ! std::isfinite (value)) return 0;
    for (const auto& k : kKnobs)
        if (std::strcmp (k.id, id) == 0)
        {
            const bool taken = k.set (*bass, value);
            bass->sync();
            return taken ? 1 : 0;
        }
    return 0;
}

FC_EXPORT float* fc_bass_cab_taps (fc_bass* bass, int length)
{
    if (bass == nullptr || length < 1 || (double) length > felitronics::cabinet::CabinetIr::maxSeconds * 768000.0) return nullptr;
    bass->cabTaps.assign ((std::size_t) length, 0.0f);
    return bass->cabTaps.data();
}

FC_EXPORT int fc_bass_cab_load (fc_bass* bass, int length, double rate)
{
    if (bass == nullptr || length < 1 || (std::size_t) length > bass->cabTaps.size()
        || ! felitronics::cabinet::CabinetIr::takes (length, rate))
        return 0;
    bass->cabKept.assign (bass->cabTaps.begin(), bass->cabTaps.begin() + length);
    bass->cabRate = rate;
    if (bass->prepared && ! bass->chain.cabinet().load (bass->cabKept.data(), length, rate)) return 0;
    if (bass->prepared) bass->chain.cabinet().flushPending();
    return 1;
}

FC_EXPORT float fc_bass_comp_gr_db (fc_bass* bass) { return bass != nullptr ? bass->chain.compGrDb() : 0.0f; }

FC_EXPORT int fc_bass_latency_samples (fc_bass* bass) { return bass != nullptr ? fc_rig_latency_samples (bass->rig) : 0; }
