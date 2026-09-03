// Vode Plugins — test entry point shim.
//
// Catch2 v3's Catch2WithMain target defines `wmain` on Windows (wide-char
// entry), but our exe links against the console CRT which requires `main`.
// This shim bridges the two by running the Catch session from `main`.

#include <catch2/catch_session.hpp>

#if defined(_WIN32)
// The SDK's vstguieditor.cpp calls Steinberg::getPlatformModuleHandle(), which
// reads the global-scope moduleHandle normally defined by a plugin DLL's dllmain.cpp.
// Our test exe is not a DLL, so provide the symbol here (nullptr = no module).
#include <cstddef>
void* moduleHandle = nullptr;
#endif

// Defined in l1_sectra_scope.cpp; no-op when no [ui] test ran this process.
extern "C" void vdplg_tests_shutdown_ui ();

int main(int argc, char* argv[]) {
    Catch::Session session;
    const int rc = session.run(argc, argv);
    // Deterministic VSTGUI/COM shutdown while main() is still alive — see
    // l1_sectra_scope.cpp for why cross-TU static destruction order is not
    // relied upon. No-op when no [ui] test ran in this process.
    vdplg_tests_shutdown_ui ();
    return rc;
}
