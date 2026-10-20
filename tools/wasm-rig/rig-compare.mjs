// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-core — see LICENSE.
//
// rig-compare — two fcrig builds through the real ABI, session by session: the previous release against this one.
//
//   node --experimental-vm-modules tools/wasm-rig/rig-compare.mjs <old-build-dir> <new-build-dir> <pack.orbitrig>...
//
// A build dir is what build.sh leaves (fcrig.mjs, fcrig.wasm, fcrig-worklet.js); each module is loaded the way its
// own worklet loads it, in a simulated AudioWorkletGlobalScope, and then driven straight through its fc_rig_* entry
// points — so what is compared is the module, not either worklet.
//
// 1. PARITY: per pack, a mono 48 kHz session of 6 s — the pack loaded with the preload off and on (the whole pack),
//    each with the blend (two models), the dip (one), PARK every 300 quanta, and the dip under PARK: 8 sessions a
//    pack, the gain dial walked over 13 positions, every job answered with the file's bytes — and 4 more at a
//    44.1 kHz host (the rate-matcher in; WebKit's common rate): preload off and on, the blend and the dip. Every output
//    sample of the new build must equal the old one's, BIT for bit (compared as integers: -0 is not +0), and none
//    may be NaN or infinite.
// 2. THE CONTROL: the same session with every capture of the pack one ulp off in one weight (the last float of the
//    payload — the full-size submodel's) on the NEW side must NOT be identical — the comparison can say no.
// 3. NUMBERS, both builds, each on a fresh module: the whole-pack preload's compute (the sum of fc_rig_job_run), the
//    slowest job, and what the module's memory grew to; for the new one also the whole preload with the files handed in up front
//    (fc_rig_put_file, ABI 5): how many jobs still needed bytes (0), and with one file left out (1).
// Exit 0 when every parity session is identical and every control differs.
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { performance } from 'node:perf_hooks';

const [oldDir, newDir, ...packs] = process.argv.slice(2);
if (!oldDir || !newDir || packs.length === 0) {
    console.error('usage: rig-compare.mjs <old-build-dir> <new-build-dir> <pack.orbitrig>...');
    process.exit(2);
}
const SR = 48000, MB = 128, WHOLE = 0x7fffffff;

class AudioWorkletProcessor {
    constructor() { this.port = { onmessage: null, posted: [], postMessage(m) { this.posted.push(m); }, close() {} }; }
}

/** The build's module, instantiated by its own worklet in a simulated worklet scope. */
async function loadModule(dir) {
    let registered = null, module = null;
    const ctx = vm.createContext({ console, AudioWorkletProcessor, sampleRate: SR, currentFrame: 0, currentTime: 0,
                                   registerProcessor: (name, cls) => { registered = cls; } });
    vm.runInContext('globalThis.AudioWorkletGlobalScope = function AudioWorkletGlobalScope() {};', ctx);
    ctx.__onRigModule = (m) => { module = m; };
    const meta = (url) => ({ initializeImportMeta(m) { m.url = url; } });
    const glue = new vm.SourceTextModule(fs.readFileSync(path.join(dir, 'fcrig.mjs'), 'utf8'),
                                         { context: ctx, ...meta('https://worklet.invalid/fcrig.mjs') });
    const spy = new vm.SourceTextModule("import create from './glue.mjs';\nexport default async function (a) { const m = await create(a); globalThis.__onRigModule(m); return m; }\n",
                                        { context: ctx });
    const worklet = new vm.SourceTextModule(fs.readFileSync(path.join(dir, 'fcrig-worklet.js'), 'utf8'),
                                            { context: ctx, ...meta('https://worklet.invalid/fcrig-worklet.js') });
    await worklet.link((spec, ref) => (spec === './fcrig.mjs' && ref === worklet ? spy : glue));
    await worklet.evaluate();
    const wasm = await vm.runInContext('WebAssembly', ctx).compile(fs.readFileSync(path.join(dir, 'fcrig.wasm')));
    const proc = new registered({ numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
                                  processorOptions: { module: wasm, calibrate: false } });
    for (let i = 0; i < 2000 && !module; ++i) await new Promise((r) => setImmediate(r));
    if (!module) throw new Error(`${dir}: the module did not come up (${JSON.stringify(proc.port.posted)})`);
    return module;
}

function readPack(dir) {
    const manifest = fs.readFileSync(path.join(dir, 'rig.json'));
    const files = {};
    for (const f of fs.readdirSync(dir)) if (f !== 'rig.json') files[f] = fs.readFileSync(path.join(dir, f));
    return { name: path.basename(dir), manifest, files };
}

/** Every .namz one ulp off in its payload's last float (the low bit of its least significant byte). */
function perturbed(files) {
    const out = {};
    for (const [id, b0] of Object.entries(files)) {
        const b = Buffer.from(b0);
        if (id.endsWith('.namz') && b.length > 16 && b.toString('latin1', 0, 4) === 'NAMZ') {
            let p = b[4] >= 2 ? 10 + b.readUInt16LE(8) : 8;
            p += 4 + b.readUInt32LE(p);
            const segments = b.readUInt32LE(p);
            p += 4 + 4 * segments;
            const floats = (b.length - p) / 4;
            b[(b[7] & 1) ? p + floats - 1 : p + 4 * (floats - 1)] ^= 1;   // byte-shuffled: plane 0 is the low bytes
        }
        out[id] = b;
    }
    return out;
}

const str = (m, p, n) => Buffer.from(m.HEAPU8.subarray(p, p + n)).toString('utf8');
function cstr(m, s) { const b = Buffer.from(s + '\0', 'utf8'); const p = m._fc_rig_alloc(b.length); m.HEAPU8.set(b, p); return p; }

/** One job, if the player hands one out: bytes in when it needs them, run (timed), delivered. */
function pump(m, rig, files, t) {
    if (m._fc_rig_take_job(rig) !== 1) return false;
    const id = str(m, m._fc_rig_job_file(rig), m._fc_rig_job_file_length(rig));
    if (m._fc_rig_job_needs_bytes(rig) === 1) {
        ++t.needs;
        const b = files[id];
        const q = b ? m._fc_rig_job_bytes(rig, b.length) : 0;
        if (q) m.HEAPU8.set(b, q);                      // HEAPU8 read after the call: it may have grown the memory
    }
    const a = performance.now();
    m._fc_rig_job_run(rig);
    const ms = performance.now() - a;
    t.run += ms; t.max = Math.max(t.max, ms); ++t.jobs;
    m._fc_rig_job_deliver(rig);
    return true;
}

function open(m, pack, files, preload, opt = {}) {
    const rig = m._fc_rig_create();
    if (!m._fc_rig_prepare(rig, opt.sr || SR, MB, 1)) throw new Error('prepare');
    if (m._fc_rig_set_preload) m._fc_rig_set_preload(rig, preload);
    if (opt.maxModels) m._fc_rig_set_max_models(rig, opt.maxModels);
    const p = m._fc_rig_alloc(pack.manifest.length);
    m.HEAPU8.set(pack.manifest, p);
    if (!m._fc_rig_load(rig, p, pack.manifest.length)) throw new Error('load');
    m._fc_rig_free(p);
    const t = { run: 0, max: 0, jobs: 0, needs: 0 };
    if (opt.upFront) for (const [id, b] of Object.entries(files)) {
        if (id === opt.leaveOut) continue;
        const name = cstr(m, id);
        const q = m._fc_rig_put_file(rig, name, b.length);
        if (q) m.HEAPU8.set(b, q);
        m._fc_rig_free(name);
    }
    if (preload) for (let g = 0; g < 100000 && (pump(m, rig, files, t) || m._fc_rig_preload_pending(rig) > 0); ++g);
    return { rig, t };
}

function signal(n) {
    const x = new Float32Array(n);
    let s = 777;
    const notes = [82.41, 110, 146.83, 196, 246.94, 329.63];
    for (let i = 0; i < n; ++i) {
        s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0;
        const t = (i % 12000) / SR, f = notes[Math.floor(i / 12000) % notes.length];
        let v = 0;
        for (let h = 1; h <= 6; ++h) v += Math.sin(2 * Math.PI * f * h * t) / h;
        x[i] = 0.25 * v * Math.exp(-3 * t) + 0.02 * (s / 4294967296 - 0.5);
    }
    return x;
}
const X = signal(SR * 6);

function session(m, pack, files, preload, opt) {
    const { rig } = open(m, pack, files, preload, opt);
    const t = { run: 0, max: 0, jobs: 0, needs: 0 };
    const sweep = m._fc_rig_gain_dial_sweep(rig), q = Math.floor(X.length / MB), step = Math.floor(q / 13);
    const y = new Float32Array(X.length);
    for (let i = 0; i < q; ++i) {
        if (i % step === 0) m._fc_rig_set_gain_dial(rig, sweep * ((i / step) % 13) / 12);
        if (opt.park && i % 300 === 150) m._fc_rig_set_standby(rig, 1);
        if (opt.park && i % 300 === 200) m._fc_rig_set_standby(rig, 0);
        const L = m._fc_rig_buffer(rig, 0);
        m.HEAPF32.set(X.subarray(i * MB, (i + 1) * MB), L >> 2);
        m._fc_rig_process(rig, L, 0, MB);
        const o = m._fc_rig_buffer(rig, 0) >> 2;
        y.set(m.HEAPF32.subarray(o, o + MB), i * MB);
        pump(m, rig, files, t);
    }
    m._fc_rig_destroy(rig);
    return y;
}

/** Bit for bit: the samples compared as their integers, so -0 differs from +0; a NaN or an infinity anywhere fails. */
function diff(a, b) {
    const ua = new Uint32Array(a.buffer, a.byteOffset, a.length), ub = new Uint32Array(b.buffer, b.byteOffset, b.length);
    let n = 0, first = -1, peak = 0, nonFinite = 0;
    for (let i = 0; i < a.length; ++i) {
        peak = Math.max(peak, Math.abs(a[i]));
        if (!Number.isFinite(a[i]) || !Number.isFinite(b[i])) ++nonFinite;
        if (ua[i] !== ub[i]) { if (first < 0) first = i; ++n; }
    }
    return { identical: n === 0 && nonFinite === 0 && a.length === b.length, mismatches: n, first, peak, nonFinite };
}

const OPTS = { blend: {}, dip: { maxModels: 1 }, park: { park: true }, dip_park: { maxModels: 1, park: true } };
const oldM = await loadModule(oldDir), newM = await loadModule(newDir);
console.log(`old: ABI ${oldM._fc_rig_abi_version()} (${oldDir})\nnew: ABI ${newM._fc_rig_abi_version()} (${newDir})`);
let failures = 0, sessions = 0, controls = 0;
// The comparison's own controls: it must say no to a sign of zero and to a NaN on both sides.
for (const [what, a, b] of [['+0 against -0', 0, -0], ['NaN against NaN', NaN, NaN], ['Infinity against Infinity', Infinity, Infinity]]) {
    const d = diff(Float32Array.of(1, a), Float32Array.of(1, b));
    if (d.identical) ++failures;
    console.log(`${d.identical ? 'FAIL' : 'ok  '} diff() refuses ${what}`);
}
const numbers = {};
for (const dir of packs) {
    const pack = readPack(dir);
    console.log(`\n== ${pack.name}`);
    for (const pre of [0, WHOLE])
        for (const [k, opt] of Object.entries(OPTS)) {
            const d = diff(session(oldM, pack, pack.files, pre, opt), session(newM, pack, pack.files, pre, opt));
            ++sessions;
            if (!d.identical) ++failures;
            console.log(`  ${d.identical ? 'ok  ' : 'FAIL'} preload ${pre ? 'on ' : 'off'} ${k.padEnd(8)} `
                        + `${d.identical ? 'identical' : `${d.mismatches} samples differ from ${d.first}, ${d.nonFinite} not finite`} (peak ${d.peak.toFixed(3)})`);
        }
    for (const pre of [0, WHOLE])
        for (const k of ['blend', 'dip']) {
            const opt = { ...OPTS[k], sr: 44100 };
            const d = diff(session(oldM, pack, pack.files, pre, opt), session(newM, pack, pack.files, pre, opt));
            ++sessions;
            if (!d.identical) ++failures;
            console.log(`  ${d.identical ? 'ok  ' : 'FAIL'} 44.1 kHz preload ${pre ? 'on ' : 'off'} ${k.padEnd(8)} `
                        + `${d.identical ? 'identical' : `${d.mismatches} samples differ from ${d.first}, ${d.nonFinite} not finite`} (peak ${d.peak.toFixed(3)})`);
        }
    const bad = perturbed(pack.files);
    const c = diff(session(oldM, pack, pack.files, 0, {}), session(newM, pack, bad, 0, {}));
    ++controls;
    if (c.identical) ++failures;
    console.log(`  ${c.identical ? 'FAIL' : 'ok  '} control: one weight one ulp off — ${c.mismatches} samples differ`);
    const row = {};
    for (const [side, d] of [['old', oldDir], ['new', newDir]]) {
        const m = await loadModule(d);                     // a fresh module: wasm memory never shrinks
        const mem0 = m.HEAPU8.length;
        const { rig, t } = open(m, pack, pack.files, WHOLE);
        row[side] = { copies: m._fc_rig_preload_total(rig), jobs: t.jobs, wholePackMs: +t.run.toFixed(2),
                      perCopyMs: +(t.run / Math.max(1, t.jobs)).toFixed(3), slowestMs: +t.max.toFixed(2),
                      moduleMiB: +(m.HEAPU8.length / 1048576).toFixed(2), grewMiB: +((m.HEAPU8.length - mem0) / 1048576).toFixed(2) };
        m._fc_rig_destroy(rig);
    }
    const up = open(newM, pack, pack.files, WHOLE, { upFront: true });
    newM._fc_rig_destroy(up.rig);
    const ids = Object.keys(pack.files).filter((f) => f.endsWith('.namz')).sort();
    const left = open(newM, pack, pack.files, WHOLE, { upFront: true, leaveOut: ids[0] });
    newM._fc_rig_destroy(left.rig);
    row.upFront = { needs: up.t.needs, needsWithOneLeftOut: left.t.needs, jobs: up.t.jobs };
    if (up.t.needs !== 0 || left.t.needs !== 1) ++failures;
    console.log(`  ${up.t.needs === 0 && left.t.needs === 1 ? 'ok  ' : 'FAIL'} files up front: ${up.t.needs} needs `
                + `(${left.t.needs} with ${ids[0]} left out), ${up.t.jobs} jobs`);
    numbers[pack.name] = row;
    console.log('  ' + JSON.stringify(row));
}
console.log(`\n${sessions} parity sessions, ${controls} controls, ${failures} failures`);
process.exit(failures ? 1 : 0);
