// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// The AudioWorklet processor of fcguitarfx: the amp's echo and room (tools/fc_guitar_fx_abi.h) on the audio thread.
// Registered as 'fc-guitar-fx'. Stereo out; stereo in (a mono input is played on both channels, no input is silence,
// and the tails still ring out).
//
// THE PAGE COMPILES, THE PROCESSOR INSTANTIATES. An AudioWorkletGlobalScope has no fetch, and in some browsers no
// URL and no TextDecoder, so the processor never loads anything: the page compiles fcguitarfx.wasm itself and hands
// the WebAssembly.Module over in processorOptions, and the glue gets it through `instantiateWasm`.
//
//   const module = await WebAssembly.compileStreaming(fetch(wasmUrl));
//   await context.audioWorklet.addModule(workletUrl);       // this file; it imports ./fcguitarfx.mjs statically
//   const node = new AudioWorkletNode(context, 'fc-guitar-fx', {
//       numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
//       processorOptions: { module, knobs: { delay_mix: 25, reverb_character: 2 } } });
//   node.port.onmessage = (e) => { if (e.data.error) console.error(e.data.error); };   // { ready, abi } or { error }
//   node.port.postMessage({ set: { delay_mix: 40 } });     // any knobs, any time
//   node.port.postMessage({ reset: true });                // a stream restart
//   node.port.postMessage({ dispose: true });              // the end: the instance freed, the processor finished
//
// THE BYTES PATH. WebKit does not throw on a WebAssembly.Module it cannot clone into the worklet: the option arrives
// as null, and the processor answers { error, retry: 'wasmBytes' }. The page then drops that node and makes another
// with processorOptions: { wasmBytes: <ArrayBuffer of fcguitarfx.wasm> }; that processor compiles the 32 KB itself,
// SYNCHRONOUSLY, on the audio thread, once, at construction.
//
// AN ERROR IS NOT AN END. The node stands IN SERIES (amp -> this -> out), so a processor that went silent would mute
// everything after it. A setup or runtime error — no module (the bytes retry included), a module older than this
// processor, a refused prepare, a render quantum of another size than the one prepared — posts one { error }, frees
// the C instance if there is one, and from then on passes its input through dry and keeps answering true. The page
// decides what follows: it may rewire around the node and send { dispose }. ONLY { dispose } ENDS IT: the instance
// freed, the module and the views dropped, the port closed, process() false — so the node and its ~25 MB of wasm
// memory can be collected without waiting for the context to close. Until the module is up, the dry passes through.
//
// KNOBS ARE MESSAGES, NOT AudioParams. Every knob here is a setting a hand moves, not a signal: two are switches, two
// are choices, and the stages already glide what must glide (the delay's head, the room's parameters). An AudioParam
// would be read every render quantum for fifteen knobs to find them unchanged; a message costs nothing until a knob
// moves. A message is handled on the audio thread between two quanta, and the next quantum hears it from its first
// sample. The keys are the ABI's setter names without their prefix (delay_mix, reverb_character, …).
//
// ZERO ALLOCATION PER QUANTUM. process() copies into views made once after prepare, calls the module, copies back:
// no array, no closure, no object per call. The module itself allocates only in prepare.

import createFcGuitarFx from './fcguitarfx.mjs';

const ABI = 1;   // the oldest fc_guitar_fx ABI this processor speaks
const KNOBS = [
    'delay_on', 'delay_sync', 'delay_division', 'delay_bpm', 'delay_time_ms', 'delay_repeats', 'delay_dark',
    'delay_offset_ms', 'delay_mix',
    'reverb_on', 'reverb_character', 'reverb_mix', 'reverb_decay', 'reverb_predelay_ms', 'reverb_hpf_hz',
];
// The integer knobs, settled HERE: a wasm i32 parameter takes ToInt32 of the number, which turns NaN into 0 (a
// switch off, the first character) and wraps 2^32 to 0. So, as the float knobs are treated in the module: a
// non-finite value is ignored, a switch is any non-zero, and a choice is rounded (the app's roundToInt) and clamped
// onto its range. The float knobs need nothing: the module ignores a non-finite one and clamps a finite one.
const SWITCHES = new Set(['delay_on', 'delay_sync', 'reverb_on']);
const CHOICES = { delay_division: 12, reverb_character: 5 };

class FcGuitarFxProcessor extends AudioWorkletProcessor {
    constructor(options) {
        super();
        const o = (options && options.processorOptions) || {};
        this.m = null;
        this.fx = 0;
        this.done = false;     // disposed: process() answers false
        this.failed = false;   // an error said: the dry passes through
        this.L = this.R = null;
        this.block = 128;
        this.queued = Object.assign({}, o.knobs);   // knobs that arrive before the module is up
        this.port.onmessage = (e) => this.onMessage(e.data);

        let module;
        if (o.module instanceof WebAssembly.Module) {
            module = o.module;
        } else if (o.wasmBytes) {
            try {
                module = new WebAssembly.Module(o.wasmBytes);
            } catch (e) {
                this.fail('fc-guitar-fx: the wasmBytes do not compile: ' + e);
                return;
            }
        } else {
            this.fail('fc-guitar-fx: no WebAssembly.Module arrived in processorOptions.module', 'wasmBytes');
            return;
        }
        createFcGuitarFx({
            instantiateWasm(imports, done) {
                const instance = new WebAssembly.Instance(module, imports);
                done(instance, module);
                return instance.exports;
            },
        }).then((m) => this.start(m), (e) => this.fail('fc-guitar-fx: ' + e));
    }

    /** A setup or runtime error: said once, the C instance freed, and the dry passing through from here on (see AN
        ERROR IS NOT AN END above). `retry` names what the page should send instead. */
    fail(error, retry) {
        if (this.failed || this.done) return;
        this.failed = true;
        this.port.postMessage(retry ? { error, retry } : { error });
        this.release();
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
        if (this.m && this.fx) this.m._fc_guitar_fx_destroy(this.fx);
        this.fx = 0;
        this.m = this.setters = this.queued = this.L = this.R = null;
    }

    start(m) {
        if (this.done || this.failed) return;   // disposed (or failed) while the module was coming up
        const abi = m._fc_guitar_fx_abi_version();
        if (abi < ABI) {
            this.fail(`fc-guitar-fx: the module speaks ABI ${abi}, this processor needs ${ABI}`);
            return;
        }
        const fx = m._fc_guitar_fx_create();
        this.setters = {};
        for (const k of KNOBS)
            this.setters[k] = m['_fc_guitar_fx_set_' + k];
        this.m = m;
        this.fx = fx;
        this.applyKnobs(this.queued);   // before prepare: the room starts AT its character
        this.queued = null;

        // The render quantum: 128 frames everywhere today; a context that renders more announces it.
        this.block = globalThis.renderQuantumSize || 128;
        if (!fx || !m._fc_guitar_fx_prepare(fx, sampleRate, this.block)) {
            this.fail(`fc-guitar-fx: prepare refused (${sampleRate} Hz, ${this.block})`);   // and the instance freed
            return;
        }
        // Views onto the module's own two buffers, made AFTER prepare (its allocation may grow the memory, which
        // detaches every older view) and never again: process() allocates nothing, so the memory never moves.
        this.pL = m._fc_guitar_fx_buffer(fx, 0);
        this.pR = m._fc_guitar_fx_buffer(fx, 1);
        this.L = new Float32Array(m.HEAPF32.buffer, this.pL, this.block);
        this.R = new Float32Array(m.HEAPF32.buffer, this.pR, this.block);
        this.port.postMessage({ ready: true, abi });
    }

    applyKnobs(knobs) {
        for (const k in knobs) {
            const set = this.setters[k];
            let v = +knobs[k];
            if (!set || !Number.isFinite(v)) continue;
            if (SWITCHES.has(k)) v = v !== 0 ? 1 : 0;
            else if (k in CHOICES) v = Math.min(CHOICES[k], Math.max(0, Math.round(v)));
            set(this.fx, v);
        }
    }

    onMessage(d) {
        if (!d || this.done) return;
        if (d.dispose) {
            this.dispose();
            return;
        }
        if (this.failed) return;
        if (d.set) {
            if (this.fx) this.applyKnobs(d.set);
            else if (this.queued) Object.assign(this.queued, d.set);
        }
        if (d.reset && this.fx) this.m._fc_guitar_fx_reset(this.fx);
    }

    process(inputs, outputs) {
        if (this.done) return false;
        const out = outputs[0];
        const oL = out[0];
        const oR = out.length > 1 ? out[1] : null;
        const input = inputs[0];
        const inL = input.length > 0 ? input[0] : null;
        const inR = input.length > 1 ? input[1] : inL;

        if (this.fx === 0 || oL.length !== this.block) {
            // Not up yet, or failed: the dry passes through, as the links do when they stand by. A quantum of another
            // size than the one prepared (a context rendering more, with no renderQuantumSize to say so) is an error,
            // said once — and this quantum, like every one after it, passes dry.
            if (inL) { oL.set(inL); if (oR) oR.set(inR); }
            else { oL.fill(0); if (oR) oR.fill(0); }
            if (this.fx !== 0)
                this.fail(`fc-guitar-fx: a ${oL.length}-frame render quantum; prepared for ${this.block}`);
            return true;
        }

        const L = this.L, R = this.R;
        if (inL) { L.set(inL); R.set(inR); }
        else { L.fill(0); R.fill(0); }
        this.m._fc_guitar_fx_process(this.fx, this.pL, this.pR, this.block);
        oL.set(L);
        if (oR) oR.set(R);
        return true;
    }
}

registerProcessor('fc-guitar-fx', FcGuitarFxProcessor);
