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

enum class Kind { Gain, Dial, Switch, Max, Normalize, Trims, Shape, Reset, Reload, Unload, Standby, Feed };
struct Event { int block; Kind kind; std::string name, value; double a = 0.0, b = 0.0; };

/** The player as the app drives it: a ModelSource over the files, one job per pump, run and delivered in place. */
struct Direct
{
    RigPlayer p;
    Files files;
    Counts refuse;
    long long fetched = 0, jobs = 0;
    bool fed = false;      // the host's feed…
    double feedDb = 0.0;
    // …handed to the player as the feed (setFeedDb, what fc_rig does) — or, on v0.5.0's spelling, as the host's whole
    // input level, the pack's own plus it, restated after every load: the PREVIOUS code path, which the sound of the
    // new one is held to (the scope cannot be: only setFeedDb puts the dry between the feed and the pack's level).
    bool previousFeed = false;

    void feed()
    {
        if (! fed) return;
        if (previousFeed) p.setHostInputDb (p.stageInputDb() + feedDb);
        else test::run (p.setFeedDb (feedDb));
    }
    bool load (const std::string& manifest)
    {
        bool okParse = false;
        const auto rig = namz::rig::loadRigManifest (manifest, &okParse);
        if (! okParse) { p.unload(); feed(); return false; }
        const bool loaded = p.load (rig, [this] (const std::string& id) {
            ++fetched;
            const auto it = files.find (id);
            return it == files.end() || refused (refuse, id) ? std::vector<std::byte> {} : it->second;
        });
        feed();
        return loaded;
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
        case Kind::Unload:    d.p.unload(); d.feed(); break;
        case Kind::Standby:   d.p.setStandby (e.a != 0.0); break;
        case Kind::Feed:      d.fed = true; d.feedDb = e.a; d.feed(); break;
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
        case Kind::Standby:   fc_rig_set_standby (a.r, e.a != 0.0 ? 1 : 0); break;
        case Kind::Feed:      (void) fc_rig_set_feed_db (a.r, e.a); break;
    }
}

struct Outcome
{
    int  differBlocks = 0, firstDiffer = -1;
    int  scopeDiffer = 0, scopeReads = 0;   // blocks whose scope frame (or count) differed; reads that had a frame
    double energy = 0.0, moved = 0.0;
    long long handed = 0, abiJobs = 0, fetched = 0, directJobs = 0, failed = 0;
    int runs = 0, pairs = 0;
    Counts asked, landed;
};

/** `blocks` blocks of the input through both sides, the events applied before their block, pumped after each. */
Outcome compare (const Files& files, int channels, int maxBeforeLoad, const std::vector<Event>& events, int blocks,
                 const Counts& refuse = {}, bool previousFeed = false)
{
    const auto fixture = fc_rig_fixture::pack();
    Direct d; d.files = files; d.refuse = refuse; d.previousFeed = previousFeed;
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
        // The scope as a page reads it after a quantum: the newest 2048 of both sides, through the ABI and directly.
        {
            static std::vector<float> ad (2048), aw (2048), dd (2048), dw (2048);
            const int an = fc_rig_scope_read (a.r, ad.data(), aw.data(), 2048);
            const int dn = d.p.scopeRead (dd.data(), dw.data(), 2048);
            const bool scopeSame = an == dn && (double) d.p.scopeFrames() == fc_rig_scope_frames (a.r)
                && std::memcmp (ad.data(), dd.data(), sizeof (float) * (std::size_t) an) == 0
                && std::memcmp (aw.data(), dw.data(), sizeof (float) * (std::size_t) an) == 0;
            if (! scopeSame && ! previousFeed) ++o.scopeDiffer;
            if (an > 0) ++o.scopeReads;
        }
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
    ok (two.scopeDiffer == 0 && one.scopeDiffer == 0 && stereo.scopeDiffer == 0 && reload.scopeDiffer == 0
        && two.scopeReads > 1000, "ABI 2: the scope frame and its count are the player's, bit for bit, after every block");

    // ABI 2: PARK and the feed — stood by mid-sweep, knobs and the feed moved while it stands by, back; a fade cut
    // short both ways; a reload under a feed.
    auto park = theHand();
    park.insert (park.end(), { { 1050, Kind::Feed, "", "", 4.0 }, { 1070, Kind::Standby, "", "", 1.0 },
                               { 1090, Kind::Gain, "", "", 60.0 }, { 1095, Kind::Dial, "Treble", "", 60.0 },
                               { 1100, Kind::Feed, "", "", -3.0 }, { 1120, Kind::Standby, "", "", 0.0 },
                               { 1140, Kind::Standby, "", "", 1.0 }, { 1142, Kind::Standby, "", "", 0.0 },
                               { 1160, Kind::Reload }, { 1180, Kind::Feed, "", "", 0.0 },
                               { 1200, Kind::Feed, "", "", 5.0 }, { 1220, Kind::Unload }, { 1250, Kind::Reload } });
    for (const int ch : { 1, 2 })
    {
        const auto o = compare (files, ch, 0, park, 1300);
        report (ch == 1 ? "PARK and the feed, mono" : "PARK and the feed, stereo", o, 1300);
        ok (o.differBlocks == 0 && o.scopeDiffer == 0 && o.energy > 1.0,
            std::string ("ABI 2, ") + (ch == 1 ? "mono" : "stereo") + ": stand-by, a fade cut short both ways, knobs and "
            "the feed moved while stood by, a reload and an unload under a feed — output and scope identical");
        // …and the sound is v0.5.0's: the same scenario against the player driven on v0.5.0's spelling of the feed —
        // up to the unload. v0.5.0's fc_rig restated the feed as the hand on the EMPTY player (fc_rig_unload →
        // applyFeed → setHostInputDb(0 + feed)), and v0.5.0's player published that hand there: linOf(feed) in, unity
        // out. This player publishes the feed alone on an empty player and keeps the hand for the next load
        // (publishLevels), so the old spelling run on it no longer stands for v0.5.0 past an unload. The numbers it
        // publishes there are v0.5.0 fc_rig's all the same — linOf(feed), unity — and fc_rig against the player is held
        // through the unload just above.
        auto parkLoaded = park;
        parkLoaded.erase (std::remove_if (parkLoaded.begin(), parkLoaded.end(), [] (const Event& e) { return e.block >= 1220; }),
                          parkLoaded.end());
        const auto was = compare (files, ch, 0, parkLoaded, 1300, {}, true);
        ok (was.differBlocks == 0 && was.energy > 1.0,
            std::string ("ABI 2, ") + (ch == 1 ? "mono" : "stereo") + ": the same scenario up to the unload plays byte "
            "for byte as the player fed on v0.5.0's spelling (the host's input level = the pack's plus the feed)");
    }
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

/** The feed's MEANING, not its forwarding (bitIdentity drives both sides through the same call, so it proves only that
    the feed is carried). It is the app's block IN (PluginProcessor.cpp:1497), in front of the player: the scope's dry
    stands after it and before the pack's own input level (CapturedBlock.h:543-544 copies the block's input behind that
    trim and before player.process) — and the device is fed at the pack's level plus it, so a pack at -6 dB fed +6 plays
    byte for byte as that pack at 0 dB unfed, and a reload follows the new pack's level. A feed of 0 is no feed at all. */
void feedMeaning()
{
    group ("the feed: on the dry tap alone, in front of the pack's level; the sound at the pack's level plus it; 0 is none");
    const auto fixture = fc_rig_fixture::pack();
    bool okParse = false;
    auto rig = namz::rig::loadRigManifest (fixture.manifest, &okParse);
    test::run (okParse && rig.chain.size() == 1 && rig.chain[0].inputDb == -2.0);
    const auto at = [&rig] (double db) { auto r = rig; r.chain[0].inputDb = db; return namz::rig::writeManifest (r); };
    const std::string minus6 = at (-6.0), zero = at (0.0), plus4 = at (4.0);
    const float six = (float) std::pow (10.0, 6.0 / 20.0);

    int clock = 0;
    const auto play = [&clock] (Abi& a, int blocks, std::vector<float>* out = nullptr) {
        for (int b = 0; b < blocks; ++b, clock += kBlock)
        {
            float* io = fc_rig_buffer (a.r, 0);
            for (int i = 0; i < kBlock; ++i) io[i] = sample (clock + i, 0);
            test::run (fc_rig_process (a.r, io, nullptr, kBlock) == 1);
            if (out != nullptr) out->insert (out->end(), io, io + kBlock);
            a.pump();
        }
    };
    // The dry tap's last block against the input of that block times `g`, bit for bit (48 kHz: no rate-match).
    const auto dryIs = [&clock] (Abi& a, float g) {
        std::vector<float> d (kBlock), w (kBlock);
        if (fc_rig_scope_read (a.r, d.data(), w.data(), kBlock) != kBlock || fc_rig_latency_samples (a.r) != 0) return false;
        for (int i = 0; i < kBlock; ++i)
            if (d[(std::size_t) i] != sample (clock - kBlock + i, 0) * g) return false;
        return true;
    };
    const auto load = [] (Abi& a, const std::string& m) { test::run (fc_rig_load (a.r, m.c_str(), (int) m.size()) == 1); };
    {
        Abi a; a.files = fixture.files;
        test::run (fc_rig_prepare (a.r, kFs, kBlock, 1) == 1);
        load (a, minus6);
        test::run (fc_rig_set_feed_db (a.r, 6.0) == 1);
        play (a, 60);
        ok (dryIs (a, six), "a pack at -6 dB in, the feed +6: the dry tap is the input times the feed alone (+6 dB), "
            "not the pack's level — bit for bit");
        load (a, fixture.manifest);
        play (a, 60);
        ok (dryIs (a, six), "…reloaded to a pack at -2 dB, the feed still +6: the dry tap still the input times the feed");
    }
    {
        // The sound: the device is fed at the pack's level PLUS the feed — the same gain, the same bytes.
        Abi fed, flat;
        fed.files = flat.files = fixture.files;
        test::run (fc_rig_prepare (fed.r, kFs, kBlock, 1) == 1 && fc_rig_prepare (flat.r, kFs, kBlock, 1) == 1);
        load (fed, minus6);
        test::run (fc_rig_set_feed_db (fed.r, 6.0) == 1);
        load (flat, zero);
        std::vector<float> outFed, outFlat;
        int start = clock;
        play (fed, 300, &outFed);
        clock = start;
        play (flat, 300, &outFlat);
        double energy = 0.0;
        for (const float v : outFed) energy += (double) v * v;
        ok (energy > 1.0 && outFed == outFlat, "a pack at -6 dB fed +6 plays byte-identical to that pack at 0 dB unfed, 300 blocks");
        load (fed, fixture.manifest);
        load (flat, plus4);
        outFed.clear(); outFlat.clear();
        start = clock;
        play (fed, 300, &outFed);
        clock = start;
        play (flat, 300, &outFlat);
        ok (outFed == outFlat, "…reloaded to the pack at -2 dB under the same feed: byte-identical to that pack at +4 dB "
            "unfed — the feed follows the pack");
    }
    {
        Abi none, zeroFeed;
        none.files = zeroFeed.files = fixture.files;
        for (Abi* a : { &none, &zeroFeed })
        {
            test::run (fc_rig_prepare (a->r, kFs, kBlock, 1) == 1);
            test::run (fc_rig_load (a->r, fixture.manifest.c_str(), (int) fixture.manifest.size()) == 1);
        }
        test::run (fc_rig_set_feed_db (zeroFeed.r, 0.0) == 1);
        std::vector<float> outNone, outZero;
        const int start = clock;
        play (none, 300, &outNone);
        clock = start;
        play (zeroFeed, 300, &outZero);
        double energy = 0.0;
        for (const float v : outNone) energy += (double) v * v;
        ok (energy > 1.0 && outNone.size() == outZero.size()
                && std::memcmp (outNone.data(), outZero.data(), sizeof (float) * outNone.size()) == 0,
            "a feed of 0 dB: the output byte-identical to a feed never set, 300 blocks");
    }
}

void contract()
{
    group ("the contract");
    {
        std::vector<float> d (16), w (16);
        ok (fc_rig_scope_read (nullptr, d.data(), w.data(), 16) == 0 && fc_rig_scope_frames (nullptr) == 0.0
            && fc_rig_set_feed_db (nullptr, 1.0) == 0, "ABI 2 on a NULL instance: nothing read, nothing written, refused");
        fc_rig_set_standby (nullptr, 1);
        fc_rig* r = fc_rig_create();
        ok (fc_rig_scope_read (r, d.data(), w.data(), 16) == 0 && fc_rig_scope_frames (r) == 0.0,
            "an instance never prepared has no frame");
        test::run (fc_rig_prepare (r, kFs, kBlock, 1) == 1);
        ok (fc_rig_set_feed_db (r, std::numeric_limits<double>::quiet_NaN()) == 0
            && fc_rig_set_feed_db (r, std::numeric_limits<double>::infinity()) == 0 && fc_rig_set_feed_db (r, 3.0) == 1,
            "a non-finite feed is refused, a finite one taken");
        ok (fc_rig_scope_read (r, d.data(), w.data(), 16) == 0, "prepared, nothing processed: no frame yet");
        float* io = fc_rig_buffer (r, 0);
        std::fill (io, io + kBlock, 0.1f);
        test::run (fc_rig_process (r, io, nullptr, kBlock) == 1);
        ok (fc_rig_scope_read (r, d.data(), w.data(), 16) == 16 && fc_rig_scope_frames (r) == (double) kBlock
            && fc_rig_scope_read (r, nullptr, w.data(), 16) == 0 && fc_rig_scope_read (r, d.data(), w.data(), 0) == 0,
            "one block: a frame, the count moved by the block; a NULL array or n < 1 reads nothing");
        fc_rig_set_standby (r, 1);
        for (int k = 0; k < 10; ++k) test::run (fc_rig_process (r, io, nullptr, kBlock) == 1);
        const double frames = fc_rig_scope_frames (r);
        test::run (fc_rig_process (r, io, nullptr, kBlock) == 1);
        ok (fc_rig_scope_frames (r) == frames, "stood by, the count stands still");
        fc_rig_destroy (r);
    }
    ok (fc_rig_abi_version() == FC_RIG_ABI_VERSION && FC_RIG_ABI_VERSION == 5u,
        "abi_version answers the header's 5 (the lighter load and the files up front since v0.7.0; the preload since v0.6.0; the scope at the app's two points since v0.5.1)");
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

// ABI 5: the files up front. Every file but one handed in right after the load: across a sweep of the gain dial that
// visits every capture, the only file a job ever needs bytes for is the one left out; and a job already out waiting
// for a file takes it from fc_rig_put_file.
void filesUpFront()
{
    group ("ABI 5 — the files up front (fc_rig_put_file)");
    const auto expect = [] (bool c, const std::string& msg) { ok (c, msg); return c; };
    const auto fixture = fc_rig_fixture::pack();
    if (! expect (fixture.files.size() >= 3, "the fixture pack has files to hand in")) return;
    const auto put = [] (fc_rig* r, const std::string& id, const std::vector<std::byte>& b) {
        std::uint8_t* p = fc_rig_put_file (r, id.c_str(), (int) b.size());
        if (p != nullptr) std::memcpy (p, b.data(), b.size());
        return p != nullptr;
    };
    // A sweep of the gain dial over every capture; the files handed in up front are all but `missing` (all of them
    // when it is empty… none, when `upFront` is false). Answers which files a job needed bytes for.
    float io[kBlock] {};
    const auto sweep = [&] (bool upFront, const std::string& missing, int& jobs, int& landed, int& taken) {
        std::map<std::string, int> needed;
        jobs = landed = taken = 0;
        fc_rig* r = fc_rig_create();
        test::run (fc_rig_prepare (r, kFs, kBlock, 1) == 1);
        if (upFront) ok (! put (r, fixture.files.begin()->first, fixture.files.begin()->second), "no pack loaded: nothing taken (NULL)");
        test::run (fc_rig_load (r, fixture.manifest.c_str(), (int) fixture.manifest.size()) == 1);
        if (upFront)
        {
            for (const auto& [id, bytes] : fixture.files)
                if (id != missing) taken += put (r, id, bytes) ? 1 : 0;
            const auto& first = *fixture.files.begin();
            ok (! put (r, first.first, first.second), "a file already held: NULL, the first bytes stay");
            ok (fc_rig_put_file (r, "no-such-file.namz", 16) == nullptr, "an id the pack does not name: NULL");
            ok (fc_rig_put_file (r, first.first.c_str(), 0) == nullptr && fc_rig_put_file (r, nullptr, 16) == nullptr
                && fc_rig_put_file (nullptr, first.first.c_str(), 16) == nullptr, "size 0, a NULL id, no instance: NULL");
        }
        const double span = fc_rig_gain_dial_sweep (r);
        for (int b = 0; b < 1300; ++b)
        {
            if (b % 100 == 0) (void) fc_rig_set_gain_dial (r, span * (double) ((b / 100) % 13) / 12.0);
            std::fill (io, io + kBlock, 0.01f);
            test::run (fc_rig_process (r, io, nullptr, kBlock) == 1);
            if (fc_rig_take_job (r) != 1) continue;
            ++jobs;
            const std::string id (fc_rig_job_file (r), (std::size_t) fc_rig_job_file_length (r));
            if (fc_rig_job_needs_bytes (r) == 1)
            {
                ++needed[id];
                const auto& bytes = fixture.files.at (id);
                std::memcpy (fc_rig_job_bytes (r, (int) bytes.size()), bytes.data(), bytes.size());
            }
            (void) fc_rig_job_run (r);
            landed += fc_rig_job_deliver (r) == 1 ? 1 : 0;
        }
        fc_rig_destroy (r);
        return needed;
    };
    int jobs = 0, landed = 0, taken = 0;
    const auto asked = sweep (false, "", jobs, landed, taken);
    if (! expect (asked.size() >= 2 && landed == jobs, "without the files up front, the sweep asks for "
                  + std::to_string (asked.size()) + " files")) return;
    const std::string missing = asked.rbegin()->first;   // one the sweep visits, never handed in
    const auto needed = sweep (true, missing, jobs, landed, taken);
    ok (taken == (int) fixture.files.size() - 1, "every file handed in is taken (" + std::to_string (taken) + ")");
    ok (landed == jobs && jobs >= (int) asked.size(), "the sweep loaded captures (" + std::to_string (jobs) + " jobs, "
        + std::to_string (landed) + " landed)");
    ok (needed.size() == 1 && needed.count (missing) == 1,
        "only the file not handed in ever needed bytes (" + std::to_string (needed.size()) + " distinct, " + missing + ")");

    // A job out waiting for a file: handed in now, the job carries it and runs to a model.
    fc_rig* r = fc_rig_create();
    test::run (fc_rig_prepare (r, kFs, kBlock, 1) == 1);
    test::run (fc_rig_load (r, fixture.manifest.c_str(), (int) fixture.manifest.size()) == 1);
    int got = 0;
    for (int b = 0; b < 50 && got == 0; ++b)
    {
        std::fill (io, io + kBlock, 0.0f);
        test::run (fc_rig_process (r, io, nullptr, kBlock) == 1);
        got = fc_rig_take_job (r);
    }
    if (expect (got == 1 && fc_rig_job_needs_bytes (r) == 1, "a job out, needing bytes"))
    {
        const std::string id (fc_rig_job_file (r), (std::size_t) fc_rig_job_file_length (r));
        ok (put (r, id, fixture.files.at (id)), "its file handed in up front is taken");
        ok (fc_rig_job_needs_bytes (r) == 0, "…and the job no longer needs bytes");
        ok (fc_rig_job_run (r) == 1 && fc_rig_job_deliver (r) == 1, "…and runs to a model that lands");
    }
    fc_rig_destroy (r);
}

// THE QUIET WARM (fc_rig_warm_quiet, v0.7.0): a warming capture fed blocks of silence ahead of real time while the
// input is silence opens after a few blocks instead of its field — and from the moment the live warm-up opens too
// the two sessions are the same, bit for bit (riff included). Mid-riff it does nothing: the input is not silence.
struct QuietRun { std::vector<float> out; int firstAudible = -1; long long quietBlocks = 0; };

QuietRun quietSession (const fc_rig_fixture::Pack& fixture, int quietBlocks, int maxModels, double dialFrac, int riffAt,
                       int blocks, int ulpAt = -1)
{
    QuietRun q;
    fc_rig* r = fc_rig_create();
    test::run (fc_rig_prepare (r, kFs, kBlock, 1) == 1);
    test::run (fc_rig_load (r, fixture.manifest.c_str(), (int) fixture.manifest.size()) == 1);
    for (const auto& [id, bytes] : fixture.files)
        if (std::uint8_t* p = fc_rig_put_file (r, id.c_str(), (int) bytes.size()); p != nullptr)
            std::memcpy (p, bytes.data(), bytes.size());
    (void) fc_rig_set_max_models (r, maxModels);
    (void) fc_rig_set_gain_dial (r, fc_rig_gain_dial_sweep (r) * dialFrac);
    float io[kBlock] {};
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const double t = (double) ((b - riffAt) * kBlock + i) / kFs;
            io[i] = b < riffAt ? 0.0f : (float) (0.3 * std::sin (2.0 * kPi * 82.41 * t) + 0.12 * std::sin (2.0 * kPi * 196.0 * t));
        }
        if (b * kBlock <= ulpAt && ulpAt < (b + 1) * kBlock)
            io[ulpAt - b * kBlock] += 1e-5f;   // the control: -100 dB on one sample (one ulp is rounded away by the input level)
        test::run (fc_rig_process (r, io, nullptr, kBlock) == 1);
        q.out.insert (q.out.end(), io, io + kBlock);
        if (q.firstAudible < 0 && fc_rig_audible (r) == 1) q.firstAudible = b;
        if (quietBlocks > 0) q.quietBlocks += fc_rig_warm_quiet (r, quietBlocks);
        if (fc_rig_take_job (r) == 1) { (void) fc_rig_job_run (r); (void) fc_rig_job_deliver (r); }
    }
    fc_rig_destroy (r);
    return q;
}

bool sameBits (const std::vector<float>& a, const std::vector<float>& b, std::size_t from)
{
    return a.size() == b.size() && std::memcmp (a.data() + from, b.data() + from, (a.size() - from) * sizeof (float)) == 0;
}

void quietWarm()
{
    group ("ABI 5 — the quiet warm (fc_rig_warm_quiet): silence warms ahead, bit-identical to the live warm-up");
    // The fixture with fields worth warming: two of its captures WaveNets reaching back 2000 and 1200 samples (16 and
    // 10 blocks); the 60 capture stays a gain (no field: nothing to warm, at once either way).
    auto fixture = fc_rig_fixture::pack();
    fixture.files["g150.namz"] = fc_rig_fixture::packed (fc_rig_fixture::waveNet (2000));
    fixture.files["g240.nam"]  = fc_rig_fixture::bytesOf (fc_rig_fixture::waveNet (1200));
    constexpr int kRiffAt = 400, kBlocks = 700;
    for (const int maxModels : { 1, 2 })
        for (const double dial : { 0.0, 0.5, 0.8 })
        {
            const std::string at = "maxModels " + std::to_string (maxModels) + ", dial " + std::to_string (dial).substr (0, 3);
            const auto live  = quietSession (fixture, 0, maxModels, dial, kRiffAt, kBlocks);
            const auto quiet = quietSession (fixture, 3, maxModels, dial, kRiffAt, kBlocks);
            ok (quiet.firstAudible >= 0 && (dial < 0.1 ? quiet.firstAudible == live.firstAudible && quiet.quietBlocks == 0
                                                       : quiet.firstAudible < live.firstAudible && quiet.quietBlocks > 0),
                at + ": opens at block " + std::to_string (quiet.firstAudible) + " with the quiet warm (" + std::to_string (quiet.quietBlocks)
                + " blocks of silence ahead), " + std::to_string (live.firstAudible) + " without");
            std::size_t same = live.out.size();
            while (same > 0 && std::memcmp (&live.out[same - 1], &quiet.out[same - 1], sizeof (float)) == 0) --same;
            ok (sameBits (live.out, quiet.out, (std::size_t) kRiffAt * kBlock),
                at + ": the riff is bit-identical (identical from sample " + std::to_string (same) + ", block "
                + std::to_string (same / kBlock) + "; the riff from block " + std::to_string (kRiffAt) + ")");
            const auto control = quietSession (fixture, 3, maxModels, dial, kRiffAt, kBlocks, kRiffAt * kBlock + 1000);
            ok (! sameBits (live.out, control.out, (std::size_t) kRiffAt * kBlock), at + ": the control (one input sample 1e-5 off) differs");
            const auto midLive  = quietSession (fixture, 0, maxModels, dial, 0, 300);
            const auto midQuiet = quietSession (fixture, 3, maxModels, dial, 0, 300);
            ok (midQuiet.quietBlocks == 0 && sameBits (midLive.out, midQuiet.out, 0),
                at + ": mid-riff (the input never silent) nothing is warmed ahead, every sample the same");
        }
}

int main()
{
    std::printf ("felitronics_rig_abi_tests\n");
    contract();
    bitIdentity();
    feedMeaning();
    jobProtocol();
    filesUpFront();
    quietWarm();
    noAllocation();
    return felitronics::test::report();
}
