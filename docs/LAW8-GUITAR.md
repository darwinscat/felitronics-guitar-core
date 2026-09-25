<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Law 8 — the verdicts for this repository's kernels

felitronics-core's `docs/LAW8-AUDIT.md` is the audit of every recursive kernel against law 8 (denormals,
and more generally a state that decays to a non-zero fixed point). The parts of it written against
`rigplayer` and `poweramp` moved here VERBATIM when those modules left core; read core's audit for the
mechanism, the six-question checklist and every other verdict. Also swept clean there, and moved with
their modules: `rigplayer::runBands` (flushes each `MatchedBiquad` per block), `nam::BlendLaw` (linear
step + clamp lands on exact 0/1), `SagEnvelope` (1e-30f), `TubeStage` (memoryless), and poweramp's
`gApplied`/`postApplied` (linear ramps, land exactly). `poweramp::PowerAmpStage` and
`rigplayer::RigPlayer` were on its list of kernels whose flush is anchored at the call, not at audio time.

## Verdict table rows

| kernel | recursive | stalls | cost | verdict |
|---|---|---|---|---|
| `rigplayer` blend / trim ramps | yes | **yes** — 2 ulp, stall from ~1 s | ~3 assisting ops **per sample per channel**, forever | **FIXED** |
| `poweramp` coefficient smoothers | yes | **yes** — ~5 ulp | `topoCur`/`leakCur` reach the **per-oversampled-sample** kernel | **FIXED** |

## The two fixed kernels

**`rigplayer`'s blend and trim ramps.** `end = want + (current−want)·decay`, per block. An exact-zero
target needs no exotic pack: `BlendKnob::linOf` returns `0.0` for any level at or below −120 dB, which is
how a pack spells "this path is off" — the repo's own test rig ships it (dry end `wetDb −120`, wet end
`dryDb −120`). Move the dial there after an audible position and the gain decays to a subnormal fixed
point (`decay = 0.766` at block 128 → `k ≤ 2.1`, so 2 ulp; stall from ~1 s) and stays, while the mix loop
— gated on `dryActive_`, a dry IR being **loaded**, not on either gain — keeps running
`gd += stepDry; a[c][i]·gw + d[c][i]·gd` over it, ~3 assisting operations per sample per channel, for the
life of the rig. What does *not* stall is the initialised or reset `0.0f`: `0 → 0` stays exactly 0.

**`poweramp`'s 13 block-rate coefficient smoothers.** Most are consumed behind a per-block gate
(`presOn`, `depthOn`, `loadOn`, `biasOn`, `ironOn`, all `> 1e-4f`), which reads a stuck subnormal as
"off" — the right answer, reached by accident. **`topoCur` is not gated**: `TubeStage` blends
`(1−topo)·pp + topo·se` on every *oversampled* sample, so after one SE→PP toggle a stuck topo costs a
subnormal multiply and add per sample per channel at 4× rate, forever; `leakCur` reaches the per-sample
curves the same way. So this was never the 13-FMAs-per-block housekeeping it looked like. Snapping also
restores a real property: `topo` lands on exact 0, so the "all-off ⇒ bare push-pull path, byte-identical"
contract holds after a toggle and not only from a cold start. The per-*sample* states in this file
(`dcx1/dcy1`, `otLp/otHf`) were already flushed at 1e-30f and are clean.

## How they are tested

| kernel | observable | negative control (fix reverted) |
|---|---|---|
| `rigplayer` | `liveWet()`, the gain the audio thread actually applied | 1.8e-35 at 0.8 s |

rigplayer: the flush fires at 0.69 s, the value goes subnormal at 0.87 s, and the assertion sits at 0.8 s, so it
discriminates on any machine, FTZ or not.

**Not independently tested, deliberately:** `poweramp`'s *gated* coefficient smoothers — every poweramp gate
reads a stuck subnormal as "off", which is the same answer as a snapped zero, so there is no assertion that
would fail without the fix. `poweramp`'s `topoCur` is the exception in principle (it is consumed per sample),
but at `u = 0` both `pp` and `se` are exactly zero and with signal the subnormal is absorbed, so it is not
output-observable either.

**Not verified:** NAM inference state (external code; an LSTM's zero-input fixed point is nonzero, so no
subnormal stall is expected, but it has not been measured).
