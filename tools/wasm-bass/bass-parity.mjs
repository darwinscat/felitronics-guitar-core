// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// bass-parity — THE WEB'S BASS AMP AGAINST THE PLUG-IN'S. fcbass run the way a page runs it — fcbass-worklet.js and the
// rig half it shares (rig-port.js) in a context shaped like an AudioWorkletGlobalScope, quanta of 128, every { need }
// answered from the pack's folder — and fc_bass_render, the same ABI natively; both held to the plug-in's own renders
// (orbit-amp's RedGrooveBassReference: RedGrooveBass's whole chain, both gears, 48 and 44.1 kHz).
//
//   node --experimental-vm-modules bass-parity.mjs [--native-only] <fcbass build-dir> <fc_bass_render> <references-dir> <pack-dir>
//
// <references-dir> is what RedGrooveBassReference writes (its manifest.txt, in-*.f32, the renders, cab-<gear>.wav from
// --cabinets, params-<gear>-<setting>.txt from --params); <pack-dir> the pack it played (orbit-amp's
// resources/set/Devices/Ampeg SGT-DI.orbitrig).
//
// THE SESSION, from the plug-in's parameters as a page sets them (sessionOf): every chain knob by the app's id
// (fc_bass_set); DRIVE to the player's gain dial — a tenth of the dial's sweep a step of 0 … 10, and in STEP (the app's
// SMOOTH off, its default) the captured position nearest it, the first of two as near (CapturedBlock::nearestKnot); the
// app's tone slots onto the pack's tones in the pack's order — a dial a fraction of its sweep, a switch the position
// nearest (roundToInt, ties to even; CapturedBlock::updateToneIfMoved); on a pack that wears a tone of its own the bass
// standard's BASS, MIDDLE and TREBLE stand flat, so they are not sent (the app's updateEqSettings); the gear's Voice;
// the files' input trims.
//
// WHAT IS COMPARED, AND AGAINST WHAT. The plug-in's output carries its limiter's lookahead (and, at 44.1 kHz, the
// capture's rate-match, which fc_bass carries too): it is read that much later. Where the limiter stands out of the path
// ('bypass') the plug-in's chain is the chain fcbass plays, to the sample; where it is in ('open', 'working') it adds what
// a true-peak limiter does to a signal under its ceiling. The plug-in's DI mix runs on Accelerate (vDSP) on macOS, the
// web's in plain arithmetic; its NAM on the host's Eigen packets; its libm the host's. So the web is held to a measured
// tolerance, not to bits, and each number is printed.
//
// For a reading: BASS_PARITY_DUMP=<dir> writes each session's web and native renders there; BASS_PARITY_JOBS=1 prints
// the quantum each capture landed after (fc_bass_render prints its own with FC_BASS_RENDER_JOBS=1).

import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { spawnSync } from 'node:child_process';

const NATIVE_ONLY = process.argv.includes('--native-only');   // a row with no wasm build: the native ABI against the plug-in
const [dir, renderBin, refs, packDir] = process.argv.slice(2).filter((a) => a !== '--native-only');
if (!dir || !renderBin || !refs || !packDir) {
    console.error('usage: bass-parity.mjs <fcbass build-dir> <fc_bass_render> <references-dir> <pack-dir>');
    process.exit(2);
}
if (typeof vm.SourceTextModule !== 'function') {
    console.error('bass-parity: needs node --experimental-vm-modules');
    process.exit(2);
}

// THE TOLERANCES. The web against its own native build: one source, one ABI — what differs is Eigen's packets (fused
// on native arm64, twice-rounded in wasm) and libm, through a capture's twenty-odd layers: rig-parity.mjs measured
// 1.7e-6 … 5.5e-6 on the site's packs, and bounds it at 2e-5. The web against the plug-in, the chain alone ('bypass'):
// those and the DI mix's vector library; with its limiter in, what the limiter adds besides. Measured, then bounded
// with room (README.md has the measurements).
const WASM_NATIVE = 2e-5;
const WEB_PLUGIN = { bypass: 1e-4, open: 1e-4, working: 1e-4 };
// AT A SESSION RATE OTHER THAN THE PACK'S (44.1 kHz against the SGT-DI's 48), the player's rate-match runs, and there
// the web is NOT its native build: the AMP alone — every other stage out, the captures landing at the same quantum in
// both — reads 2.3e-3 apart on the SGT-DI (measured on arm64 macOS, 06.11.2026), and the whole chain up to 6.3e-3. That
// is the rig's (felitronics::rigplayer / nam's rate-match in wasm against native), not the chain's, and it is an open
// item there; the plug-in itself spreads up to 4.5e-3 across its own host blocks at 44.1 kHz (where a capture lands
// decides its rate-match's phase). The site plays at the demo's 48 kHz (GuitarDemoSpec sampleRate; chain.ts makes its
// AudioContext at it), where none of this runs. Bounded here so a worse regression still fails, and printed.
const OTHER_RATE_BOUND = 1e-2;
const Q = 128;

let failures = 0, checks = 0;
const ok = (cond, msg) => { ++checks; if (!cond) { ++failures; console.log('    FAIL: ' + msg); } };
const group = (name) => console.log('  - ' + name);

// ---- the plug-in's references ---------------------------------------------------------------------------------------
const manifest = new Map();
for (const line of fs.readFileSync(path.join(refs, 'manifest.txt'), 'utf8').split('\n').filter(Boolean)) {
    const [name, ...kv] = line.split(' ');
    manifest.set(name, Object.fromEntries(kv.map((p) => p.split('='))));
}
const f32 = (file) => { const b = fs.readFileSync(file); return new Float32Array(b.buffer, b.byteOffset, b.byteLength / 4); };
const params = (gear, setting) => Object.fromEntries(fs.readFileSync(path.join(refs, `params-${gear}-${setting}.txt`), 'utf8')
    .split('\n').filter(Boolean).map((l) => { const [id, v] = l.split(' '); return [id, Number(v)]; }));

/** JUCE's roundToInt: ties to even. */
const roundToInt = (v) => { const f = Math.floor(v), d = v - f; return d > 0.5 ? f + 1 : d < 0.5 ? f : (f % 2 === 0 ? f : f + 1); };

// ---- the session ----------------------------------------------------------------------------------------------------
const rigJson = JSON.parse(fs.readFileSync(path.join(packDir, 'rig.json'), 'utf8'));
// THE FILES UP FRONT, as the site hands a pack's files in with it: every capture the pack names (fc_bass_render does the
// same), so a capture lands at the same quantum in both renders — at a session rate other than the pack's, that decides
// the phase its rate-match starts at.
const packFiles = [...new Set(rigJson.chain.flatMap((s) => (s.files || []).map((f) => f.file)))]
    .filter((f) => fs.existsSync(path.join(packDir, f)));
const stage = rigJson.chain.find((s) => s.kind === 'nam');
const tones = stage.tone || [];
const gainControl = stage.controls.find((c) => c.role === 'gain');
const sweep = gainControl ? gainControl.sweep : 0;
const knots = gainControl ? gainControl.values.map(Number) : [];
// A tone the app plays as the pack's: a dial, or a switch whose positions carry filters (CapturedBlock::computeWearableTone).
const wearsTone = tones.some((t) => t.sweep > 0 || (t.positions || []).some((p) => (p.sections || []).length > 0));
const CHAIN_IDS = ['gate_on', 'gate_threshold', 'gate_pos', 'comp_on', 'comp', 'comp_threshold', 'comp_ratio', 'comp_attack',
    'comp_release', 'comp_character', 'octave_on', 'octave_present', 'octave', 'octave_tone_hz', 'filter_on', 'filter_present',
    'filter', 'filter_mode', 'filter_direction', 'filter_resonance', 'filter_range', 'filter_keep_lows', 'filter_keep_hz',
    'filter_release_ms', 'chorus_on', 'chorus_present', 'chorus', 'chorus_rate_hz', 'chorus_crossover_hz', 'preamp_eq_lo_hz',
    'preamp_eq_b1_hz', 'preamp_eq_b1_q', 'preamp_eq_hi_hz', 'amp_blend_wet', 'amp_blend_hz', 'amp_blend_invert', 'cab_on',
    'cab_mix', 'bass_hpf_on', 'bass_hpf', 'bass_hpf_hz_4', 'bass_hpf_hz_5', 'bass_lpf_on', 'bass_lpf', 'bass_lpf_hz_fingers',
    'bass_lpf_hz_slap'];
const STANDARD_TONE = ['preamp_eq_lo_db', 'preamp_eq_b1_db', 'preamp_eq_hi_db'];

function sessionOf(p, gear) {
    const chain = {};
    for (const id of CHAIN_IDS) chain[id] = p[id];
    if (!wearsTone) for (const id of STANDARD_TONE) chain[id] = p[id];
    let deg = Math.min(1, Math.max(0, p.preamp_gain / 10)) * sweep;
    if (!(p.preamp_smooth > 0.5) && knots.length) {
        let best = knots[0];
        for (const k of knots) if (Math.abs(k - deg) < Math.abs(best - deg)) best = k;
        deg = best;
    }
    const dial = {}, sw = { Voice: gear };
    for (let i = 0; i < Math.min(6, tones.length); ++i) {
        const t = tones[i], v = Math.fround(p['preamp_meas' + (i + 1)]);
        if (t.sweep > 0) dial[t.name] = Math.min(1, Math.max(0, v)) * t.sweep;
        else if ((t.positions || []).length) {
            const n = t.positions.length;
            sw[t.name] = t.positions[Math.min(n - 1, Math.max(0, roundToInt(Math.fround(v * (n - 1)))))].value;
        }
    }
    return { chain, gain: deg, dial, switch: sw, inputTrims: p.pack_level_comp > 0.5 };
}

/** The session as fc_bass_render's lines — the player's knobs IN THE ORDER the processor lands them (rig-port.js apply:
    the input trims, the switches, the dials, the gain): at a session rate other than the pack's, which captures load on
    the way, and when, decides the phase each one's rate-match starts at, so the two renders must take the same way. */
function sessionText(s) {
    const lines = [];
    for (const [id, v] of Object.entries(s.chain)) lines.push(`set\t${id}\t${v}`);
    lines.push(`trims\t${s.inputTrims ? 1 : 0}`);
    for (const [n, v] of Object.entries(s.switch)) lines.push(`switch\t${n}\t${v}`);
    for (const [n, v] of Object.entries(s.dial)) lines.push(`dial\t${n}\t${v}`);
    lines.push(`gain\t${s.gain}`);
    return lines.join('\n') + '\n';
}

/** The cabinet's WAV as the plug-in reads it: its first channel, the integers over 2^(bits-1). */
function readWav(file) {
    const b = fs.readFileSync(file);
    let channels = 0, bits = 0, rate = 0, at = 12;
    while (at + 8 <= b.length) {
        const id = b.toString('latin1', at, at + 4), size = b.readUInt32LE(at + 4);
        if (id === 'fmt ') { channels = b.readUInt16LE(at + 10); rate = b.readUInt32LE(at + 12); bits = b.readUInt16LE(at + 22); }
        if (id === 'data') {
            const frame = channels * bits / 8, n = Math.floor(size / frame), taps = new Float32Array(n);
            for (let i = 0; i < n; ++i) {
                const o = at + 8 + i * frame;
                taps[i] = bits === 24 ? b.readIntLE(o, 3) / 8388608 : bits === 16 ? b.readInt16LE(o) / 32768 : b.readFloatLE(o);
            }
            return { taps, rate };
        }
        at += 8 + size + (size & 1);
    }
    throw new Error('no data in ' + file);
}

// ---- the simulated AudioWorkletGlobalScope (rig-parity.mjs's, its port and its clock) -------------------------------
class AudioWorkletProcessor {
    constructor() {
        this.port = { onmessage: null, posted: [],
                      postMessage(m, transfer) { this.posted.push(transfer && transfer.length ? structuredClone(m, { transfer }) : m); },
                      close() {} };
    }
}

async function loadWorklet(sampleRate) {
    let registered = null;
    const ctx = vm.createContext({ console, AudioWorkletProcessor, sampleRate, currentFrame: 0, currentTime: 0,
                                   registerProcessor: (name, cls) => { registered = { name, cls }; } });
    vm.runInContext('globalThis.AudioWorkletGlobalScope = function AudioWorkletGlobalScope() {};'
                    + 'delete WebAssembly.Memory.prototype.toResizableBuffer;', ctx);
    const meta = (url) => ({ initializeImportMeta(m) { m.url = url; } });
    const glue = new vm.SourceTextModule(fs.readFileSync(path.join(dir, 'fcbass.mjs'), 'utf8'),
                                         { context: ctx, identifier: 'fcbass.mjs', ...meta('https://worklet.invalid/fcbass.mjs') });
    const port = new vm.SourceTextModule(fs.readFileSync(path.join(dir, 'rig-port.js'), 'utf8'),
                                         { context: ctx, identifier: 'rig-port.js', ...meta('https://worklet.invalid/rig-port.js') });
    const worklet = new vm.SourceTextModule(fs.readFileSync(path.join(dir, 'fcbass-worklet.js'), 'utf8'),
                                            { context: ctx, identifier: 'fcbass-worklet.js', ...meta('https://worklet.invalid/fcbass-worklet.js') });
    await worklet.link((spec, ref) => {
        if (spec === './fcbass.mjs' && ref === worklet) return glue;
        if (spec === './rig-port.js' && ref === worklet) return port;
        throw new Error('the worklet imports ' + spec);
    });
    await worklet.evaluate();
    if (!registered || registered.name !== 'fc-bass') throw new Error('no fc-bass processor registered');
    vm.runInContext('Date.now = (() => { let t = 1.7e12; return () => (t += 1); })();', ctx);
    const module = await vm.runInContext('WebAssembly', ctx).compile(fs.readFileSync(path.join(dir, 'fcbass.wasm')));
    const buffer = (b) => { const a = vm.runInContext('(n) => new ArrayBuffer(n)', ctx)(b.length); new Uint8Array(a).set(b); return a; };
    const floats = (f) => { const a = vm.runInContext('(n) => new Float32Array(n)', ctx)(f.length); a.set(f); return a; };
    return { cls: registered.cls, module, buffer, floats, ctx };
}

async function settle(proc, pred) {
    for (let i = 0; i < 4000; ++i) {
        const hit = proc.port.posted.find(pred);
        if (hit) return hit;
        await new Promise((r) => setImmediate(r));
    }
    return undefined;
}

function answer(proc, w) {
    proc.answered ??= 0;
    while (proc.answered < proc.port.posted.length) {
        const msg = proc.port.posted[proc.answered++];
        if (msg.need === undefined) continue;
        const file = path.join(packDir, msg.need);
        proc.port.onmessage({ data: { file: msg.need, pack: msg.pack, bytes: fs.existsSync(file) ? w.buffer(fs.readFileSync(file)) : null } });
    }
}

/** The web's render: the processor as a page makes it, the session as knobs, `warm` frames of silence, then `input`. */
async function webRender(w, session, cab, input, warm) {
    const proc = new w.cls({ numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
        processorOptions: { module: w.module, rig: w.buffer(fs.readFileSync(path.join(packDir, 'rig.json'))),
                            files: packFiles.map((f) => ({ file: f, bytes: w.buffer(fs.readFileSync(path.join(packDir, f))) })),
                            calibrate: false, maxModels: 2, quietWarm: false,
                            knobs: { gain: session.gain, dial: session.dial, switch: session.switch, inputTrims: session.inputTrims },
                            bass: session.chain, cab: { taps: w.floats(cab.taps), rate: cab.rate } } });
    const up = await settle(proc, (m) => m.ready || m.error);
    if (!up || !up.ready) throw new Error('the processor did not come up: ' + JSON.stringify(up));
    const cabAnswer = proc.port.posted.find((m) => m.cab !== undefined);
    ok(cabAnswer && cabAnswer.cab === true, 'the cabinet taken');
    const iL = new Float32Array(Q), oL = new Float32Array(Q), oR = new Float32Array(Q);
    const out = new Float32Array(2 * input.length);
    const run = (from, n, src) => {
        for (let at = 0; at < n; at += Q) {
            iL.fill(0);
            if (src) iL.set(src.subarray(at, Math.min(n, at + Q)));
            proc.process([[iL]], [[oL, oR]], {});
            proc.quanta = (proc.quanta || 0) + 1;
            if (process.env.BASS_PARITY_JOBS)
                for (proc.seenLoaded ??= 0; proc.seenLoaded < proc.port.posted.length; ++proc.seenLoaded) {
                    const msg = proc.port.posted[proc.seenLoaded];
                    if (msg.loaded !== undefined) console.log(`      job ${msg.loaded} after quantum ${proc.quanta}`);
                }
            answer(proc, w);
            if (src) for (let i = 0; i < Q && at + i < n; ++i) { out[2 * (at + i)] = oL[i]; out[2 * (at + i) + 1] = oR[i]; }
        }
    };
    run(0, warm, null);
    run(0, input.length, input);
    const err = proc.port.posted.find((m) => m.error);
    ok(!err, 'no error from the processor' + (err ? ': ' + err.error : ''));
    proc.onMessage({ dispose: true });
    return out;
}

function nativeRender(session, cabFile, inFile, rate, warm, tmp) {
    const sFile = path.join(tmp, 'session.txt'), oFile = path.join(tmp, 'native.f32');
    fs.writeFileSync(sFile, sessionText(session));
    const r = spawnSync(renderBin, [packDir, String(rate), String(Q), String(warm), cabFile, sFile, inFile, oFile], { encoding: 'utf8' });
    if (r.status !== 0) throw new Error('fc_bass_render: ' + r.stderr);
    const latency = Number((/latency (-?\d+)/.exec(r.stderr) || [])[1]);
    return { out: f32(oFile), latency };
}

/** The largest difference and the error's level against `ref`, `b` read `shift` frames earlier than `ref`. */
function measure(ref, b, shift) {
    let worst = 0, e = 0, s = 0;
    const frames = Math.min(ref.length / 2 - shift, b.length / 2);
    for (let i = 0; i < frames; ++i)
        for (let c = 0; c < 2; ++c) {
            const x = ref[2 * (i + shift) + c], d = x - b[2 * i + c];
            worst = Math.max(worst, Math.abs(d));
            e += d * d; s += x * x;
        }
    return { worst, db: e > 0 ? 10 * Math.log10(e / s) : -Infinity };
}

// ---- the run --------------------------------------------------------------------------------------------------------
const tmp = fs.mkdtempSync(path.join(fs.realpathSync(process.env.TMPDIR || '/tmp'), 'bass-parity-'));
console.log(`bass-parity: ${dir} against ${refs} (the pack wears a tone of its own: ${wearsTone})`);
for (const rate of [48000, 44100]) {
    const w = NATIVE_ONLY ? null : await loadWorklet(rate);
    for (const input of ['phrase', 'take']) {
        const inFile = path.join(refs, `in-${input}-${rate}.f32`);
        if (!fs.existsSync(inFile)) continue;
        const inSamples = f32(inFile);
        for (const gear of ['B15', 'SVT']) {
            const cabFile = path.join(refs, `cab-${gear}.wav`), cab = readWav(cabFile);
            for (const setting of ['open', 'working', 'bypass']) {
                const name = `${input}-${gear}-${setting}-${rate}-512`;
                const m = manifest.get(name);
                if (!m) continue;
                group(name);
                const session = sessionOf(params(gear, setting), gear);
                const warm = Number(m.warm);
                const { out: native, latency: rigLatency } = nativeRender(session, cabFile, inFile, rate, warm, tmp);
                const plugin = f32(path.join(refs, name + '.f32'));
                // THE PLUG-IN'S OWN SPREAD: the same render at its other host blocks. At 48 kHz it is ~3e-8; at a rate other
                // than the pack's the capture's rate-match starts where the capture landed, which the host's block decides,
                // so the plug-in differs from ITSELF by up to a few 1e-3 — the web is held to that, not to less.
                let spread = 0;
                for (const blk of [64, 333]) {
                    const other = path.join(refs, `${input}-${gear}-${setting}-${rate}-${blk}.f32`);
                    if (fs.existsSync(other)) spread = Math.max(spread, measure(plugin, f32(other), 0).worst);
                }
                const bound = Math.max(WEB_PLUGIN[setting], 1.5 * spread);
                if (NATIVE_ONLY) {
                    const np = measure(plugin, native, Number(m.latency) - rigLatency);
                    console.log(`      native vs plug-in ${np.worst.toExponential(2)} (${np.db.toFixed(1)} dB)`);
                    ok(np.worst <= bound, `${name}: the native ABI within ${bound.toExponential(2)} of the plug-in (${np.worst})`);
                    continue;
                }
                const web = await webRender(w, session, cab, inSamples, warm);
                const shift = Number(m.latency) - rigLatency;   // the plug-in's limiter's lookahead: its latency less the player's
                if (process.env.BASS_PARITY_DUMP) fs.writeFileSync(path.join(process.env.BASS_PARITY_DUMP, `web-${name}.f32`), web);
                if (process.env.BASS_PARITY_DUMP) fs.writeFileSync(path.join(process.env.BASS_PARITY_DUMP, `native-${name}.f32`), native);
                const wn = measure(native, web, 0), wp = measure(plugin, web, shift), np = measure(plugin, native, shift);
                console.log(`      web vs native ${wn.worst.toExponential(2)} (${wn.db.toFixed(1)} dB); web vs plug-in ${wp.worst.toExponential(2)} `
                            + `(${wp.db.toFixed(1)} dB); native vs plug-in ${np.worst.toExponential(2)} (${np.db.toFixed(1)} dB); shift ${shift}; `
                            + `the plug-in's own block spread ${spread.toExponential(2)}`);
                const packRate = rigLatency === 0;   // the player plays the captures at their own rate: no rate-match
                const nb = packRate ? WASM_NATIVE : OTHER_RATE_BOUND, pb = packRate ? bound : Math.max(bound, OTHER_RATE_BOUND);
                ok(wn.worst <= nb, `${name}: the web within ${nb} of its native build (${wn.worst})`);
                ok(wp.worst <= pb, `${name}: the web within ${pb.toExponential(2)} of the plug-in (${wp.worst})`);
            }
        }
    }
}
console.log(`\n${checks} checks, ${failures} failures`);
process.exit(failures === 0 ? 0 : 1);
