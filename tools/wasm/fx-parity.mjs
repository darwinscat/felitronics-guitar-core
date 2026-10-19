// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// fx-parity — the built fcguitarfx module, run the way a page runs it, against the native render of the same ABI.
//
//   node --experimental-vm-modules --expose-gc tools/wasm/fx-parity.mjs <build-dir> <fc_guitar_fx_render> <abi-header>
//
// <build-dir> holds what build.sh ships: fcguitarfx.mjs, fcguitarfx.wasm, fcguitarfx-worklet.js. What it proves:
//   1. THE WORKLET LOADS WHERE IT WILL RUN. The processor script and the glue it imports are evaluated as ES modules
//      in a context shaped like an AudioWorkletGlobalScope — AudioWorkletProcessor, registerProcessor, sampleRate,
//      console, WebAssembly, and NO fetch, URL, TextDecoder, atob, setTimeout or importScripts — and the module
//      arrives as a WebAssembly.Module in processorOptions, as the page posts it.
//   2. THE EXPORTS ARE THE ABI: the module's fc_guitar_fx_* entry points are exactly the header's.
//   3. THE RENDER IS THE NATIVE ONE within law 10's tolerance: a fixed input through a few control settings (knobs
//      before prepare, knobs moved mid-stream by port messages), every 128-frame quantum through the processor's own
//      process(), against fc_guitar_fx_render driven with the same knobs at the same frames.
//   4. NO ALLOCATION PER QUANTUM: sixty thousand quanta through process() more than a shorter window start no
//      garbage collection and move the JS heap by less than 16 KB more, where one 16-byte object per quantum would
//      be 960 KB.
//   5. THE COST of one 128-frame stereo quantum at 48 kHz under the heaviest settings — reported, not gated.
//   And around those: the glue may not name toResizableBuffer, and every processor runs with it deleted (the browser
//   floor); a non-finite sample fails a comparison for good; the integer knobs' NaN and out-of-range values; the
//   bytes path against the module path; an error — dry from then on, bit for bit, and alive — and dispose, the only end.

import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { spawnSync } from 'node:child_process';
import { performance, PerformanceObserver } from 'node:perf_hooks';

const [dir, renderBin, header] = process.argv.slice(2);
if (!dir || !renderBin || !header) {
    console.error('usage: fx-parity.mjs <build-dir> <fc_guitar_fx_render> <fc_guitar_fx_abi.h>');
    process.exit(2);
}
if (typeof vm.SourceTextModule !== 'function') {
    console.error('fx-parity: needs node --experimental-vm-modules');
    process.exit(2);
}

// LAW 10'S TOLERANCE. The two rows compile the same source under the same `-ffp-contract=on`, and still need not
// agree to the bit: wasm has no FMA instruction, so nothing fuses there, while a native arm64 row fuses `a*b + c`
// inside one expression (an x86-64 row without FMA does not). The transcendentals are core::det on both, so libm is
// not a difference. Measured on arm64 macOS: worst 1.2e-7 over these four renders — and 0 samples of 1 312 768 differ
// against a native build with contraction off, so the whole difference is the fusing. 2e-6 is about 17 float ulps at
// full scale: room for another row's fusing, no room for a wrong knob or a wrong order.
const TOLERANCE = 2e-6;

let failures = 0, checks = 0;
const ok = (cond, msg) => { ++checks; if (!cond) { ++failures; console.log('    FAIL: ' + msg); } };
const group = (name) => console.log('  - ' + name);

const Q = 128;   // the render quantum

// ---- the simulated AudioWorkletGlobalScope -------------------------------------------------------------------------
class AudioWorkletProcessor {
    constructor() {
        this.port = { onmessage: null, posted: [], closed: false,
                      postMessage(m) { this.posted.push(m); }, close() { this.closed = true; } };
    }
}

const GLUE = fs.readFileSync(path.join(dir, 'fcguitarfx.mjs'), 'utf8');

// The worklet's import of ./fcguitarfx.mjs is linked to a thin wrapper around the real glue, so a test can see the
// module object a processor receives (and spy on it) before the processor starts using it.
const WRAPPER = "import create from './glue.mjs';\n" +
    'export default async function (arg) { const m = await create(arg); globalThis.__onFxModule?.(m); return m; }\n';

/** A context shaped like an AudioWorkletGlobalScope. `renderQuantumSize` is put in scope only when given. And, as on
    every engine older than Chrome 144 / Firefox 145 / Safari 26.2, WebAssembly.Memory has no toResizableBuffer. */
async function loadWorklet(sampleRate, { renderQuantumSize } = {}) {
    let registered = null;
    const ctx = vm.createContext({
        console, AudioWorkletProcessor, sampleRate, currentFrame: 0, currentTime: 0,
        registerProcessor: (name, cls) => { registered = { name, cls }; },
        ...(renderQuantumSize ? { renderQuantumSize } : {}),
    });
    vm.runInContext('globalThis.AudioWorkletGlobalScope = function AudioWorkletGlobalScope() {};' +
                    'delete WebAssembly.Memory.prototype.toResizableBuffer;', ctx);
    const absent = vm.runInContext(
        "['fetch', 'URL', 'TextDecoder', 'atob', 'setTimeout', 'importScripts', 'window', 'self']" +
        '.filter((n) => typeof globalThis[n] !== "undefined")' +
        ".concat(typeof WebAssembly.Memory.prototype.toResizableBuffer === 'undefined' ? [] : ['toResizableBuffer'])", ctx);
    if (absent.length) throw new Error('the simulated worklet scope leaks ' + absent.join(', '));

    const glue = new vm.SourceTextModule(GLUE, {
        context: ctx, identifier: 'fcguitarfx.mjs',
        initializeImportMeta(meta) { meta.url = 'https://worklet.invalid/fcguitarfx.mjs'; },
    });
    const wrapper = new vm.SourceTextModule(WRAPPER, { context: ctx, identifier: 'fcguitarfx.mjs (spy)' });
    const worklet = new vm.SourceTextModule(fs.readFileSync(path.join(dir, 'fcguitarfx-worklet.js'), 'utf8'), {
        context: ctx, identifier: 'fcguitarfx-worklet.js',
        initializeImportMeta(meta) { meta.url = 'https://worklet.invalid/fcguitarfx-worklet.js'; },
    });
    await worklet.link((spec, referrer) => {
        if (spec === './fcguitarfx.mjs' && referrer === worklet) return wrapper;
        if (spec === './glue.mjs' && referrer === wrapper) return glue;
        throw new Error('the worklet imports ' + spec);
    });
    await worklet.evaluate();
    if (!registered || registered.name !== 'fc-guitar-fx') throw new Error('no fc-guitar-fx processor registered');

    // The page compiles; the structured clone lands a Module of the WORKLET's realm, so compile in that realm.
    const CtxWasm = vm.runInContext('WebAssembly', ctx);
    const bytes = fs.readFileSync(path.join(dir, 'fcguitarfx.wasm'));
    const module = await CtxWasm.compile(bytes);
    const wasmBytes = vm.runInContext('(n) => new ArrayBuffer(n)', ctx)(bytes.length);
    new Uint8Array(wasmBytes).set(bytes);
    return { cls: registered.cls, module, wasmBytes, ctx };
}

async function settle(proc) {
    for (let i = 0; i < 1000 && proc.port.posted.length === 0; ++i)
        await new Promise((r) => setImmediate(r));
    return proc.port.posted[0];
}

async function newProcessor(w, knobs, options = { module: w.module }) {
    const proc = new w.cls({ numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
                             processorOptions: { ...options, knobs } });
    const first = await settle(proc);
    if (!first || !first.ready) throw new Error('the processor did not come up: ' + JSON.stringify(first));
    return proc;
}

/** Every sample of `a` against `b`. A non-finite sample on EITHER side fails the comparison for good: it is counted
    apart, and never folded into `worst`, where a later finite difference could overwrite it. */
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

// ---- a fixed input: plucks, stereo-different, a noise burst, tapered ---------------------------------------------
function makeInput(frames, sr, ch) {
    const x = new Float32Array(frames);
    let seed = (0x2545f491 + ch) >>> 0;
    for (let i = 0; i < frames; ++i) {
        const t = i / sr, pluck = t % 0.6;
        const env = Math.exp(-pluck * 7) * (pluck < 0.002 ? pluck / 0.002 : 1);
        const f = ch === 0 ? 196 : 246.94;
        let v = 0.45 * env * (Math.sin(2 * Math.PI * f * t) + 0.3 * Math.sin(2 * Math.PI * 3 * f * t));
        seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
        if (t > 1 && t < 1.05) v += 0.2 * ((seed >>> 8) / 16777216 - 0.5);
        x[i] = v;
    }
    const fade = Math.floor(0.01 * sr);
    for (let i = 0; i < fade && i < frames; ++i) x[frames - 1 - i] *= i / fade;
    return x;
}

function renderNative(sr, frames, knobs, moves, inL, inR) {
    const args = [String(sr), String(Q), String(frames), String(Q)];
    for (const [k, v] of Object.entries(knobs)) args.push(`${k}=${+v}`);
    for (const m of moves) for (const [k, v] of Object.entries(m.set)) args.push(`@${m.frame}:${k}=${+v}`);
    const input = Buffer.alloc(frames * 8);
    Buffer.from(inL.buffer).copy(input, 0);
    Buffer.from(inR.buffer).copy(input, frames * 4);
    const r = spawnSync(renderBin, args, { input, maxBuffer: frames * 8 + 1024 });
    if (r.status !== 0) throw new Error(`fc_guitar_fx_render exited ${r.status}: ${r.stderr}`);
    const out = new Float32Array(r.stdout.buffer.slice(r.stdout.byteOffset, r.stdout.byteOffset + frames * 8));
    return [out.subarray(0, frames), out.subarray(frames)];
}

async function renderWorklet(w, frames, knobs, moves, inL, inR) {
    const proc = await newProcessor(w, knobs);
    const iL = new Float32Array(Q), iR = new Float32Array(Q), oL = new Float32Array(Q), oR = new Float32Array(Q);
    const outL = new Float32Array(frames), outR = new Float32Array(frames);
    const inputs = [[iL, iR]], outputs = [[oL, oR]];
    let next = 0;
    for (let at = 0; at < frames; at += Q) {
        while (next < moves.length && moves[next].frame <= at) proc.port.onmessage({ data: { set: moves[next++].set } });
        iL.set(inL.subarray(at, at + Q));
        iR.set(inR.subarray(at, at + Q));
        proc.process(inputs, outputs, {});
        outL.set(oL, at);
        outR.set(oR, at);
    }
    return { out: [outL, outR], proc };
}

// ---- the scenarios ------------------------------------------------------------------------------------------------
const BUSY = { delay_sync: 1, delay_division: 7, delay_bpm: 132, delay_repeats: 62, delay_dark: 30,
               delay_offset_ms: 12.5, delay_mix: 45, reverb_mix: 55, reverb_decay: 1.6, reverb_predelay_ms: 25,
               reverb_hpf_hz: 210 };
const SCENARIOS = [
    { name: 'defaults (Plate 30, delay on at mix 0) 48k', sr: 48000, seconds: 3, knobs: {}, moves: [] },
    { name: 'busy echo + Hall 48k', sr: 48000, seconds: 3, knobs: { ...BUSY, reverb_character: 2 }, moves: [] },
    { name: 'free 900 ms + Modulated, moved mid-stream 44.1k', sr: 44100, seconds: 4,
      knobs: { delay_sync: 0, delay_time_ms: 900, delay_repeats: 80, delay_mix: 60, reverb_character: 5, reverb_mix: 40 },
      moves: [
          { frame: 128 * 200, set: { delay_time_ms: 210 } },                          // the head glides: a bend
          { frame: 128 * 400, set: { reverb_character: 4, delay_on: 0 } },             // Spring; the echo stands by
          { frame: 128 * 700, set: { delay_on: 1, delay_sync: 1, delay_division: 3 } },
          { frame: 128 * 900, set: { reverb_on: 0, delay_dark: 85, delay_offset_ms: -30 } },
          { frame: 128 * 1200, set: { reverb_on: 1, reverb_decay: 0.5, reverb_hpf_hz: 40 } },
      ] },
    { name: 'Room + Ambience at 96k, ragged knob values', sr: 96000, seconds: 2,
      knobs: { reverb_character: 1, reverb_mix: 70, delay_mix: 30, delay_division: 11, delay_bpm: 97.3 },
      moves: [ { frame: 128 * 600, set: { reverb_character: 0, delay_mix: 1e9, reverb_predelay_ms: -4 } } ] },
];

const w0 = await loadWorklet(48000);

group('the worklet loads in an AudioWorkletGlobalScope, and its exports are the ABI');
{
    const proc = await newProcessor(w0, {});
    const want = [...fs.readFileSync(header, 'utf8').matchAll(/^[A-Za-z_][A-Za-z0-9_ *]* (fc_guitar_fx_[a-z_]+) \(/gm)]
        .map((m) => '_' + m[1]).sort();
    const got = Object.keys(proc.m).filter((k) => k.startsWith('_fc_guitar_fx_')).sort();
    ok(want.length === 22, `the header declares 22 entry points (read ${want.length})`);
    ok(JSON.stringify(got) === JSON.stringify(want), `the module exports exactly the header's entry points\n      got  ${got}\n      want ${want}`);
    ok(proc.m._fc_guitar_fx_abi_version() === 1, 'abi_version answers 1');
}

group('the glue runs without growable ArrayBuffers (the browser floor)');
ok(!/toResizableBuffer/.test(GLUE),
   'the glue never calls WebAssembly.Memory.toResizableBuffer — Chrome 144, Firefox 145 and Safari 26.2 have it, nothing older');
// ...and every processor in this file runs with the method deleted (loadWorklet), as on those older engines.

/** One quantum through `proc` with silence or `inL/inR`; answers what process() returned. */
function quantum(proc, n = Q) {
    const i = [new Float32Array(n), new Float32Array(n)], o = [new Float32Array(n), new Float32Array(n)];
    return proc.process([i], [o], {});
}

/** `n` quanta of the fixed input through `proc` (quanta of `size` frames): every one must answer true and come out
    as it went in, bit for bit — the dry a failed processor owes the chain it stands in. */
function passesDry(proc, n, size = Q) {
    const inL = makeInput(n * size, 48000, 0), inR = makeInput(n * size, 48000, 1);
    const oL = new Float32Array(size), oR = new Float32Array(size);
    let alive = true, exact = true;
    for (let q = 0; q < n; ++q) {
        const iL = inL.subarray(q * size, (q + 1) * size), iR = inR.subarray(q * size, (q + 1) * size);
        alive = proc.process([[iL, iR]], [[oL, oR]], {}) === true && alive;
        for (let i = 0; i < size; ++i)
            exact = exact && Object.is(oL[i], iL[i]) && Object.is(oR[i], iR[i]);
    }
    return alive && exact;
}

/** `frames` of the fixed input through a processor made with `options`, the knob moves posted at their frames. */
async function renderWith(w, frames, knobs, moves, options) {
    const proc = await newProcessor(w, knobs, options);
    const inL = makeInput(frames, 48000, 0), inR = makeInput(frames, 48000, 1);
    const iL = new Float32Array(Q), iR = new Float32Array(Q), oL = new Float32Array(Q), oR = new Float32Array(Q);
    const out = [new Float32Array(frames), new Float32Array(frames)];
    let next = 0;
    for (let at = 0; at < frames; at += Q) {
        while (next < moves.length && moves[next].frame <= at) proc.port.onmessage({ data: { set: moves[next++].set } });
        iL.set(inL.subarray(at, at + Q)); iR.set(inR.subarray(at, at + Q));
        proc.process([[iL, iR]], [[oL, oR]], {});
        out[0].set(oL, at); out[1].set(oR, at);
    }
    return out;
}

group('integer knobs: non-finite ignored, finite clamped onto the range — as the float knobs');
{
    const base = { reverb_character: 2, delay_mix: 40, delay_division: 6, reverb_mix: 50 };
    const frames = Q * 400;
    const dirty = await renderWith(w0, frames, base, [
        { frame: Q * 100, set: { reverb_on: NaN, reverb_character: NaN, delay_on: -Infinity, delay_sync: 2 ** 32,
                                 delay_division: 2 ** 32 + 3 } },
        { frame: Q * 200, set: { reverb_character: 99, delay_division: -7 } },
    ], { module: w0.module });
    const clean = await renderWith(w0, frames, base, [
        { frame: Q * 100, set: { delay_division: 12 } },
        { frame: Q * 200, set: { reverb_character: 5, delay_division: 0 } },
    ], { module: w0.module });
    const c = compare(dirty, clean);
    ok(c.within(0), `NaN and ±Infinity leave a switch or a choice where it stood, 2^32 is no 0, 99 and -7 clamp `
                    + `(${c.differ} samples differ, worst ${c.worst})`);
}

group('the bytes path: a processor that compiles the posted bytes itself renders the same bits');
{
    const knobs = { ...BUSY, reverb_character: 2 };
    const viaModule = await renderWith(w0, Q * 300, knobs, [], { module: w0.module });
    const viaBytes = await renderWith(w0, Q * 300, knobs, [], { module: null, wasmBytes: w0.wasmBytes });
    const c = compare(viaBytes, viaModule);
    ok(c.within(0), `wasmBytes against module: ${c.differ} samples differ`);

    // What WebKit does with a Module it cannot clone into the worklet: the option arrives as null.
    const proc = new w0.cls({ numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
                              processorOptions: { module: null, knobs } });
    const first = await settle(proc);
    ok(first && first.error && first.retry === 'wasmBytes',
       `a null module answers { error, retry: 'wasmBytes' } (got ${JSON.stringify(first)})`);
    ok(passesDry(proc, 200) && !proc.port.closed, 'and then passes its input dry, bit for bit, for 200 quanta, alive');
}

group('an error is not an end: the node stands in series, so it passes dry and lives; only dispose ends it');
{
    const destroyed = [];
    const spy = (m) => {
        const destroy = m._fc_guitar_fx_destroy;
        m._fc_guitar_fx_destroy = (fx) => { destroyed.push(fx); return destroy(fx); };
    };
    w0.ctx.__onFxModule = spy;
    const disposeEnds = (proc, what) => {
        proc.port.onmessage?.({ data: { dispose: true } });
        ok(quantum(proc) === false && proc.port.closed && proc.m === null, `${what}: dispose ends it — false, port closed`);
    };

    // A live processor: dispose is the end, and frees the instance.
    const proc = await newProcessor(w0, {});
    const fx = proc.fx;
    ok(quantum(proc) === true, 'a live processor keeps its node alive');
    disposeEnds(proc, 'live');
    ok(destroyed.length === 1 && destroyed[0] === fx && proc.L === null, `dispose destroys the instance (${destroyed})`);

    // No module: the bytes retry asked for, the dry through, alive until disposed.
    const none = new w0.cls({ numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
                              processorOptions: { module: null } });
    const asked = await settle(none);
    ok(asked && asked.retry === 'wasmBytes' && passesDry(none, 200) && !none.port.closed,
       'no module: { error, retry } once, then 200 quanta dry, bit for bit, alive');
    ok(none.port.posted.filter((m) => m.error).length === 1, 'no module: one error, not one per quantum');
    disposeEnds(none, 'no module');

    // A refused prepare: the instance freed at once, the dry through.
    destroyed.length = 0;
    const wLow = await loadWorklet(500);   // below the stages' 1 kHz floor: prepare refuses
    wLow.ctx.__onFxModule = spy;
    const low = new wLow.cls({ numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
                               processorOptions: { module: wLow.module } });
    const said = await settle(low);
    ok(said && said.error && destroyed.length === 1, `a refused prepare: an error, the instance destroyed `
                                                     + `(${JSON.stringify(said)}, ${destroyed.length} destroyed)`);
    ok(passesDry(low, 200) && !low.port.closed && low.port.posted.filter((m) => m.error).length === 1,
       'a refused prepare: then 200 quanta dry, bit for bit, alive, one error');
    disposeEnds(low, 'refused prepare');
    ok(destroyed.length === 1, 'dispose after a failure frees nothing twice');

    // A quantum of another size than the one prepared: an error once, that quantum and every later one dry.
    destroyed.length = 0;
    const odd = await newProcessor(w0, { delay_mix: 50, reverb_mix: 60 });
    ok(passesDry(odd, 50, 256) && destroyed.length === 1 && !odd.port.closed,
       `256-frame quanta with no renderQuantumSize in scope: dry from the first, bit for bit, alive (${destroyed.length} destroyed)`);
    ok(odd.port.posted.filter((m) => m.error).length === 1, 'and one error for fifty such quanta');
    odd.port.onmessage({ data: { set: { delay_mix: 100 } } });   // knobs to a failed processor change nothing
    ok(passesDry(odd, 10, 256), 'a knob after the error changes nothing: still dry');
    disposeEnds(odd, 'foreign quantum');
    w0.ctx.__onFxModule = undefined;
}

group(`the wasm render against the native render, within ${TOLERANCE} (law 10)`);
for (const s of SCENARIOS) {
    const frames = Math.round(s.sr * s.seconds / Q) * Q;
    const inL = makeInput(frames, s.sr, 0), inR = makeInput(frames, s.sr, 1);
    const w = s.sr === 48000 ? w0 : await loadWorklet(s.sr);
    const { out } = await renderWorklet(w, frames, s.knobs, s.moves, inL, inR);
    const ref = renderNative(s.sr, frames, s.knobs, s.moves, inL, inR);
    const c = compare(out, ref);
    let peak = 0, moved = 0;
    for (let ch = 0; ch < 2; ++ch)
        for (let i = 0; i < frames; ++i) {
            peak = Math.max(peak, Math.abs(ref[ch][i]));
            moved = Math.max(moved, Math.abs(ref[ch][i] - (ch ? inR : inL)[i]));
        }
    console.log(`      ${s.name}: ${c.differ} of ${2 * frames} samples differ, worst ${c.worst.toExponential(3)}, `
                + `${c.nonFinite} non-finite (peak ${peak.toFixed(3)})`);
    ok(c.within(TOLERANCE), `${s.name}: worst |wasm - native| ${c.worst}, ${c.nonFinite} non-finite, against ${TOLERANCE}`);
    ok(moved > 1e-3, `${s.name}: the render is not the input`);
}

group('the comparison itself: a NaN is a failure that no later difference can hide');
{
    const pair = () => [[new Float32Array(1000), new Float32Array(1000)], [new Float32Array(1000), new Float32Array(1000)]];
    const [a, b] = pair();
    a[0][100] = NaN;          // a fault, early...
    a[0][900] = 1e-7;         // ...then a finite difference, inside the tolerance, after it
    const c = compare(a, b);
    ok(!c.within(TOLERANCE) && c.nonFinite === 1, `a NaN followed by a finite difference is caught (nonFinite ${c.nonFinite}, worst ${c.worst})`);
    const [x, y] = pair();
    y[1][500] = Infinity;     // on the reference side
    const d = compare(x, y);
    ok(!d.within(TOLERANCE) && d.nonFinite === 1, `an Infinity in the native render is caught (nonFinite ${d.nonFinite})`);
}

group('no allocation per render quantum');
{
    if (typeof globalThis.gc !== 'function') {
        ok(false, 'run with --expose-gc');
    } else {
        const proc = await newProcessor(w0, { ...BUSY, reverb_character: 5, delay_mix: 80 });
        // ONE loop, warmed first. The heap still moves a little while a loop's code tiers up — measured 5 KB with no
        // quanta, 17 KB after 20 000 and 21.6 KB after 320 000 — so the gate is the DIFFERENCE between a short window
        // and a long one, which a fixed cost cannot reach and a per-quantum allocation cannot hide from: 60 000 more
        // quanta with one 16-byte object each would add 960 KB.
        const iL = makeInput(Q, 48000, 0), iR = makeInput(Q, 48000, 1), oL = new Float32Array(Q), oR = new Float32Array(Q);
        const inputs = [[iL, iR]], outputs = [[oL, oR]], params = {};
        const run = (k) => { for (let i = 0; i < k; ++i) proc.process(inputs, outputs, params); };
        run(4000);
        run(4000);
        // A collection is counted by WHEN it ran: the observer delivers late, so one that finished before a window
        // (the explicit gc() below, the loads' own) can arrive inside it.
        const gcs = [];
        const obs = new PerformanceObserver((list) => { for (const e of list.getEntries()) gcs.push(e.startTime); });
        obs.observe({ entryTypes: ['gc'] });
        const windowed = async (n) => {
            globalThis.gc();
            await new Promise((r) => setImmediate(r));
            const before = process.memoryUsage().heapUsed;
            const t0 = performance.now();
            run(n);
            const t1 = performance.now();
            return { grew: process.memoryUsage().heapUsed - before, t0, t1 };
        };
        const short = await windowed(20000);
        const long = await windowed(80000);
        await new Promise((r) => setTimeout(r, 50));
        obs.disconnect();
        const inWindows = gcs.filter((t) => (t >= short.t0 && t <= short.t1) || (t >= long.t0 && t <= long.t1)).length;
        const extra = long.grew - short.grew;
        console.log(`      the JS heap moved ${short.grew} bytes over 20 000 quanta and ${long.grew} over 80 000 `
                    + `(${extra} for the 60 000 more), ${inWindows} garbage collections in either window`);
        ok(inWindows === 0 && extra < 16384, `process() allocates: ${extra} bytes for 60 000 more quanta, ${inWindows} GCs`);
    }
}

group('cost of one 128-frame stereo quantum at 48 kHz, heaviest settings (reported, not gated)');
{
    const budgetUs = Q / 48000 * 1e6;
    const heavy = { delay_sync: 1, delay_division: 0, delay_bpm: 40, delay_repeats: 100, delay_offset_ms: 30,
                    delay_mix: 100, reverb_mix: 100, reverb_decay: 2, reverb_predelay_ms: 100, reverb_hpf_hz: 40 };
    for (const [name, c] of [['Hall', 2], ['Modulated', 5]]) {
        const proc = await newProcessor(w0, { ...heavy, reverb_character: c });
        const iL = new Float32Array(Q), iR = new Float32Array(Q), oL = new Float32Array(Q), oR = new Float32Array(Q);
        let seed = 1;
        for (let i = 0; i < Q; ++i) {
            seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; iL[i] = (seed / 4294967296 - 0.5) * 0.5;
            seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; iR[i] = (seed / 4294967296 - 0.5) * 0.5;
        }
        const inputs = [[iL, iR]], outputs = [[oL, oR]], params = {};
        for (let i = 0; i < 5000; ++i) proc.process(inputs, outputs, params);
        const n = 40000;
        const t0 = performance.now();
        for (let i = 0; i < n; ++i) proc.process(inputs, outputs, params);
        const meanUs = (performance.now() - t0) * 1000 / n;
        const m = proc.m, fx = proc.fx, pL = proc.pL, pR = proc.pR;
        const t1 = performance.now();
        for (let i = 0; i < n; ++i) m._fc_guitar_fx_process(fx, pL, pR, Q);
        const coreUs = (performance.now() - t1) * 1000 / n;
        const per = new Float64Array(n);
        for (let i = 0; i < n; ++i) { const a = performance.now(); proc.process(inputs, outputs, params); per[i] = performance.now() - a; }
        per.sort();
        const p99 = per[Math.floor(n * 0.99)] * 1000, p999 = per[Math.floor(n * 0.999)] * 1000;
        console.log(`      ${name} + 1/1 at 40 BPM (6 s line): process() ${meanUs.toFixed(1)} us mean `
                    + `(${(100 * meanUs / budgetUs).toFixed(2)} % of the ${budgetUs.toFixed(0)} us quantum), `
                    + `p99 ${p99.toFixed(1)} us, p99.9 ${p999.toFixed(1)} us; the wasm call alone ${coreUs.toFixed(1)} us`);
    }
}

console.log(`\n${checks} checks, ${failures} failures\n${failures ? 'TESTS FAILED' : 'ALL TESTS PASSED'}`);
process.exit(failures ? 1 : 0);
