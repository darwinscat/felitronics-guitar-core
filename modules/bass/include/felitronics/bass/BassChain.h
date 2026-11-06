// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

#pragma once

// felitronics::bass — THE BASS AMP'S CHAIN, its stages in the app's order and the app's knobs wired to them. Moved here
// from OrbitAmp's processor (src/PluginProcessor.cpp: processChunk's bass path, tellOctave / filterKnobsNow /
// tellChorus, innerCutSettings / bassCutSettings, the bass's tone in updateEqSettings, the CAB's DI mix), so the
// bass amps' plug-ins and their web port play one chain. JUCE-free, header-only; the stages are felitronics-core's.
//
//   input cut → GATE (its key; muting here at START) → ENVELOPE's key → COMP → cut → OCTAVE → cut → ENVELOPE
//     → THE AMP (the host's: the capture, its BLEND, then its TONE — toneSettings()) →
//   cut → GATE muting at the END (the default) → the seam (one signal to the bus's planes) → CHORUS
//     → (DELAY, REVERB — the host's, where its chain holds them) → CAB with the DI mixed in → the bass's cuts
//
// Mono up to the seam (the bus's channel 0), the bus's planes (one or two) from it on. The host's: the input and
// output trims, the AMP, the echo and the room, the limiter, the door — and every meter and picture (the stages'
// read-outs are here for them). ONE METHOD PER STAGE, each called once per block in the order above with that block's
// Settings (a snapshot: the same for every stage of one block); process() calls them all in that order with the AMP as
// the host's callback — what a host without stages of its own between them plays.
//
// THE CONTRACT. prepare() allocates; the stage methods allocate nothing, lock nothing, throw nothing. One thread owns
// the chain's audio side; cabinet() is the cabinet's producer side (CabinetIr's own contract: its prepare, load and post
// on another thread, one thread at a time — a host with more holds its own lock around them). A block longer than the
// prepared maximum is refused by the stages that keep a copy of it (ENVELOPE's key, the fades, the DI) exactly as the app
// refuses it: a host cuts its blocks to what it prepared.

#include <felitronics/basscomp/BassComp.h>
#include <felitronics/cabinet/CabinetIr.h>
#include <felitronics/chorus/BassChorus.h>
#include <felitronics/chorus/ChorusLaw.h>
#include <felitronics/core/BypassFade.h>
#include <felitronics/core/Math.h>
#include <felitronics/dynamics/NoiseGate.h>
#include <felitronics/envfilter/EnvelopeFilter.h>
#include <felitronics/envfilter/EnvelopeFilterLaw.h>
#include <felitronics/eq/BassCuts.h>
#include <felitronics/eq/EqLink.h>
#include <felitronics/octave/OctaveLaw.h>
#include <felitronics/octave/Octaver.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace felitronics::bass
{

/** The bass amp's knobs as its chain reads them — the app's parameters, in the app's units, and which of its links
    work (the app's linkWorks: the link switched on and, where it can leave the strip, in it). The defaults are the
    app's parameter defaults. */
struct Settings
{
    // GATE
    bool  gateWorks        = false;
    float gateThresholdDb  = -50.0f;    // -80 … -10
    bool  gateMutesAtStart = false;     // the gate's mute at the START; the default, false, mutes at the END, after the AMP

    // COMP (basscomp::BassComp)
    bool  compWorks       = true;
    float compBlend       = 0.5f;       // 0 … 1, the knob
    float compThresholdDb = -24.0f;     // -48 … 0
    float compRatio       = 4.0f;       // 1.5 … 10
    int   compAttack      = 1;          // 0 slow, 1 med, 2 fast
    int   compRelease     = 0;          // 0 auto, 1 fast, 2 slow
    int   compCharacter   = 2;          // 0 smooth, 1 punchy, 2 opto, 3 fet, 4 squash

    // OCTAVE (octave::Octaver through its law)
    bool  octaveWorks  = true;
    float octave       = 0.0f;          // 0 … 1, the slider
    float octaveToneHz = 120.0f;        // 30 … 2000

    // ENVELOPE (envfilter::EnvelopeFilter through its law)
    bool  envelopeWorks      = true;
    float envelope           = 0.0f;    // 0 … 1, the slider
    int   envelopeMode       = 0;       // 0 LP, 1 BP
    int   envelopeDirection  = 0;       // 0 up, 1 down
    float envelopeResonance  = 4.0f;    // 0.5 … 12
    int   envelopeRange      = 1;       // 0 low, 1 mid, 2 high
    bool  envelopeKeepLows   = true;
    float envelopeKeepHz     = 150.0f;  // 80 … 300
    float envelopeReleaseMs  = 120.0f;  // 5 … 2000

    // CHORUS (chorus::BassChorus through its law)
    bool  chorusWorks       = true;
    float chorus            = 0.0f;     // 0 … 1, the slider
    float chorusRateHz      = 0.7f;     // 0.05 … 10
    float chorusCrossoverHz = 200.0f;   // 200 … 500

    // CAB and its MIX against the DI
    bool  cabWorks = true;
    float cabMix   = 70.0f;             // percent, 100 the cabinet alone

    // THE BASS'S CUTS: the HPF by the strings, the LPF by the hand, each its switch apart from its place
    bool  hpfOn        = true;
    int   hpf          = 0;             // 0 5 STR, 1 4 STR
    float hpfHz4       = 32.0f;
    float hpfHz5       = 22.0f;
    bool  lpfOn        = false;
    int   lpf          = 0;             // 0 FINGERS, 1 SLAP
    float lpfHzFingers = 4500.0f;
    float lpfHzSlap    = 9500.0f;

    bool  innerCutsOut = false;         // for a gate: the HPF at the END alone, none across the chain
};

/** THE AMP'S TONE ON THE BASS (the app's updateEqSettings for its bass standard): the console's settings `s` with the
    bass's own overrides — no cut of the console's (the bass's are its own), MIDDLE's frequency held to 200 … 3000 Hz,
    the second bell and the scalpel out of the sound; on a pack that wears a tone of its own, BASS, MIDDLE and TREBLE
    stand flat. */
inline eq::EqLink::Settings toneSettings (eq::EqLink::Settings s, bool packWearsTone = false) noexcept
{
    s.hpfOn = false;
    s.lpfOn = false;
    s.b1Hz  = std::clamp (s.b1Hz, 200.0, 3000.0);
    s.b2Db  = 0.0;
    s.b3On  = false;
    s.b3Db  = 0.0;
    if (packWearsTone)
        s.loDb = s.b1Db = s.hiDb = 0.0;
    return s;
}

/** THE TWO VECTOR PASSES OF THE CAB'S SETTLED DI MIX — a scale, then a scaled add — as a host's vector library does
    them: this plain loop by default; a plug-in hands its own library's (JUCE's FloatVectorOperations) so the bits are the
    ones it played before the chain moved here. */
struct ScalarVectorOps
{
    static void multiply (float* d, float k, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
            d[i] *= k;
    }

    static void addWithMultiply (float* d, const float* src, float k, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
            d[i] += src[i] * k;
    }
};

template <typename VectorOps = ScalarVectorOps>
class BassChainT
{
public:
    /** What OCTAVE no longer asks the player: no -2, the sub's own high pass left to the chain's HPF (core's lowest),
        the tracking gate the GATE's threshold while the GATE works, else this. */
    static constexpr float octaveSubHpHz = 10.0f, octaveGateIdleDb = -50.0f;
    /** ENVELOPE's follower attacks in a fixed 3 ms; its RANGE's sweeps. */
    static constexpr float envelopeAttackMs = 3.0f;
    static constexpr float envelopeRangeHz[3][2] = { { 100.0f, 1500.0f }, { 150.0f, 3000.0f }, { 300.0f, 6000.0f } };
    /** CHORUS's head swings about a fixed centre. */
    static constexpr float chorusCentreMs = 8.0f;

    /** For `sampleRate`, host blocks of up to `maxBlock`, a bus of `channels` (1 or 2) — told `s` before its stages
        prepare, so they start where it stands (a slider at 0 idle from the first sample, one up on its knobs). False when
        a stage refused; the chain then plays what the app plays with that stage refused. */
    bool prepare (double sampleRate, int maxBlock, int channels, const Settings& s)
    {
        rate  = sampleRate;
        block = std::max (1, maxBlock);
        wide  = std::clamp (channels, 1, 2);

        bool ok = true;
        cab.setReference (cabinet::CabinetIr::Reference::bass);
        cab.prepare (sampleRate, block, std::max (1, channels));
        ok = gate.prepare (sampleRate, block, std::max (1, channels)) && ok;
        gate.seedEnabled (s.gateWorks);

        compReady = comp.prepare (sampleRate, block);
        comp.snapAmount ((double) s.compBlend);
        cuts.prepare (sampleRate);
        for (auto& c : innerCuts)
            c.prepare (sampleRate);
        diNow  = std::clamp ((100.0f - s.cabMix) * 0.01f, 0.0f, 1.0f);
        diStep = (float) (1000.0 / (std::max (8000.0, sampleRate) * 20.0));

        octaveToldValid = envelopeToldValid = chorusToldValid = false;
        tellOctave (s);
        tellEnvelope (s);
        tellChorus (s);
        octaverReady  = octaver.prepare (sampleRate, block, 1);
        envelopeReady = envFilter.prepare (sampleRate, block, 1);
        chorusReady   = chorusStage.prepare (sampleRate, block, wide);
        key.assign ((size_t) block, 0.0f);

        diDry.assign ((size_t) std::max (2, channels) * (size_t) block, 0.0f);
        fadeDry.assign ((size_t) std::max (2, channels) * (size_t) block, 0.0f);
        fadeChannels = std::max (2, channels);

        for (auto* f : { &compFade, &cabFade })
            f->prepare (sampleRate);
        compFade.snapTo (s.compWorks && s.compBlend > 0.0f);
        cabFade.snapTo (s.cabWorks);
        prepared = true;
        return ok && compReady && octaverReady && envelopeReady && chorusReady;
    }

    // ---- the front: the one signal, the bus's channel 0 -------------------------------------------------------------

    /** THE INPUT CUT — the HPF's gentle third across the chain, before every key and every stage. */
    void inputCut (float* mono, int n, const Settings& s) noexcept { innerCut (0, mono, n, s); }

    /** THE GATE: its decision on the signal as it stands here; its mute here when it mutes at the START. */
    void gateAnalyse (float* mono, int n, const Settings& s) noexcept
    {
        float* io[] { mono };
        (void) gate.analyse (io, 1, n, s.gateWorks, s.gateThresholdDb);
        if (s.gateMutesAtStart)
            (void) gate.applyGain (io, 1, n);
    }

    /** ENVELOPE's key, kept before COMP touches the signal: the gated string itself — at the START the copy is taken
        after the mute, at the END the curve the gate has just analysed is laid on the copy. */
    void keepEnvelopeKey (const float* mono, int n, const Settings& s) noexcept
    {
        keyValid = n <= (int) key.size();
        if (! keyValid)
            return;
        std::memcpy (key.data(), mono, sizeof (float) * (size_t) std::max (0, n));
        if (! s.gateMutesAtStart)
        {
            float* k[] { key.data() };
            (void) gate.applyGain (k, 1, n);
        }
    }

    /** COMP, then the cut after it. Its STANDBY is the house's crossfade toward the link working AND its knob up. */
    void compress (float* mono, int n, const Settings& s) noexcept
    {
        const double amount = (double) s.compBlend;
        const auto span = compFade.advance (n, s.compWorks && amount > 0.0);
        const auto attack    = static_cast<basscomp::BassComp::Attack>  (std::clamp (s.compAttack, 0, 2));
        const auto release   = static_cast<basscomp::BassComp::Release> (std::clamp (s.compRelease, 0, 2));
        const auto character = static_cast<basscomp::BassComp::Character> (std::clamp (s.compCharacter, 0, basscomp::BassComp::characterCount - 1));
        comp.process (mono, n, amount, span, attack, release, character, (double) s.compThresholdDb, (double) s.compRatio);
        innerCut (1, mono, n, s);
    }

    /** OCTAVE, then the cut after it: its sub's own DC and thumps go before ENVELOPE and the AMP hear them. */
    void octaveDown (float* mono, int n, const Settings& s) noexcept
    {
        if (octaverReady)
        {
            tellOctave (s);
            float* io[] { mono };
            (void) octaver.process (io, 1, n);
        }
        innerCut (2, mono, n, s);
    }

    /** ENVELOPE, keyed by the key kept above (keepEnvelopeKey). */
    void envelope (float* mono, int n, const Settings& s) noexcept
    {
        if (! envelopeReady)
            return;
        tellEnvelope (s);
        float* io[] { mono };
        const float* k[] { key.data() };
        const bool keyed = keyValid && n <= (int) key.size();
        (void) envFilter.process (io, 1, n, keyed ? k : nullptr, keyed ? 1 : 0);
    }

    // ---- after the AMP ----------------------------------------------------------------------------------------------

    /** The cut after the AMP — after BLEND's sum, never inside one leg — then the gate's mute where it mutes at the END:
        everything the chain added dies there too. */
    void afterAmp (float* mono, int n, const Settings& s) noexcept
    {
        innerCut (3, mono, n, s);
        if (! s.gateMutesAtStart)
        {
            float* io[] { mono };
            (void) gate.applyGain (io, 1, n);
        }
    }

    /** THE SEAM: the one signal copied to the bus's other plane — `planes` the bus's width, at most the prepared. */
    void widen (float* const* io, int planes, int n) const noexcept
    {
        for (int ch = 1; ch < std::min (planes, wide); ++ch)
            std::memcpy (io[ch], io[0], sizeof (float) * (size_t) std::max (0, n));
    }

    /** CHORUS, on the planes the seam made: each its own voice, above the low end only. */
    void chorus (float* const* io, int planes, int n, const Settings& s) noexcept
    {
        if (! chorusReady)
            return;
        tellChorus (s);
        (void) chorusStage.process (io, std::min (planes, wide), n);
    }

    /** CAB, with the DI — the signal the cabinet is handed — mixed back by MIX's share, gliding over 20 ms for the whole
        travel; standing down, a crossfade to what it was handed, its memory drained. */
    void cabinetStage (float* const* io, int planes, int n, const Settings& s) noexcept
    {
        const int  nb    = std::min (planes, wide);
        const bool works = s.cabWorks;
        const auto span  = cabFade.advance (n, works);
        const bool on    = core::BypassFade::runs (span, works);
        const bool fade  = span.moving && canFade (n, nb);

        if (fade)
            for (int ch = 0; ch < nb; ++ch)
                std::memcpy (fadePlane (ch), io[ch], sizeof (float) * (size_t) n);

        const float diWant = std::clamp ((100.0f - s.cabMix) * 0.01f, 0.0f, 1.0f);
        const bool  diMix  = on && (diWant > 0.0f || diNow > 0.0f) && n <= block;
        if (! on)
            diNow = diWant;
        if (diMix)
            for (int ch = 0; ch < nb; ++ch)
                std::memcpy (diPlane (ch), io[ch], sizeof (float) * (size_t) n);

        cab.process (io, nb, n, on);

        if (fade)
        {
            const float* dry[2] { fadePlane (0), fadePlane (1) };
            core::BypassFade::blend (io, dry, nb, n, span);
        }

        if (diMix)
        {
            const bool  gliding = diNow < diWant || diNow > diWant;
            const float step    = diStep;
            float       end     = diNow;
            for (int ch = 0; ch < nb; ++ch)
            {
                float*       out = io[ch];
                const float* di  = diPlane (ch);
                if (gliding)
                {
                    float sh = diNow;
                    for (int i = 0; i < n; ++i)
                    {
                        sh = diWant > sh ? std::min (diWant, sh + step) : std::max (diWant, sh - step);
                        out[i] = (1.0f - sh) * out[i] + sh * di[i];
                    }
                    end = sh;
                }
                else if (diNow >= 1.0f)   // all DI: the signal the cabinet was handed, bit for bit
                    std::memcpy (out, di, sizeof (float) * (size_t) n);
                else if (diNow > 0.0f)
                {
                    VectorOps::multiply (out, 1.0f - diNow, n);
                    VectorOps::addWithMultiply (out, di, diNow, n);
                }
            }
            diNow = end;
        }

        if (! on)
            cab.idle (n);
    }

    /** THE BASS'S CUTS, a stage after DI/CAB: the HPF by the strings, the LPF by the hand. Off, a wire. */
    void bassCuts (float* const* io, int planes, int n, const Settings& s) noexcept
    {
        cuts.process (io, std::min (planes, wide), n, cutSettings (s));
    }

    /** THE WHOLE CHAIN, a block of it, in place: `left` the input (the bus's channel 0) and the left output, `right` the
        right output (null on a one-plane bus). `amp (float* mono, int n)` is the host's AMP — the capture, its BLEND and
        its TONE — on the one signal, in place, between the front and the seam. The echo and the room are the host's;
        this plays the chain without them (the walker's: its strip holds neither). */
    template <typename Amp>
    void process (float* left, float* right, int n, const Settings& s, Amp&& amp) noexcept
    {
        if (! prepared || left == nullptr || n <= 0 || n > block)
            return;
        inputCut (left, n, s);
        gateAnalyse (left, n, s);
        keepEnvelopeKey (left, n, s);
        compress (left, n, s);
        octaveDown (left, n, s);
        envelope (left, n, s);
        amp (left, n);
        afterAmp (left, n, s);
        float* planes[2] { left, right };
        const int nb = right != nullptr ? 2 : 1;
        widen (planes, nb, n);
        chorus (planes, nb, n, s);
        cabinetStage (planes, nb, n, s);
        bassCuts (planes, nb, n, s);
    }

    // ---- what a host reads and loads --------------------------------------------------------------------------------

    /** The cabinet: its producer side (prepare, load, setPost, setReference, flushPending) is the host's — one thread at
        a time, CabinetIr's contract: a host with more than one holds its own lock around them. */
    cabinet::CabinetIr&       cabinet() noexcept       { return cab; }
    const cabinet::CabinetIr& cabinet() const noexcept { return cab; }

    /** COMP's squeeze now, dB (0 or below), and its picture of the last block; the gate's gain now. */
    float compGrDb() const noexcept { return comp.grDb.load (std::memory_order_relaxed); }
    basscomp::BassComp&       compressor() noexcept       { return comp; }
    const dynamics::NoiseGate& noiseGate() const noexcept { return gate; }
    const envfilter::EnvelopeFilter& envelopeFilter() const noexcept { return envFilter; }
    const octave::Octaver&           octaveStage() const noexcept { return octaver; }
    const chorus::BassChorus&        chorusVoice() const noexcept { return chorusStage; }
    int wideness() const noexcept { return wide; }

    /** The cuts' settings the stages play from `s`: the END's, and the inner ones across the chain (a third of the HPF's
        corner — a tenth of a decibel off the lowest string — and no LPF). */
    static eq::BassCuts::Settings cutSettings (const Settings& s) noexcept
    {
        eq::BassCuts::Settings c;
        c.hpfOn = s.hpfOn;
        c.hpfHz = (double) (std::clamp (s.hpf, 0, 1) == 1 ? s.hpfHz4 : s.hpfHz5);
        c.lpfOn = s.lpfOn;
        c.lpfHz = (double) (std::clamp (s.lpf, 0, 1) == 1 ? s.lpfHzSlap : s.lpfHzFingers);
        return c;
    }

    static eq::BassCuts::Settings innerCutSettings (const Settings& s) noexcept
    {
        auto c  = cutSettings (s);
        c.hpfHz /= 3.0;
        c.hpfOn  = c.hpfOn && ! s.innerCutsOut;
        c.lpfOn  = false;
        return c;
    }

private:
    void innerCut (int at, float* mono, int n, const Settings& s) noexcept
    {
        float* io[] { mono };
        innerCuts[(size_t) at].process (io, 1, n, innerCutSettings (s));
    }

    template <typename P>
    static bool sameKnobs (const P& a, const P& b) noexcept { return std::memcmp (&a, &b, sizeof (P)) == 0; }

    void tellOctave (const Settings& s) noexcept
    {
        auto menu = octaveToldValid ? octaveTold : octave::OctaverParams {};
        menu.sub2Top = 0.0f;
        menu.toneHz  = s.octaveToneHz;
        menu.subHpHz = octaveSubHpHz;
        menu.gateDb  = s.gateWorks ? std::clamp (s.gateThresholdDb, -90.0f, -20.0f) : octaveGateIdleDb;
        const float amount = s.octaveWorks ? s.octave : 0.0f;
        const auto  p = octave::paramsForAmount (amount, menu);
        if (! octaveToldValid || ! sameKnobs (p, octaveTold))
        {
            octaver.setParams (p);
            octaveTold = p;
            octaveToldValid = true;
        }
    }

    void tellEnvelope (const Settings& s) noexcept
    {
        auto menu = envelopeToldValid ? envelopeTold : envfilter::EnvelopeFilterParams {};
        const int range = std::clamp (s.envelopeRange, 0, 2);
        menu.attackMs   = envelopeAttackMs;
        menu.releaseMs  = s.envelopeReleaseMs;
        menu.sweepLoHz  = envelopeRangeHz[range][0];
        menu.sweepHiHz  = envelopeRangeHz[range][1];
        menu.direction  = s.envelopeDirection == 1 ? envfilter::Direction::down : envfilter::Direction::up;
        menu.mode       = s.envelopeMode == 1 ? envfilter::Mode::bandPass : envfilter::Mode::lowPass;
        menu.resonance  = s.envelopeResonance;
        menu.keepLows   = s.envelopeKeepLows ? 1.0f : 0.0f;
        menu.keepLowsHz = s.envelopeKeepHz;
        const float amount = s.envelopeWorks ? s.envelope : 0.0f;
        const auto  p = envfilter::paramsForAmount (amount, menu);
        if (! envelopeToldValid || ! sameKnobs (p, envelopeTold))
        {
            envFilter.setParams (p);
            envelopeTold = p;
            envelopeToldValid = true;
        }
    }

    void tellChorus (const Settings& s) noexcept
    {
        auto menu = chorusToldValid ? chorusTold : chorusStage.params();
        menu.rateHz      = s.chorusRateHz;
        menu.crossoverHz = s.chorusCrossoverHz;
        menu.delayMs     = chorusCentreMs;
        const float amount = s.chorusWorks ? s.chorus : 0.0f;
        const auto  p = chorus::paramsForAmount (amount, menu);
        if (! chorusToldValid || ! sameKnobs (p, chorusTold))
        {
            chorusStage.setParams (p);
            chorusTold = p;
            chorusToldValid = true;
        }
    }

    bool canFade (int n, int planes) const noexcept { return n <= block && planes <= fadeChannels; }
    float* fadePlane (int ch) noexcept { return fadeDry.data() + (size_t) ch * (size_t) block; }
    float* diPlane (int ch) noexcept   { return diDry.data() + (size_t) ch * (size_t) block; }

    static_assert (sizeof (octave::OctaverParams) == 7 * sizeof (float), "OctaverParams: 7 floats, no padding");
    static_assert (sizeof (envfilter::EnvelopeFilterParams)
                       == 9 * sizeof (float) + sizeof (envfilter::Direction) + sizeof (envfilter::Mode),
                   "EnvelopeFilterParams: 9 floats and 2 enums, no padding");
    static_assert (sizeof (chorus::BassChorusParams) == 5 * sizeof (float), "BassChorusParams: 5 floats, no padding");

    double rate = 48000.0;
    int    block = 1, wide = 1, fadeChannels = 2;
    bool   prepared = false, compReady = false, octaverReady = false, envelopeReady = false, chorusReady = false;

    dynamics::NoiseGate gate;
    basscomp::BassComp  comp;
    core::BypassFade    compFade, cabFade;
    std::array<eq::BassCuts, 4> innerCuts;
    eq::BassCuts        cuts;
    octave::Octaver           octaver;
    envfilter::EnvelopeFilter envFilter;
    chorus::BassChorus        chorusStage;
    cabinet::CabinetIr        cab;

    octave::OctaverParams           octaveTold {};
    envfilter::EnvelopeFilterParams envelopeTold {};
    chorus::BassChorusParams        chorusTold {};
    bool octaveToldValid = false, envelopeToldValid = false, chorusToldValid = false;

    std::vector<float> key, diDry, fadeDry;
    bool  keyValid = false;
    float diNow = 0.3f, diStep = 0.001f;
};

/** The chain with the plain vector passes — what a host without a vector library of its own plays. */
using BassChain = BassChainT<>;

} // namespace felitronics::bass
