# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

# 3.22, as core. modules/nam requires 3.24 itself ($<LINK_LIBRARY:WHOLE_ARCHIVE> — NAM registers its
# architectures through static initialisers, and plain static linking silently drops them), so poweramp
# alone (FELITRONICS_GUITAR_WITH_NAM=OFF) still configures on 3.22.
cmake_minimum_required(VERSION 3.22)
project(felitronics_guitar_core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ----------------------------------------------------------------------------------------------------
# felitronics-guitar-core — the guitar-amp DSP of the Felitronics family, over felitronics-core.
# JUCE-free, real-time-safe C++20, under core's laws (felitronics-core docs/DSP-ARCHITECTURE.md §2).
#
# Module      Target                   What                                                  Deps
#   nam       felitronics::nam         NeuralAmpModelerCore behind core's neural seam        core, neural (+ NAM, namz)
#   rigplayer felitronics::rigplayer   one device of a .orbitrig pack, playing               nam, convolution, lineareq, eq
#   poweramp  felitronics::poweramp    the white-box tube power-amp trunk                    core, eq, oversampling, saturation
#
# Consume — felitronics-core FIRST, then this; ONE core is resolved by the product that owns the graph:
#   FetchContent_Declare(felitronics_core GIT_REPOSITORY <url> GIT_TAG <tag>)
#   FetchContent_MakeAvailable(felitronics_core)
#   FetchContent_Declare(felitronics_guitar_core GIT_REPOSITORY <url> GIT_TAG <tag>)
#   FetchContent_MakeAvailable(felitronics_guitar_core)
#   target_link_libraries(app PRIVATE felitronics::rigplayer felitronics::poweramp)
# ----------------------------------------------------------------------------------------------------

option(FELITRONICS_GUITAR_BUILD_TESTS "Build felitronics-guitar-core self-tests" "${PROJECT_IS_TOP_LEVEL}")
option(FELITRONICS_GUITAR_WITH_NAM
       "Build felitronics::nam and felitronics::rigplayer (fetches the pinned NeuralAmpModelerCore + namz)" ON)

# --- felitronics-core, resolved by whoever owns the graph ---------------------------------------------
# Top-level: this repository IS the owner, so it resolves core itself — a sibling checkout when there is
# one (local development), the pinned tag otherwise (what CI gets: it checks out this repository alone, so
# a green CI run proves the PIN, not somebody's working copy).
if(PROJECT_IS_TOP_LEVEL)
    include(FetchContent)
    set(FELITRONICS_GUITAR_FCORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../felitronics-core" CACHE PATH
        "Local felitronics-core checkout (used if present); else the pinned release is fetched")
    set(FELITRONICS_GUITAR_FCORE_TAG "v0.51.0" CACHE STRING
        "felitronics-core git tag to fetch when no local checkout is present")
    if(FELITRONICS_GUITAR_FCORE_DIR AND EXISTS "${FELITRONICS_GUITAR_FCORE_DIR}/CMakeLists.txt")
        message(STATUS "felitronics-core: local checkout — ${FELITRONICS_GUITAR_FCORE_DIR}")
        FetchContent_Declare(felitronics_core SOURCE_DIR "${FELITRONICS_GUITAR_FCORE_DIR}")
    else()
        message(STATUS "felitronics-core: fetching ${FELITRONICS_GUITAR_FCORE_TAG} from GitHub")
        FetchContent_Declare(felitronics_core
            GIT_REPOSITORY https://github.com/darwinscat/felitronics-core.git
            GIT_TAG        ${FELITRONICS_GUITAR_FCORE_TAG}
            GIT_SHALLOW    TRUE)
    endif()
    # Core's own suites are core's business. A NORMAL variable, which core's option() honours (CMP0077),
    # so this does not rewrite a cache entry the developer set.
    set(FELITRONICS_BUILD_TESTS OFF)
    FetchContent_MakeAvailable(felitronics_core)
endif()

# Consumed, not top-level: core must already be there. A nested fetch here is how a product ends up with
# two cores at two versions. felitronics::test_support and cmake/FelitronicsPolicy.cmake are also the
# minimum-version check: both first shipped in the core release this repository was split from.
if(NOT TARGET felitronics::core OR NOT TARGET felitronics::test_support
   OR NOT EXISTS "${felitronics_core_SOURCE_DIR}/cmake/FelitronicsPolicy.cmake")
    message(FATAL_ERROR
        "felitronics-guitar-core needs felitronics-core v0.51.0 or later, made available BEFORE this "
        "repository (FetchContent_MakeAvailable(felitronics_core) first), so ONE core is resolved by the "
        "product that owns the graph.")
endif()

# Core's build policy — FP contraction (law 10), the MSVC stack + UTF-8, the emscripten physics, the
# sanitizer option, the hygiene warning set — stated for THIS directory by the same file core uses. It is
# load-bearing, not decoration: without it NamStage.cpp and the NAM sources would compile under the
# consumer's FP contraction (gcc's default is `fast`), not the `on` they compile under in core today.
include(${felitronics_core_SOURCE_DIR}/cmake/FelitronicsPolicy.cmake)

# modules/nam and modules/rigplayer came from core verbatim and still self-gate on FELITRONICS_WITH_NAM, the
# name the switch had there; all three build their suites on FELITRONICS_BUILD_TESTS. Both are set here as
# NORMAL variables, after core is configured: core now refuses a FELITRONICS_WITH_NAM of its own (the
# backend moved here), and a normal variable in this directory never reaches it.
set(FELITRONICS_WITH_NAM ${FELITRONICS_GUITAR_WITH_NAM})
set(FELITRONICS_BUILD_TESTS ${FELITRONICS_GUITAR_BUILD_TESTS})
if(FELITRONICS_GUITAR_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(modules/nam)         # compiled NAM backend; self-gates on FELITRONICS_WITH_NAM
add_subdirectory(modules/rigplayer)   # header-only pack player over nam; same gate
add_subdirectory(modules/poweramp)    # header-only; core + eq + oversampling + saturation

if(FELITRONICS_GUITAR_BUILD_TESTS)
    add_subdirectory(tests)           # the law-11 suites for poweramp, header hygiene, the no-exceptions probe
endif()
