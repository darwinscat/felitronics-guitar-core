// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.
#pragma once
// felitronics::rigplayer — THE CROSSOVER BLEND'S MECHANISM: a bass amp's clean low end. RigPlayer owns one and says
// when; this holds what it does — the clean ring, the two slot taps and their mirrored delay lines, the one
// Linkwitz–Riley pair that splits both sides, and the glides:
//
//     out = HP4(device) + LP4((1 − amount) · device + amount · clean)
//     clean = the slots' clean taps (each at its capture's lag and polarity), mixed as the slots are, then the pack's
//             own dry part mixed in by the blend knob exactly as it is mixed into the device
//
// LP4 and HP4 are core's eq::Crossover2, built of the same two Butterworth SVFs a side (eq::Svf, Q = 1/√2), run on the
// clean leg's low half and the device's high half — so a device that is a wire sums to the pair's all-pass.
//
// The clean leg is the dry copy the player hands in (ring), tapped per slot at the lag its capture carries (taps),
// delayed by the slot's alignment delay through a line that mirrors the slot's own, and mixed by the law's recipe —
// mixSlots, the ONE spelling of how a block's two slots become one plane, which the player's models go through too.
//
// Threads: the setters may be called from the message thread (atomics); everything else is the audio thread's, or a
// restart's (prepare / clear / snap), which never runs beside it. No allocation outside prepare().

#include <felitronics/core/Math.h>
#include <felitronics/core/StateGrid.h>
#include <felitronics/eq/Svf.h>
#include <felitronics/nam/BlendLaw.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace felitronics::rigplayer {

// HOW ONE BLOCK'S TWO SLOTS BECOME ONE PLANE — what the law decided (its weights and gain), which slots ran, and the
// three shapes the mix takes: nothing heard (hush), slot 1 asleep (aAlone), slot 0 asleep (bAlone).
struct MixRecipe {
    const felitronics::nam::BlendStep* law = nullptr;
    bool run[2] { false, false };
    bool hush = false, aAlone = false, bAlone = false;
};

// x0 holds slot 0's block, x1 slot 1's, each already through its delay line; x0 leaves as the plane the law hears. A
// one-model block the law holds silent throughout is SILENCE, written, not a mix multiplied by zero: the weight may be
// travelling between two slots neither of which was run, and a stale buffer times zero is a NaN if it held one. Then the
// law's own gain, below one only while a slot that carries weight is not yet safe to hear — or, with one model, through
// a dip, whose ramp may end inside the block (gainRamp) and holds there.
inline void mixSlots(float* x0, const float* x1, int count, const MixRecipe& r) noexcept {
    const auto& law = *r.law;
    if (r.hush)        std::fill(x0, x0 + count, 0.0f);
    else if (r.aAlone) { /* slot 1 asleep at zero: x0 is the whole sound */ }
    else if (r.bAlone) std::copy(x1, x1 + count, x0);
    else               felitronics::nam::blendMix(x0, x1, count, law.beginB, law.endB);
    if (! r.hush && (law.beginGain < 1.0 || law.endGain < 1.0)) {
        const int ramp = law.gainRamp > 0 ? std::min(law.gainRamp, count) : count;
        const float dg = (float) (law.endGain - law.beginGain) / (float) ramp;
        float g = (float) law.beginGain;
        for (int i = 0; i < count; ++i) {
            if (i < ramp) g += dg; else g = (float) law.endGain;
            x0[i] *= g;
        }
    }
}

// ONE SLOT'S ALIGNMENT DELAY LINE, every plane of it — a model's output and the crossover's clean tap go through one
// each. Per plane it keeps the last kCap samples it was fed, so its delay can MOVE while the slot is heard: crossfaded
// between the old and the new delay over `fade` samples, where nam::blendDelay's tail holds only its own delay's worth
// and could only be cleared. For a delay that holds, its samples are blendDelay's (CrossoverBlendTests' null).
//
// THE MOVE IS THE SLOT'S, not a plane's: one delay, one fade clock, advanced by the block's audio time (advance) whether
// a plane was handed over or not — a plane that stops and comes back finds the fade where the others have it. A delay
// asked while a fade is under way WAITS for it and starts its own fade from where it lands. clear() is a restart: the
// planes' memory empty and the move landed.
class SlotLine {
public:
    static constexpr int kCap = felitronics::nam::kBlendMaxDelay;
    static constexpr int kPlanes = 2;
    void clear() noexcept { for (auto& h : hist_) h.fill(0.0f); from_ = next_ = n_; pos_ = len_ = 0; }
    void clearPlane(int c) noexcept { if (c >= 0 && c < kPlanes) hist_[(std::size_t) c].fill(0.0f); }
    void set(int delay, int fade = 0) noexcept {
        delay = std::clamp(delay, 0, kCap);
        if (fade <= 0) { from_ = n_ = next_ = delay; pos_ = len_ = 0; return; }
        next_ = delay;
        if (pos_ >= len_ && delay != n_) { from_ = n_; n_ = delay; pos_ = 0; len_ = fade; }
    }
    int delay() const noexcept { return n_; }
    // Plane c, in place: x leaves delayed. `scratch` holds `count` samples. The fade is read where the block began.
    void process(int c, float* x, int count, float* scratch) noexcept {
        if (count <= 0 || c < 0 || c >= kPlanes) return;
        auto& h = hist_[(std::size_t) c];
        std::copy(x, x + count, scratch);
        const auto at = [&] (int d, int k) { return k >= d ? scratch[k - d] : h[(std::size_t) (kCap - d + k)]; };
        for (int k = 0; k < count; ++k) {
            const float now = at(n_, k);
            if (pos_ + k < len_) {
                const float was = at(from_, k);
                x[k] = was + (now - was) * ((float) (pos_ + k + 1) / (float) len_);
            } else x[k] = now;
        }
        if (count >= kCap) std::copy(scratch + count - kCap, scratch + count, h.begin());
        else {
            std::move(h.begin() + count, h.end(), h.begin());
            std::copy(scratch, scratch + count, h.end() - count);
        }
    }
    // The block's audio time, once per block: the fade moves on, and a waiting delay starts its own fade where it lands.
    void advance(int count) noexcept {
        if (count <= 0) return;
        pos_ = std::min(pos_ + count, len_);
        if (pos_ >= len_ && next_ != n_ && len_ > 0) { from_ = n_; n_ = next_; pos_ = 0; }
    }
private:
    std::array<float, (std::size_t) kCap> hist_[kPlanes] {};
    int n_ = 0, from_ = 0, next_ = 0, pos_ = 0, len_ = 0;
};

class CrossoverBlend {
public:
    static constexpr int    kMaxChannels = 2;
    static constexpr int    kMaxDelay = felitronics::nam::kBlendMaxDelay;
    static constexpr double kMinHz = 20.0, kMaxHz = 2000.0;
    // THE CLEAN LEG'S REACH IS A TIME: 2048 samples at 48 kHz, sized in prepare() for the host rate (8192 at 192 kHz),
    // so a lag that fits fits at every rate. maxLag() is it in host samples; the player refuses past it.
    static constexpr double kMaxLagSeconds = 2048.0 / 48000.0;
    // Where the pair is faded in and out: the SVF's floor. At 1 Hz its all-pass is a wire across the audible band (1.1
    // degrees at 150 Hz), so the device can be crossfaded against it without the notch a crossfade against the pair at
    // its working frequency would leave; the frequency is then swept up from here — and down to here before it stops.
    static constexpr float  kFloorHz = 1.0f;
    // The sweep starts once the fade at the floor is 95 % in (about 30 ms at 48 kHz; the sweep then reaches 1 % of the
    // frequency in about 46 ms more). Waiting for 99.9 % only added 40 ms: no transition tried — 95 % or 99.9 %, a 10 or
    // 20 ms sweep — did better than about -3 dB at the worst tone; the true worst is -3.47 dB (CrossoverBlendTests'
    // switch envelope: 32 phases x tones 31 … 300 Hz x crossovers 50 … 250 Hz).
    static constexpr float  kSweepAt = 0.95f;
    static constexpr double kQ = 0.7071067811865476;           // eq::Crossover2's: 1/√2 Butterworth, cascaded = LR4

    // ------------------------------------------------------------------ settings (any thread: atomics) ----
    void setHz(float hz)         { hz_.store(hz, std::memory_order_release); }
    void setAmount(float amount) { amount_.store(amount, std::memory_order_release); }
    void setCleanGain(float g)   { cleanG_.store(g, std::memory_order_release); }
    bool runningFlag() const     { return running_.load(std::memory_order_acquire); }
    int  maxLag() const          { return maxLag_; }       // host samples; set by prepare()

    // ------------------------------------------------------------------------- restarts (never beside run) ----
    // A ring long enough for a block plus the longest lag, a power of two so a tap is a mask; one block of the mixed taps
    // a plane. Sized whether or not it is on — nothing is allocated on the audio thread.
    void prepare(double fs, int maxBlock, int channels) {
        fs_ = fs; channels_ = std::clamp(channels, 1, kMaxChannels);
        maxLag_ = (int) std::ceil(kMaxLagSeconds * fs_);
        std::size_t ring = 1;
        while (ring < (std::size_t) maxBlock + (std::size_t) maxLag_ + 1u) ring <<= 1;
        for (int c = 0; c < kMaxChannels; ++c) {
            ring_[c].assign(ring, 0.0f);
            clean_[c].assign((std::size_t) maxBlock, 0.0f);
        }
        tmp_.assign((std::size_t) maxBlock, 0.0f);
        lineTmp_.assign((std::size_t) maxBlock, 0.0f);
        mask_ = ring - 1;
        fadeLen_ = std::max(1, (int) (0.010 * fs_));
        for (auto& f : lp_) f.prepare(fs_, channels_);
        for (auto& f : hp_) f.prepare(fs_, channels_);
    }
    // The filters' memory, the clean ring, the lines, the grid and the taps — what a crossover starting, or stopped by a
    // park or a restart, must not replay. No glide is touched.
    void clear() noexcept {
        for (auto& f : lp_) f.reset();
        for (auto& f : hp_) f.reset();
        for (auto& r : ring_) std::fill(r.begin(), r.end(), 0.0f);
        for (auto& l : lines_) l.clear();
        w_ = 0;
        grid_.reset();
        for (auto& t : tap_) t = Tap {};                      // the next look snaps each tap to its slot
    }
    // prepare() and reset(): the glides land where the settings stand, as the player's gains land.
    void snap() noexcept {
        const float amount = amount_.load(std::memory_order_acquire);
        run_  = amount > 0.0f;
        mix_  = run_ ? 1.0f : 0.0f;
        dryG_ = amount;
        snapSign_ = true;
        wetG_ = 1.0f - amount;
        hzRt_ = hz_.load(std::memory_order_acquire);
        design(hzRt_);
        clear();
        running_.store(run_ && ! parked_, std::memory_order_release);
    }

    // ---------------------------------------------------------------------------------------- audio thread ----
    bool running() const noexcept { return run_; }
    // Once a block, before the clean leg is rung: an amount that leaves 0 starts the crossover from silence — empty
    // filters and ring, faded in from the device alone (mix_ 0), the frequency where it is asked.
    bool begin() noexcept {
        if (! run_ && amount_.load(std::memory_order_acquire) > 0.0f) {
            run_  = true;
            mix_  = 0.0f; dryG_ = 0.0f; wetG_ = 1.0f;
            snapSign_ = true;                                 // nothing of the clean leg is heard yet
            hzRt_ = kFloorHz;                                 // faded in at the floor, then swept up (run)
            design(hzRt_);
            clear();
            running_.store(true, std::memory_order_release);
        }
        return run_;
    }
    void ring(const float* const* d, int count) noexcept {
        for (int c = 0; c < channels_; ++c) {
            float* r = ring_[c].data();
            for (int k = 0; k < count; ++k) r[(w_ + (std::size_t) k) & mask_] = d[c][k];
        }
        w_ += (std::size_t) count;
    }
    // The slot's line takes the delay the player's own line takes, at the same moment and the same way (fade 0: at once).
    void setLine(int slot, int delay, int fade) noexcept { lines_[(std::size_t) (slot & 1)].set(delay, fade); }
    // The slot's line is cleared wherever the player clears the slot's own (a landing, a retime, falling asleep), and a
    // plane's where the host stops handing it over — only while running: a start clears everything anyway.
    void clearSlotLine(int slot) noexcept {
        if (run_) lines_[(std::size_t) (slot & 1)].clear();
    }
    // PARKED (the player's stand-by): nothing of this runs, and crossoverRunning() says so until the player is back.
    void setParked(bool on) noexcept { parked_ = on; running_.store(run_ && ! on, std::memory_order_release); }
    // A PLANE THE HOST STOPS HANDING OVER leaves nothing behind: its lines, its ring and the pair's memory for it are
    // cleared on the falling edge (law 11c). The clocks and the glides are shared and go on.
    void stopPlane(int c) noexcept {
        if (! run_ || c < 0 || c >= kMaxChannels) return;
        for (auto& l : lines_) l.clearPlane(c);
        std::fill(ring_[c].begin(), ring_[c].end(), 0.0f);
        for (auto& f : lp_) f.resetChannel(c);
        for (auto& f : hp_) f.resetChannel(c);
    }
    // The clean plane after clean(): the player mixes the pack's own dry part into it with the blend knob's ramp, the
    // one it mixes into the device, so the clean leg carries the composite the device carries.
    float* cleanPlane(int c) noexcept { return clean_[c].data(); }
    // EACH SLOT'S CLEAN TAP is what its capture carries: the held model's own lag, read from the ring; the slot's
    // alignment delay is then applied by the mirrored line (clean). A new capture in the slot — a landing — is followed
    // at once, as its model is; so is any change while the slot is silent. A change of reading alone on a slot that is
    // heard (a table, setCrossoverLag) crossfades the tap over fadeLen_ rather than splice the clean leg.
    // A lag asked while a fade is still under way WAITS for it (`next`) and starts its own fade from where that one
    // lands, so the tap never jumps away from the mixture it is playing. Silent means silent at both ends of the block.
    void taps(const std::uint64_t held[2], const int own[2], const felitronics::nam::BlendStep& law) noexcept {
        for (int i = 0; i < 2; ++i) {
            auto& t = tap_[i];
            const int want = std::clamp(own[i], 0, maxLag_);
            const double w0 = i == 0 ? 1.0 - law.beginB : law.beginB, w1 = i == 0 ? 1.0 - law.endB : law.endB;
            const bool moved = held[i] != t.held;
            t.held = held[i];
            // SILENT means nothing of the slot is heard through the block: its weight zero at both ends, or the law's own
            // gain zero at both ends (a one-model warm-up holds the slot at weight 1 and gain 0).
            const bool silent = (w0 <= 0.0 && w1 <= 0.0) || (law.beginGain <= 0.0 && law.endGain <= 0.0);
            if (moved || silent) snapGain_[i] = true;   // its polarity and level land with it
            if (moved || silent) {
                if (want != t.lag || t.pos < fadeLen_ || moved) { t.from = t.lag = t.next = want; t.pos = fadeLen_; }
                continue;
            }
            t.next = want;
            if (t.pos >= fadeLen_ && t.next != t.lag) { t.from = t.lag; t.lag = t.next; t.pos = 0; }
        }
    }
    // THE CLEAN LEG, MIXED AS THE MODELS WERE: a slot that runs is tapped and delayed through its
    // mirrored line (the slot's delay of this block), a slot that does not run contributes nothing, and mixSlots makes
    // one plane of the two. A HUSHED block still advances both lines, as the slots' do under the same silence: a capture
    // fed while nothing is heard (a one-model dip) must not come back with its line empty where the slot's is full. A
    // plane the host did not hand over carries silence, as its delay lines do.
    // `invert[i]`: whether slot i's capture hands its input back inverted (its own reading, or the host's word) — the
    // slot's clean tap takes that polarity, gliding, before the slots are mixed; the pack's dry part never does.
    void clean(int nch, int count, const MixRecipe& r, const bool invert[2]) noexcept {
        const float decay = std::exp(-(float) count / (float) (0.010 * fs_));
        float signEnd[2], signStep[2];
        const float level = cleanG_.load(std::memory_order_acquire);
        for (int i = 0; i < 2; ++i) {
            // A SLOT'S GAIN on its clean tap: its capture's polarity times the clean level — the capture calibration, on
            // the model-referenced part alone (the pack's dry part joins after). It LANDS with a new capture and on a slot
            // silent at both ends; only an edit to a capture that is heard glides.
            const float want = (invert[i] ? -1.0f : 1.0f) * level;
            if (snapSign_ || snapGain_[i]) sign_[i] = want;
            snapGain_[i] = false;
            signEnd[i]  = glideEnd(want, sign_[i], decay);
            signStep[i] = (signEnd[i] - sign_[i]) / (float) count;
        }
        for (int c = 0; c < channels_; ++c) {
            float* cl = clean_[c].data();
            float* other = tmp_.data();
            const float* ring = ring_[c].data();
            if (c >= nch) { std::fill(cl, cl + count, 0.0f); continue; }
            float* slot[2] { cl, other };
            for (int i = 0; i < 2; ++i) {
                if (! r.run[i]) { std::fill(slot[i], slot[i] + count, 0.0f); continue; }
                for (int k = 0; k < count; ++k) slot[i][k] = tapAt(tap_[i], ring, count, k);
                lines_[i].process(c, slot[i], count, lineTmp_.data());
                // The slot's gain AFTER its line: a polarity or level that lands reaches what the line still holds too,
                // as the capture's own reading does on the device side (its line holds the capture's output).
                float g = sign_[i];
                for (int k = 0; k < count; ++k) { g += signStep[i]; slot[i][k] *= g; }
            }
            mixSlots(cl, other, count, r);
        }
        for (auto& t : tap_) t.pos = std::min(t.pos + count, fadeLen_);
        for (auto& l : lines_) l.advance(count);              // the slots' line clocks, in step with the player's
        sign_[0] = signEnd[0]; sign_[1] = signEnd[1];
        snapSign_ = false;
    }
    // THE SPLIT, on the audio-time grid (law 8a): per segment the glides take one step of the ~10 ms law and walk it
    // linearly, the frequency is designed where it has got to, and the filters are flushed on the boundary. Every
    // prepared plane runs — a plane the host stopped carries its zeros through, so nothing it held is replayed.
    void run(float* const* a, int count) noexcept {
        const float amount  = amount_.load(std::memory_order_acquire);
        const float wantDry = amount, wantWet = 1.0f - amount;
        const float target  = hz_.load(std::memory_order_acquire);
        for (int off = 0; off < count; ) {
            const int seg = grid_.segment(count - off);
            const float decay  = std::exp(-(float) seg / (float) (0.010 * fs_));
            // ON: faded in at the floor, swept up once the fade is (all but) in. OFF: swept down to the floor, then faded
            // out. A turn back on halfway simply sweeps up again from wherever it had got to.
            const bool on = amount > 0.0f;
            const float wantMix = on ? 1.0f : (hzRt_ <= 1.5f * kFloorHz ? 0.0f : mix_);
            const float wantHz  = on && mix_ >= kSweepAt ? target : kFloorHz;
            const float endMix = glideEnd(wantMix, mix_, decay);
            const float endDry = glideEnd(wantDry, dryG_, decay);
            const float endWet = glideEnd(wantWet, wetG_, decay);
            hzRt_ = glideEnd(wantHz, hzRt_, decay);
            if (! felitronics::core::exactlyEqual(hzRt_, hzAt_)) design(hzRt_);
            const float stepMix = (endMix - mix_) / (float) seg;
            const float stepDry = (endDry - dryG_) / (float) seg;
            const float stepWet = (endWet - wetG_) / (float) seg;
            const bool whole = isOne(mix_) && isOne(endMix);       // split in full: the sum IS the output
            for (int c = 0; c < channels_; ++c) {
                float* x = a[c] + off;
                const float* cl = clean_[c].data() + off;
                float m = mix_, gd = dryG_, gw = wetG_;
                for (int i = 0; i < seg; ++i) {
                    m += stepMix; gd += stepDry; gw += stepWet;
                    const float w  = x[i];
                    const float lo = lp_[1].processSample(c, lp_[0].processSample(c, w * gw + cl[i] * gd));
                    const float hi = hp_[1].processSample(c, hp_[0].processSample(c, w));
                    const float sum = lo + hi;
                    x[i] = whole ? sum : w + (sum - w) * m;
                }
            }
            mix_ = endMix; dryG_ = endDry; wetG_ = endWet;
            off += seg;
            if (grid_.advance(seg)) {
                for (auto& f : lp_) f.flushDenormals();
                for (auto& f : hp_) f.flushDenormals();
            }
        }
        for (auto& f : lp_) f.healPoison();             // the poison half keeps the call's clock (eq::Svf)
        for (auto& f : hp_) f.healPoison();
        // OFF once every glide has ARRIVED at the player without it (glideEnd lands exactly): from the next block on
        // nothing of this runs, and the output is the player's own, bit for bit.
        // Below 1e-9 of the device (-180 dB) the rest of the approach is inaudible: land it, rather than run the pair for
        // the ~0.7 s glideEnd takes to reach 1e-30.
        if (! (amount > 0.0f) && std::fabs(mix_) < 1e-9f && std::fabs(dryG_) < 1e-9f && std::fabs(wetG_ - 1.0f) < 1e-9f) {
            mix_ = 0.0f; dryG_ = 0.0f; wetG_ = 1.0f;
            run_ = false;
            running_.store(false, std::memory_order_release);
        }
    }

private:
    // One slot's clean tap: the lag it reads at, the lag a crossfade leaves (and how far it is), and what it last saw
    // the slot hold. `held` starts at a value no slot holds, so the first look snaps.
    struct Tap { int lag = 0, from = 0, pos = 0, next = 0; std::uint64_t held = ~std::uint64_t { 0 }; };

    // RigPlayer::rampEnd's law: an asymptotic approach that ARRIVES — the residual would otherwise park in a fixed-point
    // band, subnormal when the target is zero (law 8).
    static float glideEnd(float want, float current, float decay) noexcept {
        const float d  = (current - want) * decay;
        const float nx = want + d;
        return (std::fabs(d) < 1e-30f || felitronics::core::exactlyEqual(nx, current)) ? want : nx;
    }
    static bool isOne(float g) noexcept { return felitronics::core::exactlyEqual(g, 1.0f); }

    void design(float hz) noexcept {
        for (auto& f : lp_) f.setParams(felitronics::eq::FilterType::LowPass,  (double) hz, kQ, 0.0);
        for (auto& f : hp_) f.setParams(felitronics::eq::FilterType::HighPass, (double) hz, kQ, 0.0);
        hzAt_ = hz;
    }
    // One tap, sample k of this block: the ring read at the slot's lag, through its crossfade while one runs.
    float tapAt(const Tap& t, const float* ring, int count, int k) const noexcept {
        const std::size_t at = w_ - (std::size_t) count + (std::size_t) k;          // the block's sample k, rung
        const float now = ring[(at - (std::size_t) t.lag) & mask_];
        if (t.pos + k >= fadeLen_) return now;
        const float was = ring[(at - (std::size_t) t.from) & mask_];
        return was + (now - was) * ((float) (t.pos + k + 1) / (float) fadeLen_);
    }

    std::atomic<float> hz_ { 150.0f }, amount_ { 0.0f };
    std::atomic<float> cleanG_ { 1.0f };
    std::atomic<bool>  running_ { false };
    bool   parked_ = false, snapSign_ = true, snapGain_[2] { true, true };
    int    maxLag_ = 2048;
    double fs_ = 48000.0;
    int    channels_ = 1;
    bool   run_ = false;
    float  mix_ = 0.0f, dryG_ = 0.0f, wetG_ = 1.0f;            // the split's fade, the clean and the device's low weights
    float  sign_[2] { 1.0f, 1.0f };                            // each slot's clean gain: polarity × level, gliding
    float  hzRt_ = 150.0f, hzAt_ = 0.0f;                       // the frequency gliding, and the one designed
    int    fadeLen_ = 1;                                       // a tap's crossfade: 10 ms at fs_
    Tap    tap_[2];
    felitronics::eq::Svf lp_[2], hp_[2];
    felitronics::core::StateGrid grid_;
    std::vector<float> ring_[kMaxChannels], clean_[kMaxChannels], tmp_;
    std::size_t mask_ = 0, w_ = 0;
    SlotLine lines_[2];
    std::vector<float> lineTmp_;
};

} // namespace felitronics::rigplayer
