# Vode Plugins — Sectra Scope: real-time spectrum analyzer (VST3).
#
# Phase 2 (TDD Red): passthrough + six parameters, no UI yet. The VSTGUI
# editor (two stacked scopes + control strip) lands in Phase 4; at that point
# SMTG_ENABLE_VSTGUI_SUPPORT flips ON at the top level and `vstgui` gets linked
# here.

set(sectrascope_sources
    Source/PluginFactory.cpp
    Source/SectraScopeProcessor.h
    Source/SectraScopeProcessor.cpp
    Source/Controller.h
    Source/Controller.cpp
    Source/ScopeView.h
    Source/ScopeView.cpp
    Source/scopedata.h
    Source/version.h
    Source/sectracids.h
    Source/sectraparamids.h
)

smtg_add_vst3plugin(sectrascope ${sectrascope_sources})
smtg_target_configure_version_file(sectrascope)
# Bundle the VSTGUI editor description next to the built .vst3 so the controller
# can load "editor.uidesc" at runtime.
smtg_target_add_plugin_resources(sectrascope
    RESOURCES
        Resource/editor.uidesc
)
# NOTE: smtg_target_configure_version_file() writes projectversion.h into the
# target's binary dir but only adds PROJECT_BINARY_DIR to the include path;
# add the binary dir explicitly so version.h can #include "projectversion.h".
target_include_directories(sectrascope PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_compile_features(sectrascope PUBLIC cxx_std_17)

target_link_libraries(sectrascope
    PRIVATE
        sdk
        vstgui_support
    common
)
