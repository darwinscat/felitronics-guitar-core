# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

# fc_guitar_fx — the amp's echo and room behind a C ABI (../fc_guitar_fx_abi.h). Natively: a static library and the
# suites that hold it to the core classes. Under emscripten: the module a browser AudioWorklet runs,
# fcguitarfx.mjs + fcguitarfx.wasm — built by build.sh, which configures this tree with emcmake.

# THE EXPORT LIST, read from the header: every `fc_guitar_fx_*` it declares, with the linker's leading underscore. A
# statement of the ABI rather than the thing that keeps a name alive (EMSCRIPTEN_KEEPALIVE does that); the node suite
# holds the built module's exports to exactly this list.
set(FC_GUITAR_FX_ABI_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/../fc_guitar_fx_abi.h")
file(STRINGS "${FC_GUITAR_FX_ABI_HEADER}" _fx_decls REGEX "^[A-Za-z_][A-Za-z0-9_ *]* fc_guitar_fx_[a-z_]+ \\(")
set(FC_GUITAR_FX_EXPORTS "")
foreach(_decl IN LISTS _fx_decls)
    string(REGEX REPLACE "^.* (fc_guitar_fx_[a-z_]+) \\(.*$" "_\\1" _name "${_decl}")
    list(APPEND FC_GUITAR_FX_EXPORTS "${_name}")
endforeach()
# ...and as many as the translation unit defines: a declaration this regex cannot read fails the configure instead of
# quietly shortening the list.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/fc_guitar_fx.cpp" _fx_defs REGEX "^FC_EXPORT ")
list(LENGTH FC_GUITAR_FX_EXPORTS _fx_count)
list(LENGTH _fx_defs _fx_def_count)
if(NOT _fx_count EQUAL _fx_def_count OR _fx_count EQUAL 0)
    message(FATAL_ERROR "fc_guitar_fx: ${FC_GUITAR_FX_ABI_HEADER} declares ${_fx_count} entry points, "
                        "fc_guitar_fx.cpp defines ${_fx_def_count}")
endif()

add_library(felitronics_guitar_fx_abi STATIC fc_guitar_fx.cpp)
add_library(felitronics::guitar_fx_abi ALIAS felitronics_guitar_fx_abi)
target_include_directories(felitronics_guitar_fx_abi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(felitronics_guitar_fx_abi PUBLIC felitronics::delay felitronics::reverb)
target_compile_features(felitronics_guitar_fx_abi PUBLIC cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(felitronics_guitar_fx_abi PRIVATE ${FELITRONICS_HYGIENE_WARNINGS})
endif()


if(EMSCRIPTEN)
    # THE BROWSER MODULE. The numeric contract is the tree's: FelitronicsPolicy.cmake (law 10's FP contraction, the
    # emscripten physics, --wrap=pthread_create), included by the top-level CMakeLists, plus what core's wasm-audio
    # preset states as policy — no exceptions, no RTTI (build.sh passes those in CMAKE_CXX_FLAGS; see there why
    # not -msimd128).
    #
    # How it is linked, and why:
    #   -sENVIRONMENT=worklet   the glue is imported by the AudioWorklet processor and by nothing else: the page
    #                           compiles the .wasm itself and posts the WebAssembly.Module to the processor, which
    #                           hands it to the factory through `instantiateWasm`. So the glue never fetches,
    #                           never builds a URL and never decodes text (README.md, "Loading it in a worklet").
    #   -sEXPORT_ES6            a worklet is an ES module; the processor imports this one statically.
    #   -sMODULARIZE            one factory, one instance per processor: two nodes on a page share nothing.
    #   -sDYNAMIC_EXECUTION=0   no eval / new Function in the glue.
    #   -sFILESYSTEM=0, emmalloc   nothing here opens a file; the smaller allocator is enough for two allocations.
    #   HEAPF32                 the processor's views onto the module's own two channel buffers.
    add_executable(fcguitarfx fc_guitar_fx.cpp)
    target_include_directories(fcguitarfx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(fcguitarfx PRIVATE felitronics::delay felitronics::reverb)
    target_compile_features(fcguitarfx PRIVATE cxx_std_20)
    target_compile_options(fcguitarfx PRIVATE ${FELITRONICS_HYGIENE_WARNINGS})
    list(JOIN FC_GUITAR_FX_EXPORTS "," _fx_exports)
    set_target_properties(fcguitarfx PROPERTIES SUFFIX ".mjs")
    target_link_options(fcguitarfx PRIVATE
        -O3 --no-entry
        -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createFcGuitarFx
        -sENVIRONMENT=worklet
        -sDYNAMIC_EXECUTION=0 -sFILESYSTEM=0 -sMALLOC=emmalloc
        "-sEXPORTED_FUNCTIONS=${_fx_exports}"
        "-sEXPORTED_RUNTIME_METHODS=HEAPF32")
    # Which core this module was compiled against, for build.sh's BUILD-INFO.
    file(WRITE "${CMAKE_BINARY_DIR}/felitronics-core-dir.txt" "${felitronics_core_SOURCE_DIR}\n")
endif()


if(FELITRONICS_GUITAR_BUILD_TESTS)
    # The ABI against the core classes driven directly with the app's mapping, bit for bit; the contract; no
    # allocation in process().
    add_executable(felitronics_guitar_fx_abi_tests tests/GuitarFxAbiTests.cpp)
    target_link_libraries(felitronics_guitar_fx_abi_tests PRIVATE felitronics::guitar_fx_abi felitronics::test_support)
    add_test(NAME felitronics_guitar_fx_abi_tests COMMAND felitronics_guitar_fx_abi_tests)

    # The suite above runs under emscripten too (CI's wasm-audio job, and its SAFE_HEAP build); what follows is
    # native only. The native half of the node suite: renders what fx-parity.mjs asks for, through this same ABI.
    if(EMSCRIPTEN)
        return()
    endif()
    add_executable(fc_guitar_fx_render fc_guitar_fx_render.cpp)
    target_link_libraries(fc_guitar_fx_render PRIVATE felitronics::guitar_fx_abi)

    # The wasm module against that render, when there is a module to hold to it: FELITRONICS_GUITAR_FX_WASM_DIR is
    # build.sh's output directory (fcguitarfx.mjs, fcguitarfx.wasm, fcguitarfx-worklet.js).
    set(FELITRONICS_GUITAR_FX_WASM_DIR "" CACHE PATH "tools/wasm/build.sh output to diff against the native render")
    if(FELITRONICS_GUITAR_FX_WASM_DIR)
        find_program(FELITRONICS_NODE node REQUIRED)
        add_test(NAME felitronics_guitar_fx_wasm_parity
                 COMMAND ${FELITRONICS_NODE} --experimental-vm-modules --expose-gc ${CMAKE_CURRENT_SOURCE_DIR}/fx-parity.mjs
                         ${FELITRONICS_GUITAR_FX_WASM_DIR} $<TARGET_FILE:fc_guitar_fx_render>
                         ${FC_GUITAR_FX_ABI_HEADER})
    endif()
endif()
