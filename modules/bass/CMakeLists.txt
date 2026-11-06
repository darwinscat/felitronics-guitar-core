# felitronics::bass — THE BASS AMP'S CHAIN: its stages in the app's order and the app's knobs wired to them (BassChain),
# moved from OrbitAmp's processor so the bass amps' plug-ins and their web port (tools/wasm-bass) play one chain.
# Header-only, JUCE-free; the stages are felitronics-core's (v0.63.0+): basscomp, cabinet, eq, dynamics, octave,
# envfilter, chorus. The AMP between its front and its back is the host's (in the web port: rigplayer and eq::EqLink).
add_library(felitronics_bass INTERFACE)
add_library(felitronics::bass ALIAS felitronics_bass)
target_include_directories(felitronics_bass INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(felitronics_bass INTERFACE
    felitronics::core felitronics::eq felitronics::dynamics felitronics::basscomp felitronics::cabinet
    felitronics::octave felitronics::envfilter felitronics::chorus)
target_compile_features(felitronics_bass INTERFACE cxx_std_20)

if(FELITRONICS_BUILD_TESTS)
    add_executable(felitronics_bass_chain_tests tests/BassChainTests.cpp)
    target_link_libraries(felitronics_bass_chain_tests PRIVATE felitronics::bass felitronics::test_support)
    target_compile_features(felitronics_bass_chain_tests PRIVATE cxx_std_20)
    add_test(NAME felitronics_bass_chain_tests COMMAND felitronics_bass_chain_tests)
endif()
