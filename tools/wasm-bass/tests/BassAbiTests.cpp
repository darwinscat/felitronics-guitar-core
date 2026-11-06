// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// fc_bass — the ABI against the chain driven directly: felitronics::bass::BassChain around a RigPlayer and an
// eq::EqLink, wired as the plug-in wires them (the tone told at the top of every block, BLEND told as 1 - BLEND to the
// player's crossover) — bit for bit, both planes, every block, through knobs moved by the app's ids, the gain dial, a
// cabinet loaded and another, BLEND inverted and the cabinet out; the contract; no allocation in process().

#include "fc_bass_abi.h"
#include "fc_rig_abi.h"
#include "tests/rig_fixture.h"

#include <alloc_counter.h>
#include <felitronics/bass/BassChain.h>
#include <felitronics/eq/EqLink.h>
#include <felitronics/rigplayer/RigPlayer.h>
#include <felitronics_test.h>
#include <namz_rig_load.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace test = felitronics::test;
using felitronics::rigplayer::RigPlayer;
using test::ok;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 128;

float sample (long n)
{
    const double t = (double) n / kFs;
    const double note = std::fmod (t, 0.5);
    return (float) (0.3 * std::exp (-4.0 * note) * (std::sin (2.0 * 3.141592653589793 * 55.0 * t)
                                                     + 0.4 * std::sin (2.0 * 3.141592653589793 * 165.0 * t)));
}

std::vector<float> shot (double decay, double hz)
{
    std::vector<float> ir (4800);
    for (size_t i = 0; i < ir.size(); ++i)
        ir[i] = (float) (std::exp (-(double) i / kFs * decay) * std::cos (2.0 * 3.141592653589793 * hz * (double) i / kFs)
                         + (i == 0 ? 0.5 : 0.0));
    return ir;
}

/** The chain the plug-in plays, driven directly — what fc_bass is held to. */
struct Direct
{
    RigPlayer p;
    felitronics::bass::BassChain chain;
    felitronics::bass::Settings s;
    felitronics::eq::EqLink tone;
    felitronics::eq::EqLink::Settings console;
    std::map<std::string, std::vector<std::byte>> files;
    double blendWet = 1.0, blendHz = 150.0;
    bool blendInvert = false;

    Direct() { console.b1Hz = 400.0; }

    void tellBlend()
    {
        test::run (p.setCrossoverAmount (1.0 - blendWet));
        test::run (p.setCrossoverHz (blendHz));
        p.setCrossoverInvert (blendInvert);
    }

    bool load (const std::string& manifest)
    {
        bool okParse = false;
        const auto rig = namz::rig::loadRigManifest (manifest, &okParse);
        return okParse && p.load (rig, [this] (const std::string& id) {
            const auto it = files.find (id);
            return it != files.end() ? it->second : std::vector<std::byte> {};
        });
    }

    void pump()
    {
        p.service();
        if (auto job = p.takeLoadJob()) p.deliver (RigPlayer::run (std::move (*job)));
    }

    void process (float* l, float* r, int n)
    {
        tone.setSettings (felitronics::bass::toneSettings (console));
        chain.process (l, r, n, s, [this] (float* mono, int m) {
            float* io[] { mono };
            (void) p.process (io, 1, m);
            tone.process (io, 1, m);
        });
        chain.cabinet().flushPending();
    }
};

/** The same through the ABI. */
struct Abi
{
    fc_bass* b = fc_bass_create();
    std::map<std::string, std::vector<std::byte>> files;
    ~Abi() { fc_bass_destroy (b); }

    void pump()
    {
        fc_rig* r = fc_bass_rig (b);
        if (fc_rig_take_job (r) != 1) return;
        const std::string id (fc_rig_job_file (r), (std::size_t) fc_rig_job_file_length (r));
        if (fc_rig_job_needs_bytes (r) == 1)
        {
            const auto it = files.find (id);
            if (it != files.end())
                std::memcpy (fc_rig_job_bytes (r, (int) it->second.size()), it->second.data(), it->second.size());
        }
        fc_rig_job_run (r);
        fc_rig_job_deliver (r);
    }

    void cab (const std::vector<float>& ir)
    {
        float* t = fc_bass_cab_taps (b, (int) ir.size());
        std::memcpy (t, ir.data(), sizeof (float) * ir.size());
        test::run (fc_bass_cab_load (b, (int) ir.size(), kFs) == 1);
    }
};

struct Move { int block; std::string id; double value; };

/** A knob by the app's id, on the direct side as fc_bass reads it. */
void setDirect (Direct& d, const std::string& id, double v)
{
    auto& s = d.s;
    if (id == "comp") s.compBlend = (float) v;
    else if (id == "comp_threshold") s.compThresholdDb = (float) v;
    else if (id == "comp_character") s.compCharacter = (int) v;
    else if (id == "octave") s.octave = (float) v;
    else if (id == "octave_present") s.octaveWorks = v > 0.5;
    else if (id == "filter") s.envelope = (float) v;
    else if (id == "filter_mode") s.envelopeMode = (int) v;
    else if (id == "chorus") s.chorus = (float) v;
    else if (id == "cab_mix") s.cabMix = (float) v;
    else if (id == "cab_on") s.cabWorks = v > 0.5;
    else if (id == "bass_hpf") s.hpf = (int) v;
    else if (id == "bass_lpf_on") s.lpfOn = v > 0.5;
    else if (id == "gate_on") s.gateWorks = v > 0.5;
    else if (id == "gate_threshold") s.gateThresholdDb = (float) v;
    else if (id == "gate_pos") s.gateMutesAtStart = (int) v == 0;
    else if (id == "preamp_eq_lo_db") d.console.loDb = (double) (float) v;
    else if (id == "preamp_eq_b1_db") d.console.b1Db = (double) (float) v;
    else if (id == "preamp_eq_b1_hz") d.console.b1Hz = (double) (float) v;
    else if (id == "preamp_eq_hi_db") d.console.hiDb = (double) (float) v;
    else if (id == "amp_blend_wet") { d.blendWet = (double) (float) v; d.tellBlend(); }
    else if (id == "amp_blend_hz") { d.blendHz = (double) (float) v; d.tellBlend(); }
    else if (id == "amp_blend_invert") { d.blendInvert = v > 0.5; d.tellBlend(); }
    else std::printf ("    (the direct side does not know %s)\n", id.c_str());
}

void bitIdentity()
{
    test::group ("the ABI plays the chain bit for bit as BassChain, RigPlayer and EqLink driven directly");
    const auto fixture = fc_rig_fixture::pack();
    Direct d;
    Abi a;
    for (const auto& [id, bytes] : fixture.files) { d.files[id] = bytes; a.files[id] = bytes; }

    test::run (d.p.prepare (kFs, kBlock, 1));
    test::run (d.chain.prepare (kFs, kBlock, 2, d.s));
    d.tone.prepare (kFs, 2);
    d.tellBlend();
    test::run (fc_bass_prepare (a.b, kFs, kBlock) == 1);
    test::run (d.load (fixture.manifest));
    test::run (fc_rig_load (fc_bass_rig (a.b), fixture.manifest.c_str(), (int) fixture.manifest.size()) == 1);
    const auto ir1 = shot (60.0, 90.0), ir2 = shot (30.0, 140.0);
    test::run (d.chain.cabinet().load (ir1.data(), (int) ir1.size(), kFs));
    d.chain.cabinet().flushPending();
    a.cab (ir1);

    const std::vector<Move> moves {
        { 0, "comp", 1.0 }, { 0, "comp_threshold", -30.0 }, { 0, "octave", 0.5 }, { 0, "filter", 0.6 }, { 0, "chorus", 0.5 },
        { 0, "cab_mix", 60.0 }, { 0, "bass_hpf", 1.0 }, { 0, "bass_lpf_on", 1.0 }, { 0, "preamp_eq_lo_db", 3.0 },
        { 0, "preamp_eq_b1_db", -2.0 }, { 0, "preamp_eq_b1_hz", 700.0 }, { 0, "preamp_eq_hi_db", 2.0 }, { 0, "gate_on", 1.0 },
        { 0, "gate_threshold", -60.0 }, { 0, "amp_blend_wet", 0.6 }, { 0, "amp_blend_hz", 120.0 },
        { 200, "comp_character", 4.0 }, { 200, "chorus", 0.0 }, { 220, "octave_present", 0.0 },
        { 400, "gate_pos", 0.0 }, { 400, "filter_mode", 1.0 },
        { 500, "amp_blend_invert", 1.0 }, { 560, "cab_on", 0.0 }, { 620, "cab_on", 1.0 },
    };
    const int blocks = 760;
    std::vector<float> dl (kBlock), dr (kBlock);
    float* al = fc_bass_buffer (a.b, 0);
    float* ar = fc_bass_buffer (a.b, 1);
    int differ = 0, first = -1;
    double energy = 0.0, lr = 0.0;
    std::size_t next = 0;
    for (int blk = 0; blk < blocks; ++blk)
    {
        for (; next < moves.size() && moves[next].block <= blk; ++next)
        {
            setDirect (d, moves[next].id, moves[next].value);
            test::run (fc_bass_set (a.b, moves[next].id.c_str(), moves[next].value) == 1);
        }
        if (blk == 100)
        {
            test::run (d.p.setDial (d.p.dialName(), 150.0));
            test::run (fc_rig_set_gain_dial (fc_bass_rig (a.b), 150.0) == 1);
        }
        if (blk == 300)
        {
            test::run (d.chain.cabinet().load (ir2.data(), (int) ir2.size(), kFs));
            d.chain.cabinet().flushPending();
            a.cab (ir2);
        }
        for (int i = 0; i < kBlock; ++i)
            al[i] = dl[(std::size_t) i] = sample ((long) blk * kBlock + i);
        d.process (dl.data(), dr.data(), kBlock);
        test::run (fc_bass_process (a.b, al, ar, kBlock) == 1);
        const bool same = std::memcmp (al, dl.data(), sizeof (float) * kBlock) == 0
                       && std::memcmp (ar, dr.data(), sizeof (float) * kBlock) == 0;
        if (! same) { ++differ; if (first < 0) first = blk; }
        for (int i = 0; i < kBlock; ++i)
        {
            energy += (double) dl[(std::size_t) i] * dl[(std::size_t) i];
            lr = std::max (lr, (double) std::abs (dl[(std::size_t) i] - dr[(std::size_t) i]));
        }
        d.pump();
        a.pump();
    }
    std::printf ("    %d of %d blocks differ (first %d)\n", differ, blocks, first);
    ok (differ == 0, "identical through the knobs by the app's ids, the gain dial, BLEND, two cabinets, the gate's two places, "
                     "a character switch, an effect leaving the strip and the cabinet out and back");
    ok (energy > 1.0 && lr > 1.0e-3, "…and it is the chain playing, two planes of it, not silence");
    ok (fc_bass_comp_gr_db (a.b) <= 0.0f && std::abs (fc_bass_comp_gr_db (a.b) - d.chain.compGrDb()) < 1.0e-9f,
        "the squeeze read-out is the chain's");
}

void contract()
{
    test::group ("the contract");
    ok (fc_bass_abi_version() == FC_BASS_ABI_VERSION && FC_BASS_ABI_VERSION == 1u, "abi_version answers the header's 1");
    ok (fc_bass_prepare (nullptr, kFs, kBlock) == 0 && fc_bass_buffer (nullptr, 0) == nullptr
            && fc_bass_process (nullptr, nullptr, nullptr, 1) == 0 && fc_bass_rig (nullptr) == nullptr
            && fc_bass_set (nullptr, "comp", 1.0) == 0 && fc_bass_cab_taps (nullptr, 10) == nullptr
            && fc_bass_cab_load (nullptr, 10, kFs) == 0 && fc_bass_comp_gr_db (nullptr) == 0.0f
            && fc_bass_latency_samples (nullptr) == 0,
        "a NULL instance: refused, nothing read");
    fc_bass_reset (nullptr);
    fc_bass_destroy (nullptr);

    fc_bass* b = fc_bass_create();
    ok (b != nullptr && fc_bass_rig (b) != nullptr && fc_rig_abi_version() == 6u, "an instance, and the player inside it (fc_rig ABI 6)");
    std::vector<float> l (kBlock, 0.1f), r (kBlock);
    ok (fc_bass_process (b, l.data(), r.data(), kBlock) == 0 && fc_bass_buffer (b, 0) == nullptr, "unprepared: process refuses, no buffer");
    ok (fc_bass_prepare (b, 500.0, kBlock) == 0 && fc_bass_prepare (b, kFs, 0) == 0 && fc_bass_prepare (b, kFs, 70000) == 0,
        "a rate or a block out of range is refused");
    ok (fc_bass_prepare (b, kFs, kBlock) == 1 && fc_bass_buffer (b, 0) != nullptr && fc_bass_buffer (b, 1) != nullptr
            && fc_bass_buffer (b, 2) == nullptr, "prepared: two planes");
    ok (fc_bass_process (b, l.data(), r.data(), kBlock + 1) == 0 && fc_bass_process (b, nullptr, r.data(), kBlock) == 0
            && fc_bass_process (b, l.data(), r.data(), -1) == 0 && fc_bass_process (b, l.data(), r.data(), 0) == 1
            && fc_bass_process (b, l.data(), nullptr, kBlock) == 1,
        "past the block, no left or n < 0 refused; n 0 a no-op; one plane taken");
    ok (fc_bass_set (b, "comp", std::numeric_limits<double>::quiet_NaN()) == 0 && fc_bass_set (b, "no_such_knob", 1.0) == 0
            && fc_bass_set (b, nullptr, 1.0) == 0 && fc_bass_set (b, "comp_threshold", -1000.0) == 1,
        "a non-finite value, an unknown id or none refused; a value out of range taken, clamped");
    ok (fc_bass_cab_taps (b, 0) == nullptr && fc_bass_cab_taps (b, 5 * 768000 + 1) == nullptr, "taps: out of range, none");
    float* t = fc_bass_cab_taps (b, 480);
    std::fill (t, t + 480, 0.0f);
    t[0] = 1.0f;
    ok (fc_bass_cab_load (b, 480, 4000.0) == 0 && fc_bass_cab_load (b, 481, kFs) == 0 && fc_bass_cab_load (b, 480, kFs) == 1,
        "a cabinet at a rate CabinetIr refuses, or longer than its taps, is refused; one it takes, taken");
    fc_bass_reset (b);
    ok (fc_bass_process (b, l.data(), r.data(), kBlock) == 1, "a reset plays on");
    ok (fc_bass_prepare (b, 1.0e6, kBlock) == 0 && fc_bass_process (b, l.data(), r.data(), kBlock) == 0,
        "a refused prepare disarms");
    fc_bass_destroy (b);
}

void noAllocation()
{
    test::group ("RT: process allocates nothing — every stage at work, the player's two models");
    const auto fixture = fc_rig_fixture::pack();
    Abi a;
    for (const auto& [id, bytes] : fixture.files) a.files[id] = bytes;
    test::run (fc_bass_prepare (a.b, kFs, kBlock) == 1);
    test::run (fc_rig_load (fc_bass_rig (a.b), fixture.manifest.c_str(), (int) fixture.manifest.size()) == 1);
    a.cab (shot (60.0, 90.0));
    for (const auto* id : { "octave", "filter", "chorus", "comp" })
        test::run (fc_bass_set (a.b, id, 0.6) == 1);
    test::run (fc_bass_set (a.b, "amp_blend_wet", 0.5) == 1);
    test::run (fc_rig_set_gain_dial (fc_bass_rig (a.b), 100.0) == 1);
    float* l = fc_bass_buffer (a.b, 0);
    float* r = fc_bass_buffer (a.b, 1);
    for (int blk = 0; blk < 400; ++blk)   // the captures landed and warm, the cabinet's crossfade done
    {
        for (int i = 0; i < kBlock; ++i) l[i] = sample ((long) blk * kBlock + i);
        test::run (fc_bass_process (a.b, l, r, kBlock) == 1);
        a.pump();
    }
    const long long before = alloc::count.load();
    for (int blk = 400; blk < 600; ++blk)
    {
        for (int i = 0; i < kBlock; ++i) l[i] = sample ((long) blk * kBlock + i);
        test::run (fc_bass_process (a.b, l, r, kBlock) == 1);
    }
    test::okNoAlloc (alloc::count.load() == before, "200 blocks");
}
} // namespace

int main()
{
    std::printf ("felitronics_bass_abi_tests\n");
    contract();
    bitIdentity();
    noAllocation();
    return test::report();
}
