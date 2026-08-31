# Vode Plugins — passthrough template plugin (VST3).
#
# Textless effect: stereo in/out, Gain (dB) + Mix (dry/wet) parameters.
# No UI resources: the controller's createView() returns nullptr.

set(passthrough_sources
    Source/PluginFactory.cpp
    Source/Processor.h
    Source/Processor.cpp
    Source/Controller.h
    Source/Controller.cpp
    Source/version.h
    Source/passthroughcids.h
    Source/passthroughparamids.h
)

smtg_add_vst3plugin(passthrough ${passthrough_sources})
smtg_target_configure_version_file(passthrough)
# NOTE: smtg_target_configure_version_file() writes projectversion.h into the
# target's binary dir but only adds PROJECT_BINARY_DIR to the include path;
# add the binary dir explicitly so version.h can #include "projectversion.h".
target_include_directories(passthrough PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_compile_features(passthrough PUBLIC cxx_std_17)

target_link_libraries(passthrough
    PRIVATE
        sdk
    common
)
