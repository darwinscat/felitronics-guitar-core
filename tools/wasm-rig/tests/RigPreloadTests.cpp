// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// THE PRELOAD (RigPlayer::setPreload, fc_rig ABI 4) — what it is for, held as numbers: once the set is ready, a turn of
// the gain dial across the whole sweep and back hands out NO load job (so nothing runs NamStage::prepareModel: run() is
// the player's only caller of it, and run() is only ever handed a job), the sound is bit for bit the player's without
// the preload on the same dial path (the cache must not change the sound), process() allocates nothing through it, an
// unload frees it — the heap read back where it was — and the ABI says the same. On the fc_rig fixture pack (two
// panels, a link, a WaveNet, a memory), and on two packs made here: six panels (two switches over a gain dial) and a
// pack of switches alone (no gain dial).

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
#include <map>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

using felitronics::rigplayer::RigPlayer;
using felitronics::test::group;
using felitronics::test::ok;
namespace alloc = felitronics::test::alloc;

namespace
{
constexpr int kBlock = 128;
constexpr double kPi = 3.14159265358979323846;

float sample (int i, double fs)
{
    const double t = i / fs, pluck = std::fmod (t, 0.45);
    const double env = std::exp (-pluck * 6.0) * (pluck < 0.002 ? pluck / 0.002 : 1.0);
    return (float) (0.5 * env * (std::sin (2.0 * kPi * 110.0 * t) + 0.35 * std::sin (2.0 * kPi * 330.0 * t)));
}

/** The heap in use, read off the allocator — not the counter's running total, which counts what was ever allocated.
    -1 where there is no reading here (the checks that need it say so and skip). */
long long heapInUse()
{
#if defined(__APPLE__)
    malloc_statistics_t st {};
    malloc_zone_statistics (nullptr, &st);
    return (long long) st.size_in_use;
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    const auto mi = mallinfo2();
    return (long long) (mi.uordblks + mi.hblkhd);
#else
    return -1;
#endif
}

/** SIX PANELS: two switches (Boost off/on, Voice a/b/c) over a gain dial captured at seven points — 42 files, more
    copies than the old default cap of 33 held. Memoryless captures, each its own gain and tag. */
fc_rig_fixture::Pack sixPanels()
{
    namz::rig::Rig rig;
    rig.rigId = "six-panels"; rig.name = "Six Panels"; rig.modeledBy = "the preload suite";
    namz::rig::Stage st;
    st.kind = namz::rig::StageKind::Nam; st.rawKind = "nam"; st.slot = "pedal";
    st.make = "Darwin's Cat"; st.model = "Six Panels"; st.gearType = "pedal";
    auto& d = st.device;
    d.family = "Six Panels"; d.rigId = "six-panels"; d.slot = "pedal";
    namz::rig::Control boost; boost.name = "Boost"; boost.role = namz::rig::Role::Generic; boost.values = { "off", "on" };
    namz::rig::Control voice; voice.name = "Voice"; voice.role = namz::rig::Role::Channel; voice.values = { "a", "b", "c" };
    namz::rig::Control g; g.name = "Gain"; g.role = namz::rig::Role::Gain; g.sweep = 300;
    g.values = { "20", "60", "100", "150", "200", "240", "280" };
    d.controls = { boost, voice, g };
    fc_rig_fixture::Pack p;
    int n = 0;
    for (const auto& b : boost.values)
        for (const auto& v : voice.values)
            for (const auto& gv : g.values)
            {
                const std::string id = b + v + gv + ".namz";
                namz::rig::FileEntry f; f.id = id; f.settings = { { "Boost", b }, { "Voice", v }, { "Gain", gv } };
                d.files.push_back (f);
                p.files[id] = fc_rig_fixture::packed (fc_rig_fixture::taggedGain (0.2 + 0.02 * n, -24.0 + 0.2 * n));
                ++n;
            }
    rig.chain = { st };
    p.manifest = namz::rig::writeManifest (rig);
    return p;
}

/** SWITCHES ALONE: one control (Voice a/b/c), no gain dial — so a panel has no knots to tell it by. */
fc_rig_fixture::Pack switchOnly()
{
    namz::rig::Rig rig;
    rig.rigId = "switch-only"; rig.name = "Switch Only"; rig.modeledBy = "the preload suite";
    namz::rig::Stage st;
    st.kind = namz::rig::StageKind::Nam; st.rawKind = "nam"; st.slot = "pedal";
    st.make = "Darwin's Cat"; st.model = "Switch Only"; st.gearType = "pedal";
    auto& d = st.device;
    d.family = "Switch Only"; d.rigId = "switch-only"; d.slot = "pedal";
    namz::rig::Control voice; voice.name = "Voice"; voice.role = namz::rig::Role::Channel; voice.values = { "a", "b", "c" };
    d.controls = { voice };
    fc_rig_fixture::Pack p;
    int n = 0;
    for (const auto& v : voice.values)
    {
        namz::rig::FileEntry f; f.id = v + ".namz"; f.settings = { { "Voice", v } };
        d.files.push_back (f);
        p.files[f.id] = fc_rig_fixture::packed (fc_rig_fixture::taggedGain (0.3 + 0.2 * n++, -18.0));
    }
    rig.chain = { st };
    p.manifest = namz::rig::writeManifest (rig);
    return p;
}

/** A DENSE Linear on NAM's partitioned FFT engine, whose clock no reset rewinds (NamStage.h). */
std::string denseLinear (int taps)
{
    std::string w = "[";
    for (int i = 0; i < taps; ++i)
    {
        char b[40];
        std::snprintf (b, sizeof b, "%s%.6f", i > 0 ? "," : "", 0.3 * std::exp (-i / 400.0) * std::cos (i * 0.37));
        w += b;
    }
    w += "]";
    return R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":)" + std::to_string (taps)
         + R"(,"bias":false,"implementation":"fft"},"weights":)" + w + R"(,"sample_rate":48000})";
}

/** The player as a host drives it: a source over the files, every job run and delivered where it is taken. */
struct Host
{
    RigPlayer p;
    fc_rig_fixture::Pack pack;
    long long jobs = 0, fetched = 0, allocsInProcess = 0;

    Host (double fs, bool preload, int maxModels, fc_rig_fixture::Pack pk = fc_rig_fixture::pack(),
          int cap = RigPlayer::kPreloadWholePack, bool loadNow = true)
        : pack (std::move (pk))
    {
        p.setMaxModels (maxModels);
        if (preload) p.setPreload (true, cap);
        felitronics::test::run (p.prepare (fs, kBlock, 1));
        if (loadNow) load();
    }
    void load()
    {
        bool parsed = false;
        const auto rig = namz::rig::loadRigManifest (pack.manifest, &parsed);
        felitronics::test::run (parsed && p.load (rig, [this] (const std::string& id) {
            ++fetched;
            const auto it = pack.files.find (id);
            return it == pack.files.end() ? std::vector<std::byte> {} : it->second;
        }));
    }
    void pump()
    {
        p.service();
        while (auto job = p.takeLoadJob()) { ++jobs; p.deliver (RigPlayer::run (std::move (*job))); }
    }
    void block (int at, double fs, std::vector<float>& out)
    {
        float buf[kBlock];
        for (int i = 0; i < kBlock; ++i) buf[i] = sample (at + i, fs);
        float* io[1] { buf };
        const long long before = alloc::count.load();
        felitronics::test::run (p.process (io, 1, kBlock));
        allocsInProcess += alloc::count.load() - before;
        out.insert (out.end(), buf, buf + kBlock);
        pump();
    }
    bool ready (int most = 500)
    {
        for (int guard = 0; ! p.preloadProgress().ready() && guard < most; ++guard) pump();
        return p.preloadProgress().ready();
    }
    std::vector<float> scratch;
    /** `n` blocks from block `at`, the gain dial (if it has one) at `dial(k)` degrees before block k (negative: left). */
    template <class Dial> void play (int& at, int n, double fs, Dial dial, std::vector<float>& out)
    {
        for (int k = 0; k < n; ++k, ++at)
        {
            if (const double deg = dial (k); deg >= 0.0) felitronics::test::run (p.setDial ("Gain", deg));
            block (at * kBlock, fs, out);
        }
    }
};

bool same (const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
}

/** The dial path every comparison plays: a rest, 0 -> 300 degrees continuously (one degree a block), back to 0, the
    other panel (a switch), the first again, two models -> one -> two along the way. */
template <class Each> void path (Each each)
{
    int b = 0;
    for (; b < 60; ++b) each (b, -1.0, "");
    for (int d = 0; d <= 300; ++d, ++b) each (b, (double) d, "");
    for (int d = 300; d >= 0; --d, ++b) each (b, (double) d, b == 450 ? "one" : "");
    for (int d = 0; d <= 300; d += 3, ++b) each (b, (double) d, d == 150 ? "two" : "");
    each (b++, -1.0, "red");
    for (int k = 0; k < 120; ++k, ++b) each (b, -1.0, "");
    each (b++, -1.0, "green");
    for (int d = 300; d >= 0; d -= 2, ++b) each (b, (double) d, "");
}

void drive (Host& h, double fs, int b, double deg, const std::string& what, std::vector<float>& out)
{
    if (deg >= 0.0) felitronics::test::run (h.p.setDial ("Gain", deg));
    if (what == "red" || what == "green") felitronics::test::run (h.p.setSwitch ("Channel", what));
    if (what == "one") felitronics::test::run (h.p.setMaxModels (1));
    if (what == "two") felitronics::test::run (h.p.setMaxModels (2));
    h.block (b * kBlock, fs, out);
}

double maxDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    double m = a.size() == b.size() ? 0.0 : 1e9;
    for (std::size_t i = 0; i < std::min (a.size(), b.size()); ++i) m = std::max (m, (double) std::abs (a[i] - b[i]));
    return m;
}
} // namespace

int main()
{
    group ("the preload: jobs until the set is ready, then not one through the whole sweep and back");
    {
        for (const int models : { 2, 1 })
        {
            Host h (48000.0, true, models);
            int guard = 0;
            while (! h.p.preloadProgress().ready() && ++guard < 100) h.pump();
            const auto pr = h.p.preloadProgress();
            std::printf ("    %d model(s): the set %d copies, %lld jobs, %lld fetches (4 files), %d held\n", models, pr.total,
                         h.jobs, h.fetched, pr.held);
            ok (pr.ready() && pr.failed == 0, "the set is ready, nothing failed");
            ok (pr.total == 7, "seven copies: green's four knots and its two ends' twins (the link shares one), red's one");
            ok (h.jobs == pr.total, "one job per copy, and no other");
            ok (h.fetched == 4, "each file fetched once: the second copy of a file is prepared from the kept bytes");
            const long long jobsReady = h.jobs;
            std::vector<float> out;
            path ([&] (int b, double deg, const std::string& what) { drive (h, 48000.0, b, deg, what, out); });
            ok (h.jobs == jobsReady, "after ready: ZERO jobs through the sweep, both ways, the switch and back — no prepareModel");
            ok (h.allocsInProcess == 0, "process() allocated nothing through it");
            bool finite = true;
            for (const float v : out) finite = finite && std::isfinite (v);
            ok (finite, "the output is finite");
        }
    }

    group ("the preload does not change the sound: bit for bit the player without it, on the same dial path");
    {
        for (const int models : { 2, 1 })
        {
            Host pre (48000.0, true, models), plain (48000.0, false, models);
            int guard = 0;
            while (! pre.p.preloadProgress().ready() && ++guard < 100) pre.pump();
            std::vector<float> a, b;
            path ([&] (int k, double deg, const std::string& what) {
                drive (pre, 48000.0, k, deg, what, a);
                drive (plain, 48000.0, k, deg, what, b);
            });
            std::printf ("    %d model(s), at the model rate: %zu samples, max |diff| %.3g; jobs during the path %lld (plain)\n",
                         models, a.size(), maxDiff (a, b), plain.jobs);
            ok (a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0,
                "bit for bit at 48 kHz (the model rate)");
            ok (plain.jobs > 0, "…where the player without the preload loaded as it went (the control)");
        }
        // Off the model rate a prepared copy taken again carries its rate-matcher's phase from its last visit, where a
        // fresh load starts it at zero — the same divergence a cold slot's wake has (RigPlayerTests). Measured, stated.
        Host pre (44100.0, true, 2), plain (44100.0, false, 2);
        int guard = 0;
        while (! pre.p.preloadProgress().ready() && ++guard < 100) pre.pump();
        std::vector<float> a, b;
        path ([&] (int k, double deg, const std::string& what) {
            drive (pre, 44100.0, k, deg, what, a);
            drive (plain, 44100.0, k, deg, what, b);
        });
        const double d = maxDiff (a, b);
        std::printf ("    2 models at 44.1 kHz (rate-matched): max |diff| %.3g\n", d);
        ok (d < 1e-4, "off the model rate: within 1e-4 of the player without it (the matcher's resumption floor; a copy taken again\n"
           "        without its drain replayed its last visit: 0.0139)");
    }

    group ("memory, and an unload frees it");
    {
        const long long bytes0 = alloc::bytes.load();
        Host h (48000.0, true, 2);
        int guard = 0;
        while (! h.p.preloadProgress().ready() && ++guard < 100) h.pump();
        const auto pr = h.p.preloadProgress();
        const long long bytes = alloc::bytes.load() - bytes0;
        std::printf ("    the fixture's set: %d copies, %lld bytes allocated to make the player and its set\n", pr.total, bytes);
        ok (pr.held == pr.total, "the pool holds the set");
        h.p.unload();
        h.p.service();
        ok (h.p.preloadProgress().total == 0 && h.p.preloadProgress().held == 0, "unload: the set is gone");
        const auto again = h.p.load (namz::rig::loadRigManifest (h.pack.manifest, nullptr), [&h] (const std::string& id) {
            return h.pack.files.at (id);
        });
        ok (again && h.p.preloadProgress().total == 7 && h.p.preloadProgress().settled == 0, "a load preloads again");
        h.p.setPreload (false);
        ok (h.p.preloadProgress().total == 7, "turned off: the pack loaded keeps its set…");
        ok (h.p.load (namz::rig::loadRigManifest (h.pack.manifest, nullptr), [&h] (const std::string& id) {
                return h.pack.files.at (id);
            }) && h.p.preloadProgress().total == 0, "…and the next load has none");
    }

    group ("a capture that is not one, and a file that could not be fetched: the set settles, the sound as without");
    {
        for (const bool garbage : { true, false })
        {
            Host pre (48000.0, true, 2), plain (48000.0, false, 2);
            for (Host* h : { &pre, &plain })
            {
                if (garbage) h->pack.files["g150.namz"] = fc_rig_fixture::bytesOf ("not a capture");
                else h->pack.files.erase ("g240.nam");
            }
            int guard = 0;
            while (! pre.p.preloadProgress().ready() && ++guard < 100) pre.pump();
            const auto pr = pre.p.preloadProgress();
            ok (pr.ready() && pr.failed >= 1, garbage ? "garbage: ready, with a failed copy" : "unfetched: ready, with copies unfetched");
            std::vector<float> a, b;
            path ([&] (int k, double deg, const std::string& what) {
                drive (pre, 48000.0, k, deg, what, a);
                drive (plain, 48000.0, k, deg, what, b);
            });
            ok (a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0,
                "bit for bit with the player without the preload, the failures included");
        }
    }

    group ("ABI 4: fc_rig_set_preload, the read-outs, and a sweep with no job — bit for bit with RigPlayer");
    {
        const auto pack = fc_rig_fixture::pack();
        fc_rig* r = fc_rig_create();
        ok (fc_rig_abi_version() == 4u, "abi_version answers 4");
        ok (fc_rig_set_preload (r, -1) == 0 && fc_rig_set_preload (nullptr, 1) == 0, "a negative cap, no instance: refused");
        ok (fc_rig_set_preload (r, RigPlayer::kPreloadWholePack) == 1, "on");
        ok (fc_rig_prepare (r, 48000.0, kBlock, 1) == 1, "prepared");
        ok (fc_rig_preload_total (r) == 0 && fc_rig_preload_pending (r) == 0, "nothing loaded: no set");
        ok (fc_rig_load (r, pack.manifest.c_str(), (int) pack.manifest.size()) == 1, "loaded");
        ok (fc_rig_preload_total (r) == 7 && fc_rig_preload_pending (r) == 7, "the set: seven copies, all pending");
        long long jobs = 0;
        const auto pump = [&] {
            while (fc_rig_take_job (r) == 1)
            {
                ++jobs;
                const std::string id (fc_rig_job_file (r), (std::size_t) fc_rig_job_file_length (r));
                if (fc_rig_job_needs_bytes (r) == 1)
                {
                    const auto& bytes = pack.files.at (id);
                    std::memcpy (fc_rig_job_bytes (r, (int) bytes.size()), bytes.data(), bytes.size());
                }
                fc_rig_job_run (r);
                fc_rig_job_deliver (r);
            }
        };
        int guard = 0;
        while (fc_rig_preload_pending (r) > 0 && ++guard < 100) pump();
        ok (fc_rig_preload_pending (r) == 0 && fc_rig_preload_failed (r) == 0 && jobs == 7, "ready after seven jobs");
        Host direct (48000.0, true, 2);
        guard = 0;
        while (! direct.p.preloadProgress().ready() && ++guard < 100) direct.pump();
        const long long jobsReady = jobs;
        std::vector<float> a, b;
        std::vector<float> buf (kBlock);
        path ([&] (int k, double deg, const std::string& what) {
            if (deg >= 0.0) fc_rig_set_gain_dial (r, deg);
            if (what == "red" || what == "green") fc_rig_set_switch (r, "Channel", what.c_str());
            if (what == "one") fc_rig_set_max_models (r, 1);
            if (what == "two") fc_rig_set_max_models (r, 2);
            float* L = fc_rig_buffer (r, 0);
            for (int i = 0; i < kBlock; ++i) L[i] = sample (k * kBlock + i, 48000.0);
            fc_rig_process (r, L, nullptr, kBlock);
            a.insert (a.end(), L, L + kBlock);
            pump();
            drive (direct, 48000.0, k, deg, what, b);
        });
        ok (jobs == jobsReady, "after ready: fc_rig_take_job never answered 1 through the path");
        ok (a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0,
            "bit for bit with RigPlayer driven directly");
        fc_rig_unload (r);
        ok (fc_rig_preload_total (r) == 0, "unloaded: no set");
        fc_rig_destroy (r);
    }

    long long setBytes = 0;   // the fixture's whole set on the heap, for the eviction check below
    group ("unload frees the copies: the heap read back where it was (a cleared copy no slot runs was never freed)");
    {
        std::vector<float> out;
        {   // a whole cycle first, on a player that goes: the allocator's own first growth is not the set's
            Host warm (48000.0, true, 2);
            int w = 0;
            felitronics::test::run (warm.ready());
            warm.play (w, 60, 48000.0, [] (int k) { return 5.0 * k; }, out);
        }
        out.clear(); out.shrink_to_fit();
        Host h (48000.0, true, 2, fc_rig_fixture::pack(), RigPlayer::kPreloadWholePack, false);   // prepared, no pack
        const long long base = heapInUse();
        if (base < 0) std::printf ("    no heap reading on this platform: the heap checks are skipped\n");
        h.load();
        int at = 0;
        felitronics::test::run (h.ready());
        h.play (at, 60, 48000.0, [] (int k) { return 5.0 * k; }, out);   // copies land: a slot runs them
        out.clear(); out.shrink_to_fit();
        const long long held = heapInUse() - base;
        h.p.unload();
        h.play (at, 2, 48000.0, [] (int) { return -1.0; }, out);
        h.p.service();
        out.clear(); out.shrink_to_fit();
        const long long left = heapInUse() - base;
        setBytes = held;
        std::printf ("    the pack and its set held %lld bytes; after unload, two blocks and service(): %lld left (%.2f %%)\n", held,
                     left, held > 0 ? 100.0 * (double) left / (double) held : 0.0);
        if (base >= 0)
        {
            ok (held > 200000, "the set is on the heap (seven copies)");
            ok (left < held / 20, "unload freed it: under 5 % of it left");
        }
    }

    group ("an eviction frees what it evicts, and a copy landed again is the capture asked for (cap 5, red/green x 200)");
    {
        Host pre (48000.0, true, 2, fc_rig_fixture::pack(), 5), plain (48000.0, false, 2);
        felitronics::test::run (pre.ready());
        std::vector<float> a, b;
        int at = 0;
        long long heap20 = 0, heapMax = 0;
        bool alike = true;
        long long jobsWhilePlaying = 0;
        const long long jobsReady = pre.jobs;
        for (int c = 0; c < 200; ++c)
        {
            const std::string chan = c % 2 == 0 ? "red" : "green";
            for (Host* h : { &pre, &plain }) felitronics::test::run (h->p.setSwitch ("Channel", chan));
            const auto dial = [c] (int) { return c % 4 < 2 ? 150.0 : std::fmod (37.0 * c, 300.0); };   // a copy or two land
            int atA = at, atB = at;
            pre.play (atA, 6, 48000.0, dial, a);
            plain.play (atB, 6, 48000.0, dial, b);
            at = atA;
            alike = alike && same (a, b);
            a.clear(); b.clear();
            if (c == 20) heap20 = heapInUse();
            if (c > 20) heapMax = std::max (heapMax, heapInUse());
        }
        jobsWhilePlaying = pre.jobs - jobsReady;
        std::printf ("    jobs while playing: %lld with the preload, %lld without; heap from cycle 20 to the most after: %+lld bytes "
                     "(one set: %lld)\n", jobsWhilePlaying, plain.jobs, heap20 >= 0 ? heapMax - heap20 : 0LL, setBytes);
        ok (alike, "bit for bit with the player without the preload through 200 cycles that land evicted copies again");
        ok (jobsWhilePlaying <= plain.jobs, "…and never more jobs than it");
        if (heap20 >= 0 && setBytes > 0)
            ok (heapMax - heap20 < setBytes, "the heap plateaus: less than one whole set of growth over 180 cycles");
    }

    group ("the default is the whole pack: six panels, every switch combination x a sweep each way — not one job after ready");
    {
        Host h (48000.0, true, 2, sixPanels());
        felitronics::test::run (h.ready());
        const auto pr = h.p.preloadProgress();
        std::printf ("    six panels: the set %d copies (%lld jobs), %d failed\n", pr.total, h.jobs, pr.failed);
        ok (pr.total > 33 && pr.failed == 0, "more copies than the old default cap of 33, all prepared");
        const long long jobsReady = h.jobs;
        std::vector<float> out;
        int at = 0;
        for (const char* boost : { "on", "off" })
            for (const char* voice : { "c", "a", "b" })
            {
                felitronics::test::run (h.p.setSwitch ("Boost", boost) && h.p.setSwitch ("Voice", voice));
                h.play (at, 61, 48000.0, [] (int k) { return 5.0 * k; }, out);
                h.play (at, 61, 48000.0, [] (int k) { return 300.0 - 5.0 * k; }, out);
                out.clear();
            }
        std::printf ("    jobs after ready: %lld\n", h.jobs - jobsReady);
        ok (h.jobs == jobsReady, "after ready: ZERO jobs across all six panels, each swept both ways");
        ok (h.allocsInProcess == 0, "process() allocated nothing through it");
    }

    group ("under a cap, a switch to a panel outside the set runs no more jobs than the player without the preload");
    {
        Host pre (48000.0, true, 2, sixPanels(), 1), plain (48000.0, false, 2, sixPanels());
        felitronics::test::run (pre.ready());
        std::vector<float> a, b;
        int atA = 0, atB = 0;
        const auto rest = [] (int) { return -1.0; };
        pre.play (atA, 40, 48000.0, rest, a);
        plain.play (atB, 40, 48000.0, rest, b);
        const long long preBefore = pre.jobs, plainBefore = plain.jobs;
        for (Host* h : { &pre, &plain })
            felitronics::test::run (h->p.setSwitch ("Boost", "on") && h->p.setSwitch ("Voice", "c"));
        ok (! pre.p.preloadProgress().ready(), "the set is made around the new panel: not ready");
        pre.play (atA, 40, 48000.0, rest, a);
        plain.play (atB, 40, 48000.0, rest, b);
        std::printf ("    jobs at rest after the switch: %lld with the preload (cap 1), %lld without\n", pre.jobs - preBefore,
                     plain.jobs - plainBefore);
        ok (pre.jobs - preBefore <= plain.jobs - plainBefore, "at rest: no more jobs than without the preload (old: the whole panel)");
        pre.play (atA, 61, 48000.0, [] (int k) { return 5.0 * k; }, a);
        plain.play (atB, 61, 48000.0, [] (int k) { return 5.0 * k; }, b);
        std::printf ("    …and with a sweep after it: %lld with the preload, %lld without\n", pre.jobs - preBefore,
                     plain.jobs - plainBefore);
        ok (pre.jobs - preBefore <= plain.jobs - plainBefore, "with the sweep too");
        ok (same (a, b), "bit for bit with it");
        felitronics::test::run (pre.p.setSwitch ("Boost", "off") && pre.p.setSwitch ("Voice", "b"));
        const long long idle0 = pre.jobs;
        pre.pump();
        ok (pre.jobs == idle0 && ! pre.p.preloadProgress().ready(), "another panel outside the set: no job while the law asks none");
        pre.p.setPreloadIdle (true);
        ok (pre.ready() && pre.jobs > idle0, "setPreloadIdle: the copies the set lacks are handed out again, and it is ready");
    }

    group ("a pack with no gain dial: a switch makes the set again, within the cap (it used to read every panel the same)");
    {
        Host h (48000.0, true, 2, switchOnly(), 1);
        felitronics::test::run (h.ready());
        const int panel = h.p.preloadProgress().total;
        std::printf ("    one panel: %d cop%s\n", panel, panel == 1 ? "y" : "ies");
        std::vector<float> out;
        int at = 0, most = 0;
        bool notReady = true, landed = true;
        for (const char* v : { "b", "c", "a", "b", "c", "a" })
        {
            felitronics::test::run (h.p.setSwitch ("Voice", v));
            const auto pr = h.p.preloadProgress();
            most = std::max (most, pr.total);
            notReady = notReady && ! pr.ready();
            h.play (at, 20, 48000.0, [] (int) { return -1.0; }, out);
            landed = landed && h.p.preloadProgress().ready();
            most = std::max (most, h.p.preloadProgress().total);
        }
        std::printf ("    the set at most %d copies across six switches\n", most);
        ok (panel >= 1 && most <= std::max (1, panel), "the set stays within max(cap, panel) (old: 2 -> 4 -> 6)");
        ok (notReady, "after each switch the new panel reads not ready…");
        ok (landed, "…until its copy is prepared");
    }

    group ("no preload job before prepare(): there are no numbers to prepare a copy at");
    {
        RigPlayer p;
        p.setPreload (true);
        const auto pack = fc_rig_fixture::pack();
        felitronics::test::run (p.load (namz::rig::loadRigManifest (pack.manifest, nullptr),
                                        [&pack] (const std::string& id) { return pack.files.at (id); }));
        const auto early = p.takeLoadJob();
        ok (! early.has_value(), "loaded, not prepared: no job (old: one at maxBlock 0)");
        if (early) p.deliver (RigPlayer::run (*early));
        felitronics::test::run (p.prepare (48000.0, kBlock, 1));
        p.service();
        const auto job = p.takeLoadJob();
        ok (job.has_value() && job->maxBlock == kBlock, "prepared: the jobs come, at the prepared block");
    }

    group ("a dense Linear (2001 taps, NAM's partitioned FFT): the residue the docs state, pinned");
    {
        Host pre (48000.0, true, 2), plain (48000.0, false, 2);
        for (Host* h : { &pre, &plain }) h->pack.files["g150.namz"] = fc_rig_fixture::packed (denseLinear (2001));
        // the packs were loaded with the source reading h->pack: the bytes are fetched at the first job
        felitronics::test::run (pre.ready());
        std::vector<float> a, b;
        path ([&] (int k, double deg, const std::string& what) {
            drive (pre, 48000.0, k, deg, what, a);
            drive (plain, 48000.0, k, deg, what, b);
        });
        const double d = maxDiff (a, b);
        std::printf ("    max |diff| %.3g (a copy taken again keeps NAM's FFT clock; a fresh load starts it at zero)\n", d);
        ok (d <= 1e-6, "within 1e-6 of the player without the preload");
    }

    group ("re-prepared at another rate after ready, with copies in the slots: no job, and the sound of the plain player");
    {
        Host pre (48000.0, true, 2), plain (48000.0, false, 2);
        felitronics::test::run (pre.ready());
        std::vector<float> a, b;
        int atA = 0, atB = 0;
        const auto up = [] (int k) { return 2.0 * k; };
        pre.play (atA, 120, 48000.0, up, a);
        plain.play (atB, 120, 48000.0, up, b);
        const long long jobs0 = pre.jobs;
        for (Host* h : { &pre, &plain }) felitronics::test::run (h->p.prepare (44100.0, kBlock, 1));
        a.clear(); b.clear();
        const auto down = [] (int k) { return std::max (0.0, 240.0 - 2.0 * k); };
        pre.play (atA, 160, 44100.0, down, a);
        plain.play (atB, 160, 44100.0, down, b);
        const double d = maxDiff (a, b);
        bool finite = true;
        for (const float v : a) finite = finite && std::isfinite (v);
        std::printf ("    after the re-prepare: %lld jobs, max |diff| %.3g against the plain player\n", pre.jobs - jobs0, d);
        ok (pre.jobs == jobs0, "no job: the copies were re-prepared in place");
        ok (finite && d < 1e-4, "within 1e-4 of the plain player at 44.1 kHz");
    }

    group ("reset() with a copy in a slot, and stand-by and back with the preload on: as the plain player");
    {
        Host pre (48000.0, true, 2), plain (48000.0, false, 2);
        felitronics::test::run (pre.ready());
        std::vector<float> a, b;
        int atA = 0, atB = 0;
        const auto up = [] (int k) { return 3.0 * k; };
        pre.play (atA, 70, 48000.0, up, a);
        plain.play (atB, 70, 48000.0, up, b);
        const long long jobs0 = pre.jobs;
        pre.p.reset();
        plain.p.reset();
        const auto down = [] (int k) { return std::max (0.0, 210.0 - 3.0 * k); };
        pre.play (atA, 80, 48000.0, down, a);
        plain.play (atB, 80, 48000.0, down, b);
        ok (same (a, b), "reset(): bit for bit with the plain player, before and after");
        for (Host* h : { &pre, &plain }) h->p.setStandby (true);
        pre.play (atA, 200, 48000.0, up, a);
        plain.play (atB, 200, 48000.0, up, b);
        for (Host* h : { &pre, &plain }) h->p.setStandby (false);
        pre.play (atA, 150, 48000.0, down, a);
        plain.play (atB, 150, 48000.0, down, b);
        std::printf ("    max |diff| %.3g over reset, stand-by and back; jobs %lld\n", maxDiff (a, b), pre.jobs - jobs0);
        ok (same (a, b), "stand-by and back: bit for bit with the plain player");
        ok (pre.jobs == jobs0 && pre.allocsInProcess == 0, "no job and no allocation in process() through either");
    }

    return felitronics::test::report();
}
