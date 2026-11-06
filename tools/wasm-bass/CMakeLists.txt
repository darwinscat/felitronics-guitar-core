# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

# fc_bass — the bass amp's chain (felitronics::bass) with the fc_rig player as its AMP, behind a C ABI
# (../fc_bass_abi.h). Natively: a static library over fc_rig's, the suite that holds it to the chain driven directly,
# and the render the node parity run is held to. Under emscripten: the module a browser AudioWorklet runs, fcbass.mjs +
# fcbass.wasm — fc_rig's translation units and this one in ONE module, so the page drives the player through every
# fc_rig entry point (on the fc_rig* fc_bass_rig() answers) and the chain through fc_bass's — built by build.sh, which
# configures this tree with emcmake. Its flags are fcrig's (tools/wasm-rig/CMakeLists.txt says why each).

# Every entry point either header declares, exported — and each defined exactly once.
set(FC_BASS_ABI_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/../fc_bass_abi.h")
set(FC_BASS_RIG_ABI_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/../fc_rig_abi.h")
set(FC_BASS_EXPORTS "")
foreach(_pair IN ITEMS "fc_bass|${FC_BASS_ABI_HEADER}|fc_bass.cpp" "fc_rig|${FC_BASS_RIG_ABI_HEADER}|../wasm-rig/fc_rig.cpp")
    string(REPLACE "|" ";" _pair "${_pair}")
    list(GET _pair 0 _prefix)
    list(GET _pair 1 _header)
    list(GET _pair 2 _source)
    file(STRINGS "${_header}" _decls REGEX "^[A-Za-z_][A-Za-z0-9_ *]* ${_prefix}_[a-z0-9_]+ \\(")
    set(_count 0)
    foreach(_decl IN LISTS _decls)
        string(REGEX REPLACE "^.* (${_prefix}_[a-z0-9_]+) \\(.*$" "_\\1" _name "${_decl}")
        list(APPEND FC_BASS_EXPORTS "${_name}")
        math(EXPR _count "${_count} + 1")
    endforeach()
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${_source}" _defs REGEX "^FC_EXPORT ")
    list(LENGTH _defs _def_count)
    if(NOT _count EQUAL _def_count OR _count EQUAL 0)
        message(FATAL_ERROR "fc_bass: ${_header} declares ${_count} entry points, ${_source} defines ${_def_count}")
    endif()
endforeach()

if(NOT EMSCRIPTEN)
    add_library(felitronics_bass_abi STATIC fc_bass.cpp)
    add_library(felitronics::bass_abi ALIAS felitronics_bass_abi)
    target_include_directories(felitronics_bass_abi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(felitronics_bass_abi PUBLIC felitronics::rig_abi felitronics::bass)
    target_compile_features(felitronics_bass_abi PUBLIC cxx_std_20)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(felitronics_bass_abi PRIVATE ${FELITRONICS_HYGIENE_WARNINGS})
    endif()
endif()

if(EMSCRIPTEN)
    # The NAM objects with exceptions, nothing else (tools/wasm-rig/CMakeLists.txt, Oleh's decision of 19.10.2026): stated
    # here as well, for the manifest parser compiled into THIS target (a source property is per directory).
    set(FC_BASS_EH -fexceptions -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1)
    target_compile_options(nam_core PRIVATE ${FC_BASS_EH})
    target_compile_options(felitronics_nam PRIVATE ${FC_BASS_EH})
    set_source_files_properties(../wasm-rig/fc_rig_manifest.cpp PROPERTIES COMPILE_OPTIONS "${FC_BASS_EH}")
    add_executable(fcbass fc_bass.cpp ../wasm-rig/fc_rig.cpp ../wasm-rig/fc_rig_manifest.cpp)
    target_include_directories(fcbass PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_SOURCE_DIR}/../wasm-rig")
    target_link_libraries(fcbass PRIVATE felitronics::rigplayer felitronics::bass)
    target_compile_features(fcbass PRIVATE cxx_std_20)
    target_compile_options(fcbass PRIVATE ${FELITRONICS_HYGIENE_WARNINGS})
    list(JOIN FC_BASS_EXPORTS "," _bass_exports)
    set_target_properties(fcbass PROPERTIES SUFFIX ".mjs")
    target_link_options(fcbass PRIVATE
        -O3 --no-entry
        -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createFcBass
        -sENVIRONMENT=worklet -sGROWABLE_ARRAYBUFFERS=0
        -sDYNAMIC_EXECUTION=0 -sFILESYSTEM=0
        "-sEXPORTED_FUNCTIONS=${_bass_exports}"
        "-sEXPORTED_RUNTIME_METHODS=HEAPF32,HEAPU8")
    include(FetchContent)
    FetchContent_GetProperties(nam_core_src SOURCE_DIR _bass_nam_src)
    FetchContent_GetProperties(namz_src SOURCE_DIR _bass_namz_src)
    FetchContent_GetProperties(eigen_src SOURCE_DIR _bass_eigen_src)
    if(NOT _bass_eigen_src)
        set(_bass_eigen_src "${_bass_nam_src}/Dependencies/eigen")
    endif()
    file(WRITE "${CMAKE_BINARY_DIR}/fc-bass-deps.txt"
         "${felitronics_core_SOURCE_DIR}\n${_bass_nam_src}\n${_bass_namz_src}\n${_bass_eigen_src}\n")
endif()

if(FELITRONICS_GUITAR_BUILD_TESTS AND NOT EMSCRIPTEN)
    # The ABI against BassChain and the player driven directly, bit for bit; the contract; no allocation in process().
    add_executable(felitronics_bass_abi_tests tests/BassAbiTests.cpp)
    target_include_directories(felitronics_bass_abi_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../wasm-rig")
    target_link_libraries(felitronics_bass_abi_tests PRIVATE felitronics::bass_abi felitronics::test_support)
    add_test(NAME felitronics_bass_abi_tests COMMAND felitronics_bass_abi_tests)
endif()

if(NOT EMSCRIPTEN)
    # The native half of bass-parity.mjs: a pack and a cabinet through this same ABI on the processor's schedule.
    add_executable(fc_bass_render fc_bass_render.cpp)
    target_link_libraries(fc_bass_render PRIVATE felitronics::bass_abi)

    # The wasm module and the native render against the plug-in's own renders, when there are all three: build.sh's output,
    # orbit-amp's RedGrooveBassReference output (its renders, --cabinets, --params) and the pack it played.
    set(FELITRONICS_GUITAR_BASS_WASM_DIR "" CACHE PATH "tools/wasm-bass/build.sh output for the parity run")
    set(FELITRONICS_GUITAR_BASS_REFERENCES "" CACHE PATH "RedGrooveBassReference's output (orbit-amp) the parity run is held to")
    set(FELITRONICS_GUITAR_BASS_PACK "" CACHE PATH "The .orbitrig the references played (orbit-amp resources/set/Devices/Ampeg SGT-DI.orbitrig)")
    if(FELITRONICS_GUITAR_BASS_WASM_DIR AND FELITRONICS_GUITAR_BASS_REFERENCES AND FELITRONICS_GUITAR_BASS_PACK)
        find_program(FELITRONICS_NODE node REQUIRED)
        add_test(NAME felitronics_bass_wasm_parity
                 COMMAND ${FELITRONICS_NODE} --experimental-vm-modules ${CMAKE_CURRENT_SOURCE_DIR}/bass-parity.mjs
                         ${FELITRONICS_GUITAR_BASS_WASM_DIR} $<TARGET_FILE:fc_bass_render>
                         ${FELITRONICS_GUITAR_BASS_REFERENCES} ${FELITRONICS_GUITAR_BASS_PACK})
    endif()
endif()
