<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# Third-party notices — felitronics-guitar-core

The repository itself is AGPL-3.0-or-later (see `LICENSE`). `poweramp` and `rigplayer` are original code.
`nam` builds third-party sources, fetched at pinned releases by `modules/nam/CMakeLists.txt` (or taken from
local checkouts via `FELITRONICS_NAM_CORE_SRC` / `FELITRONICS_NAMZ_SRC`); nothing is vendored here.

| Module | Third-party code | Licence | AGPL-compatible |
|---|---|---|---|
| `nam` | **NeuralAmpModelerCore** `v0.5.4` at `1f42f88535884450104b8711d7595019afa0495b` (`https://github.com/sdatkinson/NeuralAmpModelerCore`) | MIT | yes |
| `nam` | **Eigen**, supplied by NeuralAmpModelerCore's `Dependencies/eigen` submodule at that pin | MPL-2.0 | yes |
| `nam` | **nlohmann/json**, vendored inside NeuralAmpModelerCore at that pin | MIT | yes |
| `nam` | **namz** `v4.1.0` at `7bacfcb2a1beb48a5d95a5aa43c3b25092f0537a` (`https://github.com/darwinscat/namz`) | MIT | yes |

`felitronics::nam` carries NAM's architecture-registration whole-archive link contract
(`$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>`) transitively, and exports only the namz and nlohmann include
directories to consumers; the NAM and Eigen include directories stay private.

felitronics-core, which this repository builds on, records its own notices in its `THIRD_PARTY_NOTICES.md`.
