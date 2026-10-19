// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.
#pragma once

#include <namz_rig.h>

#include <cstddef>

/** rig.json text into the pack's structures (namz::rig::loadRigManifest). False = not a manifest this reader takes;
    `out` is then whatever the reader left, and is not to be played. Compiled with exceptions (fc_rig_manifest.cpp). */
bool fcRigParseManifest (const char* text, std::size_t length, namz::rig::Rig& out) noexcept;
