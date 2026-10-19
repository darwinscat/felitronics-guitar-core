// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.
#pragma once

// A PACK THAT NEEDS NOBODY'S CAPTURES: rig.json text and its files, built here — so the ABI suite and the node
// parity run (fc_rig_render --fixture) have a pack in a public checkout, where the real ones are not. The device is
// modules/rigplayer's test pedal: a channel switch, a gain dial captured at 60, 150 and 240 of a 300-degree sweep
// with a LINK at 0 (the 60 capture fed 6 dB softer), one capture on red; a band tone knob after the models, a curve
// before them, a blend knob, and the stage's own two levels. The captures are small but not all trivial: tagged and
// untagged gains, a gain with an offset, a pure delay and a real WaveNet that declares a receptive field (so a load
// is warmed before it is heard), packed as .namz except one left as plain .nam JSON.

#include <felitronics/lineareq/MagnitudeCurve.h>
#include <namz.h>
#include <namz_rig.h>
#include <namz_rig_write.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace fc_rig_fixture
{

inline std::string taggedGain (double w, double loudnessDb)
{
    char buf[320];
    std::snprintf (buf, sizeof buf,
        R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":1,"bias":false,"implementation":"direct"},"weights":[%.6f],"sample_rate":48000,"metadata":{"loudness":%.6f}})", w, loudnessDb);
    return buf;
}

inline std::string biasGain (double w, double b)
{
    char buf[320];
    std::snprintf (buf, sizeof buf,
        R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":1,"bias":true,"implementation":"direct"},"weights":[%.6f,%.6f],"sample_rate":48000})", w, b);
    return buf;
}

inline std::string delayLine (int d)
{
    std::string w = "[";
    for (int i = 0; i < d; ++i) w += "0.0,";
    w += "0.8]";
    return R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":)" + std::to_string (d + 1)
         + R"(,"bias":false,"implementation":"direct"},"weights":)" + w + R"(,"sample_rate":48000})";
}

inline std::string waveNet (int dilation)
{
    return std::string (R"({"version":"0.5.0","architecture":"WaveNet","config":{"layers":[{"input_size":1,)")
         + R"("condition_size":1,"head_size":1,"head_bias":false,"channels":1,"kernel_size":2,"dilations":[)"
         + std::to_string (dilation)
         + R"(],"activation":"Tanh","gated":false}],"head_scale":1.0},"weights":[1,0.7,0.3,0,1,0,0,1,1],)"
         + R"("sample_rate":48000,"metadata":{"loudness":-14.5}})";
}

inline std::vector<std::byte> bytesOf (const std::string& s)
{
    const auto* p = reinterpret_cast<const std::byte*> (s.data());
    return { p, p + s.size() };
}

inline std::vector<std::byte> packed (const std::string& nam)
{
    const auto z = namz::pack (nam.data(), nam.size());
    const auto* p = reinterpret_cast<const std::byte*> (z.data());
    return { p, p + z.size() };
}

struct Pack
{
    std::string manifest;                                    // rig.json
    std::map<std::string, std::vector<std::byte>> files;     // files[].id -> its bytes
};

inline Pack pack()
{
    namz::rig::Rig rig;
    rig.rigId = "fc-rig-fixture"; rig.name = "Fixture Pedal"; rig.modeledBy = "the fc_rig suite";
    namz::rig::Stage st;
    st.kind = namz::rig::StageKind::Nam; st.rawKind = "nam"; st.slot = "pedal";
    st.make = "Darwin's Cat"; st.model = "Fixture Pedal"; st.gearType = "pedal";
    st.inputDb = -2.0; st.outputDb = 1.5;
    auto& d = st.device;
    d.family = "Fixture Pedal"; d.rigId = "fc-rig-fixture"; d.slot = "pedal";
    namz::rig::Control ch; ch.name = "Channel"; ch.role = namz::rig::Role::Channel; ch.values = { "green", "red" };
    namz::rig::Control g;  g.name = "Gain"; g.role = namz::rig::Role::Gain; g.values = { "60", "150", "240" }; g.sweep = 300;
    d.controls = { ch, g };
    const auto file = [] (const char* id, const char* chan, const char* gain, double inDb = 0.0) {
        namz::rig::FileEntry f; f.id = id; f.settings = { { "Channel", chan }, { "Gain", gain } }; f.inputDb = inDb; return f;
    };
    d.files = { file ("g60.namz", "green", "60"), file ("g150.namz", "green", "150"), file ("g240.nam", "green", "240"),
                file ("r150.namz", "red", "150"), file ("g60.namz", "green", "0", -6.0) };

    namz::rig::Tone tone;
    tone.name = "Treble"; tone.sweep = 300; tone.placement = "post"; tone.reference = "150"; tone.defaultValue = "150";
    namz::rig::Section hs; hs.kind = namz::rig::SectionKind::HighShelf; hs.hz = 3000.0; hs.q = 0.7;
    hs.dbAtMin = -6.0; hs.dbAtMax = 6.0;
    tone.sections = { hs };
    namz::rig::Tone bass;
    bass.name = "Bass"; bass.sweep = 300; bass.placement = "pre"; bass.reference = "0"; bass.defaultValue = "0";
    bass.grid.fLo = 20.0; bass.grid.fHi = 20000.0; bass.grid.points = 25;
    bass.trusted.levels = 1;
    const auto grid = felitronics::lineareq::logFreqGrid (20.0, 20000.0, 25);
    namz::rig::TonePosition p0; p0.value = "0"; p0.norm = 0.0; p0.db.assign (25, 0.0);
    namz::rig::TonePosition p1; p1.value = "300"; p1.norm = 1.0;
    for (const double f : grid)
        p1.db.push_back (f <= 200.0 ? 6.0 : f >= 2000.0 ? 0.0 : 6.0 * (1.0 - std::log (f / 200.0) / std::log (10.0)));
    bass.positions = { p0, p1 };
    st.tone = { tone, bass };

    namz::rig::Blend mix;
    mix.name = "Mix"; mix.sweep = 300; mix.reference = "300"; mix.dryEnd = "0"; mix.defaultValue = "300";
    mix.polarity = 1; mix.dryLevelDb = -6.0;
    mix.grid.fLo = 20.0; mix.grid.fHi = 20000.0; mix.grid.points = 25;
    mix.dryDb.assign (25, 0.0);
    namz::rig::BlendPosition dry; dry.value = "0";   dry.norm = 0.0; dry.dryDb = 0.0;    dry.wetDb = -120.0;
    namz::rig::BlendPosition wet; wet.value = "300"; wet.norm = 1.0; wet.dryDb = -120.0; wet.wetDb = 0.0;
    mix.positions = { dry, wet };
    st.blend = { mix };
    rig.chain = { st };

    Pack p;
    p.manifest = namz::rig::writeManifest (rig);
    p.files["g60.namz"]  = packed (taggedGain (0.5, -20.0));
    p.files["g150.namz"] = packed (waveNet (3));
    p.files["g240.nam"]  = bytesOf (biasGain (1.5, 0.05));
    p.files["r150.namz"] = packed (delayLine (5));
    return p;
}

} // namespace fc_rig_fixture
