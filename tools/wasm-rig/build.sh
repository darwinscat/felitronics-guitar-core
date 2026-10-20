#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.
#
# Builds fcrig — the pack player (tools/fc_rig_abi.h over felitronics::rigplayer) for a browser AudioWorklet:
#
#   fcrig.mjs          the emscripten glue, an ES module whose default export is the factory (createFcRig), linked
#                      for -sENVIRONMENT=worklet: it never fetches; the processor hands it the module.
#   fcrig.wasm         the module: RigPlayer, NamStage and NeuralAmpModelerCore behind the fc_rig ABI.
#   fcrig-worklet.js   the AudioWorklet processor ('fc-rig'), copied as it is; it imports ./fcrig.mjs, so the two
#                      travel together under these names (README.md).
#   BUILD-INFO         what it was built from: this repository, felitronics-core, NeuralAmpModelerCore, Eigen, namz, the
#                      ABI version, the toolchain and the flags.
#
#   tools/wasm-rig/build.sh [out-dir]          (default tools/wasm-rig/build)
#
# THROUGH CMAKE, as tools/wasm/build.sh: core's cmake/FelitronicsPolicy.cmake is the numeric contract (law 10's
# `-ffp-contract=on`, the emscripten physics, `--wrap=pthread_create`). What is passed here, for every object:
#   -fno-exceptions -fno-rtti   core's wasm-audio preset. The NAM objects alone get exceptions back — see
#                               CMakeLists.txt, and README.md for Oleh's decision of 19.10.2026.
#   -msimd128 -msse4.1          SIMD128, and Eigen's SSE packet path lowered onto it by emscripten. Measured in the
#                               spike of 19.10.2026 (Chromium, 48 kHz, 128 frames): one WaveNet capture 3.7 % of the
#                               quantum, the blend of two 7.3 %, TONE3000's engine 7.7 % for one. It moves the floor
#                               to SIMD128's (Safari 16.4) — README.md, "Browser floor".
# Assertions off: the shipped module. The checked configuration of this source is the native ABI suite.
#
# CORE IS PINNED: the tag the top-level CMakeLists names (v0.61.1 — the first whose FlushToZero.h compiles for wasm
# under -msse4.1), fetched, unless FELITRONICS_CORE_DIR names a checkout to build against instead. NAM and namz are
# the pins modules/nam names (NeuralAmpModelerCore v0.5.4, namz v4.4.0), fetched — or FELITRONICS_NAM_CORE_SRC /
# FELITRONICS_NAMZ_SRC, checkouts of those same tags, for a build without the network. BUILD-INFO says which.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$HERE/build}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

if ! command -v em++ > /dev/null; then
    [ -f "$HOME/emsdk/emsdk_env.sh" ] || { echo "em++ not on PATH and no ~/emsdk — source \$EMSDK/emsdk_env.sh first"; exit 1; }
    # shellcheck disable=SC1091
    source "$HOME/emsdk/emsdk_env.sh" > /dev/null 2>&1
fi
EMVER="$(emcc --version | head -1)"
echo "emcc: $EMVER"

# Stated on every run, so a build directory configured once with another source cannot keep it from its cache; and
# the pins' own cache entries (PINS_ARG) are dropped, so one configured once with another tag gets the tags the
# CMakeLists name again — a cached value would win over a new pin in the file.
PINS_ARG=(-UFELITRONICS_GUITAR_FCORE_TAG -UFELITRONICS_NAM_CORE_TAG -UFELITRONICS_EIGEN_TAG -UFELITRONICS_EIGEN_COMMIT
          -UFELITRONICS_NAMZ_TAG -UFELITRONICS_NAMZ_COMMIT)
CORE_ARG=(-DFELITRONICS_GUITAR_FCORE_DIR=)          # empty: the pinned tag
if [ -n "${FELITRONICS_CORE_DIR:-}" ]; then
    [ -f "$FELITRONICS_CORE_DIR/cmake/FelitronicsPolicy.cmake" ] || { echo "no felitronics-core at $FELITRONICS_CORE_DIR"; exit 1; }
    CORE_ARG=(-DFELITRONICS_GUITAR_FCORE_DIR="$(cd "$FELITRONICS_CORE_DIR" && pwd)")
fi
DEPS_ARG=(-DFELITRONICS_NAM_CORE_SRC="${FELITRONICS_NAM_CORE_SRC:-}" -DFELITRONICS_NAMZ_SRC="${FELITRONICS_NAMZ_SRC:-}")

GEN=()
command -v ninja > /dev/null && GEN=(-G Ninja)

FLAGS="-fno-exceptions -fno-rtti -msimd128 -msse4.1"
BUILD="$OUT/cmake"
emcmake cmake -S "$ROOT" -B "$BUILD" "${GEN[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFELITRONICS_GUITAR_WITH_NAM=ON \
    -DFELITRONICS_GUITAR_WITH_FX=OFF \
    -DFELITRONICS_GUITAR_WITH_RIG=ON \
    -DFELITRONICS_GUITAR_BUILD_TESTS=OFF \
    -DFELITRONICS_WASM_ASSERTIONS=0 \
    "-DCMAKE_CXX_FLAGS=$FLAGS" \
    "${PINS_ARG[@]}" "${CORE_ARG[@]}" "${DEPS_ARG[@]}" > "$OUT/configure.log" || { cat "$OUT/configure.log"; exit 1; }
grep -E "felitronics-core:|NeuralAmpModelerCore:|Eigen:|namz:" "$OUT/configure.log" || true
cmake --build "$BUILD" --target fcrig

cp "$BUILD/tools/wasm-rig/fcrig.mjs" "$BUILD/tools/wasm-rig/fcrig.wasm" "$HERE/fcrig-worklet.js" "$OUT/"
CORE="$(sed -n 1p "$BUILD/fc-rig-deps.txt")"
NAM="$(sed -n 2p "$BUILD/fc-rig-deps.txt")"
NAMZ="$(sed -n 3p "$BUILD/fc-rig-deps.txt")"
EIGEN="$(sed -n 4p "$BUILD/fc-rig-deps.txt")"

echo
echo "=== no threads, proven from the artifact (felitronics-core's tools/wasm/check-no-threads.mjs)"
node "$CORE/tools/wasm/check-no-threads.mjs" "$OUT/fcrig.wasm" "$OUT/fcrig.mjs"

# WHICH WASM FEATURES THE MODULE NEEDS — read off the module, not assumed: it must validate against MVP plus the
# candidates, and a candidate it fails without is one it needs. The browser floor is the newest of these, the glue's
# JavaScript (emscripten's default floor) and module AudioWorklets (README.md).
echo
echo "=== wasm features the module needs (binaryen validation against MVP + each)"
WASM_OPT="$(dirname "$(command -v emcc)")/../bin/wasm-opt"
[ -x "$WASM_OPT" ] || WASM_OPT="$(command -v wasm-opt || true)"
NEEDS=""
if [ -n "$WASM_OPT" ]; then
    CANDIDATES="sign-ext mutable-globals nontrapping-float-to-int bulk-memory bulk-memory-opt simd exception-handling multivalue reference-types tail-call extended-const"
    all=(); for f in $CANDIDATES; do all+=("--enable-$f"); done
    "$WASM_OPT" --mvp-features "${all[@]}" "$OUT/fcrig.wasm" -o /dev/null 2> /dev/null || { echo "does not validate against MVP + every candidate"; exit 1; }
    for f in $CANDIDATES; do
        rest=(); for g in $CANDIDATES; do [ "$g" = "$f" ] || rest+=("--enable-$g"); done
        "$WASM_OPT" --mvp-features "${rest[@]}" "$OUT/fcrig.wasm" -o /dev/null 2> /dev/null || NEEDS="$NEEDS $f"
    done
    echo " ${NEEDS# }"
    case " $NEEDS " in *" simd "*) ;; *) echo "the module does not use SIMD128 — was -msimd128 lost?"; exit 1 ;; esac
    case " $NEEDS " in *" exception-handling "*) ;; *) echo "the module has no wasm exception handling — the NAM objects lost it"; exit 1 ;; esac
else
    echo " (no wasm-opt: not checked)"
fi
if grep -q "toResizableBuffer" "$OUT/fcrig.mjs"; then echo "the glue names toResizableBuffer: the floor would be Chrome 144"; exit 1; fi

echo
echo "=== size"
printf "  %-20s %10s %10s %10s\n" file raw gzip brotli
for f in fcrig.wasm fcrig.mjs fcrig-worklet.js; do
    raw=$(wc -c < "$OUT/$f")
    gz=$(gzip -9 -c "$OUT/$f" | wc -c)
    if command -v brotli > /dev/null; then br=$(brotli -q 11 -c "$OUT/$f" | wc -c); else br="n/a"; fi
    printf "  %-20s %10s %10s %10s\n" "$f" "$raw" "$gz" "$br"
done

describe() { git -C "$1" describe --tags --always --dirty 2>/dev/null || echo unknown; }
ABI=$(sed -n 's/^#define FC_RIG_ABI_VERSION \([0-9][0-9]*\)u$/\1/p' "$ROOT/tools/fc_rig_abi.h")
[ -n "$ABI" ] || { echo "no FC_RIG_ABI_VERSION in tools/fc_rig_abi.h"; exit 1; }
{
    echo "felitronics-guitar-core $(describe "$ROOT")"
    echo "felitronics-core $(describe "$CORE")"
    echo "NeuralAmpModelerCore $(describe "$NAM")"
    echo "Eigen $(describe "$EIGEN")"
    echo "namz $(describe "$NAMZ")"
    echo "fc_rig ABI $ABI"
    echo "emscripten ${EMVER##*) }"
    echo "flags $FLAGS; exceptions on the NAM objects only, wasm legacy EH"
    echo "wasm features${NEEDS:- (not checked)}"
} > "$OUT/BUILD-INFO"
echo
echo "=== built from"
cat "$OUT/BUILD-INFO"
