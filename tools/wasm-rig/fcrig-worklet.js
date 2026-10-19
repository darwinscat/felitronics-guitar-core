// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// The AudioWorklet processor of fcrig: one device of a .orbitrig pack, playing (tools/fc_rig_abi.h over
// felitronics::rigplayer) on the audio thread. Registered as 'fc-rig'. Mono by default — a guitar: channel 0 of the
// input is played and the result goes to every output channel; processorOptions.stereo plays two (two networks per
// sounding capture). No input is silence.
//
// THE PAGE COMPILES, THE PROCESSOR INSTANTIATES (as fcguitarfx: tools/wasm/README.md) — and THE PAGE FETCHES. A
// worklet has no fetch, so the captures come over the port: the player asks for a file (a load JOB), the processor
// posts { need: <files[].id>, pack: <n> }, the page fetches <pack dir>/<id> and answers { file, pack, bytes }
// (an ArrayBuffer, transferred; null when it could not), and the processor hands the bytes in, runs the job and
// delivers it, posting { loaded: <id>, ok }. A capture is fetched once: the player keeps its bytes (an answer of
// null is not kept — the file is asked for again on its next visit).
//
// EVERY { need } MUST BE ANSWERED, a failure with null: the player hands out one job at a time, so a { need } left
// unanswered stops every later load. The page's handler therefore catches the fetch AND the body (a body cut off
// after the headers rejects arrayBuffer()) and always posts. Should it still not answer, the processor answers for
// it after processorOptions.needTimeoutMs (NEED_MS by default; 0 = never) with nothing — { loaded, ok: false } — and
// drops the late answer; { load } is the other recovery: it drops the job out.
//
//   const module = await WebAssembly.compileStreaming(fetch(wasmUrl));
//   await context.audioWorklet.addModule(workletUrl);       // this file; it imports ./fcrig.mjs statically
//   const node = new AudioWorkletNode(context, 'fc-rig', {
//       numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
//       channelCount: 1, channelCountMode: 'explicit',       // Web Audio folds a stereo source to the one channel
//       processorOptions: { module, rig: await (await fetch(packDir + 'rig.json')).arrayBuffer() } });
//   node.port.onmessage = async ({ data }) => {
//       if (data.need !== undefined) {
//           let bytes = null;
//           try {
//               const r = await fetch(packDir + encodeURIComponent(data.need));
//               if (r.ok) bytes = await r.arrayBuffer();
//           } catch (e) { bytes = null; }                     // whatever failed, the { need } is answered
//           node.port.postMessage({ file: data.need, pack: data.pack, bytes }, bytes ? [bytes] : []);
//       }
//       // { ready, abi }  { calibrated: { renderUs, quantumUs, share, maxModels, renders, elapsedMs, valid, reason } }
//       // { loaded, ok }  { pack: n, ok, sweep }  { state: {…} }  { error }
//   };
//   node.port.postMessage({ set: { gain: 210, dial: { Bass: 120 }, switch: { Bright: 'Center' } } });
//
// THE START, AND THE DECISION IT MAKES — AGAIN AT EVERY PACK. A browser's audio thread may afford two networks or
// only one, and the only honest way to know is to time one on it. So, unless processorOptions.calibrate is false: at
// the start, and again at every { load } (another pack's captures may be another architecture, another cost), the
// player is told ONE model (setMaxModels(1)) before the pack loads, the gain dial goes to the middle of its sweep (150
// of 300, 12 o'clock), and once that capture is loaded, warmed and running alone, a SILENT CALIBRATION BURST renders
// quanta of silence back to back until Date.now() has moved at least CAL_MS (a worklet has no performance.now(),
// and Date.now's coarse tick biases a count of quanta between two ticks; a burst that long is within a few percent).
// The burst is VALID when the player's counters say every render ran exactly one network, warm, and Date.now moved
// at least CAL_FLOOR_MS. Valid, and one render at most BLEND_SHARE of the quantum (a quarter: ~667 us at 48 kHz) ->
// setMaxModels(2), the blend; anything else -> it stays at one capture with dips. A capture at 12 o'clock that fails to
// load ends the start too, at one model ({ calibrated: { maxModels: 1, valid: false, reason: 'load-failed' } }). The
// output is silent from the start (or the { load }) until that decision (the burst feeds the models silence), the
// decision and the measured cost are posted ({ calibrated }), and knobs that arrived meanwhile land after it, in
// order. With calibrate: false the player starts at processorOptions.maxModels (2 if absent) and the knobs land at
// once.
//
// THE BURST STALLS THE WHOLE CONTEXT ONCE. It runs inside one process() call: ~CAL_MS, plus up to two ticks of
// Date.now (the wait for the first tick and the last one) — at 48 kHz and 128 frames about a dozen quanta late, and
// every other node of the AudioContext drops out with it. Where Date.now is coarsened (Firefox's
// resistFingerprinting: 100 ms) the ticks are that long. Accepted, as the load stall below is; { measure } is the
// same burst on demand.
//
// AN ERROR IS NOT AN END. A setup or runtime error — no module (the bytes retry included: { error, retry:
// 'wasmBytes' }, as fcguitarfx), a module older than this processor, a refused prepare, a render quantum of another
// size than the one prepared, a trap inside the module — posts ONE { error }, and from then on the input passes
// through dry and process() keeps answering true. ONLY { dispose } ENDS IT. After a trap the instance is not freed:
// the module's state is unknown, and the whole module is dropped instead. Until the module is up, the dry passes too.
// A load that fails is not an error: { loaded, ok: false }, and the player's law decides what sounds.
//
// THE AUDIO THREAD RUNS THE LOADS. The port's handler and process() share the audio thread, so a job's heavy half —
// parsing a capture, building and warming its network, tens of milliseconds — stalls the quantum it runs in. Accepted
// for now (Oleh, 19.10.2026); the player's own dip hides the swap itself.
//
// ZERO ALLOCATION PER QUANTUM. process() copies into views made once — and again only after the module's memory grew
// (which detaches every older view): checked at the top of every quantum (a load grows it) and again after the render,
// before copying out (a capture whose architecture allocates while it renders — LSTM and ConvNet in the pinned NAM,
// fc_rig_abi.h — may grow it there) — calls the module, copies back, and asks for a job: no array, no closure, no
// object per quantum while nothing is loading. A knob value that is not a number (a BigInt survives postMessage) is
// ignored, never a throw.

import createFcRig from './fcrig.mjs';

const ABI = 1;              // the oldest fc_rig ABI this processor speaks
const BLEND_SHARE = 0.25;   // the blend (two models) is turned on when ONE render costs at most this share of a quantum
const CAL_MS = 30;          // the calibration burst lasts at least this long by Date.now
const CAL_MAX = 20000;      // …and at most this many renders
const CAL_TRIES = 8;        // a burst that did not run exactly one network per render is tried again, this often
const CAL_FLOOR_MS = 10;    // a burst Date.now saw last less than this is not evidence (a 1 ms tick: 10 % at most)
const SPIN_MAX = 1e6;       // the wait for the burst's first tick gives up after this many reads (a stopped clock)
const NEED_MS = 10000;      // a { need } unanswered this long is answered by the processor itself, with nothing
const SCRATCH = 1024;       // module memory for the two strings of a knob (name, value): 511 UTF-8 bytes each

/** UTF-8 into the module's memory at `p`, NUL-terminated; false when it does not fit. No TextEncoder in a worklet. */
function putUtf8(heap, p, s, room) {
    let at = p;
    const end = p + room - 1;
    for (const ch of s) {
        let c = ch.codePointAt(0);
        const n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (at + n > end) return false;
        if (n === 1) { heap[at++] = c; continue; }
        for (let k = n - 1; k > 0; --k) { heap[at + k] = 0x80 | (c & 0x3f); c >>= 6; }
        heap[at] = (n === 2 ? 0xc0 : n === 3 ? 0xe0 : 0xf0) | c;
        at += n;
    }
    heap[at] = 0;
    return true;
}

/** A number out of a message, never a throw: a BigInt, an object, anything else is NaN, which every setter refuses. */
function num(v) {
    return typeof v === 'number' ? v : typeof v === 'string' ? Number(v) : NaN;
}

/** UTF-8 out of the module's memory. No TextDecoder in a worklet. */
function getUtf8(heap, p, n) {
    let s = '';
    for (let i = p, end = p + n; i < end;) {
        const b = heap[i++];
        if (b < 0x80) { s += String.fromCharCode(b); continue; }
        const k = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : 1;
        let c = b & (0x3f >> k);
        for (let j = 0; j < k && i < end; ++j) c = (c << 6) | (heap[i++] & 0x3f);
        s += String.fromCodePoint(c);
    }
    return s;
}

class FcRigProcessor extends AudioWorkletProcessor {
    constructor(options) {
        super();
        const o = (options && options.processorOptions) || {};
        this.m = null;
        this.rig = 0;
        this.done = false;       // disposed: process() answers false
        this.failed = false;     // an error said: the dry passes through
        this.L = this.R = null;
        this.block = 128;
        this.stereo = o.stereo === true;
        this.calibrate = o.calibrate !== false;
        this.maxModels = this.calibrate ? 1 : (o.maxModels === 1 ? 1 : 2);
        this.calState = this.calibrate ? 'waiting' : 'off';   // waiting -> steady -> done
        this.calTries = 0;
        this.queued = o.knobs ? [o.knobs] : [];               // knobs held until the start's decision
        this.firstRig = o.rig || null;
        this.pack = 0;           // which load() the job out belongs to (the page echoes it)
        this.awaiting = null;    // the files[].id a job out is waiting for, or null
        this.waited = 0;         // quanta since that { need } was posted
        const needMs = o.needTimeoutMs === undefined ? NEED_MS : num(o.needTimeoutMs);
        this.needMs = Number.isFinite(needMs) && needMs > 0 ? needMs : 0;
        this.needQuanta = 0;     // the same in quanta, once the quantum is known (start); 0 = never
        this.port.onmessage = (e) => this.onMessage(e.data);

        let module;
        if (o.module instanceof WebAssembly.Module) {
            module = o.module;
        } else if (o.wasmBytes) {
            try {
                module = new WebAssembly.Module(o.wasmBytes);
            } catch (e) {
                this.fail('fc-rig: the wasmBytes do not compile: ' + e);
                return;
            }
        } else {
            this.fail('fc-rig: no WebAssembly.Module arrived in processorOptions.module', 'wasmBytes');
            return;
        }
        createFcRig({
            instantiateWasm(imports, done) {
                const instance = new WebAssembly.Instance(module, imports);
                done(instance, module);
                return instance.exports;
            },
        }).then((m) => this.start(m), (e) => this.fail('fc-rig: ' + e));
    }

    /** A setup or runtime error: said once, the instance freed (unless `trapped`), the dry from here on. */
    fail(error, retry, trapped) {
        if (this.failed || this.done) return;
        this.failed = true;
        this.port.postMessage(retry ? { error, retry } : { error });
        if (trapped) this.rig = 0;   // a module that trapped is not called again, not even to free
        this.release();
    }

    trap(e) {
        this.fail('fc-rig: the module trapped: ' + e, undefined, true);
    }

    /** The only end: { dispose }. */
    dispose() {
        if (this.done) return;
        this.release();
        this.done = true;
        this.port.onmessage = null;
        this.port.close();
    }

    release() {
        if (this.m && this.rig) {
            try { this.m._fc_rig_destroy(this.rig); } catch (e) { /* freed or not, nothing calls it again */ }
        }
        this.rig = 0;
        this.m = this.L = this.R = this.queued = this.firstRig = null;
    }

    start(m) {
        if (this.done || this.failed) return;
        try {
            const abi = m._fc_rig_abi_version();
            if (abi < ABI) {
                this.fail(`fc-rig: the module speaks ABI ${abi}, this processor needs ${ABI}`);
                return;
            }
            this.m = m;
            this.rig = m._fc_rig_create();
            this.block = globalThis.renderQuantumSize || 128;
            this.needQuanta = this.needMs > 0 ? Math.max(1, Math.ceil(this.needMs * sampleRate / (1000 * this.block))) : 0;
            if (!this.rig || !m._fc_rig_prepare(this.rig, sampleRate, this.block, this.stereo ? 2 : 1)) {
                this.fail(`fc-rig: prepare refused (${sampleRate} Hz, ${this.block})`);
                return;
            }
            this.scratch = m._fc_rig_alloc(SCRATCH);
            m._fc_rig_set_max_models(this.rig, this.maxModels);
            this.views();
            this.port.postMessage({ ready: true, abi });
            if (this.firstRig) this.load(this.firstRig);
            this.firstRig = null;
            if (this.calState === 'off') this.drain();
        } catch (e) {
            this.trap(e);
        }
    }

    /** Views onto the module's channel buffers. Made after prepare and again whenever the memory grew. */
    views() {
        const m = this.m;
        this.pL = m._fc_rig_buffer(this.rig, 0);
        this.pR = this.stereo ? m._fc_rig_buffer(this.rig, 1) : 0;
        this.L = new Float32Array(m.HEAPF32.buffer, this.pL, this.block);
        this.R = this.stereo ? new Float32Array(m.HEAPF32.buffer, this.pR, this.block) : null;
    }

    load(bytes) {
        const m = this.m, n = bytes.byteLength;
        const p = n > 0 ? m._fc_rig_alloc(n) : 0;
        if (p) m.HEAPU8.set(new Uint8Array(bytes), p);
        const ok = p ? m._fc_rig_load(this.rig, p, n) === 1 : (m._fc_rig_unload(this.rig), false);
        if (p) m._fc_rig_free(p);
        ++this.pack;
        this.awaiting = null;
        const sweep = m._fc_rig_gain_dial_sweep(this.rig);
        if (ok && this.calState !== 'off' && this.calState !== 'done' && sweep > 0)
            m._fc_rig_set_gain_dial(this.rig, sweep / 2);   // 12 o'clock: the capture the start is timed on
        this.port.postMessage({ pack: this.pack, ok, sweep });
    }

    /** Another pack, with calibration on: the start again — one model, silent, knobs queued until it decides. */
    rearm() {
        this.maxModels = 1;
        this.m._fc_rig_set_max_models(this.rig, 1);
        this.calState = 'waiting';
        this.calTries = 0;
    }

    /** A knob message: { set: { maxModels, normalize, inputTrims, blendShape: [point, width], switch: { name: value },
        dial: { name: degrees }, gain: degrees } }, applied in that order. */
    apply(s) {
        const m = this.m, rig = this.rig;
        if (s.maxModels === 1 || s.maxModels === 2) { this.maxModels = s.maxModels; m._fc_rig_set_max_models(rig, s.maxModels); }
        if (s.normalize !== undefined) m._fc_rig_set_normalize(rig, s.normalize ? 1 : 0);
        if (s.inputTrims !== undefined) m._fc_rig_set_input_trims(rig, s.inputTrims ? 1 : 0);
        if (Array.isArray(s.blendShape)) m._fc_rig_set_blend_shape(rig, num(s.blendShape[0]), num(s.blendShape[1]));
        if (s.switch) for (const k in s.switch) this.named(k, String(s.switch[k]));
        if (s.dial) for (const k in s.dial) this.named(k, num(s.dial[k]));
        if (s.gain !== undefined) m._fc_rig_set_gain_dial(rig, num(s.gain));
    }

    named(name, value) {
        const heap = this.m.HEAPU8, p = this.scratch, half = SCRATCH / 2;
        if (!putUtf8(heap, p, name, half)) return;
        if (typeof value === 'string') {
            if (putUtf8(heap, p + half, value, half)) this.m._fc_rig_set_switch(this.rig, p, p + half);
        } else if (Number.isFinite(value)) {
            this.m._fc_rig_set_dial(this.rig, p, value);
        }
    }

    drain() {
        const q = this.queued;
        this.queued = [];
        if (q) for (const s of q) this.apply(s);
    }

    onMessage(d) {
        if (!d || this.done) return;
        if (d.dispose) { this.dispose(); return; }
        if (this.failed) return;
        if (!this.rig) {   // not up yet: knobs wait, a pack replaces the first one
            if (d.set && this.queued) this.queued.push(d.set);
            if (d.load) this.firstRig = d.load;
            return;
        }
        try {
            if (d.unload) { this.m._fc_rig_unload(this.rig); ++this.pack; this.awaiting = null; }
            if (d.load) {
                if (this.calibrate) this.rearm();
                this.load(d.load);
            }
            if (d.file !== undefined) this.bytes(d);
            if (d.set) {
                if (this.calState === 'waiting' || this.calState === 'steady') this.queued.push(d.set);
                else this.apply(d.set);
            }
            if (d.reset) this.m._fc_rig_reset(this.rig);
            if (d.measure) this.port.postMessage({ measured: this.burst() });
            if (d.read) this.read();
        } catch (e) {
            this.trap(e);
        }
    }

    /** The page's answer to { need }: the bytes in, the job run and delivered. A stale answer is dropped. */
    bytes(d) {
        if (this.awaiting === null || d.pack !== this.pack || d.file !== this.awaiting) return;
        const m = this.m, rig = this.rig;
        const n = d.bytes ? d.bytes.byteLength : 0;
        const p = n > 0 ? m._fc_rig_job_bytes(rig, n) : 0;
        if (p) m.HEAPU8.set(new Uint8Array(d.bytes), p);   // HEAPU8 read after the call: it may have grown the memory
        this.finish(this.awaiting);
    }

    finish(file) {
        const m = this.m, rig = this.rig;
        this.awaiting = null;
        m._fc_rig_job_run(rig);
        const ok = m._fc_rig_job_deliver(rig) === 1;
        this.port.postMessage({ loaded: file, ok });
        if (!ok && this.calState === 'waiting') this.decide(null, 'load-failed');   // nothing to time: one model
    }

    /** After every quantum: housekeeping and, when the law asks, a job — run here at once if the player has the
        bytes, otherwise asked of the page. */
    pump() {
        const m = this.m, rig = this.rig;
        if (m._fc_rig_take_job(rig) !== 1) {
            if (this.awaiting !== null && this.needQuanta > 0 && ++this.waited >= this.needQuanta)
                this.finish(this.awaiting);   // the page never answered: run without bytes, its late answer dropped
            return;
        }
        const file = getUtf8(m.HEAPU8, m._fc_rig_job_file(rig), m._fc_rig_job_file_length(rig));
        if (m._fc_rig_job_needs_bytes(rig) === 1) {
            this.awaiting = file;
            this.waited = 0;
            this.port.postMessage({ need: file, pack: this.pack });
        } else {
            this.finish(file);
        }
    }

    /** Renders of silence back to back until Date.now moved CAL_MS; what ran in them, counted by the player. */
    burst() {
        const m = this.m, rig = this.rig;
        m._fc_rig_clear_counters(rig);
        let t0 = Date.now(), spins = 0;
        while (Date.now() === t0 && ++spins < SPIN_MAX) { /* start on a tick */ }
        t0 = Date.now();
        let renders = 0, now = t0;
        do {
            if (this.L.buffer !== m.HEAPF32.buffer) this.views();
            this.L.fill(0);
            if (this.R) this.R.fill(0);
            m._fc_rig_process(rig, this.pL, this.pR, this.block);
            ++renders;
            now = Date.now();
        } while (now - t0 < CAL_MS && renders < CAL_MAX);
        const elapsedMs = now - t0;
        const runs = m._fc_rig_run_blocks(rig, 0) + m._fc_rig_run_blocks(rig, 1);
        const pairs = m._fc_rig_pair_blocks(rig), warm = m._fc_rig_warm_blocks(rig);
        const renderUs = 1000 * elapsedMs / renders, quantumUs = 1e6 * this.block / sampleRate;
        const valid = runs === renders && pairs === 0 && warm === 0 && elapsedMs >= CAL_FLOOR_MS;
        return { renderUs, quantumUs, share: renderUs / quantumUs, renders, elapsedMs, runs, pairs, warm, valid };
    }

    /** The start: wait for the capture at 12 o'clock to be held and running alone, then time it and decide. */
    calibrateStep() {
        const m = this.m, rig = this.rig;
        if (this.calState === 'waiting') {
            if (this.awaiting === null && (m._fc_rig_held(rig, 0) || m._fc_rig_held(rig, 1))) {
                m._fc_rig_clear_counters(rig);
                this.calState = 'steady';
            }
            return;
        }
        // 'steady': the block just rendered ran exactly one network, warm — then the burst.
        const runs = m._fc_rig_run_blocks(rig, 0) + m._fc_rig_run_blocks(rig, 1);
        const steady = runs === 1 && m._fc_rig_pair_blocks(rig) === 0 && m._fc_rig_warm_blocks(rig) === 0;
        if (!steady) { m._fc_rig_clear_counters(rig); return; }
        const b = this.burst();
        if ((b.runs !== b.renders || b.pairs !== 0 || b.warm !== 0) && ++this.calTries < CAL_TRIES) {
            m._fc_rig_clear_counters(rig);
            return;   // something else ran in the burst: time it again
        }
        this.decide(b, b.valid ? undefined : b.elapsedMs < CAL_FLOOR_MS ? 'clock' : 'counters');
    }

    /** The start's end: the blend only on a valid burst that fits; said, and the knobs that waited applied in order. */
    decide(b, reason) {
        this.maxModels = b !== null && b.valid && b.share <= BLEND_SHARE ? 2 : 1;
        this.m._fc_rig_set_max_models(this.rig, this.maxModels);
        this.calState = 'done';
        const said = b !== null ? { ...b, maxModels: this.maxModels } : { maxModels: this.maxModels, valid: false };
        if (reason) said.reason = reason;
        this.port.postMessage({ calibrated: said });
        this.drain();
    }

    read() {
        const m = this.m, rig = this.rig;
        this.port.postMessage({ state: {
            sounding: { db: m._fc_rig_sounding_db(rig), tagged: m._fc_rig_sounding_tagged(rig) === 1,
                        blended: m._fc_rig_sounding_blended(rig) === 1 },
            held: [m._fc_rig_held(rig, 0) === 1, m._fc_rig_held(rig, 1) === 1],
            latency: m._fc_rig_latency_samples(rig), maxModels: this.maxModels,
            runBlocks: [m._fc_rig_run_blocks(rig, 0), m._fc_rig_run_blocks(rig, 1)],
            pairBlocks: m._fc_rig_pair_blocks(rig), warmBlocks: m._fc_rig_warm_blocks(rig),
        } });
    }

    process(inputs, outputs) {
        if (this.done) return false;
        const out = outputs[0];
        const oL = out[0];
        const oR = out.length > 1 ? out[1] : null;
        const input = inputs[0];
        const inL = input.length > 0 ? input[0] : null;
        const inR = input.length > 1 ? input[1] : inL;

        if (this.rig === 0 || oL.length !== this.block) {
            // Not up yet, or failed: the dry passes through. A quantum of another size than the one prepared is an
            // error, said once — and this quantum, like every one after it, passes dry.
            if (inL) { oL.set(inL); if (oR) oR.set(inR); }
            else { oL.fill(0); if (oR) oR.fill(0); }
            if (this.rig !== 0) this.fail(`fc-rig: a ${oL.length}-frame render quantum; prepared for ${this.block}`);
            return true;
        }

        const m = this.m;
        try {
            if (this.L.buffer !== m.HEAPF32.buffer) this.views();   // a load grew the memory
            if (inL) this.L.set(inL); else this.L.fill(0);
            if (this.R) { if (inR) this.R.set(inR); else this.R.fill(0); }
            m._fc_rig_process(this.rig, this.pL, this.pR, this.block);
            if (this.L.buffer !== m.HEAPF32.buffer) this.views();   // the render grew it (an allocating architecture)
            if (this.calState === 'done' || this.calState === 'off') {
                oL.set(this.L);
                if (oR) oR.set(this.R || this.L);
            } else {
                oL.fill(0);   // the start is silent until it has decided
                if (oR) oR.fill(0);
            }
            this.pump();
            if (this.calState === 'waiting' || this.calState === 'steady') this.calibrateStep();
        } catch (e) {
            this.trap(e);
            if (inL) { oL.set(inL); if (oR) oR.set(inR); }
            else { oL.fill(0); if (oR) oR.fill(0); }
        }
        return true;
    }
}

registerProcessor('fc-rig', FcRigProcessor);
