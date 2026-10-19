<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# fcrig — the pack player, for a browser AudioWorklet

`felitronics::rigplayer::RigPlayer` — one device of a `.orbitrig` pack, playing, with NAM underneath — behind a thin C
ABI, `fc_rig`, compiled to WebAssembly. The page plays a pack as the app plays it (OrbitAmp's `CapturedBlock`): the
player applies the pack's two levels, the files' input trims, the loudness normalisation, the pack's tone and blend
knobs and the crossfade along the gain dial by itself. The module holds no DSP of its own; it shares
`fcguitarfx`'s shape (`../wasm/README.md`) wherever it can.

| file | what |
|---|---|
| `../fc_rig_abi.h` | the C ABI: the contract, the load job, `FC_RIG_ABI_VERSION` |
| `fc_rig.cpp` | its translation unit — one RigPlayer call per entry point, compiled without exceptions |
| `fc_rig_manifest.cpp` | rig.json through namz's reader — nlohmann, so compiled with exceptions |
| `fcrig-worklet.js` | the AudioWorklet processor, `'fc-rig'` |
| `build.sh` | builds `fcrig.mjs` + `fcrig.wasm` into `tools/wasm-rig/build/`, with `BUILD-INFO` |
| `tests/RigAbiTests.cpp`, `tests/rig_fixture.h` | the ABI against RigPlayer driven directly, bit for bit; the job protocol; the contract; no allocation |
| `rig-parity.mjs`, `fc_rig_render.cpp` | the shipped module in a worklet-shaped scope against the native render |

A directory of its own beside `tools/wasm`, not a second target there: this module links NAM, the one thing in the
repository that throws, and is built with SIMD; `fcguitarfx` stays exception-free and scalar. One `build.sh` per module
keeps either set of flags from leaking into the other.

## The ABI

```c
uint32_t fc_rig_abi_version (void);                                         // 1
fc_rig*  fc_rig_create (void);   void fc_rig_destroy (fc_rig*);
int      fc_rig_prepare (fc_rig*, double rate, int maxBlock, int channels);  // 1 ok, 0 refused (disarmed)
float*   fc_rig_buffer (fc_rig*, int channel);                               // maxBlock floats in the module's memory
void*    fc_rig_alloc (int bytes);   void fc_rig_free (void*);              // module memory for strings, the manifest
int      fc_rig_load (fc_rig*, const char* manifest, int length);           // rig.json; 1 = a NAM stage to play
void     fc_rig_unload (fc_rig*);
int      fc_rig_take_job (fc_rig*);                                          // the load job, below
const char* fc_rig_job_file (fc_rig*);   int fc_rig_job_file_length (fc_rig*);   int fc_rig_job_needs_bytes (fc_rig*);
uint8_t* fc_rig_job_bytes (fc_rig*, int size);   int fc_rig_job_run (fc_rig*);   int fc_rig_job_deliver (fc_rig*);
int      fc_rig_set_dial (fc_rig*, const char* name, double degrees);       // any dial, by the pack's name
int      fc_rig_set_switch (fc_rig*, const char* name, const char* value);  // any control, tone switch, blend…
int      fc_rig_set_gain_dial (fc_rig*, double degrees);   double fc_rig_gain_dial_sweep (fc_rig*);
int      fc_rig_set_max_models (fc_rig*, int n);                             // 1 or 2 (default)
void     fc_rig_set_normalize (fc_rig*, int);   void fc_rig_set_input_trims (fc_rig*, int);   // both on by default
void     fc_rig_set_blend_shape (fc_rig*, double point, double width);       // the app's SMOOTH {0.5,1} / STEP {0.5,0}
int      fc_rig_process (fc_rig*, float* left, float* right, int n);        // in place; right NULL = mono
void     fc_rig_reset (fc_rig*);
double   fc_rig_sounding_db (fc_rig*);   int fc_rig_sounding_tagged (fc_rig*);   int fc_rig_sounding_blended (fc_rig*);
int      fc_rig_held (fc_rig*, int slot);   int fc_rig_latency_samples (fc_rig*);
int      fc_rig_run_blocks (fc_rig*, int slot);   int fc_rig_pair_blocks (fc_rig*);   int fc_rig_warm_blocks (fc_rig*);
void     fc_rig_clear_counters (fc_rig*);
```

The setters are the ones the app's block calls (`setDial`, `setSwitch`, `setBlendShape`, `setInputTrims`) plus
`setMaxModels` and `setNormalize`; the read-outs are what a face shows (`soundingLoudness`) and what a host measures its
budget by (`runBlocks`, `pairBlocks`). Not exposed in ABI 1: the host-level hand (`setHostInputDb/OutputDb` — a page
has its own INPUT and OUTPUT around the player), tone overrides, the alignment table, the cold rest, the drawing
read-outs. Each is one append away. **Compatibility is append-only** exactly as `fc_guitar_fx`'s.

The contract in short (the header has it whole): one thread owns an instance; `process()` and `reset()` never lock or
throw, and never allocate while the captures are Linear or WaveNet (LSTM and ConvNet allocate inside the pinned NAM); `prepare()` takes 1 kHz … 768 kHz, maxBlock 1 … 65536, 1 or 2 channels, and a refusal
disarms; a mono call runs ONE network per sounding capture, so a guitar is prepared for 1 channel.

## The load job, across the boundary and across the port

RigPlayer never loads by itself: its law asks for a capture and the host runs a JOB and brings it back. Inside the
module: `take_job` (service, then `takeLoadJob`) → `job_file` says which `files[].id` → `job_needs_bytes` says whether
the player still needs them (it keeps every file's bytes once they arrived; a job run without bytes keeps nothing, so
that file is asked for again on its next visit) → `job_bytes(size)` is where they go → `job_run` is the heavy half
(`NamStage::prepareModel`) → `job_deliver` the light half. A job taken must come back — a failure too (run without
bytes is "could not fetch it"); a job out when the pack is loaded again or unloaded is dropped by the instance
itself, so jobs never stall.

Across the port, the processor does that for the page, which only fetches:

```
processor → page   { need: "X88-IR Crunch-0005.namz", pack: 3 }
page → processor   { file: "X88-IR Crunch-0005.namz", pack: 3, bytes: <ArrayBuffer, transferred> | null }
processor → page   { loaded: "X88-IR Crunch-0005.namz", ok: true }
```

`pack` is the processor's count of loads; an answer for an older pack, or for a file nobody is waiting for, is
dropped. **Every `{ need }` must be answered** — `null` when the fetch or its body failed — because the player hands
out one job at a time: a `{ need }` left open stops every later load. Should the page still not answer, the processor
answers for it after `needTimeoutMs` (10 s by default, `0` = never): the job runs without bytes, `{ loaded, ok:
false }`, and the late answer is dropped. `{ load }` is the other recovery — it drops the job out. The job pump runs after every render quantum; a capture the player already holds the bytes of is run at
once, with no round trip. **The job runs on the audio thread** — a worklet has no other — so the quantum it lands in is
late by the network's build and prewarm (tens of milliseconds for a WaveNet). Accepted for now (Oleh, 19.10.2026);
the player's own dip covers the swap itself.

## The processor

```js
const module = await WebAssembly.compileStreaming(fetch(wasmUrl));          // the page compiles, once
await context.audioWorklet.addModule(workletUrl);                           // imports ./fcrig.mjs statically
const node = new AudioWorkletNode(context, 'fc-rig', {
    numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
    channelCount: 1, channelCountMode: 'explicit',                          // a guitar: Web Audio folds to mono
    processorOptions: { module, rig: rigJsonArrayBuffer, knobs: { gain: 150 } } });
node.port.onmessage = async ({ data }) => {                                 // read { ready }, { calibrated }, …
    if (data.need === undefined) return;
    let bytes = null;
    try {                                                                   // the fetch AND the body: a body cut
        const r = await fetch(packDir + encodeURIComponent(data.need));     // off after the headers rejects
        if (r.ok) bytes = await r.arrayBuffer();                            // arrayBuffer()
    } catch (e) { bytes = null; }
    node.port.postMessage({ file: data.need, pack: data.pack, bytes }, bytes ? [bytes] : []);   // always answered
};
node.port.postMessage({ set: { gain: 210, dial: { Bass: 120, Treble: 200 }, switch: { Boost: 'on' } } });
node.port.postMessage({ load: otherRigJsonArrayBuffer });                   // another pack
node.port.postMessage({ read: true });                                      // → { state: { sounding, latency, … } }
node.port.postMessage({ dispose: true });                                   // the only end
```

`{ set }` takes `maxModels`, `normalize`, `inputTrims`, `blendShape: [point, width]`, `switch: { name: value }`,
`dial: { name: degrees }` and `gain` (the crossfade dial, whatever the pack names it), applied in that order. Names
and values travel as UTF-8 written by the processor itself (a worklet may have no TextEncoder or TextDecoder). Options:
`stereo: true` plays two channels (two networks per capture); `calibrate: false` skips the start's decision and
starts at `maxModels` (2 if absent); `needTimeoutMs` (above). Loading, the bytes path (`wasmBytes`, WebKit's null module), errors and dispose
are `fcguitarfx`'s: **an error is not an end** — one `{ error }`, the dry from then on, `process()` true until
`{ dispose }`. A trap inside the module is an error too, and that module is never called again (not even to free).
A failed load is not an error: `{ loaded, ok: false }`, and the law decides what sounds. A knob value that is not a
number (a BigInt survives `postMessage`) is ignored, not a throw — and not a trap.

`process()` allocates nothing per quantum. A load may grow the module's memory, which detaches every older view; the
processor compares its view's buffer with the module's at the top of every quantum and again after the render (an
LSTM or ConvNet capture allocates inside the pinned NAM while it renders — the module's allocation promise covers
Linear and WaveNet only, gated by `felitronics_nam_rtalloc_tests`), and makes the two views again only then (tested:
a 48 MiB file handed in grows the memory and the next quanta still play; a render that grows it copies out bit for
bit).

## The calibration rule

A browser's audio thread may afford two networks or only one, and the only honest way to know is to time one ON it.
At its start — and again at every `{ load }`, since another pack's captures may be another architecture and another
cost — the processor:

1. tells the player ONE model (`setMaxModels(1)`) before the pack loads, and turns the gain dial to the middle of its
   sweep (150 of 300 — 12 o'clock);
2. waits until that capture is loaded and a block ran exactly one network, warm;
3. renders quanta of SILENCE back to back until `Date.now()` has moved at least 30 ms (a worklet has no
   `performance.now()`; a count of quanta between two coarse ticks is biased, a 30 ms burst is within a few percent —
   the spike's measurement). The burst is VALID only when the player's own counters say every render ran exactly
   one network, warm, and `Date.now()` moved at least 10 ms (a 1 ms tick is then at most 10 %); a burst whose
   counters were not clean is tried again, up to 8 times;
4. turns the blend on (`setMaxModels(2)`) when the burst is valid and one render costs **at most a quarter of the
   quantum** (~667 µs at 48 kHz — two then fit in half), and otherwise stays at one capture with dips — an invalid
   burst never turns the blend on;
5. posts `{ calibrated: { renderUs, quantumUs, share, maxModels, renders, elapsedMs, runs, pairs, warm, valid,
   reason } }` (`reason` when not valid: `'clock'`, `'counters'`) and applies the knobs that arrived meanwhile, in
   order. The output is silent until then.

If the capture at 12 o'clock fails to load there is nothing to time: the start ends at one model with
`{ calibrated: { maxModels: 1, valid: false, reason: 'load-failed' } }`, and the queued knobs land.

**The burst stalls the whole AudioContext once.** It runs inside one `process()` call: ~30 ms plus up to two ticks of
`Date.now()` (the wait for the first tick, the last one) — at 48 kHz and 128 frames about a dozen quanta, and every
other node of the context drops out with it. Where `Date.now()` is coarsened (Firefox's `resistFingerprinting`,
100 ms) the ticks are that long. The same class as the load stall, accepted alike.

`{ measure: true }` runs the same burst on whatever is playing (`{ measured }` — one or two networks, counted) without
deciding anything: for a bench, at a silent moment.

## Build

```sh
source ~/emsdk/emsdk_env.sh                 # or let build.sh source ~/emsdk itself
tools/wasm-rig/build.sh [out-dir]           # FELITRONICS_CORE_DIR=<checkout> to build against another core
```

Configured with `emcmake` like `fcguitarfx`: core's `FelitronicsPolicy.cmake` is the numeric contract. Every object gets
`-fno-exceptions -fno-rtti -msimd128 -msse4.1` (SIMD128, and Eigen's SSE packet path lowered onto it); the NAM objects
get exceptions back (below); `-O3`; linked `-sENVIRONMENT=worklet -sGROWABLE_ARRAYBUFFERS=0`, ES6 factory
`createFcRig`, no eval, no file system, dlmalloc. Core is the PINNED tag, v0.61.1 (the first whose `FlushToZero.h`
compiles for wasm under `-msse4.1`), unless `FELITRONICS_CORE_DIR` names another; NeuralAmpModelerCore v0.5.4 and namz
v4.1.0 are `modules/nam`'s pins (`FELITRONICS_NAM_CORE_SRC` / `FELITRONICS_NAMZ_SRC` point at local checkouts of them for
an offline build). It proves the artefact single-threaded (core's `check-no-threads.mjs`), reads the wasm features the
module needs off the module (binaryen validation against MVP plus each candidate), refuses a glue that names
`toResizableBuffer`, and writes `BUILD-INFO`:

```
felitronics-guitar-core <git describe>
felitronics-core v0.61.1
NeuralAmpModelerCore v0.5.4
namz v4.1.0
fc_rig ABI 1
emscripten 6.0.9 (…)
flags -fno-exceptions -fno-rtti -msimd128 -msse4.1; exceptions on the NAM objects only, wasm legacy EH
wasm features sign-ext nontrapping-float-to-int simd exception-handling
```

| file | raw | gzip | brotli |
|---|---|---|---|
| `fcrig.wasm` | 796 298 | 285 546 | 215 577 |
| `fcrig.mjs` | 12 047 | 3 985 | 3 593 |
| `fcrig-worklet.js` | 20 332 | 6 687 | 5 868 |

(TONE3000's `nam-engine.wasm`, which the site ships today, is 415 269 raw: NAM alone, no player, no tone, no pack.) The
module's memory starts at 24 MiB and grows as captures load.

## C++ exceptions — Oleh's decision of 19.10.2026

**C++ exceptions are allowed only for our NAM — NeuralAmpModelerCore and nlohmann — in this wasm build; everything
else stays `-fno-exceptions`.** NeuralAmpModelerCore throws on a model it cannot read, nlohmann on JSON it cannot parse;
`NamStage::prepareModel` catches both and a bad capture becomes a failed load. So `nam_core`, `felitronics_nam`
(`NamStage.cpp`) and `fc_rig_manifest.cpp` (nlohmann under the manifest) are compiled with
`-fexceptions -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1`, and linked with the runtime that catches; `fc_rig.cpp`,
RigPlayer's `process()` and all of core are not. Tested both ways: garbage handed in for a capture is `{ loaded, ok:
false }` and the node plays on; the same test against a module whose NAM objects throw with catching disabled traps.

**Legacy, not exnref.** Wasm exception handling comes in two encodings, and the floor follows the one chosen
(emscripten's `tools/feature_matrix.py`):

| encoding | Chrome | Firefox | Safari |
|---|---|---|---|
| legacy (`try`/`catch`/`rethrow`, `-sWASM_LEGACY_EXCEPTIONS=1`) | 95 | 100 | 15.2 |
| exnref (`try_table`, `-sWASM_LEGACY_EXCEPTIONS=0`) | 137 | 131 | 18.4 |

Legacy keeps the floor within the ~five years Oleh accepted; exnref would cut it to the last year or so. The built
module has 1 729 `try` blocks and no `try_table` (wasm-dis). Emscripten's JavaScript-based exceptions (`-fexceptions`
alone) would leave the floor where emscripten's own is, and were measured: 933 KB instead of 796 KB, one capture 4.7 %
of a quantum instead of 3.6 %, two 13.6 % instead of 7.4 % (node, X88-IR Crunch) — every call that can throw goes
through a JavaScript trampoline, in the hot path too.

## Browser floor

**Chrome 95, Firefox 100, Safari 16.4 (iOS 16.4)** — the newest of what the module asks for:

- SIMD128: Chrome 91, Firefox 89, **Safari 16.4** — the one that sets Safari's floor;
- legacy wasm exception handling: **Chrome 95, Firefox 100**, Safari 15.2;
- sign-extension and non-trapping float-to-int (Chrome 75, Firefox 64, Safari 15), read off the module;
- the glue's JavaScript at emscripten's default floor (Chrome 85, Firefox 79, Safari 15), with
  `-sGROWABLE_ARRAYBUFFERS=0` (as `fcguitarfx`: otherwise Chrome 144 / Firefox 145 / Safari 26.2);
- module AudioWorklets with a static import (Chrome 66, Firefox 76, Safari 14.1).

Read from the toolchain's tables (emscripten's `tools/feature_matrix.py`; SIMD128, which that table does not list,
from caniuse) and the module, not tried on a device of each version. Against the ~five-year floor
(Chrome 85 / Firefox 79 / Safari 15) that is about a year younger on Chrome and Firefox and eighteen months on Safari —
SIMD, not exceptions, is what costs Safari 15.x. A page that must reach those would need a second, scalar build and
`WebAssembly.validate` to choose; not built.

## Cost

Measured in Chromium 151 (Playwright, an Apple M5 Pro, 48 kHz, 128-frame quanta) by the processor's own burst, five
bursts each, the median: one capture sounding **3.8 – 4.2 %** of the quantum (101 – 111 µs), two **8.0 – 8.5 %**
(214 – 227 µs); the start's calibration read 3.9 – 4.0 % on each of X88-IR Crunch, IR-X Ch1 PowerAmp and V4 Sheriff
Ch1 and turned the blend on. In node (rig-parity.mjs): 3.6 % and 7.4 %. DevTools' CPU throttling does not reach the
audio thread (×4 read the same), so the slow-machine decision is tested with an injected slow render instead.

## Tests

- `felitronics_rig_abi_tests` (ctest, native): the ABI against RigPlayer driven directly with a ModelSource — **bit for
  bit**, on the fixture pack: two models and one (set before the load, then 1 → 2 → 1 while playing), mono and stereo,
  the gain swept across captures, the link and past the top, a channel switch, tone and blend knobs, normalisation and
  input trims off and on, a blend shape, a reset, a pack loaded again and unloaded; the jobs and the bytes handed in
  where the source was asked (each file once, revisits from the kept bytes); a missing file and garbage — failures,
  still bit for bit; a fetch that failed once is asked for again on a later visit and lands, still bit for bit; a job out across a load is dropped and jobs flow again; the contract; no allocation in
  `process()`/`reset()` through loads and dips.
- `felitronics_rig_wasm_parity` (ctest when `FELITRONICS_GUITAR_RIG_WASM_DIR` names `build.sh`'s output; real packs
  too with `FELITRONICS_GUITAR_RIG_PACKS`; `node --experimental-vm-modules --expose-gc rig-parity.mjs <dir>
  <fc_rig_render> <abi header> [<packs dir>]`): the processor and the glue as ES modules in a context shaped like an
  AudioWorkletGlobalScope (no fetch, URL, TextDecoder, TextEncoder, atob, setTimeout, performance; no
  toResizableBuffer), the page simulated; exports == the header; every pack rendered through the processor's own
  `process()` against `fc_rig_render` on the same schedule, within **2e-5** (law 10's contraction is stated alike, but
  Eigen spells its own multiply-add: fused on native arm64's NEON, not on SIMD128 — measured 1.2e-7 on the fixture,
  1.7e-6 … 5.5e-6 on X88-IR Crunch, IR-X Ch1 PowerAmp, V4 Sheriff Ch1, two models and one); garbage for a capture is a
  failed load, not a trap, and growing the memory mid-stream; the start's decision with a real pack (blend on) and an
  injected slow render (one model), the silent start, the queued knobs in order; the load protocol across the port —
  a first capture that fails ends the start at one model, an answer of null is asked for again, an answer with no
  `{ need }` out posts nothing, an answer forty quanta late lands, an old pack's answer is dropped, an unanswered
  `{ need }` times out and the next job flows; a `{ load }` decides again (slow now: one model); a stopped clock and
  polluted counters are not valid evidence (one model); a render that grows the memory copies out bit for bit; a
  BigInt knob is ignored; no module, a refused prepare, a
  foreign quantum and a trap — one error, dry, alive, only dispose ends; no allocation per quantum; the cost of one
  quantum with one capture and with two, reported.

## What a page owes it

- Compile `fcrig.wasm` once, post the module; fetch `rig.json` and hand its bytes in `processorOptions.rig` (or
  `{ load }`); answer **every** `{ need }` with the file's bytes (or `null`) and the `pack` it came with — the fetch
  and the body in one `try`, the answer posted whatever failed (an open `{ need }` stops every later load until the
  processor's own timeout or a `{ load }`).
- Keep the node quiet until `{ calibrated }` (it is silent until then anyway) and start the guitar after it — and
  again after every `{ load }`, which decides anew.
- Play the pack's tone through the player (`{ set: { dial: { Bass, Middle, Treble } } }`, a tone switch through
  `switch`), not through biquads of its own; and the gain through `gain` (degrees of the pack's sweep).
- Level: the player normalises every tagged capture to **-18 dB** (NamStage's reference) and applies the pack's
  `output_db` — a page that normalised to its own figure drops that and moves its OUTPUT instead.

## Vendoring

As `fcguitarfx`: the three files travel together and the worklet's static `import './fcrig.mjs'` must keep pointing
at the glue — with content-hashed names, rename the glue, rewrite that one import line in the copied worklet (or put
the three in one hashed directory), and hash the worklet after the rewrite. Ship `BUILD-INFO` beside them. The ABI
version is the `#define FC_RIG_ABI_VERSION <n>u` line of `tools/fc_rig_abi.h`. Licences: this module is AGPL-3.0;
it compiles in NeuralAmpModelerCore (MIT), nlohmann/json (MIT), Eigen (MPL-2.0) and namz (`THIRD_PARTY_NOTICES.md`).
