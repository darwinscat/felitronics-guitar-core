<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# fcbass — the bass amp in a browser AudioWorklet

The bass amp's whole chain — `felitronics::bass::BassChain` (modules/bass) around the pack player (`fc_rig`) — as one
WebAssembly module and one AudioWorklet processor, for a page that plays a bass amp the way OrbitAmp's bass amps
(RedGrooveBass, OrbitBass) play it:

input cut → GATE → ENVELOPE's key → COMP → cut → OCTAVE → cut → ENVELOPE → **the AMP** (the capture, its BLEND, its tone)
→ cut → the gate's END mute → the seam → CHORUS → CAB with the DI mixed in → the bass's cuts

Mono in, two planes out. The echo, the room, the output trim and the limiter are not in it: a page plays its own lid.

| file | what it is |
|---|---|
| `../fc_bass_abi.h` | the C ABI: the contract, the knobs by the app's ids, `FC_BASS_ABI_VERSION` (1) |
| `fc_bass.cpp` | the ABI's translation unit; no DSP of its own |
| `fcbass-worklet.js` | the AudioWorklet processor, `'fc-bass'`: fcrig's processor (`../wasm-rig/rig-port.js`) around an fc_bass |
| `rig-port.js` | (copied from `../wasm-rig/`) the rig half both worklets share; `fcbass-worklet.js` imports it by this name |
| `build.sh` | emcmake → `fcbass.mjs`, `fcbass.wasm`, the two JS files and `BUILD-INFO` in one folder |
| `fc_bass_render.cpp` | the native half of the parity run |
| `bass-parity.mjs` | the web against its native build and against the plug-in's own renders |
| `tests/BassAbiTests.cpp` | the ABI against the chain driven directly, bit for bit; the contract; no allocation |

## Build

```sh
tools/wasm-bass/build.sh [out-dir]        # emsdk 6.0.9; core is the tag the top-level CMakeLists pins (v0.63.0)
```

`FELITRONICS_CORE_DIR=<checkout>` builds against a core checkout instead (BUILD-INFO then says so). The flags are fcrig's:
`-fno-exceptions -fno-rtti -msimd128 -msse4.1`, exceptions on the NAM objects only, wasm legacy EH; no threads (proven on
the artifact). Measured (06.11.2026, emsdk 6.0.9): `fcbass.wasm` 989 782 bytes (360 150 gzip), `fcbass.mjs` 14 684,
`fcbass-worklet.js` 6 692, `rig-port.js` 57 024. Browser floor: fcrig's (SIMD128 and wasm exceptions: Chrome 95, Firefox
100, Safari 16.4).

## The ABI, in one screen

```c
uint32_t fc_bass_abi_version (void);                                   // 1
fc_bass* fc_bass_create (void);
void     fc_bass_destroy (fc_bass*);
int      fc_bass_prepare (fc_bass*, double sampleRate, int maxBlock);  // two planes out; the player on one channel
float*   fc_bass_buffer (fc_bass*, int channel);                       // 0 the input and the left, 1 the right
int      fc_bass_process (fc_bass*, float* left, float* right, int n);
void     fc_bass_reset (fc_bass*);
fc_rig*  fc_bass_rig (fc_bass*);                                       // the AMP: every fc_rig_* entry point works on it
int      fc_bass_set (fc_bass*, const char* id, double value);         // the chain's knobs, by the app's parameter ids
float*   fc_bass_cab_taps (fc_bass*, int length);                      // the cabinet's IR in…
int      fc_bass_cab_load (fc_bass*, int length, double rate);         // …and taken
float    fc_bass_comp_gr_db (fc_bass*);                                // COMP's squeeze, for its meter
int      fc_bass_latency_samples (fc_bass*);                           // the player's (0 when the context runs at the pack's rate)
```

The module also exports every `fc_rig_*` entry point (fc_rig ABI 6) — on the fc_rig* `fc_bass_rig()` answers. The
header lists every knob id with its range and default (the app's parameter defaults, not a product's opening).

## The worklet

`fcbass-worklet.js` is fcrig's processor (`rig-port.js`, its whole contract written there) around an fc_bass: the module
handed in, the pack (`processorOptions.rig`, `{ load }`), every `{ need }` answered, the files up front (`{ files }`), the
preload, the start and its `{ calibrated }`, the quiet warm, the scope, and the player's knobs (`{ set: { gain, switch,
dial, … } }`) — exactly as for fcrig. It adds:

- `{ bass: { <id>: <value>, … } }` (and `processorOptions.bass`): the chain's knobs by the app's ids;
- `{ cab: { taps: Float32Array, rate } }` (and `processorOptions.cab`): the cabinet's IR — answered `{ cab: true|false }`;
- `{ read }` answers `{ bass: { compGrDb } }` beside the player's `{ state }`.

```js
const node = new AudioWorkletNode(context, 'fc-bass', {
    numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2], channelCount: 1, channelCountMode: 'explicit',
    processorOptions: { module, rig: rigJsonBytes, files, preload: true,
                        knobs: { gain: 180, switch: { Voice: 'SVT' }, dial: { Bass: 150, Mid: 150, Freq: 150, Treble: 150 } },
                        bass: { comp: 1, comp_ratio: 4, comp_character: 2, comp_threshold: -24, cab_mix: 70,
                                bass_hpf_on: 1, bass_lpf_on: 1, amp_blend_wet: 1 },
                        cab: { taps, rate: 48000 } } });
```

## What a page owes it

- **The rate.** Make the AudioContext at the pack's rate (48 kHz, as the guitar demos do: the demo's `sampleRate`). At
  another rate the player's rate-match runs, and there the web is not its native build to the bound below (see the parity).
- **The knobs by the app's ids** (`fc_bass_abi.h`), set from the product's opening (the face kit's export): for
  RedGrooveBass, `src/redgroove/RedGrooveLineup.cpp`'s pinned values — COMP on, BLEND 1, 4:1, OPTO; OCTAVE, ENVELOPE and
  CHORUS present and on, their sliders their amounts; both cuts on, the switches choosing their places — and the gear's
  own: its BLEND, COMP's threshold, the three effects, DI/CAB.
- **DRIVE** is the player's gain dial, `value / 10` of the dial's sweep (300°: Grit). The app's DRIVE is STEP by default
  (SMOOTH off): it plays the captured position nearest the knob (the first of two as near) — a page that wants the app's
  sound plays that; a page that turns DRIVE continuously plays the blend of the two captures around it.
- **The tone.** On a pack with a tone of its own — the SGT-DI has one: Bass, Mid, Freq, Treble, Ultra Lo, Ultra Hi — the
  walker's BASS, MIDDLE, MID FREQ and TREBLE are the pack's: `{ set: { dial: { Bass, Mid, Freq, Treble } } }`, each
  `value · 300` (the app's tone slot 0…1 times the dial's sweep), and the bass standard's `preamp_eq_*` are not sent
  (the app holds them flat on such a pack). On a pack without one, the `preamp_eq_*` ids are the tone.
- **The gear** is the pack's `Voice` switch (`{ set: { switch: { Voice: 'B15' } } }`).
- **The cabinet.** Render the gear's Cab IR Generator preset with the site's cabir module (`cabir_render (toml, 0)`, the
  same renderer and size the plug-in uses), take the 24-bit WAV at `cabir_wav()` and hand in its samples as the plug-in
  reads them — each 24-bit integer over 2^23 — as `{ cab: { taps, rate: 48000 } }`. The module levels it to the bass's
  reference itself; do not normalise it.
- **The lid.** The plug-in's output trim and its true-peak limiter are the page's: the module ends at the bass's cuts.

## The parity — the web against the plug-in

`bass-parity.mjs` plays orbit-amp's `RedGrooveBassReference` sessions (RedGrooveBass, both gears, the walker as it opens,
every stage at work, and at work with the limiter out; a synthetic phrase and a DI take) through the worklet as a page
runs it and through `fc_bass_render`, from the plug-in's own parameters (`--params`) and cabinets (`--cabinets`):

```sh
node --experimental-vm-modules tools/wasm-bass/bass-parity.mjs <build-dir> <fc_bass_render> <references-dir> <pack-dir>
```

Measured on arm64 macOS (06.11.2026; references at orbit-amp 1c39071, 54 renders, each rendered twice bit for bit):

| at 48 kHz (the site's rate), 12 sessions | worst sample | error |
|---|---|---|
| the web against its native build | 4.96e-6 | -98.0 dB |
| the web against the plug-in, the chain alone (its limiter out) | 4.96e-6 | -98.0 dB |
| the web against the plug-in, its limiter in | 3.78e-5 | -97.6 dB |
| the native build against the plug-in, the chain alone | 2.25e-6 | -143 … -111 dB |

Bounds: 2e-5 against the native build (rig-parity's: Eigen's packets and libm), 1e-4 against the plug-in.

At 44.1 kHz (the player's rate-match in) the plug-in differs from ITSELF across its host blocks by up to 4.5e-3 — where a
capture lands decides the phase its rate-match starts at — and the web differs from its native build by up to 6.3e-3 on
the SGT-DI, the AMP alone 2.3e-3 with every other stage out and the captures landing at the same quantum in both: the
rig's rate-match in wasm against native, not the chain's — an open item there. Bounded at 1e-2 and printed.
