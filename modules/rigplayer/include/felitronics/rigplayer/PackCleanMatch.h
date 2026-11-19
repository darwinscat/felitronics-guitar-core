// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.
#pragma once
// felitronics::rigplayer — THE CLEAN MATCH A PACK CARRIES, FITTED FOR IT. namz 5.2's `files[].clean_match`
// (NAMZ-FORMAT.md) is fitted once, when the pack is built, so a player plays the crossover blend matched at load with
// nothing fitted there (AlignmentTable::fromDevice reads it). The fit is measureCleanMatch's; what this adds is what a
// PACK needs that a player handed a match does not: a match on EVERY entry, so the pack says it was fitted rather than
// leaving a reader to guess why one file has none; nothing written that the format does not read; and the stage
// untouched when any file cannot be fitted. One function for every side that builds a pack — the capture app's
// exporter and OrbitAmp's tool both fitted it with a copy of this loop.
//
// Offline and heavy: NAM inference, about a tenth of a second a distinct file in a Release build on a laptop's core —
// any thread but the audio one.

#include <felitronics/nam/NamStage.h>
#include <felitronics/rigplayer/AlignmentTable.h>
#include <namz_rig.h>

#include <cmath>
#include <cstddef>
#include <exception>
#include <map>
#include <string>
#include <vector>

namespace felitronics::rigplayer {

/// WHAT THE FORMAT READS of a clean match (NAMZ-FORMAT.md, clean_match), namz's own rule: a lag of 0 to
/// kMaxLagSamples, a polarity of +1 or -1, at most two corners, each finite and at least kMinCleanMatchHz. namz's
/// writer leaves a match outside it out and its reader drops one whole, as AlignmentTable::fromDevice does — so a match
/// this says no to is never written.
inline bool cleanMatchReadable(const namz::rig::FileEntry::CleanMatch& m) {
    if (m.lagSamples < 0 || m.lagSamples > namz::rig::kMaxLagSamples) return false;
    if (m.polarity != 1 && m.polarity != -1) return false;
    if (m.allpassHz.size() > 2) return false;
    for (const double hz : m.allpassHz)
        if (! std::isfinite(hz) || hz < namz::rig::kMinCleanMatchHz) return false;
    return true;
}

/// THE CLEAN MATCH OF EVERY FILE ENTRY OF A NAM STAGE, for a pack to carry: sets `cleanMatch` on EVERY entry of `st`
/// from the model bytes `source` hands out by file id, or on none. Returns "" when every entry has its match; otherwise
/// why not, naming the file — no source, an entry with no file, bytes that do not load, a fit that threw, or a match
/// outside what the format reads (cleanMatchReadable) — and the stage is left exactly as it came. A stage with no
/// entries is left as it is, with "".
///
/// The start is the player's table for the stage, as RigPlayer::load makes it (AlignmentTable::fromDevice), and the
/// stage's own `input_db`. Each DISTINCT file is fitted once, alone (measureCleanMatch on a one-file device, the first
/// entry of an id; every entry of that id gets its match). A clean match already on the entries is not a starting
/// point — measureCleanMatch sets it aside, and so does the reading below — so a second run gives the same matches.
///
/// WHERE THE FIT BEATS NOTHING, the match is what it started from: the lag and the polarity the player taps the clean
/// leg by with no match — AlignmentTable::lagOf and invertsClean over the stage's readings without their matches: its
/// `input_lag_samples` where stated, else its `lag_samples` against the first file (a player adds a host's reference
/// offset to that lag and none to a match's; a pack states none) — and no all-passes, so such a file plays as it did
/// without one. A start outside what the format reads (a lag below 0, or past kMaxLagSamples) is then refused.
///
/// ONE RATE. NAMZ-FORMAT.md states a match's `lag_samples` at the file's own sample rate. The fit runs at the model's
/// own rate where it states one, else at `sampleRate`, and the lag it starts from is read in those samples; the lag
/// stated where it beats nothing is read in the same ones, so a fitted match and a start are in one rate. NamStage
/// accepts only models within half a hertz of 48 kHz (acceptsModelRate), and runs one that states no rate at 48 kHz:
/// the default `sampleRate` is that rate.
inline std::string fitPackCleanMatches(namz::rig::Stage& st, const ModelSource& source, double sampleRate = 48000.0) {
    const double unstated = sampleRate > 0.0 ? sampleRate : 48000.0;      // measureCleanMatch's, for a model stating none
    if (! source) return "no model bytes to fit from";

    // The player's table for this stage, as RigPlayer::load makes it — what the fit starts from — and the same without
    // the stage's matches: what the player taps a file's clean leg by with no match, which a file the fit beats nothing
    // on states instead.
    const auto start = AlignmentTable::fromDevice(st.device);
    auto readings = start;
    readings.cleanMatchByFile.clear();

    std::map<std::string, namz::rig::FileEntry::CleanMatch> want;
    for (const auto& f : st.device.files) {
        if (f.id.empty()) return "a file entry with no file";
        if (want.count(f.id) > 0) continue;
        namz::rig::FileEntry::CleanMatch m;
        try {
            // The bytes must load: measureCleanMatch skips a file that does not, which would read as "nothing beats
            // its reading". Loaded as measureCleanMatch loads it first, which is also where its rate is learnt; the fit
            // then runs on these same bytes.
            const auto bytes = source(f.id);
            felitronics::nam::NamStage probe;
            probe.prepare(unstated, 512);
            if (bytes.empty() || ! probe.loadModelFromMemory(bytes.data(), bytes.size()))
                return f.id + ": the model does not load";
            const double own = probe.modelSampleRate(), fs = own > 0.0 ? own : unstated;

            namz::rig::Device one = st.device;
            one.files = { f };
            const ModelSource these = [&bytes](const std::string&) { return bytes; };
            const auto found = measureCleanMatch(one, these, start, st.inputDb, sampleRate);
            if (const auto hit = found.find(f.id); hit != found.end()) {
                m.lagSamples = hit->second.lag;
                m.polarity = hit->second.inverted ? -1 : 1;
                for (const double hz : hit->second.allpassHz)
                    if (hz > 0.0) m.allpassHz.push_back(hz);
            } else {
                m.lagSamples = readings.lagOf(f.id, fs, fs);
                m.polarity = readings.invertsClean(f.id) ? -1 : 1;
            }
        } catch (const std::exception& e) {
            return f.id + ": the fit threw: " + e.what();
        } catch (...) {
            return f.id + ": the fit threw";
        }
        if (! cleanMatchReadable(m))
            return f.id + ": the match is outside what the format reads (lag " + std::to_string(m.lagSamples)
                 + ", polarity " + std::to_string(m.polarity) + ", " + std::to_string(m.allpassHz.size()) + " corners)";
        want[f.id] = m;
    }
    for (auto& f : st.device.files) f.cleanMatch = want.at(f.id);
    return {};
}

} // namespace felitronics::rigplayer
