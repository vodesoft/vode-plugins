# Plan — Sliding (hopped) spectrum at ~30 Hz

Status: COMPLETE (2026-09-04) — implemented, full suite green (70 cases / 19864 assertions).
Date: 2026-09-04

## Problem

Today `SpectrumAnalyzer::process(samples, n)` advances its trailing-N history by
`n` and re-runs a full zero-padded FFT on **every host block**. Two consequences:

1. **No real "slide".** The visible trace refreshes at whatever rate the worker can
   manage. With heavy padding (see below) the consumer is CPU-bound, the bounded SPSC
   queue drops most blocks, and the result is an irregular, sparse, non-sliding trace
   rather than a smooth sweep.
2. **CPU blowup at large FFT.** Padding grows to next power-of-two ≥ `fftSize × 16`.
   A 16K window therefore runs as a **256K-point** FFT, repeated every block.

Goal: run exactly **one FFT per hop**, where `hop ≈ sampleRate / 30`, giving a
deterministic ~30 Hz slide with heavy overlap for large windows. Attack/release then
act on each fast update (this is the requested "smoothing" — we reuse the existing
dB-domain ballistics; no new parameter).

## Decisions (confirmed with user)

| Question | Decision |
|---|---|
| Update-rate definition | **~30 Hz wall-clock**: `hop = max(1, round(sampleRateHz / kTargetHz))`, `kTargetHz ≈ 30`. Same refresh at any FFT size or sample rate. |
| What "smoothing" means | **Reuse existing Attack/Release** per fast update. No new knob, no EMA, no freq blur, no peak-hold. |
| CPU vs resolution | **Reduce/cap zero-padding** for large FFTs so 30 Hz stays affordable. Accept slightly coarser bins in the dense low end. |
| Knob or constant? | **Internal fixed ~30 Hz.** No new UI parameter, no state persistence change. |

## Where the logic lives

Inside `vdplg::spectrum::SpectrumAnalyzer` (`common/src/spectrum.cpp`, header
`common/include/vdplg/spectrum.h`). Rationale:

- Keeps it pure-DSP and L0-testable (matches repo convention; all existing analyzer
  tests call `an.process(...)` directly).
- `AnalysisWorker` needs **no changes** — it keeps calling `process()` per queued
  block; the analyzer now internally accumulates and only FFTs at hop boundaries.
- Ballistics step once per hop with a **constant** `dt = hop/sampleRate` (~33 ms),
  which is what makes attack/release behave predictably on the fast-updating visual.

The public API is unchanged: `configure(...)`, `process(samples, n)`, `spectrum()`,
`numColumns()`, `setBallistics(a, r)`. All behavior changes are internal.

## Design

### 1. Hop-based accumulation in `SpectrumAnalyzer`

New private members:
- `int hop_{1};` — computed in `configure()` from `sampleRate_`.
- `std::vector<float> pending_;` — samples accumulated since the last FFT frame.

`configure(int fftSize, WindowType, DbReference, double sr)`:
- After setting `sampleRate_`, compute `hop_ = std::max(1, (int)std::lround(sr / kTargetHz));`
  where `kTargetHz` is a file-local constant (~30.0).
- Reset `pending_.clear()` alongside the other buffer resets.

`process(const float* samples, int numSamples)`:
```
append [samples, samples+numSamples) into pending_
while pending_.size() >= hop_:
    take the first hop_ samples as one analysis frame
    processFrame(framePtr, hop_)          // advances history by exactly hop_, FFTs, ballistics dt=hop_/sr
    remove those hop_ samples from pending_
guard: if pending_ grows beyond some cap (e.g. 4 * hop_), drop oldest to bound memory
       when the host stalls then bursts.
```

`processFrame` stays essentially as-is but is now always called with `numSamples == hop_`,
so its `dtSec = hop_ / sampleRate_` is constant. The trailing-N history advance-by-≤N
invariant still holds because `hop_ <= fftSize_` for every offered size (largest hop at
48 kHz ≈ 1600 < smallest window 1024? → see note below).

> Note: at very high sample rates `hop_` could exceed the smallest window (1024).
> Clamp `hop_ = min(hop_, fftSize_)` so we never advance the history past its own end
> (the streaming rule that caused the Studio One heap corruption). At 48 kHz/1024 this
> clamps hop to 1024 → ~47 Hz; acceptable and safe.

### 2. Cap zero-padding

In `configure()`, replace the unbounded `×16` growth with a capped factor so large
windows stay cheap while small windows keep fine bins. Proposed shape (exact constants
pinned by tests):
```
paddedSize_ = nextPow2(fftSize_);
const int maxPad = /* e.g. */ 65536;                 // absolute cap on padded length
while (paddedSize_ < fftSize_ * PAD_FACTOR && paddedSize_ < maxPad)
    paddedSize_ <<= 1;
```
with `PAD_FACTOR` reduced from 16 (candidate values 4–8). Rationale: near 20 Hz a log
column is only ~0.2 Hz wide, which is why ×16 existed; capping trades a few low-end
columns of detail for a big CPU win. This is an accepted tradeoff per the user's choice.
The exact `PAD_FACTOR`/cap will be chosen so the L0 peak-column accuracy tests pass
within ±1 column for the standard test frequencies AND the worker-CPU guard passes.

## Features & acceptance criteria (TDD)

Each feature: write failing test first (Red), confirm it fails, implement (Green),
refactor. All new tests live in `tests/l0_spectrum.cpp` (analyzer) and/or
`tests/l0_analysisworker.cpp` (CPU guard). Existing tests must keep passing unchanged.

### F1 — Analyzer runs one FFT per hop, not per block
- **Test:** feed a steady sine in many small blocks whose total spans several hops;
  assert the display line reaches the expected peak level/column (peak found within
  ±1 col, normalized full-scale sine ≈ 0 dB). Also assert that feeding fewer than one
  hop's worth of samples produces no crash and leaves the previous spectrum intact.
- **Acceptance:** sliding cadence is time-based (`hop = round(sr/30)`), independent of
  host block size; existing per-block tests still green.

### F2 — Ballistics step once per hop with constant dt (~33 ms)
- **Test:** with instant attack + a known release, drive a tone then silence and check
  the decay matches the ballistics model stepped at `dt = hop/sr` (not per tiny block).
  Equivalently: verify the held level after a fixed wall-time of silence equals the
  analytic expectation to within tolerance.
- **Acceptance:** attack/release behave as "smoothing" on the fast-updating visual;
  no change to `MeterBallistics` semantics.

### F3 — Zero-padding capped; large FFT stays accurate enough
- **Test:** for each offered FFT size {1024…16384}, run a 440 Hz (and a low ~100 Hz)
  sine through the analyzer and require the peak lands within ±1 log column of
  `map.freqToX(freq)`. Assert `paddedSize_` for 16K is bounded (e.g. ≤ 65536) via a new
  introspection accessor or by asserting the CPU guard below.
- **Acceptance:** low-end columns may be coarser but all standard test frequencies stay
  within ±1 column; padded length no longer scales to 256K.

### F4 — Worker CPU guard: 16K @ ~30 Hz is affordable
- **Test (l0_analysisworker):** start worker at fftSize=16384, feed continuous audio,
  measure average wall-time per processed frame on the worker thread (or count delivered
  sink callbacks over a fixed real-time window). Require the effective update rate to be
  ≥ some floor (e.g. ≥ 15 Hz sustained) so the trace visibly slides rather than stalling.
- **Acceptance:** at max FFT the consumer keeps up well enough that queue drops are rare
  and the meter updates smoothly; this is the regression guard for the original bug.

## Out of scope / explicitly not doing
- No new user parameter for refresh/update rate (fixed internal ~30 Hz).
- No EMA/temporal averaging, frequency-domain blur, or peak-hold line.
- No changes to `AnalysisWorker`, channel mixing, balance diff, data-exchange, UI, or
  state persistence.
- No change to how dropped/oldest blocks are handled in the SPSC queue (the faster
  consumer simply makes drops rare).

## Risks & mitigations
- **Low-end column accuracy** degrades with less padding → pinned by F3's ±1-col tests;
  choose PAD_FACTOR/cap to satisfy them. If 100 Hz can't hold ±1 col at 16K, relax only
  the lowest-frequency assertion and document it.
- **hop > smallest window at high SR** → clamp `hop_ = min(hop_, fftSize_)` (F1 note);
  add an L0 case feeding at a high sample rate with the 1024 window to prove no OOB.
- **Existing tests assume per-block updates.** They should still pass because they feed
  enough total samples to produce ≥1 hop; if any timing-sensitive assertion breaks, adjust
  the *test* warm-up block counts (not the production semantics) and record it here.
- **Unbounded pending_ growth** after host stall/burst → cap pending_ and drop oldest
  (mirrors existing streaming rule). Add an L0 case: one huge burst then silence stays finite.

## Implementation notes
(Updated as work proceeds.)
- **Constants chosen:** `kTargetUpdateHz = 30.0`, `kPadFactor = 16` (kept), `kMaxPaddedSize = 65536`. The original plan proposed *reducing* the pad factor to 4–8; implementing that broke three pre-existing level tests (coherent-gain normalization depends on bin count), so the final design keeps x16 padding everywhere and lets only the hard cap bite on large windows (16K: 262144 → 65536 padded points, a 4x FFT-size reduction).
- **F4 threshold:** sustained-rate floor set to ≥10 Hz (measured comfortably higher); one sink callback per hop.
- **Test warm-ups:** four pre-existing level-sensitive L0 tests needed `blocks` 8→32 because hopped processing runs fewer frames over the same sample count until the trailing-N history fills with signal. Documented inline in each test.
- **No API change** beyond the new trivial `SpectrumAnalyzer::paddedSize()` introspection accessor used by F3.
