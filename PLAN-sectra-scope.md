# Plan: Sectra Scope — VST3 Spectrum Analyzer Plugin

**Date:** 2026-08-31
**Status:** Phase 3 (Implement DSP / Green) COMPLETE — all 28 `[l0][sectra]` cases pass, L1 + pre-existing suites pass, ctest 7/7 green. Awaiting user go-ahead for Phase 4 (UI).
**Brand:** Vode Plugins (vendor string "Vode Plugins", per repo naming rules)

## Goal

A new VST3 plugin **Sectra Scope**: a real-time spectrum analyzer with:

1. **Non-linear (log) frequency axis** — each octave occupies the same horizontal width.
2. **Decibel vertical grid** — dBFS scale with labeled horizontal grid lines.
3. **User-selectable FFT size** (parameter).
4. **User-selectable window type** (parameter).
5. **Two stacked scopes** with a channel-mode parameter: **L/R**, **Mid/Side**, **Mid/(Mid−Side)** (per-spectrum-line subtraction).
6. **Meter ballistics knobs** (parameters): **Attack** **0 … 20 ms** and **Release** **1 ms … 1 s**, both defined as **24 dB travel time** (time for the held level to move 24 dB), editable (drag + numeric entry).
7. **dB reference switch** (parameter): **Raw** (full-scale sample = 0 dB) vs **Normalized** (full-scale sine = 0 dB, window coherent-gain compensated).

**Mid/(Mid−Side) mode detail (user decision):** scope 2 is a **zero-referenced balance view**: its middle horizontal line = mid and side equal (0 dB); mid-dominant draws **above** the middle line, side-dominant **below**; vertical scale fixed at **±12 dB** (not the −120…0 dBFS grid). The difference is computed **after** the per-column attack/release smoothing of the mid and side spectra, so ballistics apply automatically. Values beyond ±12 dB clamp flat at the limit with a subtle indicator (brighter edge). Scope 1 shows the **mid spectrum on the normal −120…0 dBFS grid**.

Follows existing repo conventions: raw VST3 SDK 3.8.1, signalsmith-dsp v1.7.0, Catch2 L0/L1 tests, `vst3testhost` L3, C++17, MSVC x64, all-MIT deps.

---

## Assumptions (state explicitly — flag any you disagree with)

| # | Assumption | Default |
|---|-----------|---------|
| A1 | UI framework | **VSTGUI 4** (SDK-bundled) with a custom `CView`. Requires flipping `SMTG_ENABLE_VSTGUI_SUPPORT` ON at top level. Alternative: textless (no scope visible) — rejected as it defeats the purpose. |
| A2 | Frequency range | **20 Hz – 20 kHz** (~9.66 octaves), fixed. No pan/zoom in v1. |
| A3 | dB range | **−120 dBFS … 0 dBFS**, grid line every **6 dB**, labels every 12 dB. |
| A4 | FFT sizes | 1024 / 2048 / 4096 / 8192 / 16384 (5 discrete points). Default **4096**. |
| A5 | Window types | Rectangular, Hann, Hamming, Blackman, Blackman-Harris (5 points). Default **Blackman-Harris**. |
| A6 | Channel modes (user decision) | Two scopes, mode param: **L/R** (scope1=L, scope2=R), **M/S** (M=(L+R)/2, S=(L−R)/2), **M/(M−S)** (scope1=mid spectrum on normal dBFS grid; scope2=balance view, see A6b). Default **L/R**. |
| A6b | M/(M−S) balance view (user decision) | Scope 2: zero-referenced, middle line = 0 dB (M == S), up = mid dominant, down = side dominant, scale **±12 dB** fixed. Difference taken **after** per-column smoothing of M and S (so attack/release apply automatically). Clamp at ±12 dB + brighter-edge indicator. Scope 1 = mid spectrum, normal dBFS grid. |
| A7 | Ballistics (user decision) | Per-column envelope follower in dB domain: rise limited to **24 dB / T_attack** (T_attack ∈ [0, 20 ms], 0 = instant), fall limited to **24 dB / T_release** (T_release ∈ [1 ms, 1 s]). Defaults: attack **0 ms**, release **100 ms**. Both editable knobs. |
| A8 | dB reference (user decision) | Param switch: **Raw** — unscaled magnitude, full-scale sample = 0 dBFS (full-scale sine ≈ −3 dB for rectangular); **Normalized** — magnitude scaled by `2/Σwindow` (coherent gain), so a full-scale sine reads **0 dB** for *any* window. Default **Normalized**. |
| A9 | Test depth | L0 (pure math/DSP) + L1 (processor through `process()`) + one L3 case (sine input → expected peak bin region). UI rendering is verified manually in a host (REAPER) — no automated pixel tests in v1. |

---

## Architecture

```
plugins/sectra-scope/
├─ CMakeLists.txt              # smtg_add_vst3plugin(sectrascope ...)
└─ Source/
   ├─ PluginFactory.cpp        # BEGIN_FACTORY_DEF / DEF_CLASS2 x2 (component + controller)
   ├─ sectracids.h             # FUIDs + category (kFxDynamicsClass? -> kFxAnalyzer subcategory "Fx|Analyzer")
   ├─ sectraparamids.h         # ParamIDs: kFFTSizeID, kWindowTypeID, kModeID, kAttackID, kReleaseID, kDbRefID
   ├─ version.h
  ├─ SectraScopeProcessor.h/.cpp  # AudioEffect: passthrough audio + SpectrumAnalyzer per channel (renamed from Processor.* to avoid header collision with passthrough in the combined test exe)
   ├─ Controller.h/.cpp        # EditController: params + createView() -> CVSTGEditor
   ├─ ScopeView.h/.cpp         # VSTGUI CView: draws log-freq spectrum + dB grid (one instance per scope slot)
   └─ resource/editor.uidesc   # window: 2 stacked scope slots + choice controls + attack/release knobs
common/
├─ include/vdplg/spectrum.h    # NEW shared DSP (testable without VST3):
│                              #   - LogFreqMap (freq <-> x, band centers)
│                              #   - DbScale (mag <-> dBFS, db <-> y)
│                              #   - ChannelMix (L/R, M/S sample mixing)
│                              #   - MeterBallistics (per-column attack/release envelope, dB domain)
│                              #   - SpectrumAnalyzer (WindowedFFT + overlap + ballistics)
│                              #   - BalanceDiff (post-smoothing per-column M-S diff, +/-12 clamp)
└─ src/spectrum.cpp
tests/
├─ l0_spectrum.cpp             # NEW: LogFreqMap / DbScale / analyzer unit tests
└─ l1_sectra_scope.cpp         # NEW: processor process() tests
testdata/cases/
└─ sectra_sine.json            # NEW: 440 Hz sine -> unity passthrough + param_count()==6 (CTest name l3_sectra_sine)
```

### Data flow

```
audio thread (Processor::process)
  -> ChannelMix: derive the two analysis channels from L,R per current mode
  -> SpectrumAnalyzer::process(block) x2   [signalsmith WindowedFFT + MeterBallistics]
  -> mode M/(M-S): BalanceDiff(smoothedM, smoothedS) -> clamped +/-12 dB balance line
  -> latest pair of display lines (dB values per column) into ring buffer
UI thread (ScopeView, ~30 fps timer)
  -> pop newest pair of spectra from ring buffer
  -> each ScopeView draws its slot: background, grid (dBFS or +/-12 balance), octave ticks, filled curve
```

Thread safety: single-producer/single-consumer ring buffer holding a snapshot struct `{std::vector<float> specA, specB; std::vector<uint8_t> clipA, clipB;}` (clip flags feed the brighter-edge indicator; mutex-guarded copy acceptable at 30 fps).

---

## DSP design (the testable core)

### 1. `LogFreqMap` (pure math, no audio)

- Config: `fMin = 20`, `fMax = 20000`, `numColumns` (= view width in px, settable).
- Mapping (standard log scale):
  - `x(f) = (log2(f / fMin) / log2(fMax / fMin)) * numColumns`
  - `f(x) = fMin * pow(2, (x / numColumns) * log2(fMax / fMin))`
- **Band center frequency** for display column `i`: `fCenter(i) = f(x = i + 0.5)` (center of the pixel's frequency span).
- Octave tick positions: `x(20 * 2^k)` for k = 0..9 (20, 40, …, 10240 Hz).

### 2. `DbScale` (pure math)

- `dbToY(db, heightPx)`: linear map of `[−120, 0] dB → [height, 0]`.
- `balanceDbToY(db, heightPx)`: linear map of `[−12, +12] dB → [height, 0]` (middle line = 0 dB), used by the balance scope in M/(M−S) mode.
- Grid line y-positions every 6 dB.

### 3. `SpectrumAnalyzer` (DSP, uses signalsmith-dsp)

- Wraps `signalsmith::spectral::WindowedFFT<float>` sized to the selected FFT size.
- Window functions applied via `setSize(size, windowFn)` — Rectangular (`1`), Hann, Hamming, Blackman, Blackman-Harris (all closed-form, from signalsmith `windows.h` where available, else inline formulas).
- **Overlap:** hop = blockLength (host block); accumulate `N / hop` frames per process call; each frame: window → FFT → magnitude → convert to dBFS → **bin-to-column mapping**: for each FFT bin `b` with center freq `(b + 0.5) * fs / N` (ModifiedRealFFT half-bin convention), add its dB value into all display columns whose band spans that bin (max-combine within a frame, then average across frames).
- **dB reference:** Raw → `20·log10(mag)`; Normalized → `20·log10(mag · 2/Σwindow)` (coherent-gain compensation; full-scale sine ⇒ 0 dB for any window).
- **Ballistics (`MeterBallistics`, per column, dB domain):** applied once per `process()` call with `dt = blockLength / fs` (column targets update per host block, not per sample): given target `t` and current `d`: if `t > d` rise at most `24·dt/T_attack` (T_attack = 0 ⇒ snap to t); else fall at most `24·dt/T_release`. Linear-in-dB travel, symmetric definition with release.
- `configure(fftSize, windowType, sampleRate)` — resets buffers; called on init and on parameter change.
- Attack/release, dB-reference, and channel-mode changes apply live without re-prime (mixing happens before the FFT; ballistics/dB-ref/balance are per-column post-processing).
- DC/low bins below `fMin` are discarded (log axis starts at 20 Hz).

### 3b. `ChannelMix` (pure math, no audio)

- Input: L,R samples (or, for M/(M−S): two finished spectra). Output: analysis channels A,B.
- Sample-domain mixing only: `LR → (L, R)`; `MS → ((L+R)/2, (L−R)/2)`. Both modes feed two independent `SpectrumAnalyzer`s.
- Unit-testable in isolation (mixing math).

### 3c. `BalanceDiff` (pure math, no audio)

- Input: two **already-smoothed** per-column dB spectra (mid, side). Output: balance line per column: `b[i] = clamp(specM[i] − specS[i], −12, +12)` plus a per-column `clipped` flag (for the brighter-edge indicator).
- Zero-referenced: 0 = equal, positive = mid dominant, negative = side dominant.
- Unit-testable in isolation (mixing math + per-line subtraction).

### 4. Parameters (VST3)

| Param | ID | Type | Points |
|-------|----|------|--------|
| FFT Size | `kFFTSizeID` | discrete choice | 1024, 2048, 4096, 8192, 16384 |
| Window | `kWindowTypeID` | discrete choice | Rect, Hann, Hamming, Blackman, Blackman-Harris |
| Channel Mode | `kModeID` | discrete choice | L/R, Mid/Side, Mid/(Mid−Side) |
| Attack | `kAttackID` | continuous, log scale | 0 … 20 ms (24 dB rise time; 0 = instant); default 0 ms; string convert "0 ms" / "5 ms" / "20 ms" |
| Release | `kReleaseID` | continuous, log scale | 1 ms … 1 s (24 dB fall time); default 100 ms; string convert "100 ms" / "1.0 s" |
| dB Ref | `kDbRefID` | discrete choice (2) | Normalized (sine=0 dB), Raw (full-scale sample=0 dB) |

**Param-value mapping note:** Attack spans 0…20 ms on a log-ish scale — since log(0) is undefined, use: normalized 0 ⇒ 0 ms (instant); normalized v ∈ (0,1] ⇒ `20 ms · v²` (quadratic, perceptually fine for 0–20 ms). Release: normalized v ∈ [0,1] ⇒ `1 ms · 1000^v` (true log, 1 ms…1 s). Both documented in `sectraparamids.h` and covered by L1 string-convert checks.

- Processor caches values; FFT-size/window change → `analyzer.configure(...)` (re-primes); mode/attack/release/dB-ref apply live.
- Controller exposes all six with `StringConvert` (point labels "1024", "Hann", "L/R", "0 ms", "100 ms", "Norm", …).

---

## UI design (VSTGUI 4)

- Enable VSTGUI at top level: `set(SMTG_ENABLE_VSTGUI_SUPPORT ON CACHE BOOL "" FORCE)` before `include(SMTG_VST3_SDK)`; link `vstgui` into the plugin target (SDK provides the target once enabled).
- `editor.uidesc`: window ~**720×560**: two stacked scope slots (~720×200 each) + control strip (~720×120): choice controls for FFT size / window / channel mode / dB ref + **knobs for Attack and Release** (`CKnob` or `CSlider`, bound to `kAttackID`/`kReleaseID`, editable via drag and double-click numeric entry).
- `Controller::createView()` injects two `ScopeView` instances into the two slots (pattern: mandelbrot standalone example — custom `CView` subclass created in code, added to the container). Each view shows a small label of its current channel name ("L", "R", "M", "S", "M−S"), updated on mode change.
- `ScopeView::draw(CDrawContext*)`:
  1. Fill background (dark, e.g. `#1E1E1E`).
  2. **Grid:** normal scopes — horizontal lines every 6 dB (dim gray), labels every 12 dB on the left margin ("0", "−12", … "−120"); balance scope (mode M/(M−S), slot 2) — scale **+12…−12 dB**, emphasized middle line at 0 dB, labels every 6 dB.
  3. **Octave ticks:** vertical tick + label at each octave boundary (20, 40, 80, … 10 kHz); minor ticks at 3rd-octave steps optional (skip in v1).
  4. **Spectrum:** build a `CGraphicsPath` over the display columns using `dbToY`, fill with semi-transparent color + stroke the top edge; balance scope fills from the 0 dB middle line up/down; clipped columns (|diff| > 12) get a brighter edge segment.
- Refresh: `CTimer` at ~33 ms (30 fps) → pull newest snapshot from ring buffer → `invalidRect(getViewSize())`.
- Parameter widgets: four `CChoiceControl`s + attack/release knobs, standard uidesc wiring via param ids.

---

## Test plan (TDD — tests written and FAILING before implementation)

### L0 — `tests/l0_spectrum.cpp` (pure math/DSP, no VST3)

`LogFreqMap`:
- [ ] `x(fMin) == 0`, `x(fMax) == numColumns` (within epsilon).
- [ ] Octave symmetry: `x(2f) − x(f)` is constant for f = 20, 40, 80, 160 Hz (equal width per octave — the core requirement).
- [ ] Round-trip: `f(x(f)) ≈ f` for sample frequencies {20, 440, 5000, 20000}.
- [ ] `fCenter(i)` strictly increasing; `fCenter(0) > fMin`; last band center `< fMax`.

`DbScale`:
- [ ] `dbToY(0) == 0`, `dbToY(-120) == height`.
- [ ] `magToDb(1.0) ≈ 0`, `magToDb(0.5) ≈ -6.02`, `magToDb(0) <= -180` (no crash / clamped).
- [ ] Monotonic: higher magnitude → higher dB.

`SpectrumAnalyzer` (feed synthetic blocks directly, no VST3):
- [ ] **Sine peak location:** 440 Hz sine @ 44.1 kHz, FFT 4096, Hann → after warm-up, max display column is within ±1 column of `x(440)`. Same check at 100 Hz and 5 kHz.
- [ ] **Peak level (per dB reference):** Normalized → full-scale sine reads 0 dB ± 0.5 dB; Raw → ≈ −3.01 dB for rectangular (see dedicated `dB reference` tests below for per-window detail).
- [ ] **Silence floor:** all-zero input → all columns ≤ −100 dBFS after warm-up.
- [ ] **Reconfigure:** switching FFT size 1024→8192 mid-stream does not crash; peak still found at correct x after re-prime.
- [ ] **Window switch:** each of the 5 windows produces a valid spectrum (peak found, no NaN/Inf in any column).
- [ ] **Octave resolution sanity:** two sines one octave apart (440 + 880) produce two distinct local maxima at `x(440)` and `x(880)`.

`ChannelMix`:
- [ ] LR mode passes L,R through unchanged.
- [ ] MS mode: L=R ⇒ M=L, S=0; L=−R ⇒ M=0, S=L (sign convention documented).

`BalanceDiff`:
- [ ] specM == specS ⇒ all columns 0 dB (middle line).
- [ ] specM = −20, specS = −40 ⇒ +20 dB → clamped to +12 with clipped flag set.
- [ ] specM = −40, specS = −20 ⇒ −20 dB → clamped to −12 with clipped flag set.
- [ ] specS = −∞ (silence floor) ⇒ +12 clamp, no Inf/NaN.

`DbScale` (balance view):
- [ ] `balanceDbToY(0) == height/2`, `balanceDbToY(+12) == 0`, `balanceDbToY(−12) == height`.

`MeterBallistics`:
- [ ] Attack step: target jumps +24 dB, T_attack = 10 ms ⇒ reaches target in ≈10 ms (±1 dB at t=T); T_attack = 0 ⇒ snaps instantly.
- [ ] Release step: level drops 24 dB after T_release (T = 10 ms, 100 ms, 1 s), within ±1 dB tolerance.
- [ ] Asymmetry: rise and fall rates independent (fast attack + slow release keeps peak, decays slowly).
- [ ] Attack/release change mid-transient applies live (no reset of held levels).

`dB reference`:
- [ ] Normalized: full-scale sine reads 0 dB ± 0.5 dB for **each** of the 5 windows.
- [ ] Raw: full-scale sine reads ≈ −3.01 dB for rectangular; lower (more negative) for tapered windows; full-scale square-ish impulse/square stays ≤ 0 dB.

### L1 — `tests/l1_sectra_scope.cpp` (processor through `process()`)

- [ ] Passthrough integrity: output == input (analyzer must not alter audio), planar layout, using existing `toInterleaved` helper pattern from `l1_passthrough.cpp`.
- [ ] Param changes via fake `IParameterChanges` for **all six** params (FFT size, window, mode, attack, release, dB ref) → `process()` continues, output still equals input.
- [ ] Sample-rate change (`setSampleRate`) mid-stream → no crash, passthrough intact.

### L3 — `testdata/cases/sectra_sine.json` (CTest name: `l3_sectra_sine`)

- [ ] 440 Hz sine WAV in → process → assertions on output (unity passthrough). Analyzer internals already covered by L0/L1; L3 verifies the full plugin lifecycle with all 6 params registered (correct IDs/names/ranges).

### Manual UI verification (not automated)

- Load `sectrascope.vst3` in REAPER: both scopes render grids; 440 Hz mono tone shows a peak aligned with the 440 Hz position (between 200 Hz and 1 kHz ticks) in both slots; stereo-detuned or panned source in M/(M−S) mode: balance scope hovers near the middle line where M≈S and deviates above/below where one dominates, clamping at ±12 with brighter edge; attack/release knobs visibly change transient response; FFT/window/dB-ref controls re-render correctly.

---

## Phases & acceptance criteria

### Phase 1 — Plan (this document)
- [x] Written. **Gate: explicit user approval.**

### Phase 2 — Tests (Red)
Write, build, and run the failing tests:
- [x] `common/include/vdplg/spectrum.h` declared as **empty stubs** (signatures only, bodies `throw std::logic_error("not implemented")` or return sentinel values) so tests compile and FAIL.
- [x] `tests/l0_spectrum.cpp`, `tests/l1_sectra_scope.cpp` written per test plan above.
- [x] `plugins/sectra-scope/Source/Processor.*` minimal stub (passthrough + **6 params**, analyzer not yet wired) so L1 compiles.
- [x] CMake wiring: `add_subdirectory(plugins/sectra-scope)`, new test files into `vdplg_tests`.
- [x] **Acceptance:** `ctest` runs the new tests and they **FAIL** (not error) for the right reasons (sentinel values / not-implemented).

#### Implementation notes (Phase 2, 2026-08-31)
- Stubs throw `std::logic_error("spectrum: <what> not implemented")`; Catch2 reports these as failed assertions (unexpected exception), never crashes → clean Red.
- Full plugin skeleton exists already (needed so L1/L3 compile): `SectraScopeProcessor.{h,cpp}` (renamed from `Processor.*` to avoid a header-name collision with passthrough's `Processor.h` in the combined test exe), `Controller.{h,cpp}`, `PluginFactory.cpp`, `version.h`, `sectracids.h`, `sectraparamids.h`. UIDs are fixed forever once committed.
- Controller is textless but registers all six params with string conversion (`getParamStringByValue`/`getParamValueByString`). SDK 3.8.1 has no `str16FromString` helper — param strings are pure ASCII, copied verbatim into UTF-16 via a small local helper. `TChar`/`String128` live in namespace `Steinberg::Vst` (vsttypes.h), not bare `Steinberg`.
- `Component::initialize(FUnknown*)` returns `tresult` (via ComponentBase); do NOT compare its result against `kResultOk` after calling it on an object whose override chain resolves to void — keep the call plain.
- Testhost gained a `param_count()` metric (controller parameter count via sdk_hosting) and per-case plugin routing: cases named `sectra_*` load `sectrascope.vst3`, everything else loads `passthrough.vst3`; explicit `--plugin` always wins. Case file renamed `l3_sectra_sine.json` → `sectra_sine.json` (CTest prepends `l3_` itself; siblings don't carry the prefix).
- Verified Red state (Release): `vdplg_tests` = 44 cases, 28 failed — exactly the `[l0][sectra]` spectrum cases, each failing with "not implemented"; l1_sectra_scope (5 cases, 10255 assertions) + pre-existing suites pass; ctest 6/7 with only `vdplg_tests` red; `l3_sectra_sine` passes (its role is the Phase 5 gate).

### Phase 3 — Implement DSP (Green)
- [x] Implement `LogFreqMap`, `DbScale`, `ChannelMix`, `MeterBallistics`, `BalanceDiff`, `SpectrumAnalyzer` in `common/src/spectrum.cpp`.
- [x] Wire analyzers into `Processor::process()` (two analysis channels per mode, param-driven configure).
- [ ] **Acceptance:** all L0 + L1 tests pass; existing 6/6 ctest suite still green.
      → MET: all 28 `[l0][sectra]` cases pass; L1 + pre-existing suites pass; ctest 7/7 green.

#### Implementation notes (Phase 3, Green)
- **Normalization:** windowed N-sample frame is zero-padded to P = nextPow2(≥N·16) before the FFT
  (`signalsmith::fft::ModifiedRealFFT<float>`). Fine bin spacing keeps each peak aligned to the
  correct log-frequency display column even where the axis is dense (low end), and avoids scalloping.
  Coherent-gain factor `normFactor = 2/Σwindow` makes a full-scale sine read ≈A regardless of phase/window.
- **dB reference:** held levels are stored in *normalized* dB; Raw ref applies a constant −3.01 dB offset
  at the display stage so the ref switch changes live without re-prime. Satisfies raw+rectangular == −3.01 dB
  and raw(tapered) < norm(tapered) − 1 dB.
- **Display resolution:** analyzer uses exactly 720 columns over [20 Hz, 20 kHz], matching the
  `LogFreqMap(20,20000,720)` used by the L0 expected-column math.
- **Ballistics:** linear travel in dB, rise limited to 24 dB / T_attack (T=0 snaps), fall to 24 dB / T_release.
- **Processor wiring:** two `SpectrumAnalyzer`s (A/B); `syncAnalyzers(processSetup.sampleRate)` refreshes
  ballistics every block and rebuilds config only when fft/window/ref/sample-rate change; `runAnalysis` mixes
  L/R per channel mode then feeds both analyzers. Analysis is read-only on input buffers — passthrough intact.

### Phase 4 — UI (VSTGUI)
- [ ] Enable VSTGUI support; `Controller::createView()` returns editor with two `ScopeView`s + control strip.
- [ ] `ScopeView` draws grid + spectrum per UI design (incl. ±12 balance grid + clamp indicator); four choice controls + attack/release knobs bound to params.
- [ ] **Acceptance:** Release build green; plugin loads in REAPER; manual checklist above passes. (No automated gate — documented.)

### Phase 5 — Integration & docs
- [ ] L3 case added and passing (ctest 7/7).
- [ ] README updated (plugin list, build notes); PLAN.md implementation log entry appended.
- [ ] **Acceptance:** full clean Release build + ctest all green.

---

## Risks / open questions

**Resolved by user (2026-08-31):** VSTGUI enabled now; fixed 20 Hz–20 kHz; two scopes with L/R, M/S, M/(M−S) modes; M/(M−S) = zero-referenced ±12 dB balance view (middle line = M==S, up = mid dominant), difference taken after per-column smoothing; ballistics = attack 0…20 ms + release 1 ms…1 s, both defined as 24 dB travel time; dB reference as a parameter switch.

1. **ModifiedRealFFT half-bin convention:** signalsmith's `WindowedFFT` bins are centered at `(i+0.5)/N` — the bin→column mapping must use this, not `i/N`. Covered by L0 peak-location tests.
2. **Ballistics definition:** "T ms/24 dB" = *time for the level to travel 24 dB*, linear in the dB domain, symmetric for attack (rise) and release (fall). Attack 0 = instant snap.
3. **Performance:** 16384 FFT × 2 analysis channels per block is trivial on modern CPUs; no GPU path needed (NanoVG/dataexchange pattern deliberately not used in v1).
