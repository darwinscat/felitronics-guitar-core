<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Changelog

## v0.7.1 — 2026-10-21

The echo and the room no longer click when they stand by — felitronics-core pinned to v0.61.2.

### Fixed
- **fcguitarfx: a stand-by flip no longer ticks in the repeats or clicks into the room.** The delay's and the
  reverb's `feed` (an amp's PARK, DELAY or REVERB off) switched what enters the line at the call boundary; core
  v0.61.2 fades it over 5 ms. Delay: the largest step where the repeats replay the flip falls from 13-16x the steady
  echo's own to about 1x; Plate: the click above 4 kHz from about -24 dB to -64…-70 dB. With `feed` held the render
  is bit-identical to v0.7.0's.

## v0.7.0 — 2026-10-20

A load at a fraction of its cost — what plays is bit-identical to v0.6.0.

### Changed
- **`NamStage` builds a `.namz` straight into NAM** (`modules/nam/src/DirectBuild.{h,cpp}`): `namz::decode` (namz
  v4.3.0 and later; the pin is v4.4.0) hands over the parsed skeleton and the float32 weights, and NAM's own `parse_model_config_json` + `create_dsp`
  build the network from them — a SlimmableContainer assembled from its submodels as `ContainerConfig::create` does.
  The weights are no longer printed to JSON text and parsed back (1.0 + ~10 ms of a 22 ms load in a wasm build).
  Direct: Linear, WaveNet whose config holds no capture of its own, SlimmableContainer of those (A2, every real
  `.orbitrig` capture). JSON path, unchanged: raw `.nam` JSON, LSTM, ConvNet, conditioned WaveNet, a container holding
  any of those. The gate (`felitronics_nam_load_tests`) holds every NAM v0.5.4 example model, the Linears and — with
  `FELITRONICS_GUITAR_RIG_PACKS` — every real capture to bit equality with `get_dsp`, with a one-ulp control per fixture.
- **Mono: one network per load on a mono host.** `NamStage::prepare (sampleRate, maxBlock, numChannels)` and
  `prepareModel (…, numChannels, warm)`; `RigPlayer::LoadJob::channels` carries the player's. A mono model keeps its
  bytes and builds its second network when the host re-prepares for two channels; a stereo call on a mono model is
  refused. `NamStage::networksOf()` reads it, `NamStage::liveNetworks()` the live model's. A recurrent capture (LSTM)
  always gets two networks — a second one built later would start from another history and play other numbers on the
  same input. `loadModelFromMemory` builds for the stage's channel count. A new `prepareModel (shared_ptr<const
  vector<byte>>, …)` keeps the caller's bytes for the second network instead of a copy; `RigPlayer::run` uses it.
- **No NAM zero-prewarm on RigPlayer's loads** (`NamStage::Warm::Caller`) for a non-recurrent capture: no slot is heard
  before `warmFor()` of live input covers the receptive field. `loadModelFromMemory` and recurrent captures keep NAM's
  prewarm; the one block of silence that grows NAM's buffers still runs; a later re-prepare prewarms as before.
- Measured (node, the real ABI, v0.6.0 → v0.7.0): one copy 24–28 ms → 0.35–0.46 ms; the whole-pack preload 261–310 →
  3.9–5 ms (X88-IR Crunch, V4 Sheriff Ch1, 11 copies) and 1584–1849 → 24 ms (IR-X Ch1 PowerAmp, 66); module memory
  41.6 → 28.9 MiB and 179.4 → 103.75 MiB (2.35 → 1.21 MiB per copy; the module's size moves in emscripten's growth
  steps, so the ~3.9 MiB of IR-X file copies the shared bytes save do not show in it). These memory figures are
  `rig-compare.mjs`'s, which drives the ABI directly; the worklet in the parity run (`rig-parity.mjs`) holds the
  11-copy packs in 24.1 MiB, as much as without the set, and IR-X Ch1 PowerAmp in 103.8 MiB. 36 sessions (3 packs ×
  preload off/on × blend, dip, PARK, dip+PARK at 48 kHz, and blend, dip at 44.1 kHz; the gain dial over 13 positions)
  identical bit for bit (`tools/wasm-rig/rig-compare.mjs`), 3 one-ulp controls differ.
- **The worklet fills the preload in the background** (`processorOptions.preload`): the start no longer waits for the
  whole set — the law's ask goes first, so the start sounds as soon as the capture it needs is held, warmed and timed,
  and the rest of the set is filled while it plays, at most one job between two renders (a job whose answer arrives
  after one already ran waits for the next quantum). v0.6.0 prepared the whole set first, in silence: 0.35–2.3 s at
  PLAY and at every channel. Measured (Playwright, 48 kHz, IR-X Ch1 PowerAmp, each `{ need }` answered after 40 ms):
  the first sound at 261 ms in WebKit and 248 ms in Chromium (v0.6.0: 2656 and 3981 ms), the set filled ~2.2 s later
  while it played, no render quantum after the first sound later than 12 ms. The messages keep their shapes; what moves is the
  order: `{ preloaded }` usually comes after `{ calibrated }` now, nothing waits for it, and `quanta` leaves PARK out.
  With `calibrate: false` a knob lands at once while the set fills (it waited for `{ preloaded }`). A page should hand
  the pack's files in with it (`{ load, files }`): the player hands out one job at a time, so a fill `{ need }` out
  holds the law's next ask back by its round trip.
- **PARK is a bypass, and nothing loads under it**: no job runs and no `{ need }` is posted while the player stands by
  (v0.6.0 ran every job of the set under it, and posted its `{ need }`s); an answer that arrives meanwhile is kept and
  its job runs at the first quantum back; the fill resumes there.
- A capture at 12 o'clock that fails to load ends the start at once with the preload too (`reason: 'load-failed'`; it
  waited for the whole set and 16 quanta more); a copy of the fill that fails does not end it.

### Added
- **namz v4.4.0, and the float16 captures it reads** (`modules/nam/CMakeLists.txt`: `FELITRONICS_NAMZ_TAG` v4.1.0 →
  v4.4.0, `215d164`; v0.7.0's `namz::decode` came in 4.3.0). A `.nam16z` — weights stored as IEEE-754 halves, namz's
  lossy dtype 1, written only by `namz encode --f16` — loads and plays: namz widens each half to its float32 and the
  stage builds from those, so it plays BIT FOR BIT what the float32 `.namz` of the same halves widened plays. Proved
  natively on every NAM example model, the Linears and every real capture named (`felitronics_nam_load_tests`, 242 files
  with the site's packs, each with a control: the float32 original plays other samples), and through the module, the
  worklet and `fc_rig_render` on the fixture pack and the three site packs made `.nam16z` by namz's own CLI
  (`rig-parity.mjs --namz=`, `fc_rig_namz`, built from the pin when the parity run is configured). The worklet takes a
  `.nam16z` by the name rig.json gives it — asked for by `{ need }` or handed in up front — and the same bytes named
  `.namz` play the same: the dtype byte decides, never the name. On the v4.3.0 pin every `.nam16z` is refused.
- **The fetched namz is held to its commit**, as Eigen is: `FELITRONICS_NAMZ_COMMIT` (`215d164`, the commit
  THIRD_PARTY_NOTICES.md names). A tag that moved, or `FELITRONICS_NAMZ_TAG` set to another release without its commit,
  stops the configure with both commits named; a local checkout (`FELITRONICS_NAMZ_SRC`) is not held.
- **fc_rig ABI 5**: `fc_rig_put_file (rig, id, size)` — the pack's files handed in up front; a job for them never needs
  bytes. `RigPlayer::putFile`. The worklet takes `{ files: [{ file, bytes }, …] }` with `{ load }` or after it (and
  `processorOptions.files`); `{ need }` stays for any file not handed in, and a `{ need }` out is answered by `{ files }`.
  ABI 5 also marks the change of what `fc_rig_job_run` does (mono, no prewarm); the worklet refuses a module below 5.
  `RigPlayer::wantsFile (id)`: `fc_rig_put_file` asks it before it allocates, so a bogus id or a held file costs no memory.

### Fixed in review (before the release)
- A `.namz` with a NaN or infinite weight loaded through the direct build and played NaN; v0.6.0's JSON path refused it
  (NAM's reader throws on the `null` unpack prints). The direct build refuses it too.
- A recurrent capture loaded for a mono host and re-prepared for two channels played different L and R (0.055 vs 0.161
  on the decaying-cell fixture): both networks are built with the load now.
- The worklet: a `{ files, pack }` before the module was up skipped the pack check; a setup that failed kept the files
  it was handed.
- `tools/wasm-rig/build.sh` restated the dependency sources on every run but not the pins: a build directory configured
  once with another namz tag kept it from its cache and built that tag silently (one cached at v4.3.0 shipped a module
  that refuses every `.nam16z`). It drops the pins' cache entries on every run now, so the tags the CMakeLists name win.

### Changed (the start)
- **No burst, no hold: PLAY sounds after ~55–65 ms, a channel after the dip and the warm-up** (`fcrig-worklet.js`). The start
  times the player's own live renders of the warm-up (`Date.now` around each render that ran one network alone) and
  decides when the sound opens, posting `{ calibrated }` in that quantum — the riff the page starts on it is heard at
  once (it lost ~80 ms to CAL_HOLD). One decision a session per architecture (`fc_rig_signature`: the layer shape, never
  weights or trained numbers); `{ calibrate: 'again' }` forgets them. The gain dial is no longer forced to 12 o'clock:
  the knobs land at once, the start times the DRIVE's own capture, and a channel whose DRIVE is elsewhere no longer plays
  the 12-o'clock capture first. Only `maxModels` waits for the decision; a capture that opens before 8 renders were
  timed says it 8 quanta later at most. A one-model decision is SETTLED: timed on after the open (64 renders of one
  network alone, from an open) and decided again for the architecture's next start, so a start's dozen readings — the
  module's coldest — decide one pack at most. `{ calibrate: 'again' }` while two models sound is answered on 64 renders,
  not on what fit in 64 quanta. Node harness, three site packs: PLAY opens at quantum 19–23 (53–64 ms) and says so
  there; a channel change mid-riff after 57 quanta (152 ms: the dip and the live warm-up); the longest quantum of the
  start 0.4 ms (the burst: 30.8 ms).
- **The quiet warm** (`RigPlayer::warmQuiet`, `fc_rig_warm_quiet`, ABI 5): when the models' input has been exactly zero
  for a warming capture's whole warm-up (a start, `reset()`, a return from PARK, a pause), the player feeds it blocks of
  silence ahead of real time, bounded per call by the host — the worklet by an upper bound on a render's cost, kept per
  architecture, and no job in a quantum that warmed, an answer's included — so it opens after a few quanta; what it is then heard with
  is bit-identical to the live warm-up on silence from the moment that would have opened (RigAbiTests, rig-parity, with
  controls). Mid-riff nothing changes. A host that never calls it plays as before (OrbitAmp).
- ABI 5 also gains `fc_rig_audible` (the law lets a capture be heard) and `fc_rig_signature(slot)`;
  `NamStage::architectureSignature()` and `RigPlayer::{audible, heldSignature, warmQuiet}` behind them.

### Fixed
- **The dropout right after every sound start in WebKit** (PLAY, every pack swap): the calibration burst (30 ms of
  renders inside one render quantum) made WebKit's output stall twice — at the burst, in silence, and again 24–31 ms
  later, after the start had released its silence. Measured with Playwright (a probe after the processor timing every
  render quantum): with calibration off no gap at all; with it, gaps at −2.7 ms and +29 ms from the first sound. The
  start now holds its silence for `ceil(CAL_HOLD_MS / quantum)` quanta (80 ms; 30 at 48 kHz) past the burst and opens on
  a 5 ms raised-cosine fade. Measured on the shipped build (WebKit and Chromium, X88-IR Crunch and IR-X Ch1 PowerAmp,
  two runs each): WebKit's two gaps at −82.7 ms (33–34 ms long, the burst) and −50.7 ms (23–24 ms) from the first sound,
  so the second one ends ~27 ms before it; Chromium's one gap (36–37 ms) at −82.7 ms; nothing after the first sound in
  either; no gap at all with calibration off. The first sound comes ~80 ms later. **Superseded by "No burst, no hold"
  above:** the burst, the hold and the fade are gone; a start's longest quantum is ~0.7 ms in node.

## v0.6.0 — 2026-10-19

### rigplayer — the preload: a turn of the gain dial that never waits for a load

- `RigPlayer::setPreload(on, maxModels = kPreloadWholePack)` (off by default — the player as before, bit for bit), from
  the next `load()`: the player hands out jobs (the same `LoadJob`, `run()`, `deliver()`) for its SET until every copy
  in it is prepared, and from then on neither a turn of the gain dial nor a switch within the set hands out one — a
  slot takes its prepared copy at the next `service()`, the instant a delivered job would have landed it, and the law
  warms and crossfades (or dips) it as ever. THE SET: the panel sounding — every (capture, slot) the plan can name along
  the dial (asked of the plan itself at every knot, between every two and at both ends, two models and one), one copy
  per (capture, slot) since both slots hold one file at the dial's ends and across a link: **11 copies for the site's
  packs of eleven positions and nine files** — then the pack's other panels nearest first; by default every panel of
  the pack. `preloadProgress()` reads it out. No preload job before `prepare()`.
- Under a cap, a switch to a panel outside the set makes the set again around it and hands out only what the law asks
  for — never more jobs than the player without the preload — unless the host says jobs cost nothing now
  (`setPreloadIdle(true)`). An unfetched file is fetched again when the law asks for it. Those are the only jobs after
  ready. A panel is told apart by its knots' files and the other controls' settings, so a pack with no gain dial has
  panels too.
- A copy is freed whole — its `NamStage` with its capture — when it leaves the set or the pack goes: at once if no slot
  ever ran it, otherwise by the first `service()` after the audio thread has entered `process()` again (an audio clock
  `process()` ticks before it reads a slot's stage). A stage is never cleared and reused: `NamStage` frees what it
  retires only from its own `process()`. On the fixture, unload leaves 0.3 % of what the pack and its set held, and 200
  evicting switches grow the heap by 16 KB (a set is 509 KB).
- Memory: one copy is one `NamStage` holding its capture — **2.8 MB** natively and in the wasm module for the site's
  WaveNets (NamStage builds two NAM instances, stereo). The whole pack: 11 copies (~31 MB) for X88-IR Crunch and V4
  Sheriff Ch1, 44 (~123 MB) for IR-X Ch2, 66 (~185 MB) for IR-X Ch1 PowerAmp; the wasm module read 41.6 MiB with X88-IR
  Crunch preloaded against 24.1 MiB without, and 179.4 MiB with IR-X Ch1 PowerAmp.
- A copy taken again holds its last visit; a capture with a field is warmed past it by the law, and one with none is
  drained (`NamStage::reset`, a memoryless capture's matcher) before it lands — without that, off the model rate it
  replayed its last visit's tail (0.0139 at 44.1 kHz on the fixture; 5.5e-6 with it, the matcher's resumption floor).
  At the model rate the preloaded player is bit for bit the plain one on the same dial path for WaveNets and direct
  Linears, failures included; a dense `Linear` on NAM's partitioned FFT keeps its engine's clock (8.1e-8 on a 2001-tap
  fixture, guarded at 1e-6), and a recurrent (LSTM) copy starts from its last visit (0.054 on the decaying-cell fixture).

### fc_rig ABI 4 and the processor

- `fc_rig_set_preload(n)` (0 off, n ≥ 1 on with at most n copies, 2147483647 the whole pack), `fc_rig_preload_pending`,
  `fc_rig_preload_total`, `fc_rig_preload_failed`, `fc_rig_is_gain_dial`. `FC_RIG_ABI_VERSION` 4; the processor needs 4.
- `fcrig-worklet.js`: `processorOptions.preload` (`true` = the whole pack, or a cap): at every pack the set is prepared
  through the same `{ need }`, one job per quantum, SILENT; `{ preloaded: { pack, total, failed, quanta } }`; then the
  start's calibration and play. While it preloads, switches, dials and levels land at once — `processorOptions.knobs`
  and a `{ set }` posted before the module was up included — so the set is made around the panel asked for; the gain
  dial (`gain`, or its name in `dial` or `switch`) and maxModels wait with the start.
- Measured in Chromium 151 (Playwright, 'playback', 48 kHz, X88-IR Crunch, a continuous turn 0 → 300 → 0 at a degree
  per 16 ms): without the preload 8 – 9 underruns (171 – 192 ms) in the first turn and 10 – 14 more in the second
  (the bytes kept, the loads still run); with it 0 and 0 — the 2 – 4 it costs (43 – 85 ms) fall in the silent start.
  In Playwright's Firefox 153 build one capture ran at 70 – 75 % of a quantum and a load took 305 – 315 ms, the same in a
  plain Worker as in the worklet (whether the module was compiled from bytes or posted as a Module was not recorded);
  that build calibrated to one model and lost 0.45 s of audio clock per DRIVE step without the preload, 0.13 s with it.
  Whether a release Firefox, with the module compiled from bytes, is as slow is not measured here — the site's own
  record (round 13) reads 259 – 300 µs a render there, 10 % of a quantum.

## v0.5.1 — 2026-10-19

### rigplayer — the scope taps where the app taps

- `RigPlayer::scopeRead` now draws OrbitAmp's own two points, around the whole player (`CapturedBlock.h:543-556`: the
  block's input copied before the drive and `player.process`, the scope written after it). **dry** = the input as the
  block was handed it, times the feed and nothing else — before the pack's input level and the pre tone, UNDELAYED;
  **wet** = the player's output — pre tone, models, post tone, blend knob, output level — before the stand-by fade (the
  app's `BypassFade` blends after the block has tapped, `PluginProcessor.cpp:1538`). A pack's post tone and blend knob
  now move the picture; its input level no longer reaches the dry. As in the app, the wet trails the dry by a capture's
  own lag and, at a host rate other than the pack's, by `latencySamples()` (61 samples at 44.1 kHz against 48 kHz
  captures) — v0.5.0 held the dry back to align them; the app does not, and this follows the app. Kept: written only
  while the device runs (frozen while it stands by), the torn-read guard, the reader's level match, no allocation.
- `RigPlayer::setFeedDb(db)` / `feedDb()`: the bench's drive on top of the device's input level — the app's block IN
  (`PluginProcessor.cpp:1497`), which the app applies outside its player. The stand-by wire is taken before it, the
  scope's dry after it, the pack's input level after that; the sound is one gain, the input level plus the feed, summed
  in that order — the bytes of stating the feed as the host's hand (`setHostInputDb(stageInputDb() + db)`). Kept across
  `load()` and `unload()`, non-finite refused, arrives (not ramps) on the way back from stand-by as the input level does.
- The sound is untouched. Checked once while building this change (not a suite in the tree): 24 renders through
  `fc_rig_render` — every pack of the site (X88-IR ×4, IR-X ×2, V4 ×6) at 48 kHz mono and 44.1 kHz stereo, with the
  feed, PARK both ways and cut short, the gain dial, every tone and blend knob, one model and two, normalisation off
  and a reset — byte-identical to v0.5.0's. The standing guard is the ABI suite, which plays the PARK-and-feed
  scenario against the player driven on v0.5.0's spelling, up to the unload.
- An empty player is a wire behind the feed whichever setter ran last: `publishLevels()` says so itself (the feed
  alone in, unity out, the host's hand kept for the next load), so a hand or a feed set while nothing is loaded no
  longer adds the hand back in — the next pack's levels glided in from the wrong place.

### fcrig — ABI 3, the same shape, another picture

- `fc_rig_scope_read` keeps its arguments, its count and its level match; WHAT IT DRAWS is the app's (above), not
  v0.5.0's tap around the models. No entry point is added or changed in shape, but the meaning of one is, so
  `FC_RIG_ABI_VERSION` is 3 and the worklet asks for 3; `fc_rig_abi.h`'s compatibility rule now says a change of
  meaning moves the version as an addition does (it said a meaning never changes).
- `fc_rig_set_feed_db` hands the feed to `RigPlayer::setFeedDb` instead of restating the host's input level after every
  load; the output is v0.5.0's byte for byte (the ABI suite runs the PARK-and-feed scenario against the player driven
  on v0.5.0's spelling too, up to the unload: an empty player now publishes the feed alone whatever hand it is given).
- `BUILD-INFO` names the Eigen it was built against.

### Eigen from our own mirror (v0.1.1's change, ported)

NeuralAmpModelerCore's Eigen submodule points at gitlab.com, and every build cloned it from there; on a day GitLab
turned requests away under load, every build failed at that clone. `nam` now fetches NeuralAmpModelerCore without its
submodules (`GIT_SUBMODULES ""`, CMP0097) and Eigen from github.com/darwinscat/eigen — a mirror of the same repository,
untouched — at the very commit v0.5.4's submodule pins (Eigen 5.0.1, `bc3b398`). The sources built are the same, byte
for byte (1913 files; the same commit and tree as the submodule checkout). A NeuralAmpModelerCore whose submodule is
already on disk (`FELITRONICS_NAM_CORE_SRC`, CMake's own `FETCHCONTENT_SOURCE_DIR_NAM_CORE_SRC`, a parent project's
declaration) brings its own; one whose submodule was never initialised now takes the mirror's instead of failing at
`Eigen/Dense`. The configure stops when NeuralAmpModelerCore's tree pins another Eigen than `FELITRONICS_EIGEN_COMMIT`
(v0.4.0 to v0.5.3 pin `87300c9`, so another `FELITRONICS_NAM_CORE_TAG` can no longer build against this Eigen
silently), and when the Eigen used is any other commit; git runs as `find_package(Git)` found it, and a git that fails
stops the configure with its own error. A source that is not a git checkout is said so, not passed in silence.

## v0.5.0 — 2026-10-19

### rigplayer — the scope tap and stand-by (OrbitAmp's ScopeTap and PARK)

- `RigPlayer::scopeRead(dry, wet, n)` / `scopeFrames()` / `scopeSize()`: the newest `kScopeSize` (2048, OrbitAmp's
  `ScopeTap::size`; `prepare()`'s new fourth argument, 0 = no tap) samples of channel 0 at two points — the DI as fed
  (after the pack's input level, held back by `latencySamples()` as the blend's dry is) and the models' mix (aligned,
  weighed, through the law's gain; before the post tone, the blend knob and the output level) — written once per
  block on the audio thread, read on any thread with the wet at the dry's RMS, as `ScopeTap::read` does. Lock-free:
  the writer claims, fences, writes atomic floats and publishes; a read the writer may have reached is thrown away and
  retried (`kScopeTries`), never handed out torn.
- `RigPlayer::setStandby(on)` / `standingBy()`: PARK as the app's captured block plays it — the output crossfades over
  BypassFade's 15 ms (linear, both ways, its own arithmetic) to a wire (the input at the latency, unity), and then
  nothing runs: no models on audio, no tone, no law, no scope. Knobs moved meanwhile are taken on the way back; the
  input level arrives rather than ramps. Stricter than the app on one point: a pause is silence (law 11c) — the filters,
  convolvers, the dry ring and the slot delay lines are stopped when the fade ends and both stages are handed the
  width-zero call, so nothing played before PARK is heard after it (the app's freeze replays 0.399 out of digital
  silence on a 1500-sample capture).
- `RigPlayer::unload()` clears what the law last said it holds: a device that stands by runs no law, and the old
  pack's model ids named the new pack's models after a reload while parked.
- The stand-by fade ends where `BypassFade`'s does — `approximatelyEqual` at JUCE's defaults, not `>= 1` (48 kHz in
  60-sample blocks the walk up stops at 0.99999988; the player blended one sample more).
- Never set, both are free and inaudible: four renders (the fixture at 48 kHz with a 1 → 2 → 1 switch and at 44.1 kHz
  stereo with a reset, X88-IR Crunch, V4 Sheriff Ch1 at one model) are byte-identical to v0.4.0's.

### fcrig — ABI 2

- `fc_rig_scope_read`, `fc_rig_scope_frames`, `fc_rig_set_standby`, `fc_rig_set_feed_db` (the app's block IN — how hard
  the captures are fed, on top of the pack's own input level, applied after the stand-by wire and before the dry tap,
  restated against the pack held after every load; a change glides over about 10 ms like the player's other levels,
  where the app ramps its IN over one host block). `FC_RIG_ABI_VERSION` 2; the processor needs 2.
- The tap's points are the app's web spec (`Monitor.cpp:189-191`) and the site's tap around TONE3000's node, NOT the
  app's own code, which taps around its whole player (`CapturedBlock.h:544-556`: the dry before the input level and
  undelayed, the wet after the pack's tone stack and levels) — a pack with a post tone, a blend knob or an output level
  draws another SHAPE there. A block processed with no input channel writes no tap.
- `fcrig-worklet.js`: `{ scope: ArrayBuffer }` answered with the buffer, transferred back, holding the newest frame
  (`{ scope, n, frames }`); `{ set: { feed } }` queued like any knob while the start decides; `{ standby }` (or
  `{ set: { standby } }`) never queued — applied at once, the start paused while parked (no burst: a parked player
  runs no network), the output the player's own while parked whatever the start's state, the start resumed on the
  return. Queued like a knob, a `{ load }` while parked was silent for good.

## v0.4.0 — 2026-10-19

### fcrig — the pack player for a browser AudioWorklet

`tools/wasm-rig`: `felitronics::rigplayer` behind a C ABI (`tools/fc_rig_abi.h`, ABI 1) — the manifest in, the knobs by
the pack's names, the load job carried across the boundary (taken, the capture's bytes handed in, run, delivered),
audio in place — compiled to wasm with SIMD128 and Eigen's SSE path, and `fcrig-worklet.js`, a processor that asks the
page for each capture over its port and, at its start and again at every `{ load }`, times one capture on the audio
thread and turns the blend of two on only when that burst is valid evidence and one render costs at most a quarter of
the quantum; a first capture that fails to load ends the start at one model, and a `{ need }` the page leaves
unanswered is answered by the processor itself after `needTimeoutMs` (10 s). C++ exceptions only on the NAM objects (Oleh,
19.10.2026), in wasm's legacy encoding. The ABI renders bit for bit as RigPlayer driven directly; the module within
2e-5 of the native render on real packs. felitronics-core pinned to v0.61.1.

### Fixed

- `RigPlayer::deliver` no longer keeps an EMPTY answer of the `ModelSource` as the file's bytes. An empty answer is
  the host's "could not fetch it"; kept, it made every later visit run on nothing without asking, so one failed fetch
  left the capture dead until the pack was loaded again. Now the file is asked for again on its next visit (bytes
  that are not a model are still kept: they are the file).
- The allocation promise of `RigPlayer::process()` is stated as it is: nothing with Linear and WaveNet captures; an
  LSTM or ConvNet capture allocates inside the pinned NAM.

## v0.3.0 — 2026-10-19

### rigplayer — one model at a time

`RigPlayer::setMaxModels(n)`, n 1 or 2 (message thread; anything else is refused). 2, the default, is the player as it
was, bit for bit, except the first blocks after a load (below). 1 is for a host that can afford one network and not
two — the browser demo on a slow machine: the gain dial steps to the nearest capture, with a deadband of a tenth of the
gap around the capture sounding (`kStepDeadband`); the other slot is asked for nothing (no neighbour load job; one
network on audio, plus the outgoing capture's bounded drain as it rests); and a change of capture is a dip instead of a
crossfade — the gain falls over `kDipSeconds` (15 ms, OrbitAmp's `BypassFade` length), the outgoing capture rests, the
incoming one is loaded (one job) or woken and fed its receptive field while nothing is heard, and the gain rises over
the same 15 ms. A capture that lived in the resting slot comes back with a warm-up, not a load; crossing a link (one
file at two angles of different parity) is a dip and a load. Either way while playing: 1 → 2 asks for the neighbour at
weight zero and nothing of it is heard until it is fed — then, between two captures, the weight glides to the dial's
blend; 2 → 1 on a capture rests the neighbour without a change, between two captures it dips to the nearer. New
read-outs `runBlocks(slot)` and `pairBlocks()` count the blocks a slot's network ran, a resting capture's drain
included. In `nam::BlendLaw`: `BlendPolicy::single` and the dip lengths, `blendStepSingle`, and `BlendStep::gainRamp`
(a gain ramp that ends inside a block).

### nam::BlendLaw — the gain waits for every weighted slot to be safe

The two-slot law raised its gain as soon as either slot was safe to hear, while the weight still stood partly on the
other. At a cold start the weight leaves the empty slot 0 while the neighbour loads first, and an empty `NamStage`
passes its input through: v0.2.0 played the raw DI for the first four blocks of the travel, under a rising gain. The
gain now rises only once no slot that is empty, in flight or unfed carries weight in the block, so those blocks are
silent and the sound fades in once the weight has arrived. This is the one change to the default output; from there
on it is v0.2.0's, sample for sample. The same rule makes a turn from one model to two silent until the slot the
weight stands on is fed.

### rigplayer — a load is offered to the host only after the block that asked has run

The law's ask for a model used to be published before the two stages ran the block; a job thread pre-empting the
audio thread could deliver the model in between, and the new, unfed capture played at the weight the block began
with. The ask is now published after both stages. Nothing changes for a single-threaded host such as an AudioWorklet.

## v0.2.0 — 2026-10-19

### fcguitarfx — the amp's echo and room for a browser AudioWorklet

`tools/fc_guitar_fx_abi.h` (ABI 1) and `tools/wasm`: felitronics-core's `delay::TapeDelay` then `reverb::AmpReverb`,
driven by the app's fifteen knobs in the app's units and defaults, behind a C ABI; `tools/wasm/build.sh` builds
`fcguitarfx.mjs` + `fcguitarfx.wasm` (32 KB, 13 KB brotli) under core's FelitronicsPolicy, and
`fcguitarfx-worklet.js` runs it in an AudioWorklet from a `WebAssembly.Module` the page posts. Suites: the ABI
bit-identical to the core classes driven directly; the shipped module in a worklet-shaped scope against the native
render (within 2e-6, law 10). Browser floor: Chrome 85, Firefox 79, Safari 15. Needs felitronics-core v0.61.0: the pin
moves there from v0.51.0; built by default only at the top level (`FELITRONICS_GUITAR_WITH_FX`), so a consumer on an
older core is not affected.

## v0.1.0 — 2026-09-25

### nam · rigplayer · poweramp — split out of felitronics-core

The three modules and their suites, from felitronics-core v0.50.0; target names, namespaces and headers
are unchanged. New here: the law-11 and law-11c suites for `poweramp::PowerAmpStage` through core's shared
harnesses (`felitronics::test_support`), the header-hygiene TU and the `-fno-exceptions` probe.
`THIRD_PARTY_NOTICES.md` records the pins the build uses: NeuralAmpModelerCore v0.5.4, namz v4.1.0.
