// Vode Plugins — test entry point shim.
//
// Catch2 v3's Catch2WithMain target defines `wmain` on Windows (wide-char
// entry), but our exe links against the console CRT which requires `main`.
// This shim bridges the two by running the Catch session from `main`.

#include <catch2/catch_session.hpp>

int main(int argc, char* argv[]) {
    Catch::Session session;
    return session.run(argc, argv);
}
