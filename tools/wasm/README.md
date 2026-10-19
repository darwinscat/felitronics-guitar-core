<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# fcguitarfx — the amp's echo and room, for a browser AudioWorklet

felitronics-core's `delay::TapeDelay` and `reverb::AmpReverb` (core v0.61.0 and later) behind a thin C ABI,
`fc_guitar_fx`, compiled to WebAssembly. The amp plugins and the browser demo run one implementation; this module is
the browser's door to it. It holds no DSP of its own: only the app's mapping from its knobs to the two stages.

| file | what |
|---|---|
| `../fc_guitar_fx_abi.h` | the C ABI: the contract, the knobs, `FC_GUITAR_FX_ABI_VERSION` |
| `fc_guitar_fx.cpp` | its translation unit — the app's mapping, nothing else |
| `fcguitarfx-worklet.js` | the AudioWorklet processor, `'fc-guitar-fx'` |
| `build.sh` | builds `fcguitarfx.mjs` + `fcguitarfx.wasm` into `tools/wasm/build/` |
| `tests/GuitarFxAbiTests.cpp` | the ABI against the core classes driven directly, bit for bit; the contract; no allocation |
| `fx-parity.mjs`, `fc_guitar_fx_render.cpp` | the shipped module in a worklet-shaped scope against the native render |

## The ABI

One instance is the amp's last two additive links in the app's order: the **delay, then the reverb** (OrbitAmp's
`processBlock`: gate → preamp → EQ → delay → reverb). Both only add — the dry passes at unity at every setting.

```c
uint32_t      fc_guitar_fx_abi_version (void);                          // 1
fc_guitar_fx* fc_guitar_fx_create (void);                               // at the app's defaults
void          fc_guitar_fx_destroy (fc_guitar_fx*);
int           fc_guitar_fx_prepare (fc_guitar_fx*, double sampleRate, int maxBlock);   // 1 ok, 0 refused (disarmed)
float*        fc_guitar_fx_buffer (fc_guitar_fx*, int channel);         // maxBlock floats in the module's memory
int           fc_guitar_fx_process (fc_guitar_fx*, float* left, float* right, int n);  // in place; right NULL = mono
void          fc_guitar_fx_reset (fc_guitar_fx*);                       // a stream restart, knobs kept
void          fc_guitar_fx_set_<knob> (fc_guitar_fx*, <int|float>);     // fifteen, below
```

prepare takes 1 kHz … 768 kHz and maxBlock 1 … 65536, allocates every line, and a refusal disarms the instance.
process answers 1 when it processed all `n` frames and 0 when it refused without touching a sample; it allocates
nothing and locks nothing, nor do reset and the setters. One thread owns an instance — in a worklet, the audio thread.

**The knobs are the app's parameters, in the app's units** (percent where the face reads percent), at the app's
defaults, each clamped onto the app parameter's range; a non-finite value is ignored. A setter records the value; the
next `process()` hands every knob to the stages in the app's order before it runs them, as the app does at the top of
every block.

| knob | type | range | default | reaches the stage as |
|---|---|---|---|---|
| `delay_on` | int | 0/1 | 1 | `TapeDelay::process(…, feed)` — off stands by, the repeats ring out |
| `delay_sync` | int | 0/1 | 1 | sync on: `divisionMs(division, bpm)`; off: `delay_time_ms` (DelayLaw `timeMsFor`) |
| `delay_division` | int | 0 … 12 | 5 (1/4) | an index onto `delay::kDivisionNames` |
| `delay_bpm` | float | 40 … 240 | 120 | the tempo the sync counts in — the app's own BPM field |
| `delay_time_ms` | float | 20 … 2000 | 350 | `setTimeMs` |
| `delay_repeats` | float | 0 … 100 % | 35 | `setRepeats(× 0.01)` |
| `delay_dark` | float | 0 … 100 % | 45 | `setDarkHz(darkHzForPercent(%))` |
| `delay_offset_ms` | float | −30 … 30 | 0 | `setOffsetMs` |
| `delay_mix` | float | 0 … 100 % | 0 | `setMix(× 0.01)` — the app ships the delay on with nothing in its mix |
| `reverb_on` | int | 0/1 | 1 | `AmpReverb::process(…, feed)` — off stands by, the tail rings out |
| `reverb_character` | int | 0 … 5 | 3 (Plate) | Ambience, Room, Hall, Plate, Spring, Modulated (`reverb::kCharacterNames`) |
| `reverb_mix` | float | 0 … 100 % | 30 | `setMix(× 0.01)` |
| `reverb_decay` | float | 0.5 … 2 | 1 | `setDecay` |
| `reverb_predelay_ms` | float | 0 … 100 | 0 | `setPredelayMs` |
| `reverb_hpf_hz` | float | 40 … 500 | 120 | `setHpfHz` |

Not exposed: the app's IN-THE-RIG switches with their bypass crossfade (a page takes a link out by not routing through
it), the host-tempo switch (no host conducts a page), and the per-parameter step the app's sliders snap to.

**Compatibility is append-only**: an entry point's name, arguments and meaning never change and nothing is removed; a
new entry point or a widened knob moves `FC_GUITAR_FX_ABI_VERSION`. A page asks `fc_guitar_fx_abi_version()` first.

## Loading it in a worklet

An AudioWorkletGlobalScope has no `fetch`, and in some browsers no `URL` and no `TextDecoder`; a worklet may not
`import()` at run time. So **the page compiles, the processor instantiates**:

```js
const module = await WebAssembly.compileStreaming(fetch(wasmUrl));        // main thread, streaming, cached
await context.audioWorklet.addModule(workletUrl);                         // statically imports ./fcguitarfx.mjs
const node = new AudioWorkletNode(context, 'fc-guitar-fx', {
    numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
    processorOptions: { module, knobs: { delay_mix: 25, reverb_character: 2 } } });
node.port.onmessage = (e) => { /* { ready: true, abi } or { error } */ };
node.port.postMessage({ set: { delay_mix: 40, delay_division: 8 } });    // any knobs, any time
node.port.postMessage({ reset: true });                                   // or { dispose: true }
```

The processor hands the `WebAssembly.Module` to the glue through `instantiateWasm`, so the glue never fetches,
never builds a URL and never decodes text (it is linked `-sENVIRONMENT=worklet`); instantiating a compiled module is
cheap on the audio thread, and one compile serves every node on the page. Why not the alternatives: `SINGLE_FILE`
inlines the wasm as base64 (a third larger, and decoded and compiled ON the audio thread at construction), and a
processor that fetched its own module cannot in a worklet. A browser that cannot clone a `WebAssembly.Module` into
the worklet can post the bytes instead (`processorOptions: { wasmBytes }`); the processor then compiles once, at
construction.

**Knobs travel as port messages, not AudioParams.** They are settings a hand moves — two switches, two choices —
and the stages already glide what must glide (the delay's head, the room's parameters). An AudioParam would be read
every quantum for fifteen knobs to find them unchanged; a message costs nothing until a knob moves, is handled on
the audio thread between quanta, and is heard from the next quantum's first sample. Knobs posted before the module
is up are queued and land before prepare, so the room starts at its character.

`process()` allocates nothing per 128-frame quantum: it copies the input into two `Float32Array` views made once
after prepare (onto `fc_guitar_fx_buffer`), calls the module, and copies back. A mono input plays on both channels;
no input is silence, and the tails still ring out. Until the module is up the processor passes the dry through.

## Build

```sh
source ~/emsdk/emsdk_env.sh                                  # or let build.sh source ~/emsdk itself
FELITRONICS_CORE_DIR=<felitronics-core v0.61.0+> tools/wasm/build.sh [out-dir]
```

It configures this repository with `emcmake`, so the numeric contract is core's own `cmake/FelitronicsPolicy.cmake`
(law 10's `-ffp-contract=on`, the emscripten physics, `--wrap=pthread_create`), plus `-fno-exceptions -fno-rtti` as
core's `wasm-audio` preset states them; `-O3`; no SIMD (it buys nothing here, measured — see `build.sh`). Without
`FELITRONICS_CORE_DIR` the top-level CMake's rule picks core: the sibling `../felitronics-core`, else the pinned
tag. It proves the artefact single-threaded with core's `tools/wasm/check-no-threads.mjs` and writes `BUILD-INFO`:

```
felitronics-guitar-core <git describe>
felitronics-core <git describe of the core it compiled against>
fc_guitar_fx ABI <n>
```

| file | raw | gzip | brotli |
|---|---|---|---|
| `fcguitarfx.wasm` | 32 418 | 15 150 | 13 285 |
| `fcguitarfx.mjs` | 7 837 | 2 399 | 2 116 |
| `fcguitarfx-worklet.js` | 6 899 | 2 701 | 2 297 |

The module's memory starts at 24 MiB (the policy's 8 MB stack plus emscripten's 16 MB default) and an instance
allocates about 2.5 MB at 48 kHz, most of it the 6.1-second delay line.

## Tests

- `felitronics_guitar_fx_abi_tests` (ctest, native and under emscripten with SAFE_HEAP in CI's wasm-audio job): the ABI
  renders **bit-identical** to `TapeDelay` + `AmpReverb` driven directly with the app's mapping — defaults, every
  character under a busy echo, knobs moving mid-stream with ragged and mono blocks, stand-by and reset, clamped and
  ignored knobs — while the reference hands every knob over every block, as the app does; the contract; dry at unity at
  mix 0/0; no allocation in the setters, `process()` or `reset()`.
- `felitronics_guitar_fx_wasm_parity` (ctest when `FELITRONICS_GUITAR_FX_WASM_DIR` names `build.sh`'s output;
  `node --experimental-vm-modules --expose-gc fx-parity.mjs <dir> <fc_guitar_fx_render> <abi header>`): the
  processor and the glue evaluated as ES modules in a context shaped like an AudioWorkletGlobalScope (no `fetch`,
  `URL`, `TextDecoder`, `atob`, `setTimeout`), the module posted as a `WebAssembly.Module`; its exports are exactly
  the header's; four control settings rendered quantum by quantum through the processor's own `process()` against
  the native render within **2e-6** (law 10: native arm64 fuses `a*b + c`, wasm cannot — measured worst 1.2e-7, and
  a native build with contraction off agrees with the wasm bit for bit); no allocation per quantum; and the cost of
  one 128-frame stereo quantum at 48 kHz, reported.

## Vendoring

A page vendors the three files together and keeps the worklet's static `import './fcguitarfx.mjs'` pointing at the
glue: with content-hashed names, rename the glue and rewrite that one import line in the copied worklet (or put the
three in one hashed directory), and hash the worklet after the rewrite. The ABI version is the
`#define FC_GUITAR_FX_ABI_VERSION <n>u` line of `tools/fc_guitar_fx_abi.h`; the builds are named by `BUILD-INFO`.
