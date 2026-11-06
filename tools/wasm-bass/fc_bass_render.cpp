// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// fc_bass_render — the native half of bass-parity.mjs: the bass amp through the fc_bass ABI, block by block, on the
// schedule fcbass-worklet.js keeps — so the wasm module, run the way a page runs it, has a native render of the same ABI
// to be held to, and both have the plug-in's own renders (orbit-amp's RedGrooveBassReference) to be measured against.
//
//   fc_bass_render <pack-dir> <rate> <block> <warm-frames> <cabinet.wav> <session.txt> <in.f32> <out.f32>
//
//   pack-dir     the .orbitrig folder (rig.json and its captures)
//   cabinet.wav  the cabinet's IR, PCM 16/24/32-bit or float32: read as the plug-in reads it (JUCE's WAV reader — an
//                integer over 2^(bits-1)), the first channel
//   session.txt  the knobs, a line each, its fields apart by TABs, in order (bass-parity.mjs writes it from the app's
//                parameters, as a page would set them):
//                  set <id> <value>        fc_bass_set — a knob of the chain, by the app's id
//                  gain <degrees>          fc_rig_set_gain_dial — DRIVE
//                  dial <name> <degrees>   fc_rig_set_dial — a pack's tone dial
//                  switch <name> <value>   fc_rig_set_switch — the gear's Voice, a pack's tone switch
//                  trims <0|1>             fc_rig_set_input_trims
//   in.f32       the input, mono float32; out.f32 the output, both planes interleaved, float32 — after `warm-frames` of
//                silence played first and not written
//
// THE SCHEDULE, as the processor keeps it: prepare (rate, block); the pack loaded and its files handed in up front (the
// site's way, fc_rig ABI 5); the cabinet; the knobs; then per block: process, and the job pump — take a job, run and
// deliver it. (Where a capture lands decides, at a session rate other than the pack's, the phase its rate-match starts
// at — so the two renders keep the same schedule.)

#include "fc_bass_abi.h"
#include "fc_rig_abi.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace
{
std::vector<char> readFile (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    return { std::istreambuf_iterator<char> (f), std::istreambuf_iterator<char>() };
}

uint32_t u32 (const char* p) { uint32_t v; std::memcpy (&v, p, 4); return v; }
uint16_t u16 (const char* p) { uint16_t v; std::memcpy (&v, p, 2); return v; }

/** A WAV's first channel as JUCE's reader makes floats of it; its rate. Empty for anything else. */
std::vector<float> readWav (const std::string& path, double& rate)
{
    const auto bytes = readFile (path);
    std::vector<float> out;
    if (bytes.size() < 12 || std::memcmp (bytes.data(), "RIFF", 4) != 0 || std::memcmp (bytes.data() + 8, "WAVE", 4) != 0)
        return out;
    int channels = 0, bits = 0, format = 0;
    for (std::size_t at = 12; at + 8 <= bytes.size();)
    {
        const char* c = bytes.data() + at;
        const uint32_t size = u32 (c + 4);
        if (std::memcmp (c, "fmt ", 4) == 0 && size >= 16)
        {
            format = u16 (c + 8);
            channels = u16 (c + 10);
            rate = (double) u32 (c + 12);
            bits = u16 (c + 22);
            if (format == 0xFFFE && size >= 40) format = u16 (c + 32);   // WAVE_FORMAT_EXTENSIBLE: the sub-format's tag
        }
        else if (std::memcmp (c, "data", 4) == 0 && channels > 0 && bits > 0)
        {
            const int frameBytes = channels * bits / 8;
            const std::size_t frames = std::min<std::size_t> (size, bytes.size() - at - 8) / (std::size_t) frameBytes;
            const char* d = c + 8;
            for (std::size_t i = 0; i < frames; ++i)
            {
                const char* s = d + i * (std::size_t) frameBytes;
                float v = 0.0f;
                if (format == 3 && bits == 32) std::memcpy (&v, s, 4);
                else if (bits == 16) v = (float) (int16_t) u16 (s) / 32768.0f;
                else if (bits == 24)
                {
                    const int32_t x = (int32_t) (((uint32_t) (uint8_t) s[0] << 8) | ((uint32_t) (uint8_t) s[1] << 16)
                                                 | ((uint32_t) (uint8_t) s[2] << 24)) >> 8;
                    v = (float) x / 8388608.0f;
                }
                else if (bits == 32) v = (float) ((double) (int32_t) u32 (s) / 2147483648.0);
                out.push_back (v);
            }
            break;
        }
        at += 8 + size + (size & 1u);
    }
    return out;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc != 9)
    {
        std::fprintf (stderr, "usage: fc_bass_render <pack-dir> <rate> <block> <warm> <cabinet.wav> <session.txt> <in.f32> <out.f32>\n");
        return 2;
    }
    const std::string dir = argv[1];
    const double rate = std::atof (argv[2]);
    const int block = std::atoi (argv[3]);
    const long warm = std::atol (argv[4]);

    fc_bass* bass = fc_bass_create();
    fc_rig* rig = fc_bass_rig (bass);
    if (fc_bass_prepare (bass, rate, block) != 1) { std::fprintf (stderr, "prepare refused\n"); return 1; }
    const auto manifest = readFile (dir + "/rig.json");
    if (fc_rig_load (rig, manifest.data(), (int) manifest.size()) != 1) { std::fprintf (stderr, "load refused\n"); return 1; }
    // THE FILES UP FRONT, as the site hands a pack's files in with it (fc_rig ABI 5): every file the pack names, so no
    // job waits for its bytes and a capture lands at the same block here as in the worklet.
    for (const auto& entry : std::filesystem::directory_iterator (dir))
    {
        const auto name = entry.path().filename().string();
        if (! entry.is_regular_file() || name == "rig.json") continue;
        const auto bytes = readFile (entry.path().string());
        if (bytes.empty()) continue;
        if (uint8_t* p = fc_rig_put_file (rig, name.c_str(), (int) bytes.size()))
            std::memcpy (p, bytes.data(), bytes.size());
    }

    double irRate = 0.0;
    const auto ir = readWav (argv[5], irRate);
    if (ir.empty()) { std::fprintf (stderr, "no cabinet in %s\n", argv[5]); return 1; }
    std::memcpy (fc_bass_cab_taps (bass, (int) ir.size()), ir.data(), sizeof (float) * ir.size());
    if (fc_bass_cab_load (bass, (int) ir.size(), irRate) != 1) { std::fprintf (stderr, "cabinet refused\n"); return 1; }

    const auto sessionBytes = readFile (argv[6]);
    std::istringstream session (std::string (sessionBytes.data(), sessionBytes.size()));
    for (std::string line; std::getline (session, line);)
    {
        std::vector<std::string> f;
        for (std::size_t from = 0;;)
        {
            const auto tab = line.find ('\t', from);
            f.push_back (line.substr (from, tab == std::string::npos ? std::string::npos : tab - from));
            if (tab == std::string::npos) break;
            from = tab + 1;
        }
        int taken = 1;
        if (f[0] == "set" && f.size() == 3) taken = fc_bass_set (bass, f[1].c_str(), std::atof (f[2].c_str()));
        else if (f[0] == "gain" && f.size() == 2) taken = fc_rig_set_gain_dial (rig, std::atof (f[1].c_str()));
        else if (f[0] == "dial" && f.size() == 3) taken = fc_rig_set_dial (rig, f[1].c_str(), std::atof (f[2].c_str()));
        else if (f[0] == "switch" && f.size() == 3) taken = fc_rig_set_switch (rig, f[1].c_str(), f[2].c_str());
        else if (f[0] == "trims" && f.size() == 2) fc_rig_set_input_trims (rig, std::atoi (f[1].c_str()));
        else if (! line.empty()) { std::fprintf (stderr, "a session line it cannot read: %s\n", line.c_str()); return 1; }
        if (taken != 1) { std::fprintf (stderr, "refused: %s\n", line.c_str()); return 1; }
    }

    const auto inBytes = readFile (argv[7]);
    std::vector<float> in (inBytes.size() / sizeof (float));
    std::memcpy (in.data(), inBytes.data(), in.size() * sizeof (float));

    float* l = fc_bass_buffer (bass, 0);
    float* r = fc_bass_buffer (bass, 1);
    std::vector<float> out;
    out.reserve (2 * in.size());
    long blocks = 0;
    const auto pump = [&]
    {
        ++blocks;
        if (fc_rig_take_job (rig) != 1) return;
        if (fc_rig_job_needs_bytes (rig) == 1)
        {
            const auto bytes = readFile (dir + "/" + std::string (fc_rig_job_file (rig), (std::size_t) fc_rig_job_file_length (rig)));
            if (! bytes.empty()) std::memcpy (fc_rig_job_bytes (rig, (int) bytes.size()), bytes.data(), bytes.size());
        }
        const std::string file (fc_rig_job_file (rig), (std::size_t) fc_rig_job_file_length (rig));
        fc_rig_job_run (rig);
        fc_rig_job_deliver (rig);
        if (std::getenv ("FC_BASS_RENDER_JOBS") != nullptr) std::fprintf (stderr, "job %s after block %ld\n", file.c_str(), blocks);
    };
    for (long done = 0; done < warm;)
    {
        const int n = (int) std::min<long> (block, warm - done);
        std::fill_n (l, n, 0.0f);
        fc_bass_process (bass, l, r, n);
        pump();
        done += n;
    }
    for (std::size_t at = 0; at < in.size(); at += (std::size_t) block)
    {
        const int n = (int) std::min<std::size_t> ((std::size_t) block, in.size() - at);
        std::memcpy (l, in.data() + at, sizeof (float) * (std::size_t) n);
        fc_bass_process (bass, l, r, n);
        for (int i = 0; i < n; ++i) { out.push_back (l[i]); out.push_back (r[i]); }
        pump();
    }
    std::ofstream (argv[8], std::ios::binary).write (reinterpret_cast<const char*> (out.data()),
                                                      (std::streamsize) (out.size() * sizeof (float)));
    std::fprintf (stderr, "%zu frames, latency %d\n", out.size() / 2, fc_bass_latency_samples (bass));
    fc_bass_destroy (bass);
    return 0;
}
