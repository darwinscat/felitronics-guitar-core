// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// fc_guitar_fx against the core classes it wraps. The reference below drives delay::TapeDelay and reverb::AmpReverb
// DIRECTLY, the way the app drives them: its defaults are the app's parameter defaults written out as numbers
// (OrbitAmp src/Parameters.cpp), its mapping is the app's updateDelaySettings / updateReverbSettings with core's knob
// law where the migration put it (DelayLaw's darkHzForPercent for the app's pow), and it hands every knob over at the
// top of EVERY block, as the app's processBlock does. The ABI hands them over only when one moved — so a render that
// matches bit for bit also proves that shortcut changes nothing. Then the contract, the dry at unity, and no
// allocation in process().

#include "fc_guitar_fx_abi.h"

#include <felitronics_test.h>
#include <alloc_counter.h>   // installs the allocation counter

#include <felitronics/delay/DelayLaw.h>
#include <felitronics/delay/TapeDelay.h>
#include <felitronics/reverb/AmpReverb.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using felitronics::test::group;
using felitronics::test::ok;
namespace delay  = felitronics::delay;
namespace reverb = felitronics::reverb;
namespace test   = felitronics::test;

namespace
{
constexpr double kPi = 3.14159265358979323846;

/** The app's knobs in the app's units, at the app's defaults — written as the layout writes them. */
struct AppKnobs
{
    bool  delayOn = true, delaySync = true;
    int   delayDiv = 5;   // "1/4"
    float delayBpm = 120.0f, delayTimeMs = 350.0f, delayRepeats = 35.0f, delayDark = 45.0f, delayOffset = 0.0f,
          delayMix = 0.0f;
    bool  reverbOn = true;
    int   reverbType = 3;   // "Plate"
    float reverbMix = 30.0f, reverbDecay = 1.0f, reverbPredelay = 0.0f, reverbHpfHz = 120.0f;
};

/** The two core stages, driven directly. */
struct Direct
{
    delay::TapeDelay  d;
    reverb::AmpReverb r;
    AppKnobs k;

    void update()   // updateDelaySettings, then updateReverbSettings
    {
        float ms = k.delayTimeMs;
        if (k.delaySync)
            ms = (float) ((double) delay::kDivisionBeats[k.delayDiv] * 60000.0 / (double) k.delayBpm);
        d.setTimeMs (ms);
        d.setRepeats (k.delayRepeats * 0.01f);
        d.setDarkHz (delay::darkHzForPercent (k.delayDark));
        d.setOffsetMs (k.delayOffset);
        d.setMix (k.delayMix * 0.01f);

        r.setCharacter (static_cast<reverb::Character> (k.reverbType));
        r.setMix (k.reverbMix * 0.01f);
        r.setDecay (k.reverbDecay);
        r.setPredelayMs (k.reverbPredelay);
        r.setHpfHz (k.reverbHpfHz);
    }

    bool prepare (double sr, int block)
    {
        update();   // the constructor's hand-over
        const bool ok = d.prepare (sr, block, 2) && r.prepare (sr, block, 2);
        update();   // prepareToPlay's
        return ok;
    }

    bool process (float* l, float* rr, int n)
    {
        update();   // the top of every block
        float* io[2] = { l, rr };
        const int nch = rr != nullptr ? 2 : 1;
        return d.process (io, nch, n, k.delayOn) && r.process (io, nch, n, k.reverbOn);
    }
};

/** The same knob, both sides: the reference's field and the ABI's setter. */
struct Move
{
    int frame;   // before the first block starting at or after this frame; -1 = before prepare
    std::function<void (AppKnobs&)> ref;
    std::function<void (fc_guitar_fx*)> abi;
};

/** A plucked, tapered, stereo-different test input with a noise burst — deterministic. */
std::vector<float> makeInput (int frames, double sr, int ch)
{
    std::vector<float> x ((std::size_t) frames, 0.0f);
    std::uint32_t seed = 0x2545F491u + (std::uint32_t) ch;
    for (int i = 0; i < frames; ++i)
    {
        const double t     = i / sr;
        const double pluck = std::fmod (t, 0.6);
        const double env   = std::exp (-pluck * 7.0) * (pluck < 0.002 ? pluck / 0.002 : 1.0);
        const double f     = ch == 0 ? 196.0 : 246.94;
        double v = 0.45 * env * (std::sin (2.0 * kPi * f * t) + 0.3 * std::sin (2.0 * kPi * 3.0 * f * t));
        seed = seed * 1664525u + 1013904223u;
        if (t > 1.0 && t < 1.05)
            v += 0.2 * ((double) (seed >> 8) / 16777216.0 - 0.5);
        x[(std::size_t) i] = (float) v;
    }
    const int fade = (int) (0.01 * sr);
    for (int i = 0; i < fade && i < frames; ++i)
        x[(std::size_t) (frames - 1 - i)] *= (float) i / (float) fade;
    return x;
}

/** Renders `frames` through both, block sizes from `blocks` in turn (a 0 is a mono block of 64), the moves at their
    frames, and compares every sample's bits. */
void same (const std::string& name, double sr, int maxBlock, int frames, const std::vector<int>& blocks,
           const std::vector<Move>& moves, int resetAt = -1)
{
    Direct ref;
    fc_guitar_fx* fx = fc_guitar_fx_create();
    ok (fx != nullptr, name + ": create");
    if (fx == nullptr)
        return;

    for (const auto& m : moves)
        if (m.frame < 0) { m.ref (ref.k); m.abi (fx); }

    ok (ref.prepare (sr, maxBlock), name + ": the reference prepares");
    ok (fc_guitar_fx_prepare (fx, sr, maxBlock) == 1, name + ": the ABI prepares");

    auto inL = makeInput (frames, sr, 0), inR = makeInput (frames, sr, 1);
    auto aL = inL, aR = inR, bL = inL, bR = inR;
    std::vector<bool> done (moves.size(), false);
    bool resetDone = false;
    long mismatches = 0, firstAt = -1;
    double worst = 0.0;

    std::size_t bi = 0;
    for (int at = 0; at < frames;)
    {
        const int  spec = blocks[bi++ % blocks.size()];
        const bool mono = spec == 0;
        const int  n    = std::min (mono ? 64 : spec, frames - at);
        for (std::size_t i = 0; i < moves.size(); ++i)
            if (! done[i] && moves[i].frame >= 0 && moves[i].frame <= at)
            {
                moves[i].ref (ref.k);
                moves[i].abi (fx);
                done[i] = true;
            }
        if (resetAt >= 0 && ! resetDone && at >= resetAt)
        {
            ref.d.reset(); ref.r.reset();
            fc_guitar_fx_reset (fx);
            resetDone = true;
        }
        const bool r1 = ref.process (aL.data() + at, mono ? nullptr : aR.data() + at, n);
        const bool r2 = fc_guitar_fx_process (fx, bL.data() + at, mono ? nullptr : bR.data() + at, n) == 1;
        if (! r1 || ! r2)
        {
            ok (false, name + ": a block was refused at frame " + std::to_string (at));
            break;
        }
        at += n;
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto& a = ch == 0 ? aL : aR;
        const auto& b = ch == 0 ? bL : bR;
        for (int i = 0; i < frames; ++i)
        {
            std::uint32_t ua, ub;
            std::memcpy (&ua, &a[(std::size_t) i], 4);
            std::memcpy (&ub, &b[(std::size_t) i], 4);
            if (ua != ub)
            {
                if (firstAt < 0) firstAt = i;
                ++mismatches;
                worst = std::max (worst, (double) std::fabs (a[(std::size_t) i] - b[(std::size_t) i]));
            }
        }
    }

    // ...and the render is not the input: a comparison of two untouched buffers would pass as well.
    double moved = 0.0;
    for (int i = 0; i < frames; ++i)
        moved = std::max (moved, (double) std::fabs (bL[(std::size_t) i] - inL[(std::size_t) i]));

    ok (mismatches == 0, name + ": bit-identical to the core classes driven directly (" + std::to_string (mismatches)
                             + " samples differ, first at " + std::to_string (firstAt) + ", worst "
                             + std::to_string (worst) + ")");
    ok (moved > 1e-3, name + ": the stages added something (max |out - in| " + std::to_string (moved) + ")");
    fc_guitar_fx_destroy (fx);
}

#define KNOB(field, setter, v) Move { -1, [] (AppKnobs& k) { k.field = v; }, [] (fc_guitar_fx* f) { setter (f, v); } }
#define KNOB_AT(at, field, setter, v) Move { at, [] (AppKnobs& k) { k.field = v; }, [] (fc_guitar_fx* f) { setter (f, v); } }

void bitIdentity()
{
    group ("the ABI renders bit-identical to TapeDelay + AmpReverb driven directly with the app's mapping");

    // The app's defaults, untouched on both sides: Plate at 30, the delay on with nothing in its mix.
    same ("defaults 48k/128", 48000.0, 128, 96000, { 128 }, {});

    // Every character under a busy echo: 1/8. at 132 BPM, long repeats, dark, offset, the room's four refinements.
    for (int c = 0; c < reverb::kNumCharacters; ++c)
    {
        std::vector<Move> mv = {
            KNOB (delaySync, fc_guitar_fx_set_delay_sync, true),
            KNOB (delayDiv, fc_guitar_fx_set_delay_division, 7),
            KNOB (delayBpm, fc_guitar_fx_set_delay_bpm, 132.0f),
            KNOB (delayRepeats, fc_guitar_fx_set_delay_repeats, 62.0f),
            KNOB (delayDark, fc_guitar_fx_set_delay_dark, 30.0f),
            KNOB (delayOffset, fc_guitar_fx_set_delay_offset_ms, 12.5f),
            KNOB (delayMix, fc_guitar_fx_set_delay_mix, 45.0f),
            KNOB (reverbMix, fc_guitar_fx_set_reverb_mix, 55.0f),
            KNOB (reverbDecay, fc_guitar_fx_set_reverb_decay, 1.6f),
            KNOB (reverbPredelay, fc_guitar_fx_set_reverb_predelay_ms, 25.0f),
            KNOB (reverbHpfHz, fc_guitar_fx_set_reverb_hpf_hz, 210.0f),
        };
        mv.push_back (Move { -1, [c] (AppKnobs& k) { k.reverbType = c; },
                             [c] (fc_guitar_fx* f) { fc_guitar_fx_set_reverb_character (f, c); } });
        same (std::string ("busy echo + ") + reverb::kCharacterNames[c] + " 44.1k/128", 44100.0, 128, 88200, { 128 }, mv);
    }

    // Knobs moving mid-stream, ragged blocks, mono blocks, both ON switches toggling, a reset: the app's per-block
    // hand-over against the ABI's hand-over-when-moved.
    same ("moves, ragged and mono blocks, stand-by, reset 48k/256", 48000.0, 256, 144000, { 128, 37, 256, 1, 0, 200, 0, 99 },
          {
              KNOB (delayMix, fc_guitar_fx_set_delay_mix, 50.0f),
              KNOB (reverbType, fc_guitar_fx_set_reverb_character, 5),   // Modulated
              KNOB_AT (12000, delaySync, fc_guitar_fx_set_delay_sync, false),
              KNOB_AT (12000, delayTimeMs, fc_guitar_fx_set_delay_time_ms, 900.0f),
              KNOB_AT (30000, delayTimeMs, fc_guitar_fx_set_delay_time_ms, 210.0f),   // the head glides: a bend
              KNOB_AT (40000, delayOn, fc_guitar_fx_set_delay_on, false),             // stand-by: rings out
              KNOB_AT (52000, reverbType, fc_guitar_fx_set_reverb_character, 2),      // Hall
              KNOB_AT (60000, delayOn, fc_guitar_fx_set_delay_on, true),
              KNOB_AT (60000, delaySync, fc_guitar_fx_set_delay_sync, true),
              KNOB_AT (60000, delayDiv, fc_guitar_fx_set_delay_division, 3),          // 1/2T
              KNOB_AT (70000, reverbOn, fc_guitar_fx_set_reverb_on, false),
              KNOB_AT (80000, reverbOn, fc_guitar_fx_set_reverb_on, true),
              KNOB_AT (90000, delayDark, fc_guitar_fx_set_delay_dark, 90.0f),
              KNOB_AT (90000, delayOffset, fc_guitar_fx_set_delay_offset_ms, -30.0f),
              KNOB_AT (100000, reverbDecay, fc_guitar_fx_set_reverb_decay, 0.5f),
              KNOB_AT (100000, reverbHpfHz, fc_guitar_fx_set_reverb_hpf_hz, 40.0f),
              KNOB_AT (110000, delayBpm, fc_guitar_fx_set_delay_bpm, 40.0f),
              KNOB_AT (110000, delayDiv, fc_guitar_fx_set_delay_division, 0),          // 1/1 at 40: six seconds
          },
          75000);

    // Out-of-range knobs land on the app parameter's range; a NaN leaves the knob where it stood.
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        same ("clamped and ignored knobs 96k/512", 96000.0, 512, 96000, { 512, 128 },
              {
                  Move { -1, [] (AppKnobs& k) { k.delayMix = 100.0f; k.delayRepeats = 100.0f; k.delayDiv = 12; k.delayBpm = 240.0f; },
                         [] (fc_guitar_fx* f) { fc_guitar_fx_set_delay_mix (f, 400.0f); fc_guitar_fx_set_delay_repeats (f, 1e9f);
                                                fc_guitar_fx_set_delay_division (f, 99); fc_guitar_fx_set_delay_bpm (f, 900.0f); } },
                  Move { -1, [] (AppKnobs& k) { k.reverbType = 0; k.reverbMix = 0.0f; k.reverbDecay = 2.0f; k.reverbHpfHz = 500.0f; },
                         [nan] (fc_guitar_fx* f) { fc_guitar_fx_set_reverb_character (f, -4); fc_guitar_fx_set_reverb_mix (f, -10.0f);
                                                   fc_guitar_fx_set_reverb_decay (f, 7.0f); fc_guitar_fx_set_reverb_hpf_hz (f, 9000.0f);
                                                   fc_guitar_fx_set_reverb_predelay_ms (f, nan); fc_guitar_fx_set_delay_dark (f, nan); } },
                  Move { 30000, [] (AppKnobs& k) { k.delaySync = false; k.delayTimeMs = 20.0f; k.delayOffset = 30.0f; k.reverbPredelay = 100.0f; k.reverbMix = 100.0f; },
                         [] (fc_guitar_fx* f) { fc_guitar_fx_set_delay_sync (f, 0); fc_guitar_fx_set_delay_time_ms (f, -5.0f);
                                                fc_guitar_fx_set_delay_offset_ms (f, 31.0f); fc_guitar_fx_set_reverb_predelay_ms (f, 250.0f);
                                                fc_guitar_fx_set_reverb_mix (f, 101.0f); } },
                  Move { 60000, [] (AppKnobs& k) { k.delayTimeMs = 2000.0f; k.reverbHpfHz = 40.0f; },
                         [] (fc_guitar_fx* f) { fc_guitar_fx_set_delay_time_ms (f, 1e6f); fc_guitar_fx_set_reverb_hpf_hz (f, 0.0f); } },
              });
    }
}

void contract()
{
    group ("the contract: the version, refusals that disarm, the module's buffers, NULL");

    ok (fc_guitar_fx_abi_version() == FC_GUITAR_FX_ABI_VERSION && FC_GUITAR_FX_ABI_VERSION == 1u, "abi_version answers 1");

    fc_guitar_fx* fx = fc_guitar_fx_create();
    float l[64] = {}, r[64] = {};
    ok (fc_guitar_fx_buffer (fx, 0) == nullptr, "no buffer before prepare");
    ok (fc_guitar_fx_process (fx, l, r, 64) == 0, "process before prepare is refused");

    ok (fc_guitar_fx_prepare (fx, 48000.0, 128) == 1, "prepare 48k/128");
    ok (fc_guitar_fx_buffer (fx, 0) != nullptr && fc_guitar_fx_buffer (fx, 1) != nullptr
            && fc_guitar_fx_buffer (fx, 0) != fc_guitar_fx_buffer (fx, 1), "two distinct channel buffers");
    ok (fc_guitar_fx_buffer (fx, 2) == nullptr && fc_guitar_fx_buffer (fx, -1) == nullptr, "no third buffer");
    ok (fc_guitar_fx_process (fx, l, r, 0) == 1, "n == 0 is a no-op that answers 1");
    ok (fc_guitar_fx_process (fx, l, r, -1) == 0, "n < 0 is refused");
    ok (fc_guitar_fx_process (fx, nullptr, r, 64) == 0, "a NULL left channel is refused");
    ok (fc_guitar_fx_process (fx, l, nullptr, 64) == 1, "a NULL right channel is one channel");
    ok (fc_guitar_fx_process (fx, l, r, 1000) == 1, "n above maxBlock is processed in full (own pointers)");

    const struct { double sr; int block; const char* what; } bad[] = {
        { 999.0, 128, "a rate below 1 kHz" }, { 768001.0, 128, "a rate above 768 kHz" },
        { std::numeric_limits<double>::quiet_NaN(), 128, "a NaN rate" }, { 48000.0, 0, "maxBlock 0" },
        { 48000.0, 65537, "maxBlock above 65536" },
    };
    for (const auto& b : bad)
    {
        ok (fc_guitar_fx_prepare (fx, 48000.0, 128) == 1, std::string ("re-prepare before ") + b.what);
        ok (fc_guitar_fx_prepare (fx, b.sr, b.block) == 0, std::string ("refuses ") + b.what);
        ok (fc_guitar_fx_process (fx, l, r, 64) == 0 && fc_guitar_fx_buffer (fx, 0) == nullptr,
            std::string ("...and the refusal disarms: ") + b.what);
    }
    ok (fc_guitar_fx_prepare (fx, 1000.0, 1) == 1 && fc_guitar_fx_prepare (fx, 768000.0, 65536) == 1,
        "the edges of the ranges are accepted");
    fc_guitar_fx_destroy (fx);

    fc_guitar_fx_destroy (nullptr);
    fc_guitar_fx_reset (nullptr);
    fc_guitar_fx_set_delay_mix (nullptr, 50.0f);
    fc_guitar_fx_set_reverb_character (nullptr, 2);
    ok (fc_guitar_fx_prepare (nullptr, 48000.0, 128) == 0 && fc_guitar_fx_process (nullptr, l, r, 64) == 0
            && fc_guitar_fx_buffer (nullptr, 0) == nullptr, "a NULL instance is refused everywhere, and nothing crashes");
}

void dryAtUnity()
{
    group ("both links only ADD: at mix 0 and 0 the output is the input");
    fc_guitar_fx* fx = fc_guitar_fx_create();
    fc_guitar_fx_set_delay_mix (fx, 0.0f);
    fc_guitar_fx_set_delay_repeats (fx, 90.0f);
    fc_guitar_fx_set_reverb_mix (fx, 0.0f);
    fc_guitar_fx_set_reverb_character (fx, 2);
    test::run (fc_guitar_fx_prepare (fx, 48000.0, 128) == 1);
    const auto inL = makeInput (48000, 48000.0, 0), inR = makeInput (48000, 48000.0, 1);
    auto l = inL, r = inR;
    for (int at = 0; at < 48000; at += 128)
        test::run (fc_guitar_fx_process (fx, l.data() + at, r.data() + at, std::min (128, 48000 - at)) == 1);
    // Equal as VALUES: the stage adds `0 · wet`, and -0 + 0 is +0 — the sign of a zero is the one bit an addition
    // of nothing may change.
    long diff = 0;
    for (std::size_t i = 0; i < l.size(); ++i)
        diff += (l[i] != inL[i]) + (r[i] != inR[i]);
    ok (diff == 0, "dry passes untouched at mix 0 / 0");
    fc_guitar_fx_destroy (fx);
}

void noAllocation()
{
    group ("RT: process, reset and the setters never allocate");
    fc_guitar_fx* fx = fc_guitar_fx_create();
    test::run (fc_guitar_fx_prepare (fx, 48000.0, 128) == 1);
    float* l = fc_guitar_fx_buffer (fx, 0);
    float* r = fc_guitar_fx_buffer (fx, 1);
    for (int i = 0; i < 128; ++i) { l[i] = 0.3f; r[i] = -0.2f; }

    const long long before = felitronics::test::alloc::count.load();
    for (int b = 0; b < 400; ++b)
    {
        fc_guitar_fx_set_delay_mix (fx, (float) (b % 100));
        fc_guitar_fx_set_delay_sync (fx, b & 1);
        fc_guitar_fx_set_delay_time_ms (fx, 100.0f + (float) b);
        fc_guitar_fx_set_delay_dark (fx, (float) (b % 100));
        fc_guitar_fx_set_reverb_character (fx, b % 6);
        fc_guitar_fx_set_reverb_decay (fx, 0.5f + (float) (b % 10) * 0.15f);
        fc_guitar_fx_set_reverb_hpf_hz (fx, 40.0f + (float) b);
        fc_guitar_fx_set_delay_on (fx, b % 7 != 0);
        fc_guitar_fx_set_reverb_on (fx, b % 5 != 0);
        test::run (fc_guitar_fx_process (fx, l, r, 128) == 1);
        test::run (fc_guitar_fx_process (fx, l, nullptr, 64) == 1);
        if (b % 50 == 0)
            fc_guitar_fx_reset (fx);
    }
    felitronics::test::okNoAlloc (felitronics::test::alloc::count.load() == before,
                                  "no allocation: setters, process (stereo, mono), reset");
    fc_guitar_fx_destroy (fx);
}
} // namespace

int main()
{
    std::printf ("felitronics_guitar_fx_abi_tests\n");
    contract();
    bitIdentity();
    dryAtUnity();
    noAllocation();
    return felitronics::test::report();
}
