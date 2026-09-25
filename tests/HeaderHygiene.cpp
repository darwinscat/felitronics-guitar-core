// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// HEADER HYGIENE — one TU that #includes every public header of this repository, compiled under core's
// strict, downstream-grade warning set (FELITRONICS_HYGIENE_WARNINGS, -Werror). The same TU is the
// -fno-exceptions / -fno-rtti probe: built without felitronics::nam linked, FELITRONICS_WITH_NAM is not
// defined and only the exception-free headers reach it (the NAM backend's upstream sources throw, so it
// is outside that tier by design).

#if defined(FELITRONICS_WITH_NAM)   // the compiled NAM backend — its public pImpl header is gated with its target
#include <felitronics/nam/BlendLaw.h>
#include <felitronics/nam/NamStage.h>
#include <felitronics/rigplayer/AlignmentTable.h>   // …and the pack player over it, gated with the same option
#include <felitronics/rigplayer/BlendKnob.h>
#include <felitronics/rigplayer/ModelAlignment.h>
#include <felitronics/rigplayer/ModelBlend.h>
#include <felitronics/rigplayer/RigPlayer.h>
#include <felitronics/rigplayer/RigSelection.h>
#include <felitronics/rigplayer/SectionBiquad.h>
#include <felitronics/rigplayer/ToneKnobs.h>
#endif
#include <felitronics/poweramp/PowerAmpStage.h>
#include <felitronics/poweramp/SagEnvelope.h>
#include <felitronics/poweramp/TubeStage.h>

int main() { return 0; }
