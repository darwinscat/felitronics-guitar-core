#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.
#
# Builds fcguitarfx — the amp's echo and room (tools/fc_guitar_fx_abi.h) for a browser AudioWorklet:
#
#   fcguitarfx.mjs          the emscripten glue, an ES module whose default export is the factory (createFcGuitarFx),
#                           linked for -sENVIRONMENT=worklet: it never fetches; the processor hands it the module.
#   fcguitarfx.wasm         the module: core's delay::TapeDelay + reverb::AmpReverb behind the fc_guitar_fx ABI.
#   fcguitarfx-worklet.js   the AudioWorklet processor ('fc-guitar-fx'), copied as it is; it imports
#                           ./fcguitarfx.mjs, so the two travel together under these names (README.md).
#   BUILD-INFO              what it was built from: this repository, felitronics-core, the ABI version.
#
#   tools/wasm/build.sh [out-dir]          (default tools/wasm/build)
#
# THROUGH CMAKE, NOT A HAND-WRITTEN em++ LINE: the configure includes core's cmake/FelitronicsPolicy.cmake, so the
# numeric contract is core's own file and not a copy of it — law 10's `-ffp-contract=on`, the emscripten physics
# (8 MB stack, growable heap), and `--wrap=pthread_create`, which makes a thread a link error. What core's
# `wasm-audio` preset states as POLICY is passed as that preset passes it: -fno-exceptions -fno-rtti. Assertions are
# off: this is the shipped module, and the checked configuration of this source is the ABI suite, which CI runs under
# emscripten with SAFE_HEAP. (The preset also says -ffp-contract=off. It is left out here, because the policy's `on`
# follows it on the command line and wins either way, and because baseline wasm has no FMA to contract with: the two
# spellings build the same module.)
#
# NO -msimd128, though the policy allows it (SIMD128 the ISA, never -mrelaxed-simd). It buys nothing here, measured:
# the heaviest quantum (Modulated, a 6 s line) costs 9.4 us in the wasm call with it and 9.5 without, Hall 5.6 and
# 5.6 — neither stage has a kernel the vectoriser can widen, the recursion is sample by sample — and without it the
# module also runs on an engine that has no SIMD.
#
# Requires emsdk: em++ on PATH, or ~/emsdk to source. AND felitronics-core v0.61.0 or later: FELITRONICS_CORE_DIR
# when that is set (a checkout); otherwise the top-level CMake's own rule — the sibling ../felitronics-core when there
# is one, the pinned tag fetched when there is not. BUILD-INFO records which core it was.
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
echo "emcc: $(emcc --version | head -1)"

# Stated on every run, so a build directory configured once with another core cannot keep it from its cache.
CORE_ARG=(-DFELITRONICS_GUITAR_FCORE_DIR="$ROOT/../felitronics-core")
if [ -n "${FELITRONICS_CORE_DIR:-}" ]; then
    [ -f "$FELITRONICS_CORE_DIR/modules/delay/include/felitronics/delay/TapeDelay.h" ] \
        || { echo "no felitronics-core v0.61.0+ at $FELITRONICS_CORE_DIR (no modules/delay)"; exit 1; }
    CORE_ARG=(-DFELITRONICS_GUITAR_FCORE_DIR="$(cd "$FELITRONICS_CORE_DIR" && pwd)")
fi

GEN=()
command -v ninja > /dev/null && GEN=(-G Ninja)

BUILD="$OUT/cmake"
emcmake cmake -S "$ROOT" -B "$BUILD" "${GEN[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFELITRONICS_GUITAR_WITH_NAM=OFF \
    -DFELITRONICS_GUITAR_BUILD_TESTS=OFF \
    -DFELITRONICS_WASM_ASSERTIONS=0 \
    "-DCMAKE_CXX_FLAGS=-fno-exceptions -fno-rtti" \
    "${CORE_ARG[@]}" > "$OUT/configure.log" || { cat "$OUT/configure.log"; exit 1; }
grep "felitronics-core:" "$OUT/configure.log" || true
cmake --build "$BUILD" --target fcguitarfx

cp "$BUILD/tools/wasm/fcguitarfx.mjs" "$BUILD/tools/wasm/fcguitarfx.wasm" "$HERE/fcguitarfx-worklet.js" "$OUT/"
CORE="$(head -1 "$BUILD/felitronics-core-dir.txt")"

echo
echo "=== no threads, proven from the artifact (felitronics-core's tools/wasm/check-no-threads.mjs)"
node "$CORE/tools/wasm/check-no-threads.mjs" "$OUT/fcguitarfx.wasm" "$OUT/fcguitarfx.mjs"

echo
echo "=== size"
printf "  %-24s %10s %10s %10s\n" file raw gzip brotli
for f in fcguitarfx.wasm fcguitarfx.mjs fcguitarfx-worklet.js; do
    raw=$(wc -c < "$OUT/$f")
    gz=$(gzip -9 -c "$OUT/$f" | wc -c)
    if command -v brotli > /dev/null; then br=$(brotli -q 11 -c "$OUT/$f" | wc -c); else br="n/a"; fi
    printf "  %-24s %10s %10s %10s\n" "$f" "$raw" "$gz" "$br"
done

# WHAT THIS WAS BUILT FROM, beside what it built: a page that vendors the module records which engine it ships, and
# a checkout's own `git describe` cannot say which core the module was compiled against.
describe() { git -C "$1" describe --tags --always --dirty 2>/dev/null || echo unknown; }
ABI=$(sed -n 's/^#define FC_GUITAR_FX_ABI_VERSION \([0-9][0-9]*\)u$/\1/p' "$ROOT/tools/fc_guitar_fx_abi.h")
[ -n "$ABI" ] || { echo "no FC_GUITAR_FX_ABI_VERSION in tools/fc_guitar_fx_abi.h"; exit 1; }
{
    echo "felitronics-guitar-core $(describe "$ROOT")"
    echo "felitronics-core $(describe "$CORE")"
    echo "fc_guitar_fx ABI $ABI"
} > "$OUT/BUILD-INFO"
echo
echo "=== built from"
cat "$OUT/BUILD-INFO"
