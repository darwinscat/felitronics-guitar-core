<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Changelog

## Unreleased

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
