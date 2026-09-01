# Vode Plugins — L0/L1 Catch2 test suite (`vdplg_tests`).
#
# L0: Signalsmith DSP building blocks + Sectra Scope spectrum DSP.
# L1: passthrough processor driven directly through process().
#     Sectra Scope processor driven directly through process().

add_executable(vdplg_tests
    src/test_main.cpp
    l0_dsp.cpp
    l1_passthrough.cpp
    l0_spectrum.cpp
    l1_sectra_scope.cpp
)

target_link_libraries(vdplg_tests
    PRIVATE
        common
        sdk
        # NOTE: Catch2WithMain defines `wmain` on Windows, but our console exe
        # links the CRT that requires `main`. We use plain Catch2 + our own
        # shim in src/test_main.cpp that runs the Catch session from `main`.
        Catch2
)

# The passthrough processor sources are compiled straight into the test exe
# (the plugin target itself is a DLL with factory entry points we don't need).
target_sources(vdplg_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/plugins/passthrough/Source/Processor.h
        ${CMAKE_SOURCE_DIR}/plugins/passthrough/Source/Processor.cpp
        ${CMAKE_SOURCE_DIR}/plugins/passthrough/Source/passthroughcids.h
        ${CMAKE_SOURCE_DIR}/plugins/passthrough/Source/passthroughparamids.h
        ${CMAKE_SOURCE_DIR}/plugins/sectra-scope/Source/SectraScopeProcessor.h
        ${CMAKE_SOURCE_DIR}/plugins/sectra-scope/Source/SectraScopeProcessor.cpp
        ${CMAKE_SOURCE_DIR}/plugins/sectra-scope/Source/sectracids.h
        ${CMAKE_SOURCE_DIR}/plugins/sectra-scope/Source/sectraparamids.h
)
target_include_directories(vdplg_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/plugins/passthrough/Source
        ${CMAKE_SOURCE_DIR}/plugins/sectra-scope/Source
)

target_compile_features(vdplg_tests PRIVATE cxx_std_17)

add_test(NAME vdplg_tests COMMAND vdplg_tests)
