// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.
//
// LAW 11c — "A PAUSE IS SILENCE" for poweramp::PowerAmpStage, the law's seventh address. The rest of the
// law is one suite in felitronics-core (modules/mastering/tests/PauseIsSilenceTests.cpp); this address
// moved here with its module and is tested through the same fixture helpers (core's
// test_support/law11c_pause.h). The body below is the section core's suite carried, verbatim.
#include <felitronics/poweramp/PowerAmpStage.h>
#include <felitronics_test.h>
#include <law11c_pause.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace felitronics;
using felitronics::test::ok;
using felitronics::test::group;
using felitronics::test::run;
using namespace felitronics::test::law11c;

// --- PowerAmpStage (the SEVENTH address) --------------------------------------------------------
static void powerAmpInvariant()
{
    group ("law 11c — poweramp::PowerAmpStage: the shared sag supply spends the pause");
    for (int gap : kGaps)
    {
        const int B = 128;
        // The observable is the RETURN AUDIO, because the sag supply has no public getter — and the
        // return is the thing a listener hears anyway. `C` is a COLD instance that never saw the loud
        // tone: it is the precondition, and without it this fixture could not tell a live sag rail from
        // an inert one (the `Voicing` defaults are all zero, so a stage with an unfilled voicing does
        // not move a single bit — the fifth blind form, and this repository has already paid for it).
        poweramp::PowerAmpStage A, Bp, C;
        poweramp::Voicing v;
        // A 20-SECOND sag recovery, which is not musical and is the point: the drain to exact rest takes
        // 512 blocks (~1.4 s), and a musical 150 ms recovery would have released the supply completely
        // before the pause even started — the fixture would then compare two rested rails and pass
        // against a stage that freezes the supply. A state defect is tested at settings where the state
        // is still VISIBLE; the musical setting belongs to a test about sound, not about this.
        v.sagMaxDroop = 0.35f; v.sagFastMs = 3.0f; v.sagRecoveryMs = 20000.0f; v.driveScale = 1.0f;
        poweramp::Params pp; pp.driveDb = 18.0f; pp.sag = 1.0f; pp.outputDb = 0.0f;
        A.prepare (kFs, B); Bp.prepare (kFs, B); C.prepare (kFs, B);
        A.setParams (pp, v); Bp.setParams (pp, v); C.setParams (pp, v);
        std::vector<float> l (B), r (B);
        for (int k = 0; k < 24; ++k)
        {
            fillTone (l, k * B, 120.0, 0.9f); r = l; float* io[2] = { l.data(), r.data() }; run (A.process (io, 2, B));
            fillTone (l, k * B, 120.0, 0.9f); r = l; float* jo[2] = { l.data(), r.data() }; run (Bp.process (jo, 2, B));
        }
        // TWO OBSERVABLES, and the audio one is the load-bearing half. This stage's per-channel path DOES
        // reach exact rest on silence, but slowly: measured, its residue is 5.1e-08 after 64 silent
        // blocks, 8.0e-22 after 256 and exactly zero after 512 — so a 256-block drain would have compared
        // law 11a's drop against a ring-down that had not finished, and read a defect that is not there.
        // Past rest the return is bit-comparable, and it is the only thing that can see the thirteen
        // block-rate GLIDES: `sagDroop()` cannot, because it reads the supply and not the drive.
        if (! drainToRest (A, Bp, B, 1024)) ok (false, "precondition: the per-channel path reaches exact rest before the pause");
        // ...AND A GLIDE IN FLIGHT. The thirteen block-rate smoothers are snapped on the first block and
        // never moved again unless a parameter changes, so a fixture that sets the params once and then
        // pauses would pass against an implementation that freezes the glides — which is half of what
        // this stage's law-11c defect was. Move Drive and Output right before the gap so both runs enter
        // it mid-transition.
        poweramp::Params moved = pp; moved.driveDb = 3.0f; moved.outputDb = -8.0f; moved.sag = 0.3f;
        A.setParams (moved, v); Bp.setParams (moved, v); C.setParams (moved, v);
        const float chargedDroop = A.sagDroop();
        for (int off = 0; off < gap; )
        {
            const int n = std::min (B, gap - off);
            float* io[2] = { nullptr, nullptr }; run (A.process (io, 0, n));
            std::vector<float> z1 ((std::size_t) n, 0.0f), z2 ((std::size_t) n, 0.0f);
            float* jo[2] = { z1.data(), z2.data() }; run (Bp.process (jo, 2, n));
            off += n;
        }
        if (! bitsEqual (A.sagDroop(), Bp.sagDroop()))
            ok (false, "the sag supply after a gap equals the sag supply after silence, gap " + std::to_string (gap));
        {
            std::vector<float> al ((std::size_t) B), ar ((std::size_t) B), bl ((std::size_t) B), br ((std::size_t) B);
            fillTone (al, 0, 120.0, 0.9f); ar = al; bl = al; br = al;
            float* ai[2] = { al.data(), ar.data() }; float* bi[2] = { bl.data(), br.data() };
            run (A.process (ai, 2, B)); run (Bp.process (bi, 2, B));
            bool same = true, live = false;
            for (int i = 0; i < B; ++i) { same = same && bitsEqual (al[(std::size_t) i], bl[(std::size_t) i]);
                                          live = live || std::fabs ((double) bl[(std::size_t) i]) > 1.0e-3; }
            if (! same) ok (false, "the return after a gap is bit-identical to the return after silence, gap " + std::to_string (gap));
            if (gap == kGaps[0]) ok (live, "precondition: the return actually carries audio");
        }
        if (gap <= 480) ok (! bitsEqual (Bp.sagDroop(), C.sagDroop()),
                            "precondition: the sag supply is genuinely charged — a cold stage answers differently");
        if (gap >= 48000) ok (Bp.sagDroop() < chargedDroop,
                            "...and a second of pause has visibly recovered the rail, exactly as silence does");
    }
    ok (true, "PowerAmpStage: the sag supply and its glides spend a pause exactly as silence does");
}

int main()
{
    std::printf ("law 11c — a pause is silence: poweramp\n");
    powerAmpInvariant();
    return felitronics::test::report();
}
