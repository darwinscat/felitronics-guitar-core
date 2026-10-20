# felitronics::rigplayer — the pack player, and what a host owes it

One device of a `.orbitrig` pack, playing. Hand it the pack's structures (`namz::rig::Rig`, as
`namz::rig::loadRigManifest` returns them) and a way to get a file's bytes, tell it where the knobs
stand, feed it audio. Header-only, JUCE-free, no thread of its own. Moved here from OrbitCapture NAM
so the capture app and a plugin play through ONE player; the app is the reference host.

Signal order, per block:

    in → dry copy → pre tone (bands, curve) → chain trim → ┬→ trim A → model A → align ─┐
                                                           └→ trim B → model B → align ─┴→ mix
       → post tone (bands, curve) → dry/wet blend → out

- WHICH files sound is `namz::rig`'s policy (a turned control is law) joined to `pickBlend` along
  the gain dial — `RigSelection.h`, pure.
- WHEN a model may be replaced, and how loudly each is heard, is `felitronics::nam::BlendLaw` — run
  once per block on the audio thread, the only writer of the weight.
- A tone knob plays as the pack describes it: `sections` become biquads, `positions` (a curve)
  become one minimum-phase FIR per side. A blend knob mixes the DI through the dry path's response.
- The models' offsets come from the pack (`lag_samples`); a pack without them can be measured by the
  host (`AlignmentTable`) and handed in.

## Threads — the whole contract

| call | thread |
|---|---|
| `prepare(rate, maxBlock, channels ≤ 2, scopeSamples = 2048)` | message, never while `process()` runs |
| `load(rig, source)` / `unload()` | message |
| `setDial(name, degrees)` / `setSwitch(name, value)` / `setToneOverride` / `setBlendShape` / `setNormalize` / `setAlignment` / `setColdAfterSeconds` / `setMaxModels` / `setPreload` / `setPreloadIdle` / `setStandby` / `setFeedDb` | message |
| `service()` then `takeLoadJob()` | message, from a timer — a few times a second at least |
| `RigPlayer::run(job)` | ANY thread but the audio one — a pool, a worker, or right here |
| `deliver(loaded)` | message — strictly; it installs into the stages |
| `process(io, channels, n)` | audio — locks nothing, touches no file; allocates nothing with Linear and WaveNet captures (LSTM and ConvNet allocate inside the pinned NAM) |
| every read-out (`liveMix`, `heldFileId`, `soundingLoudness`, `selection`, `knobValue`, `runBlocks`, `pairBlocks`, the drawing readers…) | message |
| `scopeRead(dry, wet, n)` / `scopeFrames()` / `standingBy()` | any — lock-free beside `process()`; never beside `prepare()` |

**Loading is a job the host runs.** `service()` never loads. When the law wants a model,
`takeLoadJob()` hands out ONE job at a time — slot, `files[].id`, the bytes if already fetched, the
`ModelSource`, the numbers the stages were prepared with. `run(job)` is a pure function of it: the
bytes through the source when the job has none, then the heavy half of a load
(`NamStage::prepareModel` — parsing, two instances, the prewarm; some twenty milliseconds for a
WaveNet). `deliver(loaded)` is the light half. **A job taken MUST come back through `deliver()`** —
with a null model if it failed — or the law waits for it forever. A job that returns after `unload()`
is dropped by the player; the host need not track it. The bytes a source answered are kept, so a file is
fetched once — unless the answer was EMPTY: that is the source's "could not fetch it", not the file, and
the file is asked for again the next time the law wants it. `serviceHere()` runs the same path on one
thread, for a host without a worker and for a test.

**`ModelSource` is called from wherever the host runs `run()`.** Make it safe to call off the message
thread: it is called at most once at a time (one job in flight), but not from the thread that opened
the pack. The reference host reads from a `juce::ZipFile` kept open, one entry per call.

**A slot at rest goes cold.** A dial parked on a capture keeps the neighbouring capture in the other
slot at exactly zero, warm and waiting — and its network used to run every block for nothing (measured
in OrbitAmp's block, prepared for two planes and fed one: 4.5 % of a P-core, 14 % of an E-core). After
`RigPlayer::kColdAfterSeconds` (2 s) at exactly zero under an unchanged request the slot goes COLD: its
model is not run — nor mixed — and it stays loaded: nothing is fetched, nothing freed; its delay
line is cleared as it falls asleep, as a landing clears it. The law owns the flag (`BlendState::cold`)
and wakes the slot on the first block of the next change of request, warm-up first — the same
re-landing a load ends with, with the model's own field — so nothing unfed is ever heard, and the
first turn after a rest trails the hand by one warm-up (some 130 ms for a WaveNet, then the ordinary
slew; a model that declares no field is heard at once). A slot with any weight is never cold: between two captures both models run, on one
they do not. `setColdAfterSeconds(s)` sets the rest (zero or less = never); `slotCold(i)` and
`coldBlocks(i)` (since `clearCounters()`) read it out beside `warmBlocks()`, for a dump or a badge.

**One model at a time, for a host that cannot afford two.** `setMaxModels(2)` is the default and the
player above, bit for bit — but for the first blocks after a load: the law's gain now waits until no
slot that is empty, in flight or unfed carries weight, where v0.2.0 let an empty slot's raw DI be heard
while the weight left it. `setMaxModels(1)` — meant for a browser's audio thread on a slow
machine, which starts with one and turns the blend on only once it has measured that two fit:

- the gain dial is STEPPED to the nearest capture (nearest by the handover `setBlendShape` sets), and
  `selection()` says so — with a deadband (`kStepDeadband`, a tenth of the gap either side of the
  handover) around the capture sounding, so a hand resting on the handover does not dip at every
  tremor; the other slot is asked for nothing, so no neighbour is ever loaded and ONE network runs on
  audio, plus the outgoing capture's bounded drain as it rests (its stage feeds the lane its receptive
  field of silence once), which overlaps the incoming capture's warm-up;
- a change of capture is a DIP, not a crossfade: the output of the models falls to exactly zero over
  `kDipSeconds` (15 ms — OrbitAmp's `BypassFade`, the app's one fade for a link that replaces the
  signal), the outgoing capture rests, the incoming one is loaded (one job) — or woken, if its slot
  still holds it — and fed its receptive field while nothing is heard, and then the sound rises over
  the same 15 ms. So a capture change costs a hole of the rest of the fall's block plus the incoming
  model's warm-up, rounded up to blocks (some 130 ms for a WaveNet; a capture with no field is heard
  again on the block after the fall);
- slots go by the PARITY of a capture's place on the dial, so crossing a link — one file at two
  angles, the pack's `0` and `60` for instance — is a change of slot here: a dip and a load of the
  same file, where two models just glide;
- `1 → 2` while playing asks for the neighbour at weight zero, and nothing of it is heard until it is
  fed: on a capture the output then does not change until the dial moves; between two captures the
  weight glides — a quarter per block at most — to the dial's own blend, with no hand on the dial.
  `2 → 1` on a capture rests the neighbour and the output does not change; between two captures it
  dips to the nearer one (the one place two networks run on audio in this mode: through the dip's
  fall, because both were sounding when it was asked);
- `runBlocks(slot)` and `pairBlocks()` count the blocks in which a slot's network, and both, actually
  ran — on audio or draining a rest — since `clearCounters()`: what a host measures its budget with.

The order, the invariants and the one exception are `nam::blendStepSingle`'s; the law owns the dip
as it owns the weight.

## The preload — every capture of the pack, prepared before it is asked for

For a host whose loads stall its audio: a browser AudioWorklet runs the load job ON the audio thread (24 – 49 ms a
capture in Chromium), so a turn of the gain dial across captures underran at every capture it reached.
`setPreload(true, maxModels = kPreloadWholePack)` — off by default, and off the player is the one above, bit for bit —
takes effect at the next `load()`:

- the player hands out jobs for its SET — the same `LoadJob`, run and delivered as ever, one at a time — until every
  copy in it is prepared (`preloadProgress()`: `total`, `settled`, `failed`, `held`, `ready()`); a job the law asks
  for meanwhile goes first. No job before `prepare()`: there are no numbers to prepare a copy at;
- from then on neither a turn of the gain dial nor a switch within the set hands out a job: a slot that wants a
  capture takes its prepared copy at the next `service()` — the instant a delivered job would have landed it — and
  the law warms and crossfades it, or dips to it with one model, as before. The slot runs the copy's own `NamStage`;
  the two slots' own stages stay empty;
- THE SET is the panel sounding — every (capture, slot) the plan can name along the dial, asked of the plan itself
  at every knot, between every two and at both ends, with two models and with one — whole, whatever the cap; then
  the pack's other panels (the captured combinations of the other controls), nearest first (one control away, then
  two; ties in the manifest's order), while the set stays within `maxModels`. The default, `kPreloadWholePack`, is
  every panel. One copy per (capture, slot): the two slots hold one file at once at the dial's ends and across a
  link. The site's packs: eleven positions, nine files, **eleven copies** a panel. A panel is told apart by its
  knots' files and the other controls' settings, so a pack with no gain dial has panels too;
- UNDER A CAP, a switch to a panel outside the set makes the set again around it: copies it keeps stay, the rest
  are freed (one a slot runs, once the slot moves off it), and the copies it lacks are handed out only as the law
  asks for them — as many jobs as the player without the preload runs there, never more; they load while it plays.
  `setPreloadIdle(true)` — the host's word that a job costs its sound nothing now (silent, or jobs on a worker) —
  hands out the rest as before ready;
- a copy settles prepared, failed (not a capture) or unfetched (the source answered nothing): the set is ready without
  the last two, the law is told the load failed when it asks, and an unfetched file is a job again then — as without
  the preload. That and a panel past the cap are the only jobs after ready;
- A COPY IS FREED WHOLE — its `NamStage` with its capture — when it leaves the set or `unload()` takes the pack: at
  once if no slot ever ran it; otherwise by the first `service()` after the audio thread has entered `process()`
  again (`process()` ticks an audio clock before it reads a slot's stage, and a retired copy is freed once the clock
  has passed the moment the slots let go of it). A pool stage is never cleared and reused: `NamStage` frees what it
  retires only from its own `process()`, which a copy no slot runs never gets. Measured on the fixture: unload,
  two blocks and `service()` leave 0.3 % of what the pack and its set held; 200 evicting switches grow the heap by
  16 KB (a set is 509 KB);
- `latencySamples()` and the `sounding*` read-outs dereference the slots' stages: message thread (or inside
  `process()`), never a third thread;
- stand-by, `setMaxModels`, the cold rest, `reset()`, a re-prepare at another rate (the copies are re-prepared in
  place: no job), the scope are untouched.

Memory: one copy is one capture prepared — **2.8 MB** natively and in the wasm module for the site's WaveNets
(`NamStage` builds two NAM instances, stereo, whatever the host plays). The whole pack: **11 copies (~31 MB)** for
X88-IR Crunch and V4 Sheriff Ch1, **44 (~123 MB)** for IR-X Ch2, **66 (~185 MB)** for IR-X Ch1 PowerAmp (the wasm
module read 179.4 MiB with it, against 24.1 MiB without a preload). A cap trades that for loads while playing.

A copy taken again holds what its last visit left in it, where a load starts clean. A capture with a field owes the law
its warm-up, which feeds that memory out before it is heard — so at the model rate the preloaded player is bit for bit
the plain one on the same dial path for WaveNets and direct Linears, with two exceptions: a dense `Linear` on NAM's
partitioned FFT keeps its engine's clock, which nothing rewinds (`NamStage.h`) — about 1e-7 against a fresh load
(8.1e-8 on a 2001-tap fixture, guarded at 1e-6) — and an LSTM's cell is not emptied by anything finite: a recurrent
copy taken again starts from its last visit (0.054 on the decaying-cell fixture). A capture with no field is heard at
once, and off the model rate its rate-matcher would replay the tail of its last visit (0.0139 at 44.1 kHz on the
fixture): it is drained to the silence state (`NamStage::reset` — its matcher's few samples) at the landing, which
leaves the matcher's resumption floor (5.5e-6).

## The scope tap — OrbitAmp's ScopeTap, inside the player

The pictures of a device (the app's SHAPE) want what went in and what came out, sample beside sample. The app taps
around its whole player (`CapturedBlock.h:543-556`: the block's input copied before the drive and before
`player.process`, the scope written after it), and so does this — a ring of the newest `kScopeSize` (2048 —
`ScopeTap::size`; another length at `prepare()`, 0 = none) samples of channel 0, written once per block the device runs:

- **dry** — the input as the block was handed it, times the feed (`setFeedDb`, the app's block IN,
  `PluginProcessor.cpp:1497`) and nothing else: before the pack's input level and the pre tone, UNDELAYED;
- **wet** — the player's output: the pre tone, the models, the post tone, the blend knob, the pack's output level —
  before the stand-by fade, which the app blends after the block has tapped (`PluginProcessor.cpp:1538`).

So the two stand as they do in the app, and no closer: the wet trails the dry by a capture's own lag (the player never
takes it out of the sound either) and, at a host rate other than the pack's, by `latencySamples()` — the app's picture
is misaligned by the rate-match exactly so, and this follows it. Every knob of the pack moves the picture; the input
level and the output level only as far as they change its shape, since the reader matches levels (below).
(v0.5.0 tapped around the models — the dry after the input level and held back by `latencySamples()`, the wet before
the post tone, the blend knob and the output level — where the site's tap sat around TONE3000's node.)

**The feed.** `setFeedDb(db)` is the bench's drive ON TOP of the device's input level — the app's block IN, which the
app applies outside its player. A host that has no such stage of its own hands it here (fc_rig does), so the stand-by
wire is taken before it and the scope's dry after it; the sound is one gain, the input level plus the feed — the same
bytes as stating the feed as the host's hand (`setHostInputDb(stageInputDb() + db)`). Kept across `load()` and
`unload()`; a non-finite dB is refused; message thread.

`scopeRead(dry, wet, n)` copies the newest `min(n, size, written)` of both, oldest first, with the wet scaled to the
dry's RMS — the app's `ScopeTap::read` (`ScopeTap.h:42-72`), which matches in the reader: "a boost that is merely
louder would drown" what the device did. It answers how many it copied; 0 = nothing written since `prepare()`.
`scopeFrames()` counts the samples written, so a reader knows a frame is fresh. No lock and no torn frame: the writer
claims what it is about to write, fences, writes relaxed atomic floats and publishes; the reader copies between the
published end and the claim and throws the copy away if the writer may have reached it (two blocks of slack; retried
`kScopeTries` times, then 0). That 0 does not leave the caller's arrays as they were — every try copies into them,
so they then hold a copy the writer may have torn, not a frame: a reader that wants to keep its previous frame reads
into scratch of its own and copies out on a non-zero answer (the worklet does). `scopeFrames()` is the count when it
is read, not tied to the snapshot a concurrent reader got. A block processed with no input channel writes no tap. The
audio side pays one fence and two stores a block, and a copy of channel 0's block for the dry.

## Stand-by — OrbitAmp's PARK

`setStandby(true)`: the output crossfades over 15 ms (`kStandbyFadeMs`, the app's `BypassFade`, linear, its own
arithmetic) to a WIRE — the input at `latencySamples()`, unity, none of the pack's levels — and once the fade is down
the device is not run: no models on audio, no tone, no law, no ramps, no scope (`standingBy()` says so). `false` fades
it back in over the same 15 ms. Knobs set meanwhile are taken on the first block back as the app's frozen player takes
them (the law steps from where it stood, bands glide, gains ramp); the input level arrives instead of ramping — the
app's block IN, `lastInGain = inTarget` (`PluginProcessor.cpp:1494-1495`).

Where it is stricter than the app: the app stops calling the player, so its networks and filters replay what they held
when PARK came (0.399 out of digital silence on a 1500-sample capture, measured). Here a pause is silence (core's law
11c): when the fade ends the filters, convolvers, the dry ring and the slot delay lines are stopped (`clearAudioState`,
not a restart), and both stages get the width-zero call every stood-by block — each drains what it owes once, as a
cold slot does, and costs nothing after.

## What a host must do

1. `prepare()` in the host's own prepare; re-prepare on a rate or block change (the filters are
   designed for a rate; a model built for another rate is prepared again on install).
2. `load()` once per pack/device; the player opens on the pack's defaults (`namz::rig::defaultSettings`)
   and, if that combination was never captured, on the closest one that was.
3. A timer: `service()`, then `while (auto job = takeLoadJob()) …` — run it where you like, bring it
   back with `deliver()` on the message thread.
4. Report `latencySamples()` to the host after every `deliver()` and `prepare()` — it is the models'
   rate-matching; the alignment delays are relative and the reference model carries none.
5. Knobs by NAME, the pack's names: `setDial(name, degrees)` for anything with a sweep (a captured
   dial, a tone knob, a blend knob), `setSwitch(name, value)` for a token. `knobValue(name)` reads
   any of them back as the pack spells it; `dialDegrees()` is the crossfade dial's angle,
   `settings()` the captured combination.

## Levels — what is always applied, and the one thing a host may switch off

The pack's own two levels are applied **always**, from the moment `load()` reads them. They are what
the pack's author decided about this device: `chain[].input_db` is how hard the guitar is fed into it
— the working point, so it changes the sound and not the volume — and `chain[].output_db` is how loud
it leaves, so packs can be balanced against one another.

Where they sit is not decoration. `input_db` goes FIRST, ahead of the dry copy, because a blend knob
mixes one guitar with itself and both ends of that mix must be fed the same signal. `output_db` goes
LAST, after the mix, because it is one number for the whole stage and applied any earlier it would
ride the wet side alone and move the blend the pack states for this position.

`stageInputDb()` / `stageOutputDb()` read back what the PACK states. A host that only plays packs
needs nothing else — the player applies them and there is no switch.

**A host with a fader of its own states the WHOLE number, never a difference.** `setHostInputDb(db)` /
`setHostOutputDb(db)` say "play this device at that level"; the player uses the hand instead of the
pack's own, and `std::nullopt` hands the level back to the pack. The hand survives a `load()` — it
belongs to the bench, not to the pack.

This is not a convenience. A host that subtracts has to know which pack is loaded at the moment it
subtracts, and it reads that from its own document: the document changes when somebody edits it, when
a rebuild is in flight, when a device is switched mid-build. Every one of those leaves the level
wrong, silently, in the same class as the double application these keys were added to end. The player
is the only place that cannot disagree with itself about which pack it holds, so the arithmetic lives
here. `hostInputDb()` / `hostOutputDb()` read the hand back.

`setInputTrims(false)` switches off exactly one thing: `files[].input_db`, the trim of one alias
against its neighbour. It has never reached the pack's own levels and does not reach them now.

**The tag of what is SOUNDING, and the slot trap it exists to close.** `soundingLoudness()` returns
the `metadata.loudness` of the capture the sound is actually leaving by — `db` and `tagged` — never of
whichever model happens to sit in slot 0. Slots go by the PARITY of a capture's place on the dial, so
on an odd rung the whole sound comes out of slot 1 while slot 0 holds the silent neighbour; a face
that read slot 0 named a model nobody could hear, or warned "plays raw" about a capture that carries
a tag.

**During a crossfade there is no single honest number, and this does not invent one.** Two captures
are in the sound with two tags: `db`/`tagged` are the heavier slot's — the one carrying most of the
sound, `slot` says which that is — and `blended` says the other slot holds a DIFFERENT capture and is
audible beside it. So a face can say "one of two", or drop the number entirely while `blended` is
true, instead of stating a level the sound has not got. At the ends of the dial both slots hold the
same capture and `blended` is false however the weight sits. The weight it reads is the APPLIED one
(`liveMix()`), not the requested one: while a model is still loading or warming, the slot that IS the
sound is the old one. `tagged` is false both for a model without a tag and for a slot with no model
at all — `heldFileId(slot)` tells those two apart. It describes the MODEL mix alone: a blend knob at
its dry end takes every model out of the sound and says nothing here.

`setNormalize()` defaults to **true**. A model's `metadata.loudness` tag is a contract, not a
listener's option: with it off, every capture plays at whatever level the hardware happened to give,
and no two packs can be compared at all. A host may turn it off to hear a model against the hardware,
and should make that state loud — it is a measuring position, not a way to listen.

## Drawing — the one honest source of the curve

The tone as it PLAYS, not as it was measured: `commonGrid()` (1/12-octave, 20 Hz – 20 kHz) with
`curveDb(side)` — the summed curve-form knobs of a side, empty when none — and `curveActive(side)`
(a FIR, or a wire); `bands(side)` — the biquads of the section-form knobs at their current
positions; `blendGains()` — the dry and wet gains for the blend knob's position. A host that draws
from these draws what is sounding; recomputing from the pack's ladders draws something else.

## Not the player's business

The source (a loop, the jack), a cabinet IR, output routing, a pre-model trim the bench owns, level
metering, presets — the chain AROUND the player. In OrbitCapture NAM that is `AuditionEngine`
(the JUCE audio side) and `AuditionController` (the message-thread wiring, the pool that runs jobs).

## v0.7.0 — the load

`RigPlayer::run` prepares a capture with the player's channel count (`LoadJob::channels`: one network on a mono host)
and `NamStage::Warm::Caller` (no NAM zero prewarm for a non-recurrent capture — `warmFor()` covers the receptive field
before a slot is heard). `RigPlayer::putFile (id, bytes)` keeps a file handed in up front, as a fetched one is kept.
`RigPlayer::run` hands the job's bytes to `NamStage::prepareModel` shared, so a mono copy keeps no private copy of its
file; `RigPlayer::wantsFile (id)` says whether `putFile` would keep bytes, before a host makes room for them.

**OrbitAmp at its guitar-core bump.** The C++ seam is compatible: `takeLoadJob` / `run` / `deliver`
(`src/core/CapturedBlock.h`) are unchanged, and `LoadJob::channels` has a default that `takeLoadJob` fills in. The
BUILD is not: v0.7.0 needs namz v4.4.0 (`namz::decode`, and the float16 `.nam16z` it widens), and OrbitAmp pins namz itself. At the bump it must
- set `ORBITAMP_GUITAR_TAG` to v0.7.0 (`CMakeLists.txt`, ~122);
- set `ORBITAMP_NAMZ_TAG` to v4.4.0 (~187) — it FORCEs `FELITRONICS_NAMZ_TAG`, so guitar-core's own pin does not reach it.
  A fetched namz is held to `FELITRONICS_NAMZ_COMMIT` (v4.4.0's commit by default): a tag left behind stops the configure
  with both commits named, and a later namz bump in OrbitAmp alone FORCEs `FELITRONICS_NAMZ_COMMIT` with the tag;
- have the sibling `../namz` checkout on v4.4.0 or later: OrbitAmp FORCEs `FELITRONICS_NAMZ_SRC` to it whenever it
  exists (~178–180), so a local build breaks with "no member named 'decode'" as soon as `../felitronics-guitar-core`
  is at v0.7.0 and that checkout is not.
A mono host then builds one network per capture (two for a recurrent one) and no NAM zero prewarm on its loads.
