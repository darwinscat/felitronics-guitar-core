// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// fc_rig_render — the native half of rig-parity.mjs: a pack played through the fc_rig ABI, quantum by quantum, on
// the schedule fcrig-worklet.js keeps — so the wasm module, run the way a page runs it, has a native render of the
// same ABI to be held to.
//
//   fc_rig_render <pack-dir> <sampleRate> <frames> <channels> [script…]   input on stdin, output on stdout:
//                 float32, planar (channel 0's frames, then channel 1's)
//   fc_rig_render --fixture <dir>                                         writes the suite's fixture pack there
//
// The script is the processor's knob messages, each at the frame of the quantum it lands before:
//   @<frame> max <1|2>     @<frame> gain <deg>     @<frame> dial <name> <deg>     @<frame> switch <name> <value>
//   @<frame> normalize <0|1>   @<frame> trims <0|1>   @<frame> shape <point> <width>   @<frame> reset
// …and `max <1|2>` before the first `@` is the budget set before the pack loads (the processor's maxModels option).
//
// THE SCHEDULE, as the processor keeps it: prepare(rate, 128, channels); the budget; the pack loaded; then per
// quantum: the messages due, process(), and the job pump — take a job; if it needs bytes, they arrive (the page
// answers before the next quantum in the node run) and it is run and delivered; if not, it is run and delivered at once.

#include "fc_rig_abi.h"
#include "tests/rig_fixture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
constexpr int kQuantum = 128;

std::vector<char> readFile (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    return { std::istreambuf_iterator<char> (f), std::istreambuf_iterator<char>() };
}

int writeFixture (const std::string& dir)
{
    const auto pack = fc_rig_fixture::pack();
    std::ofstream (dir + "/rig.json", std::ios::binary) << pack.manifest;
    for (const auto& [id, bytes] : pack.files)
        std::ofstream (dir + "/" + id, std::ios::binary).write (reinterpret_cast<const char*> (bytes.data()),
                                                               (std::streamsize) bytes.size());
    return 0;
}

struct Move { long frame; std::string kind, a, b, c; };
} // namespace

int main (int argc, char** argv)
{
    if (argc == 3 && std::strcmp (argv[1], "--fixture") == 0) return writeFixture (argv[2]);
    if (argc < 5)
    {
        std::fprintf (stderr, "usage: fc_rig_render <pack-dir> <rate> <frames> <channels> [script…] | --fixture <dir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    const double rate = std::atof (argv[2]);
    const long frames = std::atol (argv[3]);
    const int channels = std::atoi (argv[4]);

    int budget = 0;
    std::vector<Move> moves;
    for (int i = 5; i < argc;)
    {
        Move m { -1, argv[i] };
        if (argv[i][0] == '@') { m.frame = std::atol (argv[i] + 1); m.kind = argv[++i]; }
        ++i;
        const auto arg = [&] (std::string& s) { if (i < argc) s = argv[i++]; };
        if (m.kind == "dial" || m.kind == "switch" || m.kind == "shape") { arg (m.a); arg (m.b); }
        else if (m.kind != "reset") arg (m.a);
        if (m.frame < 0 && m.kind == "max") budget = std::atoi (m.a.c_str());
        else moves.push_back (m);
    }

    std::vector<float> in ((std::size_t) frames * (std::size_t) channels);
    if (std::fread (in.data(), sizeof (float), in.size(), stdin) != in.size()) { std::fprintf (stderr, "short input\n"); return 1; }

    fc_rig* rig = fc_rig_create();
    if (fc_rig_prepare (rig, rate, kQuantum, channels) != 1) { std::fprintf (stderr, "prepare refused\n"); return 1; }
    if (budget != 0) fc_rig_set_max_models (rig, budget);
    const auto manifest = readFile (dir + "/rig.json");
    if (fc_rig_load (rig, manifest.data(), (int) manifest.size()) != 1) { std::fprintf (stderr, "load refused\n"); return 1; }

    std::vector<float> out (in.size());
    float* io[2] { fc_rig_buffer (rig, 0), channels > 1 ? fc_rig_buffer (rig, 1) : nullptr };
    std::size_t next = 0;
    for (long at = 0; at < frames; at += kQuantum)
    {
        for (; next < moves.size() && moves[next].frame <= at; ++next)
        {
            const auto& m = moves[next];
            const double x = std::atof (m.a.c_str());
            if (m.kind == "max") fc_rig_set_max_models (rig, (int) x);
            else if (m.kind == "gain") fc_rig_set_gain_dial (rig, x);
            else if (m.kind == "dial") fc_rig_set_dial (rig, m.a.c_str(), std::atof (m.b.c_str()));
            else if (m.kind == "switch") fc_rig_set_switch (rig, m.a.c_str(), m.b.c_str());
            else if (m.kind == "normalize") fc_rig_set_normalize (rig, (int) x);
            else if (m.kind == "trims") fc_rig_set_input_trims (rig, (int) x);
            else if (m.kind == "shape") fc_rig_set_blend_shape (rig, x, std::atof (m.b.c_str()));
            else if (m.kind == "reset") fc_rig_reset (rig);
        }
        const long n = std::min<long> (kQuantum, frames - at);
        for (int c = 0; c < channels; ++c)
            std::memcpy (io[c], &in[(std::size_t) (c * frames + at)], sizeof (float) * (std::size_t) n);
        fc_rig_process (rig, io[0], io[1], (int) n);
        for (int c = 0; c < channels; ++c)
            std::memcpy (&out[(std::size_t) (c * frames + at)], io[c], sizeof (float) * (std::size_t) n);

        if (fc_rig_take_job (rig) == 1)
        {
            if (fc_rig_job_needs_bytes (rig) == 1)
            {
                const auto bytes = readFile (dir + "/" + std::string (fc_rig_job_file (rig), (std::size_t) fc_rig_job_file_length (rig)));
                if (! bytes.empty())
                    std::memcpy (fc_rig_job_bytes (rig, (int) bytes.size()), bytes.data(), bytes.size());
            }
            fc_rig_job_run (rig);
            fc_rig_job_deliver (rig);
        }
    }
    std::fwrite (out.data(), sizeof (float), out.size(), stdout);
    fc_rig_destroy (rig);
    return 0;
}
