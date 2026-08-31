# VST3 Effect Plugin — Stack & Library Investigation

**Date:** 2026-08-31
**Goal:** Build an audio **effect** VST3 plugin on **Windows**, using **MIT-licensed** libraries for core DSP building blocks (compressor, EQ, LFO, etc.).

---

## TL;DR — Recommended Stack

| Layer | Choice | License | Why |
|-------|--------|---------|-----|
| **Plugin framework / host glue** | **JUCE** (with `juce_dsp` module) | JUCE Licence (free Starter tier) / AGPLv3 | De-facto standard; handles VST3 wrapper, UI, audio thread, params, state. |
| **Core DSP components** | **Signalsmith Audio DSP** | **MIT** ✅ | Header-only C++11; biquad/EQ, dynamics (compressor/limiter), envelopes, delay, FFT/spectral, curves, windows. Exactly the "basic components" requested. |
| **Alternative / complement DSP** | **ChowDSP** | **MIT** ✅ | Larger collection of ready-made effect modules (distortion, filters, modulators, LFOs). |
| **VST3 interface (if not using JUCE)** | **Steinberg VST3 SDK** | **MIT** ✅ | The official, now open-source VST3 spec + sample projects. |
| **Build system** | **CMake** | — | Used by both JUCE and the VST3 SDK. |
| **Compiler / IDE** | **MSVC via Visual Studio 2022** (x64) | — | Native Windows toolchain; best VST3 host compatibility. |
| **Test host** | **Reaper** or **Cockos** / any VST3 host | — | To load & audition the plugin during development. |

**Bottom line:** Use **JUCE** as the framework (it gives you a working VST3 plugin shell, GUI, and parameter/state management for free) and drop in **Signalsmith DSP** (MIT) for the actual compressor / EQ / LFO / envelope math. This keeps every *DSP* dependency MIT while JUCE's own license is satisfied by its free Starter tier.

---

## 1. Plugin Framework Options

### 1.1 JUCE — **recommended**
- The most widely used framework for audio apps/plugins across Windows, macOS, Linux, iOS, Android; supports VST2/VST3/AU/AUv3.
- Provides out-of-the-box:
  - VST3 wrapper (`juce_vst` module) — you get a valid VST3 plugin with minimal boilerplate.
  - A full GUI toolkit (draw your own UI, no external UI lib needed).
  - Audio thread / buffer management, parameter automation, state save/load, MIDI.
  - **`juce_dsp` module** — built-in `dsp::Compressor`, `dsp::Equalizer` (biquad-based), `dsp::IIR`, `dsp::FIR`, `dsp::FFT`, `dsp::Oversampling`. Convenient, but note it inherits JUCE's license (see §2).
- **License (important):** JUCE is **dual-licensed** under the **JUCE Licence** and **AGPLv3**.
  - **Starter (free):** identical features to paid tiers; capped at **$20,000/yr** revenue/funding. No watermark on this tier.
  - **Pro:** up to $300,000/yr (subscription).
  - **Perpetual:** one-time purchase (~$800 / ~$3500 tiers).
  - If you distribute **closed-source** software containing JUCE outside your org, you must be on a paid JUCE licence once you exceed the Starter revenue cap. For a hobby/free plugin or under $20k revenue, **Starter is free**.
- **Windows dev:** first-class. Build with Visual Studio + CMake via Projucer or CMake directly.

> **Note on "MIT only":** JUCE itself is *not* MIT. If a strict "everything must be MIT" policy is required, see the alternative in §1.3. For most people, "MIT for the DSP components" (the actual request) is fully satisfied by pairing JUCE + Signalsmith/ChowDSP.

### 1.2 HISE
- Node-graph based plugin framework (built on top of JUCE). Great for rapid prototyping of effects/synths.
- License: free for open-source; commercial closed-source requires a paid HISE licence.
- Good if you prefer a visual node editor over writing C++ glue.

### 1.3 Raw Steinberg VST3 SDK — **fully MIT, no framework**
- The official VST3 SDK is now **open source under MIT** (announced with VST 3.8 SDK, 2025).
- Gives you the interface definitions + sample plugin projects, buildable with **CMake + MSVC**.
- **Pros:** 100% MIT, no framework lock-in, no revenue caps, full control.
- **Cons:** You must implement *everything* yourself — UI (you'd pick a GUI lib), audio thread, parameter system, state, installer. Much more boilerplate than JUCE.
- **Choose this** if you want a pure-MIT stack and are comfortable owning the glue code.

---

## 2. MIT DSP Libraries (the core building blocks)

### 2.1 Signalsmith Audio DSP — **top pick** ✅
- **Repo:** `github.com/signalsmith-audio/dsp`
- **License:** **MIT**
- **Form factor:** C++11 **header-only** — just `#include` what you need, no build step.
- **What it provides (verified from its docs):**
  - **Filters / EQ:** `filters::Biquad` (peak, low/high shelf, etc.) with built-in smoothing → parametric EQ building block.
  - **Dynamics:** compressor/limiter-style dynamics processing (envelope followers + gain reduction).
  - **Envelopes:** `envelopes::BoxSum`, envelope followers, `curves::CubicSegmentCurve` (smooth attack/release curves).
  - **Delay:** `delay::Delay`, `delay::Buffer`, `delay::MultiDelay` (stereo), interpolators.
  - **Spectral / FFT:** `fft::FFT`, `spectral::WindowedFFT`, `windows::Kaiser`, etc.
  - **Rates / oversampling:** `rates::Oversampler2xFIR`.
  - **Mixing:** Hadamard transforms, etc.
- **LFO:** not a single named "LFO" class, but trivially built from an oscillator/curve + rate control; or use ChowDSP's modulators (below).
- **Why it fits:** It is precisely a library of "basic components" (biquad/EQ, dynamics, envelopes, delay, spectral) that you compose into your own effect chain. Header-only = zero integration friction with JUCE or raw VST3.

### 2.2 ChowDSP — **strong complement** ✅
- **Repo:** `github.com/Chowdhury-DSP/chowdsp`
- **License:** **MIT**
- **Form factor:** header-only C++ (also ships as JUCE modules).
- **What it provides:** a large catalog of ready-made effect & synth modules — filters (many topologies), distortion/waveshapers, **modulators/LFOs**, envelopes, delays, reverbs, resonators, SIMD-accelerated buffers.
- **Why it fits:** If you want more turnkey effect blocks (especially LFOs/modulation and waveshaping) alongside Signalsmith's cleaner signal-processing primitives.

### 2.3 Other candidates considered
| Library | License | Notes |
|---------|---------|-------|
| **KFR** | **GPLv2+ or commercial** ❌ | Excellent modern C++ DSP (FFT, FIR/IIR/biquad, resampling), but **not MIT** — excluded by the requirement. |
| **FFTW** | GPL / commercial ❌ | Fast FFT, but not MIT. |
| **muFFT** | MIT ✅ | Fast FFT only; useful if you need spectral processing and want to avoid Signalsmith's FFT. |
| **liquid-dsp** | LGPL-ish (C) ⚠️ | SDR-oriented C library; overkill and wrong language fit for a VST3 effect. |
| **JUCE `juce_dsp`** | JUCE licence ⚠️ | Convenient (Compressor/EQ/FFT built in) but inherits JUCE's license rather than MIT. Fine under Starter tier. |

> **Recommendation:** Use **Signalsmith DSP** as the primary MIT DSP layer. Add **ChowDSP** if you want more ready-made modulation/LFO and waveshaping blocks. Avoid KFR/FFTW due to non-MIT licensing.

---

## 3. Windows Development Toolchain

- **IDE / Compiler:** **Visual Studio 2022** (Community edition is free) with the **"Desktop development with C++"** workload → MSVC x64. This is the reference toolchain for VST3 on Windows.
- **Build system:** **CMake** (both JUCE and the VST3 SDK are CMake-based).
  - JUCE: generate projects via **Projucer** (GUI) or plain CMake (`juce_add_plugin`).
  - Raw VST3 SDK: `cmake -G "Visual Studio 17 2022" -A x64 ..\vst3sdk` then build with MSBuild/VS.
- **C++ standard:** C++17 (JUCE 8 default); Signalsmith is C++11 so it's compatible.
- **Test host:** install a VST3-capable DAW (e.g., **Reaper**, **Cockos**, or Steinberg's own hosts) to load the plugin from its build output folder during development.
- **Optional:** Git + a `.gitmodules`/submodule or CMake `FetchContent` to pull Signalsmith/ChowDSP header-only sources into the repo.

### Suggested project layout (JUCE route)
```
MyEffectPlugin/
├─ CMakeLists.txt            # juce_add_plugin(...)
├─ JucePluginDefines.h
├─ Source/
│  ├─ PluginProcessor.cpp    # audio thread: wire Signalsmith DSP chain
│  ├─ PluginEditor.cpp       # GUI
│  └─ dsp/                   # vendored MIT headers
│     ├─ signalsmith/dsp/... # (MIT)
│     └─ chowdsp/...         # (MIT, optional)
└─ LICENSES/                 # keep MIT notices for vendored libs
```

---

## 4. Licensing Compatibility Summary

| Component | License | Compatible with closed-source commercial? |
|-----------|---------|------------------------------------------|
| Signalsmith DSP | MIT ✅ | Yes — retain copyright notice. |
| ChowDSP | MIT ✅ | Yes — retain copyright notice. |
| muFFT | MIT ✅ | Yes. |
| Steinberg VST3 SDK | MIT ✅ | Yes. |
| JUCE | JUCE Licence / AGPLv3 ⚠️ | Free under **Starter** (≤ $20k/yr revenue). Paid tiers above that. Not MIT. |
| KFR | GPLv2+ / commercial ❌ | No (unless you buy a commercial licence). |
| FFTW | GPL / commercial ❌ | No (unless commercial licence). |

**Practical guidance:**
- Keep all *DSP* code MIT (Signalsmith/ChowDSP) → satisfies the stated requirement and keeps your core IP clean.
- If you use JUCE, track your revenue against the **$20k Starter cap**; upgrade to Pro/Perpetual only if you exceed it.
- If you need a **100% MIT** stack (no JUCE), go **raw VST3 SDK (MIT) + Signalsmith/ChowDSP (MIT)** and supply your own UI/audio-thread glue.

---

## 5. Final Recommendation

**Primary (fastest, most common):**
> **JUCE (Starter, free) + Signalsmith DSP (MIT) [+ ChowDSP (MIT)]**, built with **CMake + MSVC (Visual Studio 2022, x64)**, tested in **Reaper**.

**Alternative (strictly all-MIT, more work):**
> **Steinberg VST3 SDK (MIT) + Signalsmith DSP (MIT) + ChowDSP (MIT)**, CMake + MSVC, with your own UI and audio-thread implementation.

Both routes give you MIT-licensed compressor/EQ/LFO/envelope building blocks on Windows; they differ only in how much plugin "plumbing" the framework does for you.

---

## 6. Non-JUCE Ways to Get a VST3 Skeleton (Part 2 Investigation)

Question: *Is there an open-source / MIT project that can serve as a ready-made template/skeleton for a VST3 plugin, without JUCE?*

**Short answer:** Yes — several. The two best fits are **(a) the official Steinberg VST3 SDK sample projects** (MIT, but minimal — no UI) and **(b) iPlug2** (permissive zlib-style license, full framework + example projects, very close to "MIT-like"). Below is the full landscape.

### 6.1 Option A — Steinberg VST3 SDK sample projects (pure MIT) ✅
- **Repo:** `github.com/steinbergmedia/vst3sdk` — **MIT licensed**.
- The SDK ships **ready-to-build example plug-in projects** (e.g. an audio-effect plugin skeleton with parameters, editor stub, and the full VST3 class hierarchy) plus CMake files. You clone it, run CMake, and you have a compiling VST3 effect skeleton to modify.
- **Windows build (verified from official docs):**
  ```bat
  cd vst3sdk
  mkdir build && cd build
  cmake -G "Visual Studio 17 2022" -A x64 ..\vst3sdk
  ```
  Then open the generated `.sln` in Visual Studio and build.
- **Pros:** 100% MIT, official/canonical, no revenue caps, no corporate lock-in. Pairs perfectly with Signalsmith/ChowDSP (both MIT) for a fully permissive stack.
- **Cons:** The samples are **minimal** — you get the plugin/audio-thread/parameter plumbing but essentially **no GUI toolkit**. You must supply your own UI (draw with a lib of your choice, or keep a textless/parameter-only editor). More boilerplate than JUCE/iPlug.
- **Best for:** a strict all-MIT stack where you're happy to own the UI layer.

### 6.2 Option B — iPlug2 (permissive, near-MIT) ⭐ recommended non-JUCE
- **Repo:** `github.com/iPlug2/iPlug2`
- **License:** a **liberal zlib-style license** — "free to use in closed source projects, free from corporate interference," explicitly permits commercial use. It is **not literally MIT**, but it is equally permissive (no copyleft, no revenue cap, no watermark). If "MIT-like / permissive" is acceptable, this is the strongest non-JUCE option.
- **What you get:** a complete cross-platform C++ plugin framework (VST3, VST2, AU, AAX, CLAP, Web) with:
  - **Example/template projects** you can copy as a starting skeleton (effect + synth examples).
  - Built-in **UI system** (`IControl`, custom-draw controls), parameter/automation handling, state save/load, audio-thread management.
  - Ships as header-only-ish modules; integrates with CMake.
- **Windows dev:** first-class with Visual Studio + CMake; large community (forum + Discord).
- **Pros:** Full framework *and* permissive license *and* ready example skeletons → closest thing to "a template I can fork." Much less boilerplate than raw VST3 SDK because UI is included.
- **Cons:** Smaller ecosystem/docs than JUCE; license is zlib-style rather than MIT (usually a non-issue).
- **Note:** iPlug3 is now in development (the org has moved toward an `iplug3` repo); iPlug2 remains the stable, widely-used choice.

### 6.3 Other options considered
| Option | Language | License | Verdict |
|--------|----------|---------|---------|
| **DISTRHO Plugin Framework (DPF)** | C++ | **GPLv3** ❌ | Great, mature, but GPL — conflicts with a permissive/MIT goal. |
| **HISE** | C++ (node-graph, on JUCE) | Free OSS / paid commercial ⚠️ | Good visual toolkit, but built on JUCE and commercial closed-source needs a paid licence. |
| **nih-plug** | Rust | Framework **ISC**; but its **VST3 bindings are GPLv3** ❌ | Nice Rust framework, but the VST3 export path pulls in GPLv3 bindings — not MIT-clean. |
| **pluggy / other Rust VST3** | Rust | varies | Ecosystem still maturing; licensing varies, several depend on GPL VST3 bindings. |
| **vst3go** | Go | check repo ⚠️ | Minimal-boilerplate Go VST3 framework; younger/smaller — verify license & maturity before committing. |
| **EasyVst** | C++ | check repo ⚠️ | A lightweight VST3 *hosting* wrapper (for hosting, not necessarily authoring effects) — not a plugin-authoring template. |

> **Rust note:** If you'd consider Rust, the VST3 *interface bindings* used by most crates are commonly **GPLv3**, which breaks the MIT goal. C++ (VST3 SDK or iPlug2) is the cleaner path for a permissive stack.

### 6.4 Recommendation (non-JUCE)
- **Want a full framework + UI + example skeleton, permissively licensed, minimal fuss →** use **iPlug2**. Fork one of its example effect projects as your skeleton, then drop in **Signalsmith DSP (MIT)** / **ChowDSP (MIT)** for compressor/EQ/LFO. Build with **CMake + MSVC (VS 2022, x64)**.
- **Want strictly 100% MIT and don't mind writing your own UI →** use the **Steinberg VST3 SDK sample project** as the skeleton + Signalsmith/ChowDSP.

Both keep every DSP dependency MIT; they differ in how much "plumbing + UI" the starting point provides.

---

## 7. Two-System Build from Windows Only: WSL + osxcross (ARM Mac target)

Goal: produce **both** a Windows `.vst3` (MSVC) **and** an Apple-Silicon (arm64) macOS `.vst3`, while developing **only on a Windows machine**, using **WSL + osxcross** for the macOS side.

### 7.1 How it fits together
- **Windows side:** build natively with **MSVC / Visual Studio 2022 (x64)** → `MyEffect.vst3` for Windows.
- **macOS side:** inside **WSL (Ubuntu)**, use **osxcross** to cross-compile with clang against a **packaged macOS SDK**, targeting **`arm64-apple-darwin`** → `MyEffect.vst3` for Apple Silicon.
- One codebase, two toolchains. The framework (JUCE / iPlug2 / raw VST3 SDK) is unchanged; only the compiler + SDK differ.

### 7.2 The one hard requirement: the macOS SDK (legal note)
- osxcross does **not** ship the macOS SDK. You must supply a **`MacOSX*.sdk.tar.xz`** in osxcross's `tarballs/` folder.
- That SDK is legally distributed **only via Xcode / Command Line Tools under Apple's license**. The clean way to obtain it is to run osxcross's `gen_sdk_package*` script **on a Mac** (or from a downloaded Xcode `.xip`) and copy the resulting archive into WSL.
- ⚠️ **Gray area:** extracting/caching the SDK and building on non-Apple hardware works technically but sits in a gray zone of Apple's EULA. Acceptable for personal/hobby use; for commercial distribution prefer building the macOS target on an official macOS CI runner (see §7.6).

### 7.3 Step-by-step setup (WSL2 + Ubuntu)

**0) Enable WSL2 (PowerShell, as Administrator):**
```powershell
wsl --install -d Ubuntu-24.04
```

**1) Inside WSL — install osxcross build dependencies:**
```bash
sudo apt-get update
# LLVM flavor (recommended for new projects; supports arm64):
sudo apt-get install bash clang llvm lld git make patch sed tar gzip xz-utils bzip2 cpio
# (For the default 'stable' flavor instead, also add: cmake python3 libxml2-dev libssl-dev zlib1g-dev liblzma-dev libbz2-dev)
# Or let osxcross install everything for you:
sudo tools/get_dependencies.sh   # after cloning, see step 3
```

**2) Get the macOS SDK into `tarballs/`:**
- Easiest: on any Mac, run `./tools/gen_sdk_package.sh` (full Xcode) or `./tools/gen_sdk_package_tools.sh` (Command Line Tools), then copy the produced `*.tar.xz` into `~/osxcross/tarballs/` inside WSL.
- No Mac handy: download Xcode `.xip` and extract the SDK on Linux with `./tools/gen_sdk_package_pbzx.sh <xcode>.xip` (needs ~45 GB free space; install `clang make libssl-dev liblzma-dev libxml2-dev`).

**3) Clone and build osxcross (LLVM flavor, arm64):**
```bash
git clone https://github.com/tpoechtrager/osxcross.git
cd osxcross
# (SDK already in ./tarballs/)
UNATTENDED=1 BUILD_FLAVOR=llvm ENABLE_ARCHS="arm64" OSX_VERSION_MIN=13.0 ./build.sh
```
- `BUILD_FLAVOR=llvm` is the easiest to build and explicitly supports **arm64**.
- `OSX_VERSION_MIN` sets the deployment target (e.g. `13.0` for Ventura+).
- Toolchain installs to `./target` by default.

**4) Add it to PATH:**
```bash
echo 'export PATH=$HOME/osxcross/target/bin:$PATH' >> ~/.bashrc
source ~/.bashrc
```

### 7.4 Verifying the toolchain
```bash
# Should print the arm64 darwin target triple / version:
osxcross-conf
# Quick smoke test:
echo 'int main(){return 0;}' > t.c
arm64-apple-darwin13-clang t.c -o t && file t
# expect: Mach-O 64-bit executable arm64
```

### 7.5 Building your VST3 for ARM Mac via CMake
osxcross ships a **CMake toolchain file** (`target/cmake/OSXCross-arm64.cmake`, path varies by flavor/arch). Point CMake at it:
```bash
# From your plugin repo root (JUCE / iPlug2 / VST3 SDK all use CMake):
cmake -B build-mac \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/osxcross/target/cmake/OSXCross-arm64.cmake \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac -j
```
- The output `.vst3` bundle lands under `build-mac/...`. Copy it to a Mac (or a DAW on Apple Silicon) to test — **you cannot run a macOS .vst3 inside WSL.**
- For a **universal** (Intel + Apple Silicon) binary, build twice (arm64 + x86_64) and `lipo` them together; osxcross's CMake docs cover this.

### 7.6 Practical caveats & tips
- **You compile blind:** no way to load/test the macOS plugin on Windows. Keep a lightweight test path (unit tests for DSP run fine in WSL; full plugin audition needs a Mac or a macOS CI runner).
- **Signing/notarization:** a distributable macOS plugin should be code-signed + notarized, which requires Apple credentials and is easiest on a real Mac or macOS CI. Unsigned builds still load locally if the user allows it.
- **Performance:** first osxcross build (especially non-llvm flavors) can take a while; the `llvm` flavor is the fastest to set up.
- **Fallback / hybrid:** many teams keep WSL+osxcross for quick local iteration but do the **final signed macOS build in GitHub Actions on a `macos-latest` (ARM) runner** to stay fully within Apple's licensing. You can have both.
- **WSL file-system note:** build inside the Linux filesystem (`~/...`), not `/mnt/c/...`, for far better I/O performance.

### 7.7 Verdict
- **Yes — you can build a two-system (Windows + ARM Mac) VST3 using only a Windows PC**, via native MSVC for Windows and **WSL + osxcross (llvm flavor, arm64)** for macOS.
- The main constraints are (a) sourcing the macOS SDK (Apple EULA gray area) and (b) the inability to run/test the macOS binary locally. For anything you ship commercially, mirror the macOS build on an official macOS CI runner.

---

## 8. Testing Strategy for the VST3 Effect Plugin

Requirement: a **simple host** that (1) loads/runs the plugin, (2) feeds it test audio files, (3) captures the output, (4) can **automate plugin parameters during the run**, and (5) passes the result to an **analyzer** that asserts expected properties.

### 8.1 Layered testing model (recommended)
Don't put everything in one layer — split by what each layer is good at:

| Layer | What it tests | Tooling | Runs in CI? |
|-------|---------------|---------|-------------|
| **L0 — DSP unit tests** | Individual components (biquad response, compressor gain curve, LFO waveform) with synthetic signals | **Catch2 / GoogleTest** + Signalsmith/ChowDSP directly (no plugin shell needed) | ✅ anywhere, fast |
| **L1 — Processor-level tests** | `AudioProcessor::processBlock` with generated buffers, parameter changes mid-stream, edge cases (silence, clipping, block-size changes) | Catch2 + your processor class instantiated directly | ✅ anywhere, fast |
| **L2 — Plugin validation** | VST3 API conformance: instantiation, param ranges, state save/load round-trip, no crashes under random param sweeps | **pluginval** (Tracktion) | ✅ headless (`--run-tests`) |
| **L3 — Host integration / golden-audio** | Full end-to-end: real `.vst3` loaded in a host, fed WAV files, params automated over time, rendered output compared to a **gold reference** or analyzed | **REAPER + ReaScript** (or a custom mini-host) | ✅ (REAPER has a free license; headless render works) |

The layers you described ("host runs plugin, feeds files, automates, analyzes") are **L3**. L0–L2 catch most bugs far cheaper and should exist alongside it.

### 8.2 Option A — REAPER as the test host (pragmatic, proven) ⭐
REAPER is scriptable (ReaScript: Lua/Python/EEL), renders **headlessly from the command line**, and has a free license — making it a de-facto standard for this exact workflow. A documented, working pattern exists in the wild (JUCE forum, "Automated testing with REAPER"):
- Keep a **`.RPP` project per test case** in the repo: input WAVs on a track, the plugin (VST3) inserted, **parameter automation envelopes** baked into the project (this satisfies "automate the plugin during running").
- A shell script drives: `reaper --playproject <proj>.rpp` (or render via ReaScript `renderProjects`), producing an output WAV.
- **Gold-reference comparison:** first run with `--create-gold-reference` stores the expected WAV; later runs compare new render vs. reference (e.g., `sox` difference / sample-exact or tolerance-based compare) and fail on mismatch.
- **Analyzer step:** instead of (or in addition to) gold comparison, run the rendered WAV through an analyzer:
  - **sox / ffmpeg** — loudness (RMS/peak), silence detection, basic stats.
  - **Essentia** (C++/Python) — spectral centroid, RMS dynamics, etc. (note: AGPLv3 — fine as an external CLI tool, don't link into your plugin).
  - **Vamp plugins** (e.g., loudness, spectral) via `vamp-simple-cli` — purpose-built audio analysis.
  - **Python (librosa/soundfile/scipy)** — fully custom assertions (e.g., "output THD < X%", "gain reduction between Y and Z dB", "EQ band at F Hz shows ±N dB").
- **Windows note:** REAPER runs natively on Windows; the same `.RPP`-driven flow works there. For the ARM-Mac build, run the same suite on a macOS CI runner.
- **Pros:** real VST3 loading path (catches API/state bugs unit tests miss), zero custom host code, automation via project files. **Cons:** REAPER is a GUI app (headless render is supported but it's not a "pure" CLI tool); golden WAVs are sensitive to sample-rate/block-size — fix those in the render settings.

### 8.3 Option B — Custom minimal headless VST3 host (full control)
### 8.3 Option B — Custom minimal headless VST3 host (full control) ⭐ **CHOSEN**
A lean, dependency-light, CI-native tester: a small **offline VST3 host** executable (~500–1000 lines of C++) built against the **MIT-licensed VST3 SDK**. No audio device, no GUI, no DAW.

#### 8.3.1 VST3 host lifecycle (what the code must do, in order)
1. **Load the module:** `LoadLibrary`/`dlopen` the `.vst3` bundle binary → call the exported `GetPluginFactory()` → `IPluginFactory`.
2. **Enumerate & instantiate:** `countAvailableComponents()` / `getComponentInfo(i)` → pick the component → `createInstance(Steinberg::Vst::IComponent::iid)`.
3. **Query capabilities:** `queryInterface` for `IAudioProcessor`, `IParameterChanges` (params), `IMidiController` (if needed), `IEditController` (optional — only if you want UI-level access; not required for offline audio tests).
4. **Configure I/O:** `IComponent::initialize(hostContext)` with a minimal `FUnknown` host context; set bus arrangements via `AudioProcessor::setActiveBusArrangement` (typically stereo in/out, bus index 0).
5. **Declare offline mode:** `IComponent::setIoMode(Vst::kOfflineProcessing)` **and** `IAudioProcessor::setupProcessing(ProcessSetup{ sampleRate, maxSamplesPerBlock, kSample32, processMode = kOffline })`. (Both are part of the documented offline contract; plugins may check either.)
6. **Activate:** `setActive(true)` → `IAudioProcessor::setProcessing(true)`.
7. **Pump audio:** loop over input WAV samples in blocks (e.g., 512 or 1024 frames): fill `AudioBuffer32`, build a `ProcessData` (with `processMode = kOffline`, correct `symbolicSampleSize`, `processContext` timestamps if the plugin uses them), call `process()`, copy output buffers.
8. **Parameter automation during the run:** at scheduled sample offsets, apply changes via `IParameterChanges` (`addPoint(paramIndex, normalizedValue, sampleOffset)`) passed into `process()` — this is the *correct* VST3 way to automate params mid-stream (better than calling `setParamNormalized` directly, since it goes through the plugin's own parameter-change path and respects sample-accurate offsets).
9. **Teardown:** `setProcessing(false)` → `setActive(false)` → release interfaces.

#### 8.3.2 Proposed CLI & test-case format
```
vst3testhost MyEffect.vst3 \
    --input tests/sine_440.wav \
    --blocksize 512 \
    --automation "threshold=-12dB@0s, threshold=-24dB@2s, attack=10ms@1s" \
    --output out.wav \
    --assert "peak_out <= 0dBFS" \
    --assert "rms(out) < rms(in)" \
    --assert "spectral_peak(440Hz) within 1dB" \
    [--compare-golden reference.wav --tolerance 1e-6] \
    [--report json]
```
- **Test cases as data files** (JSON/YAML) checked into the repo: `{ input, sampleRate, blockSize, paramAutomation[], assertions[] }` — one file per scenario (bypass, unity-gain passthrough, heavy compression, EQ boost, LFO modulation on/off, silence-in, clip-in, block-size stress).
- Param names map to indices by reading the plugin's `IParameter` list at startup (name → index), so test files stay readable.

#### 8.3.3 Built-in analyzers (C++, no external deps needed)
- **Levels:** peak, RMS (whole-file and per-window), DC offset, true-peak approximation.
- **Spectral:** FFT (reuse **Signalsmith `fft::FFT`** — MIT, header-only) → magnitude at specified frequencies, spectral centroid, THD (harmonic sum vs. fundamental).
- **Dynamics:** gain-reduction trace — either read the plugin's exposed metering parameters (if it has them) or compute `20*log10(rms_in_window / rms_out_window)` for a known steady-state signal.
- **Determinism/golden:** sample-exact or tolerance-based comparison against a committed reference WAV (re-encode both to float32 first; fix sample rate + block size in the test config to keep goldens stable).
- **Sanity:** no NaN/Inf, output length == input length (+ tail if `getTailSamples()` reports one), latency consistency via `IAudioProcessor::getLatencySamples()`.

#### 8.3.4 Starting points & code sources
- **VST3 SDK example projects** (`github.com/steinbergmedia/vst3sdk`, MIT): the SDK ships example plug-ins *and* host-side infrastructure under its CMake tree; the host-side classes you need (`pluginterfaces/vst/ivstcomponent.h`, `ivstaudioprocessor.h`, `ivstparameterchanges.h`) are headers — your host is mostly glue code around them.
- **EasyVst** (`github.com/iffyloop/EasyVst`): lightweight C++ wrapper for hosting VST3 without JUCE — good reference for the load/instantiate/process plumbing.
- **pluginval** (`github.com/Tracktion/pluginval`): open-source host — study its `PluginBase`/host code for correct initialization order and edge-case handling (it exists precisely because VST3 host-side details are fiddly).
- **WAV I/O:** trivial with miniaudio (MIT, single-header) or dr_wav (public domain) — avoid pulling in a big audio lib just for file I/O.

#### 8.3.5 Build & CI integration
- CMake target `vst3testhost` linking the VST3 SDK (same SDK you build the plugin against); builds with MSVC on Windows and with osxcross/clang for macOS (arm64) — same source, both targets from your Windows machine (§7).
- Wire into **CTest**: each JSON test case becomes a CTest invocation; assertions produce pass/fail + a JSON report for CI artifacts.
- Run the suite in GitHub Actions: Windows runner → Windows `.vst3`; macOS runner → ARM `.vst3` (also solves local-testing of the Mac build).

#### 8.3.6 Effort estimate & risks
- **Effort:** ~1–2 weeks part-time for a solid v1 (load/instantiate/process/automation/WAV I/O/assertions), most of it fiddly VST3 interface plumbing rather than hard problems.
- **Risks:** (a) plugins that assume realtime-only behavior (rare for effects; offline mode is a documented contract); (b) plugins using `IEditController`-only state paths (mitigate by driving everything through `IParameterChanges`); (c) golden-file brittleness across platforms/compilers — mitigate with tolerance-based asserts as the primary gate and goldens as a secondary determinism check.
- **Pros:** fully scriptable, fast, no DAW dependency, trivially integrated into CTest/CI, works identically on Windows & (cross-built) macOS, stays 100% MIT/permissive (SDK + Signalsmith + miniaudio).

### 8.4 Option C — pluginval (API conformance, not audio assertions)
- **`Tracktion/pluginval`** — open-source, cross-platform plugin validator/tester. Loads your plugin and runs automated checks: instantiation, parameter enumeration/ranges, state save/load round-trips, random parameter sweeps, crash detection. JUCE-style test API for adding your own checks.
- It does **not** do golden-audio comparison or signal analysis — treat it as the **L2 gate** (stability + API correctness), complementary to L3.
- Headless mode exists for CI. Note: development has slowed (last major activity ~2018–2021), but it remains the standard reference tool and is bundled in templates like **pamplejuce**.

### 8.5 Recommended concrete setup (for this project)
1. **L0/L1:** Catch2 tests compiled alongside the plugin (CMake `enable_testing()` + CTest). Test Signalsmith components and your processor with synthetic signals; assert numeric properties (e.g., biquad peak within ±0.1 dB of theory; compressor output RMS below input for a known hot signal).
2. **L2:** run **pluginval** headless in CI against the built `.vst3`.
3. **L3 (the "host" you asked for):** **Option B — custom offline VST3 host** (chosen). Build `vst3testhost` per §8.3: loads the `.vst3`, feeds WAV inputs, automates parameters sample-accurately via `IParameterChanges`, captures output, and runs built-in analyzers/assertions. Test cases live as JSON data files in the repo; everything is driven through CTest. Keep **Option A (REAPER)** as an occasional manual sanity-check tool, not part of CI.
4. **Analyzer assertions to use for an effect plugin:** peak/RMS levels, gain-reduction behavior, spectral shape at key frequencies, THD for distortion-type stages, DC offset ≈ 0, no clipping, determinism (same input+params ⇒ identical output, sample-exact).
5. **CI wiring:** GitHub Actions matrix — Windows runner builds + runs L0–L3 for the Windows `.vst3`; macOS runner does the same for the ARM build (this also solves the "can't test macOS binary locally" problem from §7).

### 8.6 Verdict
- **Chosen L3 tool:** the custom **offline VST3 host** (`vst3testhost`, §8.3) — meets all five requirements (load plugin ✅, feed test files ✅, capture output ✅, automate params during run via `IParameterChanges` ✅, analyze vs. expectations ✅), is fully scriptable, CI-native (CTest + GitHub Actions matrix for Win/macOS), and stays 100% MIT/permissive.
- **Supporting layers:** Catch2 unit tests (L0/L1) and pluginval headless validation (L2) as lower gates; REAPER kept only as an optional manual sanity-check tool, not part of CI.

---

## Sources
- JUCE home & licensing: https://juce.com/ , https://juce.com/get-juce/ , https://juce.com/legal/juce-8-licence/
- JUCE 8 tiers (Starter ≤ $20k, Pro ≤ $300k): https://forum.juce.com/t/juce-8-is-available-now/61809
- Signalsmith Audio DSP (MIT, header-only): https://github.com/signalsmith-audio/dsp , https://signalsmith-audio.co.uk/code/dsp/
- ChowDSP (MIT): https://github.com/Chowdhury-DSP/chowdsp
- Steinberg VST3 SDK (now MIT): https://www.steinberg.net/developers/vstsdk/ , https://github.com/steinbergmedia/vst3sdk
- VST3 SDK CMake/Windows build: https://steinbergmedia.github.io/vst3_dev_portal/pages/Tutorials/Using+cmake+for+building+plug-ins.html
- KFR licensing (GPLv2+/commercial): https://community.vcvrack.com/t/complete-list-of-native-fft-libraries-for-audio/9153
- iPlug2 (permissive zlib-style license, example projects): https://github.com/iPlug2/iPlug2 , https://github.com/iPlug2/iPlug2/blob/master/LICENSE.txt
- iPlug3 (in development): https://github.com/iplug3
- DISTRHO Plugin Framework (GPLv3): https://github.com/DISTRHO/DPF
- nih-plug (Rust; ISC framework, GPLv3 VST3 bindings): https://github.com/robbert-vdh/nih-plug
- vst3go (Go VST3 framework): https://github.com/justyntemme/vst3go
- EasyVst (lightweight VST3 hosting wrapper): https://github.com/iffyloop/EasyVst
- osxcross (macOS cross-toolchain for Linux/\*BSD, runs in WSL): https://github.com/tpoechtrager/osxcross
- osxcross SDK packaging (Xcode EULA note): https://github.com/tpoechtrager/osxcross/blob/master/README.SDK.md
- osxcross build dependencies: https://github.com/tpoechtrager/osxcross/blob/master/README.BUILD-DEPENDENCIES.md
- osxcross CMake toolchain usage: https://github.com/tpoechtrager/osxcross/blob/master/README.CMAKE.md
- pluginval (Tracktion, cross-platform plugin validator/tester): https://github.com/Tracktion/pluginval
- Automated REAPER plugin testing (gold-reference pattern): https://forum.juce.com/t/automated-testing-with-reaper-on-macos/65905
- pamplejuce (JUCE template with Catch2 + pluginval + CI): https://github.com/sudara/pamplejuce
