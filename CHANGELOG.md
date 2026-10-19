<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Changelog

## v0.1.1 — 2026-10-19

### Eigen from our own mirror

NeuralAmpModelerCore's Eigen submodule points at gitlab.com, and every build cloned it from there; on a day
GitLab turned requests away under load, every build failed at that clone. `nam` now fetches NeuralAmpModelerCore
without its submodules and Eigen from github.com/darwinscat/eigen — a mirror of the same repository, untouched —
at the very commit v0.5.4's submodule pins (Eigen 5.0.1), and stops the configure if the fetched tag is any other
commit. The sources built are the same, byte for byte.

## v0.1.0 — 2026-09-25

### nam · rigplayer · poweramp — split out of felitronics-core

The three modules and their suites, from felitronics-core v0.50.0; target names, namespaces and headers
are unchanged. New here: the law-11 and law-11c suites for `poweramp::PowerAmpStage` through core's shared
harnesses (`felitronics::test_support`), the header-hygiene TU and the `-fno-exceptions` probe.
`THIRD_PARTY_NOTICES.md` records the pins the build uses: NeuralAmpModelerCore v0.5.4, namz v4.1.0.
