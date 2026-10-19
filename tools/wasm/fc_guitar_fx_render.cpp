// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// fc_guitar_fx_render — the native half of fx-parity.mjs. Renders stdin through the fc_guitar_fx ABI the way the
// node suite drives the wasm module: the same knobs at the same frames, the same blocks, through the module's own
// two buffers. Never shipped.
//
//   fc_guitar_fx_render <sampleRate> <maxBlock> <frames> <block> [<knob>=<value> | @<frame>:<knob>=<value>]...
//
// stdin: `frames` float32 of the left channel, then `frames` of the right (host byte order). stdout: the same layout,
// processed. A <knob> is a setter's name without its prefix (delay_mix, reverb_character, ...); a plain one is set
// before prepare, an @<frame> one before the first block that starts at or after that frame.

#include "fc_guitar_fx_abi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
  #include <fcntl.h>
  #include <io.h>
#endif

namespace
{
struct Knob
{
    const char* name;
    void (*setFloat) (fc_guitar_fx*, float);
    void (*setInt) (fc_guitar_fx*, int);
};

const Knob kKnobs[] = {
    { "delay_on", nullptr, fc_guitar_fx_set_delay_on },
    { "delay_sync", nullptr, fc_guitar_fx_set_delay_sync },
    { "delay_division", nullptr, fc_guitar_fx_set_delay_division },
    { "delay_bpm", fc_guitar_fx_set_delay_bpm, nullptr },
    { "delay_time_ms", fc_guitar_fx_set_delay_time_ms, nullptr },
    { "delay_repeats", fc_guitar_fx_set_delay_repeats, nullptr },
    { "delay_dark", fc_guitar_fx_set_delay_dark, nullptr },
    { "delay_offset_ms", fc_guitar_fx_set_delay_offset_ms, nullptr },
    { "delay_mix", fc_guitar_fx_set_delay_mix, nullptr },
    { "reverb_on", nullptr, fc_guitar_fx_set_reverb_on },
    { "reverb_character", nullptr, fc_guitar_fx_set_reverb_character },
    { "reverb_mix", fc_guitar_fx_set_reverb_mix, nullptr },
    { "reverb_decay", fc_guitar_fx_set_reverb_decay, nullptr },
    { "reverb_predelay_ms", fc_guitar_fx_set_reverb_predelay_ms, nullptr },
    { "reverb_hpf_hz", fc_guitar_fx_set_reverb_hpf_hz, nullptr },
};

struct Event
{
    long frame;
    const Knob* knob;
    double value;
};

bool parse (const char* arg, Event& e)
{
    std::string s (arg);
    e.frame = -1;
    if (! s.empty() && s[0] == '@')
    {
        const auto colon = s.find (':');
        if (colon == std::string::npos)
            return false;
        e.frame = std::strtol (s.c_str() + 1, nullptr, 10);
        s = s.substr (colon + 1);
    }
    const auto eq = s.find ('=');
    if (eq == std::string::npos)
        return false;
    const std::string name = s.substr (0, eq);
    e.knob = nullptr;
    for (const auto& k : kKnobs)
        if (name == k.name)
            e.knob = &k;
    e.value = std::strtod (s.c_str() + eq + 1, nullptr);
    return e.knob != nullptr;
}

void apply (fc_guitar_fx* fx, const Event& e)
{
    // JavaScript hands a wasm i32 parameter ToInt32 of the number, a f32 one the number rounded to float: the
    // same conversions here, so both halves set the same value.
    if (e.knob->setInt != nullptr)
        e.knob->setInt (fx, (int) e.value);
    else
        e.knob->setFloat (fx, (float) e.value);
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 5)
    {
        std::fprintf (stderr, "usage: %s <sampleRate> <maxBlock> <frames> <block> [knob=value | @frame:knob=value]...\n", argv[0]);
        return 2;
    }
    const double sr       = std::strtod (argv[1], nullptr);
    const int    maxBlock = std::atoi (argv[2]);
    const long   frames   = std::strtol (argv[3], nullptr, 10);
    const int    block    = std::atoi (argv[4]);
    if (frames < 0 || block < 1 || block > maxBlock)
        return 2;

    std::vector<Event> events;
    for (int i = 5; i < argc; ++i)
    {
        Event e {};
        if (! parse (argv[i], e))
        {
            std::fprintf (stderr, "bad knob: %s\n", argv[i]);
            return 2;
        }
        events.push_back (e);
    }

#if defined(_WIN32)
    _setmode (_fileno (stdin), _O_BINARY);    // raw floats, not text
    _setmode (_fileno (stdout), _O_BINARY);
#endif
    std::vector<float> in ((std::size_t) frames * 2), out ((std::size_t) frames * 2);
    if (std::fread (in.data(), sizeof (float), in.size(), stdin) != in.size())
        return 3;

    fc_guitar_fx* fx = fc_guitar_fx_create();
    if (fx == nullptr)
        return 4;
    for (const auto& e : events)
        if (e.frame < 0)
            apply (fx, e);
    if (! fc_guitar_fx_prepare (fx, sr, maxBlock))
        return 5;

    float* const l = fc_guitar_fx_buffer (fx, 0);
    float* const r = fc_guitar_fx_buffer (fx, 1);
    std::vector<bool> done (events.size(), false);
    for (long at = 0; at < frames; at += block)
    {
        const int n = (int) (frames - at < block ? frames - at : block);
        for (std::size_t i = 0; i < events.size(); ++i)
            if (! done[i] && events[i].frame >= 0 && events[i].frame <= at)
            {
                apply (fx, events[i]);
                done[i] = true;
            }
        std::memcpy (l, in.data() + at, (std::size_t) n * sizeof (float));
        std::memcpy (r, in.data() + frames + at, (std::size_t) n * sizeof (float));
        if (! fc_guitar_fx_process (fx, l, r, n))
            return 6;
        std::memcpy (out.data() + at, l, (std::size_t) n * sizeof (float));
        std::memcpy (out.data() + frames + at, r, (std::size_t) n * sizeof (float));
    }
    fc_guitar_fx_destroy (fx);

    return std::fwrite (out.data(), sizeof (float), out.size(), stdout) == out.size() ? 0 : 7;
}
