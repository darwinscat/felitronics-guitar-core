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
// THE FILES UP FRONT (ABI 5, v0.7.0): a page that has the pack's files in hand — fetched with its rig.json — hands them
// in with the pack, { load, files: [{ file: <files[].id>, bytes }, …] }, or after it ({ files, pack }), and no load then
// waits for a { need } round trip; on a network that is one round trip per capture saved. A file left out is still
// asked for with { need }. processorOptions.files does the same for processorOptions.rig.
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
//       // { ready, abi }  { calibrated: { renderUs, quantumUs, share, maxModels, renders, elapsedMs, valid, reason } }: sounding
//       // { loaded, ok }  { pack: n, ok, sweep }  { preloaded: { pack, total, failed, quanta } }  { state: {…} }  { error }
//       if (data.scope) frame = data;                         // the scope's buffer, back: see THE SCOPE below
//   };
//   node.port.postMessage({ set: { gain: 210, dial: { Bass: 120 }, switch: { Bright: 'Center' }, feed: 6 } });
//   node.port.postMessage({ standby: true });                 // PARK; { standby: false } plays again (THE PARK below)
//
// THE SCOPE (ABI 2's fc_rig_scope_read, drawing ABI 3's points — OrbitAmp's ScopeTap): the page owns ONE ArrayBuffer of 2·n floats and
// sends it ({ scope: buffer }, transferred); the processor copies the newest frame into it — the dry in floats
// [0, n), the wet at the dry's RMS in [n, 2n), n ≤ 2048 — and sends the same buffer back, transferred:
// { scope: buffer, n: <samples read, 0 = none yet>, frames: <the tap's count> }. `frames` moves only when the
// tap was written, so an unchanged count is a frame already drawn (and stood by, it does not move). Why a buffer
// that travels rather than one the processor posts: a fresh 16 KB array per frame is 1 MB/s of garbage made ON THE
// AUDIO THREAD at 60 fps, and its collector runs there too; a transferred buffer is neither copied by the structured
// clone nor allocated by either side — what each message costs is the message object and one view of the buffer,
// a few dozen bytes, on the port, never in process(). One request in flight: the page asks again when it is back.
// WHERE IT TAPS — where OrbitAmp's own code taps, around the WHOLE player (src/core/CapturedBlock.h:543-556). The dry
// is the input times the feed and nothing else — before the pack's input level and its pre tone, undelayed; the wet is
// the player's output — pre tone, models, post tone, blend knob, output level — before the PARK fade. As in the app,
// the wet trails the dry by the player's latency when the context's rate is not the pack's. (ABI 2, v0.5.0, tapped around
// the models, the app's web spec and the site's tap around TONE3000's node.) A device handed no input channel writes no tap.
//
// THE PARK ({ standby }) DOES NOT WAIT. Every message lands as it comes, PARK too, before the module is up as soon as it
// is. PARK IS A BYPASS, AND NOTHING LOADS UNDER IT: no job runs, no { need } is posted and no capture is warmed while
// parked — the background fill pauses, an answer that arrives meanwhile is kept, and its job runs at the first quantum
// after the return; the output is the player's own (the fade, then the input at the latency). A start under PARK
// pauses with it (nothing is timed) and goes on after the return. { set: { standby } } is the same message.
//
// THE START (v0.7.0: no burst, no hold, nothing silenced by this processor). A browser's audio thread may afford two
// networks or only one, and the honest way to know is to time one on it. So, unless processorOptions.calibrate is
// false, at every pack ({ load }, and the first one) the start:
//   · runs ONE model (setMaxModels(1)) — or, once the session has decided anything, the model count it decided — and
//     lands every knob at once, the gain dial included: the capture timed is the one the DRIVE asks for (no 12
//     o'clock any more), so the copy that warms is the copy that sounds. Only a maxModels waits, for the decision.
//   · once a capture is held, reads its ARCHITECTURE SIGNATURE (fc_rig_signature: architecture, layer config, rate —
//     never weights). A signature this session has decided on is not timed again: its decision stands. Otherwise
//     one model is TIMED: Date.now around each render that ran exactly one network of it, alone (the player's
//     counters; an outgoing capture's drain is not its evidence) — the renders of the warm-up itself, live, nothing
//     extra run for it; a worklet has no performance.now(), and a mean of 0-or-1 ms readings over many renders is
//     unbiased. The signature is read for the captures held when the first one lands: a pack whose captures are of
//     several architectures is decided by those (the site's packs are one architecture each).
//   · ENDS WHEN THE SOUND OPENS — the first quantum the player lets a capture be heard (fc_rig_audible: the first
//     sample of the law's kDipSeconds fade-in) — and says so then (a capture that opens before CAL_RENDERS renders
//     were timed: CAL_GRACE quanta later at most): { calibrated: { renderUs, quantumUs, share, maxModels, renders,
//     elapsedMs, runs, pairs, warm, valid, reason } }, the session's stored one for a known signature. Valid: at least
//     CAL_RENDERS renders timed and a clock seen to step by CAL_TICK_MS or less; valid and one render at most
//     BLEND_SHARE of the quantum (~667 us at 48 kHz) -> the blend (setMaxModels(2)); else one capture with dips
//     (reason 'clock': Date.now too coarse — Firefox's resistFingerprinting; 'short': too few renders timed by then,
//     timed on — THE SETTLE — and decided for the next pack).
//   · THE SETTLE: after a sound opens on ONE model, its architecture is timed on — renders that ran one network of it
//     alone, nothing run for it — until CAL_SETTLE of them, counted from an open (the start's own renders are the
//     module's first, its coldest), across the session's starts (CAL_SETTLE_QUANTA a start at most); then its decision
//     is made again from them and stands for the session. Nothing changes and nothing is said meanwhile: the next start
//     of that architecture says and applies it. So a one-model decision on a start's dozen readings lasts one pack at
//     most, as v0.6.0's burst did; a two-model one errs the safe way and is not settled.
//   · A start's capture that fails to load ends it at once: { calibrated: { maxModels, valid: false, reason:
//     'load-failed' } }.
// { calibrate: 'again' } forgets the session's decisions (a page that knows its next pack is another architecture):
// with a { load } that pack's start times; while sounding, one model is timed over CAL_AGAIN renders and the decision
// said as at a start. With calibrate: false there is no start: processorOptions.maxModels (2 if absent), knobs at once.
// { measure } still times a burst on demand ({ measured }): it stalls the context for ~CAL_MS, the page's own choice.
//
// THE QUIET WARM (fc_rig_warm_quiet, ABI 5; processorOptions.quietWarm, on with the start). A capture is not heard
// before its receptive field of input has run through it — ~135 ms of a site WaveNet at 48 kHz. When the input has been
// exactly silent for all of that (at PLAY, after a PARK; in a pause only once the tone stack's tail has decayed to an
// exact 0 — hundreds of ms after the last note — and a capture's whole field more), it is known: silence. So after
// each render the warming capture is fed blocks of silence ahead of time, as many as fit WARM_SHARE of the quantum with
// the render by an upper bound on what a render of its architecture costs (CAL_PAD; WARM_FIRST until CAL_RENDERS are
// timed), and it opens after a few quanta — bit-identical, from the moment the live warm-up would have opened, to what
// that would have played (RigAbiTests: the riff after it, sample for sample). Mid-riff the input is not silence,
// nothing is warmed ahead and the live warm-up stays. A quantum that warmed runs no job, from pump or from an answer.
// (v0.6.0: PLAY and every channel cost ~266 ms of silence — warm-up 135, burst 30, CAL_HOLD 80, ramp 5 — the burst
// stalled the context and WebKit dropped out a second time 16–40 ms after it, under the hold.)
//
// AN ERROR IS NOT AN END. A setup or runtime error — no module (the bytes retry included: { error, retry:
// 'wasmBytes' }, as fcguitarfx), a module older than this processor, a refused prepare, a render quantum of another
// size than the one prepared, a trap inside the module — posts ONE { error }, and from then on the input passes
// through dry and process() keeps answering true. ONLY { dispose } ENDS IT. After a trap the instance is not freed:
// the module's state is unknown, and the whole module is dropped instead. Until the module is up, the dry passes too.
// A load that fails is not an error: { loaded, ok: false }, and the player's law decides what sounds.
//
// THE AUDIO THREAD RUNS THE LOADS. The port's handler and process() share the audio thread, so a job's heavy half —
// building a capture's network — runs inside the quantum it is run in: ~0.25–1 ms since ABI 5 (v0.7.0; 22–49 ms
// before). So the processor runs AT MOST ONE JOB A QUANTUM — the period from one process() to the next, the handler's
// answers included: a job whose bytes arrive after one already ran in that period runs at the next quantum — and the
// player's own dip hides the swap itself.
//
// THE PRELOAD (processorOptions.preload, ABI 4 — fc_rig_set_preload) FILLS THE PACK IN THE BACKGROUND (v0.7.0).
// `preload: true` (the whole pack: RigPlayer::kPreloadWholePack), or a number (a cap on the prepared copies): at every
// pack the player prepares the pack's captures — the panel sounding first, then the pack's other panels nearest first,
// within the cap (fc_rig_abi.h, ABI 4) — through the same { need } and the same jobs, one a quantum, WHILE IT PLAYS. The
// law's own ask always goes first: the start (and every { load }) sounds as soon as the capture — or the two, the blend
// — that the law needs at the current panel is held and warmed, as without the preload, and a turn of DRIVE to a copy
// not filled yet loads it at the next quantum — at a job's cost (~0.25–1 ms) when the player holds its file, which is
// why the page should hand the pack's files in with the pack (THE FILES UP FRONT): the player hands out one job at a
// time, so without them a turn of DRIVE that reaches a copy while the fill waits for another's { need } waits for that
// round trip first, then for its own. When the whole set is prepared it posts { preloaded: { pack, total, failed,
// quanta } } (quanta: those the fill ran in, PARK's excluded) — a report, nothing waits for it: it usually comes AFTER
// { calibrated } (it came before it until v0.6.0), and a { load } or { unload } before the set is filled ends that
// fill without one — the next pack's { preloaded } names its own pack. From then on
// neither a turn of the gain dial nor a switch within the set asks for a load: no { need }, no job — a capture taken is a
// prepared one. Under a cap, a switch to a panel outside the set loads only what the law asks for, as without the
// preload. The knobs land at once, those that came with the processor's options and before the module was up included
// (so the set is made around the panel and the DRIVE the page asked for); only maxModels waits for the start's
// decision. (v0.6.0 prepared the whole set first, SILENT, and started after { preloaded }: 0.35–2.3 s of
// silence at PLAY and at each channel.)
// Memory: a copy is ~1.2 MiB of the module's memory for the site's WaveNets on a mono context: the whole pack is 11
// copies for X88-IR Crunch and V4 Sheriff Ch1, 44 for IR-X Ch2, 66 for IR-X Ch1 PowerAmp (fc_rig_abi.h).
//
// ZERO ALLOCATION PER QUANTUM. process() copies into views made once — and again only after the module's memory grew
// (which detaches every older view): checked at the top of every quantum (a load grows it) and again after the render,
// before copying out (a capture whose architecture allocates while it renders — LSTM and ConvNet in the pinned NAM,
// fc_rig_abi.h — may grow it there) — calls the module, copies back, and asks for a job: no array, no closure, no
// object per quantum while nothing is loading. A knob value that is not a number (a BigInt survives postMessage) is
// ignored, never a throw.

import createFcRig from './fcrig.mjs';

const ABI = 5;              // the oldest fc_rig ABI this processor speaks (2: the scope, stand-by, the feed; 3: the scope at the app's points; 4: the preload;
                            // 5: the files up front, and a load at a fraction of its cost — one network on a mono context, no zero prewarm)
const PRELOAD_WHOLE = 0x7fffffff;   // preload: true — RigPlayer::kPreloadWholePack, every copy of the pack
const WAIT_HELD = 16;       // preloading, the start's capture is the law's first job: nothing held this many quanta
                            // after a copy of the set failed — that capture failed
const BLEND_SHARE = 0.25;   // the blend (two models) is turned on when ONE render costs at most this share of a quantum
const CAL_RENDERS = 8;      // the start decides on at least this many timed renders (fewer: 'short', one model)
const CAL_AGAIN = 64;       // …and { calibrate: 'again' } while it plays times this many before it decides (4 x as many
                            // quanta at most: a neighbour put to rest drains first, ~50 quanta of renders not timable)
const CAL_GRACE = 8;        // a capture that opens before CAL_RENDERS renders were timed is timed on for at most this many
                            // quanta after it opens (then: 'short') — an outgoing capture's drain is not timed, so this
                            // is a bound, not a wait for renders that may not come (v0.7.0 review: 64 held it 170 ms)
const CAL_SETTLE = 64;      // THE SETTLE: after a sound opens on ONE model, its architecture is timed on, on renders
                            // that ran one network alone, until this many — then decided again for the next start
const CAL_SETTLE_QUANTA = 512;    // …for at most this many quanta of one start (a capture change's drain times nothing)
const SIG_LO = 0x3fffffff; // a signature's low 30 bits: what a timed render is compared by (a small integer, never boxed)
const CAL_PAD = 3;          // THE QUIET WARM's bound on a render's cost: the readings' sum plus this many clock ticks (or
                            // sqrt(renders), whichever is more) over the renders — ~95 % when every reading is 0 or 1 tick
const CAL_TICK_MS = 4;      // a Date.now never seen to move by less than this is too coarse to time with ('clock')
const WARM_SHARE = 0.6;     // THE QUIET WARM: a render plus its blocks of silence ahead take at most this share of a quantum
const WARM_MAX = 6;         // …and at most this many blocks a quantum
const WARM_FIRST = 1;       // …and this many while nothing is known of what a render costs
const CAL_MS = 30;          // { measure }: the burst lasts at least this long by Date.now
const CAL_MAX = 20000;      // …and at most this many renders
const CAL_FLOOR_MS = 10;    // a burst Date.now saw last less than this is not evidence (a 1 ms tick: 10 % at most)
const SPIN_MAX = 1e6;       // the wait for the burst's first tick gives up after this many reads (a stopped clock)
const NEED_MS = 10000;      // a { need } unanswered this long is answered by the processor itself, with nothing
const SCRATCH = 1024;       // module memory for the two strings of a knob (name, value): 511 UTF-8 bytes each
const SCOPE = 2048;         // the player's scope tap: the most one { scope } carries back

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
        this.calState = this.calibrate ? 'waiting' : 'off';   // waiting -> timing | opening -> settling -> done (THE START)
        this.decisions = new Map();   // the session's: architecture signature -> the { calibrated } it was decided with
        this.evidence = new Map();    // …and -> its renders timed: { n, ms, warm, settled, warmK } (evidenceOf)
        this.tick = Infinity;    // the finest step Date.now was seen to take (ms): the clock's own grain
        this.epoch = Date.now(); // THE CLOCK, read against this: the readings are small integers. (Date.now itself
                                 // allocates its heap number at every call in V8 — ~12 bytes a timed quantum in node,
                                 // measured — so a quantum is timed only in the start and THE SETTLE, both bounded.)
        this.tPrev = 0;          // the clock at the last timed render's start…
        this.tSeen = false;      // …once there was one
        this.decideAfter = 0;    // the start decides once the sound opens; 'again' while sounding: after this many renders
        this.restart();
        const qw = o.quietWarm === undefined ? this.calibrate : o.quietWarm === true;
        this.quietWarm = qw;     // THE QUIET WARM (fc_rig_warm_quiet), on with the start unless said otherwise
        this.queued = o.knobs ? [o.knobs] : [];               // knobs held until the module is up; then maxModels alone
        this.parked = o.knobs ? o.knobs.standby === true : false;   // PARK as asked: never queued (THE PARK)
        this.firstRig = o.rig || null;
        this.firstFiles = Array.isArray(o.files) ? o.files : null;   // { files } before the module was up (ABI 5)
        this.pack = 0;           // which load() the job out belongs to (the page echoes it)
        this.awaiting = null;    // the files[].id a job out is waiting for, or null
        this.armed = false;      // …and its bytes are in (or it gave up on them): it runs at the next chance (pump)
        this.spent = false;      // the quantum's spare time is spent — a job ran, or the quiet warm did — since the last
                                 // render: a job (pump, or an answer's arm) waits for the next quantum
        this.lawFirst = false;   // the pack's first job not delivered yet: the law's own ask (the start's capture)
        this.waited = 0;         // quanta since that { need } was posted
        const needMs = o.needTimeoutMs === undefined ? NEED_MS : num(o.needTimeoutMs);
        this.needMs = Number.isFinite(needMs) && needMs > 0 ? needMs : 0;
        this.needQuanta = 0;     // the same in quanta, once the quantum is known (start); 0 = never
        const pre = o.preload === true ? PRELOAD_WHOLE : num(o.preload);
        this.preload = Number.isFinite(pre) && pre >= 1 ? Math.floor(pre) : 0;   // the cap; 0 = no preload
        this.preloading = false; // the pack's set is being filled, in the background
        this.preQuanta = 0;      // quanta the fill ran in (PARK's excluded)
        this.waitHeld = 0;       // quanta the start has waited for its capture, preloading
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
        this.m = this.L = this.R = this.queued = this.firstRig = this.firstFiles = null;
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
            if (this.rig && this.parked) m._fc_rig_set_standby(this.rig, 1);   // before prepare: parked from sample one
            this.block = globalThis.renderQuantumSize || 128;
            this.needQuanta = this.needMs > 0 ? Math.max(1, Math.ceil(this.needMs * sampleRate / (1000 * this.block))) : 0;
            if (!this.rig || !m._fc_rig_prepare(this.rig, sampleRate, this.block, this.stereo ? 2 : 1)) {
                this.fail(`fc-rig: prepare refused (${sampleRate} Hz, ${this.block})`);
                return;
            }
            this.scratch = m._fc_rig_alloc(SCRATCH);
            this.scopeAt = m._fc_rig_alloc(2 * SCOPE * 4);   // the frame's two halves in the module, read from there
            m._fc_rig_set_max_models(this.rig, this.maxModels);
            if (this.preload > 0) m._fc_rig_set_preload(this.rig, this.preload);   // from the first pack on
            this.views();
            this.port.postMessage({ ready: true, abi });
            if (this.firstRig) this.load(this.firstRig);
            if (this.firstRig && this.firstFiles) this.put(this.firstFiles);
            this.firstRig = this.firstFiles = null;
            if (this.calState === 'off') this.drain();   // no start to wait for: the knobs land now, filling or not
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
        this.dropJob();
        this.lawFirst = true;
        const sweep = m._fc_rig_gain_dial_sweep(this.rig);
        this.preloading = ok && this.preload > 0 && m._fc_rig_preload_total(this.rig) > 0;
        this.preQuanta = 0;
        this.waitHeld = 0;
        // Knobs that waited for the module — the processor's options, and what came before it was up — land now,
        // before the pack's first job, so the law's first ask is the panel and the DRIVE the page asked for: only
        // maxModels waits for the start. (No start, calibrate: false: start() drains them all after this.)
        if (this.calState !== 'off' && this.queued.length > 0) {
            const q = this.queued;
            this.queued = [];
            for (const s of q) this.early(s);
        }
        this.port.postMessage({ pack: this.pack, ok, sweep });
    }

    /** Another pack, with calibration on: the start again (THE START). One model until the session has decided on
        anything; after that the model count it decided, until the capture held says which decision is its own. */
    rearm() {
        if (this.decisions.size === 0) this.setMax(1);
        this.restart();
    }

    /** The start from nothing: no architecture known for this pack yet. */
    restart() {
        this.calState = this.calibrate ? 'waiting' : 'off';
        this.sig = 0;            // the architecture this start times, opens with or settles (its signature; 0: none yet)
        this.sigLo = 0;          // …its low 30 bits (SIG_LO)
        this.ev = null;          // …its evidence (evidenceOf)
        this.warmK = WARM_FIRST; // THE QUIET WARM's blocks a quantum (an integer, kept: none computed per quantum)
        this.decideAfter = 0;
        this.sinceOpen = 0;      // quanta the sound has been open while the start still timed (or settled)
    }

    /** The session's evidence for an architecture: renders timed that ran one network of it alone — n of them, ms what
        Date.now saw them take, warm how many were still warming — and whether it is SETTLED (CAL_SETTLE renders timed
        after a sound opened: its decision is final for the session), and the quiet warm's budget it last gave. */
    evidenceOf(sig) {
        let ev = this.evidence.get(sig);
        if (!ev) this.evidence.set(sig, ev = { n: 0, ms: 0, warm: 0, settled: false, warmK: WARM_FIRST });
        return ev;
    }

    setMax(n) {
        this.maxModels = n;
        this.m._fc_rig_set_max_models(this.rig, n);
    }

    /** A knob message: { set: { maxModels, normalize, inputTrims, blendShape: [point, width], switch: { name: value },
        dial: { name: degrees }, gain: degrees, feed: dB } }, applied in that order. A `standby` in it is THE PARK's, taken
        by onMessage before the set is queued. */
    apply(s) {
        const m = this.m, rig = this.rig;
        if (s.maxModels === 1 || s.maxModels === 2) { this.maxModels = s.maxModels; m._fc_rig_set_max_models(rig, s.maxModels); }
        if (s.normalize !== undefined) m._fc_rig_set_normalize(rig, s.normalize ? 1 : 0);
        if (s.inputTrims !== undefined) m._fc_rig_set_input_trims(rig, s.inputTrims ? 1 : 0);
        if (Array.isArray(s.blendShape)) m._fc_rig_set_blend_shape(rig, num(s.blendShape[0]), num(s.blendShape[1]));
        if (s.switch) for (const k in s.switch) this.named(k, String(s.switch[k]));
        if (s.dial) for (const k in s.dial) this.named(k, num(s.dial[k]));
        if (s.gain !== undefined) m._fc_rig_set_gain_dial(rig, num(s.gain));
        if (s.feed !== undefined) m._fc_rig_set_feed_db(rig, num(s.feed));
    }

    /** { scope: ArrayBuffer } — the newest frame into the page's own buffer, which goes straight back (THE SCOPE). A
        buffer is always answered, before the module is up and after an error too: n 0. */
    scope(buf) {
        if (Object.prototype.toString.call(buf) !== '[object ArrayBuffer]') return;   // any realm's: instanceof is per realm
        const n = Math.min(SCOPE, Math.floor(buf.byteLength / 8));
        let got = 0, frames = 0;
        if (this.rig && !this.failed && n > 0) {
            const m = this.m;
            got = m._fc_rig_scope_read(this.rig, this.scopeAt, this.scopeAt + 4 * n, n);
            frames = m._fc_rig_scope_frames(this.rig);
            const heap = m.HEAPF32, at = this.scopeAt >> 2, out = new Float32Array(buf, 0, 2 * n);
            for (let i = 0; i < got; ++i) { out[i] = heap[at + i]; out[n + i] = heap[at + n + i]; }
        }
        this.port.postMessage({ scope: buf, n: got, frames }, [buf]);
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

    /** While the start decides: every knob lands now — the gain dial too: the capture the start times is the one the
        DRIVE asks for — and maxModels alone waits for the decision, in order. */
    early(s) {
        const { maxModels, ...now } = s;
        this.apply(now);
        if (maxModels !== undefined) this.queued.push({ maxModels });
    }

    drain() {
        const q = this.queued;
        this.queued = [];
        if (q) for (const s of q) this.apply(s);
    }

    onMessage(d) {
        if (!d || this.done) return;
        if (d.dispose) { this.dispose(); return; }
        if (d.scope !== undefined) {
            try {
                this.scope(d.scope);
            } catch (e) {   // a trap inside the read: said, and the buffer still goes back (n 0)
                this.trap(e);
                try { this.port.postMessage({ scope: d.scope, n: 0, frames: 0 }, [d.scope]); } catch (e2) { /* gone */ }
            }
            return;
        }
        // THE PARK: never queued. Mirrored here, applied now if the module is up, by start() otherwise.
        const park = d.standby !== undefined ? d.standby : d.set && d.set.standby !== undefined ? d.set.standby : undefined;
        if (park !== undefined) this.parked = park === true;
        if (this.failed) return;
        if (!this.rig) {   // not up yet: knobs wait, a pack replaces the first one
            if (d.set && this.queued) this.queued.push(d.set);
            if (d.load) { this.firstRig = d.load; this.firstFiles = null; }   // a new pack: the files of the old one go with it
            // No { pack } has been said yet, so a `pack` here names nothing this processor knows: kept only when it
            // comes with the { load } it belongs to, or carries no `pack` (the pack loaded last).
            if (Array.isArray(d.files) && (d.pack === undefined || d.load))
                this.firstFiles = (this.firstFiles || []).concat(d.files);
            return;
        }
        try {
            if (park !== undefined) this.m._fc_rig_set_standby(this.rig, this.parked ? 1 : 0);
            if (d.unload) { this.m._fc_rig_unload(this.rig); ++this.pack; this.dropJob(); this.preloaded(false); }
            if (d.calibrate === 'again') this.again(d.load !== undefined);
            if (d.load) {
                if (this.calibrate) this.rearm();
                this.load(d.load);
            }
            if (Array.isArray(d.files) && (d.pack === undefined || d.load || d.pack === this.pack)) this.put(d.files);
            if (d.file !== undefined) this.bytes(d);
            if (d.set) {
                if (this.calState === 'waiting' || this.calState === 'timing' || this.calState === 'opening') {
                    this.early(d.set);   // everything lands now; maxModels waits for the decision
                } else {
                    this.apply(d.set);   // decided (or no start): at once, whether the fill is still running or not
                }
            }
            if (d.reset) this.m._fc_rig_reset(this.rig);
            if (d.measure) this.port.postMessage({ measured: this.burst() });
            if (d.read) this.read();
        } catch (e) {
            this.trap(e);
        }
    }

    /** THE FILES UP FRONT (ABI 5 — fc_rig_put_file): { files: [{ file, bytes }, …] } in the { load } message or after it
        hands the pack's files in; the player keeps each, so no job ever needs it and no { need } is posted for it. A file
        left out is asked for with { need } as ever. A { files } carrying `pack` other than the current one is stale and
        dropped; without `pack` it belongs to the pack loaded last. A job already waiting for one of them runs now. */
    put(files) {
        const m = this.m, rig = this.rig;
        for (const f of files) {
            if (!f || typeof f.file !== 'string' || !f.bytes || !(f.bytes.byteLength > 0)) continue;
            if (!putUtf8(m.HEAPU8, this.scratch, f.file, SCRATCH)) continue;
            const p = m._fc_rig_put_file(rig, this.scratch, f.bytes.byteLength);
            if (p) m.HEAPU8.set(new Uint8Array(f.bytes), p);   // HEAPU8 read after the call: it may have grown the memory
        }
        if (this.awaiting !== null && !this.armed && m._fc_rig_job_needs_bytes(rig) === 0) this.arm();
    }

    /** The page's answer to { need }: the bytes in, the job run and delivered (arm). A stale answer is dropped, and so
        is a second answer to the same { need }. */
    bytes(d) {
        if (this.awaiting === null || this.armed || d.pack !== this.pack || d.file !== this.awaiting) return;
        const m = this.m, rig = this.rig;
        const n = d.bytes ? d.bytes.byteLength : 0;
        const p = n > 0 ? m._fc_rig_job_bytes(rig, n) : 0;
        if (p) m.HEAPU8.set(new Uint8Array(d.bytes), p);   // HEAPU8 read after the call: it may have grown the memory
        this.arm();
    }

    /** The job out has its bytes: it runs now — unless the player stands by (THE PARK: nothing loads under it) or a
        job already ran since the last render (one a quantum); then the next quantum that may run one runs it (pump). */
    arm() {
        this.armed = true;
        if (!this.parked && !this.spent) this.finish(this.awaiting);
    }

    /** No job out any more (a { load }, an { unload }: the player dropped it); a late answer to it is stale. */
    dropJob() {
        this.awaiting = null;
        this.armed = false;
    }

    finish(file) {
        const m = this.m, rig = this.rig;
        const law = this.lawFirst || !this.preloading;   // the law's own ask: the pack's first job, or any without the fill
        this.dropJob();
        this.spent = true;
        this.lawFirst = false;
        m._fc_rig_job_run(rig);
        const ok = m._fc_rig_job_deliver(rig) === 1;
        this.port.postMessage({ loaded: file, ok });
        if (!ok && law && this.calState === 'waiting') this.failStart();   // nothing to time
    }

    /** After every quantum the player is not parked: housekeeping and at most one job — the one out once its bytes are
        in, or, when the player hands one out (the law's ask first, then the fill), a new one: run here at once if the
        player has the bytes, otherwise asked of the page. None if a job already ran since the last render. */
    pump() {
        if (this.spent) return;
        if (this.armed) { this.finish(this.awaiting); return; }
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

    /** One render of the start, timed (Date.now around fc_rig_process, nothing else): evidence when it ran exactly one
        network alone, of the architecture the start is about (an outgoing capture's drain is another's, and not timed).
        The clock's grain is learnt on the way — the finest step it was seen to take. */
    timeRender(t0, t1) {
        const m = this.m, rig = this.rig;
        if (this.tSeen && t0 > this.tPrev && t0 - this.tPrev < this.tick) this.tick = t0 - this.tPrev;
        if (t1 > t0 && t1 - t0 < this.tick) this.tick = t1 - t0;
        this.tPrev = t0;
        this.tSeen = true;
        const run0 = m._fc_rig_run_blocks(rig, 0), slot = run0 === 1 ? 0 : 1;
        if (run0 + m._fc_rig_run_blocks(rig, 1) !== 1 || m._fc_rig_pair_blocks(rig) !== 0) return;
        if (this.sig === 0) {   // { calibrate: 'again' } while sounding: what sounds is timed
            const sig = this.calState === 'timing' ? m._fc_rig_signature(rig, slot) >>> 0 : 0;
            if (sig === 0) return;
            this.sig = sig;
            this.sigLo = sig & SIG_LO;
            this.ev = this.evidenceOf(sig);
        }
        if ((m._fc_rig_signature(rig, slot) & SIG_LO) !== this.sigLo) return;
        const ev = this.ev;
        ++ev.n;
        ev.ms += t1 - t0;
        if (m._fc_rig_warm_blocks(rig) > 0) ++ev.warm;
        this.warmK = this.warmBudget(ev);
    }

    /** THE START, before the quiet warm takes its budget (waiting, not parked): the capture held names its architecture.
        A decision of this session for it is taken as it stands ('opening'); otherwise one model is timed ('timing'). */
    identify() {
        const m = this.m, rig = this.rig;
        if (!m._fc_rig_held(rig, 0) && !m._fc_rig_held(rig, 1)) return;
        // Known when every capture held is of an architecture this session decided on (the fewer models wins).
        let known = null, knownSig = 0, unknown = 0;
        for (let i = 0; i < 2; ++i) {
            const sig = m._fc_rig_signature(rig, i) >>> 0;
            if (sig === 0) continue;
            const d = this.decisions.get(sig);
            if (!d) { unknown = sig; break; }
            if (known === null || d.maxModels < known.maxModels) { known = d; knownSig = sig; }
        }
        if (known && unknown === 0) {
            this.known = known;
            this.sig = knownSig;
            if (this.maxModels !== known.maxModels) this.setMax(known.maxModels);
            this.calState = 'opening';
        } else {
            this.sig = unknown;
            if (this.maxModels !== 1) this.setMax(1);
            this.calState = 'timing';
        }
        this.sigLo = this.sig & SIG_LO;
        this.ev = this.sig !== 0 ? this.evidenceOf(this.sig) : null;
        this.warmK = this.ev ? this.ev.warmK : WARM_FIRST;   // this architecture's own budget, never another's
    }

    /** THE START, after a render (not parked): it ends when the sound opens; then THE SETTLE, until its evidence is in. */
    startStep() {
        const m = this.m, rig = this.rig;
        if (this.calState === 'settling') {
            const ev = this.ev;
            if (ev.n >= CAL_RENDERS && !this.decisions.has(this.sig)) this.decisions.set(this.sig, this.decide(ev));   // after a 'short'
            if (ev.n >= CAL_SETTLE) {   // final for the session: what its next start says and applies (nothing changes now)
                ev.settled = true;
                this.decisions.set(this.sig, this.decide(ev));
                this.calState = 'done';
            } else if (++this.sinceOpen >= CAL_SETTLE_QUANTA) {
                this.calState = 'done';   // its evidence stays: the next start of this architecture settles on
            }
            return;
        }
        if (this.calState === 'waiting') {   // identify() runs before the warm; here only a capture that never came
            if (this.preload > 0 && this.awaiting === null && ++this.waitHeld > WAIT_HELD) this.failStart();
            return;
        }
        if (m._fc_rig_audible(rig) !== 1) return;
        if (this.calState === 'timing') {   // decided once enough renders were timed, or the clock is known too coarse
            const coarse = Number.isFinite(this.tick) && this.tick > CAL_TICK_MS;
            const n = this.ev ? this.ev.n : 0;
            if (!coarse && n < Math.max(CAL_RENDERS, this.decideAfter)
                && ++this.sinceOpen < Math.max(CAL_GRACE, 4 * this.decideAfter)) return;   // 'again': renders, not quanta
        }
        this.open();
    }

    /** A decision from evidence: the { calibrated } said for it. */
    decide(ev) {
        const n = ev ? ev.n : 0, ms = ev ? ev.ms : 0;
        const quantumUs = 1e6 * this.block / sampleRate, clock = this.tick <= CAL_TICK_MS;
        const renderUs = n > 0 ? 1000 * ms / n : 0, share = renderUs / quantumUs;
        const valid = clock && n >= CAL_RENDERS;
        const said = { renderUs, quantumUs, share, renders: n, elapsedMs: ms, runs: n, pairs: 0, warm: ev ? ev.warm : 0,
                       valid, maxModels: valid && share <= BLEND_SHARE ? 2 : 1 };
        if (!valid) said.reason = clock ? 'short' : 'clock';
        return said;
    }

    /** The sound opens: the decision — the session's own for this architecture, or made now from the timed renders — is
        applied and said ({ calibrated }), and the maxModels that waited land after it, in order. Then THE SETTLE: an
        architecture not settled yet is timed on, and its decision made again from renders timed after an open (the
        start's own are the module's first, its coldest) — for the next start; nothing changes now, nothing is said. */
    open() {
        const ev = this.ev;
        let said;
        if (this.calState === 'opening') {
            said = this.known;
        } else {
            said = this.decide(ev);
            if (said.reason !== 'short' && this.sig !== 0) this.decisions.set(this.sig, said);   // too few: timed on
            if (ev && ev.n >= CAL_SETTLE) ev.settled = true;          // { calibrate: 'again' }: CAL_AGAIN renders
            else if (ev && said.maxModels === 1) { ev.n = ev.ms = ev.warm = 0; }   // the settle counts from the open
        }
        if (this.maxModels !== said.maxModels) this.setMax(said.maxModels);
        // Settled only on one model: a 1 may be the costly mistake (a machine that affords two told one), and one model
        // renders one network alone, which is what the settle times. A 2 errs the safe way (two renders of 0.8 ms still
        // fit), and two networks a render would time nothing.
        this.calState = ev && !ev.settled && said.maxModels === 1 && this.tick <= CAL_TICK_MS ? 'settling' : 'done';
        this.sinceOpen = 0;
        this.port.postMessage({ calibrated: said });
        this.drain();
    }

    /** The start's capture could not be loaded: nothing to time — said at once, at the model count the session has. */
    failStart() {
        if (this.decisions.size === 0 && this.maxModels !== 1) this.setMax(1);
        this.calState = 'done';
        this.port.postMessage({ calibrated: { maxModels: this.maxModels, valid: false, reason: 'load-failed' } });
        this.drain();
    }

    /** { calibrate: 'again' }: the session forgets what it decided. With a { load } the new pack's start times its
        capture; while sounding, one model is timed over CAL_AGAIN renders and the decision said as at a start. */
    again(withLoad) {
        this.decisions.clear();
        this.evidence.clear();
        if (!this.calibrate || withLoad || this.calState === 'waiting') return;
        if (this.maxModels !== 1) this.setMax(1);
        this.restart();
        this.calState = this.m._fc_rig_held(this.rig, 0) || this.m._fc_rig_held(this.rig, 1) ? 'timing' : 'waiting';
        this.decideAfter = CAL_AGAIN;
    }

    /** THE QUIET WARM's blocks a quantum for an architecture: a render and its blocks of silence within WARM_SHARE of the
        quantum, by an UPPER BOUND on what one render of it costs — never the bare mean of 0-or-1 ms readings, which eight
        zeros put at nothing (a 0.3 ms render reads eight zeros one session in eighteen). WARM_FIRST while fewer than
        CAL_RENDERS are timed; kept in the evidence, so another architecture's start never takes this one's. */
    warmBudget(ev) {
        if (ev.n >= CAL_RENDERS && this.tick <= CAL_TICK_MS) {
            const us = 1000 * (ev.ms + this.tick * Math.max(CAL_PAD, Math.sqrt(ev.n))) / ev.n;
            ev.warmK = Math.max(0, Math.min(WARM_MAX, Math.floor(WARM_SHARE * 1e6 * this.block / (sampleRate * us)) - 1));
        }
        return ev.warmK;
    }

    /** The set is filled (or the pack went): said once — a report; neither the start nor a knob waits for it. */
    preloaded(say = true) {
        if (!this.preloading) return;
        this.preloading = false;
        if (say) this.port.postMessage({ preloaded: { pack: this.pack, total: this.m._fc_rig_preload_total(this.rig),
                                                      failed: this.m._fc_rig_preload_failed(this.rig), quanta: this.preQuanta } });
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
            preload: { total: m._fc_rig_preload_total(rig), pending: m._fc_rig_preload_pending(rig),
                       failed: m._fc_rig_preload_failed(rig) },
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
            // THE START times its renders — the render alone: the jobs and the quiet warm run outside it.
            const timed = !this.parked && this.calState !== 'done' && this.calState !== 'off';
            if (timed) m._fc_rig_clear_counters(this.rig);
            const t0 = timed ? Date.now() - this.epoch : 0;
            m._fc_rig_process(this.rig, this.pL, this.pR, this.block);
            if (timed) this.timeRender(t0, Date.now() - this.epoch);
            this.spent = false;   // a new quantum: one job may run before the next render (pump, or an answer's arm)
            if (this.L.buffer !== m.HEAPF32.buffer) this.views();   // the render grew it (an allocating architecture)
            // The output is the player's own, always: its law keeps a capture silent until it is warm and fades it in
            // (the dip); parked, its fade, then the wire. Nothing here holds it back any more (v0.7.0).
            oL.set(this.L);
            if (oR) oR.set(this.R || this.L);
            if (!this.parked) {   // THE PARK: no job, no { need }, no warm, no count — all resume on the return
                if (this.calState === 'waiting') this.identify();   // the warm's budget is the capture's own
                // THE QUIET WARM first; a quantum it ran in runs no job, from pump or from an answer (the budget is the
                // quantum's).
                const warmed = this.quietWarm ? m._fc_rig_warm_quiet(this.rig, this.warmK) : 0;
                if (warmed > 0) this.spent = true; else this.pump();
                if (this.preloading) {
                    ++this.preQuanta;
                    if (m._fc_rig_preload_pending(this.rig) === 0) this.preloaded();
                }
                if (this.calState !== 'done' && this.calState !== 'off') this.startStep();
            }
        } catch (e) {
            this.trap(e);
            if (inL) { oL.set(inL); if (oR) oR.set(inR); }
            else { oL.fill(0); if (oR) oR.fill(0); }
        }
        return true;
    }
}

registerProcessor('fc-rig', FcRigProcessor);
