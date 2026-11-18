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
#include <optional>
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

// A CAPTURE'S CLEAN MATCH — what the crossover blend's clean leg goes through so that it meets the capture's low end in
// phase. `input_lag_samples` and `polarity` line the clean leg up with a capture as a delay lines it up: right at every
// frequency only for a device whose phase is a delay. A pedal's coupling capacitors are high-passes, each up to 90
// degrees ahead at the bottom, so its low end TURNS with frequency, and the clean leg, lined up by the lag alone, slides
// out of phase with it below the crossover — where the blend sums the two. The low sum dips there: with the clean leg at
// the capture's own level, at 50/50, the ReVolt Bass Dirt by 14 dB at 41 Hz, its Drive by 24 dB at 82 Hz, the Ampeg
// SGT-DI by 14 dB at 30 Hz — every one with its lag and polarity stated.
//
// The cure is a filter on the clean leg that turns as the device does: a delay, a polarity and up to two first-order
// all-passes. All of it is unity gain at every frequency — the clean leg's level is untouched; only its phase follows
// the capture's. It is FITTED to the capture's own response at a handful of tones (fitCleanMatch). A pack may carry it,
// fitted once when it was built (namz 5.2's `clean_match`, NAMZ-FORMAT.md), and fromDevice reads it at load; for a pack
// that does not, a host fits it off the audio thread on the models it loaded (measureCleanMatch) and hands it in with the
// table. Either way the player applies it per slot, gliding (RigPlayer, CrossoverBlend).
struct CleanMatch {
    // How late the clean leg is tapped, in `sampleRate` samples, counted from the DI as a capture's `input_lag_samples`
    // is — the fit's delay, which REPLACES that reading for the clean tap (RigPlayer::setCrossoverLag still adds to it).
    int    lag = 0;
    bool   inverted = false;               // the fit's polarity, in place of the capture's own
    double allpassHz[2] { 0.0, 0.0 };      // the two sections' corners, in Hz; 0 = no section
    // The rate `lag` is in: the rate the fit ran at (not positive = the table's — for a pack's table, whose numbers are each
    // in their own model's rate, the model's: a pack's match is read with 0 here, as its other numbers are).
    double sampleRate = 0.0;
};

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
    // THE CLEAN MATCH per capture (CleanMatch above): the pack's own `clean_match` (fromDevice), or what a host fitted
    // (measureCleanMatch) and handed in with the table. A file that has one is tapped for
    // the crossover's clean leg at ITS lag and polarity, in place of the readings above, and through its all-passes. The
    // models' own alignment against each other (delayOf, delayOfAbsolute) never reads it: it is the clean leg's alone.
    std::map<std::string, CleanMatch> cleanMatchByFile;

    // Whether the clean tap of this file is inverted: its clean match's polarity when it has one, else its own reading.
    bool invertsClean(const std::string& fileId) const {
        if (const auto m = cleanMatchByFile.find(fileId); m != cleanMatchByFile.end()) return m->second.inverted;
        const auto inv = invertedByFile.find(fileId);
        return inv != invertedByFile.end() && inv->second;
    }

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
    // A file with a CLEAN MATCH takes the match's lag instead, in the match's own rate, plus `trim` in the table's.
    int lagOf(const std::string& fileId, double hostRate, double modelRate, int referenceOffset = 0, int trim = 0) const {
        const auto ab = absoluteLagByFile.find(fileId);
        const auto me = lagByFile.find(fileId);
        const auto cm = cleanMatchByFile.find(fileId);
        // Widened before anything is added, and saturated before it is narrowed: a reading near INT_MAX must not wrap.
        const double own = ab != absoluteLagByFile.end() ? (double) ab->second
                         : (double) (me == lagByFile.end() ? 0 : me->second) + (double) referenceOffset;
        const double lag = own + (double) trim;
        const double at = sampleRate > 0.0 ? sampleRate : modelRate;
        const double scale = (at > 0.0 && hostRate > 0.0) ? hostRate / at : 1.0;
        double host = lag * scale;
        if (cm != cleanMatchByFile.end()) {
            const double r = cm->second.sampleRate > 0.0 ? cm->second.sampleRate : at;
            host = (double) cm->second.lag * ((r > 0.0 && hostRate > 0.0) ? hostRate / r : 1.0) + (double) trim * scale;
        }
        constexpr double hi = (double) std::numeric_limits<int>::max(), lo = (double) std::numeric_limits<int>::min();
        return host >= hi ? std::numeric_limits<int>::max() : host <= lo ? std::numeric_limits<int>::min() : (int) std::lround(host);
    }

    // THE PACK'S OWN READINGS. The relative table needs every distinct file measured — a pair with one side unknown
    // cannot be lined up, and a half-measured table applied as if whole delays some models against nothing — from
    // `lag_samples`, or derived from `input_lag_samples` when every file has that instead (namz 5). A file's absolute
    // lag and polarity are read wherever stated, and so is its clean match. Nothing stated at all: the table is empty.
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
            // namz 5.2's `clean_match`, in the file's own rate as its lags are (sampleRate 0): read whole or not at all, by
            // the rule namz reads it with — a lag of 0 to kMaxLagSamples (the clean leg is never tapped early), a polarity
            // of +1 or -1, at most two finite corners of at least kMinCleanMatchHz — so an entry a host built past the
            // format is no match either.
            if (const auto& m = f.cleanMatch;
                m && m->lagSamples >= 0 && m->lagSamples <= namz::rig::kMaxLagSamples
                  && (m->polarity == 1 || m->polarity == -1) && m->allpassHz.size() <= 2
                  && std::all_of(m->allpassHz.begin(), m->allpassHz.end(),
                                 [](double hz) { return std::isfinite(hz) && hz >= namz::rig::kMinCleanMatchHz; })) {
                CleanMatch cm;
                cm.lag = m->lagSamples;
                cm.inverted = m->polarity == -1;
                for (std::size_t s = 0; s < m->allpassHz.size(); ++s) cm.allpassHz[s] = m->allpassHz[s];
                out.cleanMatchByFile[f.id] = cm;
            }
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
        // A pack that states only clean matches keeps them: they are its readings, even with no alignment beside them.
        if (out.lagByFile.empty() && out.absoluteLagByFile.empty() && out.invertedByFile.empty() && out.cleanMatchByFile.empty())
            return AlignmentTable {};
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

// ------------------------------------------------------------------------------------------- the clean match ----

/// THE TONES A CLEAN MATCH IS FITTED AT: a bass's low end, 30 Hz to 260 Hz — past the highest crossover a bass amp of
/// the family offers (250 Hz), the band the blend's clean leg is heard in. A tone per few semitones is enough: what is
/// fitted is a delay and two first-order sections, which cannot bend between them.
inline std::vector<double> cleanMatchTones() {
    return { 30.0, 35.0, 41.2, 46.0, 55.0, 62.0, 73.4, 82.4, 98.0, 110.0, 130.0, 150.0, 180.0, 220.0, 260.0 };
}

/// The probe's level at the model's door, before the pack's own input levels: 0.1, about -20 dBFS — a bass DI played,
/// where the device is the device it will be, rather than its small-signal self.
inline constexpr double kCleanMatchLevel = 0.1;

/// One tone's complex level: what the device makes of a sine, against the sine — re + j·im, so a device that delays by
/// d samples reads cos(ωd) − j·sin(ωd).
struct TonePhasor { double re = 0.0, im = 0.0; };

/// A DEVICE'S RESPONSE AT EACH TONE: a sine of `amplitude` through `process` (in place, `bool process(float*, int)`, 512
/// samples at a time), settled for 0.1 s — longer than a capture remembers (NAM's WaveNet reaches about 85 ms back) —
/// then locked in over the whole periods that fill 0.15 s, under a Hann window: the output's complex level at the tone
/// divided by the input's. A distortion's harmonics land on whole bins of that window, in its zeros. Every tone starts
/// its sine anew, so no tone's tail is read as the next one's. Empty when `process` refuses a block.
template <class Process>
std::vector<TonePhasor> toneResponse(Process&& process, const std::vector<double>& hz, double amplitude, double sampleRate) {
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    constexpr double pi = 3.14159265358979323846;
    std::vector<TonePhasor> out;
    std::vector<float> x;
    for (const double f : hz) {
        const int settle = (int) std::lround(0.1 * fs);
        const int periods = (int) std::ceil(0.15 * f);
        const int measure = std::max(1, (int) std::lround((double) periods * fs / f));
        const int total = settle + measure;
        const double w = 2.0 * pi * f / fs;
        x.assign((std::size_t) total, 0.0f);
        for (int n = 0; n < total; ++n) x[(std::size_t) n] = (float) (amplitude * std::sin(w * (double) n));
        std::vector<float> y = x;
        for (int p = 0; p < total; p += 512)
            if (! process(y.data() + p, std::min(512, total - p))) return {};
        double xr = 0.0, xi = 0.0, yr = 0.0, yi = 0.0;
        for (int m = 0; m < measure; ++m) {
            const int n = settle + m;
            const double win = 0.5 - 0.5 * std::cos(2.0 * pi * ((double) m + 0.5) / (double) measure);
            const double c = std::cos(w * (double) n), s = std::sin(w * (double) n);
            xr += win * x[(std::size_t) n] * c; xi -= win * x[(std::size_t) n] * s;
            yr += win * y[(std::size_t) n] * c; yi -= win * y[(std::size_t) n] * s;
        }
        const double d = xr * xr + xi * xi;
        out.push_back(d > 0.0 ? TonePhasor { (yr * xr + yi * xi) / d, (yi * xr - yr * xi) / d } : TonePhasor {});
    }
    return out;
}

/// THE CLEAN MATCH FOR ONE CAPTURE, from its response at the tones (toneResponse): the polarity, the delay and up to two
/// first-order all-passes whose phase is nearest the capture's — the score is Σ cos(the phase between D and the clean
/// leg), largest where the two meet in phase at every tone. EVERY TONE COUNTS ALIKE, however loud the capture is there:
/// where a dip lands depends on how loud the clean leg is against the capture at that tone — at a level-matched clean
/// leg where the two are equal, at the player's own clean level (the DI at the pack's input level, often 20 dB under a
/// drive) down where the capture's low cut has brought it to the clean leg's — and the clean level is the host's, not
/// the fit's. Weighted by the capture's level instead, the fit left the ReVolt Bass Drive's 30 Hz 3 dB worse at 50/50
/// than with no match at all, as the player plays it. A grid first — every delay from 0 to 5 ms (or past
/// the start's own, if later), the corners at 40 points from 10 Hz to 2 kHz, none, one or two of them — then each
/// corner refined to an eighth of a grid step and the delay to a sample around the best.
///
/// THE PACK'S OWN READING IS WHERE IT STARTS: `startLag` (in `sampleRate` samples) and `startInverted`, with no section,
/// is one of the candidates, and nothing is returned unless something beats it — a capture whose low end is a delay
/// keeps the readings it came with. Nothing either for a response with no level at all. A pure function: any thread.
inline std::optional<CleanMatch> fitCleanMatch(const std::vector<double>& hz, const std::vector<TonePhasor>& response,
                                               int startLag, bool startInverted, double sampleRate = 48000.0) {
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    constexpr double pi = 3.14159265358979323846;
    const std::size_t nt = std::min(hz.size(), response.size());
    double level = 0.0;
    for (std::size_t h = 0; h < nt; ++h) level += std::sqrt(response[h].re * response[h].re + response[h].im * response[h].im);
    if (nt == 0 || ! (level > 1e-9 * (double) nt)) return std::nullopt;
    // Each tone's phase alone, its level taken out (a tone with no level at all has no phase, and no say).
    std::vector<TonePhasor> unit(nt);
    for (std::size_t h = 0; h < nt; ++h) {
        const double m = std::sqrt(response[h].re * response[h].re + response[h].im * response[h].im);
        if (m > 1e-9 * level) unit[h] = { response[h].re / m, response[h].im / m };
    }
    level = (double) nt;
    // One candidate's score: Σ Re(D · conj(C)), C = e^(−jωd) · A(a) · A(b); the polarity is its sign.
    const auto section = [&](double f, double corner, double& re, double& im) {      // (1 − jΩ)/(1 + jΩ), as built
        if (! (corner > 0.0)) { re = 1.0; im = 0.0; return; }
        const double o = std::tan(pi * f / fs) / std::tan(pi * corner / fs), k = 1.0 / (1.0 + o * o);
        re = (1.0 - o * o) * k; im = -2.0 * o * k;
    };
    const auto score = [&](int d, double a, double b) {
        double sum = 0.0;
        for (std::size_t h = 0; h < nt; ++h) {
            const double ph = 2.0 * pi * hz[h] * (double) d / fs;
            double cr = std::cos(ph), ci = -std::sin(ph), ar, ai, br, bi;
            section(hz[h], a, ar, ai); section(hz[h], b, br, bi);
            const double tr = cr * ar - ci * ai, ti = cr * ai + ci * ar;
            const double ur = tr * br - ti * bi, ui = tr * bi + ti * br;
            sum += unit[h].re * ur + unit[h].im * ui;                                   // Re(D · conj(C)), D's phase alone
        }
        return sum;
    };
    struct Pick { double value; int d; double a, b; };
    const int start = std::max(0, startLag);
    const Pick from { (startInverted ? -1.0 : 1.0) * score(start, 0.0, 0.0), start, 0.0, 0.0 };
    Pick best = from;
    best.value = std::abs(score(start, 0.0, 0.0));                                     // its own polarity flipped is a candidate too
    const int reach = std::max((int) std::lround(0.005 * fs), start + (int) std::lround(0.00125 * fs));
    constexpr int kCorners = 40;
    const double lo = 10.0, hi = std::min(2000.0, 0.45 * fs), step = std::pow(hi / lo, 1.0 / (kCorners - 1));
    std::vector<double> corners { 0.0 };
    for (int k = 0; k < kCorners; ++k) corners.push_back(lo * std::pow(step, (double) k));
    // The tones' phasors per corner and per delay, once: the grid is then products and sums.
    std::vector<double> sr(corners.size() * nt), si(corners.size() * nt);
    for (std::size_t c = 0; c < corners.size(); ++c)
        for (std::size_t h = 0; h < nt; ++h) section(hz[h], corners[c], sr[c * nt + h], si[c * nt + h]);
    for (int d = 0; d <= reach; ++d) {
        std::vector<double> dr(nt), di(nt);                                             // D · e^(+jωd): the delay taken out
        for (std::size_t h = 0; h < nt; ++h) {
            const double ph = 2.0 * pi * hz[h] * (double) d / fs, c = std::cos(ph), s = -std::sin(ph);
            dr[h] = unit[h].re * c + unit[h].im * s;
            di[h] = unit[h].im * c - unit[h].re * s;
        }
        for (std::size_t a = 0; a < corners.size(); ++a)
            for (std::size_t b = a; b < corners.size(); ++b) {
                double sum = 0.0;
                for (std::size_t h = 0; h < nt; ++h) {
                    const double ur = sr[a * nt + h] * sr[b * nt + h] - si[a * nt + h] * si[b * nt + h];
                    const double ui = sr[a * nt + h] * si[b * nt + h] + si[a * nt + h] * sr[b * nt + h];
                    sum += dr[h] * ur + di[h] * ui;
                }
                if (std::abs(sum) > best.value) best = { std::abs(sum), d, corners[a], corners[b] };
            }
    }
    // …and refined: each corner over ± one grid step in eighths, the delay ± 3 samples.
    const Pick coarse = best;
    for (int dd = -3; dd <= 3; ++dd)
        for (int ka = -8; ka <= 8; ++ka)
            for (int kb = -8; kb <= 8; ++kb) {
                const int d = coarse.d + dd;
                if (d < 0 || d > reach) continue;
                const double a = coarse.a > 0.0 ? coarse.a * std::pow(step, ka / 8.0) : (ka == 0 ? 0.0 : -1.0);
                const double b = coarse.b > 0.0 ? coarse.b * std::pow(step, kb / 8.0) : (kb == 0 ? 0.0 : -1.0);
                if (a < 0.0 || b < 0.0) continue;
                const double v = std::abs(score(d, a, b));
                if (v > best.value) best = { v, d, a, b };
            }
    const bool inverted = score(best.d, best.a, best.b) < 0.0;
    if (best.value <= from.value + 1e-12 * level) return std::nullopt;                 // the pack's own reading stands
    CleanMatch m;
    m.lag = best.d;
    m.inverted = inverted;
    m.allpassHz[0] = std::min(best.a, best.b) > 0.0 ? std::min(best.a, best.b) : std::max(best.a, best.b);
    m.allpassHz[1] = std::min(best.a, best.b) > 0.0 ? std::max(best.a, best.b) : 0.0;
    m.sampleRate = fs;
    return m;
}

/// THE CLEAN MATCH OF EVERY CAPTURE OF A DEVICE, for RigPlayer::setAlignment (AlignmentTable::cleanMatchByFile): each
/// DISTINCT file once, through a NamStage at the model's own rate (`sampleRate` for a model that states none), so the
/// fit's samples are the pack's and nothing of a rate-match is read as the capture's phase — fed the tones at kCleanMatchLevel and the levels the player feeds it with
/// (`inputDb`, the stage's own `input_db`, and the file's), then fitted (fitCleanMatch) from the reading `start` holds
/// for it: its lag (AlignmentTable::lagOf, without trim or offset) and its polarity — a match already in `start` is not
/// a starting point. A file whose bytes do not load, or that nothing beats its reading on, is absent: it plays as the
/// table says. Heavy — about 4 s of audio through NAM a file, a tenth of a second on a laptop's core — and offline: any
/// thread but the audio one.
inline std::map<std::string, CleanMatch> measureCleanMatch(const namz::rig::Device& d, const ModelSource& source,
                                                           const AlignmentTable& start, double inputDb = 0.0,
                                                           double sampleRate = 48000.0) {
    std::map<std::string, CleanMatch> out;
    const double unstated = sampleRate > 0.0 ? sampleRate : 48000.0;
    if (! source) return out;
    AlignmentTable from = start;
    from.cleanMatchByFile.clear();
    const auto tones = cleanMatchTones();
    std::vector<std::string> seen;
    for (const auto& f : d.files) {
        if (f.id.empty() || std::find(seen.begin(), seen.end(), f.id) != seen.end()) continue;
        seen.push_back(f.id);
        const auto bytes = source(f.id);
        if (bytes.empty()) continue;
        felitronics::nam::NamStage first, again;
        first.prepare(unstated, 512);
        if (! first.loadModelFromMemory(bytes.data(), bytes.size())) continue;
        const double own = first.modelSampleRate(), fs = own > 0.0 ? own : unstated;
        felitronics::nam::NamStage* run = &first;
        if (std::abs(fs - unstated) > 0.5) {                                          // its own rate: loaded again there
            again.prepare(fs, 512);
            if (! again.loadModelFromMemory(bytes.data(), bytes.size())) continue;
            run = &again;
        }
        felitronics::nam::NamStage& m = *run;
        const double amp = kCleanMatchLevel * std::pow(10.0, (inputDb + f.inputDb) / 20.0);
        const auto r = toneResponse([&m](float* x, int n) { float* io[1] { x }; return m.process(io, 1, n, false); },
                                    tones, amp, fs);
        if (r.empty()) continue;
        const auto inv = from.invertedByFile.find(f.id);
        if (const auto match = fitCleanMatch(tones, r, from.lagOf(f.id, fs, fs), inv != from.invertedByFile.end() && inv->second, fs))
            out[f.id] = *match;
    }
    return out;
}

} // namespace felitronics::rigplayer