# Vode Plugins — shared library `common` (DSP helpers, WAV I/O, analysis).
add_library(common STATIC
    src/wavio.cpp
    src/analysis.cpp
    src/spectrum.cpp
    src/dr_wav_impl.cpp
)

target_include_directories(common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/dr_wav
)

target_link_libraries(common
    PUBLIC
        signalsmith-dsp
)

target_compile_features(common PUBLIC cxx_std_17)
