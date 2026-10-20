// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-core — see LICENSE.
//
// THE LOAD, v0.7.0: the direct build's gate, the mono build, and the load that leaves the warm-up to its caller.
//
//   felitronics_nam_load_tests [<dir of *.orbitrig>...]
//
// 1. THE GATE (DirectBuild.h): every architecture fixture NeuralAmpModelerCore ships (its example_models), this
//    suite's own Linears, and — when directories are named — every .namz of every real pack in them, packed and
//    built both ways: the direct build from namz::decode against ::nam::get_dsp of namz::unpack's JSON, which is
//    v0.6.0's path. An architecture DirectBuild calls direct must give the same samples, BIT FOR BIT, and the same
//    tags; one it does not call direct is listed as the JSON path's. A control moves one weight of each direct
//    fixture by one ulp and must see the samples move — so "identical" is a verdict the comparison can refuse.
// 2. MONO: a load for one channel builds one network, refuses a stereo call, and grows its second network when the
//    host re-prepares for two — after which it plays as a load made for two from the start.
// 3. WARM::CALLER: no NAM zero prewarm in the load — visible in the first samples of a WaveNet, gone once live input
//    covered the receptive field (bit-identical from there); and kept for a recurrent capture. That the first
//    process() after such a load still allocates nothing is felitronics_rigplayer_rigplayerrtalloc_tests' to say:
//    RigPlayer is where Warm::Caller is used, and that suite owns the allocation counter (A2 as a .namz, since review).
//    At the model's rate and at a 44.1 kHz host, with RigPlayer::warmFor's terms.
// 4. RECURRENT: an LSTM gets both networks whatever the host, so mono -> stereo plays L == R (through prepare and install).
// 5. NOT FINITE: a .namz with a NaN or infinite weight is refused, as v0.6.0's JSON path refused it.
// 6. FLOAT16 (namz 4.4.0): a .nam16z — its weights stored as IEEE-754 halves — plays BIT FOR BIT what the float32
//    .namz of those halves widened plays, on every fixture of 1 (example models, Linears, every real capture named):
//    namz::decode and namz::unpack widen each half exactly, and the stage builds from those floats down either path.
//    The widened file is namz's own round trip (the .nam16z unpacked, packed again as float32), not this suite's
//    arithmetic. A control per fixture: the float32 original plays other samples — the halves really are halves.
// Also in 2: loadModelFromMemory builds for the stage's channels; a mono model from shared bytes holds THEM, not a copy.
#include <felitronics/nam/NamStage.h>
#include <felitronics_test.h>

#include "../src/DirectBuild.h"

#include <NAM/dsp.h>
#include <NAM/get_dsp.h>

#include <namz.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using felitronics::nam::NamStage;
using felitronics::test::ok;

namespace
{
constexpr double kFs = 48000.0;
constexpr int    kBlock = 256;

bool expect (bool c, const std::string& msg) { ok (c, msg); return c; }   // ok(), answering its verdict

std::vector<std::uint8_t> readFile (const fs::path& p)
{
    std::ifstream in (p, std::ios::binary);
    return { std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>() };
}
std::vector<std::uint8_t> bytesOf (const std::string& s) { return { s.begin(), s.end() }; }

// A guitar-ish test signal: decaying harmonics over a little deterministic noise (one second).
std::vector<float> signal (int n)
{
    std::vector<float> x ((std::size_t) n);
    std::uint32_t s = 12345u;
    for (int i = 0; i < n; ++i)
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const double t = (double) (i % 12000) / kFs;
        double v = 0.0;
        for (int h = 1; h <= 6; ++h) v += std::sin (2.0 * 3.14159265358979323846 * 110.0 * h * t) / h;
        x[(std::size_t) i] = (float) (0.25 * v * std::exp (-3.0 * t) + 0.02 * ((double) s / 4294967296.0 - 0.5));
    }
    return x;
}

std::vector<float> render (::nam::DSP& m, const std::vector<float>& x)
{
    m.Reset (kFs, kBlock);                         // NAM's own prewarm, the same for both builds
    std::vector<float> y (x.size());
    std::vector<NAM_SAMPLE> in ((std::size_t) kBlock), out ((std::size_t) kBlock);
    for (std::size_t off = 0; off < x.size(); off += (std::size_t) kBlock)
    {
        const int n = (int) std::min<std::size_t> ((std::size_t) kBlock, x.size() - off);
        std::copy_n (x.data() + off, n, in.data());
        NAM_SAMPLE* i[1] { in.data() };
        NAM_SAMPLE* o[1] { out.data() };
        m.process (i, o, n);
        std::copy_n (out.data(), n, y.data() + off);
    }
    return y;
}

bool sameBits (const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
}

std::string denseLinear (int taps, const char* implementation)
{
    std::string w = "[";
    std::uint32_t state = 7u;
    char buf[32];
    for (int i = 0; i < taps; ++i)
    {
        state = state * 1664525u + 1013904223u;
        std::snprintf (buf, sizeof buf, "%.7g", ((double) (state >> 8) / 16777216.0 - 0.5) * 2.0 / std::sqrt ((double) taps));
        w += buf;
        if (i + 1 < taps) w += ',';
    }
    std::string json = R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":)" + std::to_string (taps)
                     + R"(,"bias":true)";
    if (implementation != nullptr) json += std::string (R"(,"implementation":")") + implementation + "\"";
    return json + R"(},"weights":)" + w + R"(,0.01],"sample_rate":48000,"metadata":{"loudness":-12.5}})";
}

struct Fixture { std::string name; std::vector<std::uint8_t> namz; };

struct GateCount { int direct = 0, json = 0, levels = 0; std::vector<std::string> directArch, jsonArch; };

std::string archOf (const nlohmann::json& skel)
{
    std::string a = skel.value ("architecture", std::string ("?"));
    const auto c = skel.find ("config");
    if (a == "SlimmableContainer" && c != skel.end() && c->contains ("submodels"))
    {
        a += "[";
        for (const auto& e : (*c)["submodels"]) a += e["model"].value ("architecture", std::string ("?")) + ",";
        a.back() = ']';
    }
    if (c != skel.end() && c->contains ("condition_dsp")) a += "+condition_dsp";
    return a;
}

void gateOne (const Fixture& f, const std::vector<float>& x, GateCount& count, bool control)
{
    const auto decoded = namz::decode (f.namz.data(), f.namz.size(), 64u << 20);
    if (! expect (decoded.has_value(), f.name + ": decodes")) return;
    const std::string arch = archOf (decoded->skeleton);
    if (! felitronics::nam::detail::directBuildable (decoded->skeleton))
    {
        ++count.json;
        if (std::find (count.jsonArch.begin(), count.jsonArch.end(), arch) == count.jsonArch.end()) count.jsonArch.push_back (arch);
        // …and the stage still loads it, down the JSON path (unless NAM itself refuses it at load).
        return;
    }
    ++count.direct;
    if (std::find (count.directArch.begin(), count.directArch.end(), arch) == count.directArch.end()) count.directArch.push_back (arch);
    const auto json = namz::unpack (f.namz.data(), f.namz.size(), 64u << 20);   // v0.6.0's path, verbatim
    auto viaJson   = ::nam::get_dsp (nlohmann::json::parse (json.begin(), json.end()));
    auto viaDirect = felitronics::nam::detail::buildDirect (*decoded);
    if (! expect (viaJson != nullptr && viaDirect != nullptr, f.name + ": both builds exist")) return;
    ok (viaDirect->GetExpectedSampleRate() == viaJson->GetExpectedSampleRate()
        && viaDirect->HasLoudness() == viaJson->HasLoudness()
        && (! viaJson->HasLoudness() || viaDirect->GetLoudness() == viaJson->GetLoudness())
        && viaDirect->HasInputLevel() == viaJson->HasInputLevel()
        && (! viaJson->HasInputLevel() || viaDirect->GetInputLevel() == viaJson->GetInputLevel())
        && viaDirect->HasOutputLevel() == viaJson->HasOutputLevel()
        && (! viaJson->HasOutputLevel() || viaDirect->GetOutputLevel() == viaJson->GetOutputLevel())
        && viaDirect->GetPrewarmSamples() == viaJson->GetPrewarmSamples(),
        f.name + " (" + arch + "): the same tags — rate, loudness, input and output level (values), prewarm");
    if (viaJson->HasInputLevel() || viaJson->HasOutputLevel()) ++count.levels;
    const auto a = render (*viaJson, x);
    const auto b = render (*viaDirect, x);
    ok (sameBits (a, b), f.name + " (" + arch + "): direct == get_dsp, bit for bit, over " + std::to_string (x.size()) + " samples");
    if (! control) return;
    // THE CONTROL: one weight of the segment the active model plays moved by one ulp — the comparison must see it.
    auto moved = *decoded;
    auto* seg = &moved.segments.back();
    for (auto it = moved.segments.rbegin(); it != moved.segments.rend() && seg->empty(); ++it) seg = &*it;
    if (! expect (! seg->empty(), f.name + ": a segment to move")) return;
    float& w = (*seg)[seg->size() / 2];
    w = std::nextafter (w, w >= 0.0f ? 1e9f : -1e9f);
    auto perturbed = felitronics::nam::detail::buildDirect (moved);
    ok (! sameBits (a, render (*perturbed, x)), f.name + ": control — one weight one ulp off, and the samples differ");
}

void gate (const std::vector<fs::path>& packDirs)
{
    felitronics::test::group ("the gate: the direct build against get_dsp, per architecture");
    std::vector<Fixture> fixtures;
    for (const auto& e : fs::directory_iterator (FELITRONICS_NAM_EXAMPLE_MODELS))
        if (e.path().extension() == ".nam")
        {
            const auto raw = readFile (e.path());
            fixtures.push_back ({ "example_models/" + e.path().filename().string(), namz::pack (raw.data(), raw.size()) });
        }
    for (const char* impl : { "direct", "fft", (const char*) nullptr })
    {
        const auto raw = bytesOf (denseLinear (impl != nullptr && std::string (impl) == "fft" ? 2001 : 97, impl));
        fixtures.push_back ({ std::string ("Linear/") + (impl != nullptr ? impl : "auto"), namz::pack (raw.data(), raw.size()) });
    }
    ok (fixtures.size() >= 12, "NAM's example models and the Linears are all here (" + std::to_string (fixtures.size()) + ")");
    const auto x = signal ((int) kFs);
    GateCount count;
    for (const auto& f : fixtures) gateOne (f, x, count, true);
    int real = 0;
    for (const auto& dir : packDirs)
        for (const auto& pack : fs::directory_iterator (dir))
        {
            if (pack.path().extension() != ".orbitrig") continue;
            int k = 0;
            for (const auto& e : fs::directory_iterator (pack.path()))
                if (e.path().extension() == ".namz")
                {
                    gateOne ({ pack.path().filename().string() + "/" + e.path().filename().string(), readFile (e.path()) },
                             x, count, k++ == 0);   // the control once per pack
                    ++real;
                }
        }
    std::string d, j;
    for (const auto& s : count.directArch) d += " " + s;
    for (const auto& s : count.jsonArch) j += " " + s;
    std::printf ("    direct (%d files):%s\n    JSON path (%d files):%s\n    real pack files: %d\n",
                 count.direct, d.c_str(), count.json, j.c_str(), real);
    // What goes direct is exactly what the header promises, and the rest is what it says stays JSON.
    ok (count.directArch.size() >= 3, "Linear, WaveNet and SlimmableContainer[WaveNet,WaveNet] went direct");
    ok (count.levels > 0, "precondition: level tags were compared on " + std::to_string (count.levels) + " direct files");
    for (const auto& s : count.jsonArch)
        ok (s.find ("LSTM") != std::string::npos || s.find ("ConvNet") != std::string::npos
            || s.find ("condition_dsp") != std::string::npos, "the JSON path holds only what is not proven: " + s);
    // ConvNet has no fixture with weights here: its routing is asked of the shape alone.
    const auto conv = nlohmann::json::parse (R"({"version":"0.5.0","architecture":"ConvNet","config":{"channels":2,"dilations":[1],"batchnorm":false,"activation":"Tanh"},"weights":0})");
    ok (! felitronics::nam::detail::directBuildable (conv), "a ConvNet takes the JSON path");
    const auto nested = nlohmann::json::parse (R"({"version":"0.5.0","architecture":"SlimmableContainer","config":{"submodels":[{"max_value":1.0,"model":{"version":"0.5.0","architecture":"SlimmableContainer","config":{"submodels":[]},"weights":0}}]},"weights":0})");
    ok (! felitronics::nam::detail::directBuildable (nested), "a container inside a container takes the JSON path");
}

// ---- 2. mono --------------------------------------------------------------------------------------------------------
std::vector<std::uint8_t> exampleNamz (const char* name)
{
    const auto raw = readFile (fs::path (FELITRONICS_NAM_EXAMPLE_MODELS) / name);
    return namz::pack (raw.data(), raw.size());
}

std::vector<float> play (NamStage& st, const std::vector<float>& x, int channels, bool* refused = nullptr)
{
    std::vector<float> l (x), r (x);
    bool all = true;
    for (std::size_t off = 0; off < x.size(); off += (std::size_t) kBlock)
    {
        float* io[2] { l.data() + off, r.data() + off };
        all = st.process (io, channels, (int) std::min<std::size_t> ((std::size_t) kBlock, x.size() - off), true) && all;
    }
    if (refused != nullptr) *refused = ! all;
    if (channels == 2) l.insert (l.end(), r.begin(), r.end());
    return l;
}

void mono()
{
    felitronics::test::group ("mono: one network per load on a mono host, the second one built when the host goes stereo");
    const auto a2 = exampleNamz ("A2.nam");
    const auto x = signal (24000);
    auto m1 = NamStage::prepareModel (a2.data(), a2.size(), kFs, kBlock, 0.0f, 1);
    auto m2 = NamStage::prepareModel (a2.data(), a2.size(), kFs, kBlock, 0.0f, 2);
    ok (NamStage::networksOf (m1) == 1 && NamStage::networksOf (m2) == 2,
        "a mono load builds ONE network, a stereo load two (" + std::to_string (NamStage::networksOf (m1)) + ", "
        + std::to_string (NamStage::networksOf (m2)) + ")");
    ok (NamStage::networksOf (NamStage::prepareModel (a2.data(), a2.size(), kFs, kBlock)) == 2,
        "the default is the stereo build, as before");

    NamStage monoStage, ref;
    monoStage.prepare (kFs, kBlock, 1);
    ok (monoStage.install (std::move (m1)), "the mono build installs into a mono stage");
    bool refused = false;
    const auto y1 = play (monoStage, x, 1, &refused);
    ok (! refused, "…and plays one channel");
    (void) play (monoStage, x, 2, &refused);
    ok (refused, "…and refuses a two-channel call it has no second network for");
    monoStage.prepare (kFs, kBlock, 2);                       // the host goes stereo: the second network is built here
    const auto y2 = play (monoStage, x, 2, &refused);
    ref.prepare (kFs, kBlock, 2);
    ok (ref.install (std::move (m2)), "a stereo build installs into a stereo stage");
    const auto r2 = play (ref, x, 2);
    ok (! refused && sameBits (y2, r2), "re-prepared for two channels, it plays both — as a load made for two, bit for bit");
    ok (sameBits ({ y1.begin(), y1.end() }, { r2.begin(), r2.begin() + (std::ptrdiff_t) x.size() }),
        "…and its mono channel played what the stereo load's left channel plays");

    // A mono build installed where two channels play is re-prepared by install(), not refused.
    NamStage wide;
    wide.prepare (kFs, kBlock);
    ok (wide.install (NamStage::prepareModel (a2.data(), a2.size(), kFs, kBlock, 0.0f, 1)),
        "a mono build installed into a stereo stage gets its second network there");
    (void) play (wide, x, 2, &refused);
    ok (! refused, "…and plays two channels");

    // loadModelFromMemory builds for the stage's channel count, as prepareModel's numChannels does.
    NamStage monoLoad;
    monoLoad.prepare (kFs, kBlock, 1);
    ok (monoLoad.loadModelFromMemory (a2.data(), a2.size()) && monoLoad.liveNetworks() == 1,
        "loadModelFromMemory on a mono stage builds ONE network (" + std::to_string (monoLoad.liveNetworks()) + ")");
    monoLoad.prepare (kFs, kBlock, 2);
    ok (monoLoad.liveNetworks() == 2, "…and the host going stereo grows the second (" + std::to_string (monoLoad.liveNetworks()) + ")");

    // From shared bytes (RigPlayer's): a mono model keeps the CALLER's bytes, one more owner, not a copy of them.
    auto shared = std::make_shared<const std::vector<std::byte>> (reinterpret_cast<const std::byte*> (a2.data()),
                                                                  reinterpret_cast<const std::byte*> (a2.data()) + a2.size());
    {
        auto monoShared = NamStage::prepareModel (shared, kFs, kBlock, 0.0f, 1);
        const long one = shared.use_count();
        auto stereoShared = NamStage::prepareModel (shared, kFs, kBlock, 0.0f, 2);
        ok (monoShared != nullptr && one == 2 && shared.use_count() == 2,
            "a mono model from shared bytes holds them (use_count " + std::to_string (one) + "), a stereo one does not ("
            + std::to_string (shared.use_count()) + ")");
        NamStage grown, made;
        grown.prepare (kFs, kBlock, 1);
        ok (grown.install (std::move (monoShared)), "…it installs");
        grown.prepare (kFs, kBlock, 2);
        made.prepare (kFs, kBlock, 2);
        ok (made.install (std::move (stereoShared)), "…and so does the stereo one");
        ok (sameBits (play (grown, x, 2), play (made, x, 2)), "…and its second network, built from those bytes, plays as the stereo load");
    }
    ok (shared.use_count() == 1, "…and lets them go with the model (" + std::to_string (shared.use_count()) + ")");
}

// ---- 4. a recurrent capture keeps both networks, whatever the host ---------------------------------------------------
// NamStageTests' decaying-cell LSTM: on silence its output is 0.5*tanh(sigmoid(10)^N), N the prewarm samples so far —
// so a second prewarm on one network and not the other reads as two different numbers on L and R.
std::string lstmModel()
{
    return R"({"version":"0.5.0","architecture":"LSTM","config":{"num_layers":1,"input_size":1,"hidden_size":1},)"
           R"("weights":[0,0,0,0,0,0,0,0,0,10,0,0,0,1,1,0],"sample_rate":48000})";
}

void recurrent()
{
    felitronics::test::group ("a recurrent capture on a mono host: both networks anyway, so a host going stereo plays L == R");
    const auto lstm = lstmModel();
    const std::vector<float> silence ((std::size_t) (2 * kBlock), 0.0f);
    auto check = [&] (NamStage& st, NamStage& ref, const std::string& how)
    {
        const auto y = play (st, silence, 2), r = play (ref, silence, 2);
        const float l = y[0], rr = y[silence.size()];
        ok (std::memcmp (&l, &rr, 4) == 0 && sameBits (y, r),
            how + ": L " + std::to_string (l) + " == R " + std::to_string (rr) + ", and both the stereo load's ("
            + std::to_string (r[0]) + ")");
    };
    ok (NamStage::networksOf (NamStage::prepareModel (lstm.data(), lstm.size(), kFs, kBlock, 0.0f, 1)) == 2,
        "an LSTM prepared for a mono host has two networks");
    {   // through prepare(): mono stage, the load, then the host goes stereo — against stereo throughout
        NamStage st, ref;
        st.prepare (kFs, kBlock, 1);
        ref.prepare (kFs, kBlock, 2);
        ok (st.install (NamStage::prepareModel (lstm.data(), lstm.size(), kFs, kBlock, 0.0f, 1))
            && ref.install (NamStage::prepareModel (lstm.data(), lstm.size(), kFs, kBlock, 0.0f, 2)), "both install");
        st.prepare (kFs, kBlock, 2);
        ref.prepare (kFs, kBlock, 2);
        check (st, ref, "re-prepared from 1 to 2 channels");
    }
    {   // through install(): a mono-prepared model on a stereo stage
        NamStage st, ref;
        st.prepare (kFs, kBlock, 2);
        ref.prepare (kFs, kBlock, 2);
        ok (st.install (NamStage::prepareModel (lstm.data(), lstm.size(), kFs, kBlock, 0.0f, 1))
            && ref.install (NamStage::prepareModel (lstm.data(), lstm.size(), kFs, kBlock, 0.0f, 2)), "both install");
        check (st, ref, "a mono build installed on a stereo stage");
    }
}

// ---- 5. a weight that is not a number --------------------------------------------------------------------------------
void finite()
{
    felitronics::test::group ("a weight that is not finite is refused at load, as v0.6.0's JSON path refused it");
    const std::string json = R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":1,"bias":false,)"
                             R"("implementation":"direct"},"weights":[0.5],"sample_rate":48000})";
    namz::PackOptions plain;
    plain.shuffle = false;
    const auto good = namz::pack (json.data(), json.size(), plain);
    float last = 0.0f;
    std::memcpy (&last, good.data() + good.size() - 4, 4);
    ok (last == 0.5f, "precondition: the one weight is the file's last four bytes, unshuffled");
    ok (NamStage::prepareModel (good.data(), good.size(), kFs, kBlock, 0.0f, 1, NamStage::Warm::Caller) != nullptr,
        "the finite one loads");
    for (const float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity() })
    {
        auto z = good;
        std::memcpy (z.data() + z.size() - 4, &bad, 4);
        auto m = NamStage::prepareModel (z.data(), z.size(), kFs, kBlock, 0.0f, 1, NamStage::Warm::Caller);
        std::string heard = "nothing";
        if (m != nullptr)
        {
            NamStage st;
            st.prepare (kFs, kBlock, 1);
            (void) st.install (std::move (m));
            heard = std::to_string (play (st, signal (kBlock), 1)[10]);
        }
        ok (heard == "nothing", "a weight of " + std::to_string (bad) + ": prepareModel refuses it (played: " + heard + ")");
    }
}

// ---- 3. Warm::Caller ----------------------------------------------------------------------------------------------
void warm()
{
    felitronics::test::group ("Warm::Caller: the load skips NAM's zero prewarm, the caller's warm-up makes it irrelevant");
    const auto x = signal (24000);
    const auto bitsFrom = [] (const std::vector<float>& a, const std::vector<float>& b, int from)
    { return std::equal (a.begin() + from, a.end(), b.begin() + from, [] (float p, float q) { return std::memcmp (&p, &q, 4) == 0; }); };
    for (const double host : { kFs, 44100.0 })                // 44.1 kHz: the rate-matcher in, its latency in warmFor
    for (const char* name : { "wavenet_a1_standard.nam", "A2.nam" })
    {
        const auto z = exampleNamz (name);
        const std::string tag = std::string (name) + " at " + std::to_string ((int) host) + " Hz";
        NamStage nam, caller;
        nam.prepare (host, kBlock, 1);
        caller.prepare (host, kBlock, 1);
        ok (nam.install (NamStage::prepareModel (z.data(), z.size(), host, kBlock, 0.0f, 1, NamStage::Warm::Nam))
            && caller.install (NamStage::prepareModel (z.data(), z.size(), host, kBlock, 0.0f, 1, NamStage::Warm::Caller)),
            tag + ": both loads install");
        const auto a = play (nam, x, 1);
        const auto b = play (caller, x, 1);
        // RigPlayer::warmFor, term for term: the field at the host's rate, the rate-matcher's latency, one block.
        const double run = NamStage::rateMatch (host, nam.modelSampleRate()).modelRunSR;
        const int pre = (int) std::ceil ((double) nam.prewarmSamples() * (run > 0.0 ? host / run : 1.0));
        const int field = pre + nam.latencySamples() + kBlock;
        ok (field > kBlock && field < (int) x.size(), tag + ": a receptive field to cover (" + std::to_string (field) + ")");
        ok (! std::equal (a.begin(), a.begin() + kBlock, b.begin()),
            tag + ": the prewarm IS skipped — the first block differs from a prewarmed load's");
        ok (bitsFrom (a, b, field), tag + ": past warmFor() of live input, bit for bit the prewarmed load");
        // NO LATENCY CONTROL HERE, measured: both stages run the same rate-matcher, so its leading zeros reach both
        // networks alike and the outputs already agree from `pre + kBlock` — the latency term guards a WOKEN slot
        // against one that never slept (RigPlayer::warmFor's note), which is not this comparison.
        if (host != kFs)
            ok (nam.latencySamples() > 0 && ! bitsFrom (a, b, kBlock),
                tag + ": the rate-matcher is in (" + std::to_string (nam.latencySamples())
                + " samples) and the skipped prewarm is still heard past the first block");
    }
    // A recurrent capture keeps its prewarm: nothing finite closes an LSTM's state.
    const auto lstm = exampleNamz ("lstm.nam");
    NamStage nam, caller;
    nam.prepare (kFs, kBlock, 1);
    caller.prepare (kFs, kBlock, 1);
    ok (nam.install (NamStage::prepareModel (lstm.data(), lstm.size(), kFs, kBlock, 0.0f, 1, NamStage::Warm::Nam))
        && caller.install (NamStage::prepareModel (lstm.data(), lstm.size(), kFs, kBlock, 0.0f, 1, NamStage::Warm::Caller)),
        "lstm.nam: both loads install");
    ok (sameBits (play (nam, x, 1), play (caller, x, 1)), "lstm.nam: recurrent — prewarmed anyway, bit for bit from sample 0");
}

// ---- 6. float16 ------------------------------------------------------------------------------------------------------
// One mono load as RigPlayer makes it (Warm::Caller, one network), installed and played; nothing when it is refused.
bool playMono (const std::vector<std::uint8_t>& file, const std::vector<float>& x, std::vector<float>& y)
{
    auto m = NamStage::prepareModel (file.data(), file.size(), kFs, kBlock, 0.0f, 1, NamStage::Warm::Caller);
    if (m == nullptr) return false;
    NamStage st;
    st.prepare (kFs, kBlock, 1);
    if (! st.install (std::move (m))) return false;
    y = play (st, x, 1);
    return true;
}

void float16 (const std::vector<fs::path>& packDirs)
{
    felitronics::test::group ("float16: a .nam16z plays bit for bit as the float32 .namz of its weights widened");
    struct Source { std::string name; std::vector<std::uint8_t> json; };
    std::vector<Source> sources;
    for (const auto& e : fs::directory_iterator (FELITRONICS_NAM_EXAMPLE_MODELS))
        if (e.path().extension() == ".nam") sources.push_back ({ "example_models/" + e.path().filename().string(), readFile (e.path()) });
    for (const char* impl : { "direct", "fft", (const char*) nullptr })
        sources.push_back ({ std::string ("Linear/") + (impl != nullptr ? impl : "auto"),
                             bytesOf (denseLinear (impl != nullptr && std::string (impl) == "fft" ? 2001 : 97, impl)) });
    for (const auto& dir : packDirs)
        for (const auto& pack : fs::directory_iterator (dir))
        {
            if (pack.path().extension() != ".orbitrig") continue;
            for (const auto& e : fs::directory_iterator (pack.path()))
                if (e.path().extension() == ".namz")
                {
                    const auto z = readFile (e.path());
                    sources.push_back ({ pack.path().filename().string() + "/" + e.path().filename().string(),
                                         namz::unpack (z.data(), z.size(), 64u << 20) });
                }
        }
    const auto x = signal ((int) kFs / 2);
    int played = 0, refusedByNam = 0;
    for (const auto& s : sources)
    {
        const auto f32 = namz::pack (s.json.data(), s.json.size());
        std::vector<float> original;
        if (! playMono (f32, x, original)) { ++refusedByNam; continue; }   // NAM refuses it as float32 too: not ours
        namz::PackOptions half;
        half.dtype = namz::DtypeF16;
        const auto f16 = namz::pack (s.json.data(), s.json.size(), half);
        if (! expect (f16.size() > 6 && f16[6] == namz::DtypeF16, s.name + ": packs as float16 (dtype byte 1)")) continue;
        const auto widened = namz::unpack (f16.data(), f16.size(), 64u << 20);   // each half as its float32
        const auto wide = namz::pack (widened.data(), widened.size());
        if (! expect (wide.size() > 6 && wide[6] == namz::DtypeF32, s.name + ": its widened weights pack as float32")) continue;
        std::vector<float> a, b;
        const bool loads = playMono (f16, x, a);
        if (! expect (loads, s.name + ": the .nam16z loads (dtype 1 is not refused)")) continue;
        ok (playMono (wide, x, b) && sameBits (a, b),
            s.name + ": the .nam16z == the float32 .namz of its halves widened, bit for bit, over " + std::to_string (x.size()) + " samples");
        ok (! sameBits (a, original), s.name + ": control — the float32 original plays other samples");
        ++played;
    }
    std::printf ("    float16: %d files played both ways, %d refused by NAM as float32 too\n", played, refusedByNam);
    ok (played >= 12, "precondition: NAM's example models and the Linears went through (" + std::to_string (played) + ")");
}
} // namespace

int main (int argc, char** argv)
{
    std::vector<fs::path> packs;
    for (int i = 1; i < argc; ++i) packs.emplace_back (argv[i]);
    gate (packs);
    mono();
    recurrent();
    finite();
    warm();
    float16 (packs);
    return felitronics::test::report();
}
