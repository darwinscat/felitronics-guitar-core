// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-core — see LICENSE.
//
// THE DIRECT BUILD: a .namz capture made into NAM networks without its weights ever becoming JSON numbers.
//
// NAM's own loader (`::nam::get_dsp (json)`) takes a parsed .nam: every weight a JSON number, read back into a
// std::vector<float> and handed to the architecture's ModelConfig. A .namz stores those floats as float32 already,
// and until v0.7.0 NamStage printed them to JSON text (namz::unpack, 1.0 ms in a wasm build) and parsed the text back
// (nlohmann, 9.7–10 ms) — twice the network's own build, for floats it had in hand. namz::decode hands over the parsed
// skeleton and the float32 segments; this file feeds them to the very calls get_dsp makes:
//   a leaf      verify_config_version → parse_model_config_json (architecture, config, sample_rate) → create_dsp
//               (config, weights, metadata) — get_dsp_with_current_prewarm_default, minus the JSON weights;
//   a container ContainerConfig::create, unrolled: each submodel built as a leaf, then ContainerModel (submodels,
//               sample_rate) with the container's own metadata applied — what create_dsp does to what create returns.
//
// WHICH ARCHITECTURES — and the rule is a PROOF, not a guess: felitronics_nam_load_tests (NamLoadTests.cpp) builds every architecture fixture this
// repository and NeuralAmpModelerCore v0.5.4 carry both ways and holds the outputs to bit equality. What passed is
// direct; everything else takes the JSON path, exactly as v0.6.0 did:
//   direct   Linear; WaveNet whose config holds no capture of its own; SlimmableContainer whose every submodel is one
//            of those two (A2, the real .orbitrig packs).
//   JSON     LSTM, ConvNet, a WaveNet with a condition_dsp (or any config holding weights), a container holding any
//            of those or another container, raw .nam JSON, and an architecture NAM may add later.
#pragma once

#include <NAM/dsp.h>
#include <namz.h>

#include <memory>

namespace felitronics::nam::detail
{

// True when `skeleton` (namz::Decoded::skeleton) is an architecture the direct build is proven for. Reads the shape
// only; a malformed skeleton answers false and is left to the JSON path's own verdict.
bool directBuildable (const nlohmann::json& skeleton);

// One network of the decoded capture, built from its float32 segments. Throws whatever NAM throws on a bad spec, as
// get_dsp does. Call only where directBuildable (decoded.skeleton) is true.
std::unique_ptr<::nam::DSP> buildDirect (const namz::Decoded& decoded);

} // namespace felitronics::nam::detail
