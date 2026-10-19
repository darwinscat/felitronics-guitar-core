// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// fc_rig — the translation unit of the fcrig module (tools/wasm-rig/build.sh), and of the native library the ctest
// suite links (tools/wasm-rig/CMakeLists.txt). The contract is in tools/fc_rig_abi.h. Every entry point is one
// RigPlayer call or a step of the load job; no DSP lives here. In the wasm build this file is compiled WITHOUT
// exceptions — only the manifest parser (fc_rig_manifest.cpp) and the NAM objects have them.

#include "fc_rig_abi.h"
#include "fc_rig_manifest.h"

#include <felitronics/rigplayer/RigPlayer.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(__EMSCRIPTEN__)
  #include <emscripten/emscripten.h>
  #define FC_EXPORT extern "C" EMSCRIPTEN_KEEPALIVE
#else
  #define FC_EXPORT extern "C"
#endif

namespace
{
using felitronics::rigplayer::RigPlayer;

constexpr int kMaxBlock = 65536;                   // the module's own buffers are maxBlock floats per channel
constexpr int kMaxFileBytes = 256 * 1024 * 1024;   // one capture; a WaveNet .namz is some 60 KB

inline double clamp01 (double v) noexcept { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
} // namespace

struct fc_rig
{
    RigPlayer player;
    std::optional<RigPlayer::LoadJob> job;      // taken, not yet run
    std::optional<RigPlayer::Loaded>  loaded;   // run, not yet delivered
    std::vector<std::byte> handed;              // the bytes the caller handed in for the job out
    std::vector<float> io[2];
    int  channels = 0;
    bool prepared = false;

    /** What the player's ModelSource answers: the bytes handed in for the job being run, taken — RigPlayer::run asks
        once, on this thread, inside fc_rig_job_run. Nothing handed in is an empty answer, which the player reads as
        "not a model" and reports to its law as a failed load. */
    std::vector<std::byte> source() { return std::exchange (handed, {}); }

    /** A job out when the pack goes: brought back for the player to DROP (its generation is the old pack's), so the
        player hands out jobs again — RigPlayer offers none while one is out. Called after the player forgot the pack,
        never before: delivered first, the failure would land in the law of the pack about to go. */
    void dropJob()
    {
        if (job)
        {
            RigPlayer::Loaded stale;
            stale.job = std::move (*job);
            job.reset();
            player.deliver (std::move (stale));
        }
        if (loaded)
        {
            player.deliver (std::move (*loaded));
            loaded.reset();
        }
        handed = {};
    }

    const std::string* jobFile() const
    {
        if (job) return &job->fileId;
        if (loaded) return &loaded->job.fileId;
        return nullptr;
    }
};

FC_EXPORT uint32_t fc_rig_abi_version (void) { return FC_RIG_ABI_VERSION; }

FC_EXPORT fc_rig* fc_rig_create (void) { return new (std::nothrow) fc_rig(); }

FC_EXPORT void fc_rig_destroy (fc_rig* rig) { delete rig; }

FC_EXPORT int fc_rig_prepare (fc_rig* rig, double sampleRate, int maxBlock, int channels)
{
    if (rig == nullptr) return 0;
    rig->prepared = false;   // disarm first: a refusal leaves nothing half-prepared playing
    if (! (sampleRate >= 1000.0 && sampleRate <= 768000.0) || maxBlock < 1 || maxBlock > kMaxBlock
        || channels < 1 || channels > 2)
        return 0;
    if (! rig->player.prepare (sampleRate, maxBlock, channels)) return 0;
    for (int c = 0; c < 2; ++c)
        rig->io[c].assign (c < channels ? (std::size_t) maxBlock : 0u, 0.0f);
    rig->channels = channels;
    rig->prepared = true;
    return 1;
}

FC_EXPORT float* fc_rig_buffer (fc_rig* rig, int channel)
{
    if (rig == nullptr || ! rig->prepared || channel < 0 || channel >= rig->channels) return nullptr;
    return rig->io[channel].data();
}

FC_EXPORT void* fc_rig_alloc (int bytes) { return bytes > 0 ? ::operator new ((std::size_t) bytes, std::nothrow) : nullptr; }

FC_EXPORT void fc_rig_free (void* p) { ::operator delete (p); }

FC_EXPORT int fc_rig_load (fc_rig* rig, const char* manifest, int length)
{
    if (rig == nullptr) return 0;
    rig->player.unload();
    rig->dropJob();
    if (manifest == nullptr || length <= 0) return 0;
    namz::rig::Rig parsed;
    if (! fcRigParseManifest (manifest, (std::size_t) length, parsed)) return 0;
    const bool loaded = rig->player.load (parsed, [rig] (const std::string&) { return rig->source(); });
    return loaded ? 1 : 0;
}

FC_EXPORT void fc_rig_unload (fc_rig* rig)
{
    if (rig == nullptr) return;
    rig->player.unload();
    rig->dropJob();
}

FC_EXPORT int fc_rig_take_job (fc_rig* rig)
{
    if (rig == nullptr) return 0;
    rig->player.service();
    if (rig->job || rig->loaded) return 0;   // one out at a time
    auto job = rig->player.takeLoadJob();
    if (! job) return 0;
    rig->job = std::move (job);
    rig->handed = {};
    return 1;
}

FC_EXPORT const char* fc_rig_job_file (fc_rig* rig)
{
    const auto* f = rig != nullptr ? rig->jobFile() : nullptr;
    return f != nullptr ? f->c_str() : nullptr;
}

FC_EXPORT int fc_rig_job_file_length (fc_rig* rig)
{
    const auto* f = rig != nullptr ? rig->jobFile() : nullptr;
    return f != nullptr ? (int) f->size() : 0;
}

FC_EXPORT int fc_rig_job_needs_bytes (fc_rig* rig)
{
    return rig != nullptr && rig->job && rig->job->bytes == nullptr ? 1 : 0;
}

FC_EXPORT uint8_t* fc_rig_job_bytes (fc_rig* rig, int size)
{
    if (fc_rig_job_needs_bytes (rig) == 0 || size < 1 || size > kMaxFileBytes) return nullptr;
    rig->handed.assign ((std::size_t) size, std::byte { 0 });
    return reinterpret_cast<uint8_t*> (rig->handed.data());
}

FC_EXPORT int fc_rig_job_run (fc_rig* rig)
{
    if (rig == nullptr || ! rig->job) return -1;
    rig->loaded = RigPlayer::run (std::move (*rig->job));
    rig->job.reset();
    rig->handed = {};   // handed in for a job that carried its own bytes: never asked for
    return rig->loaded->model != nullptr ? 1 : 0;
}

FC_EXPORT int fc_rig_job_deliver (fc_rig* rig)
{
    if (rig == nullptr || ! rig->loaded) return 0;
    const bool model = rig->loaded->model != nullptr;
    rig->player.deliver (std::move (*rig->loaded));
    rig->loaded.reset();
    return model ? 1 : 2;
}

FC_EXPORT int fc_rig_set_dial (fc_rig* rig, const char* name, double degrees)
{
    if (rig == nullptr || name == nullptr || ! std::isfinite (degrees)) return 0;
    return rig->player.setDial (name, degrees) ? 1 : 0;
}

FC_EXPORT int fc_rig_set_switch (fc_rig* rig, const char* name, const char* value)
{
    if (rig == nullptr || name == nullptr || value == nullptr) return 0;
    return rig->player.setSwitch (name, value) ? 1 : 0;
}

FC_EXPORT int fc_rig_set_gain_dial (fc_rig* rig, double degrees)
{
    if (rig == nullptr || ! std::isfinite (degrees) || rig->player.dialName().empty()) return 0;
    return rig->player.setDial (rig->player.dialName(), degrees) ? 1 : 0;
}

FC_EXPORT double fc_rig_gain_dial_sweep (fc_rig* rig)
{
    return rig != nullptr && ! rig->player.dialName().empty() ? (double) rig->player.dialSweep() : 0.0;
}

FC_EXPORT int fc_rig_set_max_models (fc_rig* rig, int n)
{
    return rig != nullptr && rig->player.setMaxModels (n) ? 1 : 0;
}

FC_EXPORT void fc_rig_set_normalize (fc_rig* rig, int on)
{
    if (rig != nullptr) rig->player.setNormalize (on != 0);
}

FC_EXPORT void fc_rig_set_input_trims (fc_rig* rig, int on)
{
    if (rig != nullptr) rig->player.setInputTrims (on != 0);
}

FC_EXPORT void fc_rig_set_blend_shape (fc_rig* rig, double point, double width)
{
    if (rig == nullptr || ! std::isfinite (point) || ! std::isfinite (width)) return;
    rig->player.setBlendShape ({ clamp01 (point), clamp01 (width) });
}

FC_EXPORT int fc_rig_process (fc_rig* rig, float* left, float* right, int n)
{
    if (rig == nullptr || ! rig->prepared || n < 0 || left == nullptr) return 0;
    const int channels = right != nullptr ? 2 : 1;
    if (channels > rig->channels) return 0;
    if (n == 0) return 1;
    float* io[2] { left, right };
    return rig->player.process (io, channels, n) ? 1 : 0;
}

FC_EXPORT void fc_rig_reset (fc_rig* rig)
{
    if (rig != nullptr) rig->player.reset();
}

FC_EXPORT double fc_rig_sounding_db (fc_rig* rig) { return rig != nullptr ? rig->player.soundingLoudness().db : 0.0; }
FC_EXPORT int fc_rig_sounding_tagged (fc_rig* rig) { return rig != nullptr && rig->player.soundingLoudness().tagged ? 1 : 0; }
FC_EXPORT int fc_rig_sounding_blended (fc_rig* rig) { return rig != nullptr && rig->player.soundingLoudness().blended ? 1 : 0; }
FC_EXPORT int fc_rig_held (fc_rig* rig, int slot)
{
    return rig != nullptr && (slot == 0 || slot == 1) && ! rig->player.heldFileId (slot).empty() ? 1 : 0;
}
FC_EXPORT int fc_rig_latency_samples (fc_rig* rig) { return rig != nullptr ? rig->player.latencySamples() : 0; }
FC_EXPORT int fc_rig_run_blocks (fc_rig* rig, int slot)
{
    return rig != nullptr && (slot == 0 || slot == 1) ? rig->player.runBlocks (slot) : 0;
}
FC_EXPORT int fc_rig_pair_blocks (fc_rig* rig) { return rig != nullptr ? rig->player.pairBlocks() : 0; }
FC_EXPORT int fc_rig_warm_blocks (fc_rig* rig) { return rig != nullptr ? rig->player.warmBlocks() : 0; }
FC_EXPORT void fc_rig_clear_counters (fc_rig* rig)
{
    if (rig != nullptr) rig->player.clearCounters();
}

// ---- ABI 2 -----------------------------------------------------------------------------------------------------------
FC_EXPORT int fc_rig_scope_read (fc_rig* rig, float* dry, float* wet, int n)
{
    return rig != nullptr ? rig->player.scopeRead (dry, wet, n) : 0;
}
FC_EXPORT double fc_rig_scope_frames (fc_rig* rig) { return rig != nullptr ? (double) rig->player.scopeFrames() : 0.0; }
FC_EXPORT void fc_rig_set_standby (fc_rig* rig, int on)
{
    if (rig != nullptr) rig->player.setStandby (on != 0);
}
FC_EXPORT int fc_rig_set_feed_db (fc_rig* rig, double db)
{
    // RigPlayer::setFeedDb: the feed in front of the device — after the stand-by wire, before the scope's dry and the
    // pack's input level — kept across loads by the player itself. v0.5.0 stated it as the host's whole input level
    // (setHostInputDb (stageInputDb() + db), restated after every load); the player sums the same two numbers in the
    // same order, so the sound is that spelling's byte for byte, and only the player can put the scope's dry between.
    return rig != nullptr && rig->player.setFeedDb (db) ? 1 : 0;
}

// ---- ABI 4 -----------------------------------------------------------------------------------------------------------
FC_EXPORT int fc_rig_set_preload (fc_rig* rig, int maxModels)
{
    if (rig == nullptr || maxModels < 0) return 0;
    return rig->player.setPreload (maxModels > 0, maxModels) ? 1 : 0;
}
FC_EXPORT int fc_rig_preload_pending (fc_rig* rig)
{
    if (rig == nullptr) return 0;
    const auto p = rig->player.preloadProgress();
    return p.total - p.settled;
}
FC_EXPORT int fc_rig_preload_total (fc_rig* rig) { return rig != nullptr ? rig->player.preloadProgress().total : 0; }
FC_EXPORT int fc_rig_preload_failed (fc_rig* rig) { return rig != nullptr ? rig->player.preloadProgress().failed : 0; }
FC_EXPORT int fc_rig_is_gain_dial (fc_rig* rig, const char* name)
{
    return rig != nullptr && name != nullptr && ! rig->player.dialName().empty() && rig->player.dialName() == name ? 1 : 0;
}
