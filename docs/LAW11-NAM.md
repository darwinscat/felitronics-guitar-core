<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Law 11 on the NAM backend and the pack player — the worked case, with the measurements

felitronics-core's law 11 (`docs/DSP-ARCHITECTURE.md` §2, 11a–11e) states the rules in general terms: a
black-box memory is DRAINED with silence, `reset()` is a stream restart, `prepare()` performs the same
restart, a prepare restates what it counts, and a composite owes its consumer the same verb. The text
below is the part of that law written against `nam::NamStage` and `rigplayer::RigPlayer`, moved here
VERBATIM when those modules left core — every number in it was measured on them. Laws and clause
letters refer to felitronics-core.

   **AND WHERE THE MEMORY CANNOT BE DROPPED, IT IS DRAINED.** "Drop its sample memory" assumes the
   memory is ours to clear, and for a delay line it is. For a stage that owns a black box it is not:
   `nam::NamStage` holds a neural network whose window belongs to NAM (whose `Reset` allocates, and for
   a `Linear` capture does not clear that window at all) and two `core::StreamResampler` legs beside it.
   The third answer is to hand the stopped channel the DIGITAL SILENCE it is receiving — the same code
   path, into the stage's own scratch, since `io` need not carry that plane and at `nch == 0` may be
   null — until its state is provably the state of a channel that was silent all along, and then to
   STOP. Bounded, so a permanently mono host still pays for one network rather than two: the length is
   the model's own memory plus each rate-matcher's tap window, each counted in ITS OWN rate. Measured
   before it, through `rigplayer::RigPlayer`, worst |out| out of digital silence: **0.518588 at
   44.1 kHz** with a memoryless capture (the rate-matchers alone) and **0.499533 at 48 kHz** with a
   2001-tap one (the network alone, at the one rate where no rate-matcher is installed) — two
   independent halves, each with a fixture that cannot see the other. **The same class reaches a stage a
   composite stops CALLING at all** for reasons of its own: `RigPlayer` skipped a slot the blend law had
   put to sleep, which replayed **0.500000** for a whole receptive field, and hands it a width-zero call
   now. And because "it drains, and then it stops" has no witness in the audio — past the debt the
   output is zero either way — the stage publishes an odometer (`NamStage::drainedSamples()`) so a test
   can see the length; three mutations of it survived a suite of 960 checks before that existed.
   ⚠️ A recurrent cell has no flush length, so for an LSTM this is a bound on NAM's own half-second
   heuristic and not on the memory (0.419 against 0.023 for a lane clocked throughout) — said here
   rather than left for the next reader to find.

   **AND THE OTHER HALF IS `reset()`, WHICH IS A DIFFERENT OPERATION AND NOT A LONGER ONE.** Everything
   above is about a lane the caller STOPPED handing over: it is still the same stream, so the answer is
   to feed it the silence it is really receiving and let its state evolve as silence evolves it. A lane
   that is PRESENT gets the caller's own samples, and a stale window speaking into them is not a falling
   edge — it is a stream RESTART, and the restart verb has to do it. `felitronics::nam::NamStage::reset()`
   was EMPTY, so it did not: a dense 2001-tap capture that had played a tone answered digital silence
   with **0.224604502320**, and so did the same capture through `prepare()`, because `::nam::DSP::Reset`
   calls `SetMaxBufferSize` and then a prewarm that is zero samples for a `Linear`. The two verbs differ
   in what they restore, not in how long they run: a drain SIMULATES silence, and a restart puts the
   stage back where a freshly loaded and prepared one is. For a finite-memory capture the two states
   coincide and the same zero-feed reaches it. What that buys is INDEPENDENCE, and it is exact: two stages
   fed different audio before the restart answer the next programme with the same bits, on real captures
   and synthetic ones, at every rate. It does NOT buy bit-identity with a stage prepared a moment ago —
   NAM's answer depends on how the stream is cut into CALLS, so a restart, whose chunking is its own,
   lands 1.037e-06 away on a real Standard at blocks 64…512 and exactly on it for a real slimmable at the
   same blocks. The restart additionally re-primes the
   rate-matcher legs, because a restart re-anchors the audio-time clocks, exactly as `eq::EqBand::reset()`
   re-anchors its `StateGrid` (leave them and the next programme runs at the previous stream's sub-sample
   phase: 1.039e-06 at 44.1 kHz). For a RECURRENT capture they do not coincide, and the exception stays
   named: a restart spends the heuristic again — which is what NAM's own `Reset` does — and leaves what
   that leaves (300 samples differing from a fresh instance, worst 1.49e-07, on a real LSTM).

   **A RESTART IS THE ONE AUDIO-THREAD CALL WHOSE COST IS NOT THE BLOCK'S.** It is a whole drain length
   of inference per dirty lane — the field, the ring and the legs, so more than `prewarmSamples()`
   reports: on an M-series core, per lane, **3.77 ms at a 64-sample block — 282 % of that callback** —
   3.46 at 256, 3.43 at 512, against 1.3 ms for a real LSTM and 0.13 for a dense 2001-tap Linear. There is no cheaper exact mechanism to substitute: NAM's own `Reset` with the prewarm off
   zeroes the Conv1D rings in 0.014 ms and still misses the prepared state by 4089 samples (worst 0.324),
   because that state is a PREWARMED one, and on a `Linear` with the FFT engine it allocates 46 times. So
   the price is published rather than hidden, and the operation is made IDEMPOTENT instead — the debt is
   re-armed only by audio actually being fed, so a second restart with nothing in between is free and a
   mono host pays for one lane. What a restart cannot rewind is a third-party clock: NAM's partitioned
   `Linear` counts every sample it has ever seen, and rewinding that means re-configuring the engine,
   which allocates; the residue peaks at 1.788139e-07 over nine block sizes x eight rates against a stage
   prepared a moment ago and is EXACTLY ZERO against one clocked to the same point, i.e. it is the
   engine's arithmetic and not our state. **And a restart
   flushes what the LEDGER can see** — so the ledger has to answer for the WHOLE model. A capture whose
   conditioner is a model of its own (`config.condition_dsp`) hid that model's memory from both readers
   of the field, and was under-flushed by exactly as much as law 11a's drain under-drained it —
   0.905147969723 either way, one defect in one ledger. It was corrected in the ledger and both readers
   moved together: `detail::receptiveFieldFromConfig` adds a conditioner's memory to the network's own
   IN SERIES, because the conditioner's output is the network's conditioning input, and the two other
   questions the ledger answers — whether anything in the tree is recurrent, and whether anything in it
   is charged NAM's partitioned-FFT ring — walk the same branch. On NAM's own shipped captures the
   whole change moves one number: +1 sample of drain on the two that carry a conditioner, and a
   byte-identical render on every other.
   **And where the ledger cannot PLACE something, it charges ONE allowance — never zero, never the
   face value** (P92). The ledger promises an upper bound, so "I do not know" has an answer, and the
   costs are asymmetric: an understated number is the previous sound coming out of digital silence, an
   overstated one is inference nobody hears — as long as it is BOUNDED. The measured case was NAM's
   slimmable wrapper, whose real config sits under `config.model` where nothing read it: 0 samples
   flushed for a model reaching 2046. Three events now mean "cannot place" — a config carrying a model's
   vocabulary under a key the ledger does not read (keyed on SHAPE, never on NAM's dispatch, so it fires
   under any key name), a value that is there and cannot be read, and a reading the ledger sets aside
   (a declared field beside a stack) — and each adds `kUnreadShapeCeiling` = 48 000 samples ONCE per
   tree to what the ledger did read. Once, because an allowance per node turned a 1.6 MB file into an
   INT_MAX drain; added, because a max let a large known part swallow the unknown's share; and never
   the face value, because a dead number costs NAM nothing and was spent here as a half-hour `reset()`.
   The price is measured through `reset()`: 39.7 ms per lane on the most expensive real capture
   rewrapped (256 block, 48 kHz). It fires on none of the author's 1229 distinct captures. What it does
   NOT close is stated with it, as two doors of which shutting either opens the other: a LIVE memory the
   ledger cannot place, longer than the allowance, drains short by the difference; and a DEAD number the
   ledger PLACES (a lower reading with no stack, the wrapped form's own decoy stack) is trusted at face
   value, as before — which door stays open is a registered policy question. Separately, NAM's own
   recursive copy of the config takes the host down on a deep enough file (about 2 000 levels, 134 KB,
   on a 512 KiB thread) before the ledger runs at all.

   **AND `prepare()` PERFORMS THAT RESTART TOO, ALWAYS — THE TWO VERBS NAME ONE STATE.** A prepared stage
   holds no audio the caller fed, on any rate, on any shape, and whether or not the rate or the block
   size actually changed. This is the second half of the same defect: the 0.224604502320 above was first
   measured through `prepare()`, and over a grid of eight host rates x two block sizes x three capture
   shapes x {re-prepare at the same rate, re-prepare at a different one}, **72 of 96 cells leaked, worst
   0.567861497402**, with both lanes PRESENT. There is deliberately no predicate on what changed: a
   re-prepare at the SAME numbers is the common case — a host's buffer-size slider moves more often than
   its rate one, and a driver stops the stream for either — and it was the case that leaked loudest.
   The mechanism is the drain above, not a second one: `configureRates` charges every lane that may still
   be holding audio — fed since it was last emptied, whether by a restart or by a falling-edge drain that
   ran to the end (a recurrent lane is never emptied, so it is always charged) — and the tail of
   `prepare()` spends it, so a first prepare after a load costs nothing and a
   model change (which prepares a never-fed backend, in `prepareModel()` and again in `install()` when the
   host's numbers moved between the halves) costs nothing either. Where it does cost, it
   is the message thread and the price is published: for an architecture whose own `Reset` already
   prewarms, the drain is a SECOND pass over the field and roughly doubles the call — a stereo real
   Standard WaveNet measured 6.5 ms before and 13.1 ms after at 48 kHz. Skipping that pass is sound only
   per architecture, and that was measured rather than argued: with the drain removed, every capture NAM
   ships keeps independence at exactly 0, while this tree's own `Buffer`-based fixtures leak on 72 of 96
   cells again. NAM's example set has no such capture in any tree, so a check against real captures alone
   would have approved a blanket skip. The predicate is structural and belongs to the receptive-field
   registry (P98).

   **AND A PREPARE RESTATES WHAT IT COUNTS, NOT ONLY WHAT IT DESIGNS.** After `RigPlayer::prepare()`
   nothing the player acts on is expressed in the samples of a rate it no longer runs at. Rebuilding the
   rate-DESIGNED state — filters, rings, stages, a threshold kept in seconds — was never the whole of it:
   the blend law's warm-up debt, its rest count, the two per-slot alignment delays and a landing in
   flight are COUNTS of host samples, and they were written once and read for ever. Measured on a 6x6
   rate grid: a slot woken after 48 -> 96 kHz warmed for half the field it owed, and 44.1 <-> 48 kHz —
   the pair a fixture reaches for first — read exactly right. Each count is restated by the rule its own
   algebra allows: the debt RECOMPUTED (it is not homogeneous in the rate), the warm-up progress mapped
   by its PREDICATE (audible stays audible, warming starts over), the rest count RESCALED (it is pure
   elapsed time, so the ratio is exact and is 1 where nothing moved).

   **AND A COMPOSITE OWES ITS CONSUMER THE SAME VERB.** `rigplayer::RigPlayer` had none, so a product
   reaching a `NamStage` through it — which is how orbit-amp reaches one — could not call the restart at
   all. `RigPlayer::reset()` is that verb: both model slots, the three convolvers (bypassed or not — a
   bypassed one is skipped, so its history freezes and is replayed), the dry path's alignment ring, the
   per-slot alignment tails, the band filters and the scratch. It leaves the blend law's state alone with
   ONE exception: a restart is not a device change, and re-arming the warm-up of a slot that is already
   AUDIBLE would not deliver invariant 3 anyway (the law ramps its gain down over four blocks, so an unfed
   network is audible regardless) while costing 192 ms of hole at every restart. A slot still WARMING is
   the exception, and it is re-armed: it is at weight zero by construction, so re-arming it is silent,
   and its network has just been flushed, so crediting it the field it heard before the flush would mark
   it audible with up to a whole field missing (`nam::blendRestated`, P89). Its price is FOUR networks,
   not one. And it reaches a convolver through `clearAudioState()` rather than `reset()`, because when
   this was written `reset()` there DISCARDED a filter published a block ago and still fading in. P88
   closed that in the verb itself (11e), and that removes the reason for the choice: `clearAudioState()`
   leaves a fade RUNNING, so it does not give this verb's own independence while one is in flight
   (measured through `CabConvolver`, 2143 of 5120 samples a channel differ between a clear one block into
   a 50 ms fade and a clear after it settled; the same two `reset()` calls differ in none), and `reset()`
   no longer races a loader (measured under ThreadSanitizer, not yet the contract — 11e). Switching the
   player to `reset()` is registered as its own task rather than taken here.
