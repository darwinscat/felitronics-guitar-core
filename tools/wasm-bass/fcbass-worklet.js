// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

// The AudioWorklet processor of fcbass: THE BASS AMP — felitronics::bass::BassChain around the pack player
// (tools/fc_bass_abi.h) — on the audio thread. Registered as 'fc-bass'. Channel 0 of the input is played (a bass DI);
// the output is two planes, the chorus's stereo from its seam on.
//
// THE RIG HALF IS fcrig's, WHOLE: this processor is rig-port.js's RigProcessor around an fc_bass instance, so
// everything fcrig-worklet.js's page does it does the same way, the same messages — the module handed in
// (processorOptions.module or wasmBytes), the pack (processorOptions.rig, { load }), every { need } answered, the files
// up front ({ files }), the preload, the start and its { calibrated }, the quiet warm, PARK, the scope, and the
// player's knobs ({ set: { gain, switch, dial, feed, crossover, … } }: DRIVE is `gain`, degrees of the pack's sweep; the
// gear is a `switch` — RedGrooveBass's Voice; BLEND is the crossover, but prefer amp_blend_* below, which tell it as the
// app does). The player's whole contract is rig-port.js's (and fc_rig_abi.h's).
//
// WHAT IT ADDS:
//   { bass: { <id>: <value>, … } }   the chain's knobs, by the app's parameter ids and in its units (fc_bass_abi.h has
//                    the list): comp_threshold, octave, filter, chorus, amp_blend_wet, cab_mix, bass_hpf, bass_lpf,
//                    preamp_eq_lo_db / _b1_db / _b1_hz / _hi_db, … — the ids a face kit names its controls by. A value
//                    that is not a finite number, or an id the module does not know, is ignored. They land at the next
//                    quantum. processorOptions.bass is the same object, landed before the first quantum.
//   { cab: { taps: Float32Array, rate } }   the cabinet's IR, one channel at its own rate (8 kHz … 768 kHz, at most
//                    5 s): baked, levelled to the bass's reference, crossfaded in over 50 ms. A bass amp's cabinets are
//                    Cab IR Generator's: render the preset's TOML with its module and hand in the 24-bit WAV's samples
//                    as the plug-in reads them, the integers over 2^23. Answers { cab: true } taken, { cab: false }
//                    refused (the cabinet it had stays). processorOptions.cab is the same object.
//   { read }         …answers the player's { state } as fcrig's does, and the chain's read-out beside it:
//                    { bass: { compGrDb } } — COMP's squeeze now, dB (0 or below), for its meter.
//
// Before the module is up, { bass } and { cab } wait and land in order once it is. Nothing here allocates per quantum:
// the knobs and the cabinet are the port's (the audio thread, between quanta), the render is one fc_bass_process.

import createFcBass from './fcbass.mjs';
import { RigProcessor } from './rig-port.js';

const BASS_ABI = 1;   // the oldest fc_bass ABI this processor speaks
const ID_BYTES = 64;  // module memory for one knob's id: 63 UTF-8 bytes

class FcBassProcessor extends RigProcessor {
    constructor(options) {
        super(options, createFcBass);
        const o = (options && options.processorOptions) || {};
        this.bassQueue = [];
        if (o.bass && typeof o.bass === 'object') this.bassQueue.push({ bass: o.bass });
        if (o.cab && typeof o.cab === 'object') this.bassQueue.push({ cab: o.cab });
        this.bass = this.bass || 0;
    }

    get label() { return 'fc-bass'; }
    planesFrom() { return 2; }
    minAbi() { return 6; }              // the player inside speaks fc_rig ABI 6 (the crossover)

    createInstance(m) {
        if (m._fc_bass_abi_version() < BASS_ABI) return 0;
        this.bass = m._fc_bass_create();
        if (!this.bass) return 0;
        this.idAt = m._fc_rig_alloc(ID_BYTES);
        return m._fc_bass_rig(this.bass);
    }
    prepareInstance(m) {
        if (m._fc_bass_prepare(this.bass, sampleRate, this.block) !== 1) return false;
        // What came before the module was up lands now, in order.
        const queued = this.bassQueue || [];
        this.bassQueue = null;
        for (const d of queued) this.extra(d);
        return true;
    }
    bufferOf(m, channel) { return m._fc_bass_buffer(this.bass, channel); }
    render(m) { m._fc_bass_process(this.bass, this.pL, this.pR, this.block); }
    resetInstance(m) { m._fc_bass_reset(this.bass); }
    destroyInstance(m) {
        if (this.idAt) m._fc_rig_free(this.idAt);
        m._fc_bass_destroy(this.bass);
        this.bass = 0;
        this.idAt = 0;
    }

    /** Before the module is up: kept. With it up: landed now. */
    onMessage(d) {
        if (d && !this.done && !this.failed && (d.bass || d.cab)) {
            if (!this.rig && this.bassQueue) { this.bassQueue.push({ bass: d.bass, cab: d.cab }); }
            // with the module up, extra() lands it (called by the rig half's own handler)
        }
        super.onMessage(d);
    }

    extra(d) {
        if (!this.rig || !this.bass) return;
        const m = this.m;
        if (d.bass && typeof d.bass === 'object') {
            for (const id in d.bass) {
                const v = d.bass[id];
                if (typeof v !== 'number' || !Number.isFinite(v)) continue;
                if (!this.putId(m, id)) continue;
                m._fc_bass_set(this.bass, this.idAt, v);
            }
        }
        if (d.cab && typeof d.cab === 'object') {
            const taps = d.cab.taps, rate = d.cab.rate;
            let ok = false;
            if (taps instanceof Float32Array && taps.length > 0 && typeof rate === 'number' && Number.isFinite(rate)) {
                const p = m._fc_bass_cab_taps(this.bass, taps.length);
                if (p) {
                    m.HEAPF32.set(taps, p >> 2);   // HEAPF32 read after the call: it may have grown the memory
                    ok = m._fc_bass_cab_load(this.bass, taps.length, rate) === 1;
                }
            }
            this.port.postMessage({ cab: ok });
            if (this.L && this.L.buffer !== m.HEAPF32.buffer) this.views();
        }
        if (d.read) this.port.postMessage({ bass: { compGrDb: m._fc_bass_comp_gr_db(this.bass) } });
    }

    putId(m, id) {
        const heap = m.HEAPU8;
        let n = 0;
        for (let i = 0; i < id.length; ++i) {
            const c = id.charCodeAt(i);
            if (c > 0x7f || n + 1 >= ID_BYTES) return false;   // the app's ids are ASCII
            heap[this.idAt + n++] = c;
        }
        heap[this.idAt + n] = 0;
        return true;
    }
}

registerProcessor('fc-bass', FcBassProcessor);
