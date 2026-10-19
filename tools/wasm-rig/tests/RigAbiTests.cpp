// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// fc_rig against RigPlayer driven directly, BIT FOR BIT: the same pack, the same knobs at the same blocks, the same
// input — one side through the C ABI (the job carried across the boundary: taken, its bytes handed in, run,
// delivered), the other the player as the app drives it (service, takeLoadJob, run, deliver with a ModelSource).
// Then the job protocol's own facts, the contract, and no allocation in process().

#include <alloc_counter.h>   // installs the allocation counter
#include <felitronics_test.h>

#include "fc_rig_abi.h"
#include "rig_fixture.h"

#include <felitronics/rigplayer/RigPlayer.h>
#include <namz_rig_load.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

using felitronics::rigplayer::RigPlayer;
using felitronics::test::group;
using felitronics::test::ok;
namespace test = felitronics::test;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 128;
constexpr double kPi = 3.14159265358979323846;

using Files = std::map<std::string, std::vector<std::byte>>;
using Counts = std::map<std::string, int>;

/** A hand that fails: the file's next `n` fetches answer nothing (a network hiccup), then its bytes again. */
bool refused (Counts& refuse, const std::string& id)
{
    const auto it = refuse.find (id);
    if (it == refuse.end() || it->second == 0) return false;
    --it->second;
    return true;
}

/** Plucks, different per channel, and a noise burst: a guitar-ish DI at about -6 dBFS. */
float sample (int i, int ch)
{
    const double t = i / kFs, pluck = std::fmod (t, 0.45);
    const double env = std::exp (-pluck * 6.0) * (pluck < 0.002 ? pluck / 0.002 : 1.0);
    const double f = ch == 0 ? 110.0 : 146.83;
    double v = 0.5 * env * (std::sin (2.0 * kPi * f * t) + 0.35 * std::sin (2.0 * kPi * 3.0 * f * t));
    if (t > 0.9 && t < 0.95)
    {
        const unsigned h = (unsigned) i * 2654435761u + (unsigned) ch * 40503u;
        v += 0.15 * ((double) (h >> 8) / 16777216.0 - 0.5);
    }
    return (float) v;
}

enum class Kind { Gain, Dial, Switch, Max, Normalize, Trims, Shape, Reset, Reload, Unload };
struct Event { int block; Kind kind; std::string name, value; double a = 0.0, b = 0.0; };

/** The player as the app drives it: a ModelSource over the files, one job per pump, run and delivered in place. */
struct Direct
{
    RigPlayer p;
    Files files;
    Counts refuse;
    long long fetched = 0, jobs = 0;

    bool load (const std::string& manifest)
    {
        bool okParse = false;
        const auto rig = namz::rig::loadRigManifest (manifest, &okParse);
        if (! okParse) { p.unload(); return false; }
        return p.load (rig, [this] (const std::string& id) {
            ++fetched;
            const auto it = files.find (id);
            return it == files.end() || refused (refuse, id) ? std::vector<std::byte> {} : it->second;
        });
    }
    void pump()
    {
        p.service();
        if (auto job = p.takeLoadJob()) { ++jobs; p.deliver (RigPlayer::run (std::move (*job))); }
    }
};

/** The same through the ABI: the job taken, its file's bytes copied in when it needs them, run, delivered. */
struct Abi
{
    fc_rig* r = fc_rig_create();
    Files files;
    Counts refuse, asked, landed;   // the hand's failures; per file: the bytes asked for, a model landed
    long long handed = 0, jobs = 0, failed = 0;

    ~Abi() { fc_rig_destroy (r); }
    void pump()
    {
        if (fc_rig_take_job (r) != 1) return;
        ++jobs;
        const std::string id (fc_rig_job_file (r), (std::size_t) fc_rig_job_file_length (r));
        if (fc_rig_job_needs_bytes (r) == 1)
        {
            ++asked[id];
            const auto it = files.find (id);
            if (it != files.end() && ! refused (refuse, id))
            {
                std::memcpy (fc_rig_job_bytes (r, (int) it->second.size()), it->second.data(), it->second.size());
                ++handed;
            }
        }
        fc_rig_job_run (r);
        const int delivered = fc_rig_job_deliver (r);
        if (delivered == 2) ++failed;
        if (delivered == 1) ++landed[id];
    }
};

void applyDirect (Direct& d, const Event& e, const std::string& manifest)
{
    switch (e.kind)
    {
        case Kind::Gain:      (void) d.p.setDial (d.p.dialName(), e.a); break;
        case Kind::Dial:      (void) d.p.setDial (e.name, e.a); break;
        case Kind::Switch:    (void) d.p.setSwitch (e.name, e.value); break;
        case Kind::Max:       (void) d.p.setMaxModels ((int) e.a); break;
        case Kind::Normalize: d.p.setNormalize (e.a != 0.0); break;
        case Kind::Trims:     d.p.setInputTrims (e.a != 0.0); break;
        case Kind::Shape:     d.p.setBlendShape ({ e.a, e.b }); break;
        case Kind::Reset:     d.p.reset(); break;
        case Kind::Reload:    (void) d.load (manifest); break;
        case Kind::Unload:    d.p.unload(); break;
    }
}

void applyAbi (Abi& a, const Event& e, const std::string& manifest)
{
    switch (e.kind)
    {
        case Kind::Gain:      (void) fc_rig_set_gain_dial (a.r, e.a); break;
        case Kind::Dial:      (void) fc_rig_set_dial (a.r, e.name.c_str(), e.a); break;
        case Kind::Switch:    (void) fc_rig_set_switch (a.r, e.name.c_str(), e.value.c_str()); break;
        case Kind::Max:       (void) fc_rig_set_max_models (a.r, (int) e.a); break;
        case Kind::Normalize: fc_rig_set_normalize (a.r, e.a != 0.0); break;
        case Kind::Trims:     fc_rig_set_input_trims (a.r, e.a != 0.0); break;
        case Kind::Shape:     fc_rig_set_blend_shape (a.r, e.a, e.b); break;
        case Kind::Reset:     fc_rig_reset (a.r); break;
        case Kind::Reload:    (void) fc_rig_load (a.r, manifest.c_str(), (int) manifest.size()); break;
        case Kind::Unload:    fc_rig_unload (a.r); break;
    }
}

struct Outcome
{
    int  differBlocks = 0, firstDiffer = -1;
    double energy = 0.0, moved = 0.0;
    long long handed = 0, abiJobs = 0, fetched = 0, directJobs = 0, failed = 0;
    int runs = 0, pairs = 0;
    Counts asked, landed;
};

/** `blocks` blocks of the input through both sides, the events applied before their block, pumped after each. */
Outcome compare (const Files& files, int channels, int maxBeforeLoad, const std::vector<Event>& events, int blocks,
                 const Counts& refuse = {})
{
    const auto fixture = fc_rig_fixture::pack();
    Direct d; d.files = files; d.refuse = refuse;
    Abi a; a.files = files; a.refuse = refuse;
    test::run (d.p.prepare (kFs, kBlock, channels));
    test::run (fc_rig_prepare (a.r, kFs, kBlock, channels) == 1);
    if (maxBeforeLoad != 0)
    {
        test::run (d.p.setMaxModels (maxBeforeLoad));
        test::run (fc_rig_set_max_models (a.r, maxBeforeLoad) == 1);
    }
    test::run (d.load (fixture.manifest));
    test::run (fc_rig_load (a.r, fixture.manifest.c_str(), (int) fixture.manifest.size()) == 1);

    Outcome o;
    std::vector<float> dio[2] { std::vector<float> (kBlock), std::vector<float> (kBlock) };
    std::size_t next = 0;
    for (int b = 0; b < blocks; ++b)
    {
        while (next < events.size() && events[next].block <= b)
        {
            applyDirect (d, events[next], fixture.manifest);
            applyAbi (a, events[next], fixture.manifest);
            ++next;
        }
        float* aio[2] { fc_rig_buffer (a.r, 0), channels > 1 ? fc_rig_buffer (a.r, 1) : nullptr };
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < kBlock; ++i)
                aio[c][i] = dio[c][(std::size_t) i] = sample (b * kBlock + i, c);
        float* dp[2] { dio[0].data(), dio[1].data() };
        test::run (d.p.process (dp, channels, kBlock));
        test::run (fc_rig_process (a.r, aio[0], aio[1], kBlock) == 1);
        bool same = true;
        for (int c = 0; c < channels; ++c)
        {
            same = same && std::memcmp (aio[c], dio[c].data(), sizeof (float) * kBlock) == 0;
            for (int i = 0; i < kBlock; ++i)
            {
                o.energy += (double) dio[c][(std::size_t) i] * dio[c][(std::size_t) i];
                o.moved = std::max (o.moved, (double) std::abs (dio[c][(std::size_t) i] - sample (b * kBlock + i, c)));
            }
        }
        if (! same) { ++o.differBlocks; if (o.firstDiffer < 0) o.firstDiffer = b; }
        d.pump();
        a.pump();
    }
    o.handed = a.handed; o.abiJobs = a.jobs; o.failed = a.failed; o.fetched = d.fetched; o.directJobs = d.jobs;
    o.runs = fc_rig_run_blocks (a.r, 0) + fc_rig_run_blocks (a.r, 1);
    o.pairs = fc_rig_pair_blocks (a.r);
    o.asked = a.asked; o.landed = a.landed;
    return o;
}

std::vector<Event> theHand()
{
    return {
        { 100, Kind::Gain, "", "", 0.0 },             // the link: the 60 capture fed 6 dB softer…
        { 130, Kind::Trims, "", "", 0.0 },            // …and not, with the files' input trims off
        { 160, Kind::Trims, "", "", 1.0 },
        { 180, Kind::Gain, "", "", 100.0 },           // between 60 and 150: a crossfade, two models
        { 260, Kind::Gain, "", "", 150.0 },
        { 330, Kind::Gain, "", "", 200.0 },
        { 400, Kind::Gain, "", "", 300.0 },           // past the top capture: the extension
        { 470, Kind::Switch, "Channel", "red" },
        { 540, Kind::Switch, "Channel", "green" },
        { 600, Kind::Dial, "Treble", "", 280.0 },
        { 640, Kind::Dial, "Bass", "", 300.0 },
        { 680, Kind::Dial, "Mix", "", 150.0 },
        { 720, Kind::Gain, "", "", 60.0 },            // back to a file already fetched: no bytes handed in again
        { 800, Kind::Normalize, "", "", 0.0 },
        { 840, Kind::Trims, "", "", 0.0 },
        { 880, Kind::Shape, "", "", 0.5, 0.0 },
        { 900, Kind::Gain, "", "", 120.0 },
        { 960, Kind::Reset },
        { 1000, Kind::Normalize, "", "", 1.0 },
        { 1010, Kind::Trims, "", "", 1.0 },
        { 1020, Kind::Shape, "", "", 0.5, 1.0 },
        { 1040, Kind::Gain, "", "", 190.0 },
    };
}

void report (const char* what, const Outcome& o, int blocks)
{
    std::printf ("    %s: %d of %d blocks differ (first %d); %lld jobs, %lld files handed in, %lld failed; "
                 "networks ran %d blocks, both %d\n",
                 what, o.differBlocks, blocks, o.firstDiffer, o.abiJobs, o.handed, o.failed, o.runs, o.pairs);
}

void bitIdentity()
{
    group ("the ABI plays the pack bit for bit as RigPlayer driven directly");
    const auto files = fc_rig_fixture::pack().files;
    const int blocks = 1200;

    const auto two = compare (files, 1, 0, theHand(), blocks);
    report ("two models, mono", two, blocks);
    ok (two.differBlocks == 0, "two models, mono: identical through the sweep, the link, a channel, tone, blend, "
                               "normalise and trims off and on, a blend shape, a reset");
    ok (two.energy > 1.0 && two.moved > 0.05, "…and it is the pack playing, not silence or the input");
    ok (two.pairs > 0, "…and two networks did run between two captures");
    ok (two.abiJobs == two.directJobs && two.handed == two.fetched,
        "the same jobs on both sides, and the bytes handed in exactly where the source was asked");

    auto handOne = theHand();
    handOne.push_back ({ 1080, Kind::Max, "", "", 2.0 });
    handOne.push_back ({ 1140, Kind::Max, "", "", 1.0 });
    const auto one = compare (files, 1, 1, handOne, blocks);
    report ("one model, mono", one, blocks);
    ok (one.differBlocks == 0, "one model (setMaxModels(1) before the load), mono: identical — the dips, 1 -> 2 -> 1");
    ok (one.energy > 1.0, "…and it plays");

    const auto stereo = compare (files, 2, 0, theHand(), blocks);
    report ("two models, stereo", stereo, blocks);
    ok (stereo.differBlocks == 0, "two models, stereo: identical");

    const std::vector<Event> reloads { { 150, Kind::Gain, "", "", 150.0 }, { 300, Kind::Reload },
                                       { 420, Kind::Unload }, { 500, Kind::Reload }, { 520, Kind::Gain, "", "", 240.0 } };
    const auto reload = compare (files, 1, 0, reloads, 700);
    report ("reload, unload, reload", reload, 700);
    ok (reload.differBlocks == 0 && reload.energy > 1.0, "a pack loaded again, unloaded, loaded again: identical, playing");
}

void jobProtocol()
{
    group ("the load job across the boundary");
    auto files = fc_rig_fixture::pack().files;
    const auto o = compare (files, 1, 0, theHand(), 1200);
    ok (o.handed == 4, "every distinct file is handed in once (" + std::to_string (o.handed) + ")");
    ok (o.abiJobs > o.handed, "a capture visited again runs from the bytes the player kept: "
                              + std::to_string (o.abiJobs) + " jobs, " + std::to_string (o.handed) + " handed in");

    // A file the host cannot fetch, and one that is not a capture: run without bytes, run on garbage. Both come back
    // as failures, the law hears it, and the ABI still matches the player whose source answers the same.
    files.erase ("g150.namz");
    const std::string junk = "{ this is not a capture";
    files["g240.nam"] = fc_rig_fixture::bytesOf (junk);
    const std::vector<Event> visits { { 50, Kind::Gain, "", "", 150.0 }, { 200, Kind::Gain, "", "", 240.0 },
                                      { 350, Kind::Gain, "", "", 60.0 } };
    const auto bad = compare (files, 1, 0, visits, 500);
    report ("a missing file and a garbage one", bad, 500);
    ok (bad.failed >= 2, "a job with no bytes and a job on garbage are delivered as failures ("
                         + std::to_string (bad.failed) + ")");
    ok (bad.differBlocks == 0, "…and the ABI still plays bit for bit as the direct player");

    // A fetch that fails ONCE — a hiccup, not a missing file: the host runs the job without bytes. The next visit must
    // ask again, not run on an empty answer the player kept: 150 at the start (refused), 240, 60 (150 is its warm
    // neighbour: asked again), 150.
    const std::vector<Event> revisits { { 50, Kind::Gain, "", "", 150.0 }, { 200, Kind::Gain, "", "", 240.0 },
                                        { 350, Kind::Gain, "", "", 60.0 }, { 500, Kind::Gain, "", "", 150.0 } };
    auto hiccup = compare (fc_rig_fixture::pack().files, 1, 0, revisits, 650, { { "g150.namz", 1 } });
    report ("g150.namz refused once", hiccup, 650);
    ok (hiccup.asked["g150.namz"] == 2 && hiccup.landed["g150.namz"] >= 1,
        "a file whose fetch failed once is asked for again on a later visit, and lands (asked "
            + std::to_string (hiccup.asked["g150.namz"]) + ", landed " + std::to_string (hiccup.landed["g150.namz"]) + ")");
    ok (hiccup.differBlocks == 0 && hiccup.handed == hiccup.fetched - 1,
        "…bit for bit as the direct player, whose source was asked as often (one answer of it empty)");

    group ("a job out when the pack is loaded again is dropped by the instance, and jobs are handed out again");
    {
        const auto pack = fc_rig_fixture::pack();
        Abi a; a.files = pack.files;
        test::run (fc_rig_prepare (a.r, kFs, kBlock, 1) == 1);
        test::run (fc_rig_load (a.r, pack.manifest.c_str(), (int) pack.manifest.size()) == 1);
        float* l = fc_rig_buffer (a.r, 0);
        test::run (fc_rig_process (a.r, l, nullptr, kBlock) == 1);
        ok (fc_rig_take_job (a.r) == 1 && fc_rig_job_file (a.r) != nullptr, "a job is out");
        ok (fc_rig_take_job (a.r) == 0, "…and only one: a second take answers 0 while it is out");
        test::run (fc_rig_load (a.r, pack.manifest.c_str(), (int) pack.manifest.size()) == 1);
        ok (fc_rig_job_file (a.r) == nullptr && fc_rig_job_needs_bytes (a.r) == 0 && fc_rig_job_bytes (a.r, 10) == nullptr
                && fc_rig_job_run (a.r) == -1 && fc_rig_job_deliver (a.r) == 0,
            "the load dropped it: no file, no buffer, nothing to run or deliver");
        double energy = 0.0;
        for (int b = 0; b < 200; ++b)
        {
            for (int i = 0; i < kBlock; ++i) l[i] = sample (b * kBlock + i, 0);
            test::run (fc_rig_process (a.r, l, nullptr, kBlock) == 1);
            for (int i = 0; i < kBlock; ++i) energy += (double) l[i] * l[i];
            a.pump();
        }
        ok (a.jobs >= 1 && fc_rig_held (a.r, 0) + fc_rig_held (a.r, 1) >= 1 && energy > 0.1,
            "the new pack's capture was asked for, loaded and plays (" + std::to_string (a.jobs) + " jobs, energy "
                + std::to_string (energy) + ")");
    }
}

void contract()
{
    group ("the contract");
    ok (fc_rig_abi_version() == FC_RIG_ABI_VERSION && FC_RIG_ABI_VERSION == 1u, "abi_version answers the header's 1");
    fc_rig* r = fc_rig_create();
    float x[4] {};
    ok (fc_rig_process (r, x, nullptr, 4) == 0, "process before prepare is refused");
    ok (fc_rig_prepare (r, 999.0, 128, 1) == 0 && fc_rig_prepare (r, 768001.0, 128, 1) == 0
            && fc_rig_prepare (r, std::numeric_limits<double>::quiet_NaN(), 128, 1) == 0
            && fc_rig_prepare (r, 48000.0, 0, 1) == 0 && fc_rig_prepare (r, 48000.0, 65537, 1) == 0
            && fc_rig_prepare (r, 48000.0, 128, 0) == 0 && fc_rig_prepare (r, 48000.0, 128, 3) == 0,
        "prepare refuses a rate outside 1 kHz … 768 kHz, a block outside 1 … 65536, channels other than 1 or 2");
    ok (fc_rig_prepare (r, 48000.0, 128, 1) == 1, "prepare(48 kHz, 128, mono) is accepted");
    ok (fc_rig_buffer (r, 0) != nullptr && fc_rig_buffer (r, 1) == nullptr && fc_rig_buffer (r, -1) == nullptr,
        "a mono instance has one buffer");
    ok (fc_rig_process (r, x, x, 4) == 0, "a right channel on a mono instance is refused");
    ok (fc_rig_process (r, nullptr, nullptr, 4) == 0 && fc_rig_process (r, x, nullptr, -1) == 0,
        "a NULL left channel and a negative n are refused");
    ok (fc_rig_process (r, x, nullptr, 0) == 1, "n == 0 is a no-op that answers 1");
    ok (fc_rig_prepare (r, 500.0, 128, 1) == 0 && fc_rig_process (r, x, nullptr, 4) == 0 && fc_rig_buffer (r, 0) == nullptr,
        "a refused prepare disarms: process refuses and the buffer is gone");
    test::run (fc_rig_prepare (r, 48000.0, 128, 2) == 1);

    ok (fc_rig_take_job (r) == 0 && fc_rig_job_file (r) == nullptr && fc_rig_job_file_length (r) == 0
            && fc_rig_job_needs_bytes (r) == 0 && fc_rig_job_bytes (r, 16) == nullptr
            && fc_rig_job_run (r) == -1 && fc_rig_job_deliver (r) == 0,
        "no pack: no job, and every job call says so");
    const std::string garbage = "{\"format\":\"not-a-rig\"}";
    ok (fc_rig_load (r, garbage.c_str(), (int) garbage.size()) == 0 && fc_rig_load (r, nullptr, 10) == 0
            && fc_rig_load (r, garbage.c_str(), 0) == 0,
        "a manifest that is not a rig, a NULL one and an empty one are refused");
    ok (fc_rig_set_gain_dial (r, 150.0) == 0 && fc_rig_gain_dial_sweep (r) == 0.0, "no pack: no gain dial");

    const auto pack = fc_rig_fixture::pack();
    ok (fc_rig_load (r, pack.manifest.c_str(), (int) pack.manifest.size()) == 1, "the fixture pack loads");
    ok (fc_rig_gain_dial_sweep (r) == 300.0 && fc_rig_set_gain_dial (r, 150.0) == 1, "its gain dial sweeps 300 degrees");
    ok (fc_rig_set_gain_dial (r, std::numeric_limits<double>::quiet_NaN()) == 0
            && fc_rig_set_dial (r, "Bass", std::numeric_limits<double>::infinity()) == 0
            && fc_rig_set_dial (r, nullptr, 10.0) == 0 && fc_rig_set_switch (r, "Channel", nullptr) == 0,
        "a non-finite angle and a NULL name or value are refused");
    ok (fc_rig_set_dial (r, "Nope", 10.0) == 0 && fc_rig_set_switch (r, "Channel", "blue") == 0,
        "a knob the pack does not have, a value nothing was captured at: refused");
    ok (fc_rig_set_max_models (r, 0) == 0 && fc_rig_set_max_models (r, 3) == 0 && fc_rig_set_max_models (r, 1) == 1,
        "max models takes 1 or 2 only");
    test::run (fc_rig_process (r, x, x, 4) == 1);   // the law asks on the audio side; the ask is offered after
    ok (fc_rig_take_job (r) == 1 && fc_rig_job_needs_bytes (r) == 1, "the law asks for its first capture");
    ok (fc_rig_job_bytes (r, 0) == nullptr && fc_rig_job_bytes (r, 256 * 1024 * 1024 + 1) == nullptr,
        "a bytes buffer of 0 or more than 256 MiB is refused");
    void* p = fc_rig_alloc (64);
    ok (p != nullptr && fc_rig_alloc (0) == nullptr, "alloc gives module memory; 0 bytes gives none");
    fc_rig_free (p);
    fc_rig_destroy (r);   // with a job out: nothing leaks (the ASan row would say)
}

void noAllocation()
{
    group ("RT: process allocates nothing — two models, one, mono, stereo, through loads and dips");
    const auto pack = fc_rig_fixture::pack();
    long long during = 0, calls = 0;
    for (const int channels : { 1, 2 })
        for (const int models : { 2, 1 })
        {
            Abi a; a.files = pack.files;
            test::run (fc_rig_prepare (a.r, kFs, kBlock, channels) == 1);
            test::run (fc_rig_set_max_models (a.r, models) == 1);
            test::run (fc_rig_load (a.r, pack.manifest.c_str(), (int) pack.manifest.size()) == 1);
            float* io[2] { fc_rig_buffer (a.r, 0), fc_rig_buffer (a.r, 1) };
            for (int b = 0; b < 600; ++b)
            {
                if (b % 120 == 60) (void) fc_rig_set_gain_dial (a.r, (double) ((b * 37) % 300));
                if (b == 300) (void) fc_rig_set_switch (a.r, "Channel", "red");
                if (b == 420) (void) fc_rig_set_switch (a.r, "Channel", "green");
                for (int c = 0; c < channels; ++c)
                    for (int i = 0; i < kBlock; ++i) io[c][i] = sample (b * kBlock + i, c);
                const long long before = test::alloc::count.load();
                test::run (fc_rig_process (a.r, io[0], io[1], kBlock) == 1);
                if (b % 200 == 199) fc_rig_reset (a.r);
                during += test::alloc::count.load() - before;
                ++calls;
                a.pump();
            }
        }
    test::okNoAlloc (during == 0, "no allocation in " + std::to_string (calls) + " process()/reset() calls ("
                                  + std::to_string (during) + ")");
}
} // namespace

int main()
{
    std::printf ("felitronics_rig_abi_tests\n");
    contract();
    bitIdentity();
    jobProtocol();
    noAllocation();
    return felitronics::test::report();
}
