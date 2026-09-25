// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

//==================================================================================================
// LAW 11 (felitronics-core docs/DSP-ARCHITECTURE.md §2) for poweramp::PowerAmpStage — the same
// properties P1–P7 that core's CallContractTests sweeps over every block-level entry point it owns,
// through the same harness (core's test_support/law11_call_contract.h). The adapter below is the one
// core's suite carried while this module lived there, verbatim.
//==================================================================================================

#include <felitronics_test.h>
#include <law11_call_contract.h>

#include <felitronics/poweramp/PowerAmpStage.h>

#include <cstdio>

namespace {

namespace core = felitronics::core;
using felitronics::test::ok;
using felitronics::test::group;
using namespace felitronics::test::law11;

ADAPT (A_PowerAmpBase, felitronics::poweramp::PowerAmpStage, 2, true, true, true,
       { felitronics::poweramp::Params p; p.driveDb = 8.0f; p.autoComp = 1.0f;
         felitronics::poweramp::Voicing v; s.prepare (kFs, kMaxBlock, 4); s.setParams (p, v); return true; },
       { return s.process (io, nch, n); },
       { (void) s; return 0.0; },
       { (void) w; (void) s; return true; });

// `poweramp::PowerAmpStage::prepare` takes no channel count at all — its width is the compile-time
// `kMaxCh`, so there is no prepare-side width to bind and P7 has nothing to ask it.
struct A_PowerAmp : A_PowerAmpBase { static constexpr bool hasPrepareWidth = false; };

} // namespace

int main()
{
    std::printf ("felitronics law 11 — the caller's contract: poweramp\n");

    allProperties<A_PowerAmp>();

    return felitronics::test::report();
}
