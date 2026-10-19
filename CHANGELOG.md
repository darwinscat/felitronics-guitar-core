<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Changelog

## v0.5.1 — unreleased

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
