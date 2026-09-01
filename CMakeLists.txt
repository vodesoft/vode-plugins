# Vode Plugins (vdplg) — monorepo build root
# See PLAN.md for layout and rationale.
cmake_minimum_required(VERSION 3.21)

project(vdplg
    VERSION 0.1.0
    DESCRIPTION "Vode Plugins — VST3 effect plugin family"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(VDPLG_BUILD_TESTS    "Build Catch2 unit tests (L0/L1)"        ON)
option(VDPLG_BUILD_TESTHOST "Build the offline VST3 test host (L3)"  ON)

# ---------------------------------------------------------------------------
# Submodule guard: fail early with an actionable message if third_party/* is
# empty (plain clone without --recurse-submodules).
# ---------------------------------------------------------------------------
set(VDPLG_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk")
if(NOT EXISTS "${VDPLG_SDK_ROOT}/pluginterfaces/vst/ivstcomponent.h")
    message(FATAL_ERROR
        "[vdplg] VST3 SDK submodule is not populated.\n"
        "Run from the repo root:\n"
        "    git submodule update --init --recursive\n"
        "(or re-run .\\init-working-copy.ps1)")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-dsp/filters.h")
    message(FATAL_ERROR
        "[vdplg] signalsmith-dsp submodule is not populated. Run 'git submodule update --init --recursive'.")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dr_wav/dr_wav.h")
    message(FATAL_ERROR
        "[vdplg] dr_wav submodule is not populated. Run 'git submodule update --init --recursive'.")
endif()
if(VDPLG_BUILD_TESTS AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/catch2/CMakeLists.txt")
    message(FATAL_ERROR
        "[vdplg] catch2 submodule is not populated. Run 'git submodule update --init --recursive'.")
endif()

# ---------------------------------------------------------------------------
# VST3 SDK: embed only the library targets we need (no samples, no hosting
# examples, no validator). See PLAN.md "Implementation log".
# ---------------------------------------------------------------------------
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)   # no admin symlink step
set(SMTG_RUN_VST_VALIDATOR  OFF CACHE BOOL "" FORCE)   # no validator target
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE) # textless editor

list(APPEND CMAKE_MODULE_PATH "${VDPLG_SDK_ROOT}/cmake/modules")
include(SMTG_VST3_SDK)

smtg_setup_platform_toolset()

set(SDK_ROOT "${VDPLG_SDK_ROOT}")
set(public_sdk_SOURCE_DIR "${SDK_ROOT}/public.sdk")
set(pluginterfaces_SOURCE_DIR "${SDK_ROOT}/pluginterfaces")

smtg_create_lib_base_target()            # -> base
smtg_create_pluginterfaces_target()      # -> pluginterfaces
smtg_create_public_sdk_common_target()   # -> sdk_common
smtg_create_public_sdk_target()          # -> sdk
smtg_create_public_sdk_hosting_target()  # -> sdk_hosting (used by vst3testhost)

# moduleinfotool: small utility that writes Contents/Resources/moduleinfo.json
# into each built .vst3 bundle (post-build step of smtg_add_vst3plugin).
add_executable(moduleinfotool
    ${SDK_ROOT}/public.sdk/samples/vst-utilities/moduleinfotool/source/main.cpp
    ${SDK_ROOT}/public.sdk/source/vst/moduleinfo/json.h
    ${SDK_ROOT}/public.sdk/source/vst/moduleinfo/jsoncxx.h
    ${SDK_ROOT}/public.sdk/source/vst/moduleinfo/moduleinfo.h
    ${SDK_ROOT}/public.sdk/source/vst/moduleinfo/moduleinfocreator.cpp
    ${SDK_ROOT}/public.sdk/source/vst/moduleinfo/moduleinfocreator.h
    ${SDK_ROOT}/public.sdk/source/vst/moduleinfo/moduleinfoparser.cpp
    ${SDK_ROOT}/public.sdk/source/vst/moduleinfo/moduleinfoparser.h
    ${SDK_ROOT}/public.sdk/source/vst/hosting/module_win32.cpp
)
target_link_libraries(moduleinfotool PRIVATE sdk_hosting)
target_include_directories(moduleinfotool PRIVATE ${SDK_ROOT})
set_target_properties(moduleinfotool PROPERTIES FOLDER "Utilities")

# ---------------------------------------------------------------------------
# Our directories
# ---------------------------------------------------------------------------
# Signalsmith DSP (MIT, header-only INTERFACE target).
add_subdirectory(third_party/signalsmith-dsp)

add_subdirectory(common)
add_subdirectory(plugins/passthrough)
add_subdirectory(plugins/sectra-scope)
add_subdirectory(testdata)

if(VDPLG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(third_party/catch2)
    add_subdirectory(tests)
endif()

if(VDPLG_BUILD_TESTHOST)
    add_subdirectory(testhost)
endif()
