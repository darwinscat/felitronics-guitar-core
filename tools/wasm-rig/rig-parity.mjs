// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// rig-parity — the built fcrig module, run the way a page runs it, against the native render of the same ABI.
//
//   node --experimental-vm-modules --expose-gc tools/wasm-rig/rig-parity.mjs [--namz=<namz CLI>] [--stand-in=<A2.nam>]
//        <build-dir> <fc_rig_render> <abi-header> [<packs-dir>]
//
// --stand-in names NAM's example A2 (the architecture of every site pack): without a real pack, THE START and the
// session's decisions are timed on it, packed by the namz CLI under the fixture's manifest (see `timedPack`).
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
//   8. FLOAT16 (namz 4.4.0; with --namz, the CLI at the pin): every pack's captures made .nam16z by `namz encode
//      --f16`, and made float32 .namz again from those halves widened (`decode`, `encode`); the .nam16z pack plays
//      BIT FOR BIT what the widened pack plays, natively and in the module, the worklet taking every .nam16z by the
//      name rig.json gives it — asked for by { need } and handed in up front — and the same halves under .namz names
//      playing the same (the dtype byte decides, never the name).

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import vm from 'node:vm';
import { spawnSync } from 'node:child_process';
import { performance, PerformanceObserver } from 'node:perf_hooks';

const NAMZ_ARG = '--namz=';
const namzCli = (process.argv.find((a) => a.startsWith(NAMZ_ARG)) || '').slice(NAMZ_ARG.length);
const STAND_IN_ARG = '--stand-in=';
const standIn = (process.argv.find((a) => a.startsWith(STAND_IN_ARG)) || '').slice(STAND_IN_ARG.length);
const [dir, renderBin, header, packsDir] = process.argv.slice(2)
    .filter((a) => !a.startsWith(NAMZ_ARG) && !a.startsWith(STAND_IN_ARG));
if (!dir || !renderBin || !header) {
    console.error('usage: rig-parity.mjs [--namz=<namz CLI>] [--stand-in=<A2.nam>] <build-dir> <fc_rig_render> <fc_rig_abi.h> '
                  + '[<packs-dir>]');
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
const SCOPE_N = 2048;   // the player's scope tap: what a { scope } carries back at most

let failures = 0, checks = 0;
const ok = (cond, msg) => { ++checks; if (!cond) { ++failures; console.log('    FAIL: ' + msg); } };
const group = (name) => console.log('  - ' + name);

// ---- the simulated AudioWorkletGlobalScope -------------------------------------------------------------------------
class AudioWorkletProcessor {
    constructor() {
        // A port as a page's end of it sees one: a message posted WITH a transfer list arrives as the structured
        // clone (the listed buffers detached here, the receiver's its own), and the list is recorded.
        this.port = { onmessage: null, posted: [], transferred: [], closed: false,
                      postMessage(m, transfer) {
                          const list = transfer || [];
                          this.transferred.push(...list);
                          this.posted.push(list.length ? structuredClone(m, { transfer: list }) : m);
                      },
                      close() { this.closed = true; } };
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
    // A WORKLET'S CLOCK IS PACED: its quanta come one every 128 / sampleRate s, and node runs them back to back. So the
    // scope's Date.now is the host's time within the quantum plus the quanta's own pace — the start times its renders
    // with it exactly as in a browser. (A test that stubs Date.now replaces this one.)
    ctx.__hostNow = () => performance.now();
    // A quantum starts at a random phase of the millisecond, as in a browser (its callbacks are not locked to Date.now's
    // ticks). The state lives in a Float64Array: the pace allocates nothing per quantum (the allocation gate below).
    vm.runInContext(`(() => { const st = new Float64Array(3);
        globalThis.__paceQuantum = (now) => { st[0] += 1; st[1] = now; st[2] = Math.random(); };
        Date.now = () => Math.floor(1.7e12 + st[0] * ${1000 * Q / sampleRate} + st[2] + (__hostNow() - st[1])); })()`, ctx);
    const paced = registered.cls.prototype.process, pace = ctx.__paceQuantum;
    registered.cls.prototype.process = function (inputs, outputs, params) {
        if (!this.unpaced) pace(performance.now());
        return paced.call(this, inputs, outputs, params);
    };
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
        if (s.feed !== undefined) args.push(at, 'feed', String(s.feed));
        if (s.standby !== undefined) args.push(at, 'standby', s.standby ? '1' : '0');
    }
    return args;
}

function renderNative(packDir, sr, frames, channels, maxModels, moves, ins, scopeEvery = 0) {
    const scopeFile = scopeEvery > 0 ? path.join(fixtureDir, 'scope.bin') : null;   // one file, removed with the fixture
    const args = [packDir, String(sr), String(frames), String(channels), 'max', String(maxModels),
                  ...(scopeFile ? ['scope', scopeFile, String(scopeEvery)] : []), ...scriptOf(moves)];
    const input = Buffer.alloc(frames * 4 * channels);
    for (let c = 0; c < channels; ++c) Buffer.from(ins[c].buffer).copy(input, c * frames * 4);
    const r = spawnSync(renderBin, args, { input, maxBuffer: frames * 4 * channels + 1024 });
    if (r.status !== 0) throw new Error(`fc_rig_render exited ${r.status}: ${r.stderr}`);
    const out = new Float32Array(r.stdout.buffer.slice(r.stdout.byteOffset, r.stdout.byteOffset + frames * 4 * channels));
    const outs = Array.from({ length: channels }, (_, c) => out.subarray(c * frames, (c + 1) * frames));
    if (!scopeFile) return outs;
    // fc_rig_render's records: int32 n, float64 frames, 2048 dry, 2048 wet.
    const bin = fs.readFileSync(scopeFile), rec = 4 + 8 + 2 * SCOPE_N * 4, scopes = [];
    for (let at = 0; at + rec <= bin.length; at += rec) {
        const dv = new DataView(bin.buffer, bin.byteOffset + at, rec);
        const f = new Float32Array(bin.buffer.slice(bin.byteOffset + at + 12, bin.byteOffset + at + rec));
        scopes.push({ n: dv.getInt32(0, true), frames: dv.getFloat64(4, true), dry: f.subarray(0, SCOPE_N), wet: f.subarray(SCOPE_N) });
    }
    return { outs, scopes };
}

async function renderWorklet(w, packDir, frames, channels, maxModels, moves, ins, override, scopeEvery = 0, extra = {}) {
    let buf = scopeEvery > 0 ? new ArrayBuffer(2 * SCOPE_N * 4) : null;   // the page's ONE scope buffer, travelling
    const scopes = [];
    let travelBad = 0;   // a { scope } that did not travel as a page's does: transferred out, transferred back
    const proc = await newProcessor(w, packDir, { calibrate: false, maxModels, stereo: channels === 2, ...extra });
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
        if (buf && (at / Q + 1) % scopeEvery === 0) {
            const inbound = structuredClone({ scope: buf }, { transfer: [buf] });   // the page posts it, transferred
            const mark = proc.port.posted.length;
            proc.port.onmessage({ data: inbound });
            const back = proc.port.posted.slice(mark).find((m) => m.scope !== undefined);
            if (!back || !proc.port.transferred.includes(inbound.scope) || inbound.scope.byteLength !== 0
                || !(back.scope instanceof ArrayBuffer) || back.scope.byteLength !== 2 * SCOPE_N * 4) {
                ++travelBad;
                buf = new ArrayBuffer(2 * SCOPE_N * 4);
                continue;
            }
            const f = new Float32Array(back.scope);
            scopes.push({ n: back.n, frames: back.frames, dry: f.slice(0, SCOPE_N), wet: f.slice(SCOPE_N) });
            buf = back.scope;
        }
    }
    return { outs, proc, scopes, travelBad };
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
// THE PACK THE START IS TIMED ON: one whose capture warms for many quanta before it is heard, every capture one
// architecture — what THE START's and the session's decisions are about. A real pack is one. The fixture is not, and on
// it those decisions are not the machine's but the pack's: its captures (a receptive field of a few samples) open the
// sound two quanta into a start, and CAL_GRACE quanta later fewer than CAL_RENDERS renders were timed — 'short', one
// model, by design, on every machine ('nothing timable' below asserts exactly that). Where the real packs are absent
// (CI: they are not public), NAM's example A2 — the architecture of every site pack — stands in: packed by the namz CLI
// and put under every file name of the fixture's manifest, so the pack's controls are the fixture's and its captures
// one architecture. Without --stand-in and --namz it is null and those groups say they were skipped.
const timedPack = realPack !== packs[0] ? realPack : (() => {
    if (!standIn || !namzCli) return null;
    const d = fs.mkdtempSync(path.join(os.tmpdir(), 'fc-rig-a2-'));
    const z = path.join(d, 'a2.namz');
    const r = spawnSync(namzCli, ['encode', standIn, z]);
    if (r.status !== 0) throw new Error(`namz encode ${standIn} exited ${r.status}: ${r.stderr}`);
    const rig = JSON.parse(fs.readFileSync(path.join(fixtureDir, 'rig.json'), 'utf8'));
    const st = rig.chain.find((s) => s.kind === 'nam');
    for (const f of (st.device || st).files) {
        f.file = f.file.replace(/\.nam$/, '.namz');   // the fixture's one plain .nam: the stand-in is packed
        fs.copyFileSync(z, path.join(d, f.file));
    }
    fs.writeFileSync(path.join(d, 'rig.json'), JSON.stringify(rig));
    return { name: 'A2 (NAM\'s example, standing in for a real pack)', dir: d };
})();

const w0 = await loadWorklet(48000);

group('the worklet loads in an AudioWorkletGlobalScope, and its exports are the ABI');
{
    const proc = await newProcessor(w0, null, { calibrate: false });
    const want = [...fs.readFileSync(header, 'utf8').matchAll(/^[A-Za-z_][A-Za-z0-9_ *]* (fc_rig_[a-z_]+) \(/gm)]
        .map((m) => '_' + m[1]).sort();
    const got = Object.keys(proc.m).filter((k) => k.startsWith('_fc_rig_')).sort();
    ok(want.length === 48, `the header declares 48 entry points (read ${want.length})`);
    ok(JSON.stringify(got) === JSON.stringify(want), `the module exports exactly the header's entry points\n      got  ${got}\n      want ${want}`);
    ok(proc.m._fc_rig_abi_version() === 5, 'abi_version answers 5');
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

// ---- 8. float16 ----------------------------------------------------------------------------------------------------
/** The nam stage's file list in a manifest (the device's fields sit on the stage in rig.json, or under `device`). */
const namFilesOf = (rig) => { const st = rig.chain.find((s) => s.kind === 'nam'); return (st.device || st).files; };

/** A pack made float16 with namz's CLI, and its float32 twin: every capture `decode`d (a raw .nam taken as it is),
    `encode --f16` to a .nam16z,
    that `decode`d (each half widened to its float32) and `encode`d to a .namz. Three packs: `f16` names the .nam16z,
    `wide` the widened .namz, `renamed` the very bytes of `f16` under .namz names. */
function float16Packs(pack) {
    const base = fs.mkdtempSync(path.join(os.tmpdir(), 'fc-rig-f16-'));
    const out = { base, f16: path.join(base, 'f16'), wide: path.join(base, 'wide'), renamed: path.join(base, 'renamed') };
    for (const d of [out.f16, out.wide, out.renamed]) fs.mkdirSync(d);
    const cli = (...args) => {
        const r = spawnSync(namzCli, args);
        if (r.status !== 0) throw new Error(`namz ${args.join(' ')} exited ${r.status}: ${r.stderr}`);
    };
    const manifest = fs.readFileSync(path.join(pack.dir, 'rig.json'), 'utf8');
    const rigs = { f16: JSON.parse(manifest), wide: JSON.parse(manifest), renamed: JSON.parse(manifest) };
    const files = namFilesOf(rigs.f16);
    files.forEach((f, i) => {
        const stem = `capture-${String(i).padStart(3, '0')}`, nam = path.join(base, stem + '.nam'), back = path.join(base, stem + '-wide.nam');
        const src = path.join(pack.dir, f.file);
        if (fs.readFileSync(src, 'utf8').trimStart().startsWith('{')) fs.copyFileSync(src, nam);   // a raw .nam (the fixture has one)
        else cli('decode', src, nam);
        cli('encode', nam, path.join(out.f16, stem + '.nam16z'), '--f16');
        cli('decode', path.join(out.f16, stem + '.nam16z'), back);
        cli('encode', back, path.join(out.wide, stem + '.namz'));
        fs.copyFileSync(path.join(out.f16, stem + '.nam16z'), path.join(out.renamed, stem + '.namz'));
        namFilesOf(rigs.f16)[i].file = stem + '.nam16z';
        namFilesOf(rigs.wide)[i].file = stem + '.namz';
        namFilesOf(rigs.renamed)[i].file = stem + '.namz';
    });
    for (const k of ['f16', 'wide', 'renamed']) fs.writeFileSync(path.join(out[k], 'rig.json'), JSON.stringify(rigs[k]));
    out.names = namFilesOf(rigs.f16).map((f) => f.file);
    out.dtypes = out.names.map((n) => fs.readFileSync(path.join(out.f16, n))[6]);
    return out;
}

const sameBits = (a, b) => a.length === b.length && a.every((x, c) => Buffer.compare(Buffer.from(x.buffer, x.byteOffset, x.byteLength),
                                                                                   Buffer.from(b[c].buffer, b[c].byteOffset, b[c].byteLength)) === 0);

if (!namzCli) {
    console.log('  - float16: skipped (no --namz=<namz CLI>)');
} else {
    group('float16 (namz encode --f16): a .nam16z pack plays bit for bit as the float32 pack of its widened weights');
    for (const pack of packs) {
        const p = float16Packs(pack);
        ok(p.dtypes.length > 0 && p.dtypes.every((d) => d === 1), `${pack.name}: the CLI wrote ${p.dtypes.length} files of dtype 1 (${[...new Set(p.dtypes)]})`);
        const frames = Math.round(48000 * 2 / Q) * Q;
        const moves = movesFor(p.f16, frames);
        for (const maxModels of pack.name === 'fixture' ? [2, 1] : [2]) {
            const ins = [makeInput(frames, 48000, 0)];
            const label = `${pack.name}, ${maxModels === 2 ? 'two models' : 'one model'}`;
            const half = await renderWorklet(w0, p.f16, frames, 1, maxModels, moves, ins);
            const wide = await renderWorklet(w0, p.wide, frames, 1, maxModels, moves, ins);
            const needs = half.proc.port.posted.filter((m) => m.need !== undefined).map((m) => m.need);
            const loads = half.proc.port.posted.filter((m) => m.loaded !== undefined);
            let peak = 0;
            for (const v of half.outs[0]) peak = Math.max(peak, Math.abs(v));
            ok(needs.length >= 2 && needs.every((n) => p.names.includes(n) && n.endsWith('.nam16z')),
               `${label}: the worklet asks for the .nam16z by rig.json's names (${needs.length} { need })`);
            ok(loads.length >= 2 && loads.every((m) => m.ok), `${label}: every .nam16z loads (${loads.filter((m) => !m.ok).length} of ${loads.length} refused)`);
            ok(peak > 0.01 && sameBits(half.outs, wide.outs), `${label}: wasm — the .nam16z pack == the widened float32 pack, bit for bit (peak ${peak.toFixed(3)}, ${compare(half.outs, wide.outs).differ} samples differ)`);
            const nHalf = renderNative(p.f16, 48000, frames, 1, maxModels, moves, ins);
            const nWide = renderNative(p.wide, 48000, frames, 1, maxModels, moves, ins);
            ok(sameBits(nHalf, nWide), `${label}: native — the .nam16z pack == the widened float32 pack, bit for bit (${compare(nHalf, nWide).differ} samples differ)`);
            ok(compare(half.outs, nHalf).within(TOLERANCE), `${label}: …and the module's .nam16z render is the native one within ${TOLERANCE}`);
            for (const r of [half, wide]) r.proc.port.onmessage({ data: { dispose: true } });
            if (maxModels !== 2) continue;
            // Handed in up front ({ files } with the pack, fc_rig_put_file), by the names rig.json gives: no { need } at all.
            const upFront = p.names.map((n) => ({ file: n, bytes: w0.buffer(fs.readFileSync(path.join(p.f16, n))) }));
            const given = await renderWorklet(w0, p.f16, frames, 1, maxModels, moves, ins, undefined, 0, { files: upFront });
            ok(!given.proc.port.posted.some((m) => m.need !== undefined) && sameBits(given.outs, wide.outs),
               `${label}: the .nam16z handed in up front — no { need }, the same samples, bit for bit`);
            given.proc.port.onmessage({ data: { dispose: true } });
            // The same halves under .namz names: the name never picks the dtype.
            const renamed = await renderWorklet(w0, p.renamed, frames, 1, maxModels, moves, ins);
            ok(sameBits(renamed.outs, half.outs), `${label}: the same halves named .namz play the same, bit for bit`);
            renamed.proc.port.onmessage({ data: { dispose: true } });
        }
        fs.rmSync(p.base, { recursive: true, force: true });
    }
}

group(`ABI 2 — the scope and PARK through the processor, against the native render, within ${TOLERANCE}`);
{
    const early = await newProcessor(w0, null, { calibrate: false });
    const probe = new ArrayBuffer(2 * SCOPE_N * 4);
    early.port.onmessage({ data: { scope: probe } });
    const first = early.port.posted.findLast((m) => m.scope !== undefined);
    ok(first && early.port.transferred.includes(probe) && probe.byteLength === 0 && first.scope.byteLength === 2 * SCOPE_N * 4
       && first.n === 0 && first.frames === 0,
       'a { scope } before anything was rendered goes back transferred (detached here, whole there): n 0, frames 0');
    early.port.onmessage({ data: { dispose: true } });
    for (const pack of [packs[0], ...(realPack !== packs[0] ? [realPack] : [])]) {
        const frames = Math.round(48000 * 3 / Q) * Q;
        const at = (s) => Math.round(48000 * s / Q) * Q;
        const moves = [
            { frame: at(0.4), set: { feed: 4 } },
            { frame: at(0.9), set: { standby: true } },          // PARK…
            { frame: at(1.2), set: { gain: 60, feed: -3 } },     // …knobs and the feed moved while stood by…
            { frame: at(1.6), set: { standby: false } },         // …and back
            { frame: at(2.0), set: { standby: true } },          // a fade cut short both ways
            { frame: at(2.0) + 3 * Q, set: { standby: false } },
            { frame: at(2.5), set: { feed: 0 } },
        ];
        const ins = [makeInput(frames, 48000, 0)];
        const every = 8;
        const { outs, proc, scopes, travelBad } = await renderWorklet(w0, pack.dir, frames, 1, 2, moves, ins, undefined, every);
        const ref = renderNative(pack.dir, 48000, frames, 1, 2, moves, ins, every);
        const c = compare(outs, ref.outs);
        let worst = 0, frameMiss = 0, nMiss = 0, still = 0, fresh = 0;
        for (let k = 0; k < Math.min(scopes.length, ref.scopes.length); ++k) {
            const a = scopes[k], b = ref.scopes[k];
            if (a.n !== b.n) ++nMiss;
            if (a.frames !== b.frames) ++frameMiss;
            if (k > 0) { if (a.frames === scopes[k - 1].frames) ++still; else ++fresh; }
            for (let i = 0; i < a.n; ++i)
                worst = Math.max(worst, Math.abs(a.dry[i] - b.dry[i]), Math.abs(a.wet[i] - b.wet[i]));
        }
        console.log(`      ${pack.name}: output worst ${c.worst.toExponential(3)}; ${scopes.length} frames read, scope worst `
                    + `${worst.toExponential(3)}, ${fresh} fresh and ${still} unchanged (stood by)`);
        ok(c.within(TOLERANCE), `${pack.name}: the output through PARK and back, worst ${c.worst}`);
        ok(scopes.length === ref.scopes.length && scopes.length > 100 && nMiss === 0 && frameMiss === 0 && worst <= TOLERANCE,
           `${pack.name}: every scope frame — its count, its length, both sides — the native one's (worst ${worst})`);
        ok(still > 5 && fresh > 50, `${pack.name}: the count stands still while stood by and moves otherwise`);
        ok(travelBad === 0, `${pack.name}: every scope buffer went back in the transfer list — detached on the processor's `
           + `side, the frame whole on the page's (${travelBad} did not)`);
        ok(!proc.port.posted.some((m) => m.error), `${pack.name}: no error`);
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

group('the start: the DRIVE\'s own capture timed on its live renders — the blend on when one render fits a quarter of the '
      + 'quantum, { calibrated } in the quantum the sound opens, no stall');
/** A CLOCK THE TEST OWNS, on the next module the worklet makes: the scope's Date.now reads floor(now); `c.quantum()`
    moves now by a quantum's length and every render by `c.cost` ms. The start's DECISION is then what the test says a
    render costs, on any machine. The paced clock (loadWorklet) reads the host's own time, so on it a decision is the
    machine's speed: a slow runner timing a capture fast enough elsewhere says one model, and a test asserting either
    outcome on it fails somewhere. `spy(m)` hooks the module too; `c.renders` counts fc_rig_process calls. */
const QUANTUM_MS = 1000 * Q / 48000;
function ownedClock(spy) {
    const c = { now: 1.7e12, cost: 0, renders: 0, restore: () => {} };
    c.quantum = () => { c.now += QUANTUM_MS; };
    w0.ctx.__onRigModule = (m) => {
        w0.ctx.__onRigModule = undefined;
        const D = vm.runInContext('Date', w0.ctx), real = D.now, process = m._fc_rig_process;
        D.now = () => Math.floor(c.now);
        m._fc_rig_process = (...a) => { const r = process(...a); ++c.renders; c.now += c.cost; return r; };
        c.restore = () => { D.now = real; };
        if (spy) spy(m);
    };
    return c;
}
async function calibrated(pack, costMs) {
    const calls = { max: [], gain: [] };
    const c = ownedClock((m) => {
        const max = m._fc_rig_set_max_models, gain = m._fc_rig_set_gain_dial;
        m._fc_rig_set_max_models = (r, n) => { calls.max.push(n); return max(r, n); };
        m._fc_rig_set_gain_dial = (r, d) => { calls.gain.push(d); return gain(r, d); };
    });
    c.cost = costMs;
    const proc = await newProcessor(w0, pack.dir, { knobs: { gain: 200 } });
    proc.port.onmessage({ data: { set: { gain: 210 } } });   // sent while it decides: lands at once, in order
    const ins = makeInput(Q * 3000, 48000, 0), oL = new Float32Array(Q), oR = new Float32Array(Q);
    let loudBefore = 0, loudAt = 0, q = 0, cal, longest = 0, most = 0;
    for (; q < 3000 && !(cal = proc.port.posted.find((m) => m.calibrated)); ++q) {
        const t = performance.now(), r0 = c.renders;
        c.quantum();
        proc.process([[ins.subarray(q * Q, (q + 1) * Q)]], [[oL, oR]], {});
        longest = Math.max(longest, performance.now() - t);
        most = Math.max(most, c.renders - r0);
        let peak = 0;
        for (let i = 0; i < Q; ++i) peak = Math.max(peak, Math.abs(oL[i]), Math.abs(oR[i]));
        if (proc.port.posted.find((m) => m.calibrated)) loudAt = peak; else loudBefore = Math.max(loudBefore, peak);
        answer(proc, w0, pack.dir);
    }
    c.restore();
    return { cal: cal && cal.calibrated, calls, loudBefore, loudAt, quanta: q, proc, longest, most };
}
if (!timedPack) {
    console.log('  - skipped: no real pack, and no --stand-in with --namz to make one');
} else {
    // A fast render: 0.2 ms of the 2.667 ms quantum, 7.5 % (a site pack's A2 capture measured 94.5 us a quantum in
    // node on arm64 macOS — the cost group below).
    const fast = await calibrated(timedPack, 0.2);
    const c = fast.cal;
    console.log(`      ${timedPack.name}: one render ${c ? c.renderUs.toFixed(1) : '?'} us = ${c ? (100 * c.share).toFixed(2) : '?'} % `
                + `of the ${c ? c.quantumUs.toFixed(0) : '?'} us quantum (${c ? c.renders : '?'} renders in ${c ? c.elapsedMs : '?'} ms, `
                + `${c ? c.warm : '?'} of them warming; decided after ${fast.quanta} quanta; the longest quantum ${fast.longest.toFixed(2)} ms `
                + `of this machine's time)`);
    ok(c && c.valid && c.runs === c.renders && c.pairs === 0 && c.renders >= 8, 'timed on its own renders, each one network alone');
    ok(c && c.maxModels === 2 && c.share <= 0.25, 'a fast render: the blend is turned on (maxModels 2)');
    ok(JSON.stringify(fast.calls.max) === '[1,2]', `the module was told 1 at the start and 2 after (${fast.calls.max})`);
    ok(JSON.stringify(fast.calls.gain) === '[200,210]', `the knobs land at once, in order — no 12 o'clock (${fast.calls.gain})`);
    ok(fast.loudBefore === 0 && fast.loudAt > 0, `{ calibrated } in the first quantum that sounds (peak ${fast.loudAt.toFixed(3)} in it)`);
    // NO STALL, counted rather than clocked: v0.6.0's burst ran renders back to back until Date.now moved 30 ms, i.e. as
    // many renders in one quantum as fit there. One render a quantum is that claim on any machine; the wall time above
    // is this machine's and is reported, not gated.
    ok(fast.most === 1, `no stall: one render a quantum through the start, none timed back to back (at most ${fast.most})`);
    fast.proc.port.onmessage({ data: { dispose: true } });

    const slow = await calibrated(timedPack, 2.0);   // a slow render: 2 ms of the 2.667 ms quantum (75 %)
    const s = slow.cal;
    console.log(`      injected slow render: ${s ? s.renderUs.toFixed(0) : '?'} us = ${s ? (100 * s.share).toFixed(1) : '?'} %`);
    ok(s && s.valid && s.maxModels === 1 && s.share > 0.25, 'a slow render: it stays at one model');
    ok(!slow.calls.max.includes(2), `the module was never told 2 (${slow.calls.max})`);
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

group('one decision a session per architecture: the same architecture is not timed again, another is; { calibrate: \'again\' }');
if (!timedPack) {
    console.log('  - skipped: no real pack, and no --stand-in with --namz to make one');
} else {
    // The clock is the test's (ownedClock): a render costs 0.2 ms while "fast" and 2 ms while "slow", on any machine.
    const c = ownedClock();
    c.cost = 0.2;
    // A pack whose captures are one architecture (the fixture mixes a WaveNet with gains, and a blend of two
    // architectures is timed as what it is). The fixture's gain capture is the other architecture.
    const rig = (dir) => w0.buffer(fs.readFileSync(path.join(dir, 'rig.json')));
    let dir = timedPack.dir;
    const proc = await newProcessor(w0, dir, { knobs: { gain: 150 } });
    const step = () => { c.quantum(); play(proc, 1, () => answer(proc, w0, dir)); };
    for (let q = 0; q < 400; ++q) step();
    const first = proc.port.posted.filter((m) => m.calibrated).map((m) => m.calibrated);
    const next = (data) => {
        const mark = proc.port.posted.length;
        proc.port.onmessage({ data });
        for (let q = 0; q < 400 && after(proc, mark, (m) => m.calibrated).length === 0; ++q) step();
        return after(proc, mark, (m) => m.calibrated).map((m) => m.calibrated);
    };
    c.cost = 2.0;                                                         // slow now: a new timing would say so
    const same = next({ load: rig(dir), set: { gain: 200 } });
    dir = fixtureDir;
    const other = next({ load: rig(dir), set: { gain: 60 } });             // a gain capture: another architecture
    dir = timedPack.dir;
    const forced = next({ load: rig(dir), set: { gain: 150 }, calibrate: 'again' });
    c.cost = 0.2;
    const sounding = next({ calibrate: 'again' });                         // while it plays, fast again
    c.restore();
    ok(first.length === 1 && first[0].valid && first[0].maxModels === 2, `the first pack: timed, fast, the blend on (${JSON.stringify(first.map((c) => c.maxModels))})`);
    ok(same.length === 1 && same[0] === first[0], 'the same architecture again: not timed, its decision said as it stands');
    ok(other.length === 1 && other[0] !== first[0] && other[0].maxModels === 1 && other[0].share > 0.25,
       `another architecture: timed (slow now: one model, ${other[0] ? (100 * other[0].share).toFixed(0) : '?'} %)`);
    ok(forced.length === 1 && forced[0] !== first[0] && forced[0].maxModels === 1, "{ calibrate: 'again' } with a { load }: timed again");
    ok(sounding.length === 1 && sounding[0].renders >= 64 && sounding[0].maxModels === 2,
       `{ calibrate: 'again' } while sounding: ${sounding[0] ? sounding[0].renders : '?'} renders timed, the blend on again`);
    proc.port.onmessage({ data: { dispose: true } });
}
group('PARK does not wait for the start: a { load } while parked plays the wire, the start resumes on the return');
{
    const proc = await newProcessor(w0, fixtureDir);                       // calibrate: on (the default)
    play(proc, 400, () => answer(proc, w0, fixtureDir));
    const decided = proc.port.posted.filter((m) => m.calibrated).length;
    proc.port.onmessage({ data: { standby: true } });
    play(proc, 40, () => answer(proc, w0, fixtureDir));
    const mark = proc.port.posted.length;
    proc.port.onmessage({ data: { load: w0.buffer(fs.readFileSync(path.join(fixtureDir, 'rig.json'))) } });
    const n = 300 * Q, xin = new Float32Array(n), yL = new Float32Array(n), yR = new Float32Array(n);
    const oL = new Float32Array(Q), oR = new Float32Array(Q);
    for (let q = 0; q < 300; ++q) {
        riffAt = (riffAt + Q) % RIFF.length;
        const x = RIFF.subarray(riffAt, riffAt + Q);
        proc.process([[x]], [[oL, oR]], {});
        xin.set(x, q * Q); yL.set(oL, q * Q); yR.set(oR, q * Q);
        answer(proc, w0, fixtureDir);
    }
    const lat = proc.m._fc_rig_latency_samples(proc.rig);
    let wire = true, energy = 0;
    for (let i = lat; i < n && wire; ++i) { wire = yL[i] === xin[i - lat] && yR[i] === yL[i]; energy += yL[i] * yL[i]; }
    ok(decided === 1 && wire && energy > 1,
       `parked, a { load }: 300 quanta of the input at fc_rig_latency_samples (${lat}), bit for bit — not silence (energy ${energy.toFixed(1)})`);
    ok(after(proc, mark, (m) => m.calibrated).length === 0 && proc.calState === 'waiting',
       '…and the start waits: nothing timed, no decision while it stands by');
    proc.port.onmessage({ data: { standby: false } });
    proc.port.onmessage({ data: { set: { gain: 240 } } });   // sent during the start: both land at once, in order
    proc.port.onmessage({ data: { set: { gain: 60 } } });
    const back = proc.port.posted.length;
    let q = 0;
    for (; q < 600 && after(proc, back, (m) => m.calibrated).length === 0; ++q) play(proc, 1, () => answer(proc, w0, fixtureDir));
    const at = proc.port.posted.findIndex((m, i) => i >= back && m.calibrated);
    const early = proc.port.posted.slice(back, at < 0 ? undefined : at).some((m) => m.loaded === 'g240.nam' || m.loaded === 'g60.namz');
    const peak = play(proc, 300, () => answer(proc, w0, fixtureDir));
    proc.port.onmessage({ data: { read: true } });
    const st = proc.port.posted.findLast((m) => m.state);
    ok(at >= 0, `back: the start resumes and ends (a second { calibrated } after ${q} quanta)`);
    ok(early !== undefined && peak > 0.01 && st && st.state.sounding.tagged === true,
       `…the knobs sent during it land at once, in order (the last one, 60 — the tagged capture — sounds; peak ${peak.toFixed(3)})`);
    ok(!proc.port.posted.some((m) => m.error), 'no error');
    proc.port.onmessage({ data: { dispose: true } });
}
for (const [what, install, reason] of [
    ['a clock that never moves', () => {
        const D = vm.runInContext('Date', w0.ctx), real = D.now;
        D.now = () => 1000;
        return () => { D.now = real; };
    }, 'clock'],
    ['a coarse clock (100 ms steps: Firefox resistFingerprinting)', () => {
        const D = vm.runInContext('Date', w0.ctx), real = D.now;
        D.now = () => Math.floor(real() / 100) * 100;
        return () => { D.now = real; };
    }, 'clock'],
    ['counters that never show one network alone (two every render)', (m) => {
        m._fc_rig_run_blocks = () => 1;
        return () => {};
    }, 'short'],
]) {
    let restore = () => {}, mod = null;
    hooked((m) => { mod = m; restore = install(m); });
    const proc = await newProcessor(w0, fixtureDir, { knobs: { gain: 150 } });
    let cal;
    for (let q = 0; q < 600 && !(cal = proc.port.posted.find((m) => m.calibrated)); ++q) play(proc, 1, () => answer(proc, w0, fixtureDir));
    restore();
    const c = cal && cal.calibrated;
    ok(mod && c && c.valid === false && c.maxModels === 1 && c.reason === reason,
       `${what}: the evidence is not valid, so one model (${c ? `valid ${c.valid}, maxModels ${c.maxModels}, reason ${c.reason}, `
       + `${c.renders} renders in ${c.elapsedMs} ms` : 'no decision'})`);
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

// NO ALLOCATION, AS A BOUND PER QUANTUM. heapUsed moves by itself in every window, whatever its length, and it is V8's:
// read space by space around each window (200 runs), new space — where a fresh object lands — moved 7.4 … 11.8 KB in
// every one, code and trusted space 6.5 … 31 KB (the JIT). So the gate is the difference between a 5 000-quantum and a
// 20 000-quantum window, per quantum of the 15 000 more: as shipped a first trial reads 0.6 … 1.8 bytes in node 26 on
// arm64 macOS (25 runs) and −0.5 … 2.3 in node 24.19.0 on Linux x64, CI's (60 runs), a second, warmer, −1.6 … 0.6, with
// no garbage collection. The smallest object JavaScript can put on V8's heap is a 16-byte HeapNumber (12 with pointer
// compression, as in Chrome); the bound, 4 bytes per quantum (60 KB over the 15 000), is a quarter of it. Its controls,
// injected into process() in a scratch copy of the worklet and not kept: one boxed number a quantum reads 13.7 … 18.0
// bytes per quantum and fails every trial, an empty object stored on the scope's global 110 … 114.
// TWO CLEAN TRIALS OF THREE (27.10.2026). Once in a while a window's heap shrinks by up to ~110 KB with no collection
// seen. That nightly failed so at 7.40 bytes per quantum, its 5 000 quanta reading −49.8 KB and its 20 000 +61.2 (a run
// of 20.10: +45.0 and +60.6); on Linux x64, eight runs at a time, the 5 000 quanta did so 3 times in 600 runs, and a
// 20 000 read −75.7 KB where +34.7 is usual. The one read space by space had heapUsed ~97 KB above its usual between
// the gc() and the window and old space falling 87.8 KB inside it, new space as in every window: the cause is not
// established, but a heap that shrinks with no collection is not the processor's objects. A trial is the pair of
// windows, clean with no collection in either and under the bound; the gate holds on two clean of up to three. In the
// 5 000 quanta an excursion fails its trial; in the 20 000 it flatters it — letting one boxed number a quantum through
// takes ≥ 145 KB there, in two trials of three, and the one seen was 110.
const ALLOC_BYTES_PER_QUANTUM = 4, ALLOC_TRIALS = 3, ALLOC_CLEAN = 2;
group(`no allocation per render quantum: under ${ALLOC_BYTES_PER_QUANTUM} bytes of JS heap per quantum, no GC`);
for (const opts of [{ calibrate: false, maxModels: 2, knobs: { gain: 165 } }, { knobs: { gain: 165 } }]) {
    // the second: the start, its timing and the quiet warm on (the site's options), measured once the start is over
    const proc = await newProcessor(w0, realPack.dir, opts);
    const iL = makeInput(Q, 48000, 0), oL = new Float32Array(Q), oR = new Float32Array(Q);
    const inputs = [[iL]], outputs = [[oL, oR]], params = {};
    const run = (k) => { for (let i = 0; i < k; ++i) { proc.process(inputs, outputs, params); if (proc.awaiting !== null) answer(proc, w0, realPack.dir); } };
    run(4000);
    run(2000);
    proc.unpaced = true;   // the start is over: the test's paced clock (which boxes a double per quantum) steps aside
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
    const trials = [];
    const clean = () => trials.filter((r) => r.clean).length;
    while (clean() < ALLOC_CLEAN && trials.length - clean() <= ALLOC_TRIALS - ALLOC_CLEAN) {
        const short = await windowed(5000), long = await windowed(20000);
        await new Promise((r) => setTimeout(r, 50));
        const inWindows = gcs.filter((t) => (t >= short.t0 && t <= short.t1) || (t >= long.t0 && t <= long.t1)).length;
        const extra = long.grew - short.grew, perQuantum = extra / 15000;
        console.log(`      trial ${trials.length + 1}: the JS heap moved ${short.grew} bytes over 5 000 quanta and ${long.grew} over `
                    + `20 000 (${extra} for the 15 000 more: ${perQuantum.toFixed(2)} bytes per quantum), ${inWindows} garbage `
                    + `collections in either window`);
        trials.push({ extra, perQuantum, inWindows, clean: inWindows === 0 && perQuantum < ALLOC_BYTES_PER_QUANTUM });
    }
    obs.disconnect();
    ok(clean() >= ALLOC_CLEAN,
       `${JSON.stringify(opts)}: process() allocates: ${clean()} clean of ${trials.length} trials — `
       + trials.map((r) => `${r.perQuantum.toFixed(2)} bytes per quantum (${r.extra} for 15 000 more quanta), ${r.inWindows} GCs`).join('; ')
       + `; the bound ${ALLOC_BYTES_PER_QUANTUM}`);
    proc.port.onmessage({ data: { dispose: true } });
}

{
    // THE START's timed quanta, which the gate above measures after: Date.now allocates its heap number at every call in
    // V8 (~12 bytes a timed quantum in node, measured), so the quanta that read it must END. Two models: nothing is
    // timed after { calibrated }. One model: THE SETTLE, CAL_SETTLE_QUANTA = 512 quanta at most, then nothing.
    for (const slowMs of [0, 2.0]) {
        hooked((m) => {
            const process = m._fc_rig_process;
            m._fc_rig_process = (...a) => { if (slow) { const t = Date.now(); while (Date.now() - t < slowMs) { /* slow */ } } return process(...a); };
        });
        let slow = slowMs > 0;   // the start's renders slow (one model), the rest fast
        const proc = await newProcessor(w0, realPack.dir, { knobs: { gain: 165 } });
        let q = 0, timedAfter = 0;
        for (; q < 400 && !proc.port.posted.some((m) => m.calibrated); ++q) play(proc, 1, () => answer(proc, w0, realPack.dir));
        slow = false;
        const cal = proc.port.posted.find((m) => m.calibrated);
        for (let k = 0; k < 2000; ++k) {
            if (proc.calState !== 'done') ++timedAfter;
            play(proc, 1, () => answer(proc, w0, realPack.dir));
        }
        const two = cal && cal.calibrated.maxModels === 2;
        ok(cal && (two ? timedAfter === 0 : timedAfter > 0 && timedAfter <= 512) && proc.calState === 'done',
           `${two ? 'two models' : 'one model'}: ${timedAfter} quanta timed after { calibrated } (${two ? 'none' : 'THE SETTLE, 512 at most'}), then none`);
        proc.port.onmessage({ data: { dispose: true } });
    }
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

group('THE PRELOAD, FILLED IN THE BACKGROUND (v0.7.0): the start does not wait for the set, one job a quantum, then the '
      + 'gain dial swept both ways — no { need }, no job');
const H = 8;   // CAL_RENDERS: a capture with a short warm-up says { calibrated } at most this many quanta after it opens
for (const pack of packs) {
    const sweep = (JSON.parse(fs.readFileSync(path.join(pack.dir, 'rig.json'), 'utf8')).chain[0].device?.controls
        || JSON.parse(fs.readFileSync(path.join(pack.dir, 'rig.json'), 'utf8')).chain[0].controls || [])
        .find((c) => c.sweep)?.sweep || 300;
    // Jobs counted between two renders: the fill runs ONE a quantum, the handler's answers included.
    const runs = { n: 0, since: 0, most: 0 };
    hooked((m) => {
        const run = m._fc_rig_job_run, render = m._fc_rig_process;
        m._fc_rig_job_run = (r) => { ++runs.n; runs.most = Math.max(runs.most, ++runs.since); return run(r); };
        m._fc_rig_process = (...a) => { runs.since = 0; return render(...a); };
    });
    const proc = await newProcessor(w0, pack.dir, { preload: true });
    const oL = new Float32Array(Q), oR = new Float32Array(Q);
    const ins = makeInput(Q * 6000, 48000, 0);
    // The page answers the { need }s every RTT quanta (43 ms at 48 kHz: a network's round trip), so the fill outlasts
    // the start's warm-up, burst and hold — with every answer at once an 11-copy set is filled before the start's
    // capture has warmed, and the order would prove nothing.
    const RTT = 16;
    let q = 0, loudCal = 0, loudFill = 0, pre, cal, qCal = -1, qPre = -1, pendingCal = -1, qOpen = -1;
    const step = () => {
        proc.process([[ins.subarray((q % 6000) * Q, (q % 6000 + 1) * Q)]], [[oL, oR]], {});
        ++q;
        if (q % RTT === 0) answer(proc, w0, pack.dir);
    };
    for (; q < 5000 && !(pre && cal);) {
        step();
        let peak = 0;
        for (let i = 0; i < Q; ++i) peak = Math.max(peak, Math.abs(oL[i]));
        if (qOpen < 0 && proc.m._fc_rig_audible(proc.rig) === 1) qOpen = q;
        if (!cal && (cal = proc.port.posted.find((m) => m.calibrated))) { qCal = q; pendingCal = proc.m._fc_rig_preload_pending(proc.rig); }
        else if (!cal) loudCal = Math.max(loudCal, peak);
        else if (!pre) loudFill = Math.max(loudFill, peak);
        if (!pre && (pre = proc.port.posted.find((m) => m.preloaded))) qPre = q;
    }
    const runsPre = runs.n, memPre = proc.m.HEAPU8.buffer.byteLength;
    const p = pre && pre.preloaded;
    ok(p && p.total > 0 && p.failed === 0, `${pack.name}: the set is ready (${p ? p.total : '?'} copies, ${p ? p.quanta : '?'} quanta, `
       + `${runsPre} jobs run, the module's memory ${(memPre / 1048576).toFixed(1)} MiB)`);
    ok(cal && cal.calibrated.valid && pendingCal > 0,
       `${pack.name}: the start calibrated without waiting for the set (maxModels ${cal ? cal.calibrated.maxModels : '?'}, at quantum `
       + `${qCal} with ${pendingCal} copies still to fill; { preloaded } at quantum ${qPre})`);
    ok(qCal >= 0 && qOpen >= 0 && qCal - qOpen <= H,
       `${pack.name}: { calibrated } when the sound opened (quantum ${qOpen}, { calibrated } at ${qCal}), not after the set`);
    if (qPre - qCal > H + 4) ok(loudFill > 0, `${pack.name}: it sounded while the set filled (peak ${loudFill.toFixed(3)} before { preloaded })`);
    ok(runs.most === 1, `${pack.name}: at most one job between two renders (${runs.most})`);
    const mark = proc.port.posted.length, runsReady = runs.n;
    let peak = 0, finite = true;
    for (let d = 0; d <= sweep; ++d) { proc.port.onmessage({ data: { set: { gain: d } } }); step(); for (const v of oL) { finite &&= Number.isFinite(v); peak = Math.max(peak, Math.abs(v)); } }
    for (let d = sweep; d >= 0; --d) { proc.port.onmessage({ data: { set: { gain: d } } }); step(); for (const v of oL) { finite &&= Number.isFinite(v); peak = Math.max(peak, Math.abs(v)); } }
    const needs = after(proc, mark, (m) => m.need !== undefined).length;
    ok(needs === 0 && runs.n === runsReady, `${pack.name}: a sweep 0 -> ${sweep} -> 0, one degree a quantum: ${needs} { need }, `
       + `${runs.n - runsReady} jobs run (none), peak ${peak.toFixed(3)}`);
    ok(finite && peak > 0, `${pack.name}: it played`);
    console.log(`      ${pack.name}: { calibrated } at quantum ${qCal} with ${pendingCal} copies to fill, { preloaded } at ${qPre}, `
                + `peak ${loudFill.toFixed(3)} between them; ${p ? p.total : '?'} copies prepared in ${p ? p.quanta : '?'} quanta (${runsPre} jobs), `
                + `module memory ${(memPre / 1048576).toFixed(1)} MiB; the sweep: ${needs} { need }, ${runs.n - runsReady} jobs`);
    proc.port.onmessage({ data: { dispose: true } });
}
{
    // The cache must not change the sound: the preloaded processor against one without, the same input and the same
    // dial path from the first quantum (no calibration: its burst is timed, so its quanta differ run to run). The quanta
    // compared are those after { preloaded }: while the set fills, the preloaded one may take a capture the law asks
    // for a quantum later than the plain one (a fill job ran in that quantum), and both have played the same input all along.
    const pack = realPack;
    const sweep = 300;
    const a = await newProcessor(w0, pack.dir, { preload: true, calibrate: false, maxModels: 2 });
    const b = await newProcessor(w0, pack.dir, { calibrate: false, maxModels: 2 });
    const ins = makeInput(Q * 6000, 48000, 0);
    const aL = new Float32Array(Q), aR = new Float32Array(Q), bL = new Float32Array(Q), bR = new Float32Array(Q);
    let q = 0, ready = false, worst = 0, compared = 0, needsB = 0;
    const path = [];
    for (let d = 0; d <= sweep; ++d) path.push(d);
    for (let d = sweep; d >= 0; d -= 2) path.push(d);
    let at = 0;
    for (; q < 6000 && at < path.length; ++q) {
        if (ready) { a.port.onmessage({ data: { set: { gain: path[at] } } }); b.port.onmessage({ data: { set: { gain: path[at] } } }); ++at; }
        const x = ins.subarray(q * Q, (q + 1) * Q);
        const mb = b.port.posted.length;
        a.process([[x]], [[aL, aR]], {});
        b.process([[x]], [[bL, bR]], {});
        answer(a, w0, pack.dir);
        answer(b, w0, pack.dir);
        if (ready) {
            needsB += after(b, mb, (m) => m.need !== undefined).length;
            for (let i = 0; i < Q; ++i) worst = Math.max(worst, Math.abs(aL[i] - bL[i]));
            compared += Q;
        }
        ready ||= !!a.port.posted.find((m) => m.preloaded);
    }
    ok(compared > 0 && worst === 0, `${pack.name} in wasm: the preloaded player bit for bit with the plain one over `
       + `${compared} samples of the sweep (max |diff| ${worst})`);
    console.log(`      (the plain one asked for ${needsB} { need } through the same sweep and ran its loads on the audio thread; `
                + `module memory: preloaded ${(a.m.HEAPU8.buffer.byteLength / 1048576).toFixed(1)} MiB, plain `
                + `${(b.m.HEAPU8.buffer.byteLength / 1048576).toFixed(1)} MiB)`);
    ok(needsB > 0, 'the control: the plain one did load as it went');
    a.port.onmessage({ data: { dispose: true } });
    b.port.onmessage({ data: { dispose: true } });
}

group('THE PRELOAD and the knobs: the set is made around them, the gain dial lands at once');
{
    // The knobs of processorOptions, and a { set } posted before the module is up, choose the panel: the set is made
    // around THAT panel before its first job (cap 1: red's two copies, not green's) — so after { preloaded } nothing
    // more is loaded, through the start and play.
    for (const how of ['knobs', 'early']) {
        const runs = { n: 0 };
        hooked((m) => { const run = m._fc_rig_job_run; m._fc_rig_job_run = (r) => { ++runs.n; return run(r); }; });
        const rig = w0.buffer(fs.readFileSync(path.join(fixtureDir, 'rig.json')));
        const red = { switch: { Channel: 'red' } };
        const proc = new w0.cls({ numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
                                  processorOptions: { module: w0.module, rig, preload: 1, ...(how === 'knobs' ? { knobs: red } : {}) } });
        if (how === 'early') proc.port.onmessage({ data: { set: red } });
        const up = await settle(proc, (m) => m.ready || m.error);
        ok(up && up.ready, `${how}: the processor came up`);
        let q = 0, pre;
        for (; q < 400 && !(pre = proc.port.posted.find((m) => m.preloaded)); ++q) play(proc, 1, () => answer(proc, w0, fixtureDir));
        const mark = proc.port.posted.length, runsReady = runs.n;
        for (let k = 0; k < 300 && !proc.port.posted.find((m) => m.calibrated); ++k) play(proc, 1, () => answer(proc, w0, fixtureDir));
        play(proc, 60, () => answer(proc, w0, fixtureDir));
        const needs = after(proc, mark, (m) => m.need !== undefined).length;
        const p = pre && pre.preloaded;
        ok(p && p.total === 2 && p.pack === proc.pack, `${how}: the set made around red, the panel asked for (${p ? p.total : '?'} `
           + `copies; { preloaded } names pack ${p ? p.pack : '?'})`);
        ok(needs === 0 && runs.n === runsReady && proc.m._fc_rig_preload_pending(proc.rig) === 0,
           `${how}: after { preloaded }, through the start and 60 quanta: ${needs} { need }, ${runs.n - runsReady} jobs`);
        proc.port.onmessage({ data: { dispose: true } });
    }
    // The gain dial addressed by its NAME (in `dial` or `switch`) while the set is prepared lands at once, as `gain`
    // does: the start is timed on the capture the page turned to (v0.7.0; it waited for the 12 o'clock one before).
    for (const key of ['dial', 'switch']) {
        const seen = { proc: null, calls: [] };
        hooked((m) => {
            const dial = m._fc_rig_set_dial, sw = m._fc_rig_set_switch;
            const note = (what) => seen.calls.push({ what, at: seen.proc ? seen.proc.port.posted.length : -1 });
            m._fc_rig_set_dial = (r, p, v) => { note('dial'); return dial(r, p, v); };
            m._fc_rig_set_switch = (r, p, v) => { note('switch'); return sw(r, p, v); };
        });
        const proc = await newProcessor(w0, fixtureDir, { preload: true });
        seen.proc = proc;
        proc.port.onmessage({ data: { set: { [key]: { Gain: key === 'dial' ? 30 : '60' } } } });
        let q = 0;
        for (; q < 400 && !proc.port.posted.find((m) => m.calibrated); ++q) play(proc, 1, () => answer(proc, w0, fixtureDir));
        const cal = proc.port.posted.findIndex((m) => m.calibrated);
        const early = seen.calls.filter((c) => c.at <= cal);
        ok(cal >= 0 && seen.calls.length > 0 && early.length === seen.calls.length,
           `{ set: { ${key}: { Gain } } } while preloading: applied at once, before { calibrated } (${early.length} call(s) before it, `
           + `${seen.calls.length} in all)`);
        proc.port.onmessage({ data: { dispose: true } });
    }
}

group('THE FILL AND PARK (v0.7.0): nothing loads under PARK — no job, no { need }; an answer that arrives meanwhile is kept '
      + 'and its job runs at the first quantum back; the fill resumes, and its quanta leave PARK out');
{
    const jobs = { n: 0 };
    hooked((m) => { const run = m._fc_rig_job_run; m._fc_rig_job_run = (r) => { ++jobs.n; return run(r); }; });
    const proc = await newProcessor(w0, fixtureDir, { preload: true, calibrate: false });
    const total = proc.m._fc_rig_preload_total(proc.rig);
    let mark0 = 3;
    play(proc, 3, () => answer(proc, w0, fixtureDir));          // a few copies in
    for (let k = 0; k < 10 && proc.awaiting === null; ++k, ++mark0) play(proc, 1);   // a { need } out, not answered yet
    const out = proc.awaiting;                                   // (a copy whose file the player holds needs none)
    proc.port.onmessage({ data: { standby: true } });
    const mark = proc.port.posted.length, jobsAt = jobs.n, pendingAt = proc.m._fc_rig_preload_pending(proc.rig);
    answer(proc, w0, fixtureDir);                                // its answer arrives while parked
    play(proc, 200, () => answer(proc, w0, fixtureDir));
    const needs = after(proc, mark, (m) => m.need !== undefined).length;
    const loaded = after(proc, mark, (m) => m.loaded !== undefined).length;
    ok(out !== null && pendingAt > 0 && needs === 0 && loaded === 0 && jobs.n === jobsAt
       && proc.m._fc_rig_preload_pending(proc.rig) === pendingAt,
       `parked mid-fill (${pendingAt} of ${total} copies to go, ${out} asked for): 200 quanta and its answer — ${needs} { need }, `
       + `${jobs.n - jobsAt} jobs run, ${loaded} { loaded }`);
    proc.port.onmessage({ data: { standby: false } });
    const back = proc.port.posted.length;
    play(proc, 1);                                               // the first quantum back runs the job it kept
    const first = after(proc, back, (m) => m.loaded !== undefined);
    ok(first.length === 1 && first[0].loaded === out && first[0].ok,
       `back: the first quantum runs the kept job (${JSON.stringify(first)})`);
    let q = 1;
    for (; q < 400 && !proc.port.posted.find((m) => m.preloaded); ++q) play(proc, 1, () => answer(proc, w0, fixtureDir));
    const p = (proc.port.posted.find((m) => m.preloaded) || {}).preloaded;
    const unparked = mark0 + q;
    ok(p && p.failed === 0 && p.total === total && p.quanta <= unparked,
       `…and the fill resumes: { preloaded } (${p ? `${p.total} copies, quanta ${p.quanta}` : 'none'}) — at most the ${unparked} `
       + 'quanta played unparked, not the 200 parked');
    proc.port.onmessage({ data: { dispose: true } });
}
{
    // A { load } before the set is filled ends that fill: no { preloaded } for it; the new pack's names the new pack.
    const proc = await newProcessor(w0, fixtureDir, { preload: true, calibrate: false });
    play(proc, 2, () => answer(proc, w0, fixtureDir));
    const was = proc.pack;
    proc.port.onmessage({ data: { load: w0.buffer(fs.readFileSync(path.join(fixtureDir, 'rig.json'))) } });
    for (let q = 0; q < 400 && !proc.port.posted.find((m) => m.preloaded); ++q) play(proc, 1, () => answer(proc, w0, fixtureDir));
    const said = proc.port.posted.filter((m) => m.preloaded).map((m) => m.preloaded);
    ok(said.length === 1 && proc.pack === was + 1 && said[0].pack === proc.pack && said[0].failed === 0,
       `a { load } mid-fill: one { preloaded }, for the new pack (${JSON.stringify(said)}; pack ${was} -> ${proc.pack})`);
    proc.port.onmessage({ data: { dispose: true } });
}
{
    // calibrate: false — no start to wait for, so a knob lands at once, the gain dial included, while the set fills.
    const calls = [];
    hooked((m) => { const g = m._fc_rig_set_gain_dial; m._fc_rig_set_gain_dial = (r, v) => { calls.push(v); return g(r, v); }; });
    const proc = await newProcessor(w0, fixtureDir, { preload: true, calibrate: false });
    proc.port.onmessage({ data: { set: { gain: 60 } } });
    const pending = proc.m._fc_rig_preload_pending(proc.rig);
    ok(calls.includes(60) && pending > 0, `calibrate: false, the set filling (${pending} to go): { set: { gain: 60 } } lands at once `
       + `(${JSON.stringify(calls)})`);
    proc.port.onmessage({ data: { dispose: true } });
}
// The start's capture failing ends the start at once, filling or not; a copy of the fill failing does not end it.
for (const [what, failAt] of [["the start's capture (the pack's first job, the law's)", 0], ['a copy of the fill (the second job)', 1]]) {
    const proc = await newProcessor(w0, fixtureDir, { preload: true });
    const junk = w0.buffer(Buffer.from('not a capture'));
    let k = 0;
    const override = () => (k++ === failAt ? junk : undefined);
    let cal;
    for (let q = 0; q < 400 && !(cal = proc.port.posted.find((m) => m.calibrated)); ++q) play(proc, 1, () => answer(proc, w0, fixtureDir, override));
    const bad = proc.port.posted.findIndex((m) => m.loaded !== undefined && m.ok === false);
    const at = proc.port.posted.findIndex((m) => m.calibrated);
    const c = cal && cal.calibrated;
    if (failAt === 0) ok(bad >= 0 && at === bad + 1 && c.reason === 'load-failed' && c.maxModels === 1,
                         `${what} fails: { calibrated } right after its { loaded } (${bad}, ${at}: ${JSON.stringify(c)})`);
    else ok(bad >= 0 && c && c.valid === true, `${what} fails: the start goes on and calibrates (${c ? `valid ${c.valid}` : 'no decision'})`);
    proc.port.onmessage({ data: { dispose: true } });
}

group('ABI 5 — the files up front: { files } with the pack posts no { need }; a file left out is still asked for');
{
    const ids = fs.readdirSync(fixtureDir).filter((f) => f !== 'rig.json');
    const files = (skip) => ids.filter((f) => f !== skip).map((f) => ({ file: f, bytes: w0.buffer(fs.readFileSync(path.join(fixtureDir, f))) }));
    const needsOf = (proc) => proc.port.posted.filter((m) => m.need !== undefined).map((m) => m.need);
    const all = await newProcessor(w0, fixtureDir, { preload: true, calibrate: false, files: files(null) });
    for (let q = 0; q < 200 && !all.port.posted.find((m) => m.preloaded); ++q) play(all, 1, () => answer(all, w0, fixtureDir));
    const p = (all.port.posted.find((m) => m.preloaded) || {}).preloaded;
    ok(p && p.failed === 0 && needsOf(all).length === 0,
       `processorOptions.files: the whole set preloaded (${p ? p.total : '?'} copies) with no { need } (${needsOf(all).length})`);
    all.port.onmessage({ data: { dispose: true } });
    const proc = await newProcessor(w0, null, { preload: true, calibrate: false });
    const left = ids.filter((f) => f.endsWith('.namz')).sort()[0];
    proc.port.onmessage({ data: { load: w0.buffer(fs.readFileSync(path.join(fixtureDir, 'rig.json'))), files: files(left) } });
    for (let q = 0; q < 200 && !proc.port.posted.find((m) => m.preloaded); ++q) play(proc, 1, () => answer(proc, w0, fixtureDir));
    const needs = needsOf(proc);
    ok(needs.length === 1 && needs[0] === left && proc.port.posted.find((m) => m.preloaded),
       `{ load, files } with ${left} left out: one { need }, for it (${JSON.stringify(needs)}), and the set preloaded`);
    proc.port.onmessage({ data: { dispose: true } });
    // A job already waiting for a file: { files } after the { need } answers it, and the late { file } is dropped.
    const late = await newProcessor(w0, null, { preload: true, calibrate: false });
    late.port.onmessage({ data: { load: w0.buffer(fs.readFileSync(path.join(fixtureDir, 'rig.json'))) } });
    for (let q = 0; q < 20 && needsOf(late).length === 0; ++q) play(late, 1, () => {});
    const first = needsOf(late)[0];
    late.port.onmessage({ data: { files: files(null), pack: late.pack } });
    const landed = late.port.posted.find((m) => m.loaded === first);
    ok(first !== undefined && landed && landed.ok, `a { need } out (${first}) answered by { files }: it loads (${landed ? landed.ok : 'no { loaded }'})`);
    late.port.onmessage({ data: { dispose: true } });
}

group('ABI 5 — the processor refuses a module older than itself');
{
    hooked((m) => { m._fc_rig_abi_version = () => 4; });
    const proc = new w0.cls({ numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2], processorOptions: { module: w0.module } });
    const said = await settle(proc, (m) => m.ready || m.error);
    ok(said && said.error && /ABI 4/.test(said.error) && /needs 5/.test(said.error),
       `a module that speaks ABI 4: { error } (${said ? JSON.stringify(said.error || said) : 'nothing'})`);
}

group('THE START WITHOUT A BURST (v0.7.0): PLAY opens after the dip and a few quanta (THE QUIET WARM), a channel change after '
      + 'the dip and the live warm-up, no hold, the DRIVE\'s own capture; the quiet warm bit-identical to the live warm-up');
{
    const zero = new Float32Array(Q), oL = new Float32Array(Q), oR = new Float32Array(Q);
    const ms = (q) => (q * 1000 * Q / 48000).toFixed(1) + ' ms';
    const rigOf = (pack) => w0.buffer(fs.readFileSync(path.join(pack.dir, 'rig.json')));
    for (const pack of packs.filter((p) => p.name !== 'fixture')) {
        // PLAY on silence: the quanta to the first one the player lets a capture be heard, and to { calibrated }; then
        // the riff, started on { calibrated } as the site's engine starts it. ON A CLOCK THE TEST OWNS (ownedClock, a
        // render 0.1 ms — what a site pack's capture costs in node on arm64): the quiet warm's budget is a bound on what a
        // render costs, so on the paced clock the quanta to the open were this machine's load — 19 to 26 quanta here,
        // alone and under a parallel ctest, against the gate's 24.
        const start = async (opts) => {
            const c = ownedClock();
            c.cost = 0.1;
            const proc = await newProcessor(w0, pack.dir, opts);
            const tick = (n) => { for (let k = 0; k < n; ++k) { c.quantum(); play(proc, 1, () => answer(proc, w0, pack.dir)); } };
            let q = 0, open = -1, cal = -1, longest = 0;
            for (; q < 400 && cal < 0; ++q) {
                const t = performance.now();
                c.quantum();
                proc.process([[zero]], [[oL, oR]], {});
                longest = Math.max(longest, performance.now() - t);
                if (open < 0 && proc.m._fc_rig_audible(proc.rig) === 1) open = q;
                if (cal < 0 && proc.port.posted.some((m) => m.calibrated)) cal = q;
                answer(proc, w0, pack.dir);
            }
            riffAt = 0;
            c.quantum();
            let peak = play(proc, 1, () => answer(proc, w0, pack.dir));
            c.quantum();
            peak = Math.max(peak, play(proc, 1, () => answer(proc, w0, pack.dir)));
            return { proc, open, cal, longest, peak, c, tick };
        };
        const quiet = await start({}), live = await start({ quietWarm: false });
        live.proc.port.onmessage({ data: { dispose: true } });
        live.c.restore();                                    // the clock is quiet's again, until quiet.c.restore()
        console.log(`      ${pack.name}: PLAY opens at quantum ${quiet.open} (${ms(quiet.open + 1)}), { calibrated } at ${quiet.cal}; `
                    + `without the quiet warm at ${live.open} (${ms(live.open + 1)}); the longest quantum ${quiet.longest.toFixed(2)} ms`);
        ok(quiet.open >= 0 && quiet.open <= 24 && quiet.cal === quiet.open && live.open > 2 * quiet.open,
           `${pack.name}: PLAY sounds after ${quiet.open + 1} quanta and says so in that quantum (${live.open + 1} without the quiet warm)`);
        ok(quiet.peak > 0.01, `${pack.name}: the riff started on { calibrated } is heard at once — nothing swallowed (peak ${quiet.peak.toFixed(3)})`);
        ok(quiet.longest < 10, `${pack.name}: no stall at the start (the longest quantum ${quiet.longest.toFixed(2)} ms)`);

        // A channel change mid-riff, the DRIVE far from 12 o'clock: the first capture loaded is the DRIVE's own (the one a
        // player without a start loads first), and the sound is back after the dip and the live warm-up, said in that quantum.
        const proc = quiet.proc;
        quiet.tick(200);
        const gain = 0.9 * proc.m._fc_rig_gain_dial_sweep(proc.rig);
        const ref = await newProcessor(w0, pack.dir, { calibrate: false, maxModels: proc.maxModels, knobs: { gain } });
        play(ref, 4, () => answer(ref, w0, pack.dir));
        const want = (ref.port.posted.find((m) => m.loaded !== undefined) || {}).loaded;
        ref.port.onmessage({ data: { dispose: true } });
        const mark = proc.port.posted.length;
        proc.port.onmessage({ data: { load: rigOf(pack), set: { gain } } });
        let q = 0, open = -1, cal = -1;
        for (; q < 400 && cal < 0; ++q) {
            quiet.tick(1);
            if (open < 0 && proc.m._fc_rig_audible(proc.rig) === 1) open = q;
            if (cal < 0 && after(proc, mark, (m) => m.calibrated).length > 0) cal = q;
        }
        const first = (after(proc, mark, (m) => m.loaded !== undefined)[0] || {}).loaded;
        console.log(`      ${pack.name}: a channel change mid-riff sounds again after ${open + 1} quanta (${ms(open + 1)}), first capture ${first}`);
        ok(first !== undefined && first === want, `${pack.name}: the first capture after the change is the DRIVE's own (${first}, want ${want})`);
        ok(open >= 40 && open <= 70 && cal === open, `${pack.name}: …back after the dip and the live warm-up (${open + 1} quanta), `
           + `{ calibrated } in that quantum (${cal + 1}) — no burst, no hold`);

        // PARK: a { load } while parked (the riff on), back on silence: open after a few quanta — PARK starts from silence.
        proc.port.onmessage({ data: { standby: true } });
        quiet.tick(60);
        proc.port.onmessage({ data: { load: rigOf(pack) } });
        quiet.tick(60);
        proc.port.onmessage({ data: { standby: false } });
        let back = 0;
        for (; back < 400 && proc.m._fc_rig_audible(proc.rig) !== 1; ++back) {
            quiet.c.quantum();
            proc.process([[zero]], [[oL, oR]], {});
            answer(proc, w0, pack.dir);
        }
        ok(back <= 24, `${pack.name}: a { load } under PARK, back on silence: sounding after ${back} quanta (${ms(back)})`);
        proc.port.onmessage({ data: { dispose: true } });
        quiet.c.restore();

        // THE QUIET WARM AGAINST THE LIVE WARM-UP, the real ABI through the worklet: 400 quanta of silence, then the riff.
        // From the riff on, every sample the same, bit for bit; the control (one input sample 1e-5 off) must differ.
        const R = 400, N = 1000;
        const run = async (quietWarm, maxModels, poke) => {
            const pr = await newProcessor(w0, pack.dir, { calibrate: false, quietWarm, maxModels, knobs: { gain: 0.7 * 300 } });
            const y = new Float32Array(N * Q);
            for (let k = 0; k < N; ++k) {
                const x = k < R ? zero : RIFF.slice((k - R) * Q, (k - R + 1) * Q);
                if (poke && k === R + 10) x[5] += 1e-5;
                pr.process([[x]], [[oL, oR]], {});
                y.set(oL, k * Q);
                answer(pr, w0, pack.dir);
            }
            pr.port.onmessage({ data: { dispose: true } });
            return new Uint8Array(y.buffer, R * Q * 4);
        };
        for (const maxModels of [1, 2]) {
            const a = await run(false, maxModels), b = await run(true, maxModels), c = await run(true, maxModels, true);
            ok(Buffer.compare(a, b) === 0 && Buffer.compare(a, c) !== 0,
               `${pack.name}, maxModels ${maxModels}: the riff after the quiet warm is the riff after the live warm-up, bit for bit `
               + `(${a.length / 4} samples); the control differs`);
        }
    }
    // The quiet warm and a job never share a quantum: a quantum that warmed runs no job (the fill waits one quantum).
    const seen = { warmed: false, both: 0, quanta: 0 };
    hooked((m) => {
        const process = m._fc_rig_process, warm = m._fc_rig_warm_quiet, run = m._fc_rig_job_run;
        m._fc_rig_process = (...a) => { seen.warmed = false; return process(...a); };
        m._fc_rig_warm_quiet = (r, n) => { const k = warm(r, n); if (k > 0) { seen.warmed = true; ++seen.quanta; } return k; };
        m._fc_rig_job_run = (r) => { if (seen.warmed) ++seen.both; return run(r); };
    });
    const proc = await newProcessor(w0, realPack.dir, { preload: true });
    for (let k = 0; k < 300; ++k) { proc.process([[zero]], [[oL, oR]], {}); answer(proc, w0, realPack.dir); }
    ok(seen.quanta > 0 && seen.both === 0, `${realPack.name}, the fill on: ${seen.quanta} quanta warmed ahead, a job in ${seen.both} of them`);
    proc.port.onmessage({ data: { dispose: true } });
}

group('THE START, reviewed (v0.7.0): the quiet warm\'s budget is a bound on a render\'s cost and the architecture\'s own; a '
      + 'decision on a start\'s few readings is settled for the next start; { calibrated } a bounded time after the open; '
      + 'a quantum that warmed runs no job, an answer\'s included');
{
    const zero = new Float32Array(Q), oL = new Float32Array(Q), oR = new Float32Array(Q);
    const QMS = 1000 * Q / 48000, SHARE = 0.6;   // the quantum in ms; WARM_SHARE
    const rigOf = (dir) => w0.buffer(fs.readFileSync(path.join(dir, 'rig.json')));
    // A CLOCK THE TEST OWNS (it replaces the paced one): Date.now is floor(now); a quantum moves now by its length, a
    // render by `cost` ms. A cost under the phase's room reads 0 every time — the case the bare mean took for nothing.
    const own = (spy) => {
        const c = { now: 1.7e12, cost: 0, restore: () => {} };
        hooked((m) => {
            const D = vm.runInContext('Date', w0.ctx), real = D.now, process = m._fc_rig_process;
            D.now = () => Math.floor(c.now);
            m._fc_rig_process = (...a) => { const r = process(...a); c.now += c.cost; return r; };
            c.restore = () => { D.now = real; };
            if (spy) spy(m);
        });
        return c;
    };
    const quantum = (proc, c, dir, x = zero) => { c.now += QMS; proc.process([[x]], [[oL, oR]], {}); answer(proc, w0, dir); };
    const untilCal = (proc, c, dir, x) => {
        const mark = proc.port.posted.length;
        let q = 0, open = -1, cal = -1;
        for (; q < 600 && cal < 0; ++q) {
            quantum(proc, c, dir, x && x(q));
            if (open < 0 && proc.m._fc_rig_audible(proc.rig) === 1) open = q;
            if (after(proc, mark, (m) => m.calibrated).length > 0) cal = q;
        }
        return { open, cal, said: (after(proc, mark, (m) => m.calibrated)[0] || {}).calibrated };
    };

    // 1. Every reading 0 ms, a render really 0.3 ms: the budget must keep a render and its blocks within WARM_SHARE.
    {
        const ran = [];
        const c = own((m) => { const warm = m._fc_rig_warm_quiet; m._fc_rig_warm_quiet = (r, n) => { const k = warm(r, n); ran.push(k); return k; }; });
        c.cost = 0.3;
        const proc = await newProcessor(w0, realPack.dir, {});
        const r = untilCal(proc, c, realPack.dir);
        c.restore();
        const most = Math.max(0, ...ran);
        ok(r.said && (most + 1) * c.cost <= SHARE * QMS,
           `every reading 0 ms, a render 0.3 ms: at most ${most} blocks of silence a quantum — ${((most + 1) * c.cost).toFixed(1)} ms `
           + `with the render, within ${(SHARE * QMS).toFixed(1)} (the bare mean allowed 6: 2.1 ms)`);
        proc.port.onmessage({ data: { dispose: true } });
    }
    // 2. A slow architecture, a fast one, the slow one again: its start warms by its own budget, not the last one timed.
    {
        let ran = 0, track = false;
        const c = own((m) => { const warm = m._fc_rig_warm_quiet; m._fc_rig_warm_quiet = (r, n) => { const k = warm(r, n); if (track) ran = Math.max(ran, k); return k; }; });
        c.cost = 1.0;
        let dir = realPack.dir;
        const proc = await newProcessor(w0, dir, { knobs: { gain: 150 } });
        const a = untilCal(proc, c, dir);
        for (let q = 0; q < 300; ++q) quantum(proc, c, dir);
        c.cost = 0.3;
        dir = fixtureDir;
        proc.port.onmessage({ data: { load: rigOf(dir), set: { gain: 60 } } });
        const b = untilCal(proc, c, dir);
        for (let q = 0; q < 100; ++q) quantum(proc, c, dir);
        c.cost = 1.0;
        dir = realPack.dir;
        track = true;
        proc.port.onmessage({ data: { load: rigOf(dir), set: { gain: 150 } } });
        const again = untilCal(proc, c, dir);
        c.restore();
        ok(a.said && a.said.maxModels === 1 && b.said && again.said && (ran + 1) * 1.0 <= SHARE * QMS,
           `${realPack.name} at 1 ms a render, the fixture's other architecture fast, then ${realPack.name} again: at most ${ran} `
           + `blocks of silence a quantum (${(ran + 1).toFixed(0)} ms with the render — the fast one's budget was 6)`);
        proc.port.onmessage({ data: { dispose: true } });
    }
    // 3. THE SETTLE: the start's readings slow (a cold module), the renders after it fast — the next start says the blend;
    //    nothing changes while the first pack plays.
    {
        const max = [];
        const c = own((m) => { const set = m._fc_rig_set_max_models; m._fc_rig_set_max_models = (r, n) => { max.push(n); return set(r, n); }; });
        c.cost = 1.0;
        const proc = await newProcessor(w0, realPack.dir, { knobs: { gain: 150 } });
        const first = untilCal(proc, c, realPack.dir);
        c.cost = 0.3;
        const mark = max.length;
        for (let q = 0; q < 400; ++q) quantum(proc, c, realPack.dir, RIFF.subarray((q % 4000) * Q, (q % 4000 + 1) * Q));
        const during = max.slice(mark);
        proc.port.onmessage({ data: { load: rigOf(realPack.dir), set: { gain: 200 } } });
        const next = untilCal(proc, c, realPack.dir);
        c.restore();
        ok(first.said && first.said.maxModels === 1 && first.said.share > 0.25,
           `the first start read slow: one model (${first.said ? (100 * first.said.share).toFixed(0) : '?'} % on ${first.said ? first.said.renders : '?'} renders)`);
        ok(during.length === 0, `nothing changed while that pack played (${JSON.stringify(during)})`);
        ok(next.said && next.said.maxModels === 2 && next.said.renders >= 64 && next.cal === next.open,
           `the next start of the architecture says the settled decision at its open: maxModels ${next.said ? next.said.maxModels : '?'} on `
           + `${next.said ? next.said.renders : '?'} renders timed after an open`);
        proc.port.onmessage({ data: { dispose: true } });
    }
    // 4. Renders that never run one network alone — an outgoing capture draining through the whole start (a DRIVE turn
    //    while it times), stood in for by the counters — cannot be timed: { calibrated } still comes CAL_GRACE quanta
    //    after the open at most ('short': one model, settled later), not CAL_AGAIN.
    {
        const c = own((m) => { m._fc_rig_run_blocks = () => 1; });
        const proc = await newProcessor(w0, realPack.dir, { knobs: { gain: 150 } });
        const r = untilCal(proc, c, realPack.dir);
        c.restore();
        console.log(`      nothing timable: open at quantum ${r.open}, { calibrated } at ${r.cal} (${r.said ? r.said.reason : '?'})`);
        ok(r.open >= 0 && r.cal - r.open <= 8 && r.said && r.said.reason === 'short' && r.said.maxModels === 1,
           `nothing timable: { calibrated } ${r.cal - r.open} quanta after the open (at most CAL_GRACE = 8), 'short', one model`);
        proc.port.onmessage({ data: { dispose: true } });
    }
    //    …and { calibrate: 'again' } while TWO models sound: the neighbour's drain makes pairs for ~50 quanta, and the
    //    page's own request is answered on CAL_AGAIN renders all the same, not on what fit in CAL_AGAIN quanta.
    {
        const c = own();
        c.cost = 0.3;
        const proc = await newProcessor(w0, realPack.dir, { knobs: { gain: 165 } });
        const riff = (q) => RIFF.subarray((q % 4000) * Q, (q % 4000 + 1) * Q);
        const first = untilCal(proc, c, realPack.dir, riff);
        for (let q = 0; q < 300; ++q) quantum(proc, c, realPack.dir, riff(q));
        proc.port.onmessage({ data: { calibrate: 'again' } });
        const again = untilCal(proc, c, realPack.dir, riff);
        c.restore();
        ok(first.said && first.said.maxModels === 2 && again.said && again.said.valid && again.said.renders >= 64,
           `{ calibrate: 'again' } while two models sound: decided on ${again.said ? again.said.renders : '?'} renders (CAL_AGAIN = 64)`);
        proc.port.onmessage({ data: { dispose: true } });
    }
    // 5. A quantum that warmed runs no job — not from pump, and not from an answer that arrives after it (the handler).
    {
        const seen = { warmed: false, both: 0, runs: 0 };
        let fake = false;
        hooked((m) => {
            const process = m._fc_rig_process, warm = m._fc_rig_warm_quiet, run = m._fc_rig_job_run;
            m._fc_rig_process = (...a) => { seen.warmed = false; return process(...a); };
            m._fc_rig_warm_quiet = (r, n) => { const k = fake ? 1 : warm(r, n); if (k > 0) seen.warmed = true; return k; };
            m._fc_rig_job_run = (r) => { if (seen.warmed) ++seen.both; ++seen.runs; return run(r); };
        });
        const proc = await newProcessor(w0, realPack.dir, {});
        for (let q = 0; q < 10 && proc.awaiting === null; ++q) proc.process([[zero]], [[oL, oR]], {});   // the start's { need }, unanswered
        const pending = proc.awaiting !== null;
        fake = true;                                   // this quantum warmed (the warm's return stood in for)…
        proc.process([[zero]], [[oL, oR]], {});
        answer(proc, w0, realPack.dir);                // …and the answer arrives after it, in the same period
        const inWarmed = seen.runs;
        fake = false;
        proc.process([[zero]], [[oL, oR]], {});
        ok(pending && seen.both === 0 && inWarmed === 0 && seen.runs === 1,
           `an answer after a quantum that warmed: its job waits for the next quantum (${seen.both} run in the warmed one, ${seen.runs} after)`);
        proc.port.onmessage({ data: { dispose: true } });
    }
}

group('ABI 5 — the files before the module is up: a stale { pack } is dropped, a failed setup keeps none');
{
    const rigBytes = () => w0.buffer(fs.readFileSync(path.join(fixtureDir, 'rig.json')));
    const x = fs.readdirSync(fixtureDir).filter((f) => f !== 'rig.json').sort()[0];
    const proc = new w0.cls({ numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
                              processorOptions: { module: w0.module, preload: true, calibrate: false } });
    proc.port.onmessage({ data: { load: rigBytes() } });
    proc.port.onmessage({ data: { files: [{ file: x, bytes: realBytes(fixtureDir, x) }], pack: 999 } });   // no such pack yet
    await settle(proc, (m) => m.ready || m.error);
    for (let q = 0; q < 200 && !proc.port.posted.find((m) => m.preloaded); ++q) play(proc, 1, () => answer(proc, w0, fixtureDir));
    const needs = proc.port.posted.filter((m) => m.need !== undefined).map((m) => m.need);
    ok(needs.includes(x), `{ files, pack: 999 } before the start is dropped: ${x} is still asked for (${JSON.stringify(needs)})`);
    proc.port.onmessage({ data: { dispose: true } });

    hooked((m) => { m._fc_rig_abi_version = () => 4; });
    const old = new w0.cls({ numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
                             processorOptions: { module: w0.module, rig: rigBytes(), files: [{ file: x, bytes: realBytes(fixtureDir, x) }] } });
    const said = await settle(old, (m) => m.ready || m.error);
    ok(said && said.error && old.firstFiles === null && old.firstRig === null,
       `a setup that failed ({ error }) keeps no files (firstFiles ${old.firstFiles === null ? 'null' : 'still set'})`);
}

group('ABI 5 — fc_rig_put_file allocates only for a file the pack still wants');
{
    const proc = await newProcessor(w0, fixtureDir, { calibrate: false });
    play(proc, 8, () => answer(proc, w0, fixtureDir));
    const m = proc.m, rig = proc.rig;
    const cname = (s) => { const b = Buffer.from(s + '\0', 'utf8'); const p = m._fc_rig_alloc(b.length); m.HEAPU8.set(b, p); return p; };
    const held = proc.port.posted.find((x) => x.loaded !== undefined && x.ok);
    for (const [what, id] of [['an id the pack does not name', 'no-such-file.namz'], ['a file already held', held && held.loaded]]) {
        const name = cname(id);
        const before = m.HEAPU8.buffer.byteLength;
        const p = m._fc_rig_put_file(rig, name, 64 << 20);
        const grew = m.HEAPU8.buffer.byteLength - before;
        m._fc_rig_free(name);
        ok(id && p === 0 && grew === 0, `${what} (${id}), 64 MiB: NULL (${p}) and the memory did not grow (${grew} bytes)`);
    }
    proc.port.onmessage({ data: { dispose: true } });
}

fs.rmSync(fixtureDir, { recursive: true, force: true });
console.log(`\n${checks} checks, ${failures} failures\n${failures ? 'TESTS FAILED' : 'ALL TESTS PASSED'}`);
process.exit(failures ? 1 : 0);
