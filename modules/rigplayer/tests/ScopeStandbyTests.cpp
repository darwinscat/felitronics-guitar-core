// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// The scope tap and stand-by (RigPlayer::scopeRead, RigPlayer::setStandby) — OrbitAmp's ScopeTap and its PARK.
//   · the tap's two points are the app's (CapturedBlock.h:543-556), against the player's own stages, bit for bit: the
//     dry is the input as handed in (undelayed, before the pack's input level and the pre tone), the wet is the
//     player's output (pre tone, models, post tone, blend knob, output level) — the stand-by fade is not in it;
//   · the alignment, the app's: the dry undelayed, the wet trailing it by a capture's own lag and by the rate-match;
//   · the RMS match in the reader (ScopeTap::read);
//   · stand-by: the fade's length both ways (BypassFade's arithmetic), the wire while stood by, the tap frozen, the
//     stages not run, no job asked, a pause that is silence, knobs taken on the way back, the input level arriving;
//   · no allocation in process() or scopeRead(); a read never torn by a concurrent writer; prepare's scope length.

#include <alloc_counter.h>   // installs the allocation counter
#include <felitronics_test.h>

#include <functional>
#include <limits>

// THE TEST SEAM in RigPlayer::scopeRead (FELITRONICS_RIGPLAYER_TEST_SCOPE_READING): called on every try, after the
// published end is taken and before a sample is copied. A test that hangs a writer on it races the whole copy — the
// worst a writer on another thread can do, here on one thread and every time (scopeOvertaken()). This binary is the
// one translation unit that sees the seam; no product defines it.
namespace {
std::function<void()>* scopeSeam = nullptr;
int scopeSeamCalls = 0;
} // namespace
#define FELITRONICS_RIGPLAYER_TEST_SCOPE_READING(player) (++scopeSeamCalls, scopeSeam != nullptr ? (*scopeSeam)() : (void) 0)

#include <felitronics/rigplayer/RigPlayer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

using felitronics::test::group;
using felitronics::test::ok;
using namespace felitronics::rigplayer;
namespace test = felitronics::test;

namespace {

constexpr int kBlock = 128;

std::string gainModel(double w) {
    char buf[256];
    std::snprintf(buf, sizeof buf,
        R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":1,"bias":false,"implementation":"direct"},"weights":[%.6f],"sample_rate":48000})", w);
    return buf;
}

std::string delayModel(int d, double w) {
    std::string s = "[";
    for (int i = 0; i < d; ++i) s += "0.0,";
    s += std::to_string(w) + "]";
    return R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":)" + std::to_string(d + 1)
         + R"(,"bias":false,"implementation":"direct"},"weights":)" + s + R"(,"sample_rate":48000})";
}

std::vector<std::byte> bytesOf(const std::string& s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return { p, p + s.size() };
}

struct Spec {
    std::string early, mid, late;   // the captures at 60, 150, 240
    double inDb = 0.0, outDb = 0.0;
    bool tone = false;              // a post high shelf "treble" and a pre curve "bass"
    bool blend = false;             // a blend knob "mix": full wet at 300 (the default), half and half at 150
};

namz::rig::Rig rigOf(const Spec& s) {
    namz::rig::Rig r;
    namz::rig::Stage st; st.kind = namz::rig::StageKind::Nam; st.rawKind = "nam"; st.slot = "pedal";
    st.inputDb = s.inDb; st.outputDb = s.outDb;
    namz::rig::Control g; g.name = "gain"; g.role = namz::rig::Role::Gain;
    g.values = { "60", "150", "240" }; g.sweep = 300;
    st.device.controls = { g };
    namz::rig::FileEntry fe; fe.id = "early"; fe.settings = { { "gain", "60" } };
    namz::rig::FileEntry fm; fm.id = "mid";   fm.settings = { { "gain", "150" } };
    namz::rig::FileEntry fl; fl.id = "late";  fl.settings = { { "gain", "240" } };
    st.device.files = { fe, fm, fl };
    if (s.tone) {
        namz::rig::Tone treble;
        treble.name = "treble"; treble.sweep = 300; treble.placement = "post"; treble.reference = "150"; treble.defaultValue = "150";
        namz::rig::Section hs; hs.kind = namz::rig::SectionKind::HighShelf; hs.hz = 3000.0; hs.q = 0.7;
        hs.dbAtMin = -6.0; hs.dbAtMax = 6.0;
        treble.sections = { hs };
        namz::rig::Tone bass;
        bass.name = "bass"; bass.sweep = 300; bass.placement = "pre"; bass.reference = "0"; bass.defaultValue = "0";
        bass.grid.fLo = 20.0; bass.grid.fHi = 20000.0; bass.grid.points = 25;
        bass.trusted.levels = 1;
        const auto grid = felitronics::lineareq::logFreqGrid(20.0, 20000.0, 25);
        namz::rig::TonePosition p0; p0.value = "0"; p0.norm = 0.0; p0.db.assign(25, 0.0);
        namz::rig::TonePosition p1; p1.value = "300"; p1.norm = 1.0;
        for (const double f : grid)
            p1.db.push_back(f <= 200.0 ? 6.0 : f >= 2000.0 ? 0.0 : 6.0 * (1.0 - std::log(f / 200.0) / std::log(10.0)));
        bass.positions = { p0, p1 };
        st.tone = { treble, bass };
    }
    if (s.blend) {
        namz::rig::Blend mix;
        mix.name = "mix"; mix.sweep = 300; mix.reference = "300"; mix.dryEnd = "0"; mix.defaultValue = "300";
        mix.dryLevelDb = -6.0;
        mix.grid.fLo = 20.0; mix.grid.fHi = 20000.0; mix.grid.points = 25;
        mix.dryDb.assign(25, 0.0);
        namz::rig::BlendPosition dry;  dry.value  = "0";   dry.norm  = 0.0; dry.dryDb  = 0.0;    dry.wetDb  = -120.0;
        namz::rig::BlendPosition half; half.value = "150"; half.norm = 0.5; half.dryDb = 0.0;    half.wetDb = 0.0;
        namz::rig::BlendPosition wet;  wet.value  = "300"; wet.norm  = 1.0; wet.dryDb  = -120.0; wet.wetDb  = 0.0;
        mix.positions = { dry, half, wet };
        st.blend = { mix };
    }
    r.chain = { st };
    return r;
}

/** A guitar-ish DI, deterministic: two partials and a little noise. */
float di(long i, double fs) {
    const double t = (double) i / fs;
    const unsigned h = (unsigned) i * 2654435761u;
    return (float) (0.3 * std::sin(2.0 * 3.14159265358979 * 196.0 * t) + 0.1 * std::sin(2.0 * 3.14159265358979 * 1317.0 * t)
                    + 0.05 * ((double) (h >> 8) / 16777216.0 - 0.5));
}

struct Bench {
    RigPlayer p;
    std::map<std::string, std::vector<std::byte>> files;
    double fs;
    long at = 0;                                 // the input's clock
    std::vector<float> x = std::vector<float>(kBlock), y = std::vector<float>(kBlock);

    Bench(const Spec& s, double sampleRate, int scope = RigPlayer::kScopeSize) : fs(sampleRate) {
        test::run(p.prepare(fs, kBlock, 1, scope));
        p.setNormalize(false);
        reload(s);
    }
    void reload(const Spec& s) {
        files = { { "early", bytesOf(s.early) }, { "mid", bytesOf(s.mid) }, { "late", bytesOf(s.late) } };
        p.load(rigOf(s), [this](const std::string& id) {
            const auto it = files.find(id);
            return it == files.end() ? std::vector<std::byte> {} : it->second;
        });
        (void) p.setDial("gain", 60.0);
    }
    /** One block: `gen(i)` for each sample of the input clock, into the player, out to `y`; the jobs pumped. */
    template <typename Gen> void block(Gen gen, bool pump = true) {
        for (int i = 0; i < kBlock; ++i) y[(std::size_t) i] = x[(std::size_t) i] = gen(at + i);
        float* io[1] { y.data() };
        test::run(p.process(io, 1, kBlock));
        at += kBlock;
        if (pump) p.serviceHere();
    }
    void settle(int blocks) { for (int b = 0; b < blocks; ++b) block([this](long i) { return di(i, fs); }); }
};

struct Frame { std::vector<float> dry, wet; int n = 0; };
Frame read(const RigPlayer& p, int n) {
    Frame f; f.dry.assign((std::size_t) n, 0.0f); f.wet.assign((std::size_t) n, 0.0f);
    f.n = p.scopeRead(f.dry.data(), f.wet.data(), n);
    return f;
}
bool same(const std::vector<float>& a, const std::vector<float>& b) { return a == b; }

/** The frame scopeRead must hand out for a raw dry and wet: ScopeTap::read's level match as RigPlayer::scopeRead spells
    it — sums in double, the same floor, one float factor — so a frame can be held to its two raw sides bit for bit. */
Frame expected(const std::vector<float>& dry, const std::vector<float>& wet) {
    Frame f; f.dry = dry; f.wet = wet; f.n = (int) dry.size();
    double dr = 0.0, wr = 0.0;
    for (std::size_t i = 0; i < dry.size(); ++i) { dr += (double) dry[i] * dry[i]; wr += (double) wet[i] * wet[i]; }
    if (wr > 1.0e-12 && dr > 1.0e-12) {
        const float k = (float) std::sqrt(dr / wr);
        for (auto& v : f.wet) v *= k;
    }
    return f;
}

/** The last `blocks` blocks of a bench, played now: what went in and what came out. */
std::pair<std::vector<float>, std::vector<float>> history(Bench& b, int blocks) {
    std::vector<float> in, out;
    for (int k = 0; k < blocks; ++k) {
        b.settle(1);
        in.insert(in.end(), b.x.begin(), b.x.end());
        out.insert(out.end(), b.y.begin(), b.y.end());
    }
    return { in, out };
}

/** juce::approximatelyEqual at its defaults (absolute FLT_MIN, relative FLT_EPSILON) — BypassFade's end test. */
bool fadeEnd(float g, float want) {
    const float d = std::abs(g - want);
    return d <= std::numeric_limits<float>::min() || d <= std::numeric_limits<float>::epsilon() * std::max(std::abs(g), std::abs(want));
}

/** BypassFade::advance and BypassFade::blend (BypassFade.h:58-108), transcribed: the gain of every sample of a block —
    or none, when the block sits at the walk's end and nothing is blended (the device alone up, the wire alone down). */
struct Walk {
    float g, per;
    std::vector<float> block(int n, bool up) {
        const float want = up ? 1.0f : 0.0f;
        if (fadeEnd(g, want)) return {};
        const float from = g;
        const float needed = std::abs(want - g) / std::max(1.0e-9f, per);
        const int ramp = std::clamp((int) std::ceil(needed), 1, n);
        const float step = per * (float) ramp;
        g = up ? std::min(want, g + step) : std::max(want, g - step);
        const float st = (g - from) / (float) ramp;
        std::vector<float> out((std::size_t) n);
        for (int i = 0; i < n; ++i) out[(std::size_t) i] = i < ramp ? from + st * (float) i : g;
        return out;
    }
};

// ---------------------------------------------------------------------------------------------------------------------

void tapPoints() {
    group("the tap's points are the app's (CapturedBlock.h:543-556): the input as handed in, the player's output");
    // CapturedBlock::process copies the block's input before the drive and before player.process (:543-544) and writes
    // the scope after it (:554-556): the dry is what the player is HANDED — undelayed, before the pack's input level and
    // the pre tone — and the wet is everything the player does. Held to both raw sides bit for bit, at 48 kHz and at
    // 44.1 kHz against 48 kHz captures (a rate-match, latencySamples() > 0), with every stage of the player in use.
    const Spec base { gainModel(0.5), gainModel(0.8), gainModel(1.5), -6.0, 4.5, true };
    for (const double fs : { 48000.0, 44100.0 }) {
        Bench b(base, fs);
        (void) b.p.setDial("treble", 240.0);
        (void) b.p.setDial("bass", 200.0);
        b.settle(120);
        const auto [in, out] = history(b, 16);
        const Frame f = read(b.p, 2048), want = expected(in, out);
        const std::string at = fs == 48000.0 ? std::string("48 kHz")
            : "44.1 kHz against 48 kHz captures (latencySamples() = " + std::to_string(b.p.latencySamples()) + ")";
        ok(f.n == 2048 && same(f.dry, in), at + ": the dry is the input as handed in — undelayed, before the pack's "
           "input level (-6 dB) and the pre tone — bit for bit");
        ok(f.n == 2048 && same(f.wet, want.wet), at + ": the wet is the player's output — pre tone, models, post tone, "
           "output level (+4.5 dB) — bit for bit (at the dry's RMS, as read)");
    }

    const auto frameOf = [](const Spec& s, const char* knob, double deg) {
        Bench b(s, 48000.0);
        if (knob != nullptr) (void) b.p.setDial(knob, deg);
        b.settle(80);
        return read(b.p, 2048);
    };
    const Frame ref = frameOf(base, nullptr, 0.0);
    ok(ref.n == 2048, "a full tap after 80 blocks (" + std::to_string(ref.n) + ")");
    const Frame post = frameOf(base, "treble", 300.0);
    ok(same(post.dry, ref.dry) && ! same(post.wet, ref.wet), "the post tone moves the wet and not the dry");
    const Frame pre = frameOf(base, "bass", 300.0);
    ok(same(pre.dry, ref.dry) && ! same(pre.wet, ref.wet), "the pre tone moves the wet and not the dry");
    Spec hotter = base; hotter.inDb = 0.0;
    ok(same(frameOf(hotter, nullptr, 0.0).dry, ref.dry),
       "the pack's input level does not reach the dry: it is the player's, and the dry is taken before the player");
    const Spec mixed { delayModel(5, 0.5), delayModel(5, 0.5), delayModel(5, 0.5), 0.0, 0.0, false, true };
    const Frame wetEnd = frameOf(mixed, nullptr, 0.0), halfway = frameOf(mixed, "mix", 150.0);
    ok(wetEnd.n == 2048 && same(halfway.dry, wetEnd.dry) && ! same(halfway.wet, wetEnd.wet),
       "the blend knob moves the wet and not the dry (half the DI beside a capture that lags 5 samples: a comb)");

    // The stand-by fade is the app's BypassFade, which blends AFTER the block has tapped (PluginProcessor.cpp:1538,
    // behind blk.process at :1516): a device fading out draws what the same device not fading draws.
    {
        Bench fading(base, 48000.0), steady(base, 48000.0);
        fading.settle(40);
        steady.settle(40);
        fading.p.setStandby(true);
        fading.settle(3);                     // 384 of the fade's 720 samples
        steady.settle(3);
        const Frame a = read(fading.p, 2048), c = read(steady.p, 2048);
        ok(a.n == 2048 && same(a.dry, c.dry) && same(a.wet, c.wet) && ! same(fading.y, steady.y),
           "a fade under way is heard and not drawn: the tap is the device's, the fade comes after it");
    }

    // The dry IS the input, sample for sample, and ScopeTap::read's level match brings a gain capture back as the dry.
    Bench b(Spec { gainModel(0.5), gainModel(0.5), gainModel(0.5), -6.0, 0.0, false }, 48000.0);
    b.settle(80);
    const Frame f = read(b.p, kBlock);
    bool dryIsIn = f.n == kBlock;
    for (int i = 0; i < kBlock && dryIsIn; ++i) dryIsIn = f.dry[(std::size_t) i] == b.x[(std::size_t) i];
    ok(dryIsIn, "the dry tap is the last block's input itself, bit for bit");
    double wd = 0.0;
    for (int i = 0; i < kBlock; ++i) wd = std::max(wd, (double) std::abs(f.wet[(std::size_t) i] - f.dry[(std::size_t) i]));
    ok(wd < 1e-6, "the wet is read at the dry's RMS — a gain capture reads back as the dry (worst " + std::to_string(wd) + ")");
}

void alignment() {
    group("aligned as the app aligns it: the dry undelayed, the wet trailing by a capture's own lag and the rate-match");
    {
        Bench b(Spec { delayModel(5, 0.5), delayModel(5, 0.5), delayModel(5, 0.5), 0.0, 0.0, false }, 48000.0);
        b.settle(80);
        const Frame f = read(b.p, 2048);
        // The level match is over windows 5 samples apart, so the wet is the dry 5 samples later times ONE factor.
        double num = 0.0, den = 0.0, worst = 0.0;
        for (int i = 5; i < 2048; ++i) { num += (double) f.wet[(std::size_t) i] * f.dry[(std::size_t) (i - 5)];
                                         den += (double) f.dry[(std::size_t) (i - 5)] * f.dry[(std::size_t) (i - 5)]; }
        const double c = num / den;
        for (int i = 5; i < 2048; ++i) worst = std::max(worst, std::abs(f.wet[(std::size_t) i] - c * f.dry[(std::size_t) (i - 5)]));
        ok(f.n == 2048 && worst < 1e-6, "48 kHz, a capture that lags 5 samples: the wet is the dry 5 samples later "
           "(one factor " + std::to_string(c) + ", worst " + std::to_string(worst) + ")");
    }
    // At a host rate other than the pack's the player's output is late by its rate-match, and the app's picture shows
    // it so: CapturedBlock.h:544 copies the dry undelayed, :556 writes the wet as the player hands it out.
    Bench b(Spec { gainModel(0.5), gainModel(0.5), gainModel(0.5), 0.0, 0.0, false }, 44100.0);
    b.settle(120);
    const int lat = b.p.latencySamples();
    const auto [in, out] = history(b, 16);
    const Frame f = read(b.p, 2048);
    ok(f.n == 2048 && lat > 0 && same(f.dry, in),
       "44.1 kHz against 48 kHz captures: the dry is the input itself, not held back by latencySamples() = " + std::to_string(lat));
    int best = -99; double top = -1.0;
    for (int lag = -8; lag <= lat + 8; ++lag) {
        double c = 0.0;
        for (int i = 8; i + lat + 8 < 2048; ++i) c += (double) f.dry[(std::size_t) i] * f.wet[(std::size_t) (i + lag)];
        if (c > top) { top = c; best = lag; }
    }
    ok(best == lat, "…and the wet trails it by exactly that: the cross-correlation peaks at lag " + std::to_string(best));
}

/** The feed (setFeedDb, the app's block IN): the dry stands behind it, the stand-by wire in front of it. */
void feed() {
    group("the feed: the dry tap behind it, the wire in front of it, a non-finite one refused");
    Bench b(Spec { gainModel(0.5), gainModel(0.5), gainModel(0.5), -6.0, 0.0, false }, 48000.0);
    ok(! b.p.setFeedDb(std::numeric_limits<double>::quiet_NaN()) && ! b.p.setFeedDb(std::numeric_limits<double>::infinity())
       && b.p.feedDb() == 0.0, "a non-finite feed is refused and changes nothing");
    ok(b.p.setFeedDb(6.0) && b.p.feedDb() == 6.0, "a finite one is taken");
    b.settle(80);
    const Frame f = read(b.p, kBlock);
    const float g = (float) std::pow(10.0, 6.0 / 20.0);
    bool dryIsFed = f.n == kBlock;
    for (int i = 0; i < kBlock && dryIsFed; ++i) dryIsFed = f.dry[(std::size_t) i] == b.x[(std::size_t) i] * g;
    ok(dryIsFed, "the dry tap is the input times the feed (+6 dB) and nothing of the pack's level (-6 dB), bit for bit");
    b.p.setStandby(true);
    b.settle(12);
    ok(b.p.standingBy() && same(b.y, b.x), "stood by under a feed: the wire is the input itself — taken before the feed");
}

void feedGlide() {
    group("a feed that glides: the dry tap and the sound ride one ramp — their ratio is the pack's levels, every sample");
    // The dry is the input times the feed's own ramp; the sound is the input times the input level's ramp (the pack's
    // level plus the feed, one gain). Both walk rampInto's law toward targets one constant apart, so in real numbers the
    // ratio of the two never moves: y / (0.5 · dry) = linOf(inDb) · linOf(outDb) through the glide, not only after it.
    const double inDb = -6.0, outDb = 3.0;
    Bench b(Spec { gainModel(0.5), gainModel(0.5), gainModel(0.5), inDb, outDb, false }, 48000.0);
    b.settle(80);
    const double want = std::pow(10.0, inDb / 20.0) * std::pow(10.0, outDb / 20.0);
    double worst = 0.0;
    long checked = 0;
    for (const double feedDb : { 9.0, -3.0, 0.0 }) {
        test::run(b.p.setFeedDb(feedDb));
        for (int k = 0; k < 12; ++k) {
            b.settle(1);
            const Frame f = read(b.p, kBlock);
            if (f.n != kBlock) { worst = 1.0; break; }
            for (int i = 0; i < kBlock; ++i) {
                const double dry = f.dry[(std::size_t) i], y = b.y[(std::size_t) i];
                if (std::abs(dry) < 1e-3) continue;
                worst = std::max(worst, std::abs(y / (0.5 * dry) / want - 1.0));
                ++checked;
            }
        }
    }
    ok(checked > 3000 && worst < 2e-5, "through three glides, " + std::to_string(checked) + " samples: y / (0.5 · dry) = "
       "linOf(-6) · linOf(+3) within " + std::to_string(worst) + " relative");
}

/** Samples from the request until `y == target(i)` holds and keeps holding, over `blocks` blocks of DC. */
int fadeLength(Bench& b, float dc, const std::function<float(long)>& target, int blocks) {
    int last = -1, n = 0;
    for (int k = 0; k < blocks; ++k) {
        b.block([dc](long) { return dc; });
        for (int i = 0; i < kBlock; ++i, ++n)
            if (b.y[(std::size_t) i] != target(n)) last = n;
    }
    return last + 1;
}

void standby() {
    group("stand-by — OrbitAmp's PARK: BypassFade's 15 ms, the wire, nothing run");
    for (const double fs : { 48000.0, 44100.0 }) {
        Bench b(Spec { gainModel(0.5), gainModel(0.5), gainModel(0.5), 0.0, 0.0, false }, fs);
        b.settle(120);
        const float dc = 0.5f;
        for (int k = 0; k < 40; ++k) b.block([dc](long) { return dc; });
        const float dev = b.y[(std::size_t) kBlock - 1];
        // BypassFade's own count: the walk is 1 / perSample samples, rounded up (BypassFade.h:36-38, :58-81).
        const float per = (float) (1000.0 / (fs * 15.0));
        const int want = (int) std::ceil(1.0f / per);
        b.p.setStandby(true);
        const int down = fadeLength(b, dc, [dc](long) { return dc; }, 12);
        ok(std::abs(down - want) <= 1, std::string(fs > 44100.0 ? "48" : "44.1") + " kHz: the fade-out lasts " + std::to_string(down)
           + " samples, BypassFade's " + std::to_string(want) + " (" + std::to_string(1000.0 * down / fs) + " ms)");
        ok(b.p.standingBy(), "…and the device stands by after it");
        b.p.setStandby(false);
        const int up = fadeLength(b, dc, [dev](long) { return dev; }, 12);
        // At 44.1 kHz the captures' rate-matcher restarts from the silence of the park and its phase rows round DC
        // differently sample by sample, so the device is not one number to land on: the fade-in is timed at 48 kHz.
        if (fs > 44100.0) ok(std::abs(up - want) <= 1, "…and the fade-in " + std::to_string(up) + " samples");
        std::printf("    %.1f kHz: fade-out %d samples, fade-in %d, BypassFade's walk %d (%.3f ms)\n", fs / 1000.0, down, up, want,
                    1000.0 * want / fs);
        ok(! b.p.standingBy(), "…and it runs again");
    }

    group("stood by: the wire, the tap frozen, nothing run, no job asked");
    Bench b(Spec { gainModel(0.5), gainModel(0.8), gainModel(1.5), -6.0, 3.0, true }, 44100.0);
    b.settle(120);
    b.p.setStandby(true);
    b.settle(24);                                  // the fade, and the drain the stages owe
    const Frame before = read(b.p, 2048);
    const auto frames = b.p.scopeFrames();
    const int lat = b.p.latencySamples();
    b.p.clearCounters();
    (void) b.p.setDial("gain", 240.0);              // a capture never loaded: the law would ask for it
    std::vector<float> in, out;
    bool asked = false;
    for (int k = 0; k < 30; ++k) {
        // Not settle(): its serviceHere() would take any job itself, before this loop could see one.
        b.block([&b](long i) { return di(i, b.fs); }, false);
        in.insert(in.end(), b.x.begin(), b.x.end());
        out.insert(out.end(), b.y.begin(), b.y.end());
        b.p.service();
        asked = asked || b.p.takeLoadJob().has_value();
    }
    bool wire = lat > 0;
    for (std::size_t i = (std::size_t) lat; i < out.size() && wire; ++i) wire = out[i] == in[i - (std::size_t) lat];
    ok(wire, "the output is the input at latencySamples() = " + std::to_string(lat) + ", unity, bit for bit");
    const Frame after = read(b.p, 2048);
    ok(b.p.scopeFrames() == frames && same(after.dry, before.dry) && same(after.wet, before.wet),
       "the tap is not written: the same frame, the same count, 30 blocks later");
    ok(b.p.runBlocks(0) == 0 && b.p.runBlocks(1) == 0, "no network runs once the drain is spent");
    ok(! asked, "the dial moved to a capture nobody holds: no job is asked while it stands by");

    group("back: knobs taken on the way, the feed and the input level arriving");
    // The app's block IN arrives on the way back (`lastInGain = inTarget`, PluginProcessor.cpp:1494-1495): the feed,
    // read off the dry tap, which stands behind it alone — undelayed, so the whole first block is compared.
    b.p.setHostInputDb(-12.0);
    test::run(b.p.setFeedDb(-6.0));
    b.p.setStandby(false);
    b.block([&b](long i) { return di(i, b.fs); }, false);
    const Frame first = read(b.p, kBlock);
    const float g = (float) std::pow(10.0, -6.0 / 20.0);
    bool arrived = first.n == kBlock;
    for (int i = 0; i < kBlock && arrived; ++i) arrived = first.dry[(std::size_t) i] == b.x[(std::size_t) i] * g;
    ok(arrived, "the feed set while stood by is applied from the first sample back, not ramped into");
    {
        // …and the input level in the sound. A gain capture at 48 kHz with nothing else in the way: the device's
        // output is the dry times one number only if no gain was still travelling through the block, and the reader's
        // level match then brings it back as the dry. Ramped from unity to -12 dB, it would drift by four times.
        Bench c(Spec { gainModel(0.5), gainModel(0.5), gainModel(0.5), 0.0, 0.0, false }, 48000.0);
        c.settle(60);
        c.p.setStandby(true);
        c.settle(12);
        c.p.setHostInputDb(-12.0);
        c.p.setStandby(false);
        c.block([&c](long i) { return di(i, c.fs); }, false);
        const Frame f = read(c.p, kBlock);
        double worst = f.n == kBlock ? 0.0 : 1.0;
        for (int i = 0; i < kBlock; ++i)
            worst = std::max(worst, (double) std::abs(f.wet[(std::size_t) i] - f.dry[(std::size_t) i]));
        ok(worst < 1e-6, "the host level set while stood by is applied from the first sample back, not ramped into "
           "(the first block's device output against its input, worst " + std::to_string(worst) + ")");
        // …and its VALUE, which the level match above erases: the device is 0.5 · (-12 dB) · x, and the output is the
        // fade-in's blend of the wire (the input itself — latency 0 at 48 kHz) and that, BypassFade's walk from zero.
        const float g12 = (float) std::pow(10.0, -12.0 / 20.0);
        Walk walk { 0.0f, (float) (1000.0 / (c.fs * 15.0)) };
        const auto w = walk.block(kBlock, true);
        double off = w.empty() ? 1.0 : 0.0;
        for (int i = 0; i < kBlock && ! w.empty(); ++i) {
            const float x = c.x[(std::size_t) i], dev = 0.5f * (x * g12);
            off = std::max(off, (double) std::abs(c.y[(std::size_t) i] - (x + (dev - x) * w[(std::size_t) i])));
        }
        ok(c.p.latencySamples() == 0 && off < 1e-6, "…the first block back is the fade from the wire to 0.5 · (-12 dB) · x "
           "(worst " + std::to_string(off) + ")");
        c.settle(10);                                    // the fade is over
        double rel = 0.0;
        for (int i = 0; i < kBlock; ++i) {
            const float x = c.x[(std::size_t) i];
            if (std::abs(x) > 1e-3f) rel = std::max(rel, std::abs((double) c.y[(std::size_t) i] / (0.5 * x * g12) - 1.0));
        }
        ok(rel < 1e-6, "…and once it has faded in, the output is 0.5 · (-12 dB) · x (worst " + std::to_string(rel) + " relative)");
    }
    b.p.serviceHere();
    bool landed = false;
    for (int k = 0; k < 200 && ! landed; ++k) {
        b.settle(1);
        landed = b.p.heldFileId(0) == "late" || b.p.heldFileId(1) == "late";
    }
    b.settle(60);
    const auto sounding = b.p.soundingLoudness();
    ok(landed && b.p.heldFileId(sounding.slot) == "late" && ! sounding.blended,
       "the dial turned while stood by lands on its capture after the return");
}

void pauseIsSilence() {
    group("a pause is silence — nothing played before PARK is heard after it");
    Bench b(Spec { delayModel(1500, 0.9), delayModel(1500, 0.9), delayModel(1500, 0.9), 0.0, 0.0, true }, 48000.0);
    (void) b.p.setDial("treble", 300.0);
    b.settle(60);
    b.p.setStandby(true);
    b.settle(20);
    b.p.setStandby(false);
    double worst = 0.0;
    for (int k = 0; k < 40; ++k) {
        b.block([](long) { return 0.0f; });
        for (const float v : b.y) worst = std::max(worst, (double) std::abs(v));
    }
    ok(worst == 0.0, "1500 samples of a capture's memory, a post tone ringing: silence in, exactly silence out after "
                     "the return (worst " + std::to_string(worst) + ")");
}

void noAllocation() {
    group("no allocation: process() through a park and back, and scopeRead()");
    Bench b(Spec { gainModel(0.5), gainModel(0.8), gainModel(1.5), -6.0, 3.0, true }, 44100.0);
    (void) b.p.setDial("gain", 100.0);   // between two captures: both run
    b.settle(160);
    std::vector<float> dry(2048), wet(2048);
    const long long before = test::alloc::count.load();
    for (int k = 0; k < 10; ++k) b.block([&b](long i) { return di(i, b.fs); }, false);
    b.p.setStandby(true);
    for (int k = 0; k < 30; ++k) { b.block([&b](long i) { return di(i, b.fs); }, false); (void) b.p.scopeRead(dry.data(), wet.data(), 2048); }
    b.p.setStandby(false);
    for (int k = 0; k < 30; ++k) { b.block([&b](long i) { return di(i, b.fs); }, false); (void) b.p.scopeRead(dry.data(), wet.data(), 2048); }
    const long long seen = test::alloc::count.load() - before;
    test::okNoAlloc(seen == 0, "70 blocks and 60 reads allocate nothing (" + std::to_string(seen) + ")");
}

void neverTorn() {
    group("a read is whole or refused — a writer on another thread never tears it");
    Bench b(Spec { gainModel(0.5), gainModel(0.5), gainModel(0.5), 0.0, 0.0, false }, 48000.0);
    b.settle(60);
    std::atomic<bool> stop { false };
    std::thread writer([&] {
        std::vector<float> y((std::size_t) kBlock);
        long n = 0;
        while (! stop.load(std::memory_order_relaxed)) {
            for (int i = 0; i < kBlock; ++i) y[(std::size_t) i] = (float) ((n + i) % 1000000);
            float* io[1] { y.data() };
            (void) b.p.process(io, 1, kBlock);
            n += kBlock;
        }
    });
    // Only once the tap holds nothing but the writer's count: the settling DI before it is not a tear.
    const std::uint64_t from = b.p.scopeFrames() + 4096;
    while (b.p.scopeFrames() < from) std::this_thread::yield();
    std::vector<float> dry(2048), wet(2048);
    long reads = 0, whole = 0, refused = 0, torn = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < until) {
        const int m = b.p.scopeRead(dry.data(), wet.data(), 2048);
        ++reads;
        if (m == 0) { ++refused; continue; }
        // Whole: the dry one run of the writer's count, and the wet the dry itself (a 0.5 gain capture, normalize
        // off, read at the dry's RMS — a factor of exactly 2), so a wet torn apart from its dry is caught too.
        bool run = wet[0] == dry[0];
        for (int i = 1; i < m && run; ++i) {
            const float d = dry[(std::size_t) i] - dry[(std::size_t) (i - 1)];
            run = (d == 1.0f || (dry[(std::size_t) i] == 0.0f && dry[(std::size_t) (i - 1)] == 999999.0f))
               && wet[(std::size_t) i] == dry[(std::size_t) i];
        }
        if (run) ++whole; else ++torn;
    }
    stop.store(true);
    writer.join();
    std::printf("    %ld reads: %ld whole, %ld refused after %d tries, %ld torn\n", reads, whole, refused,
                RigPlayer::kScopeTries, torn);
    ok(torn == 0 && whole > 0, "no torn read among " + std::to_string(reads));
}

void fadeAgainstWire() {
    group("the fade sample for sample: the device against its DELAYED wire, BypassFade's walk");
    // A twin never parked is the device itself: until the fade-out ends the parked one runs exactly as it does.
    // The wire is the input held back by latencySamples() — 44.1 kHz, so the hold is not zero and a wire that
    // forgot it is wrong on every sample of a DI that moves.
    {
        const double fs = 44100.0;
        const Spec s { gainModel(0.5), gainModel(0.8), gainModel(1.5), -3.0, 2.0, true };
        Bench b(s, fs), twin(s, fs);
        b.settle(120); twin.settle(120);
        const int lat = b.p.latencySamples();
        std::vector<float> in(b.x.begin(), b.x.end());   // the input's history, for the wire
        Walk walk { 1.0f, (float) (1000.0 / (fs * 15.0)) };
        b.p.setStandby(true);
        long bad = 0, checked = 0;
        for (int k = 0; k < 6; ++k) {
            const auto gen = [&b](long i) { return di(i, b.fs); };
            b.block(gen, false); twin.block(gen, false);
            in.insert(in.end(), b.x.begin(), b.x.end());
            const auto g = walk.block(kBlock, false);
            for (int i = 0; i < kBlock; ++i, ++checked) {
                const float w = in[in.size() - (std::size_t) kBlock + (std::size_t) i - (std::size_t) lat];
                const float t = twin.y[(std::size_t) i];
                const float want = g.empty() ? w : w + (t - w) * g[(std::size_t) i];
                if (b.y[(std::size_t) i] != want) ++bad;
            }
        }
        ok(lat > 0 && bad == 0 && walk.g == 0.0f, "44.1 kHz fade-out, " + std::to_string(checked) + " samples: y = w + (twin - w)·g "
           "exactly, w the input at latencySamples() = " + std::to_string(lat) + " (" + std::to_string(bad) + " off)");
    }
    // The fade-in: a memoryless capture comes back as the twin plays it from its first sample, so the walk up is
    // checked as exactly. 48 kHz: the rate-matcher's phase rows would restart from the park's silence otherwise.
    {
        const double fs = 48000.0;
        const Spec s { gainModel(0.5), gainModel(0.5), gainModel(0.5), -3.0, 2.0, false };
        Bench b(s, fs), twin(s, fs);
        b.settle(120); twin.settle(120);
        b.p.setStandby(true);
        for (int k = 0; k < 12; ++k) { b.settle(1); twin.settle(1); }
        Walk walk { 0.0f, (float) (1000.0 / (fs * 15.0)) };
        b.p.setStandby(false);
        long bad = 0, checked = 0;
        for (int k = 0; k < 8; ++k) {
            const auto gen = [&b](long i) { return di(i, b.fs); };
            b.block(gen, false); twin.block(gen, false);
            const auto g = walk.block(kBlock, true);
            for (int i = 0; i < kBlock; ++i, ++checked) {
                const float w = b.x[(std::size_t) i], t = twin.y[(std::size_t) i];
                const float want = g.empty() ? t : w + (t - w) * g[(std::size_t) i];
                if (b.y[(std::size_t) i] != want) ++bad;
            }
        }
        ok(b.p.latencySamples() == 0 && bad == 0 && fadeEnd(walk.g, 1.0f), "48 kHz fade-in, " + std::to_string(checked)
           + " samples: y = w + (twin - w)·g exactly (" + std::to_string(bad) + " off)");
    }
}

void fadeEndTolerance() {
    group("the fade ends where BypassFade's does — approximatelyEqual, not >= 1");
    // 48 kHz in 60-sample blocks: twelve steps of perSample·60 stop at 0.99999988, which the app calls the end and
    // stops blending at. A player that wanted 1.0 exactly blended one more sample through that gain: 0.25 + 2^-25.
    constexpr int n = 60;
    RigPlayer p;
    test::run(p.prepare(48000.0, n, 1));
    p.setNormalize(false);
    const Spec s { gainModel(0.5), gainModel(0.5), gainModel(0.5), 0.0, 0.0, false };
    const std::map<std::string, std::vector<std::byte>> files { { "early", bytesOf(s.early) }, { "mid", bytesOf(s.mid) },
                                                                { "late", bytesOf(s.late) } };
    p.load(rigOf(s), [&files](const std::string& id) {
        const auto it = files.find(id);
        return it == files.end() ? std::vector<std::byte> {} : it->second;
    });
    (void) p.setDial("gain", 60.0);
    std::vector<float> y((std::size_t) n), out;
    const auto run = [&](int blocks, bool keep) {
        for (int k = 0; k < blocks; ++k) {
            std::fill(y.begin(), y.end(), 0.5f);
            float* io[1] { y.data() };
            test::run(p.process(io, 1, n));
            p.serviceHere();
            if (keep) out.insert(out.end(), y.begin(), y.end());
        }
    };
    run(200, false);
    p.setStandby(true);
    run(40, false);
    p.setStandby(false);
    run(20, true);
    bool flat = true;
    for (std::size_t i = 720; i < out.size() && flat; ++i) flat = out[i] == 0.25f;
    char got[64];
    std::snprintf(got, sizeof got, "%.10f", (double) out[720]);
    ok(out[719] != 0.25f && flat, std::string("fade-in sample 720 and on: exactly 0.25 (") + got + ")");
}

void unloadWhileParked() {
    group("a pack swapped while it stands by: nothing held until the law has run");
    const Spec s { gainModel(0.5), gainModel(0.8), gainModel(1.5), 0.0, 0.0, false };
    Bench b(s, 48000.0);
    b.settle(120);
    const bool heldBefore = ! b.p.heldFileId(0).empty() || ! b.p.heldFileId(1).empty();
    b.p.setStandby(true);
    b.settle(24);
    b.p.unload();
    b.reload(s);
    b.settle(10);
    ok(heldBefore && b.p.standingBy() && b.p.heldFileId(0).empty() && b.p.heldFileId(1).empty(),
       "park, unload, load: heldFileId empty on both slots — not the old pack's ids naming the new pack's models");
    b.p.setStandby(false);
    bool landed = false;
    for (int k = 0; k < 200 && ! landed; ++k) {
        b.settle(1);
        landed = b.p.heldFileId(0) == "early" || b.p.heldFileId(1) == "early";
    }
    ok(landed, "…and back, the law asks for the capture and holds it");
}

void scopeOvertaken() {
    group("a writer inside the read: whole within the slack, retried past it, refused at the end");
    Bench b(Spec { gainModel(0.5), gainModel(0.5), gainModel(0.5), 0.0, 0.0, false }, 48000.0);
    b.settle(60);
    const auto count = [](long i) { return (float) (i % 1000000); };
    for (int k = 0; k < 24; ++k) b.block(count, false);   // the tap holds nothing but the count
    struct Got { Frame f; int tries = 0; };
    const auto attempt = [&](int blocks, bool everyTry) {
        bool first = true;
        std::function<void()> writer = [&] {
            if (everyTry || first) for (int k = 0; k < blocks; ++k) b.block(count, false);
            first = false;
        };
        scopeSeam = &writer;
        scopeSeamCalls = 0;
        Got g { read(b.p, 2048), 0 };
        g.tries = scopeSeamCalls;
        scopeSeam = nullptr;
        return g;
    };
    // Whole: one run of the count on the dry, and the wet the dry itself (0.5 in, read at the dry's RMS: exactly 2).
    const auto whole = [](const Frame& f) {
        bool w = f.n == 2048 && f.wet[0] == f.dry[0];
        for (int i = 1; i < f.n && w; ++i)
            w = f.dry[(std::size_t) i] - f.dry[(std::size_t) (i - 1)] == 1.0f && f.wet[(std::size_t) i] == f.dry[(std::size_t) i];
        return w;
    };
    const Got two = attempt(2, false);
    ok(whole(two.f) && two.tries == 1, "the writer two blocks on (the slack): accepted on the first try, whole ("
       + std::to_string(two.tries) + " tries)");
    const Got three = attempt(3, false);
    ok(whole(three.f) && three.tries == 2, "three blocks on, once: the copy was torn, so retried — whole ("
       + std::to_string(three.tries) + " tries)");
    const Got every = attempt(3, true);
    bool written = false;
    for (const float v : every.f.dry) written = written || v != 0.0f;
    ok(every.f.n == 0 && every.tries == RigPlayer::kScopeTries && written,
       "three blocks on, every try: 0 after kScopeTries — and the arrays are written, not a frame (the documented 0)");
}

void scopeLength() {
    group("prepare()'s scope length — refused outside 0 … kMaxScopeSize, zero is no tap");
    RigPlayer p;
    ok(! p.prepare(48000.0, kBlock, 1, -1) && ! p.prepare(48000.0, kBlock, 1, RigPlayer::kMaxScopeSize + 1),
       "-1 and kMaxScopeSize + 1 are refused");
    std::vector<float> y((std::size_t) kBlock, 0.25f);
    float* io[1] { y.data() };
    ok(! p.process(io, 1, kBlock), "…and the refusal disarms");
    std::vector<float> dry(4096), wet(4096);
    ok(p.prepare(48000.0, kBlock, 1, 0) && p.process(io, 1, kBlock) && p.scopeRead(dry.data(), wet.data(), 4096) == 0
       && p.scopeFrames() == 0 && p.scopeSize() == 0, "0: no tap — nothing read, nothing counted");
    ok(p.prepare(48000.0, kBlock, 1, 64) && p.scopeRead(dry.data(), wet.data(), 4096) == 0,
       "64: nothing written yet reads 0");
    test::run(p.process(io, 1, kBlock));
    ok(p.scopeRead(dry.data(), wet.data(), 4096) == 64 && p.scopeFrames() == (std::uint64_t) kBlock,
       "…and after a block, 64 of the 4096 asked for; the count moved by the block");
    ok(p.scopeRead(dry.data(), wet.data(), 10) == 10 && p.scopeRead(nullptr, wet.data(), 10) == 0
       && p.scopeRead(dry.data(), wet.data(), 0) == 0, "fewer when fewer are asked; none for a null array or n < 1");
}

} // namespace

int main() {
    std::printf("felitronics_rigplayer_scopestandby_tests\n");
    tapPoints();
    alignment();
    feed();
    feedGlide();
    standby();
    pauseIsSilence();
    noAllocation();
    neverTorn();
    scopeOvertaken();
    fadeAgainstWire();
    fadeEndTolerance();
    unloadWhileParked();
    scopeLength();
    return test::report();
}
