<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->

# felitronics-guitar-core

[![CI](https://github.com/darwinscat/felitronics-guitar-core/actions/workflows/ci.yml/badge.svg)](https://github.com/darwinscat/felitronics-guitar-core/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](#)
[![core: felitronics-core](https://img.shields.io/badge/core-felitronics--core-brightgreen.svg)](https://github.com/darwinscat/felitronics-core)
[![NAM: NeuralAmpModelerCore](https://img.shields.io/badge/NAM-NeuralAmpModelerCore-orange.svg)](https://github.com/sdatkinson/NeuralAmpModelerCore)
[![License: AGPL v3](https://img.shields.io/badge/License-AGPL%20v3-blue.svg)](LICENSE)
[![Latest release](https://img.shields.io/github/v/tag/darwinscat/felitronics-guitar-core)](https://github.com/darwinscat/felitronics-guitar-core/tags)

Guitar-amp DSP for the Darwin's Cat products, on top of [felitronics-core](https://github.com/darwinscat/felitronics-core).

| Module | What |
|---|---|
| `nam` | NeuralAmpModelerCore backend: `.nam` / `.namz` models, true stereo, loudness makeup, host-rate matching |
| `rigplayer` | plays one device of a `.orbitrig` pack: captures, crossfade, tone and blend knobs |
| `poweramp` | tube power-amp stage: sag, tube transfer, output stage |

## Build

```sh
cmake --preset desktop && cmake --build --preset desktop && ctest --preset desktop
```

Licence: AGPL-3.0-or-later. Third-party code: `THIRD_PARTY_NOTICES.md`.

Part of the Felitronics line by [Darwin's Cat](https://darwinscat.com).
