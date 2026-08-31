# Vode Plugins

CMake-based C++ monorepo for the **Vode Plugins** VST3 family on Windows (MSVC / VS2022).
Raw [Steinberg VST3 SDK 3.8.1](https://github.com/steinbergmedia/vst3sdk) (MIT),
[Signalsmith DSP v1.7.0](https://github.com/signalsmith-audio/dsp) (MIT),
[Catch2 v3.16.0](https://github.com/catchorg/Catch2) for unit tests, and a custom
offline VST3 test host (`vst3testhost`) for integration tests.

See `PLAN.md` (plan + append-only implementation log) and `INVESTIGATION.md` (research notes).

## Requirements

- Windows with Visual Studio 2022 ("Desktop development with C++", MSVC x64)
- CMake ≥ 3.21 (the VS-bundled one works; it does not need to be on PATH — the scripts find it via vswhere)
- Git ≥ 2.x

Check your machine:

```powershell
.\setup-dev-env.ps1
```

## Getting started

```powershell
git clone --recurse-submodules <repo-url>
cd vode-vst
.\init-working-copy.ps1          # syncs submodules, verifies sentinels, preps build dir
.\init-working-copy.ps1 -Configure   # ...and run cmake configure too
```

Or manually:

```powershell
git submodule update --init --recursive
cmake -B build -G "Visual Studio 17 2022" -A x64 .
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

> **Note:** the VST3 SDK contains *nested* submodules. If a top-level recursive
> update fails (e.g. transient file locks), re-run from inside the SDK:
> `cd third_party\vst3sdk; git submodule update --init --force --recursive`.

## Layout

```
├─ setup-dev-env.ps1            # machine-level prerequisite check (read-only)
├─ init-working-copy.ps1        # per-clone/per-branch bootstrap (submodules, build dir)
├─ CMakeLists.txt               # top-level: project(vdplg), options, submodule guards
├─ third_party/                 # pinned git submodules
│  ├─ vst3sdk/                  # @ v3.8.1_build_84
│  ├─ signalsmith-dsp/          # @ v1.7.0
│  ├─ catch2/                   # @ v3.16.0
│  └─ dr_wav/                   # mackron/dr_libs @ wav-0.14.5
├─ common/                      # shared static lib `common` (WAV I/O, dB utils, analysis)
│  └─ include/vdplg/            # public headers, namespace vdplg
├─ plugins/
│  └─ passthrough/              # template plugin: Gain (dB) + Mix, sample-accurate automation
├─ tests/                       # L0/L1 Catch2 suite → vdplg_tests.exe
├─ testhost/                    # L3 offline host → vst3testhost.exe
└─ testdata/                    # generated WAVs + JSON cases for L3
```

## Building & testing

| Target | Artifact | Purpose |
|---|---|---|
| `passthrough` | `build\VST3\Release\passthrough.vst3` | loadable VST3 plugin |
| `vdplg_tests` | `build\tests\Release\vdplg_tests.exe` | L0 (DSP) + L1 (processor) unit tests |
| `vst3testhost` | `build\testhost\Release\vst3testhost.exe` | L3 offline integration host |
| `gen_wavs` | `build\testdata\Release\gen_wavs.exe` | generates `testdata/wavs/*.wav` |

CTest runs everything: `vdplg_tests` plus one L3 case per `testdata/cases/*.json`
(unity gain, gain applied, mix zero, silence in, automation ramp).

### Using vst3testhost directly

```powershell
.\build\testhost\Release\vst3testhost.exe `
	.\build\VST3\Release\passthrough.vst3 `
	--input testdata\wavs\sine_440.wav `
	--automation "Gain=6dB@0samples" `
	--output out.wav `
	--assert "peak_out >= 1.9" `
	--report json
```

Param names are resolved to indices by enumerating the plugin's `IParameter`s.
Automation syntax: `Name=value@time[,Name=value@time...]` where time is seconds
or `Nsamples`.

## Adding a new plugin

1. Copy `plugins/passthrough/` → `plugins/<name>/`.
2. Generate a **new UUID** and update the ClassID in `<name>cids.h` (each plugin
	 must have a unique VST3 ClassID — do not reuse the passthrough one).
3. Update the factory strings (plugin name, vendor stays "Vode Plugins").
4. Add `add_subdirectory(plugins/<name>)` in the top-level `CMakeLists.txt`.
5. Optionally add L3 cases under `testdata/cases/` (they are auto-discovered by CTest).

## Updating dependencies

```powershell
cd third_party\<dep>
git fetch --tags
git checkout <tag-or-sha>
cd ..\..
git add third_party/<dep>
git commit -m "bump <dep> to <version>"
```

## Conventions

- Brand string everywhere user-facing: **"Vode Plugins"**.
- Internal prefixes: `VDPLG` (CMake options), `vdplg_` (targets/files), namespace `vdplg`, headers under `include/vdplg/`. No "vode" in code identifiers.
- C++17 throughout.