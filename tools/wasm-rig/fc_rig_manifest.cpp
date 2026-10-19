// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// The pack's manifest, parsed — the ONE translation unit of fc_rig that is compiled with C++ exceptions in the wasm
// build, beside the NAM objects themselves (NeuralAmpModelerCore and NamStage.cpp). It is nlohmann's parser at work,
// which is what Oleh's permission of 19.10.2026 covers: exceptions for our NAM — NeuralAmpModelerCore and nlohmann —
// and nothing else. namz's reader is written not to throw (allow_exceptions=false, every value type-checked before
// it is read), and this catch is the floor under it: whatever nlohmann throws on a manifest nobody tested — a
// bad_alloc included — is a refused load here, not an abort of the module.

#include "fc_rig_manifest.h"

#include <namz_rig_load.h>

#include <string>

bool fcRigParseManifest (const char* text, std::size_t length, namz::rig::Rig& out) noexcept
{
    try
    {
        bool ok = false;
        out = namz::rig::loadRigManifest (std::string (text, length), &ok);
        return ok;
    }
    catch (...)
    {
        return false;
    }
}
