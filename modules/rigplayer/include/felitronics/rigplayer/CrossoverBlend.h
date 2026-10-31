// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.
#pragma once
// felitronics::rigplayer — THE CROSSOVER BLEND'S MECHANISM: a bass amp's clean low end. RigPlayer owns one and says
// when; this holds what it does — the clean ring, the two slot taps and their mirrored delay lines, the one
// Linkwitz–Riley pair that splits both sides, and the glides:
//
//     out = HP4(device) + LP4((1 − amount) · device + amount · ±clean)
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

class CrossoverBlend {
public:
    static constexpr int    kMaxChannels = 2;
    static constexpr int    kMaxDelay = felitronics::nam::kBlendMaxDelay;
    static constexpr double kMinHz = 20.0, kMaxHz = 2000.0;
    static constexpr int    kMaxLag = 2048;                    // host samples: the most the clean leg is held back
    static constexpr double kQ = 0.7071067811865476;           // eq::Crossover2's: 1/√2 Butterworth, cascaded = LR4

    // ------------------------------------------------------------------ settings (any thread: atomics) ----
    void setHz(float hz)         { hz_.store(hz, std::memory_order_release); }
    void setAmount(float amount) { amount_.store(amount, std::memory_order_release); }
    void setInvert(bool on)      { invert_.store(on, std::memory_order_release); }
    bool runningFlag() const     { return running_.load(std::memory_order_acquire); }

    // ------------------------------------------------------------------------- restarts (never beside run) ----
    // A ring long enough for a block plus the longest lag, a power of two so a tap is a mask; one block of the mixed taps
    // a plane. Sized whether or not it is on — nothing is allocated on the audio thread.
    void prepare(double fs, int maxBlock, int channels) {
        fs_ = fs; channels_ = std::clamp(channels, 1, kMaxChannels);
        std::size_t ring = 1;
        while (ring < (std::size_t) maxBlock + (std::size_t) kMaxLag + 1u) ring <<= 1;
        for (int c = 0; c < kMaxChannels; ++c) {
            ring_[c].assign(ring, 0.0f);
            clean_[c].assign((std::size_t) maxBlock, 0.0f);
        }
        tmp_.assign((std::size_t) maxBlock, 0.0f);
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
        for (auto& slot : tail_) for (auto& t : slot) t.fill(0.0f);
        w_ = 0;
        grid_.reset();
        for (auto& t : tap_) t = Tap {};                      // the next look snaps each tap to its slot
    }
    // prepare() and reset(): the glides land where the settings stand, as the player's gains land.
    void snap() noexcept {
        const float amount = amount_.load(std::memory_order_acquire);
        const float sign = invert_.load(std::memory_order_acquire) ? -1.0f : 1.0f;
        run_  = amount > 0.0f;
        mix_  = run_ ? 1.0f : 0.0f;
        dryG_ = amount * sign;
        wetG_ = 1.0f - amount;
        hzRt_ = hz_.load(std::memory_order_acquire);
        design(hzRt_);
        clear();
        running_.store(run_, std::memory_order_release);
    }

    // ---------------------------------------------------------------------------------------- audio thread ----
    bool running() const noexcept { return run_; }
    // Once a block, before the clean leg is rung: an amount that leaves 0 starts the crossover from silence — empty
    // filters and ring, faded in from the device alone (mix_ 0), the frequency where it is asked.
    bool begin() noexcept {
        if (! run_ && amount_.load(std::memory_order_acquire) > 0.0f) {
            run_  = true;
            mix_  = 0.0f; dryG_ = 0.0f; wetG_ = 1.0f;
            hzRt_ = hz_.load(std::memory_order_acquire);
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
    // The slot's line is cleared wherever the player clears the slot's own (a landing, a retime, falling asleep), and a
    // plane's where the host stops handing it over — only while running: a start clears everything anyway.
    void clearSlotLine(int slot) noexcept {
        if (run_) for (auto& t : tail_[(std::size_t) (slot & 1)]) t.fill(0.0f);
    }
    void clearPlaneLines(int c) noexcept {
        if (run_) for (auto& slot : tail_) slot[(std::size_t) c].fill(0.0f);
    }
    // EACH SLOT'S CLEAN TAP is what its capture carries: the held model's own lag, read from the ring; the slot's
    // alignment delay is then applied by the mirrored line (clean). A new capture in the slot — a landing — is followed
    // at once, as its model is; so is any change while the slot is silent. A change of reading alone on a slot that is
    // heard (a table, setCrossoverLag) crossfades the tap over fadeLen_ rather than splice the clean leg.
    void taps(const std::uint64_t held[2], const int own[2], const felitronics::nam::BlendStep& law) noexcept {
        for (int i = 0; i < 2; ++i) {
            auto& t = tap_[i];
            const int want = std::clamp(own[i], 0, kMaxLag);
            const double w = i == 0 ? 1.0 - law.endB : law.endB;
            const bool moved = held[i] != t.held;
            t.held = held[i];
            if (want == t.lag && ! moved) continue;
            if (moved || w <= 0.0) { t.from = t.lag = want; t.pos = fadeLen_; }
            else                   { t.from = t.lag; t.lag = want; t.pos = 0; }
        }
    }
    // THE CLEAN LEG, MIXED AS THE MODELS WERE: a slot that runs is tapped and delayed by nam::blendDelay through its
    // mirrored line (the slot's delay of this block), a slot that does not run contributes nothing, and mixSlots makes
    // one plane of the two. A HUSHED block still advances both lines, as the slots' do under the same silence: a capture
    // fed while nothing is heard (a one-model dip) must not come back with its line empty where the slot's is full. A
    // plane the host did not hand over carries silence, as its delay lines do.
    void clean(int nch, int count, const MixRecipe& r, const int delay[2]) noexcept {
        for (int c = 0; c < channels_; ++c) {
            float* cl = clean_[c].data();
            float* other = tmp_.data();
            const float* ring = ring_[c].data();
            if (c >= nch) { std::fill(cl, cl + count, 0.0f); continue; }
            float* slot[2] { cl, other };
            for (int i = 0; i < 2; ++i) {
                if (! r.run[i]) { std::fill(slot[i], slot[i] + count, 0.0f); continue; }
                for (int k = 0; k < count; ++k) slot[i][k] = tapAt(tap_[i], ring, count, k);
                felitronics::nam::blendDelay(slot[i], tail_[i][(std::size_t) c].data(), kMaxDelay, delay[i], count);
            }
            mixSlots(cl, other, count, r);
        }
        for (auto& t : tap_) t.pos = std::min(t.pos + count, fadeLen_);
    }
    // THE SPLIT, on the audio-time grid (law 8a): per segment the glides take one step of the ~10 ms law and walk it
    // linearly, the frequency is designed where it has got to, and the filters are flushed on the boundary. Every
    // prepared plane runs — a plane the host stopped carries its zeros through, so nothing it held is replayed.
    void run(float* const* a, int count) noexcept {
        const float amount  = amount_.load(std::memory_order_acquire);
        const float sign    = invert_.load(std::memory_order_acquire) ? -1.0f : 1.0f;
        const float wantHz  = hz_.load(std::memory_order_acquire);
        const float wantMix = amount > 0.0f ? 1.0f : 0.0f, wantDry = amount * sign, wantWet = 1.0f - amount;
        for (int off = 0; off < count; ) {
            const int seg = grid_.segment(count - off);
            const float decay  = std::exp(-(float) seg / (float) (0.010 * fs_));
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
        if (! (amount > 0.0f) && ! (mix_ > 0.0f || mix_ < 0.0f) && ! (dryG_ > 0.0f || dryG_ < 0.0f) && isOne(wetG_)) {
            run_ = false;
            running_.store(false, std::memory_order_release);
        }
    }

private:
    // One slot's clean tap: the lag it reads at, the lag a crossfade leaves (and how far it is), and what it last saw
    // the slot hold. `held` starts at a value no slot holds, so the first look snaps.
    struct Tap { int lag = 0, from = 0, pos = 0; std::uint64_t held = ~std::uint64_t { 0 }; };

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
    std::atomic<bool>  invert_ { false }, running_ { false };
    double fs_ = 48000.0;
    int    channels_ = 1;
    bool   run_ = false;
    float  mix_ = 0.0f, dryG_ = 0.0f, wetG_ = 1.0f;            // the split's fade, the clean and the device's low weights
    float  hzRt_ = 150.0f, hzAt_ = 0.0f;                       // the frequency gliding, and the one designed
    int    fadeLen_ = 1;                                       // a tap's crossfade: 10 ms at fs_
    Tap    tap_[2];
    felitronics::eq::Svf lp_[2], hp_[2];
    felitronics::core::StateGrid grid_;
    std::vector<float> ring_[kMaxChannels], clean_[kMaxChannels], tmp_;
    std::size_t mask_ = 0, w_ = 0;
    std::array<float, (std::size_t) kMaxDelay> tail_[2][kMaxChannels] {};
};

} // namespace felitronics::rigplayer
