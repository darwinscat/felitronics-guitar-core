// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-core — see LICENSE.
#pragma once
// felitronics::rigplayer — HOW FAR ONE MODEL LAGS ANOTHER. Two NAM models of the same device, run on the same
// probe, come out shifted by a few samples (different trained latencies); the player must delay the
// faster one so a crossfade between them does not comb-filter. The measurement is a normalized
// cross-correlation over a small search window — pure math, extracted here from the player's alignment
// pass (the NAM inference and caching stay in the orchestrator). std-only, ctest (ModelAlignmentTest).

#include <cmath>
#include <cstddef>
#include <span>

namespace felitronics::rigplayer {

inline constexpr int kSearch = 96;   // samples either way; real drift between two models is single digits

struct Best { int lag = 0; double corr = -1.0; bool inverted = false; };

// The lag (in [-search, search]) at which `cur` best aligns to `ref` by NORMALIZED cross-correlation,
// evaluated over the interior [search, N-search) so every shifted index is in range. The result is the
// DELAY of `cur` relative to `ref` (cur[i] approx ref[i-lag]); read `cur` at index k+lag to align it.
// Returns {0, -1} when the two differ in length, are too short to search, or carry no energy at all —
// "no reading", which is a different answer from "aligned at zero".
//
// THE PEAK BY ITS SIZE, NOT ITS SIGN. Two captures of one device can play inverted against each other — the
// Victory V4's boosted captures do against its plain ones (measured 18.11; why is not established) — and then
// the main lobe is negative. Taking the largest POSITIVE value picked a ripple far from it: 78 and 96 samples
// for captures that lined up within four, and a blend between two of them combed. A polarity is not a
// delay: `corr` is the peak's size (0 … 1, so a negative one still means no reading) and `inverted` its
// sign.
inline Best bestLag (std::span<const float> ref, std::span<const float> cur, int search = kSearch) {
    Best out;
    if (ref.size() != cur.size() || ref.size() <= (std::size_t) (2 * search)) return out;
    const std::size_t n = ref.size();
    for (int lag = -search; lag <= search; ++lag) {
        double num = 0.0, xx = 0.0, yy = 0.0;
        for (std::size_t k = (std::size_t) search; k + (std::size_t) search < n; ++k) {
            const double u = ref[k], v = cur[(std::size_t) ((long) k + lag)];
            num += u * v; xx += u * u; yy += v * v;
        }
        // A lag with no energy on either side is not a WORSE alignment, it is no reading at all —
        // scoring it 0 made the first lag tried beat the {0, -1} that means "nothing to say", so two
        // silent models came back aligned at -96 samples and the player delayed one against the other.
        if (xx <= 0.0 || yy <= 0.0) continue;
        const double c = num / std::sqrt (xx * yy);
        if (std::abs (c) > out.corr) { out.corr = std::abs (c); out.lag = lag; out.inverted = c < 0.0; }
    }
    return out;
}

} // namespace felitronics::rigplayer