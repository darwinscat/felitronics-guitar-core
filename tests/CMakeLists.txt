# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa Lafoks. Part of felitronics-guitar-core — see LICENSE.

# Law 11 and law 11c for poweramp::PowerAmpStage, through core's shared harnesses (felitronics::test_support):
# the same properties core's CallContractTests and PauseIsSilenceTests apply to every address they own.
add_executable(felitronics_poweramp_call_contract_tests PowerAmpCallContractTests.cpp)
target_link_libraries(felitronics_poweramp_call_contract_tests PRIVATE felitronics::poweramp felitronics::test_support)
add_test(NAME felitronics_poweramp_call_contract_tests COMMAND felitronics_poweramp_call_contract_tests)

add_executable(felitronics_poweramp_pause_is_silence_tests PowerAmpPauseIsSilenceTests.cpp)
target_link_libraries(felitronics_poweramp_pause_is_silence_tests PRIVATE felitronics::poweramp felitronics::test_support)
add_test(NAME felitronics_poweramp_pause_is_silence_tests COMMAND felitronics_poweramp_pause_is_silence_tests)

# Header hygiene and the -fno-exceptions / -fno-rtti probe — Clang/GCC only, as in core (MSVC maps the
# warnings differently and spells the probe /EHs-c-, where it warns rather than errors).
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_executable(felitronics_guitar_header_hygiene HeaderHygiene.cpp)
    target_link_libraries(felitronics_guitar_header_hygiene PRIVATE felitronics::poweramp)
    if(FELITRONICS_GUITAR_WITH_NAM)
        target_link_libraries(felitronics_guitar_header_hygiene PRIVATE felitronics::nam felitronics::rigplayer)
    endif()
    target_compile_options(felitronics_guitar_header_hygiene PRIVATE ${FELITRONICS_HYGIENE_WARNINGS})
    add_test(NAME felitronics_guitar_header_hygiene COMMAND felitronics_guitar_header_hygiene)

    # Building it IS the assertion. NOT linked against nam / rigplayer — see HeaderHygiene.cpp.
    add_library(felitronics_guitar_no_exceptions_probe OBJECT HeaderHygiene.cpp)
    target_link_libraries(felitronics_guitar_no_exceptions_probe PRIVATE felitronics::poweramp)
    target_compile_options(felitronics_guitar_no_exceptions_probe PRIVATE -fno-exceptions -fno-rtti)
endif()
