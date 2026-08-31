# Plan: Vode Plugins — Monorepo Skeleton (Raw VST3 SDK, all-MIT)

**Date:** 2026-08-31
**Status:** Approved plan — Phase 1 (Tests) pending start
**Brand:** **Vode Plugins** (replaces the working name "vode-vst"). Used as the product/vendor brand everywhere user-facing: plugin vendor string in VST3 factories ("Vode Plugins"), README title, about boxes. **Naming rule (user decision):** no "vode" in folder or variable prefixes — the shared library is simply `common`, and all internal prefixes (CMake options, targets, namespaces, files) use **`VDPLG`** / `vdplg_`. CMake project name: `vdplg`. The on-disk workspace folder remains `d:\Projects\vode-vst` unless the user asks to rename it.

## TL;DR
Create a CMake-based C++ monorepo skeleton for the **Vode Plugins** family — multiple VST3 effect plugins on Windows (MSVC/VS2022) — using the **raw Steinberg VST3 SDK 3.8.1 (MIT)** as the plugin framework, **Signalsmith DSP v1.7.0 (MIT)** for DSP building blocks, **Catch2 v3** for L0/L1 unit tests, and a custom **offline VST3 test host (`vst3testhost`)** for L3 integration tests — per INVESTIGATION.md §5-alt, §6.1, §8.3. Dependencies via **git submodules** (pinned commits). Windows-only build for now; repo structured so an osxcross arm64 toolchain can be added later.

## User decisions (from Q&A)
- Framework: **Raw VST3 SDK** (strictly all-MIT), NOT JUCE/iPlug2.
- Scope: minimal plugin + Catch2 unit tests (L0/L1) + `vst3testhost` (L3). No GitHub Actions CI in this plan.
- **Monorepo layout**: separate folder per plugin + a shared "common" part (project hosts multiple plugins).
- macOS: Windows-only for now; keep CMake clean for a future `-DCMAKE_TOOLCHAIN_FILE` osxcross build.
- Dependencies: **git submodules** with pinned commits/tags (changed from FetchContent at user request).

## Verified facts (web, 2026-08-31)
- VST3 SDK: `github.com/steinbergmedia/vst3sdk`, MIT, latest tag **VST3 SDK 3.8.1**. Uses git submodules internally (base, cmake, pluginterfaces, public.sdk, vstgui4, tutorials) → our clone of it as a submodule must be recursive (`git submodule update --init --recursive`) so nested submodules populate. Windows build: `cmake -G "Visual Studio 17 2022" -A x64 ..\vst3sdk`; note `-DSMTG_CREATE_PLUGIN_LINK=0` option to avoid symlink issues.
- Signalsmith DSP: `github.com/signalsmith-audio/dsp`, MIT, header-only C++11, latest **v1.7.0**, headers under `include/signalsmith-dsp/`, provides its own `CMakeLists.txt` (INTERFACE target `signalsmith-dsp`).
- Catch2 v3: `github.com/catchorg/Catch2`, BSL-1.0, CMake-based, `Catch2WithMain` target.
- dr_wav: public domain single-header WAV I/O for the test host (or miniaudio, MIT).

## Proposed repository layout
```
vdplg/                         # brand: "Vode Plugins"; CMake project `vdplg` (workspace dir may stay vode-vst)
├─ setup-dev-env.ps1              # machine-level prerequisites check (see Phase 0)
├─ init-working-copy.ps1          # per-working-copy/branch bootstrap: submodules, build dir (Phase 0)
├─ CMakeLists.txt                 # top-level: project(), C++17, guard on populated submodules, add_subdirectory()
├─ third_party/                   # git submodules (pinned commits)
│  ├─ vst3sdk/                    # github.com/steinbergmedia/vst3sdk @ "VST3 SDK 3.8.1" (has nested submodules)
│  ├─ signalsmith-dsp/            # github.com/signalsmith-audio/dsp @ v1.7.0
│  ├─ catch2/                     # github.com/catchorg/Catch2 @ latest v3.x stable
│  └─ dr_wav/                     # github.com/mackron/dr_wav (public domain, single header)
├─ common/                        # SHARED code across all plugins
│  ├─ CMakeLists.txt              # static lib `common` (DSP helpers, param utils, test helpers)
│  ├─ include/vdplg/              # headers: dsp chain helpers, wav io wrapper, assert/analysis utils
│  └─ src/
├─ plugins/                       # one folder per plugin
│  └─ passthrough/                # first (template) plugin: minimal effect
│     ├─ CMakeLists.txt           # builds <name>.vst3 via SMTG_add_vst3_plugin (SDK macro)
│     └─ Source/
│        ├─ PluginFactory.cpp     # factory + component registration
│        ├─ Processor.h/.cpp      # AudioEffect: passthrough + 1-2 params (e.g., dry/wet gain)
│        └─ Editor.h/.cpp         # minimal editor (VSTGUI or textless stub)
├─ testhost/                      # L3: offline VST3 host (per INVESTIGATION §8.3)
│  ├─ CMakeLists.txt              # exe `vst3testhost`, links vst3sdk pluginterfaces + common
│  └─ src/
│     ├─ main.cpp                 # CLI parsing (§8.3.2)
│     ├─ host/                    # load/instantiate/configure/process lifecycle (§8.3.1)
│     ├─ automation.cpp           # IParameterChanges sample-accurate param changes
│     ├─ wavio.cpp                # dr_wav wrapper
│     └─ analysis/                # peak/RMS/DC/FFT magnitude/THD/golden compare (Signalsmith fft)
├─ tests/                         # L0/L1 Catch2 suite
│  ├─ CMakeLists.txt              # exe `vdplg_tests`, links common + Catch2WithMain; CTest wiring
│  ├─ dsp/                        # Signalsmith component tests (biquad response, envelope follower)
│  └─ processor/                  # instantiate passthrough processor directly, feed buffers
├─ testdata/                      # WAV inputs + JSON test cases for vst3testhost
│  ├─ sine_440.wav, noise.wav, silence.wav
│  └─ cases/*.json                # {input, sampleRate, blockSize, paramAutomation[], assertions[]}
├─ LICENSES/                      # MIT notices for vendored/fetched libs
├─ .gitignore                     # build/, .vs/, etc.
└─ README.md                      # build instructions (VS2022 + CMake), layout explanation
```

## Steps

### Phase 0 — Bootstrap scripts (two, both idempotent & re-run safe)
Two PowerShell scripts at repo root, split by scope. Both must be **safe to re-run at any time**: every step checks current state first and no-ops if already done; re-running on an already-prepared machine/copy changes nothing and exits 0 quickly.

#### 0a. `setup-dev-env.ps1` — machine-level prerequisites (once per machine, not per copy)
Checks/installs nothing automatically — it **detects and reports** what the machine needs, failing fast with actionable messages (non-zero exit) when something is missing:
1. **OS/tooling checks:** Windows + PowerShell ≥ 5.1; `git` on PATH (version ≥ minimum); `cmake` on PATH (≥ 3.21); Visual Studio 2022 with "Desktop development with C++" (MSVC x64) detected via `vswhere` (`-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64`) or `cl.exe` fallback.
2. **Optional dev tools (warn-only, non-fatal):** a VST3 test host (e.g., REAPER) for manual sanity checks; `git` identity configured (`user.name`/`user.email`) — warn only.
3. **Output:** summary table of each prerequisite → found/missing + version + exact install instruction (VS2022 workload name, cmake.org link, git-scm link). Exit 0 only if all hard requirements pass.
- Idempotency: pure read-only checks, zero side effects — trivially re-run safe.
- Design: `#Requires -Version 5.1`, `Set-StrictMode`, `$ErrorActionPreference = 'Stop'`; flag `-InstallHints` (default on) to print remediation commands.

#### 0b. `init-working-copy.ps1` — per working copy / per branch (run after every clone AND after every checkout/branch switch)
Prepares *this* checkout to match its committed state. Order:
1. **Preflight:** running from repo root (via `$PSScriptRoot` + marker check); `git` available; optionally invoke/assume `setup-dev-env.ps1` passed (flag `-SkipEnvCheck` to skip re-checking; default runs the env check first so the script is self-sufficient).
2. **Working-tree hygiene (branch-switch safety):** if the tree has uncommitted changes in tracked files → warn and abort (never destroy work); submodules are exempt from this check since we manage them.
3. **Submodule sync (the key function):**
   - No `.gitmodules` → skip (pre-first-commit state).
   - `git submodule sync --recursive` (remotes match superproject pins — matters after branch switches that change pins), then `git submodule update --init --recursive --force` (idempotent: already-correct submodules untouched; branch-specific pin changes applied; detached HEAD set to the committed SHA).
   - **Verify population** via sentinel files: `third_party/vst3sdk/pluginterfaces/vst/ivstcomponent.h`, `third_party/signalsmith-dsp/filters.h`, `third_party/catch2/CMakeLists.txt`, `third_party/dr_wav/dr_wav.h`. Any missing after update → print exact remediation command, exit non-zero (catches partial/failed clones).
4. **Build directory prep:** create `build/` if absent; **never delete** an existing build dir. Note: after a branch switch that changes CMake options, a stale cache is possible — detect by comparing a hash of top-level `CMakeLists.txt` stored in `build/.vdplg-cmake-hash` and *warn* (not auto-delete) suggesting `Remove-Item build -Recurse` or `-Reconfigure`.
5. **Optional configure** (flag `-Configure`, default off): `cmake -B build -G "Visual Studio 17 2022" -A x64 <repoRoot>` (CMake configure is idempotent).
6. **Summary:** current branch, `git submodule status` pins, tool versions, next-step hint ("open build in VS / run ctest").

Design constraints (both scripts): `#Requires -Version 5.1` (Windows PowerShell, no pwsh dependency); `Set-StrictMode`; `$ErrorActionPreference = 'Stop'`; all git calls check `$LASTEXITCODE`; network access only in the submodule fetch step when something is actually missing. README documents the workflow: *machine once → `.\setup-dev-env.ps1`; every clone/checkout → `.\init-working-copy.ps1`*.

### Phase 1 — Repo bootstrap & build system
1. Init git repo (if not already), add `.gitignore` (build/, .vs/, out/), flesh out `README.md`.
2. Add git submodules under `third_party/`, pinned to tags:
   ```bat
   git submodule add https://github.com/steinbergmedia/vst3sdk.git third_party/vst3sdk
   git submodule add https://github.com/signalsmith-audio/dsp.git third_party/signalsmith-dsp
   git submodule add https://github.com/catchorg/Catch2.git third_party/catch2
   git submodule add https://github.com/mackron/dr_wav.git third_party/dr_wav
   ```
   Then check out the pinned tags inside each (`vst3sdk` → `VST3 SDK 3.8.1`, `signalsmith-dsp` → `v1.7.0`, `catch2` → latest v3.x stable) and commit the resulting pinned SHAs in the superproject. Document in README: clone with `git clone --recurse-submodules` or `git submodule update --init --recursive` (required because vst3sdk itself contains nested submodules).
3. Top-level `CMakeLists.txt`: `cmake_minimum_required(>=3.21)`, `project(vdplg CXX)`, C++17, options (`VDPLG_BUILD_TESTS=ON`, `VDPLG_BUILD_TESTHOST=ON`), a **guard** that fails configure with a clear message if `third_party/*` are empty (submodules not initialized), then `add_subdirectory(third_party/vst3sdk)` (or its `public.sdk` entry point as required by the SDK's CMake layout), `add_subdirectory(third_party/signalsmith-dsp)`, `add_subdirectory(third_party/catch2)` (tests only), and our dirs: `common`, `plugins/passthrough`, `tests`, `testhost`.
4. Verify: `git submodule update --init --recursive && cmake -B build -G "Visual Studio 17 2022" -A x64 && cmake --build build --config Release` configures cleanly with no targets yet beyond common.

### Phase 2 — Common library
5. `common/` static lib **`common`**: WAV I/O wrapper (dr_wav), small DSP utilities (dB conversions, block helpers), analysis math shared by tests and testhost (RMS/peak/DC/FFT-magnitude via Signalsmith `fft::FFT`). Headers under `include/vdplg/`, namespace `vdplg`. Keep it dependency-light so both plugin tests and the host can link it.

### Phase 3 — First plugin (template for all future plugins)
6. `plugins/passthrough/`: port the VST3 SDK's **AudioEffectExample** structure (factory → `IComponent`/`IAudioProcessor` + `IEditController`) into our folder. Processor: stereo passthrough with 2 parameters (e.g., `Gain` in dB, `Mix` dry/wet) to exercise parameter plumbing, state save/load, and sample-accurate param changes. Factory metadata uses brand vendor string **"Vode Plugins"** and unique ClassID (UUID). Editor: minimal VSTGUI editor showing the two params (SDK ships VSTGUI4; keep it trivial — a textless editor is acceptable fallback if VSTGUI setup fights us).
7. Build target produces `passthrough.vst3` bundle (Windows: `.vst3` folder with Contents/x86_64-win). Use the SDK's CMake plugin macros (`SMTG_add_vst3_plugin` / `SMTG_add_vst3_helper_library`) from `public.sdk/source`.
8. Smoke check: load in any local VST3 host (Reaper) OR immediately via `vst3testhost` once Phase 5 lands.

### Phase 4 — L0/L1 unit tests (Catch2)
9. `tests/` exe `vdplg_tests` linking `common` + `Catch2WithMain`; `enable_testing()` + `add_test` at top level.
   - L0: Signalsmith biquad frequency response vs theory (±0.1 dB at peak), envelope follower steady-state gain, delay line round-trip.
   - L1: instantiate the passthrough processor class directly (link its sources into the test exe or a small static lib), feed synthetic buffers through `process()`, assert: unity-gain passthrough sample-exact, gain param applied correctly, silence-in → silence-out, no NaN.
10. Verify: `ctest --test-dir build -C Release` passes.

### Phase 5 — vst3testhost (L3 offline host)
11. Implement per INVESTIGATION §8.3.1 lifecycle: LoadLibrary the built `.vst3` binary → `GetPluginFactory()` → instantiate component → `initialize(hostContext)` → bus arrangement (stereo) → `setIoMode(kOfflineProcessing)` + `setupProcessing(ProcessSetup{44100, 512, kSample32, kOffline})` → `setActive/setProcessing(true)` → block loop filling `AudioBuffer32` from WAV → `process()` → collect output → teardown.
12. CLI per §8.3.2: `vst3testhost <plugin.vst3> --input x.wav [--blocksize N] --automation "param=value@time,..." --output out.wav --assert "..." [--compare-golden ref.wav --tolerance t] [--report json]`. Param names resolved to indices by enumerating `IParameter`s. Automation applied via `IParameterChanges::addPoint(index, normalized, sampleOffset)`.
13. Built-in analyzers (reuse `common`): peak, RMS, DC offset, FFT magnitude at given Hz, THD, golden compare (float32, tolerance-based primary gate).
14. Wire as CTest invocations over `testdata/cases/*.json`: passthrough unity-gain, gain automation mid-file, silence-in, clip-in sanity (no NaN), determinism (run twice, sample-exact).
15. Verify: full `ctest` run green; JSON report artifact produced.

## Dependencies between steps
- Phase 0 (both scripts) is written alongside Phase 1 (they reference the submodules and sentinels created in Phase 1); they block nothing else but must land before any "verify" step that assumes a ready working copy.
- Step 1–4 (Phase 1) block everything else.
- Phase 2 (common) blocks Phases 4 & 5 (both link it); Phase 3 is independent of Phase 2 (can run in parallel).
- Phase 5 step 15 needs Phase 3's built `.vst3` and Phase 2's analysis utils.
- Parallelizable: Phase 3 ∥ Phase 2; within Phase 5, wavio/analysis ∥ host lifecycle.

## Relevant files / references
- `INVESTIGATION.md` — §6.1 (VST3 SDK skeleton), §8.3 (testhost design, chosen L3), §7 (future osxcross path).
- VST3 SDK `public.sdk/source/pluginsamples/AudioEffectExample` — template for factory/processor/editor structure.
- VST3 SDK CMake macros (`SMTG_add_vst3_plugin` / `SMTG_add_vst3_helper_library`) — reuse instead of hand-rolling bundle packaging.
- Signalsmith headers: `include/signalsmith-dsp/filters.h` (Biquad), `fft.h` (FFT), `envelopes.h` — used by common lib + tests.
- Reference hosts for lifecycle edge cases: `Tracktion/pluginval` (init order), `iffyloop/EasyVst` (lightweight hosting glue) — read-only references, not dependencies.

## Verification
1. Fresh-clone simulation: `git clone --recurse-submodules <repo>` into a temp dir → run `.\init-working-copy.ps1` → exits 0, summary shows branch + submodule pins.
2. Idempotency: run `.\init-working-copy.ps1` again immediately → exits 0 fast, no fetches, no files modified, existing `build/` untouched.
3. Partial-state recovery: `git submodule deinit third_party/catch2` (or delete a sentinel file's dir) → `.\init-working-copy.ps1` detects, re-inits, exits 0.
4. Branch-switch scenario: create a temp branch that bumps one submodule pin → `git checkout <branch>` → `.\init-working-copy.ps1` syncs only the changed submodule, warns about stale CMake cache hash, exits 0.
5. `setup-dev-env.ps1` on this machine exits 0 with full summary; simulate a missing tool (e.g., temporarily hide `cmake` from PATH in a subshell) → non-zero exit with the exact install hint.
6. `cmake -B build -G "Visual Studio 17 2022" -A x64` (or `.\init-working-copy.ps1 -Configure`) configures with zero errors/warnings about missing deps.
7. `cmake --build build --config Release` produces `passthrough.vst3` and `vst3testhost.exe`.
8. `ctest --test-dir build -C Release` — all L0/L1 Catch2 tests + L3 testhost JSON cases pass.
9. Manual: load `passthrough.vst3` in Reaper (or any local VST3 host), confirm it appears, params move, audio passes through.
10. `vst3testhost passthrough.vst3 --input testdata/sine_440.wav --output out.wav --assert "peak_out <= 0dBFS"` exits 0 with JSON report.

## Decisions
- **Brand = "Vode Plugins"** (user request, replaces working name "vode-vst"): VST3 vendor string "Vode Plugins", README/about branding. **No "vode" in folder/variable prefixes** (user request): shared lib is just `common`; all internal prefixes use `VDPLG`/`vdplg_` — CMake project `vdplg`, options `VDPLG_BUILD_TESTS`/`VDPLG_BUILD_TESTHOST`, test exe `vdplg_tests`, headers `include/vdplg/`, namespace `vdplg`, hash file `.vdplg-cmake-hash`.
- **Raw VST3 SDK over JUCE/iPlug2** — user chose strictly all-MIT stack.
- **No UI investment**: minimal VSTGUI editor (or textless) — UI is out of scope for the skeleton; the point is the multi-plugin architecture.
- **Windows-only build now**; CMake stays toolchain-file-friendly for later osxcross arm64 (no WSL setup in this plan).
- **Git submodules** (pinned commits in `third_party/`) over FetchContent — user preference; explicit pins, no network at configure time.
- **Two-script bootstrap split** (user request): `setup-dev-env.ps1` = machine-level prerequisites (read-only checks, once per machine); `init-working-copy.ps1` = per-clone/per-branch state (submodule sync, build dir, optional configure). Both idempotent.
- **First plugin = "passthrough"** — deliberately trivial effect that still exercises params/state/automation; serves as the copy-paste template for real plugins.
- **Excluded from this plan**: GitHub Actions CI, osxcross/WSL setup, ChowDSP (add later if needed), AU/VST2/CLAP wrappers, code signing/notarization.

## Further considerations
1. vst3sdk contains **nested submodules** (base, cmake, pluginterfaces, public.sdk, vstgui4) — a plain `git clone` of our repo leaves them empty; README must mandate `--recurse-submodules`, and the CMake guard (Phase 1 step 3) should detect this and print the exact fix command.
2. Should `vst3testhost` also run against future plugins automatically (glob `plugins/*/*.vst3` + per-plugin `testdata`)? Recommend yes — add a CTest loop over discovered plugin bundles once a second plugin exists.
3. Plugin naming/IDs: each plugin needs a unique VST3 ClassID (UUID). Convention to adopt: generate one UUID per plugin at creation, store in its `PluginFactory.cpp`; document in README so new plugins don't collide.
4. Submodule update workflow: bumping a dep = `cd third_party/<dep> && git fetch --tags && git checkout <tag>` then commit the new pin in the superproject. Document in README.
