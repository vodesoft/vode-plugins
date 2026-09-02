# Plan — Move Sectra Scope analysis off the audio thread

## Problem

Inserting Sectra Scope changes perceived playback tempo, correlated with the FFT-size
parameter (smaller FFT → faster playback). Pitch is preserved. Removing the plugin restores
normal speed; Studio One's CPU/DSP meter spikes while it is inserted.

### Diagnosis

`Processor::process()` calls `runAnalysis()` every host block, which drives two
`spectrum::SpectrumAnalyzer::process()` calls. Each analyzer chunks input into `fftSize_`
frames and runs a zero-padded FFT where `paddedSize_ = fftSize_ × 16` (next power of two ≥
that). Per-block cost therefore grows super-linearly with the FFT parameter:

| FFT param | padded size | ~per-FFT flops (5·N·log₂N) | ×2 analyzers @ 48 kHz / 128-smp blocks |
|-----------|-------------|----------------------------|----------------------------------------|
| 1024      | 16384       | ~2.9 M                     | ~217 Gflops/s                          |
| 4096      | 65536       | ~15 M                      | ~1.1 Tflops/s                          |
| 16384     | 262144      | ~63 M                      | ~4.6 Tflops/s                          |

A single audio thread realistically sustains a few hundred Mflops/s. So the DSP thread runs
over budget; larger FFT ⇒ more overrun ⇒ Studio One stretches harder. This matches the report
exactly (pitch intact because the host resamples rather than drops samples).

**Conclusion:** the passthrough path is correct (pure memcpy); the defect is doing heavy FFT
work on the audio thread. An analysis-only meter must never do this there.

## Goal

Keep the audio-thread work in `process()` at O(numSamples) (a sample copy + a push), regardless
of FFT size. All windowing/FFT/ballistics run on a dedicated worker thread fed by a bounded
single-producer/single-consumer queue. The UI data-exchange send moves to the worker thread too.

Non-goals: changing any DSP math, parameters, or the display. Passthrough stays bit-perfect.

## Design

New component `common/src/analysisworker.{h,cpp}` (namespace `vdplg::spectrum`, header exposed
via `common/include/vdplg/analysisworker.h`) so it is unit-testable without the VST3 SDK.

```cpp
class AnalysisWorker {
public:
    struct Config { int fftSize; WindowType window; DbReference ref; double sampleRateHz;
                    ChannelMode mode; double attackSec; double releaseSec; };

    void start(const Config& cfg);   // spawn worker thread if not running
    void stop();                     // signal exit, join, clear queue
    bool running() const;

    // Audio thread ONLY. Non-blocking: copies L/R into a bounded SPSC queue and returns.
    // If the queue is full, the oldest pending block is dropped (analyzer may fall behind).
    void feed(const float* left, const float* right, int numSamples);

    // Worker thread only — called inside the loop after processing each block.
    using ScopeSink = std::function<void(const ScopeData&)>;
    void setScopeSink(ScopeSink sink);

private:
    void loop();
    // mutex-protected bounded queue of {left,right} blocks (SPSC, drop-oldest when full)
    // two SpectrumAnalyzer members, current Config, std::thread, std::atomic<bool> running_
};
```

Notes / decisions:
- **Queue policy:** bounded (e.g. 64 blocks), drop-oldest when full. The audio thread NEVER
  waits on the worker → no risk of blocking the DSP thread even under sustained overload.
- **Threading model:** one worker thread per plugin instance. `start`/`stop` are idempotent and
  guarded so they can be called from `setActive(true/false)` and the destructor.
- **Ballistics timing:** analyzers already advance ballistics per processed frame using the
  sample rate; feeding whole host blocks preserves that behavior (same as today, just off-thread).
- **M/(M-S):** the balance-diff computation (`balancediff::diff`) moves into the worker's scope
  snapshot build, since the worker owns both analyzers and the current mode.
- **Processor changes** (`SectraScopeProcessor.{h,cpp}`):
  - Remove `analyzerA_/B_`, `mixA_/B_`, `runAnalysis()`, and the inline mix switch.
  - Add an `spectrum::AnalysisWorker analysis_;`.
  - `syncAnalyzers()` → builds an `AnalysisWorker::Config` and calls `analysis_.configure(...)`
    (reconfigures analyzers on the worker side; cheap, no FFT).
  - `process()`: passthrough memcpy unchanged; replace `runAnalysis(...)+sendScopeData()` with a
    single `analysis_.feed(in32[0], right, data.numSamples)`.
  - `setActive(true)`: start worker + wire `setScopeSink([this](const ScopeData& d){ pushToExchange(d); })`.
    `setActive(false)`: stop worker. Destructor stops it too.
  - `pushToExchange()` = old `sendScopeData()` body but reading from a `ScopeData` handed in by
    the sink (no longer reads analyzer state directly). Runs on the worker thread — safe because
    `DataExchangeHandler` is designed for cross-thread sends after activation.
- **Lifetime safety:** `feed()` must be a no-op if the worker isn't running (before first
  activate / after deactivate). The mutex-guarded queue makes feed/loop race-free.

## Acceptance criteria

1. **Passthrough bit-perfect** — existing L1 tests still pass unchanged (output == input exactly).
2. **Audio thread does no FFT** — `process()` performs only O(numSamples) work: a copy plus a
   bounded-queue push. No `SpectrumAnalyzer::process` call occurs on the audio thread.
3. **UI still updates** — the worker produces scope snapshots and delivers them via the
   data-exchange sink at roughly block cadence.
4. **No blocking under overload** — when the queue is full, `feed()` drops the oldest pending
   block and returns immediately (bounded worst-case time independent of FFT size).
5. **Clean lifecycle** — repeated `start`/`stop` cycles are safe (idempotent), destructor joins
   the worker without deadlock or leak.
6. **M/(M-S) preserved** — balance-mode snapshot uses `balancediff::diff` as before.
7. Full suite green (existing 7 + new AnalysisWorker tests).

## Test plan (Catch2, new file `tests/l0_analysisworker.cpp`)

These target `AnalysisWorker` directly (no VST host needed):

- **T1 feed→spectrum:** configure small FFT, feed known sine blocks, pump the worker (via a
  test hook that runs one loop iteration synchronously), assert the produced `ScopeData` has
  plausible non-zero energy in the expected frequency region.
- **T2 drop-oldest when full:** fill the queue past capacity, verify `feed()` never blocks and
  the queue length stays bounded; assert oldest samples were dropped (not appended unboundedly).
- **T3 audio-path cost bound:** with a large FFT configured, measure wall-clock time of many
  `feed()` calls on the calling thread; assert per-feed time is far below one FFT's cost
  (i.e., feed is O(numSamples), not O(FFT)). This is the direct regression guard for the tempo bug.
- **T4 M/(M-S):** configure kMBalance mode, feed mid/side content, assert scope B carries the
  clamped ±12 dB difference values.
- **T5 lifecycle:** start/stop/start/stop repeatedly; assert no hang, `running()` reflects state,
  and destruction is clean.

A synchronous "pump" hook (`runOneIteration()`, used by tests instead of the real thread) keeps
these deterministic while production uses the background thread.

## Phases

- **Phase 1 (this doc):** plan + acceptance criteria. Awaiting approval.
- **Phase 2 (Red):** write T1–T5 failing against a stubbed `AnalysisWorker`.
- **Phase 3 (Green):** implement `analysisworker.{h,cpp}`, wire into processor, make all pass.
- **Phase 4 (Validate):** full ctest (expect ≥7 green), rebuild plugin, publish (host closed),
  user verifies: normal playback speed across FFT sizes AND M/(M-S) graph renders.

## Implementation notes
(record decisions/changes here as work proceeds so context survives compaction)

- **Phase 2 (Red):** `tests/l0_analysisworker.cpp` added (5 cases), wired into `vdplg_tests`.
  Confirmed Red — failed to compile (`analysisworker.h` missing).
- **Phase 3 (Green):** Implemented `common/{include/vdplg/analysisworker.h,src/analysisworker.cpp}`,
  added to `common` lib. Wired into processor: removed inline analyzers + `runAnalysis()`;
  `process()` now only calls `analysis_.feed(...)` (O(numSamples)); sink → `pushScopeToExchange`;
  worker started lazily by `syncAnalyzers()`, stopped on deactivate. `modeIndex_` made atomic
  (read on worker thread). ScopeSink extended to carry precomputed balance array.
- **Test fix:** M/(M-S) test initially used L=0.7/R=0.05 which yields mid≈side (balance≈0);
  corrected to identical L/R (mono) so side cancels → balance drives to +12 clamp.
- **Result:** full ctest suite 7/7 green; vdplg_tests runtime dropped ~129s→1.4s. Plugin DLL builds.
- **Pending:** Phase 4 publish (host must be CLOSED) + user verifies normal playback speed across
  FFT sizes AND M/(M-S) graph renders. Batched with the earlier pending M/(M-S) publish.
