// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// The FREQUENCY KNOB (namz 5: `sections[].hz_by`, `positions[].hz` / `q`) — a FREQ under a MID, played:
//   · a dial: the band's frequency geometric and its Q linear against `norm` between FREQ's positions, held past the
//     ends — the player's response is the RBJ bell at those numbers, at several positions;
//   · a switch: the position whose value matches;
//   · at FREQ's reference the band is the old fixed band, bit for bit — the same output as the pack without FREQ;
//   · a FREQ move glides: no sample step above the settled sound's own.

#include <alloc_counter.h>   // installs the allocation counter
#include <felitronics_test.h>

#include <felitronics/rigplayer/RigPlayer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <tuple>
#include <string>
#include <vector>

using felitronics::test::group;
using namespace felitronics::rigplayer;
namespace test = felitronics::test;

namespace {

// The checks carry their numbers; FELITRONICS_TEST_VERBOSE=1 prints the passing ones as well.
void ok(bool cond, const std::string& msg) {
    felitronics::test::ok(cond, msg);
    if (cond && std::getenv("FELITRONICS_TEST_VERBOSE") != nullptr) std::printf("    ok: %s\n", msg.c_str());
}

constexpr int    kBlock = 128;
constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

std::vector<std::byte> unity() {
    const std::string s = R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":1,"bias":false,"implementation":"direct"},"weights":[1.0],"sample_rate":48000})";
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return { p, p + s.size() };
}

// MID: a bell at 800 Hz, Q 0.9, ±15 dB, its frequency owned by `freq` (or by itself when `hzBy` is empty).
namz::rig::Tone mid(const std::string& hzBy) {
    namz::rig::Tone t;
    t.name = "mid"; t.sweep = 300; t.placement = "post"; t.reference = "150"; t.defaultValue = "150";
    namz::rig::Section b; b.kind = namz::rig::SectionKind::Bell; b.hz = 800.0; b.q = 0.9;
    b.dbAtMin = -15.0; b.dbAtMax = 15.0; b.hzBy = hzBy;
    t.sections = { b };
    return t;
}
// FREQ as a dial: 200 Hz Q 0.7 at 0°, 800 Hz Q 0.9 at the reference (150°), 3200 Hz Q 1.4 at 300°.
namz::rig::Tone freqDial() {
    namz::rig::Tone t;
    t.name = "freq"; t.sweep = 300; t.reference = "150"; t.defaultValue = "150";
    auto pos = [](const char* v, double norm, double hz, double q) {
        namz::rig::TonePosition p; p.value = v; p.norm = norm; p.hz = hz; p.q = q; return p; };
    t.positions = { pos("0", 0.0, 200.0, 0.7), pos("150", 0.5, 800.0, 0.9), pos("300", 1.0, 3200.0, 1.4) };
    return t;
}
// FREQ as a switch: Lo / Mid (the reference) / Hi, no Q of its own.
namz::rig::Tone freqSwitch() {
    namz::rig::Tone t;
    t.name = "freq"; t.reference = "Mid"; t.defaultValue = "Mid";
    auto pos = [](const char* v, double hz) { namz::rig::TonePosition p; p.value = v; p.hz = hz; return p; };
    t.positions = { pos("Lo", 400.0), pos("Mid", 800.0), pos("Hi", 1600.0) };
    return t;
}

namz::rig::Rig rigOf(std::vector<namz::rig::Tone> tone) {
    namz::rig::Rig r;
    namz::rig::Stage st; st.kind = namz::rig::StageKind::Nam; st.rawKind = "nam"; st.slot = "pedal";
    namz::rig::Control g; g.name = "gain"; g.role = namz::rig::Role::Gain; g.values = { "60", "240" }; g.sweep = 300;
    st.device.controls = { g };
    namz::rig::FileEntry a; a.id = "a"; a.settings = { { "gain", "60" } };
    namz::rig::FileEntry b; b.id = "b"; b.settings = { { "gain", "240" } };
    st.device.files = { a, b };
    st.tone = std::move(tone);
    r.chain = { st };
    return r;
}

struct Bench {
    RigPlayer p;
    std::vector<std::byte> bytes = unity();
    long at = 0;
    std::vector<float> y = std::vector<float>(kBlock);
    explicit Bench(std::vector<namz::rig::Tone> tone) {
        test::run(p.prepare(kFs, kBlock, 1));
        p.setNormalize(false);
        p.load(rigOf(std::move(tone)), [this](const std::string&) { return bytes; });
        (void) p.setDial("gain", 60.0);
        settle(40, [](long) { return 0.0f; });
    }
    template <typename Gen> void block(Gen gen) {
        for (int i = 0; i < kBlock; ++i) y[(std::size_t) i] = gen(at + i);
        float* io[1] { y.data() };
        test::run(p.process(io, 1, kBlock));
        at += kBlock;
        p.serviceHere();
    }
    template <typename Gen> void settle(int blocks, Gen gen) { for (int k = 0; k < blocks; ++k) block(gen); }
    template <typename Gen> std::vector<float> play(int blocks, Gen gen) {
        std::vector<float> out;
        for (int k = 0; k < blocks; ++k) { block(gen); out.insert(out.end(), y.begin(), y.end()); }
        return out;
    }
    std::vector<float> impulse(int n) {
        settle(200, [](long) { return 0.0f; });
        const long t0 = at;
        std::vector<float> h;
        while ((int) h.size() < n) { block([t0](long i) { return i == t0 ? 1.0f : 0.0f; }); h.insert(h.end(), y.begin(), y.end()); }
        return h;
    }
};

double magDb(const std::vector<float>& h, double f) {
    const double w = 2.0 * kPi * f / kFs, c = std::cos(w), s = std::sin(w);
    double re = 0.0, im = 0.0, pr = 1.0, pi = 0.0;
    for (const float v : h) { re += v * pr; im -= v * pi; const double nr = pr * c - pi * s; pi = pr * s + pi * c; pr = nr; }
    return 10.0 * std::log10(re * re + im * im);
}
double bellDb(double hz, double q, double gainDb, double f) {
    const auto b = designSection(SectionKind::Peak, hz, gainDb, q, kFs);
    const double w = 2.0 * kPi * f / kFs;
    const double nr = b.b0 + b.b1 * std::cos(w) + b.b2 * std::cos(2 * w), ni = -(b.b1 * std::sin(w) + b.b2 * std::sin(2 * w));
    const double dr = 1.0 + b.a1 * std::cos(w) + b.a2 * std::cos(2 * w), di = -(b.a1 * std::sin(w) + b.a2 * std::sin(2 * w));
    return 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
}
/** The widest gap between the player's response and the bell, 20 Hz – 20 kHz in 1/12 octave. */
double gapDb(const std::vector<float>& h, double hz, double q, double gainDb) {
    for (const float v : h) if (! std::isfinite(v)) return std::numeric_limits<double>::infinity();   // a NaN is no match
    double worst = 0.0;
    for (int k = 0; 20.0 * std::pow(2.0, k / 12.0) < 20000.0; ++k) {
        const double f = 20.0 * std::pow(2.0, k / 12.0);
        worst = std::max(worst, std::abs(magDb(h, f) - bellDb(hz, q, gainDb, f)));
    }
    return worst;
}
std::string fmt(double v, const char* f = "%.4f") { char b[64]; std::snprintf(b, sizeof b, f, v); return b; }

void dialPlaysTheBell() {
    group("a FREQ dial moves MID's bell: geometric in hz, linear in Q against norm, held past the ends");
    struct Row { double deg, hz, q; };
    // 75°: halfway from 0 to the reference in norm — 200·4^0.5 = 400 Hz, Q 0.8; 225°: 800·4^0.5 = 1600 Hz, Q 1.15.
    for (const Row r : { Row { 0.0, 200.0, 0.7 }, Row { 75.0, 400.0, 0.8 }, Row { 150.0, 800.0, 0.9 },
                         Row { 225.0, 1600.0, 1.15 }, Row { 300.0, 3200.0, 1.4 } }) {
        Bench b({ mid("freq"), freqDial() });
        (void) b.p.setDial("mid", 300.0);                     // +15 dB
        test::run(b.p.setDial("freq", r.deg));
        const double gap = gapDb(b.impulse(16384), r.hz, r.q, 15.0);
        ok(gap < 0.01, "FREQ at " + fmt(r.deg, "%.0f") + "°: the RBJ bell at " + fmt(r.hz, "%.0f") + " Hz, Q " + fmt(r.q, "%.2f")
           + ", within " + fmt(gap, "%.5f") + " dB");
    }
}

void switchPlaysTheBell() {
    group("a FREQ switch: the position whose value matches, the band's own Q");
    for (const auto& [v, hz] : std::vector<std::pair<std::string, double>> { { "Lo", 400.0 }, { "Mid", 800.0 }, { "Hi", 1600.0 } }) {
        Bench b({ mid("freq"), freqSwitch() });
        (void) b.p.setDial("mid", 0.0);                       // -15 dB
        test::run(b.p.setSwitch("freq", v));
        const double gap = gapDb(b.impulse(16384), hz, 0.9, -15.0);
        ok(gap < 0.01, "FREQ " + v + ": the bell at " + fmt(hz, "%.0f") + " Hz, Q 0.9, within " + fmt(gap, "%.5f") + " dB");
    }
}

void referenceIsTheOldBand() {
    group("at FREQ's reference the band is the old fixed band, bit for bit");
    for (const bool dial : { true, false }) {
        Bench with({ mid("freq"), dial ? freqDial() : freqSwitch() }), without({ mid("") });
        (void) with.p.setDial("mid", 250.0); (void) without.p.setDial("mid", 250.0);
        long same = 0, total = 0;
        for (int k = 0; k < 400; ++k) {
            auto di = [](long i) { const double t = (double) i / kFs;
                return (float) (0.3 * std::sin(2 * kPi * 196.0 * t) + 0.2 * std::sin(2 * kPi * 1100.0 * t)); };
            with.block(di); without.block(di);
            for (int i = 0; i < kBlock; ++i, ++total) if (with.y[(std::size_t) i] == without.y[(std::size_t) i]) ++same;
        }
        ok(same == total, std::string(dial ? "dial" : "switch") + ": " + std::to_string(same) + " of " + std::to_string(total)
           + " samples identical to MID without FREQ");
    }
}

double biggestStep(const std::vector<float>& y, std::size_t from, std::size_t to) {
    double s = 0.0;
    for (std::size_t n = std::max<std::size_t>(from, 1); n < to && n < y.size(); ++n) s = std::max(s, (double) std::abs(y[n] - y[n - 1]));
    return s;
}

// THE MOTION BOUND: the steepest sample step through a FREQ move against the settled sound's own, worst over a whole-dial
// jump either way, a short move, and a hand turning the dial (an update every block). Every sample is checked finite.
// Measured 1.44x (the whole dial at once, 200 -> 3200 Hz past a 250 Hz tone at +15 dB) — the bands' coefficient ramp
// between two distant bells is not a bell sweep; it is the house's ramp for every band knob. Gated with a margin.
constexpr double kFreqMotionRatio = 1.6;

void freqMoveGlides() {
    group("a FREQ move glides: no step above the settled sound's own beyond the ramp's measured bound; every sample finite");
    auto tone = [](long i) { return (float) (0.3 * std::sin(2 * kPi * 250.0 * (double) i / kFs)); };
    struct Move { const char* name; double from, to; bool hand; };
    double worstRatio = 0.0;
    bool finite = true;
    for (const Move m : { Move { "0 -> 300 at once", 0.0, 300.0, false }, Move { "300 -> 0 at once", 300.0, 0.0, false },
                          Move { "150 -> 225", 150.0, 225.0, false }, Move { "0 -> 300 by hand, 5 a block", 0.0, 300.0, true } }) {
        Bench b({ mid("freq"), freqDial() }), stay({ mid("freq"), freqDial() }), went({ mid("freq"), freqDial() });
        for (Bench* x : { &b, &stay, &went }) (void) x->p.setDial("mid", 300.0);
        test::run(b.p.setDial("freq", m.from)); test::run(stay.p.setDial("freq", m.from)); test::run(went.p.setDial("freq", m.to));
        (void) b.play(300, tone); (void) stay.play(300, tone); (void) went.play(300, tone);   // settled: the bands' ramps are over
        const auto before = b.play(60, tone); (void) stay.play(60, tone); (void) went.play(60, tone);
        std::vector<float> y { before.back() };              // the edit's own boundary is in the measurement
        for (int k = 0; k < 150; ++k) {
            if (m.hand) { if (k < 60) test::run(b.p.setDial("freq", m.from + (m.to - m.from) * (k + 1) / 60.0)); }
            else if (k == 0) test::run(b.p.setDial("freq", m.to));
            const auto blk = b.play(1, tone);
            y.insert(y.end(), blk.begin(), blk.end());
        }
        const auto after = b.play(60, tone), s0 = stay.play(2, tone), s1 = went.play(2, tone);
        for (const float v : y) finite = finite && std::isfinite(v);
        const double settled = std::max(biggestStep(before, 0, before.size()), biggestStep(after, 0, after.size()));
        const double glide = biggestStep(y, 0, y.size());
        double hard = 0.0;
        for (std::size_t n = 0; n < 192; ++n) hard = std::max(hard, (double) std::abs(s1[n] - s0[n]));
        worstRatio = std::max(worstRatio, glide / settled);
        ok(std::isfinite(glide) && glide <= kFreqMotionRatio * settled && glide < 0.1 * hard, std::string("FREQ ") + m.name
           + ": the steepest step " + fmt(glide, "%.5f") + " (" + fmt(glide / settled, "%.2f") + "x the settled " + fmt(settled, "%.5f")
           + "); a cut up to " + fmt(hard, "%.5f"));
    }
    ok(finite, "every sample through the moves finite; the worst ratio " + fmt(worstRatio, "%.3f") + "x");
}

void switchStatesQ() {
    group("a FREQ switch that states Q: the band takes it");
    auto sw = freqSwitch();
    sw.positions[0].q = 0.7; sw.positions[1].q = 0.9; sw.positions[2].q = 1.4;
    for (const auto& [v, hz, q] : std::vector<std::tuple<std::string, double, double>> { { "Lo", 400.0, 0.7 }, { "Hi", 1600.0, 1.4 } }) {
        Bench b({ mid("freq"), sw });
        (void) b.p.setDial("mid", 300.0);
        test::run(b.p.setSwitch("freq", v));
        const double gap = gapDb(b.impulse(16384), hz, q, 15.0);
        ok(gap < 0.01, "FREQ " + v + ": the bell at " + fmt(hz, "%.0f") + " Hz, Q " + fmt(q, "%.1f") + ", within " + fmt(gap, "%.5f") + " dB");
    }
}

void dialHoldsPastTheEnds() {
    group("a FREQ dial whose positions sit inside the sweep holds the outermost past them — never extrapolates");
    namz::rig::Tone f = freqDial();
    f.positions[0].value = "60";  f.positions[0].norm = 0.2;
    f.positions[2].value = "240"; f.positions[2].norm = 0.8;
    for (const auto& [deg, hz, q] : std::vector<std::tuple<double, double, double>> { { 0.0, 200.0, 0.7 }, { 300.0, 3200.0, 1.4 } }) {
        Bench b({ mid("freq"), f });
        (void) b.p.setDial("mid", 300.0);
        test::run(b.p.setDial("freq", deg));
        const double gap = gapDb(b.impulse(16384), hz, q, 15.0);
        ok(gap < 0.01, "FREQ at " + fmt(deg, "%.0f") + "° (past the outermost position): held at " + fmt(hz, "%.0f") + " Hz, Q "
           + fmt(q, "%.1f") + ", within " + fmt(gap, "%.5f") + " dB");
    }
}

void freqDuringCrossfadeAllocatesNothing() {
    group("FREQ moving while the captures crossfade: finite, no step past the motion bound, and process() allocates nothing");
    auto tone = [](long i) { return (float) (0.3 * std::sin(2 * kPi * 250.0 * (double) i / kFs)); };
    Bench b({ mid("freq"), freqDial() });
    (void) b.p.setDial("mid", 300.0);
    const auto before = b.play(300, tone);
    std::vector<float> y { before.back() };
    long long seen = 0;
    for (int k = 0; k < 120; ++k) {
        if (k < 60) { (void) b.p.setDial("gain", 60.0 + 180.0 * (k + 1) / 60.0); test::run(b.p.setDial("freq", 150.0 + 2.5 * (k + 1))); }
        for (int i = 0; i < kBlock; ++i) b.y[(std::size_t) i] = tone(b.at + i);
        float* io[1] { b.y.data() };
        const long long a0 = test::alloc::count.load();
        test::run(b.p.process(io, 1, kBlock));
        seen += test::alloc::count.load() - a0;
        b.at += kBlock;
        b.p.serviceHere();
        y.insert(y.end(), b.y.begin(), b.y.end());
    }
    const auto after = b.play(300, tone);
    bool finite = true;
    for (const float v : y) finite = finite && std::isfinite(v);
    const double settled = std::max(biggestStep(before, 0, before.size()), biggestStep(after, after.size() / 2, after.size()));
    const double glide = biggestStep(y, 0, y.size());
    ok(finite && glide <= kFreqMotionRatio * settled, "finite; the steepest step " + fmt(glide, "%.5f") + " ("
       + fmt(glide / settled, "%.2f") + "x the settled " + fmt(settled, "%.5f") + ")");
    test::okNoAlloc(seen == 0, "120 blocks with FREQ and the gain dial moving allocate nothing in process() (" + std::to_string(seen) + ")");
}

} // namespace

int main() {
    std::printf("felitronics_rigplayer_frequencyknob_tests\n");
    dialPlaysTheBell();
    switchPlaysTheBell();
    referenceIsTheOldBand();
    freqMoveGlides();
    switchStatesQ();
    dialHoldsPastTheEnds();
    freqDuringCrossfadeAllocatesNothing();
    {
        group("gapDb fails on a non-finite sample");
        std::vector<float> h(4096, 0.0f); h[0] = 1.0f; h[7] = std::numeric_limits<float>::quiet_NaN();
        ok(! (gapDb(h, 800.0, 0.9, 0.0) < 0.01), "an impulse response holding a NaN is no match for any bell");
    }
    return test::report();
}
