// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// rig-parity — the built fcrig module, run the way a page runs it, against the native render of the same ABI.
//
//   node --experimental-vm-modules --expose-gc tools/wasm-rig/rig-parity.mjs <build-dir> <fc_rig_render> <abi-header>
//        [<packs-dir>]
//
// <build-dir> holds what build.sh ships (fcrig.mjs, fcrig.wasm, fcrig-worklet.js). The fixture pack (written by
// fc_rig_render --fixture) is always played; <packs-dir>, when given, adds real packs: every one of PACKS found there
// as <name>.orbitrig/ (rig.json beside its captures). What it proves:
//   1. THE WORKLET LOADS WHERE IT WILL RUN: the processor and the glue as ES modules in a context shaped like an
//      AudioWorkletGlobalScope (no fetch, URL, TextDecoder, TextEncoder, atob, setTimeout, performance), the module
//      posted as a WebAssembly.Module, WebAssembly.Memory without toResizableBuffer, and the page simulated: every
//      { need } answered with the file's bytes before the next quantum.
//   2. THE EXPORTS ARE THE ABI: exactly the header's fc_rig_* entry points.
//   3. THE RENDER IS THE NATIVE ONE within TOLERANCE, on every pack, two models and one, knobs moved mid-stream.
//   4. THE EXCEPTIONS WORK where they are allowed: garbage handed in for a capture is a failed load, not a trap.
//   5. THE START'S DECISION: a fast render turns the blend on, an injected slow one keeps one model; the start is
//      silent until then, and knobs sent meanwhile land after it, in order.
//   6. AN ERROR IS NOT AN END (no module, a refused prepare, a foreign quantum, a trap): dry, alive, one { error }.
//   7. NO ALLOCATION PER QUANTUM, and the cost of one quantum with one capture sounding and with two — reported.

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import vm from 'node:vm';
import { spawnSync } from 'node:child_process';
import { performance, PerformanceObserver } from 'node:perf_hooks';

const [dir, renderBin, header, packsDir] = process.argv.slice(2);
if (!dir || !renderBin || !header) {
    console.error('usage: rig-parity.mjs <build-dir> <fc_rig_render> <fc_rig_abi.h> [<packs-dir>]');
    process.exit(2);
}
if (typeof vm.SourceTextModule !== 'function') {
    console.error('rig-parity: needs node --experimental-vm-modules');
    process.exit(2);
}

// LAW 10, AND WHAT IT CANNOT PROMISE HERE. Both rows compile one source under `-ffp-contract=on`, but a NAM capture
// is Eigen's matrix products, and Eigen spells its multiply-add itself: on native arm64 its NEON packets use a fused
// vfmaq, in wasm its SSE packets (lowered to SIMD128, which has no FMA) round twice. Every one of thousands of dot
// products per sample may then differ by an ulp or so, through twenty-odd layers and an activation each (that this
// is where the difference comes from is the reading, not a measurement isolating it). Measured on arm64 macOS: the
// fixture's tiny captures 1.2e-7 (as fcguitarfx: no deep network); the three site packs, 3 s, two models and one,
// 1.7e-6 … 5.5e-6. 2e-5 (-94 dBFS) leaves room for another native row's packets and none for a wrong knob, a wrong
// capture or a wrong order — any of which moves a sample by 1e-2 or more.
const TOLERANCE = 2e-5;
const PACKS = ['X88-IR Crunch', 'IR-X Ch1 PowerAmp', 'V4 Sheriff Ch1'];
const Q = 128;

let failures = 0, checks = 0;
const ok = (cond, msg) => { ++checks; if (!cond) { ++failures; console.log('    FAIL: ' + msg); } };
const group = (name) => console.log('  - ' + name);

// ---- the simulated AudioWorkletGlobalScope -------------------------------------------------------------------------
class AudioWorkletProcessor {
    constructor() {
        this.port = { onmessage: null, posted: [], closed: false,
                      postMessage(m) { this.posted.push(m); }, close() { this.closed = true; } };
    }
}
const GLUE = fs.readFileSync(path.join(dir, 'fcrig.mjs'), 'utf8');
const WRAPPER = "import create from './glue.mjs';\n" +
    'export default async function (arg) { const m = await create(arg); globalThis.__onRigModule?.(m); return m; }\n';

async function loadWorklet(sampleRate) {
    let registered = null;
    const ctx = vm.createContext({
        console, AudioWorkletProcessor, sampleRate, currentFrame: 0, currentTime: 0,
        registerProcessor: (name, cls) => { registered = { name, cls }; },
    });
    vm.runInContext('globalThis.AudioWorkletGlobalScope = function AudioWorkletGlobalScope() {};' +
                    'delete WebAssembly.Memory.prototype.toResizableBuffer;', ctx);
    const leaks = vm.runInContext(
        "['fetch', 'URL', 'TextDecoder', 'TextEncoder', 'atob', 'setTimeout', 'importScripts', 'window', 'self', 'performance']" +
        '.filter((n) => typeof globalThis[n] !== "undefined")', ctx);
    if (leaks.length) throw new Error('the simulated worklet scope leaks ' + leaks.join(', '));
    const glue = new vm.SourceTextModule(GLUE, { context: ctx, identifier: 'fcrig.mjs',
        initializeImportMeta(meta) { meta.url = 'https://worklet.invalid/fcrig.mjs'; } });
    const wrapper = new vm.SourceTextModule(WRAPPER, { context: ctx, identifier: 'fcrig.mjs (spy)' });
    const worklet = new vm.SourceTextModule(fs.readFileSync(path.join(dir, 'fcrig-worklet.js'), 'utf8'), {
        context: ctx, identifier: 'fcrig-worklet.js',
        initializeImportMeta(meta) { meta.url = 'https://worklet.invalid/fcrig-worklet.js'; } });
    await worklet.link((spec, referrer) => {
        if (spec === './fcrig.mjs' && referrer === worklet) return wrapper;
        if (spec === './glue.mjs' && referrer === wrapper) return glue;
        throw new Error('the worklet imports ' + spec);
    });
    await worklet.evaluate();
    if (!registered || registered.name !== 'fc-rig') throw new Error('no fc-rig processor registered');
    const bytes = fs.readFileSync(path.join(dir, 'fcrig.wasm'));
    const module = await vm.runInContext('WebAssembly', ctx).compile(bytes);
    const buffer = (b) => { const a = vm.runInContext('(n) => new ArrayBuffer(n)', ctx)(b.length); new Uint8Array(a).set(b); return a; };
    return { cls: registered.cls, module, wasmBytes: buffer(bytes), buffer, ctx };
}

async function settle(proc, pred = (m) => true) {
    for (let i = 0; i < 2000; ++i) {
        const hit = proc.port.posted.find(pred);
        if (hit) return hit;
        await new Promise((r) => setImmediate(r));
    }
    return undefined;
}

/** The page's half: every { need } posted since the last call answered with the file's bytes (or `override`). */
function answer(proc, w, packDir, override) {
    proc.answered ??= 0;
    while (proc.answered < proc.port.posted.length) {
        const msg = proc.port.posted[proc.answered++];
        if (msg.need === undefined) continue;
        const forced = override && override(msg.need);
        const file = path.join(packDir, msg.need);
        const bytes = forced !== undefined ? forced : (fs.existsSync(file) ? w.buffer(fs.readFileSync(file)) : null);
        proc.port.onmessage({ data: { file: msg.need, pack: msg.pack, bytes } });
    }
}

async function newProcessor(w, packDir, opts = {}, options = { module: w.module }) {
    const rig = packDir ? w.buffer(fs.readFileSync(path.join(packDir, 'rig.json'))) : undefined;
    const proc = new w.cls({ numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
                             processorOptions: { ...options, rig, ...opts } });
    const first = await settle(proc, (m) => m.ready || m.error);
    if (!first || !first.ready) throw new Error('the processor did not come up: ' + JSON.stringify(first));
    return proc;
}

function compare(a, b) {
    let worst = 0, differ = 0, nonFinite = 0;
    for (let ch = 0; ch < a.length; ++ch)
        for (let i = 0; i < a[ch].length; ++i) {
            if (!Number.isFinite(a[ch][i]) || !Number.isFinite(b[ch][i])) { ++nonFinite; continue; }
            const d = Math.abs(a[ch][i] - b[ch][i]);
            if (d > 0) ++differ;
            if (d > worst) worst = d;
        }
    return { worst, differ, nonFinite, within: (tol) => nonFinite === 0 && worst <= tol };
}

// ---- a fixed input: a DI-ish riff, stereo-different, tapered -------------------------------------------------------
function makeInput(frames, sr, ch) {
    const x = new Float32Array(frames);
    let seed = (0x2545f491 + ch) >>> 0;
    for (let i = 0; i < frames; ++i) {
        const t = i / sr, pluck = t % 0.5;
        const env = Math.exp(-pluck * 5) * (pluck < 0.002 ? pluck / 0.002 : 1);
        const f = (ch === 0 ? 82.41 : 110) * (1 + Math.floor(t / 0.5) % 3 * 0.25);
        let v = 0.3 * env * (Math.sin(2 * Math.PI * f * t) + 0.4 * Math.sin(2 * Math.PI * 2 * f * t));
        seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
        if (t > 1.2 && t < 1.25) v += 0.1 * ((seed >>> 8) / 16777216 - 0.5);
        x[i] = v;
    }
    return x;
}

/** A knob message as fc_rig_render's script, in the order the processor applies a { set }. */
function scriptOf(moves) {
    const args = [];
    for (const { frame, set: s } of moves) {
        const at = '@' + frame;
        if (s.maxModels !== undefined) args.push(at, 'max', String(s.maxModels));
        if (s.normalize !== undefined) args.push(at, 'normalize', s.normalize ? '1' : '0');
        if (s.inputTrims !== undefined) args.push(at, 'trims', s.inputTrims ? '1' : '0');
        if (s.blendShape) args.push(at, 'shape', String(s.blendShape[0]), String(s.blendShape[1]));
        for (const k in s.switch || {}) args.push(at, 'switch', k, String(s.switch[k]));
        for (const k in s.dial || {}) args.push(at, 'dial', k, String(s.dial[k]));
        if (s.gain !== undefined) args.push(at, 'gain', String(s.gain));
    }
    return args;
}

function renderNative(packDir, sr, frames, channels, maxModels, moves, ins) {
    const args = [packDir, String(sr), String(frames), String(channels), 'max', String(maxModels), ...scriptOf(moves)];
    const input = Buffer.alloc(frames * 4 * channels);
    for (let c = 0; c < channels; ++c) Buffer.from(ins[c].buffer).copy(input, c * frames * 4);
    const r = spawnSync(renderBin, args, { input, maxBuffer: frames * 4 * channels + 1024 });
    if (r.status !== 0) throw new Error(`fc_rig_render exited ${r.status}: ${r.stderr}`);
    const out = new Float32Array(r.stdout.buffer.slice(r.stdout.byteOffset, r.stdout.byteOffset + frames * 4 * channels));
    return Array.from({ length: channels }, (_, c) => out.subarray(c * frames, (c + 1) * frames));
}

async function renderWorklet(w, packDir, frames, channels, maxModels, moves, ins, override) {
    const proc = await newProcessor(w, packDir, { calibrate: false, maxModels, stereo: channels === 2 });
    const iL = new Float32Array(Q), iR = new Float32Array(Q), oL = new Float32Array(Q), oR = new Float32Array(Q);
    const outs = Array.from({ length: channels }, () => new Float32Array(frames));
    const inputs = [channels === 2 ? [iL, iR] : [iL]], outputs = [[oL, oR]];
    let next = 0;
    for (let at = 0; at < frames; at += Q) {
        while (next < moves.length && moves[next].frame <= at) proc.port.onmessage({ data: { set: moves[next++].set } });
        iL.set(ins[0].subarray(at, at + Q));
        if (channels === 2) iR.set(ins[1].subarray(at, at + Q));
        proc.process(inputs, outputs, {});
        outs[0].set(oL, at);
        if (channels === 2) outs[1].set(oR, at);
        answer(proc, w, packDir, override);
    }
    return { outs, proc };
}

/** Knob moves for a pack, read off its manifest: the gain dial through captures and between them, every other
    captured control to another value, every tone knob turned. */
function movesFor(packDir, frames) {
    const rig = JSON.parse(fs.readFileSync(path.join(packDir, 'rig.json'), 'utf8'));
    const st = rig.chain.find((s) => s.kind === 'nam');
    const dev = st.device || st;
    const sweep = (dev.controls.find((c) => c.role === 'gain' && c.sweep) || {}).sweep || 300;
    const step = Math.floor(frames / Q / 10) * Q;
    const moves = [
        { frame: step * 1, set: { gain: sweep * 0.55 } },     // between two captures
        { frame: step * 2, set: { gain: sweep * 0.8 } },
        { frame: step * 3, set: { gain: sweep * 0.25 } },
    ];
    let k = 4;
    for (const c of dev.controls)
        if (!(c.role === 'gain' && c.sweep) && c.values && c.values.length > 1)
            moves.push({ frame: step * Math.min(k++, 9), set: { switch: { [c.name]: c.values[c.values.length - 1] } } });
    const tone = {};
    for (const t of st.tone || []) {
        if (t.sweep) tone[t.name] = t.sweep * 0.8;
    }
    moves.push({ frame: step * Math.min(k++, 9), set: { dial: tone } });
    for (const t of st.tone || [])
        if (!t.sweep && t.positions && t.positions.length > 1)
            moves.push({ frame: step * Math.min(k++, 9), set: { switch: { [t.name]: t.positions[t.positions.length - 1].value } } });
    moves.push({ frame: step * 9, set: { gain: sweep * 0.5, normalize: false } });
    return moves.sort((a, b) => a.frame - b.frame);
}

// ---- the packs ------------------------------------------------------------------------------------------------------
const fixtureDir = fs.mkdtempSync(path.join(os.tmpdir(), 'fc-rig-fixture-'));
{
    const r = spawnSync(renderBin, ['--fixture', fixtureDir]);
    if (r.status !== 0) throw new Error('fc_rig_render --fixture failed: ' + r.stderr);
}
const packs = [{ name: 'fixture', dir: fixtureDir }];
if (packsDir)
    for (const p of PACKS) {
        const d = path.join(packsDir, p + '.orbitrig');
        if (fs.existsSync(path.join(d, 'rig.json'))) packs.push({ name: p, dir: d });
        else console.log(`  (no ${p} under ${packsDir})`);
    }
const realPack = packs.find((p) => p.name !== 'fixture') || packs[0];

const w0 = await loadWorklet(48000);

group('the worklet loads in an AudioWorkletGlobalScope, and its exports are the ABI');
{
    const proc = await newProcessor(w0, null, { calibrate: false });
    const want = [...fs.readFileSync(header, 'utf8').matchAll(/^[A-Za-z_][A-Za-z0-9_ *]* (fc_rig_[a-z_]+) \(/gm)]
        .map((m) => '_' + m[1]).sort();
    const got = Object.keys(proc.m).filter((k) => k.startsWith('_fc_rig_')).sort();
    ok(want.length === 35, `the header declares 35 entry points (read ${want.length})`);
    ok(JSON.stringify(got) === JSON.stringify(want), `the module exports exactly the header's entry points\n      got  ${got}\n      want ${want}`);
    ok(proc.m._fc_rig_abi_version() === 1, 'abi_version answers 1');
    ok(!/toResizableBuffer/.test(GLUE), 'the glue never calls WebAssembly.Memory.toResizableBuffer (Chrome 144+)');
    proc.port.onmessage({ data: { dispose: true } });
}

group(`the wasm render against the native render, within ${TOLERANCE}`);
for (const pack of packs) {
    const frames = Math.round(48000 * 3 / Q) * Q;
    const moves = movesFor(pack.dir, frames);
    for (const [maxModels, channels] of [[2, 1], [1, 1], ...(pack.name === 'fixture' ? [[2, 2]] : [])]) {
        const ins = [makeInput(frames, 48000, 0), makeInput(frames, 48000, 1)].slice(0, channels);
        const { outs, proc } = await renderWorklet(w0, pack.dir, frames, channels, maxModels, moves, ins);
        const ref = renderNative(pack.dir, 48000, frames, channels, maxModels, moves, ins);
        const c = compare(outs, ref);
        let peak = 0;
        for (const r of ref) for (const v of r) peak = Math.max(peak, Math.abs(v));
        const loads = proc.port.posted.filter((m) => m.loaded !== undefined);
        const label = `${pack.name}, ${maxModels === 2 ? 'two models' : 'one model'}, ${channels === 2 ? 'stereo' : 'mono'}`;
        console.log(`      ${label}: ${c.differ} of ${channels * frames} samples differ, worst ${c.worst.toExponential(3)}, `
                    + `${c.nonFinite} non-finite (peak ${peak.toFixed(3)}; ${loads.length} loads, `
                    + `${loads.filter((m) => !m.ok).length} failed)`);
        ok(c.within(TOLERANCE), `${label}: worst ${c.worst}, ${c.nonFinite} non-finite, against ${TOLERANCE}`);
        ok(peak > 0.01 && loads.length >= 2 && loads.every((m) => m.ok), `${label}: the pack plays, every load landed`);
        ok(!proc.port.posted.some((m) => m.error), `${label}: no error`);
        proc.port.onmessage({ data: { dispose: true } });
    }
}

group('exceptions where they are allowed: garbage for a capture is a failed load, not a trap');
{
    const frames = Q * 400;
    const ins = [makeInput(frames, 48000, 0)];
    // …and the second garbage is 48 MiB, so handing it in GROWS the module's memory: every view made before is
    // detached, and the quanta after it play only if the processor makes its views again.
    const big = Buffer.alloc(48 << 20, 0x7b);
    const garbage = (id) => id === 'g150.namz' ? w0.buffer(Buffer.from('{ this is not a capture'))
                          : id === 'g240.nam' ? w0.buffer(big) : undefined;
    const moves = [{ frame: Q * 100, set: { gain: 150 } }, { frame: Q * 200, set: { gain: 240 } }, { frame: Q * 300, set: { gain: 60 } }];
    const memory0 = (await newProcessor(w0, null, { calibrate: false })).m.HEAPU8.buffer.byteLength;
    const { outs, proc } = await renderWorklet(w0, fixtureDir, frames, 1, 2, moves, ins, garbage);
    const memory1 = proc.m ? proc.m.HEAPU8.buffer.byteLength : 0;   // null: the processor failed (said below)
    ok(memory1 > memory0 + (40 << 20), `the memory grew during a load (${memory0 >> 20} -> ${memory1 >> 20} MiB)`);
    const failed = proc.port.posted.filter((m) => m.loaded !== undefined && !m.ok).map((m) => m.loaded);
    ok(failed.includes('g150.namz') && failed.includes('g240.nam'),
       `NAM threw and NamStage caught it, twice: { loaded, ok: false } for ${failed}`);
    ok(!proc.port.posted.some((m) => m.error) && proc.process([[new Float32Array(Q)]], [[new Float32Array(Q), new Float32Array(Q)]], {}) === true,
       'no error, no trap: the processor plays on');
    let tail = 0;
    for (let i = frames - Q * 50; i < frames; ++i) tail = Math.max(tail, Math.abs(outs[0][i]));
    ok(tail > 0.01, `and the capture that does load plays after them (peak ${tail.toFixed(3)})`);
    proc.port.onmessage({ data: { dispose: true } });
}

group('the start: one model, 12 o\'clock, a silent burst — the blend on when one render fits a quarter of the quantum');
async function calibrated(pack, slowMs) {
    const calls = { max: [], gain: [] };
    w0.ctx.__onRigModule = (m) => {
        const max = m._fc_rig_set_max_models, gain = m._fc_rig_set_gain_dial, process = m._fc_rig_process;
        m._fc_rig_set_max_models = (r, n) => { calls.max.push(n); return max(r, n); };
        m._fc_rig_set_gain_dial = (r, d) => { calls.gain.push(d); return gain(r, d); };
        if (slowMs > 0)
            m._fc_rig_process = (...a) => { const t = Date.now(); while (Date.now() - t < slowMs) { /* a slow machine */ } return process(...a); };
    };
    const proc = await newProcessor(w0, pack.dir, { knobs: { gain: 200 } });
    w0.ctx.__onRigModule = undefined;
    proc.port.onmessage({ data: { set: { gain: 210 } } });   // sent while it calibrates: lands after, in order
    const ins = makeInput(Q * 3000, 48000, 0), oL = new Float32Array(Q), oR = new Float32Array(Q);
    let loudBefore = 0, q = 0, cal;
    for (; q < 3000 && !(cal = proc.port.posted.find((m) => m.calibrated)); ++q) {
        proc.process([[ins.subarray(q * Q, (q + 1) * Q)]], [[oL, oR]], {});
        for (let i = 0; i < Q; ++i) loudBefore = Math.max(loudBefore, Math.abs(oL[i]), Math.abs(oR[i]));
        answer(proc, w0, pack.dir);
    }
    return { cal: cal && cal.calibrated, calls, loudBefore, quanta: q, proc };
}
{
    const fast = await calibrated(realPack, 0);
    const c = fast.cal;
    console.log(`      ${realPack.name}: one render ${c ? c.renderUs.toFixed(1) : '?'} us = ${c ? (100 * c.share).toFixed(2) : '?'} % `
                + `of the ${c ? c.quantumUs.toFixed(0) : '?'} us quantum (${c ? c.renders : '?'} renders in ${c ? c.elapsedMs : '?'} ms; `
                + `decided after ${fast.quanta} quanta)`);
    ok(c && c.valid && c.runs === c.renders && c.pairs === 0, 'the burst ran exactly one network per render');
    ok(c && c.maxModels === 2 && c.share <= 0.25, 'a fast render: the blend is turned on (maxModels 2)');
    ok(JSON.stringify(fast.calls.max) === '[1,2]', `the module was told 1 at the start and 2 after (${fast.calls.max})`);
    ok(fast.calls.gain[0] === 150 && JSON.stringify(fast.calls.gain.slice(1)) === '[200,210]',
       `the dial at 150 for the burst, then the queued knobs in order (${fast.calls.gain})`);
    ok(fast.loudBefore === 0, 'the start is silent until the decision');
    fast.proc.port.onmessage({ data: { dispose: true } });

    const slow = await calibrated(packs[0], 1.0);   // every render made a millisecond longer: 37 % of the quantum
    const s = slow.cal;
    console.log(`      injected slow render: ${s ? s.renderUs.toFixed(0) : '?'} us = ${s ? (100 * s.share).toFixed(1) : '?'} %`);
    ok(s && s.maxModels === 1 && s.share > 0.25, 'a slow render: it stays at one model');
    ok(JSON.stringify(slow.calls.max) === '[1,1]', `the module was never told 2 (${slow.calls.max})`);
    slow.proc.port.onmessage({ data: { dispose: true } });
}

// ---- the load protocol across the port, and the start's edges ------------------------------------------------------
const RIFF = makeInput(Q * 4096, 48000, 0);
let riffAt = 0;
/** `n` quanta of the riff through `proc`, `each(q)` after every one (the page's half); the peak of what came out. */
function play(proc, n, each) {
    const oL = new Float32Array(Q), oR = new Float32Array(Q);
    let peak = 0;
    for (let q = 0; q < n; ++q) {
        riffAt = (riffAt + Q) % RIFF.length;
        proc.process([[RIFF.subarray(riffAt, riffAt + Q)]], [[oL, oR]], {});
        for (let i = 0; i < Q; ++i) peak = Math.max(peak, Math.abs(oL[i]), Math.abs(oR[i]));
        if (each) each(q);
    }
    return peak;
}
const after = (proc, mark, pred) => proc.port.posted.slice(mark).filter(pred);
const realBytes = (dir, id) => w0.buffer(fs.readFileSync(path.join(dir, id)));
/** Hooks on the next module the worklet instantiates (the spy in the glue's wrapper). */
const hooked = (fn) => { w0.ctx.__onRigModule = (m) => { w0.ctx.__onRigModule = undefined; fn(m); }; };

group('the load protocol across the port: late, stale, unanswered and failed answers');
{
    // A first capture that fails to load ends the start (one model, not valid) instead of waiting for ever.
    const proc = await newProcessor(w0, fixtureDir);                       // calibrate: on (the default)
    let first = true, sent = false;
    const failFirst = (id) => (first ? (first = false, null) : undefined);
    const peak = play(proc, 400, () => {
        answer(proc, w0, fixtureDir, failFirst);
        if (!first && !sent) { sent = true; proc.port.onmessage({ data: { set: { gain: 240 } } }); }
    });
    const cal = proc.port.posted.filter((m) => m.calibrated).map((m) => m.calibrated);
    ok(cal.length === 1 && cal[0].valid === false && cal[0].maxModels === 1 && cal[0].reason === 'load-failed',
       `the first capture failed: the start ends, one model, not valid (${JSON.stringify(cal)})`);
    ok(peak > 0.01, `…and the knob sent meanwhile lands: another capture loads and plays (peak ${peak.toFixed(3)})`);
    proc.port.onmessage({ data: { dispose: true } });
}
{
    // An answer of null is "could not fetch it", not the file: a later visit asks again (RigPlayer::deliver).
    const proc = await newProcessor(w0, fixtureDir, { calibrate: false, maxModels: 1 });
    let nulls = 1;
    const once = (id) => (id === 'g150.namz' && nulls > 0 ? (--nulls, null) : undefined);
    play(proc, 60, () => answer(proc, w0, fixtureDir, once));
    proc.port.onmessage({ data: { set: { gain: 240 } } });
    play(proc, 60, () => answer(proc, w0, fixtureDir, once));
    const mark = proc.port.posted.length;
    proc.port.onmessage({ data: { set: { gain: 150 } } });
    const peak = play(proc, 120, () => answer(proc, w0, fixtureDir, once));
    const needs = after(proc, mark, (m) => m.need === 'g150.namz').length;
    const landed = after(proc, mark, (m) => m.loaded === 'g150.namz').map((m) => m.ok);
    ok(needs === 1 && JSON.stringify(landed) === '[true]',
       `a capture answered null is asked for again on the next visit, and lands (${needs} need, loaded ${landed})`);
    ok(peak > 0.01, `…and plays (peak ${peak.toFixed(3)})`);

    // { file: null } with no job out is nobody's answer: nothing is run, nothing posted.
    const quiet = proc.port.posted.length;
    proc.port.onmessage({ data: { file: null, pack: proc.pack, bytes: null } });
    ok(proc.awaiting === null && after(proc, quiet, (m) => m.loaded !== undefined).length === 0,
       'an answer with no { need } out posts no { loaded }');
    proc.port.onmessage({ data: { dispose: true } });
}
{
    // An answer several quanta late lands; nothing else is asked meanwhile (one job out).
    const proc = await newProcessor(w0, fixtureDir, { calibrate: false, maxModels: 1 });
    play(proc, 1);
    const need = proc.port.posted.find((m) => m.need !== undefined);
    const silent = play(proc, 40);
    ok(need && proc.port.posted.filter((m) => m.need !== undefined).length === 1 && silent === 0,
       'a { need } out: no second one, and silence, while the page has not answered');
    const mark = proc.port.posted.length;
    proc.port.onmessage({ data: { file: need.need, pack: need.pack, bytes: realBytes(fixtureDir, need.need) } });
    const peak = play(proc, 60, () => answer(proc, w0, fixtureDir));
    ok(after(proc, mark, (m) => m.loaded === need.need && m.ok).length === 1 && peak > 0.01,
       `…the answer 40 quanta late lands and plays (peak ${peak.toFixed(3)})`);
    proc.port.onmessage({ data: { dispose: true } });
}
{
    // A stale answer — for the pack before an { unload } and a { load } — is dropped, even for a file of the same name.
    const proc = await newProcessor(w0, fixtureDir, { calibrate: false, maxModels: 1 });
    play(proc, 1);
    const old = proc.port.posted.find((m) => m.need !== undefined);
    const rig = w0.buffer(fs.readFileSync(path.join(fixtureDir, 'rig.json')));
    proc.port.onmessage({ data: { unload: true } });
    proc.port.onmessage({ data: { load: rig } });
    let mark = proc.port.posted.length;
    play(proc, 1);
    const fresh = after(proc, mark, (m) => m.need !== undefined)[0];
    ok(old && fresh && fresh.need === old.need && fresh.pack > old.pack,
       `the new pack asks for the same file (${old && old.need}, pack ${old && old.pack} -> ${fresh && fresh.pack})`);
    mark = proc.port.posted.length;
    proc.port.onmessage({ data: { file: old.need, pack: old.pack, bytes: w0.buffer(Buffer.from('{ stale garbage')) } });
    ok(after(proc, mark, (m) => m.loaded !== undefined).length === 0 && proc.awaiting === fresh.need,
       'the old pack\'s answer is dropped: nothing loaded, the new { need } still out');
    proc.port.onmessage({ data: { file: fresh.need, pack: fresh.pack, bytes: realBytes(fixtureDir, fresh.need) } });
    ok(after(proc, mark, (m) => m.loaded === fresh.need && m.ok).length === 1, '…and the new pack\'s answer lands');
    proc.port.onmessage({ data: { dispose: true } });
}
{
    // A page that never answers (its handler threw: a body cut off after the headers rejects arrayBuffer()) does not
    // stall the loads for ever: the processor answers itself after needTimeoutMs, and the next job flows.
    const proc = await newProcessor(w0, fixtureDir, { calibrate: false, maxModels: 1, needTimeoutMs: 100 });
    const brokenPage = () => {
        while ((proc.seen ??= 0) < proc.port.posted.length) {
            const msg = proc.port.posted[proc.seen++];
            if (msg.need !== undefined) Promise.reject(new Error('body cut off')).catch(() => {});   // posts nothing
        }
    };
    play(proc, 60, brokenPage);                                            // 100 ms = 38 quanta
    const timed = proc.port.posted.filter((m) => m.loaded !== undefined);
    ok(timed.length === 1 && timed[0].ok === false && proc.awaiting === null,
       `an unanswered { need }: { loaded, ok: false } after the timeout (${JSON.stringify(timed)})`);
    const mark = proc.port.posted.length;
    proc.port.onmessage({ data: { file: timed[0] && timed[0].loaded, pack: proc.pack, bytes: realBytes(fixtureDir, 'g150.namz') } });
    ok(after(proc, mark, (m) => m.loaded !== undefined).length === 0, '…its late answer is dropped');
    proc.answered = proc.port.posted.length;
    proc.port.onmessage({ data: { set: { gain: 240 } } });
    const peak = play(proc, 60, () => answer(proc, w0, fixtureDir));
    ok(after(proc, mark, (m) => m.need === 'g240.nam').length === 1 && after(proc, mark, (m) => m.loaded === 'g240.nam' && m.ok).length === 1
       && peak > 0.01, `…and the next job flows: asked, landed, playing (peak ${peak.toFixed(3)})`);
    proc.port.onmessage({ data: { dispose: true } });
}

group('the start decides again at every { load }, and only on valid evidence');
{
    // A pack loaded after the start is timed again: its captures may cost another share of the quantum.
    let slowMs = 0;
    hooked((m) => {
        const process = m._fc_rig_process;
        m._fc_rig_process = (...a) => { if (slowMs > 0) { const t = Date.now(); while (Date.now() - t < slowMs) { /* slow */ } } return process(...a); };
    });
    const proc = await newProcessor(w0, fixtureDir);
    play(proc, 400, () => answer(proc, w0, fixtureDir));
    const firstCal = proc.port.posted.filter((m) => m.calibrated);
    slowMs = 1.0;
    const mark = proc.port.posted.length;
    proc.port.onmessage({ data: { load: w0.buffer(fs.readFileSync(path.join(fixtureDir, 'rig.json'))) } });
    proc.port.onmessage({ data: { set: { gain: 240 } } });                // queued until the new decision
    let loudBefore = 0;
    for (let q = 0; q < 400 && after(proc, mark, (m) => m.calibrated).length === 0; ++q)
        loudBefore = Math.max(loudBefore, play(proc, 1, () => answer(proc, w0, fixtureDir)));
    slowMs = 0;
    const again = after(proc, mark, (m) => m.calibrated).map((m) => m.calibrated);
    ok(firstCal.length === 1 && firstCal[0].calibrated.maxModels === 2, 'the first pack: fast, the blend on');
    ok(again.length === 1 && again[0].maxModels === 1 && again[0].share > 0.25,
       `the { load } re-arms the start: timed again, slow now, one model (${JSON.stringify(again.map((c) => c.maxModels))})`);
    ok(loudBefore === 0, '…silent until it decided');
    proc.port.onmessage({ data: { dispose: true } });
}
for (const [what, install] of [
    ['a clock that stops (Date.now frozen during the burst)', (m) => {
        const D = vm.runInContext('Date', w0.ctx), real = D.now;
        let calls = 0;
        D.now = () => (++calls === 1 ? 1000 : 1001);   // moves once, for the start's tick, then never
        return () => { D.now = real; };
    }],
    ['counters polluted inside the burst (a warm block every time)', (m) => {
        let since = 0;
        const clear = m._fc_rig_clear_counters, process = m._fc_rig_process, warm = m._fc_rig_warm_blocks;
        m._fc_rig_clear_counters = (r) => { since = 0; return clear(r); };
        m._fc_rig_process = (...a) => { ++since; return process(...a); };
        m._fc_rig_warm_blocks = (r) => (since > 1 ? 1 : warm(r));
        return () => {};
    }],
]) {
    let restore = () => {}, armed = false, mod = null;
    hooked((m) => { mod = m; });
    const proc = await newProcessor(w0, fixtureDir);
    let cal;
    for (let q = 0; q < 600 && !(cal = proc.port.posted.find((m) => m.calibrated)); ++q) {
        if (!armed && proc.calState === 'steady') { armed = true; restore = install(mod); }
        play(proc, 1, () => answer(proc, w0, fixtureDir));
    }
    restore();
    const c = cal && cal.calibrated;
    ok(armed && c && c.valid === false && c.maxModels === 1,
       `${what}: the evidence is not valid, so one model (${c ? `valid ${c.valid}, maxModels ${c.maxModels}, `
       + `${c.renders} renders in ${c.elapsedMs} ms, warm ${c.warm}` : 'no decision'})`);
    proc.port.onmessage({ data: { dispose: true } });
}

group('a render that grows the memory, and a knob value that is not a number');
{
    // An LSTM or ConvNet capture allocates while it renders; if that grows the memory, the views made before the
    // render are detached when the processor copies out. Same output as a processor whose memory never grew.
    const run = async (grow) => {
        if (grow) hooked((m) => {
            const process = m._fc_rig_process;
            let calls = 0;
            m._fc_rig_process = (...a) => { if (++calls === 200) m._fc_rig_free(m._fc_rig_alloc(64 << 20)); return process(...a); };
        });
        const proc = await newProcessor(w0, fixtureDir, { calibrate: false, maxModels: 2 });
        const out = new Float32Array(Q * 300), oL = new Float32Array(Q), oR = new Float32Array(Q);
        const memory0 = proc.m.HEAPU8.buffer.byteLength;
        for (let q = 0; q < 300; ++q) {
            proc.process([[RIFF.subarray(q * Q, (q + 1) * Q)]], [[oL, oR]], {});
            out.set(oL, q * Q);
            answer(proc, w0, fixtureDir);
        }
        const grew = proc.m ? proc.m.HEAPU8.buffer.byteLength - memory0 : -1;
        const errors = proc.port.posted.filter((m) => m.error).map((m) => m.error);
        proc.port.onmessage({ data: { dispose: true } });
        return { out, grew, errors };
    };
    const plain = await run(false), grown = await run(true);
    const same = plain.out.every((v, i) => Object.is(v, grown.out[i]));
    ok(grown.grew > (32 << 20) && grown.errors.length === 0 && same,
       `the memory grew inside a render (+${grown.grew >> 20} MiB): no error, the output bit for bit (${grown.errors})`);
}
{
    // BigInt survives postMessage and throws on unary +: a knob that is not a number is ignored, not a trap.
    const proc = await newProcessor(w0, fixtureDir, { calibrate: false });
    play(proc, 100, () => answer(proc, w0, fixtureDir));
    proc.port.onmessage({ data: { set: { gain: 210n, dial: { Treble: 10n }, blendShape: [1n, 2n] } } });
    const peak = play(proc, 100, () => answer(proc, w0, fixtureDir));
    ok(!proc.port.posted.some((m) => m.error) && proc.m !== null && peak > 0.01,
       `{ set: { gain: 210n } } is ignored: no error, the module kept, playing (peak ${peak.toFixed(3)})`);
    proc.port.onmessage({ data: { dispose: true } });
}

/** `n` quanta of the fixed input through `proc`: every one must answer true and come out as it went in. */
function passesDry(proc, n, size = Q) {
    const inL = makeInput(n * size, 48000, 0);
    const oL = new Float32Array(size), oR = new Float32Array(size);
    let alive = true, exact = true;
    for (let q = 0; q < n; ++q) {
        const iL = inL.subarray(q * size, (q + 1) * size);
        alive = proc.process([[iL]], [[oL, oR]], {}) === true && alive;
        for (let i = 0; i < size; ++i) exact = exact && Object.is(oL[i], iL[i]) && Object.is(oR[i], iL[i]);
    }
    return alive && exact;
}

group('an error is not an end: dry, alive, one { error }; only dispose ends it');
{
    const destroyed = [];
    const spyDestroy = (m) => { const d = m._fc_rig_destroy; m._fc_rig_destroy = (r) => { destroyed.push(r); return d(r); }; };
    const ends = (proc, what) => {
        proc.port.onmessage?.({ data: { dispose: true } });
        const o = [new Float32Array(Q), new Float32Array(Q)];
        ok(proc.process([[new Float32Array(Q)]], [o], {}) === false && proc.port.closed, `${what}: dispose ends it`);
    };

    const none = new w0.cls({ processorOptions: { module: null } });
    const asked = await settle(none, (m) => m.error);
    ok(asked && asked.retry === 'wasmBytes' && passesDry(none, 100) && !none.port.closed,
       'no module: { error, retry: wasmBytes }, then dry, bit for bit, alive');
    ends(none, 'no module');

    const viaBytes = await newProcessor(w0, fixtureDir, { calibrate: false }, { module: null, wasmBytes: w0.wasmBytes });
    ok(viaBytes.m !== null && !viaBytes.port.posted.some((m) => m.error), 'the bytes path comes up');
    ends(viaBytes, 'bytes path');

    const wLow = await loadWorklet(500);
    wLow.ctx.__onRigModule = spyDestroy;
    const low = new wLow.cls({ processorOptions: { module: wLow.module } });
    const said = await settle(low, (m) => m.error);
    ok(said && destroyed.length === 1 && passesDry(low, 100) && low.port.posted.filter((m) => m.error).length === 1,
       `a refused prepare: one error, the instance freed, then dry (${destroyed.length} freed)`);
    ends(low, 'refused prepare');

    destroyed.length = 0;
    w0.ctx.__onRigModule = spyDestroy;
    const odd = await newProcessor(w0, fixtureDir, { calibrate: false });
    ok(passesDry(odd, 30, 256) && destroyed.length === 1 && odd.port.posted.filter((m) => m.error).length === 1,
       'a 256-frame quantum: one error, freed, dry from that quantum on');
    ends(odd, 'foreign quantum');

    destroyed.length = 0;
    let trapAt = -1;
    w0.ctx.__onRigModule = (m) => {
        spyDestroy(m);
        const p = m._fc_rig_process;
        m._fc_rig_process = (...a) => { if (--trapAt === 0) throw new (vm.runInContext('WebAssembly', w0.ctx).RuntimeError)('unreachable'); return p(...a); };
    };
    const trapped = await newProcessor(w0, fixtureDir, { calibrate: false });
    trapAt = 50;
    for (let q = 0; q < 60; ++q) trapped.process([[new Float32Array(Q)]], [[new Float32Array(Q), new Float32Array(Q)]], {});
    ok(trapped.port.posted.filter((m) => m.error).length === 1 && destroyed.length === 0 && passesDry(trapped, 100),
       `a trap: one error, the module NOT called again (${destroyed.length} destroy calls), then dry, alive`);
    ends(trapped, 'trap');
    w0.ctx.__onRigModule = undefined;
}

// NO ALLOCATION, AS A BOUND PER QUANTUM. heapUsed moves by itself in every window, whatever its length — 12 … 37 KB
// in node 26 on arm64 macOS, V8's own (feedback, tiering, the observer) — so the difference between a 5 000-quantum
// and a 20 000-quantum window read −8 … +16 KB over a dozen runs: at most ~1.1 byte per quantum, and no garbage
// collection. The smallest object JavaScript can put on V8's heap is a 16-byte HeapNumber (12 with pointer
// compression, as in Chrome); one per quantum is 180 … 240 KB over the 15 000. The bound, 4 bytes per quantum (60 KB):
// 3.7 times the most the noise read, a third of that object. Its control, injected into process() in a scratch copy
// of the build and not kept: one boxed number per quantum reads 16.5 … 17.1 bytes per quantum and fails, an empty
// object 57.1; the processor as shipped reads 0.07 … 1.04.
const ALLOC_BYTES_PER_QUANTUM = 4;
group(`no allocation per render quantum: under ${ALLOC_BYTES_PER_QUANTUM} bytes of JS heap per quantum, no GC`);
{
    const proc = await newProcessor(w0, realPack.dir, { calibrate: false, maxModels: 2, knobs: { gain: 165 } });
    const iL = makeInput(Q, 48000, 0), oL = new Float32Array(Q), oR = new Float32Array(Q);
    const inputs = [[iL]], outputs = [[oL, oR]], params = {};
    const run = (k) => { for (let i = 0; i < k; ++i) { proc.process(inputs, outputs, params); if (proc.awaiting !== null) answer(proc, w0, realPack.dir); } };
    run(4000);
    run(2000);
    const gcs = [];
    const obs = new PerformanceObserver((list) => { for (const e of list.getEntries()) gcs.push(e.startTime); });
    obs.observe({ entryTypes: ['gc'] });
    const windowed = async (n) => {
        globalThis.gc();
        await new Promise((r) => setImmediate(r));
        const before = process.memoryUsage().heapUsed, t0 = performance.now();
        run(n);
        return { grew: process.memoryUsage().heapUsed - before, t0, t1: performance.now() };
    };
    const short = await windowed(5000), long = await windowed(20000);
    await new Promise((r) => setTimeout(r, 50));
    obs.disconnect();
    const inWindows = gcs.filter((t) => (t >= short.t0 && t <= short.t1) || (t >= long.t0 && t <= long.t1)).length;
    const extra = long.grew - short.grew, perQuantum = extra / 15000;
    console.log(`      the JS heap moved ${short.grew} bytes over 5 000 quanta and ${long.grew} over 20 000 (${extra} for `
                + `the 15 000 more: ${perQuantum.toFixed(2)} bytes per quantum), ${inWindows} garbage collections in `
                + `either window`);
    ok(inWindows === 0 && perQuantum < ALLOC_BYTES_PER_QUANTUM,
       `process() allocates: ${perQuantum.toFixed(2)} bytes per quantum (${extra} for 15 000 more quanta; the bound `
       + `${ALLOC_BYTES_PER_QUANTUM}), ${inWindows} GCs`);
    proc.port.onmessage({ data: { dispose: true } });
}

group(`cost of one 128-frame mono quantum at 48 kHz in node, ${realPack.name} (reported, not gated)`);
for (const [what, max, gain] of [['one capture', 1, 0.5], ['two captures', 2, 0.55]]) {
    const sweep = (JSON.parse(fs.readFileSync(path.join(realPack.dir, 'rig.json'), 'utf8')).chain[0].controls || [])
        .find((c) => c.sweep)?.sweep || 300;
    const proc = await newProcessor(w0, realPack.dir, { calibrate: false, maxModels: max, knobs: { gain: sweep * gain } });
    const iL = makeInput(Q, 48000, 0), oL = new Float32Array(Q), oR = new Float32Array(Q);
    const inputs = [[iL]], outputs = [[oL, oR]], params = {};
    for (let i = 0; i < 3000; ++i) { proc.process(inputs, outputs, params); answer(proc, w0, realPack.dir); }
    proc.m._fc_rig_clear_counters(proc.rig);
    const n = 5000, t0 = performance.now();
    for (let i = 0; i < n; ++i) proc.process(inputs, outputs, params);
    const us = (performance.now() - t0) * 1000 / n;
    const runs = proc.m._fc_rig_run_blocks(proc.rig, 0) + proc.m._fc_rig_run_blocks(proc.rig, 1), pairs = proc.m._fc_rig_pair_blocks(proc.rig);
    console.log(`      ${what}: ${us.toFixed(1)} us per quantum = ${(100 * us / (1e6 * Q / 48000)).toFixed(2)} % `
                + `(networks ran ${runs} block-runs in ${n} quanta, both ${pairs})`);
    proc.port.onmessage({ data: { dispose: true } });
}

fs.rmSync(fixtureDir, { recursive: true, force: true });
console.log(`\n${checks} checks, ${failures} failures\n${failures ? 'TESTS FAILED' : 'ALL TESTS PASSED'}`);
process.exit(failures ? 1 : 0);
