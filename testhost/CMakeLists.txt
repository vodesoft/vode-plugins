# Vode Plugins — vst3testhost: minimal offline VST3 test host (L3).
# Loads a built .vst3 bundle, pumps WAV input through process() in blocks,
# applies sample-accurate automation, and evaluates assertions / golden compare.

add_executable(vst3testhost
    src/main.cpp
    # The SDK's sdk_hosting target omits the platform-specific Module::create
    # implementation (module_win32.cpp). We compile it directly here.
    ${CMAKE_SOURCE_DIR}/third_party/vst3sdk/public.sdk/source/vst/hosting/module_win32.cpp
)

target_link_libraries(vst3testhost
    PRIVATE
        sdk_hosting
        common
)

target_compile_features(vst3testhost PRIVATE cxx_std_17)
add_dependencies(vst3testhost vdplg_testdata)

# ---------------------------------------------------------------------------
# L3 CTest wiring: each JSON case under testdata/cases becomes a test.
# The plugin path depends on the build config; use a generator expression so
# the same cache works for any configuration.
# ---------------------------------------------------------------------------
enable_testing()

set(VDPLG_L3_PLUGIN_DIR "$<TARGET_FILE_DIR:passthrough>")
set(VDPLG_L3_PLUGIN_PATH "${VDPLG_L3_PLUGIN_DIR}/passthrough.vst3")

file(GLOB VDPLG_L3_CASES "${CMAKE_SOURCE_DIR}/testdata/cases/*.json")
foreach(caseFile ${VDPLG_L3_CASES})
    get_filename_component(caseName ${caseFile} NAME_WE)
    add_test(NAME "l3_${caseName}"
             COMMAND vst3testhost
                     --case "${caseFile}"
                     --plugin "${VDPLG_L3_PLUGIN_PATH}"
                     --report json)
    # Resolve paths relative to the repo root at test time.
    set_tests_properties("l3_${caseName}" PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endforeach()
