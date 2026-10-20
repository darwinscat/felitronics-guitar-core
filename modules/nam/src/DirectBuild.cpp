// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-core — see LICENSE.
#include "DirectBuild.h"

#include <NAM/container.h>
#include <NAM/get_dsp.h>
#include <NAM/model_config.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace felitronics::nam::detail
{

namespace
{
using json = nlohmann::json;

// A config that carries a "weights" key anywhere holds a capture of its own (a conditioned WaveNet's condition_dsp):
// its parser builds that one through get_dsp, from JSON numbers — not proven here, so not direct.
bool holdsWeights (const json& j)
{
    if (j.is_object())
    {
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.key() == "weights" || holdsWeights (it.value()))
                return true;
    }
    else if (j.is_array())
    {
        for (const auto& e : j)
            if (holdsWeights (e))
                return true;
    }
    return false;
}

bool leafBuildable (const json& model)
{
    if (! model.is_object()) return false;
    const auto a = model.find ("architecture");
    const auto c = model.find ("config");
    if (a == model.end() || ! a->is_string() || c == model.end() || ! c->is_object()) return false;
    const auto& arch = a->get_ref<const std::string&>();
    if (arch != "Linear" && arch != "WaveNet") return false;
    return ! holdsWeights (*c);
}

// get_dsp_with_current_prewarm_default's metadata, read the way it reads it (populate_dsp_data's sample rate).
::nam::ModelMetadata metadataOf (const json& model)
{
    ::nam::ModelMetadata md;
    md.version = model.at ("version").get<std::string>();
    md.sample_rate = ::nam::get_sample_rate_from_nam_file (model);
    const json meta = model.value ("metadata", json());
    if (! meta.is_null())
    {
        auto extract = [&meta] (const char* key) -> std::optional<double> {
            const auto it = meta.find (key);
            if (it != meta.end() && ! it->is_null()) return it->get<double>();
            return std::nullopt;
        };
        md.loudness     = extract ("loudness");
        md.input_level  = extract ("input_level_dbu");
        md.output_level = extract ("output_level_dbu");
    }
    return md;
}

const std::vector<float>& weightsOf (const namz::Decoded& d, const json& model)
{
    const auto* w = d.weightsOf (model);
    if (w == nullptr) throw std::runtime_error ("Corrupted model file is missing weights.");   // GetWeights' verdict
    return *w;
}

std::unique_ptr<::nam::DSP> buildLeaf (const json& model, const namz::Decoded& d)
{
    ::nam::verify_config_version (model.at ("version").get<std::string>());
    const auto md = metadataOf (model);
    const auto& w = weightsOf (d, model);
    auto config = ::nam::parse_model_config_json (model.at ("architecture").get<std::string>(), model.at ("config"),
                                                  md.sample_rate);
    return ::nam::create_dsp (std::move (config), w, md);   // the segment copied: a placeholder may name it twice
}
} // namespace

bool directBuildable (const json& skeleton)
{
    if (leafBuildable (skeleton)) return true;
    if (! skeleton.is_object()) return false;
    const auto a = skeleton.find ("architecture");
    const auto c = skeleton.find ("config");
    if (a == skeleton.end() || ! a->is_string() || *a != "SlimmableContainer") return false;
    if (c == skeleton.end() || ! c->is_object()) return false;
    const auto s = c->find ("submodels");
    if (s == c->end() || ! s->is_array() || s->empty()) return false;
    for (const auto& entry : *s)
    {
        if (! entry.is_object()) return false;
        const auto m = entry.find ("model");
        if (m == entry.end() || ! leafBuildable (*m)) return false;
    }
    return true;
}

std::unique_ptr<::nam::DSP> buildDirect (const namz::Decoded& d)
{
    const json& top = d.skeleton;
    if (top.at ("architecture").get<std::string>() != "SlimmableContainer")
        return buildLeaf (top, d);

    // get_dsp (json) on a container: populate_dsp_data first (the version, the weights key, the metadata)…
    ::nam::verify_config_version (top.at ("version").get<std::string>());
    (void) weightsOf (d, top);
    const auto md = metadataOf (top);
    // …then ContainerConfig::create: each submodel through get_dsp, in order, into a ContainerModel…
    const auto& entries = top.at ("config").at ("submodels");
    std::vector<::nam::container::Submodel> submodels;
    submodels.reserve (entries.size());
    for (const auto& entry : entries)
    {
        const double maxValue = entry.at ("max_value").get<double>();
        submodels.push_back ({ maxValue, buildLeaf (entry.at ("model"), d) });
    }
    auto out = std::make_unique<::nam::container::ContainerModel> (std::move (submodels), md.sample_rate);
    // …and create_dsp's apply_metadata on what it returned.
    if (md.loudness.has_value())     out->SetLoudness (*md.loudness);
    if (md.input_level.has_value())  out->SetInputLevel (*md.input_level);
    if (md.output_level.has_value()) out->SetOutputLevel (*md.output_level);
    return out;
}

} // namespace felitronics::nam::detail
