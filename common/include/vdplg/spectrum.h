// Vode Plugins — spectrum analysis DSP (Sectra Scope).
//
// Pure math / DSP building blocks, testable without the VST3 SDK:
//   - LogFreqMap      : log-frequency axis mapping (freq <-> column)
//   - DbScale         : dBFS <-> pixel-Y mapping (+ balance view scale)
//   - ChannelMix      : L/R -> analysis-channel sample mixing
//   - MeterBallistics : per-column attack/release envelope in the dB domain
//   - BalanceDiff     : post-smoothing per-column mid-side difference
//   - SpectrumAnalyzer: windowed FFT + overlap + ballistics -> display line
//
// Phase 2 (TDD Red): signatures only; bodies throw std::logic_error.

#pragma once

#include <cstdint>
#include <cstddef>
#include <complex>
#include <stdexcept>
#include <vector>

#include "signalsmith-dsp/fft.h"

namespace vdplg {
namespace spectrum {

//------------------------------------------------------------------------
// enums
//------------------------------------------------------------------------
enum class WindowType
{
	kRectangular = 0,
	kHann = 1,
	kHamming = 2,
	kBlackman = 3,
	kBlackmanHarris = 4,
};

enum class DbReference
{
	kNormalized = 0, // full-scale sine reads 0 dB (coherent-gain compensated)
	kRaw = 1,        // full-scale sample reads 0 dB
};

enum class ChannelMode
{
	kLR = 0,       // scope A = L, scope B = R
	kMS = 1,       // scope A = Mid=(L+R)/2, scope B = Side=(L-R)/2
	kMBalance = 2, // scope A = Mid (dBFS grid), scope B = M-S balance view
};

constexpr int kNumWindowTypes = 5;
constexpr int kNumChannelModes = 3;

//------------------------------------------------------------------------
// LogFreqMap
//------------------------------------------------------------------------
class LogFreqMap
{
public:
	LogFreqMap(double fMinHz, double fMaxHz, int numColumns);

	double freqToX(double hz) const;          // [0, numColumns]
	double xToFreq(double x) const;           // Hz
	double bandCenterFreq(int col) const;     // center of column's frequency span
	int numColumns() const;
	double fMin() const;
	double fMax() const;

private:
	double fMin_{20.0};
	double fMax_{20000.0};
	int numColumns_{720};
	double logSpan_{1.0};
};

//------------------------------------------------------------------------
// DbScale
//------------------------------------------------------------------------
class DbScale
{
public:
	static constexpr double kDbMin = -120.0;
	static constexpr double kBalanceRangeDb = 12.0;

	// Normal scope: [-120, 0] dB -> [heightPx, 0].
	static float dbToY(float db, int heightPx);
	// Balance scope: [-12, +12] dB -> [heightPx, 0]; 0 dB at the middle line.
	static float balanceDbToY(float db, int heightPx);
	// Magnitude -> dBFS with a floor for zero/near-zero magnitudes.
	static float magToDb(float magnitude);
};

//------------------------------------------------------------------------
// ChannelMix
//------------------------------------------------------------------------
struct MixResult
{
	std::vector<float> a; // analysis channel A
	std::vector<float> b; // analysis channel B
};

namespace channelmix {
MixResult mix(const std::vector<float>& left, const std::vector<float>& right,
              ChannelMode mode);
} // namespace channelmix

//------------------------------------------------------------------------
// MeterBallistics
//------------------------------------------------------------------------
class MeterBallistics
{
public:
	MeterBallistics();

	void setTimes(double attackSec, double releaseSec);
	double attackSec() const;
	double releaseSec() const;

	// Advance held level `held` toward target by dt seconds.
	// Rise limited to 24 dB / T_attack (T_attack == 0 snaps instantly);
	// fall limited to 24 dB / T_release. Linear travel in the dB domain.
	float step(float held, float target, double dtSec);

private:
	double attackSec_{0.0};   // 0 => instant snap up
	double releaseSec_{0.1};  // seconds per 24 dB of fall
};

//------------------------------------------------------------------------
// BalanceDiff
//------------------------------------------------------------------------
struct BalanceResult
{
	std::vector<float> db;      // clamped to [-12, +12] per column
	std::vector<uint8_t> clipped; // 1 where |mid - side| exceeded +/-12 dB
};

namespace balancediff {
BalanceResult diff(const std::vector<float>& midDb, const std::vector<float>& sideDb);
} // namespace balancediff

//------------------------------------------------------------------------
// SpectrumAnalyzer
//------------------------------------------------------------------------
class SpectrumAnalyzer
{
public:
	SpectrumAnalyzer();

	// Resets internal buffers and ballistics state.
	void configure(int fftSize, WindowType window, DbReference ref, double sampleRateHz);

	// Processes one host block of samples; updates the display line.
	void process(const float* samples, int numSamples);

	const std::vector<float>& spectrum() const; // dB values per display column
	int numColumns() const;

	// Live-updatable meter ballistics (seconds).
	void setBallistics(double attackSec, double releaseSec);

private:
	float rawOffset() const;
	// Processes exactly one fftSize_-sample frame (history must hold >= N).
	void processFrame(const float* samples, int numSamples);

	bool configured_{false};
	int fftSize_{0};
	int paddedSize_{0};
	WindowType window_{WindowType::kHann};
	DbReference ref_{DbReference::kNormalized};
	double sampleRate_{44100.0};
	float normFactor_{1.0f};

	signalsmith::fft::ModifiedRealFFT<float> mrfft_;
	LogFreqMap map_{20.0, 20000.0, 720};
	MeterBallistics ballistics_;
	std::vector<float> winBuf_;      // analysis window [0, N)
	std::vector<float> history_;     // trailing-N samples
	std::vector<float> buf_;         // zero-padded FFT input [P]
	std::vector<std::complex<float>> bins_; // P/2 complex output
	std::vector<float> targetLinear_;// linear magnitude per column
	std::vector<float> held_;        // smoothed dB per column
	std::vector<float> display_;     // held + raw offset
};

} // namespace spectrum
} // namespace vdplg
