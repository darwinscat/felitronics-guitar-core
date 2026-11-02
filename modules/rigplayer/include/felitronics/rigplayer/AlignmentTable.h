// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-core — see LICENSE.
#pragma once
// felitronics::rigplayer — HOW FAR APART THE MODELS OF ONE DEVICE LAND. Two captures of one pedal
// come out of training shifted by a few samples; crossfaded as they are, the pair combs. The reading is
// a relation between two files, re-measured whenever a pack is built — which is whenever a model could
// have changed — and THE PACK CARRIES IT: `lag_samples` on every file entry (NAMZ-FORMAT.md), measured
// by the capture side with every model in hand. A player takes it from there (fromDevice) and delays
// the earlier models so any pair lines up.
//
// For a pack that does not carry it, the same measurement is here: half a second of broadband through
// each model, a normalized cross-correlation against the first — a pure function over bytes, heavy
// (NAM inference per file), for the host to run on whatever thread it likes and hand to the player.
// Nothing here touches audio hardware or the player's state.

#include <felitronics/rigplayer/ModelAlignment.h>

#include <felitronics/eq/Crossover2.h>      // measureLowBandGainDb: the crossover blend's own pair
#include <felitronics/nam/BlendLaw.h>       // kBlendMaxDelay — the most a slot can be delayed
#include <felitronics/nam/NamStage.h>
#include <namz_rig.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace felitronics::rigplayer {

// The bytes of one file of the pack, by its `files[].id`. The pack lives wherever the host keeps it —
// a zip, a folder, a database row — and the player never finds out. Empty = not available.
using ModelSource = std::function<std::vector<std::byte>(const std::string& fileId)>;

/// HALF A SECOND OF BROADBAND, generated rather than fetched. It never sounds and nobody keeps it: it
/// is pushed through two models to see how far apart their outputs land, and for that any signal with
/// energy everywhere will do. Deterministic, so two runs of the same check agree. A quiet probe on
/// purpose: a model is a nonlinearity, and two of them only shift alike while neither is being driven
/// into its own compression.
inline std::vector<float> alignmentProbe(double sampleRate = 48000.0) {
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int len = (int) std::lround(fs * 0.5);
    std::vector<float> out((std::size_t) len);
    const double f1 = 40.0, f2 = 10000.0, T = (double) len / fs;
    const double k = std::log(f2 / f1) / T;
    for (int n = 0; n < len; ++n) {
        const double t = (double) n / fs;
        const double ph = 2.0 * 3.14159265358979323846 * f1 * (std::exp(k * t) - 1.0) / k;
        out[(std::size_t) n] = (float) (0.05 * std::sin(ph));
    }
    return out;
}

// A file's output on the probe, at `sampleRate`, in 512-sample blocks. Empty when the bytes do not load.
inline std::vector<float> probeThrough(const std::vector<std::byte>& model, const std::vector<float>& probe,
                                       double sampleRate) {
    std::vector<float> y;
    if (model.empty()) return y;
    felitronics::nam::NamStage m;
    m.prepare(sampleRate, 512);
    if (! m.loadModelFromMemory(model.data(), model.size())) return y;
    y = probe;
    for (std::size_t p = 0; p < y.size(); p += 512) {
        const int n = (int) std::min<std::size_t>(512, y.size() - p);
        float* io[1] { y.data() + p };
        if (! m.process(io, 1, n, false)) return y;
    }
    return y;
}

struct AlignmentTable {
    // Each file's LAG against the first measurable file, in samples. Positive = this file comes out
    // later than it.
    std::map<std::string, int> lagByFile;
    // The rate those samples are in: the rate the probe ran at, or 0 for a pack's own numbers, which
    // are in each MODEL's rate (the pack does not say it; the player learns it from the model it has
    // loaded). A player converts to its own rate with the three-argument delayOf.
    double sampleRate = 0.0;
    bool fromPack = false;
    // THE ANCHOR: the file every reading above is counted from — a pack's first file (NAMZ-FORMAT.md: "later than the
    // stage's first file"), or the first whose bytes loaded when measured. One definition, carried with the numbers.
    std::string reference;
    // A capture's OWN reading, when something measured it against its DI: how late its output arrives (in the table's
    // samples) and whether it hands its input back inverted — namz 5's `input_lag_samples` and `polarity`, which
    // fromDevice reads, or a host's measurement. lagOf prefers the absolute lag to the relative one plus the reference's
    // offset; RigPlayer applies the polarity per slot.
    std::map<std::string, int>  absoluteLagByFile;
    std::map<std::string, bool> invertedByFile;
    // fromDevice DERIVED `lagByFile` from the absolute readings (no `lag_samples` in the pack). Those readings are each
    // in their own model's rate, so a player compares them in TIME (delayOfAbsolute) once it knows the rates; the derived
    // `lagByFile` stays as the table's shape (its reference, its files) and for a host that reads it.
    bool derivedFromAbsolute = false;

    // How much to DELAY this file so it lines up with the latest one, from the ABSOLUTE readings compared in time — each
    // in its own model's rate (`rateOf`; not positive = the host's) — in HOST samples. 48 samples at 48 kHz and 96 at
    // 96 kHz are both 1 ms: neither is delayed. An unknown file is not delayed.
    int delayOfAbsolute(const std::string& fileId, double hostRate, const std::function<double(const std::string&)>& rateOf) const {
        const double d = delayOfAbsoluteRaw(fileId, hostRate, rateOf);
        return d >= (double) felitronics::nam::kBlendMaxDelay ? felitronics::nam::kBlendMaxDelay
                                                              : std::clamp((int) std::lround(d), 0, felitronics::nam::kBlendMaxDelay);
    }
    // …before it is rounded and held to the line's reach.
    double delayOfAbsoluteRaw(const std::string& fileId, double hostRate, const std::function<double(const std::string&)>& rateOf) const {
        const auto me = absoluteLagByFile.find(fileId);
        if (me == absoluteLagByFile.end() || ! (hostRate > 0.0)) return 0.0;
        const auto seconds = [&] (const std::string& id, int lag) {
            const double r = rateOf ? rateOf(id) : 0.0;
            return (double) lag / (r > 0.0 ? r : hostRate);
        };
        double latest = seconds(fileId, me->second);
        for (const auto& [id, lag] : absoluteLagByFile) latest = std::max(latest, seconds(id, lag));
        return (latest - seconds(fileId, me->second)) * hostRate;
    }

    bool empty() const { return lagByFile.empty(); }

    // How much to DELAY this file so it lines up with the latest one, in the table's own samples:
    // every model is held back to the slowest, so no delay is ever negative and the pair sums in
    // phase. Unknown file = no delay.
    int delayOf(const std::string& fileId) const {
        const auto me = lagByFile.find(fileId);
        if (me == lagByFile.end()) return 0;
        int latest = me->second;
        for (const auto& [id, lag] : lagByFile) latest = std::max(latest, lag);
        return std::max(0, latest - me->second);
    }

    // …and in HOST samples, given the rate the number is in (the table's, else the model's own).
    int delayOf(const std::string& fileId, double hostRate, double modelRate) const {
        const double at = sampleRate > 0.0 ? sampleRate : modelRate;
        const double scale = (at > 0.0 && hostRate > 0.0) ? hostRate / at : 1.0;
        return std::clamp((int) std::lround((double) delayOf(fileId) * scale), 0, felitronics::nam::kBlendMaxDelay);
    }

    // How LATE this file's output arrives against its DI, in HOST samples (converted as delayOf converts). A file with
    // its own absolute reading takes it, plus `trim`; otherwise its relative reading against `reference`, plus
    // `referenceOffset` (how late the reference trails its DI), plus `trim` — the offset never reaches an absolute
    // reading. Both in the table's samples; an unknown file is offset and trim alone. Its lag plus its delayOf is how
    // late a slot playing it trails the DI: what RigPlayer's crossover holds its clean leg back by.
    int lagOf(const std::string& fileId, double hostRate, double modelRate, int referenceOffset = 0, int trim = 0) const {
        const auto ab = absoluteLagByFile.find(fileId);
        const auto me = lagByFile.find(fileId);
        // Widened before anything is added, and saturated before it is narrowed: a reading near INT_MAX must not wrap.
        const double own = ab != absoluteLagByFile.end() ? (double) ab->second
                         : (double) (me == lagByFile.end() ? 0 : me->second) + (double) referenceOffset;
        const double lag = own + (double) trim;
        const double at = sampleRate > 0.0 ? sampleRate : modelRate;
        const double scale = (at > 0.0 && hostRate > 0.0) ? hostRate / at : 1.0;
        const double host = lag * scale;
        constexpr double hi = (double) std::numeric_limits<int>::max(), lo = (double) std::numeric_limits<int>::min();
        return host >= hi ? std::numeric_limits<int>::max() : host <= lo ? std::numeric_limits<int>::min() : (int) std::lround(host);
    }

    // THE PACK'S OWN READINGS. The relative table needs every distinct file measured — a pair with one side unknown
    // cannot be lined up, and a half-measured table applied as if whole delays some models against nothing — from
    // `lag_samples`, or derived from `input_lag_samples` when every file has that instead (namz 5). A file's absolute
    // lag and polarity are read wherever stated. Nothing stated at all: the table is empty.
    static AlignmentTable fromDevice(const namz::rig::Device& d) {
        AlignmentTable out;
        out.fromPack = true;
        // namz 5's per-capture readings FIRST, in the file's own rate as `lag_samples` is: `input_lag_samples` (how late
        // the model's output arrives against its own DI) and `polarity` (-1: it hands its input back inverted). Not
        // stated = not measured: such a file has no absolute reading and plays by its relative one.
        bool allRelative = true, allAbsolute = true;
        for (const auto& f : d.files) {
            if (f.id.empty()) continue;
            if (out.reference.empty()) out.reference = f.id;
            if (f.inputLagSamples) out.absoluteLagByFile[f.id] = *f.inputLagSamples;
            if (f.polarity && *f.polarity == -1) out.invertedByFile[f.id] = true;
            allRelative = allRelative && f.lagSamples.has_value();
            allAbsolute = allAbsolute && f.inputLagSamples.has_value();
        }
        // The relative table: the pack's own `lag_samples` when every file states one (schema 4, and 5 with both); else,
        // when every file states its absolute lag, derived from those — each against the reference's — so a pack that
        // carries only the new readings is aligned as well. Neither whole: no relative table, as before.
        if (allRelative) {
            for (const auto& f : d.files) if (! f.id.empty()) out.lagByFile[f.id] = *f.lagSamples;
        } else if (allAbsolute && ! out.reference.empty()) {
            out.derivedFromAbsolute = true;
            const long long ref = out.absoluteLagByFile[out.reference];
            for (const auto& [id, lag] : out.absoluteLagByFile) {
                const long long rel = (long long) lag - ref;
                if (rel < std::numeric_limits<int>::min() || rel > std::numeric_limits<int>::max()) { out.lagByFile.clear(); break; }
                out.lagByFile[id] = (int) rel;
            }
        }
        if (out.lagByFile.empty() && out.absoluteLagByFile.empty() && out.invertedByFile.empty()) return AlignmentTable {};
        return out;
    }
};

/// HOW LOUD A CAPTURE'S LOW END IS against its DI, below `hz`: the alignment probe through the file, both sides through
/// the crossover blend's own low-pass (eq::Crossover2), their energies compared — in dB. What a host may propose as the
/// clean level (RigPlayer::setCrossoverCleanDb) so that turning the amount up does not also turn the bass up or down.
/// `normalize` plays the capture as the player does with setNormalize(true) (its loudness tag applied). Offline: NAM
/// inference, any thread but the audio one. NaN when the bytes do not load.
inline double measureLowBandGainDb(const std::vector<std::byte>& model, double hz, double sampleRate = 48000.0,
                                   bool normalize = false) {
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    const auto probe = alignmentProbe(fs);
    felitronics::nam::NamStage m;
    m.prepare(fs, 512);
    if (model.empty() || ! m.loadModelFromMemory(model.data(), model.size())) return std::nan("");
    std::vector<float> y = probe;
    for (std::size_t p = 0; p < y.size(); p += 512) {
        float* io[1] { y.data() + p };
        if (! m.process(io, 1, (int) std::min<std::size_t>(512, y.size() - p), normalize)) return std::nan("");
    }
    felitronics::eq::Crossover2 lx, ly;
    lx.prepare(fs, 1); ly.prepare(fs, 1);
    lx.setFrequency((float) hz); ly.setFrequency((float) hz);
    double ex = 0.0, ey = 0.0;
    for (std::size_t n = 0; n < y.size(); ++n) {
        float lo = 0.0f, hi = 0.0f;
        lx.processSample(0, probe[n], lo, hi); ex += (double) lo * lo;
        ly.processSample(0, y[n], lo, hi);     ey += (double) lo * lo;
    }
    return ex > 0.0 && ey > 0.0 ? 10.0 * std::log10(ey / ex) : std::nan("");
}

// The table for a device: every DISTINCT file once (several settings may point at one file), against the
// first one whose bytes load. A file with no reading — bytes that will not load, an output with no
// energy — is simply absent from the table and plays undelayed.
inline AlignmentTable measureAlignment(const namz::rig::Device& d, const ModelSource& source,
                                       double sampleRate = 48000.0) {
    AlignmentTable out;
    out.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    if (! source) return out;
    const auto probe = alignmentProbe(sampleRate);
    std::vector<std::string> ids;
    for (const auto& f : d.files)
        if (! f.id.empty() && std::find(ids.begin(), ids.end(), f.id) == ids.end()) ids.push_back(f.id);
    std::vector<float> ref;
    std::string refId;
    for (const auto& id : ids) {
        const auto y = probeThrough(source(id), probe, sampleRate);
        if (y.empty()) continue;
        if (ref.empty()) { ref = y; refId = id; out.reference = id; out.lagByFile[id] = 0; continue; }
        const auto b = felitronics::rigplayer::bestLag(ref, y, felitronics::rigplayer::kSearch);
        if (b.corr < 0.0) continue;                          // no reading is not a complaint
        out.lagByFile[id] = b.lag;
    }
    return out;
}

} // namespace felitronics::rigplayer