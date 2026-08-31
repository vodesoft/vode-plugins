# Vode Plugins — L3 test data.
# Builds a tiny generator that writes the deterministic WAV inputs used by the
# JSON test cases, and runs it at build time so the files always exist.

add_executable(gen_wavs
    tools/gen_wavs.cpp
)
target_link_libraries(gen_wavs PRIVATE common)
target_include_directories(gen_wavs PRIVATE ${CMAKE_SOURCE_DIR}/third_party/dr_wav)
target_compile_features(gen_wavs PRIVATE cxx_std_17)

set(VDPLG_TESTDATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

add_custom_command(
    OUTPUT ${VDPLG_TESTDATA_DIR}/wavs/sine_440.wav
           ${VDPLG_TESTDATA_DIR}/wavs/noise.wav
           ${VDPLG_TESTDATA_DIR}/wavs/silence.wav
    COMMAND gen_wavs ${VDPLG_TESTDATA_DIR}/wavs
    DEPENDS gen_wavs
    COMMENT "Generating L3 test WAV inputs"
    VERBATIM
)
add_custom_target(vdplg_testdata ALL
    DEPENDS
        ${VDPLG_TESTDATA_DIR}/wavs/sine_440.wav
        ${VDPLG_TESTDATA_DIR}/wavs/noise.wav
        ${VDPLG_TESTDATA_DIR}/wavs/silence.wav
)
