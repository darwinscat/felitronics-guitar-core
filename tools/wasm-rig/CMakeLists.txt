# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

# fc_rig — felitronics::rigplayer behind a C ABI (../fc_rig_abi.h). Natively: a static library and the suites that
# hold it to RigPlayer driven directly. Under emscripten: the module a browser AudioWorklet runs, fcrig.mjs +
# fcrig.wasm — built by build.sh, which configures this tree with emcmake.
#
# A DIRECTORY OF ITS OWN beside tools/wasm, not a second target there: this module links NAM, and NAM is the one
# thing in this repository that throws. Its flags — C++ exceptions on the NAM objects, SIMD128 with Eigen's SSE path —
# are a different build from fcguitarfx's, which stays exception-free and scalar; one build.sh per module keeps
# either from leaking into the other.

set(FC_RIG_ABI_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/../fc_rig_abi.h")
file(STRINGS "${FC_RIG_ABI_HEADER}" _rig_decls REGEX "^[A-Za-z_][A-Za-z0-9_ *]* fc_rig_[a-z_]+ \\(")
set(FC_RIG_EXPORTS "")
foreach(_decl IN LISTS _rig_decls)
    string(REGEX REPLACE "^.* (fc_rig_[a-z_]+) \\(.*$" "_\\1" _name "${_decl}")
    list(APPEND FC_RIG_EXPORTS "${_name}")
endforeach()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fc_rig.cpp" _rig_defs REGEX "^FC_EXPORT ")
list(LENGTH FC_RIG_EXPORTS _rig_count)
list(LENGTH _rig_defs _rig_def_count)
if(NOT _rig_count EQUAL _rig_def_count OR _rig_count EQUAL 0)
    message(FATAL_ERROR "fc_rig: ${FC_RIG_ABI_HEADER} declares ${_rig_count} entry points, fc_rig.cpp defines ${_rig_def_count}")
endif()

if(NOT EMSCRIPTEN)
    add_library(felitronics_rig_abi STATIC fc_rig.cpp fc_rig_manifest.cpp)
    add_library(felitronics::rig_abi ALIAS felitronics_rig_abi)
    target_include_directories(felitronics_rig_abi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(felitronics_rig_abi PUBLIC felitronics::rigplayer)
    target_compile_features(felitronics_rig_abi PUBLIC cxx_std_20)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(felitronics_rig_abi PRIVATE ${FELITRONICS_HYGIENE_WARNINGS})
    endif()
endif()


if(EMSCRIPTEN)
    # THE BROWSER MODULE. The numeric contract is the tree's (FelitronicsPolicy.cmake: law 10's FP contraction, the
    # emscripten physics, --wrap=pthread_create). build.sh passes, for every object, what core's wasm-audio preset
    # states as policy — -fno-exceptions -fno-rtti — plus -msimd128 -msse4.1: SIMD128 the ISA (never relaxed SIMD),
    # and Eigen's SSE packet path, which emscripten lowers onto it (one model 3.7 % of a 128-frame quantum against
    # more without; the measurement spike of 19.10.2026).
    #
    # EXCEPTIONS ON THE NAM OBJECTS ONLY — Oleh's decision of 19.10.2026: C++ exceptions are allowed for our NAM
    # (NeuralAmpModelerCore and nlohmann) in this build, and nowhere else. NeuralAmpModelerCore throws on a model it
    # cannot read and nlohmann throws on JSON it cannot parse; NamStage::prepareModel catches both, and a load that
    # fails is a refused model, not an aborted module. So: nam_core (the NAM sources), felitronics_nam (NamStage.cpp,
    # the catch) and fc_rig_manifest.cpp (nlohmann's parser under the pack's manifest) are compiled with exceptions;
    # fc_rig.cpp — the ABI, RigPlayer, its process() — and every core object are not. Native wasm exceptions in the
    # LEGACY encoding (try/catch/rethrow): Chrome 95, Firefox 100, Safari 15.2 — the exnref encoding would ask for
    # Chrome 137, Firefox 131, Safari 18.4 (emscripten's tools/feature_matrix.py; README.md, "Browser floor").
    set(FC_RIG_EH -fexceptions -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1)
    target_compile_options(nam_core PRIVATE ${FC_RIG_EH})
    target_compile_options(felitronics_nam PRIVATE ${FC_RIG_EH})
    set_source_files_properties(fc_rig_manifest.cpp PROPERTIES COMPILE_OPTIONS "${FC_RIG_EH}")

    # Linked as fcguitarfx is (tools/wasm/CMakeLists.txt says why each): worklet-only glue, no growable
    # ArrayBuffers, ES6 factory, no eval, no file system. And:
    #   -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1   the C++ runtime that catches what the NAM objects throw.
    #   the default allocator (dlmalloc), not emmalloc: a model is hundreds of allocations, and loads come and go.
    #   HEAPF32, HEAPU8   the processor's views: the channel buffers, the bytes of a capture and of the manifest.
    #                     Re-read after every call that may allocate — a load grows the memory, which detaches every
    #                     older view (fcrig-worklet.js checks its two at the top of every quantum).
    add_executable(fcrig fc_rig.cpp fc_rig_manifest.cpp)
    target_include_directories(fcrig PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(fcrig PRIVATE felitronics::rigplayer)
    target_compile_features(fcrig PRIVATE cxx_std_20)
    target_compile_options(fcrig PRIVATE ${FELITRONICS_HYGIENE_WARNINGS})
    list(JOIN FC_RIG_EXPORTS "," _rig_exports)
    set_target_properties(fcrig PROPERTIES SUFFIX ".mjs")
    target_link_options(fcrig PRIVATE
        -O3 --no-entry
        -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createFcRig
        -sENVIRONMENT=worklet -sGROWABLE_ARRAYBUFFERS=0
        -sDYNAMIC_EXECUTION=0 -sFILESYSTEM=0
        "-sEXPORTED_FUNCTIONS=${_rig_exports}"
        "-sEXPORTED_RUNTIME_METHODS=HEAPF32,HEAPU8")
    # What this module was compiled against, for build.sh's BUILD-INFO (modules/nam fetched the last three; their
    # variables live in that directory's scope, the FetchContent properties are global). Eigen is fetched on its own
    # from the mirror — unless the NeuralAmpModelerCore on disk brought its own (modules/nam, FELITRONICS_EIGEN_DIR).
    include(FetchContent)
    FetchContent_GetProperties(nam_core_src SOURCE_DIR _rig_nam_src)
    FetchContent_GetProperties(namz_src SOURCE_DIR _rig_namz_src)
    FetchContent_GetProperties(eigen_src SOURCE_DIR _rig_eigen_src)
    if(NOT _rig_eigen_src)
        set(_rig_eigen_src "${_rig_nam_src}/Dependencies/eigen")
    endif()
    file(WRITE "${CMAKE_BINARY_DIR}/fc-rig-deps.txt"
         "${felitronics_core_SOURCE_DIR}\n${_rig_nam_src}\n${_rig_namz_src}\n${_rig_eigen_src}\n")
endif()


if(FELITRONICS_GUITAR_BUILD_TESTS AND NOT EMSCRIPTEN)
    # The ABI against RigPlayer driven directly, bit for bit; the job protocol; the contract; no allocation in process().
    add_executable(felitronics_rig_abi_tests tests/RigAbiTests.cpp)
    target_include_directories(felitronics_rig_abi_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(felitronics_rig_abi_tests PRIVATE felitronics::rig_abi felitronics::test_support)
    add_test(NAME felitronics_rig_abi_tests COMMAND felitronics_rig_abi_tests)
    add_executable(felitronics_rig_preload_tests tests/RigPreloadTests.cpp)
    target_include_directories(felitronics_rig_preload_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(felitronics_rig_preload_tests PRIVATE felitronics::rig_abi felitronics::test_support)
    add_test(NAME felitronics_rig_preload_tests COMMAND felitronics_rig_preload_tests)

    # The native half of the node suite: a pack through this same ABI on the processor's schedule.
    add_executable(fc_rig_render fc_rig_render.cpp)
    target_include_directories(fc_rig_render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(fc_rig_render PRIVATE felitronics::rig_abi)

    # The wasm module against that render, when there is one: FELITRONICS_GUITAR_RIG_WASM_DIR is build.sh's output.
    # The fixture pack always; real packs too when FELITRONICS_GUITAR_RIG_PACKS names a directory of *.orbitrig.
    set(FELITRONICS_GUITAR_RIG_WASM_DIR "" CACHE PATH "tools/wasm-rig/build.sh output to diff against the native render")
    set(FELITRONICS_GUITAR_RIG_PACKS "" CACHE PATH "A directory of .orbitrig packs for the wasm parity run (optional)")
    if(FELITRONICS_GUITAR_RIG_WASM_DIR)
        find_program(FELITRONICS_NODE node REQUIRED)
        # namz's own CLI at the pin (cli/namz.cpp, header-only, nlohmann from NeuralAmpModelerCore as modules/nam takes
        # it): the parity run makes its float16 packs with it — `encode --f16`, as a pack's author would.
        include(FetchContent)
        FetchContent_GetProperties(namz_src SOURCE_DIR _rig_namz_cli_src)
        FetchContent_GetProperties(nam_core_src SOURCE_DIR _rig_nam_cli_src)
        add_executable(fc_rig_namz "${_rig_namz_cli_src}/cli/namz.cpp")
        target_include_directories(fc_rig_namz SYSTEM PRIVATE "${_rig_namz_cli_src}/include"
                                   "${_rig_nam_cli_src}/Dependencies/nlohmann" "${_rig_nam_cli_src}/Dependencies")
        target_compile_features(fc_rig_namz PRIVATE cxx_std_17)
        add_test(NAME felitronics_rig_wasm_parity
                 COMMAND ${FELITRONICS_NODE} --experimental-vm-modules --expose-gc ${CMAKE_CURRENT_SOURCE_DIR}/rig-parity.mjs
                         --namz=$<TARGET_FILE:fc_rig_namz>
                         ${FELITRONICS_GUITAR_RIG_WASM_DIR} $<TARGET_FILE:fc_rig_render> ${FC_RIG_ABI_HEADER}
                         ${FELITRONICS_GUITAR_RIG_PACKS})
    endif()
endif()
