// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// The crossover blend (RigPlayer::setCrossoverAmount / setCrossoverHz / setCrossoverInvert / setCrossoverLag) — a bass
// amp's clean low end, the device above it:
//   · amount 0 is the player without it, bit for bit, on NAM's A2 beside a Linear capture — and again once a turn up
//     and back has glided out and stopped;
//   · a device that is a wire sums flat (±0.1 dB, 20 Hz – 20 kHz) at 80, 150 and 250 Hz;
//   · a capture that trails by 5 or 37 samples — the way `lag_samples` delays one — still sums flat, on either capture
//     and between them; holding the clean leg back by nothing instead is not flat (the control);
//   · the clean leg is the models' mix, sample for sample, through a dial walk, a table handed in while playing, and
//     one-model dips: the output nulls against core's eq::Crossover2 run on the player without it;
//   · an inverting device notches at the crossover; setCrossoverInvert sums it flat;
//   · a step of the amount, the frequency, the polarity or the lag glides: no sample step above the settled signal's own;
//   · law 8: silence after a note is exactly silence, with no subnormal on the way;
//   · no allocation in process() with the crossover running and moving; law 11 through core's harness;
//   · INFORMATIONAL: the seam on a real capture (NAM's example models, and any capture named in
//     FELITRONICS_GUITAR_CROSSOVER_CAPTURES, ':'-separated) — the summed level's dip at 80/120/150/200 Hz.

#include <alloc_counter.h>   // installs the allocation counter
#include <felitronics_test.h>
#include <law11_call_contract.h>

#include <felitronics/eq/Crossover2.h>
#include <felitronics/rigplayer/RigPlayer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using felitronics::test::group;
using namespace felitronics::rigplayer;
namespace test = felitronics::test;
namespace core = felitronics::core;

namespace {

// The checks here carry their numbers in the message; FELITRONICS_TEST_VERBOSE=1 prints the passing ones as well.
void ok(bool cond, const std::string& msg) {
    felitronics::test::ok(cond, msg);
    if (cond && std::getenv("FELITRONICS_TEST_VERBOSE") != nullptr) std::printf("    ok: %s\n", msg.c_str());
}

constexpr int    kBlock = 128;
constexpr double kPi = 3.14159265358979323846;

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

std::string fileText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}
std::string exampleModel(const char* name) { return fileText(std::string(FELITRONICS_NAM_EXAMPLE_MODELS) + "/" + name); }

std::vector<std::byte> bytesOf(const std::string& s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return { p, p + s.size() };
}

// A capture of the pack: its file, where it sits on the gain dial, and its `lag_samples` (none = the pack has no table).
struct Cap { std::string id, model, at; std::optional<int> lag; };

namz::rig::Rig rigOf(const std::vector<Cap>& caps, bool tone = false, bool blend = false) {
    namz::rig::Rig r;
    namz::rig::Stage st; st.kind = namz::rig::StageKind::Nam; st.rawKind = "nam"; st.slot = "pedal";
    namz::rig::Control g; g.name = "gain"; g.role = namz::rig::Role::Gain; g.sweep = 300;
    for (const auto& c : caps) {
        g.values.push_back(c.at);
        namz::rig::FileEntry fe; fe.id = c.id; fe.settings = { { "gain", c.at } }; fe.lagSamples = c.lag;
        st.device.files.push_back(fe);
    }
    st.device.controls = { g };
    if (tone) {
        namz::rig::Tone treble;
        treble.name = "treble"; treble.sweep = 300; treble.placement = "post"; treble.reference = "150"; treble.defaultValue = "220";
        namz::rig::Section hs; hs.kind = namz::rig::SectionKind::HighShelf; hs.hz = 3000.0; hs.q = 0.7;
        hs.dbAtMin = -6.0; hs.dbAtMax = 6.0;
        treble.sections = { hs };
        st.tone = { treble };
    }
    if (blend) {
        namz::rig::Blend mix;
        mix.name = "mix"; mix.sweep = 300; mix.reference = "300"; mix.dryEnd = "0"; mix.defaultValue = "200";
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
    return (float) (0.3 * std::sin(2.0 * kPi * 196.0 * t) + 0.1 * std::sin(2.0 * kPi * 1317.0 * t)
                    + 0.05 * ((double) (h >> 8) / 16777216.0 - 0.5));
}
/** A bass line with nothing high in it: a click is the only thing that can put a large step in it. */
float bass(long i, double fs) {
    const double t = (double) i / fs;
    return (float) (0.4 * std::sin(2.0 * kPi * 55.0 * t) + 0.2 * std::sin(2.0 * kPi * 110.0 * t + 0.3)
                    + 0.1 * std::sin(2.0 * kPi * 220.0 * t + 1.1));
}

struct Bench {
    RigPlayer p;
    std::map<std::string, std::vector<std::byte>> files;
    double fs;
    long at = 0;
    std::vector<float> y = std::vector<float>(kBlock);

    // `pre` runs before prepare(), so a crossover set there is SNAPPED on (its glides land at prepare).
    Bench(const std::vector<Cap>& caps, double sampleRate, double dial, const std::function<void(RigPlayer&)>& pre = {},
          bool tone = false, bool blend = false) : fs(sampleRate) {
        if (pre) pre(p);
        test::run(p.prepare(fs, kBlock, 1));
        p.setNormalize(false);
        for (const auto& c : caps) files[c.id] = bytesOf(c.model);
        p.load(rigOf(caps, tone, blend), [this](const std::string& id) {
            const auto it = files.find(id);
            return it == files.end() ? std::vector<std::byte> {} : it->second;
        });
        (void) p.setDial("gain", dial);
    }
    template <typename Gen> void block(Gen gen, bool pump = true) {
        for (int i = 0; i < kBlock; ++i) y[(std::size_t) i] = gen(at + i);
        float* io[1] { y.data() };
        test::run(p.process(io, 1, kBlock));
        at += kBlock;
        if (pump) p.serviceHere();
    }
    void settle(int blocks) { for (int b = 0; b < blocks; ++b) block([this](long i) { return di(i, fs); }); }
    template <typename Gen> std::vector<float> play(int blocks, Gen gen) {
        std::vector<float> out;
        for (int b = 0; b < blocks; ++b) { block(gen); out.insert(out.end(), y.begin(), y.end()); }
        return out;
    }
};

std::function<void(RigPlayer&)> crossover(double hz, double amount, bool invert = false, int lag = 0) {
    return [=](RigPlayer& p) {
        test::run(p.setCrossoverHz(hz)); test::run(p.setCrossoverAmount(amount));
        p.setCrossoverInvert(invert); test::run(p.setCrossoverLag(lag));
    };
}

/** The player's impulse response, after everything it held has died away: 1.07 s of silence, then a unit sample. */
std::vector<float> impulseResponse(Bench& b, int n) {
    for (int k = 0; k < 400; ++k) b.block([](long) { return 0.0f; });
    const long t0 = b.at;
    std::vector<float> h;
    while ((int) h.size() < n) { b.block([t0](long i) { return i == t0 ? 1.0f : 0.0f; }); h.insert(h.end(), b.y.begin(), b.y.end()); }
    return h;
}
/** |H(f)| in dB: the z-transform of `h` on the unit circle, by a rotating phasor in double. */
double magDb(const std::vector<float>& h, double f, double fs) {
    const double w = 2.0 * kPi * f / fs, c = std::cos(w), s = std::sin(w);
    double re = 0.0, im = 0.0, pr = 1.0, pi = 0.0;
    for (const float v : h) {
        re += v * pr; im -= v * pi;
        const double nr = pr * c - pi * s; pi = pr * s + pi * c; pr = nr;
    }
    return 10.0 * std::log10(re * re + im * im);
}
std::vector<double> grid12() {   // 1/12 octave, 20 Hz – 20 kHz
    std::vector<double> f;
    for (int k = 0; 20.0 * std::pow(2.0, k / 12.0) < 20000.0; ++k) f.push_back(20.0 * std::pow(2.0, k / 12.0));
    f.push_back(20000.0);
    return f;
}
/** The widest excursion from 0 dB over the grid. */
double ripple(const std::vector<float>& h, double fs) {
    double worst = 0.0;
    for (const double f : grid12()) worst = std::max(worst, std::abs(magDb(h, f, fs)));
    return worst;
}
/** How far the response is from a bare (delayed) unit sample: the crossover is in it, not bypassed. */
double offPeak(const std::vector<float>& h) {
    std::size_t peak = 0;
    for (std::size_t i = 0; i < h.size(); ++i) if (std::abs(h[i]) > std::abs(h[peak])) peak = i;
    double e = 0.0;
    for (std::size_t i = 0; i < h.size(); ++i) if (i != peak) e += (double) h[i] * h[i];
    return std::sqrt(e);
}
std::string fmt(double v, const char* f = "%.4f") { char b[64]; std::snprintf(b, sizeof b, f, v); return b; }

// ------------------------------------------------------------------------------------------------------------------

void amountZeroIsThePlayer() {
    group("amount 0 is the player without the crossover, bit for bit — and again once a turn up has glided out");
    const std::string a2 = exampleModel("A2.nam");
    ok(! a2.empty(), "precondition: NAM's A2.nam");
    const std::vector<Cap> caps { { "a2", a2, "60", 0 }, { "lin", delayModel(3, 0.8), "240", 3 } };
    Bench plain(caps, 48000.0, 150.0, {}, true, true);                       // no setter ever called
    Bench set(caps, 48000.0, 150.0, crossover(80.0, 0.0, true, 7), true, true);   // every setting moved, amount 0
    Bench turned(caps, 48000.0, 150.0, {}, true, true);
    long same = 0, total = 0, sameAfter = 0, after = 0, during = 0;
    bool stopped = false, ranWhileUp = false;
    for (int k = 0; k < 1200; ++k) {
        if (k == 300) { test::run(turned.p.setCrossoverHz(120.0)); test::run(turned.p.setCrossoverAmount(0.8)); }
        if (k == 500) test::run(turned.p.setCrossoverAmount(0.0));
        if (k == 450) ranWhileUp = turned.p.crossoverRunning();
        plain.settle(1); set.settle(1); turned.settle(1);
        for (int i = 0; i < kBlock; ++i) {
            ++total; if (plain.y[(std::size_t) i] == set.y[(std::size_t) i]) ++same;
            if (k >= 300 && k < 500 && plain.y[(std::size_t) i] != turned.y[(std::size_t) i]) ++during;
        }
        if (k > 500 && ! turned.p.crossoverRunning()) stopped = true;
        if (stopped) for (int i = 0; i < kBlock; ++i) { ++after; if (plain.y[(std::size_t) i] == turned.y[(std::size_t) i]) ++sameAfter; }
    }
    ok(! set.p.crossoverRunning() && same == total, "frequency 80, inverted, lag 7, amount 0: " + std::to_string(same)
       + " of " + std::to_string(total) + " samples identical, and nothing runs");
    ok(ranWhileUp && during > 10000, "precondition: amount 0.8 changes the sound (" + std::to_string(during) + " samples differ)");
    ok(stopped && after > 40000 && sameAfter == after, "back to 0: stopped, then " + std::to_string(sameAfter) + " of "
       + std::to_string(after) + " samples identical to the player that never had it");
}

void wireSumsFlat() {
    group("a device that is a wire sums flat: ±0.1 dB, 20 Hz – 20 kHz, at 48 kHz");
    const std::vector<Cap> caps { { "early", gainModel(1.0), "60", 0 }, { "late", gainModel(1.0), "240", 0 } };
    for (const double hz : { 80.0, 150.0, 250.0 }) {
        Bench b(caps, 48000.0, 60.0, crossover(hz, 1.0));
        b.settle(60);
        const auto h = impulseResponse(b, 32768);
        const double r = ripple(h, 48000.0), off = offPeak(h);
        ok(r <= 0.1 && off > 0.05, "crossover at " + fmt(hz, "%.0f") + " Hz: widest excursion " + fmt(r, "%.6f")
           + " dB (the split is in it: " + fmt(off) + " off the peak)");
    }
}

void lagSumsFlat() {
    group("a capture that trails by `lag_samples` still sums flat — the clean leg is held back by what the wet carries");
    for (const int lag : { 5, 37 }) {
        const std::vector<Cap> caps { { "early", gainModel(1.0), "60", 0 }, { "late", delayModel(lag, 1.0), "240", lag } };
        double aligned = 0.0;
        for (const double dial : { 60.0, 150.0, 240.0 }) {
            Bench b(caps, 48000.0, dial, crossover(150.0, 1.0));
            b.settle(80);
            const double r = ripple(impulseResponse(b, 32768), 48000.0);
            aligned = std::max(aligned, r);
            ok(r <= 0.1, "lag " + std::to_string(lag) + ", dial at " + fmt(dial, "%.0f") + (dial == 60.0 ? " (the alignment delays it)"
               : dial == 240.0 ? " (the capture's own)" : " (both, half and half)") + ": " + fmt(r, "%.6f") + " dB");
        }
        // THE CONTROL: setCrossoverLag(-lag) cancels what the table says, so the clean leg is not held back at all.
        Bench c(caps, 48000.0, 240.0, crossover(150.0, 1.0, false, -lag));
        c.settle(80);
        const double r = ripple(impulseResponse(c, 32768), 48000.0);
        ok(r > 100.0 * aligned && r > 0.01, "control, the late capture with its clean leg on time (lag " + std::to_string(lag)
           + " unheld): " + fmt(r, "%.3f") + " dB — the test sees it" + (lag == 5 ? " (5 samples are 5.6 degrees at 150 Hz)" : ""));
    }
}

/** The player without the crossover, through core's eq::Crossover2 by hand — what the player with it must play. */
struct Reference {
    felitronics::eq::Crossover2 x;
    explicit Reference(double fs, float hz) { x.prepare(fs, 1); x.setFrequency(hz); }
    float operator()(float v) { float lo = 0.0f, hi = 0.0f; x.processSample(0, v, lo, hi); return lo + hi; }
};

void cleanLegIsTheModelsMix(int maxModels) {
    group(std::string("the clean leg is the models' mix, sample for sample — a dial walk, a table handed in while playing")
          + (maxModels == 1 ? ", one-model dips" : ", crossfades"));
    // The pack carries NO table: the mid capture trails by 20 samples, the late one by 37, and nobody has said so yet.
    // Three knots, so the walk LANDS captures in slots (a slot goes by the parity of its capture's place on the dial).
    const std::vector<Cap> caps { { "early", gainModel(1.0), "60", std::nullopt }, { "mid", delayModel(20, 1.0), "150", std::nullopt },
                                  { "late", delayModel(37, 1.0), "240", std::nullopt } };
    auto pre0 = [maxModels](RigPlayer& p) { test::run(p.setMaxModels(maxModels)); };
    auto pre1 = [maxModels](RigPlayer& p) { test::run(p.setMaxModels(maxModels)); crossover(150.0, 1.0)(p); };
    Bench off(caps, 48000.0, 240.0, pre0), on(caps, 48000.0, 240.0, pre1);
    Reference ref(48000.0, 150.0f);
    auto step = [&](double& worst) {
        off.settle(1); on.settle(1);
        for (int i = 0; i < kBlock; ++i)
            worst = std::max(worst, (double) std::abs(ref(off.y[(std::size_t) i]) - on.y[(std::size_t) i]));
    };
    double unknown = 0.0, known = 0.0;
    for (int k = 0; k < 120; ++k) { double w = 0.0; step(w); if (k >= 60) unknown = std::max(unknown, w); }
    AlignmentTable t; t.lagByFile = { { "early", 0 }, { "mid", 20 }, { "late", 37 } };
    off.p.setAlignment(t); on.p.setAlignment(t);
    for (int k = 0; k < 40; ++k) { double w = 0.0; step(w); }   // the tap's 10 ms crossfade, then the filters forget the misfit
    std::set<std::string> held;                       // what the slots held along the walk: landings, not one capture
    bool between = false;                             // …and a block that sounded both
    for (const double dial : { 200.0, 150.0, 60.0, 100.0, 240.0, 120.0, 60.0 }) {
        (void) off.p.setDial("gain", dial); (void) on.p.setDial("gain", dial);
        for (int k = 0; k < 150; ++k) {
            step(known);
            held.insert(on.p.heldFileId(0) + "|" + on.p.heldFileId(1));
            between = between || (on.p.liveMix() > 0.0f && on.p.liveMix() < 1.0f);
        }
    }
    ok(held.size() >= 3 && (between || maxModels == 1), "precondition: the walk moved the slots (" + std::to_string(held.size())
       + " pairs held)" + (maxModels == 1 ? "" : ", and sounded both"));
    ok(unknown > 0.01, "precondition: with no reading the clean leg is 37 samples early (" + fmt(unknown, "%.4f") + " off)");
    ok(known <= 1e-5 && on.p.crossoverRunning(), "the table handed in, then a walk across both captures (" + std::string(maxModels == 1
       ? "dips" : "crossfades, retimes") + "): within " + fmt(known, "%.3g") + " of eq::Crossover2 on the player without it");
}

void polarity() {
    group("an inverting device: a notch at the crossover, and setCrossoverInvert sums it flat");
    const std::vector<Cap> caps { { "early", gainModel(-1.0), "60", 0 }, { "late", gainModel(-1.0), "240", 0 } };
    Bench as(caps, 48000.0, 60.0, crossover(150.0, 1.0));
    as.settle(60);
    const auto h = impulseResponse(as, 32768);
    double deepest = 0.0;
    for (const double f : grid12()) deepest = std::min(deepest, magDb(h, f, 48000.0));
    ok(magDb(h, 150.0, 48000.0) < -40.0, "not inverted: " + fmt(magDb(h, 150.0, 48000.0), "%.1f") + " dB at 150 Hz (deepest "
       "on the 1/12-octave grid " + fmt(deepest, "%.1f") + " dB)");
    Bench inv(caps, 48000.0, 60.0, crossover(150.0, 1.0, true));
    inv.settle(60);
    const double r = ripple(impulseResponse(inv, 32768), 48000.0);
    ok(r <= 0.1, "inverted: widest excursion " + fmt(r, "%.6f") + " dB");
}

/** The largest sample step |y[n] - y[n-1]| over [from, to). */
double biggestStep(const std::vector<float>& y, std::size_t from, std::size_t to) {
    double s = 0.0;
    for (std::size_t n = std::max<std::size_t>(from, 1); n < to && n < y.size(); ++n) s = std::max(s, (double) std::abs(y[n] - y[n - 1]));
    return s;
}

void glides() {
    group("a step of the amount, the frequency, the polarity or the lag glides: no step above the settled sound's own");
    const std::vector<Cap> caps { { "early", gainModel(0.5), "60", 0 }, { "late", gainModel(0.5), "240", 0 } };
    struct Edit { const char* name; std::function<void(RigPlayer&)> before, after; bool cut; };
    const std::vector<Edit> edits {
        { "amount 0 -> 1",   crossover(150.0, 0.0), crossover(150.0, 1.0), true },
        { "amount 1 -> 0",   crossover(150.0, 1.0), crossover(150.0, 0.0), true },
        { "amount 0.3 -> 0.9", crossover(150.0, 0.3), crossover(150.0, 0.9), true },
        { "80 -> 250 Hz",    crossover(80.0, 1.0),  crossover(250.0, 1.0), false },
        { "250 -> 80 Hz",    crossover(250.0, 1.0), crossover(80.0, 1.0), false },
        { "polarity",        crossover(150.0, 1.0), crossover(150.0, 1.0, true), true },
        { "lag 0 -> 37",     crossover(150.0, 1.0), crossover(150.0, 1.0, false, 37), true },
    };
    constexpr int kEdit = 200;                       // blocks before the edit
    const std::size_t e = (std::size_t) kEdit * kBlock;
    for (const auto& ed : edits) {
        Bench b(caps, 48000.0, 60.0, ed.before), still(caps, 48000.0, 60.0, ed.after);
        std::vector<float> y, z;
        for (int k = 0; k < kEdit + 300; ++k) {
            if (k == kEdit) ed.after(b.p);
            b.block([&b](long i) { return bass(i, b.fs); });
            still.block([&still](long i) { return bass(i, still.fs); });
            y.insert(y.end(), b.y.begin(), b.y.end()); z.insert(z.end(), still.y.begin(), still.y.end());
        }
        const double settled = std::max(biggestStep(y, e - 9600, e), biggestStep(y, e + 24000, e + 33600));
        const double glide = biggestStep(y, e, e + 9600);
        std::vector<float> cut(y.begin(), y.begin() + (long) e);
        cut.insert(cut.end(), z.begin() + (long) e, z.end());
        const double hard = biggestStep(cut, e, e + 2);
        ok(glide <= 1.1 * settled && (! ed.cut || hard > 3.0 * settled), std::string(ed.name) + ": the biggest step through "
           "the glide " + fmt(glide, "%.5f") + ", settled " + fmt(settled, "%.5f") + "; a cut at the same instant " + fmt(hard, "%.5f"));
    }
}

void silenceIsExact() {
    group("law 8: silence after a note is exactly silence, with no subnormal on the way");
    const std::vector<Cap> caps { { "early", gainModel(1.0), "60", 0 }, { "late", gainModel(1.0), "240", 0 } };
    Bench b(caps, 48000.0, 60.0, crossover(80.0, 1.0));
    b.settle(200);
    long subnormal = 0, last = -1, n = 0;
    for (int k = 0; k < 750; ++k) {                  // two seconds
        b.block([](long) { return 0.0f; });
        for (const float v : b.y) { if (std::fpclassify(v) == FP_SUBNORMAL) ++subnormal; if (v != 0.0f) last = n; ++n; }
    }
    ok(b.p.crossoverRunning() && subnormal == 0 && last >= 0 && last < 48000, "running all along: the last non-zero "
       "sample at " + std::to_string(last) + ", " + std::to_string(subnormal) + " subnormal samples in 2 s of silence");
}

void noAllocation() {
    group("no allocation in process() with the crossover running, gliding, retapping and stopping — stereo, 44.1 kHz");
    const std::vector<Cap> caps { { "early", delayModel(4, 0.9), "60", 0 }, { "late", gainModel(0.7), "240", 4 } };
    std::map<std::string, std::vector<std::byte>> files;
    for (const auto& c : caps) files[c.id] = bytesOf(c.model);
    RigPlayer p;
    test::run(p.prepare(44100.0, kBlock, 2));
    p.load(rigOf(caps, true, true), [&files](const std::string& id) { return files.at(id); });
    (void) p.setDial("gain", 150.0);
    std::vector<float> l(kBlock), r(kBlock);
    long at = 0;
    auto run = [&] {
        for (int i = 0; i < kBlock; ++i) { l[(std::size_t) i] = di(at + i, 44100.0); r[(std::size_t) i] = 0.5f * l[(std::size_t) i]; }
        at += kBlock;
        float* io[2] { l.data(), r.data() };
        long long seen = 0;
        const long long before = test::alloc::count.load();
        test::run(p.process(io, 2, kBlock));
        seen = test::alloc::count.load() - before;
        return seen;
    };
    for (int k = 0; k < 120; ++k) { (void) run(); p.serviceHere(); }
    long long seen = 0;
    bool ran = false, stopped = false;
    for (int k = 0; k < 600; ++k) {
        if (k == 0)   { test::run(p.setCrossoverAmount(0.7)); }
        if (k == 50)  { test::run(p.setCrossoverHz(400.0)); p.setCrossoverInvert(true); }
        if (k == 100) { test::run(p.setCrossoverLag(11)); }
        if (k == 150) { test::run(p.setCrossoverAmount(1.0)); test::run(p.setCrossoverHz(60.0)); }
        if (k == 200) { test::run(p.setCrossoverAmount(0.0)); }
        if (k == 520) { test::run(p.setCrossoverAmount(0.4)); }
        seen += run();
        if (k == 100) ran = p.crossoverRunning();
        if (k == 500) stopped = ! p.crossoverRunning();
    }
    ok(ran && stopped, "precondition: it ran, and stopped once back at 0");
    test::okNoAlloc(seen == 0, "600 blocks allocate nothing (" + std::to_string(seen) + ")");
}

// ------------------------------------------------------------------------------------------------- law 11 ----

const std::map<std::string, std::vector<std::byte>>& law11Files() {
    static const std::map<std::string, std::vector<std::byte>> f {
        { "early", bytesOf(delayModel(3, 0.9)) }, { "late", bytesOf(gainModel(0.7)) } };
    return f;
}
bool law11Prep(RigPlayer& s) {
    if (! s.prepare(test::law11::kFs, test::law11::kMaxBlock, 2)) return false;
    s.setNormalize(false);
    const std::vector<Cap> caps { { "early", "", "60", 0 }, { "late", "", "240", 3 } };
    if (! s.load(rigOf(caps), [](const std::string& id) { return law11Files().at(id); })) return false;
    (void) s.setDial("gain", 150.0);
    (void) s.setCrossoverHz(150.0);
    (void) s.setCrossoverAmount(0.8);
    s.setCrossoverInvert(true);
    std::vector<float> l((std::size_t) test::law11::kMaxBlock), r((std::size_t) test::law11::kMaxBlock);
    for (int k = 0; k < 400; ++k) {
        for (int i = 0; i < test::law11::kMaxBlock; ++i) l[(std::size_t) i] = r[(std::size_t) i] = di(k * 64 + i, test::law11::kFs);
        float* io[2] { l.data(), r.data() };
        if (! s.process(io, 2, test::law11::kMaxBlock)) return false;
        s.serviceHere();
    }
    return s.crossoverRunning();
}

using namespace felitronics::test::law11;
ADAPT (A_RigBase, felitronics::rigplayer::RigPlayer, 2, true, true, true,
       { return law11Prep (s); },
       { return s.process (io, nch, n); },
       { (void) s; return 0.0; },
       { return s.prepare (kFs, kMaxBlock, w); });
struct A_RigCrossover : A_RigBase { static constexpr int prepareCeiling = RigPlayer::kMaxChannels; };

// ------------------------------------------------------------------------------------------ informational ----

struct Lock { double re = 0.0, im = 0.0; };
/** One frequency's complex level over a Hann window: the input's and the output's, so their ratio is the response. */
std::pair<Lock, Lock> lockIn(Bench& b, double f, double amp, int settleBlocks, int measureBlocks) {
    const double w = 2.0 * kPi * f / b.fs;
    auto gen = [&](long i) { return (float) (amp * std::sin(w * (double) i)); };
    for (int k = 0; k < settleBlocks; ++k) b.block(gen);
    Lock x, y;
    const int n = measureBlocks * kBlock;
    for (int k = 0; k < measureBlocks; ++k) {
        const long t0 = b.at;
        b.block(gen);
        for (int i = 0; i < kBlock; ++i) {
            const int m = k * kBlock + i;
            const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * (m + 0.5) / n);
            const double ph = w * (double) (t0 + i);
            const double xi = amp * std::sin(ph), yi = b.y[(std::size_t) i];
            x.re += win * xi * std::cos(ph); x.im -= win * xi * std::sin(ph);
            y.re += win * yi * std::cos(ph); y.im -= win * yi * std::sin(ph);
        }
    }
    return { x, y };
}
struct Resp { double mag = 0.0, phase = 0.0; };
Resp ratio(const std::pair<Lock, Lock>& xy) {
    const auto& [x, y] = xy;
    const double d = x.re * x.re + x.im * x.im;
    const double re = (y.re * x.re + y.im * x.im) / d, im = (y.im * x.re - y.re * x.im) / d;
    return { std::sqrt(re * re + im * im), std::atan2(im, re) };
}

void seamOnRealCaptures() {
    group("INFORMATIONAL — the seam on a real capture: the sum's dip against the two bands added in phase (not pass/fail)");
    std::vector<std::pair<std::string, std::string>> models { { "NAM A2.nam", exampleModel("A2.nam") },
                                                              { "NAM wavenet.nam", exampleModel("wavenet.nam") } };
    if (const char* env = std::getenv("FELITRONICS_GUITAR_CROSSOVER_CAPTURES")) {
        std::string list = env;
        for (std::size_t p = 0; p <= list.size();) {
            const std::size_t q = std::min(list.find(':', p), list.size());
            if (q > p) { const std::string path = list.substr(p, q - p); models.push_back({ path.substr(path.rfind('/') + 1), fileText(path) }); }
            p = q + 1;
        }
    }
    constexpr double kAmp = 0.02;                    // quiet: the device near its small-signal response
    for (const auto& [name, text] : models) {
        if (text.empty()) { std::printf("  %s: not found, skipped\n", name.c_str()); continue; }
        // How late the capture trails its DI — what setCrossoverLag is for (a single-file pack's lag_samples is 0).
        const auto probe = alignmentProbe(48000.0);
        const auto out = probeThrough(bytesOf(text), probe, 48000.0);
        const Best best = out.empty() ? Best {} : bestLag(probe, out);
        const std::vector<Cap> caps { { "cap", text, "60", 0 }, { "cap2", text, "240", 0 } };
        const int lag = std::max(0, best.lag);
        Bench wet(caps, 48000.0, 60.0, crossover(150.0, 0.0, false, lag));
        Bench sum(caps, 48000.0, 60.0, crossover(150.0, 1.0, false, lag));
        Bench inv(caps, 48000.0, 60.0, crossover(150.0, 1.0, true, lag));
        wet.settle(100); sum.settle(100); inv.settle(100);
        std::printf("  %s: trails its DI by %d samples (correlation %.3f)\n", name.c_str(), best.lag, best.corr);
        for (const double fc : { 80.0, 120.0, 150.0, 200.0 }) {
            (void) sum.p.setCrossoverHz(fc); (void) inv.p.setCrossoverHz(fc);
            double dip = 0.0, dipInv = 0.0, gainAt = 0.0, phaseAt = 0.0;
            for (int k = -4; k <= 4; ++k) {
                const double f = fc * std::pow(2.0, k / 4.0);
                const Resp W = ratio(lockIn(wet, f, kAmp, 40, 120));
                const Resp Y = ratio(lockIn(sum, f, kAmp, 40, 120));
                const Resp I = ratio(lockIn(inv, f, kAmp, 40, 120));
                const double r4 = std::pow(f / fc, 4.0), lp = 1.0 / (1.0 + r4), hp = r4 / (1.0 + r4);
                const double inPhase = lp + hp * W.mag;          // the two bands' levels, added as if they met in phase
                dip = std::min(dip, 20.0 * std::log10(Y.mag / inPhase));
                dipInv = std::min(dipInv, 20.0 * std::log10(I.mag / inPhase));
                if (k == 0) {
                    gainAt = 20.0 * std::log10(W.mag);
                    double ph = W.phase + 2.0 * kPi * f * (double) lag / 48000.0;   // against the clean leg as it is held back
                    ph = std::remainder(ph, 2.0 * kPi);
                    phaseAt = ph * 180.0 / kPi;
                }
            }
            std::printf("    crossover %3.0f Hz: device %+6.1f dB, %+7.1f deg against the clean leg at it; the sum's deepest dip "
                        "fc/2..2fc %6.2f dB as it is, %6.2f dB inverted\n", fc, gainAt, phaseAt, dip, dipInv);
        }
    }
    ok(true, "printed above");
}

} // namespace

int main() {
    std::printf("felitronics_rigplayer_crossoverblend_tests\n");
    amountZeroIsThePlayer();
    wireSumsFlat();
    lagSumsFlat();
    cleanLegIsTheModelsMix(2);
    cleanLegIsTheModelsMix(1);
    polarity();
    glides();
    silenceIsExact();
    noAllocation();
    allProperties<A_RigCrossover>();
    seamOnRealCaptures();
    return test::report();
}
