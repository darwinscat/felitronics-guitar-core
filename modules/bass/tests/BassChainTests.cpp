// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// felitronics::bass::BassChain — the bass amp's chain (moved from OrbitAmp's processor).
//
//   every stage out of the way (the cuts off, COMP's knob and the effects at 0, the gate off, the cabinet out) is a
//   wire, bit for bit, the seam copying the one signal to both planes; the stages called one by one in the documented
//   order are process(), sample for sample; CAB/DI at 0 % is the signal the cabinet was handed, bit for bit, and at
//   100 % with a unit impulse the cabinet at its bass level; the chain at work plays the same however the host cuts its
//   blocks (within the cuts' ulp off their cadence); the gate at the END mutes what the chain added; the knobs reach
//   their stages as the app wires them (the inner cuts at a third of the HPF, the cuts' places, the tone's bass
//   overrides); and process() allocates nothing.
//
//   A HOST WITH PICTURES AND GAPS: the stages' parts (OCTAVE alone and its cut, ENVELOPE a piece at a time from a place
//   in the key, the cut after the AMP and the END mute apart, CAB through the host's own cabinet) play what the stages
//   play, bit for bit; a gap is its parts (the cuts over silence, the key taken, the effects handed no channels), bit for
//   bit, and allocates nothing; the effects' tells are counted, CHORUS answers whether it ran, the cabinet whether it
//   was in the path.

#include <felitronics_test.h>
#include <alloc_counter.h>
#include <felitronics/bass/BassChain.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace felitronics;
using bass::BassChain;
using bass::Settings;

namespace
{
    constexpr double fs = 48000.0;

    /** A bass phrase: plucks on E1, A1 and D2 with their harmonics, then silence. */
    std::vector<float> phrase (int n)
    {
        std::vector<float> x ((size_t) n, 0.0f);
        const double notes[] { 41.2034, 55.0, 73.4162 };
        for (int k = 0; k < 3; ++k)
        {
            const int from = (int) (k * 0.4 * fs);
            for (int i = 0; i < (int) (0.35 * fs) && from + i < n; ++i)
            {
                const double t = (double) i / fs;
                double v = 0.0;
                for (int h = 1; h <= 6; ++h)
                    v += std::sin (2.0 * core::kPi * h * notes[k] * t) * std::exp (-t * (2.0 + h)) / h;
                x[(size_t) (from + i)] = (float) (0.4 * v);
            }
        }
        return x;
    }

    /** Every stage out of the way. */
    Settings wire()
    {
        Settings s;
        s.hpfOn = s.lpfOn = false;
        s.compBlend = 0.0f;
        s.cabWorks = false;
        return s;
    }

    /** Every stage at work. */
    Settings working()
    {
        Settings s;
        s.gateWorks = true;
        s.gateThresholdDb = -62.0f;
        s.compBlend = 1.0f;
        s.compThresholdDb = -32.0f;
        s.octave = 0.5f;
        s.envelope = 0.6f;
        s.chorus = 0.5f;
        s.cabMix = 60.0f;
        s.hpf = 1;
        s.lpfOn = true;
        s.lpf = 1;
        return s;
    }

    std::vector<float> cabinetShot()
    {
        std::vector<float> ir (4800);
        for (size_t i = 0; i < ir.size(); ++i)
        {
            const double t = (double) i / fs;
            ir[i] = (float) (std::exp (-t * 50.0) * std::cos (2.0 * core::kPi * 90.0 * t) + (i == 0 ? 0.4 : 0.0));
        }
        return ir;
    }

    struct Out { std::vector<float> l, r; };

    /** `x` through a fresh chain in host blocks of `block`, a plain gain of 2 as the AMP; its cabinet `ir` (none: empty). */
    Out run (const std::vector<float>& x, int block, const Settings& s, const std::vector<float>& ir = {}, int warm = 48000)
    {
        BassChain chain;
        (void) chain.prepare (fs, 512, 2, s);
        if (! ir.empty())
            (void) chain.cabinet().load (ir.data(), (int) ir.size(), fs);
        std::vector<float> l (512), r (512);
        const auto amp = [] (float* mono, int n) { for (int i = 0; i < n; ++i) mono[i] *= 2.0f; };
        for (int done = 0; done < warm;)   // the cabinet's crossfade, the stages' doors: settled the same on every block size
        {
            const int n = std::min (block, warm - done);
            std::fill_n (l.data(), n, 0.0f);
            chain.process (l.data(), r.data(), n, s, amp);
            chain.cabinet().flushPending();
            done += n;
        }
        Out o;
        for (size_t at = 0; at < x.size(); at += (size_t) block)
        {
            const int n = (int) std::min ((size_t) block, x.size() - at);
            std::memcpy (l.data(), x.data() + at, sizeof (float) * (size_t) n);
            chain.process (l.data(), r.data(), n, s, amp);
            o.l.insert (o.l.end(), l.begin(), l.begin() + n);
            o.r.insert (o.r.end(), r.begin(), r.begin() + n);
        }
        return o;
    }

    double worst (const std::vector<float>& a, const std::vector<float>& b)
    {
        double w = a.size() == b.size() ? 0.0 : 1.0e30;
        for (size_t i = 0; i < std::min (a.size(), b.size()); ++i)
            w = std::max (w, std::abs ((double) a[i] - (double) b[i]));
        return w;
    }
}

int main()
{
    std::printf ("felitronics::bass BassChain tests\n");
    const auto x = phrase ((int) (1.6 * fs));

    test::group ("every stage out of the way: a wire, the seam copying the one signal");
    {
        const auto o = run (x, 128, wire());
        bool same = true;
        for (size_t i = 0; i < x.size(); ++i)
            same = same && core::exactlyEqual (o.l[i], 2.0f * x[i]) && core::exactlyEqual (o.r[i], o.l[i]);
        test::ok (same, "both planes: the AMP's doubling of the input, bit for bit");
    }

    test::group ("the stages one by one are process()");
    {
        const auto s = working();
        const auto ir = cabinetShot();
        BassChain a, b;
        (void) a.prepare (fs, 256, 2, s);
        (void) b.prepare (fs, 256, 2, s);
        (void) a.cabinet().load (ir.data(), (int) ir.size(), fs);
        (void) b.cabinet().load (ir.data(), (int) ir.size(), fs);
        const auto amp = [] (float* mono, int n) { for (int i = 0; i < n; ++i) mono[i] = std::tanh (3.0f * mono[i]); };
        std::vector<float> al (256), ar (256), bl (256), br (256);
        bool same = true;
        for (size_t at = 0; at + 256 <= x.size(); at += 256)
        {
            std::memcpy (al.data(), x.data() + at, sizeof (float) * 256);
            std::memcpy (bl.data(), x.data() + at, sizeof (float) * 256);
            a.process (al.data(), ar.data(), 256, s, amp);
            b.inputCut (bl.data(), 256, s);
            b.gateAnalyse (bl.data(), 256, s);
            b.keepEnvelopeKey (bl.data(), 256, s);
            b.compress (bl.data(), 256, s);
            b.octaveDown (bl.data(), 256, s);
            b.envelope (bl.data(), 256, s);
            amp (bl.data(), 256);
            b.afterAmp (bl.data(), 256, s);
            float* planes[] { bl.data(), br.data() };
            b.widen (planes, 2, 256);
            b.chorus (planes, 2, 256, s);
            b.cabinetStage (planes, 2, 256, s);
            b.bassCuts (planes, 2, 256, s);
            same = same && std::memcmp (al.data(), bl.data(), sizeof (float) * 256) == 0
                        && std::memcmp (ar.data(), br.data(), sizeof (float) * 256) == 0;
        }
        test::ok (same, "the same samples, both planes, every block");
    }

    test::group ("CAB / DI: 0 % the signal the cabinet was handed; 100 % the cabinet alone, at the bass's level");
    {
        auto di = wire();
        di.cabWorks = true;
        di.cabMix = 0.0f;
        const auto withDi = run (x, 128, di, cabinetShot());
        bool same = true;
        for (size_t i = 0; i < x.size(); ++i)
            same = same && core::exactlyEqual (withDi.l[i], 2.0f * x[i]);
        test::ok (same, "MIX 0 %: the DI, bit for bit, the cabinet loaded and running");

        std::vector<float> impulse (4800, 0.0f);
        impulse[0] = 1.0f;
        auto cabOnly = di;
        cabOnly.cabMix = 100.0f;
        const auto o = run (x, 128, cabOnly, impulse);
        const float g = measurement::bassReferenceUnityGain (impulse.data(), (int) impulse.size(), fs);
        double w = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
            w = std::max (w, std::abs ((double) o.l[i] - (double) g * 2.0 * x[i]));
        test::ok (w < 1.0e-5, "MIX 100 % through a unit impulse: the signal at the bass's reference level (" + std::to_string (w) + ")");
    }

    test::group ("at work, the host's blocks do not matter");
    {
        const auto s = working();
        const auto ir = cabinetShot();
        const auto a = run (x, 512, s, ir), b = run (x, 128, s, ir), c = run (x, 333, s, ir);
        test::ok (worst (a.l, b.l) < 1.0e-6 && worst (a.r, b.r) < 1.0e-6, "blocks of 512 and 128: within 1e-6 ("
                                                                          + std::to_string (std::max (worst (a.l, b.l), worst (a.r, b.r))) + ")");
        test::ok (worst (a.l, c.l) < 1.0e-6 && worst (a.r, c.r) < 1.0e-6, "blocks of 512 and 333: within 1e-6 ("
                                                                          + std::to_string (std::max (worst (a.l, c.l), worst (a.r, c.r))) + ")");
        test::ok (worst (a.l, a.r) > 1.0e-3, "and the chorus made two planes of the one signal");
    }

    test::group ("the gate at the END mutes what the chain added");
    {
        auto s = wire();
        s.gateWorks = true;
        s.gateThresholdDb = -20.0f;   // over the phrase's tail: it closes
        std::vector<float> quiet ((size_t) fs, 0.0f);
        for (size_t i = 0; i < quiet.size(); ++i)
            quiet[i] = (float) (0.003 * std::sin (2.0 * core::kPi * 55.0 * (double) i / fs));   // -50 dBFS, under it
        const auto o = run (quiet, 128, s);
        double tail = 0.0;
        for (size_t i = quiet.size() / 2; i < quiet.size(); ++i)
            tail = std::max (tail, (double) std::abs (o.l[i]));
        test::ok (tail < 1.0e-4, "a note under the threshold: closed, " + std::to_string (tail));
    }

    test::group ("the knobs reach their stages as the app wires them");
    {
        Settings s;
        s.hpfHz5 = 22.0f;
        s.hpfHz4 = 32.0f;
        s.hpf = 1;
        s.lpfOn = true;
        s.lpf = 1;
        const auto end = BassChain::cutSettings (s), inner = BassChain::innerCutSettings (s);
        test::ok (end.hpfOn && core::exactlyEqual (end.hpfHz, 32.0) && end.lpfOn && core::exactlyEqual (end.lpfHz, 9500.0),
                  "the END: 4 STR at 32 Hz, SLAP at 9.5 kHz");
        test::ok (inner.hpfOn && core::exactlyEqual (inner.hpfHz, 32.0 / 3.0) && ! inner.lpfOn, "inside: a third of the HPF, no LPF");
        s.innerCutsOut = true;
        test::ok (! BassChain::innerCutSettings (s).hpfOn, "innerCutsOut: none inside");

        eq::EqLink::Settings console;
        console.hpfOn = console.lpfOn = true;
        console.b1Hz = 6000.0;
        console.b2Db = 4.0;
        console.b3On = true;
        console.b3Db = -6.0;
        console.loDb = 3.0;
        const auto t = bass::toneSettings (console);
        test::ok (! t.hpfOn && ! t.lpfOn && core::exactlyEqual (t.b1Hz, 3000.0) && core::exactlyEqual (t.b2Db, 0.0) && ! t.b3On
                      && core::exactlyEqual (t.b3Db, 0.0) && core::exactlyEqual (t.loDb, 3.0),
                  "the tone: no console cut, MIDDLE held to 3 kHz, the second bell and the scalpel out, BASS kept");
        const auto worn = bass::toneSettings (console, true);
        test::ok (core::exactlyEqual (worn.loDb, 0.0) && core::exactlyEqual (worn.b1Db, 0.0) && core::exactlyEqual (worn.hiDb, 0.0),
                  "on a pack wearing a tone of its own, BASS, MIDDLE and TREBLE flat");
    }

    test::group ("a host with pictures: the stages' parts play what the stages play");
    {
        // Chain a plays the stages; chain b plays them as the plug-in does around its pictures: OCTAVE in pieces of 100 and
        // its cut once on the block, ENVELOPE in pieces of 37 from their place in the key, the cut after the AMP and the
        // END mute apart, CAB through a cabinet of the host's own — on blocks of 333, so no piece is the block.
        const auto s = working();
        const auto ir = cabinetShot();
        constexpr int block = 333;
        BassChain a, b;
        (void) a.prepare (fs, block, 2, s);
        (void) b.prepare (fs, block, 2, s, false);
        cabinet::CabinetIr own;
        own.setReference (cabinet::CabinetIr::Reference::bass);
        own.prepare (fs, block, 2);
        (void) a.cabinet().load (ir.data(), (int) ir.size(), fs);
        (void) own.load (ir.data(), (int) ir.size(), fs);
        const auto amp = [] (float* mono, int n) { for (int i = 0; i < n; ++i) mono[i] = std::tanh (3.0f * mono[i]); };
        std::vector<float> al (block), ar (block), bl (block), br (block);
        bool same = true, ranAlways = true;
        for (size_t at = 0; at + block <= x.size(); at += block)
        {
            std::memcpy (al.data(), x.data() + at, sizeof (float) * block);
            std::memcpy (bl.data(), x.data() + at, sizeof (float) * block);
            float* ap[] { al.data(), ar.data() };
            float* bp[] { bl.data(), br.data() };

            a.inputCut (al.data(), block, s);
            a.gateAnalyse (al.data(), block, s);
            a.keepEnvelopeKey (al.data(), block, s);
            a.compress (al.data(), block, s);
            a.octaveDown (al.data(), block, s);
            a.envelope (al.data(), block, s);
            amp (al.data(), block);
            a.afterAmp (al.data(), block, s);
            a.widen (ap, 2, block);
            a.chorus (ap, 2, block, s);
            a.cabinetStage (ap, 2, block, s);
            a.bassCuts (ap, 2, block, s);

            b.inputCut (bl.data(), block, s);
            b.gateAnalyse (bl.data(), block, s);
            b.keepEnvelopeKey (bl.data(), block, s);
            b.compress (bl.data(), block, s);
            for (int done = 0; done < block; done += 100)
                b.octave (bl.data() + done, std::min (100, block - done), s);
            b.cut (2, bl.data(), block, s);
            for (int done = 0; done < block; done += 37)
                b.envelope (bl.data() + done, std::min (37, block - done), s, done);
            amp (bl.data(), block);
            b.cut (3, bl.data(), block, s);
            b.muteAtEnd (bl.data(), block, s);
            b.widen (bp, 2, block);
            ranAlways = b.chorus (bp, 2, block, s) && ranAlways;
            b.cabinetStage (bp, 2, block, s, own);
            b.bassCuts (bp, 2, block, s);

            same = same && std::memcmp (al.data(), bl.data(), sizeof (float) * block) == 0
                        && std::memcmp (ar.data(), br.data(), sizeof (float) * block) == 0;
        }
        test::ok (same, "the same samples, both planes, every block");
        test::ok (ranAlways && b.cabinetRan(), "CHORUS answers it ran; the cabinet that it was in the path");
        test::ok (b.effectReady (0) && b.effectReady (1) && b.effectReady (2), "the three effects prepared");

        auto out = s;
        out.cabWorks = false;
        float* bp[] { bl.data(), br.data() };
        for (int k = 0; k < 20; ++k)   // the cabinet's fade out, and past it
            b.cabinetStage (bp, 2, block, out, own);
        test::ok (! b.cabinetRan(), "the cabinet out: out of the path, once its fade is over");
    }

    test::group ("a gap is its parts, and time");
    {
        // Two chains play the phrase's first half, then a gap of 2.5 blocks — gap() on one, its parts on the other, as a
        // host drawing through it calls them — then the rest: the same samples. A third, with no gap, plays otherwise.
        const auto s = working();
        constexpr int block = 256;
        BassChain a, b, c;
        for (auto* ch : { &a, &b, &c })
            (void) ch->prepare (fs, block, 2, s);
        const auto amp = [] (float* mono, int n) { for (int i = 0; i < n; ++i) mono[i] *= 2.0f; };
        std::vector<float> l (block), r (block);
        const auto play = [&] (BassChain& ch, size_t from, size_t to, std::vector<float>& out)
        {
            for (size_t at = from; at < to; at += block)
            {
                const int n = (int) std::min ((size_t) block, to - at);
                std::memcpy (l.data(), x.data() + at, sizeof (float) * (size_t) n);
                ch.process (l.data(), r.data(), n, s, amp);
                out.insert (out.end(), l.begin(), l.begin() + n);
                out.insert (out.end(), r.begin(), r.begin() + n);
            }
        };
        const size_t half = x.size() / 2;
        std::vector<float> oa, ob, oc;
        play (a, 0, half, oa);
        play (b, 0, half, ob);
        play (c, 0, half, oc);

        constexpr int gapLength = 640;
        a.gap (gapLength, s);
        {
            std::vector<float> plane (block), wide (2 * block);
            for (int start = 0; start < gapLength; start += block)
            {
                const int len = std::min (block, gapLength - start);
                for (int k = 0; k < 4; ++k)
                {
                    std::fill (plane.begin(), plane.end(), 0.0f);
                    b.cut (k, plane.data(), len, s);
                    if (k == 0)
                        b.takeKey (plane.data(), len);
                }
                std::fill (wide.begin(), wide.end(), 0.0f);
                float* planes[] { wide.data(), wide.data() + block };
                b.bassCuts (planes, 2, len, s);
                b.octave (nullptr, len, s);
                for (int done = 0; done < len; done += 50)
                    b.envelope (nullptr, std::min (50, len - done), s, done);
                (void) b.chorus (nullptr, 0, len, s);
            }
        }
        oa.clear();
        ob.clear();
        oc.clear();
        play (a, half, x.size(), oa);
        play (b, half, x.size(), ob);
        play (c, half, x.size(), oc);
        test::ok (oa.size() == ob.size() && std::memcmp (oa.data(), ob.data(), sizeof (float) * oa.size()) == 0,
                  "gap() and its parts: the same samples after it, both planes");
        test::ok (worst (oa, oc) > 1.0e-6, "and a gap is time: the chain that had none plays otherwise ("
                                               + std::to_string (worst (oa, oc)) + ")");

        const long long before = alloc::count.load();
        a.gap (5 * block + 17, s);
        test::okNoAlloc (alloc::count.load() == before, "a gap of five blocks and a piece allocates nothing");
    }

    test::group ("the effects' tells are counted");
    {
        auto s = working();
        BassChain chain;
        (void) chain.prepare (fs, 128, 2, s);
        const int o = chain.told (0), e = chain.told (1), c = chain.told (2);
        std::vector<float> l (128, 0.0f), r (128, 0.0f);
        const auto amp = [] (float*, int) {};
        chain.process (l.data(), r.data(), 128, s, amp);
        test::ok (chain.told (0) == o && chain.told (1) == e && chain.told (2) == c, "the same knobs: no tell");
        s.octave = 0.8f;
        s.chorus = 0.1f;
        chain.process (l.data(), r.data(), 128, s, amp);
        test::ok (chain.told (0) == o + 1 && chain.told (1) == e && chain.told (2) == c + 1,
                  "OCTAVE and CHORUS moved: one tell each, ENVELOPE none");
    }

    test::group ("no allocation in process()");
    {
        const auto s = working();
        const auto ir = cabinetShot();
        BassChain chain;
        (void) chain.prepare (fs, 128, 2, s);
        (void) chain.cabinet().load (ir.data(), (int) ir.size(), fs);
        std::vector<float> l (128), r (128);
        const auto amp = [] (float* mono, int n) { for (int i = 0; i < n; ++i) mono[i] *= 2.0f; };
        for (int b = 0; b < 20; ++b)
        {
            std::memcpy (l.data(), x.data() + 128 * b, sizeof (float) * 128);
            chain.process (l.data(), r.data(), 128, s, amp);
            chain.cabinet().flushPending();
        }
        const long long before = alloc::count.load();
        for (int b = 20; b < 200; ++b)
        {
            std::memcpy (l.data(), x.data() + 128 * (size_t) b, sizeof (float) * 128);
            chain.process (l.data(), r.data(), 128, s, amp);
        }
        test::okNoAlloc (alloc::count.load() == before, "180 blocks with every stage at work");
    }

    return test::report();
}
