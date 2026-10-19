<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Changelog

## Unreleased

### fcguitarfx — the amp's echo and room for a browser AudioWorklet

`tools/fc_guitar_fx_abi.h` (ABI 1) and `tools/wasm`: felitronics-core's `delay::TapeDelay` then `reverb::AmpReverb`,
driven by the app's fifteen knobs in the app's units and defaults, behind a C ABI; `tools/wasm/build.sh` builds
`fcguitarfx.mjs` + `fcguitarfx.wasm` (32 KB, 13 KB brotli) under core's FelitronicsPolicy, and
`fcguitarfx-worklet.js` runs it in an AudioWorklet from a `WebAssembly.Module` the page posts. Suites: the ABI
bit-identical to the core classes driven directly; the shipped module in a worklet-shaped scope against the native
render (within 2e-6, law 10). Needs felitronics-core v0.61.0: the pin moves there from v0.51.0.

## v0.1.0 — 2026-09-25

### nam · rigplayer · poweramp — split out of felitronics-core

The three modules and their suites, from felitronics-core v0.50.0; target names, namespaces and headers
are unchanged. New here: the law-11 and law-11c suites for `poweramp::PowerAmpStage` through core's shared
harnesses (`felitronics::test_support`), the header-hygiene TU and the `-fno-exceptions` probe.
`THIRD_PARTY_NOTICES.md` records the pins the build uses: NeuralAmpModelerCore v0.5.4, namz v4.1.0.
