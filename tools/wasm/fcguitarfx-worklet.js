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
//
// A browser that cannot clone a WebAssembly.Module into the worklet can send the bytes instead —
// processorOptions: { wasmBytes: <ArrayBuffer> } — and the processor compiles them once, at construction.
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

class FcGuitarFxProcessor extends AudioWorkletProcessor {
    constructor(options) {
        super();
        const o = (options && options.processorOptions) || {};
        this.m = null;
        this.fx = 0;
        this.block = 128;
        this.queued = Object.assign({}, o.knobs);   // knobs that arrive before the module is up
        this.port.onmessage = (e) => this.onMessage(e.data);

        let module;
        try {
            module = o.module instanceof WebAssembly.Module ? o.module : new WebAssembly.Module(o.wasmBytes);
        } catch (e) {
            this.port.postMessage({ error: 'fc-guitar-fx: no WebAssembly.Module in processorOptions (module or wasmBytes)' });
            return;
        }
        createFcGuitarFx({
            instantiateWasm(imports, done) {
                const instance = new WebAssembly.Instance(module, imports);
                done(instance, module);
                return instance.exports;
            },
        }).then((m) => this.start(m), (e) => this.port.postMessage({ error: 'fc-guitar-fx: ' + e }));
    }

    start(m) {
        const abi = m._fc_guitar_fx_abi_version();
        if (abi < ABI) {
            this.port.postMessage({ error: `fc-guitar-fx: the module speaks ABI ${abi}, this processor needs ${ABI}` });
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
            this.fx = 0;
            this.port.postMessage({ error: `fc-guitar-fx: prepare refused (${sampleRate} Hz, ${this.block})` });
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
            if (set) set(this.fx, +knobs[k]);
        }
    }

    onMessage(d) {
        if (!d) return;
        if (d.set) {
            if (this.fx) this.applyKnobs(d.set);
            else if (this.queued) Object.assign(this.queued, d.set);
        }
        if (d.reset && this.fx) this.m._fc_guitar_fx_reset(this.fx);
        if (d.dispose && this.fx) {
            this.m._fc_guitar_fx_destroy(this.fx);
            this.fx = 0;
        }
    }

    process(inputs, outputs) {
        const out = outputs[0];
        const oL = out[0];
        const oR = out.length > 1 ? out[1] : null;
        const input = inputs[0];
        const inL = input.length > 0 ? input[0] : null;
        const inR = input.length > 1 ? input[1] : inL;

        if (this.fx === 0 || oL.length !== this.block) {
            // Not up yet (or gone): pass the dry through, as the links do when they stand by.
            if (inL) { oL.set(inL); if (oR) oR.set(inR); }
            else { oL.fill(0); if (oR) oR.fill(0); }
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
