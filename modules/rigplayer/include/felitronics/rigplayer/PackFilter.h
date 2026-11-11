// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-core — see LICENSE.
#pragma once
// felitronics::rigplayer — THE PACK'S OWN FILTER (namz `chain[].high_pass` / `low_pass`): a Butterworth
// high-pass or low-pass of order 1 to 8 — 6 to 48 dB per octave — that a device always plays with and no
// knob moves. A cascade of second-order sections, plus one first-order section when the order is odd,
// each the bilinear transform of its analog prototype with the cutoff prewarped, so the -3 dB point lands
// on the stated frequency at any host rate. Double precision inside: an 80 Hz high-pass at 192 kHz puts
// its poles a hair inside the unit circle, where a float state would hum.
//
// Built on the AUDIO thread when the spec changes (a pack swap): a handful of tan() and cos(), nothing
// allocated. The state is kept across a rebuild, so a swap between two packs filtered alike is seamless.

#include <algorithm>
#include <cmath>

namespace felitronics::rigplayer {

class PackFilter {
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kMaxOrder    = 8;

    /// `order` 0 is a wire. `hz` is held inside (0, 0.45 fs); a low-pass the rate cannot place is a wire.
    void set(bool highPass, double hz, int order, double sampleRate) noexcept {
        sections_ = 0;
        if (order < 1 || order > kMaxOrder || ! (hz > 0.0) || ! (sampleRate > 0.0)) return;
        if (! highPass && hz >= 0.45 * sampleRate) return;
        hz = std::min(hz, 0.45 * sampleRate);
        const double pi = 3.14159265358979323846;
        const double k  = std::tan(pi * hz / sampleRate);
        const double k2 = k * k;
        for (int i = 1; i <= order / 2; ++i) {
            // The pole pairs' Q: 1 / (2 sin((2i - 1) pi / 2N)) — 0.707 at N 2; 0.541 and 1.307 at N 4; 1.0 beside
            // the real pole at N 3.
            const double q    = 1.0 / (2.0 * std::sin((2.0 * i - 1.0) * pi / (2.0 * order)));
            const double norm = 1.0 / (1.0 + k / q + k2);
            Coef c;
            c.b0 = highPass ? norm : k2 * norm;
            c.b1 = highPass ? -2.0 * c.b0 : 2.0 * c.b0;
            c.b2 = c.b0;
            c.a1 = 2.0 * (k2 - 1.0) * norm;
            c.a2 = (1.0 - k / q + k2) * norm;
            c_[sections_++] = c;
        }
        if (order % 2 == 1) {
            const double norm = 1.0 / (1.0 + k);
            Coef c;
            c.b0 = highPass ? norm : k * norm;
            c.b1 = highPass ? -norm : c.b0;
            c.b2 = 0.0;
            c.a1 = (k - 1.0) * norm;
            c.a2 = 0.0;
            c_[sections_++] = c;
        }
    }

    bool on() const noexcept { return sections_ > 0; }

    void reset() noexcept {
        for (auto& ch : z1_) std::fill(std::begin(ch), std::end(ch), 0.0);
        for (auto& ch : z2_) std::fill(std::begin(ch), std::end(ch), 0.0);
    }

    void process(float* const* a, int nch, int n) noexcept {
        nch = std::min(nch, kMaxChannels);
        for (int c = 0; c < nch; ++c)
            for (int s = 0; s < sections_; ++s) {
                const Coef& k = c_[s];
                double z1 = z1_[c][s], z2 = z2_[c][s];
                float* x = a[c];
                for (int i = 0; i < n; ++i) {
                    const double in  = (double) x[i];
                    const double out = k.b0 * in + z1;
                    z1 = k.b1 * in - k.a1 * out + z2;
                    z2 = k.b2 * in - k.a2 * out;
                    x[i] = (float) out;
                }
                z1_[c][s] = z1; z2_[c][s] = z2;
            }
    }

private:
    static constexpr int kMaxSections = (kMaxOrder + 1) / 2;
    struct Coef { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; };
    Coef   c_[kMaxSections] {};
    double z1_[kMaxChannels][kMaxSections] {}, z2_[kMaxChannels][kMaxSections] {};
    int    sections_ = 0;
};

}  // namespace felitronics::rigplayer
