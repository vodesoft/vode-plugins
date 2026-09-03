# Vode Plugins — shared library `common` (DSP helpers, WAV I/O, analysis).
add_library(common STATIC
    src/wavio.cpp
    src/analysis.cpp
    src/spectrum.cpp
    src/analysisworker.cpp
    src/dr_wav_impl.cpp
    src/png.cpp
)

# miniz ships no pre-generated export header; we compile tinfl into this static
# lib, so an empty definition is all we need.
file(WRITE ${CMAKE_BINARY_DIR}/generated/miniz_export.h
"#pragma once\n#define MINIZ_EXPORT\n")

target_include_directories(common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/dr_wav
        ${CMAKE_SOURCE_DIR}/third_party/miniz
        ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(common
    PUBLIC
        signalsmith-dsp
)

# Only the inflate core (tinfl) is needed for PNG decoding.
target_sources(common
    PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz_tinfl.c
)

target_compile_features(common PUBLIC cxx_std_17)
