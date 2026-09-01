// Vode Plugins — spectrum analysis DSP (Sectra Scope).
//
// Phase 3 (Green): real implementations of the pure-math building blocks and
// the windowed-FFT analyzer. See PLAN-sectra-scope.md for the design rationale.

#include "vdplg/spectrum.h"

#include <cmath>
#include <algorithm>
#include <limits>

namespace vdplg {
namespace spectrum {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// Display resolution used by the analyzer's log-frequency axis. The L0 tests
// build their expected column positions with a LogFreqMap(20, 20000, 720), so
// the analyzer must use exactly this many columns to line up with them.
constexpr int kDisplayColumns = 720;
constexpr double kFMinHz = 20.0;
constexpr double kFMaxHz = 20000.0;

// Deep floor for empty/near-empty bins (dBFS). Kept well below any audible
// signal and finite so downstream math never sees Inf/NaN.
constexpr float kDbFloor = -300.0f;

// Raw reference is uniformly ~3.01 dB below normalized (full-scale sine:
// normalized reads 0 dB, raw reads ~-3.01 dB). Applied as a constant offset in
// the dB domain so the ref switch can change live without re-prime.
constexpr float kRawOffsetDb = -3.0103f; // -20*log10(sqrt(2))

double clampD(double v, double lo, double hi)
{
	return std::min(hi, std::max(lo, v));
}

float clampF(float v, float lo, float hi)
{
	return std::min(hi, std::max(lo, v));
}

// Closed-form symmetric windows evaluated at r in [0,1] (r = i/(N-1)).
float windowValue(WindowType w, double r)
{
	switch (w)
	{
		case WindowType::kRectangular:
			return 1.0f;
		case WindowType::kHann:
			return static_cast<float>(0.5 * (1.0 - std::cos(kTwoPi * r)));
		case WindowType::kHamming:
			return static_cast<float>(0.54 - 0.46 * std::cos(kTwoPi * r));
		case WindowType::kBlackman:
			return static_cast<float>(0.42 - 0.5 * std::cos(kTwoPi * r) +
			                          0.08 * std::cos(2.0 * kTwoPi * r));
		case WindowType::kBlackmanHarris:
			return static_cast<float>(0.35875 - 0.48829 * std::cos(kTwoPi * r) +
			                          0.14128 * std::cos(2.0 * kTwoPi * r) -
			                          0.01168 * std::cos(3.0 * kTwoPi * r));
		default:
			return 1.0f;
	}
}

} // namespace

//------------------------------------------------------------------------
// LogFreqMap
//------------------------------------------------------------------------
LogFreqMap::LogFreqMap(double fMinHz, double fMaxHz, int numColumns)
    : fMin_(fMinHz), fMax_(fMaxHz), numColumns_(numColumns)
{
	if (fMin_ <= 0.0 || fMax_ <= fMin_)
	{
		fMin_ = 1.0;
		fMax_ = 2.0;
	}
	if (numColumns_ < 1)
		numColumns_ = 1;
	logSpan_ = std::log2(fMax_ / fMin_);
}

double LogFreqMap::freqToX(double hz) const
{
	const double f = clampD(hz, fMin_, fMax_);
	return (std::log2(f / fMin_) / logSpan_) * static_cast<double>(numColumns_);
}

double LogFreqMap::xToFreq(double x) const
{
	const double t = clampD(x, 0.0, static_cast<double>(numColumns_)) /
	                 static_cast<double>(numColumns_);
	return fMin_ * std::pow(2.0, t * logSpan_);
}

double LogFreqMap::bandCenterFreq(int col) const
{
	const int c = clampF(static_cast<float>(col), 0.0f,
	                     static_cast<float>(numColumns_ - 1));
	const int ci = static_cast<int>(c);
	return xToFreq(ci + 0.5);
}

int LogFreqMap::numColumns() const { return numColumns_; }
double LogFreqMap::fMin() const { return fMin_; }
double LogFreqMap::fMax() const { return fMax_; }

//------------------------------------------------------------------------
// DbScale
//------------------------------------------------------------------------
float DbScale::dbToY(float db, int heightPx)
{
	const float h = static_cast<float>(heightPx);
	const float lo = static_cast<float>(kDbMin);
	const float t = clampF(db / lo, 0.0f, 1.0f);
	return t * h;
}

float DbScale::balanceDbToY(float db, int heightPx)
{
	const float h = static_cast<float>(heightPx);
	const float r = static_cast<float>(kBalanceRangeDb);
	const float t = clampF((r - db) / (2.0f * r), 0.0f, 1.0f);
	return t * h;
}

float DbScale::magToDb(float magnitude)
{
	if (!(magnitude > 0.0f))
		return kDbFloor;
	float db = 20.0f * std::log10(magnitude);
	if (!std::isfinite(db) || db < kDbFloor)
		db = kDbFloor;
	return db;
}

//------------------------------------------------------------------------
// ChannelMix
//------------------------------------------------------------------------
namespace channelmix {

MixResult mix(const std::vector<float>& left, const std::vector<float>& right,
              ChannelMode mode)
{
	MixResult res;
	res.a.resize(left.size());
	res.b.resize(right.size());
	const std::size_t n = std::min(left.size(), right.size());
	for (std::size_t i = 0; i < n; ++i)
	{
		const float l = left[i];
		const float r = right[i];
		switch (mode)
		{
			case ChannelMode::kLR:
				res.a[i] = l;
				res.b[i] = r;
				break;
			case ChannelMode::kMS:
				res.a[i] = 0.5f * (l + r); // mid
				res.b[i] = 0.5f * (l - r); // side
				break;
			case ChannelMode::kMBalance:
				res.a[i] = 0.5f * (l + r); // mid (dBFS grid scope)
				res.b[i] = 0.5f * (l - r); // side (feeds balance view)
				break;
		}
	}
	return res;
}

} // namespace channelmix

//------------------------------------------------------------------------
// MeterBallistics
//------------------------------------------------------------------------
MeterBallistics::MeterBallistics() = default;

void MeterBallistics::setTimes(double attackSec, double releaseSec)
{
	attackSec_ = attackSec < 0.0 ? 0.0 : attackSec;
	releaseSec_ = releaseSec < 0.0 ? 0.0 : releaseSec;
}

double MeterBallistics::attackSec() const { return attackSec_; }
double MeterBallistics::releaseSec() const { return releaseSec_; }

float MeterBallistics::step(float held, float target, double dtSec)
{
	if (target == held)
		return held;
	if (dtSec <= 0.0)
		return held;

	const bool rising = target > held;
	double t = rising ? attackSec_ : releaseSec_;
	if (t <= 0.0)
		return target; // instant snap in that direction

	const double maxTravelDb = 24.0 * (dtSec / t);
	float next = held + (rising ? static_cast<float>(maxTravelDb)
	                            : -static_cast<float>(maxTravelDb));
	// Never overshoot the target.
	next = rising ? std::min(next, target) : std::max(next, target);
	return next;
}

//------------------------------------------------------------------------
// BalanceDiff
//------------------------------------------------------------------------
namespace balancediff {

BalanceResult diff(const std::vector<float>& midDb, const std::vector<float>& sideDb)
{
	BalanceResult res;
	const std::size_t n = std::min(midDb.size(), sideDb.size());
	res.db.resize(n);
	res.clipped.assign(n, 0);
	for (std::size_t i = 0; i < n; ++i)
	{
		const float d = midDb[i] - sideDb[i];
		const float lo = static_cast<float>(-DbScale::kBalanceRangeDb);
		const float hi = static_cast<float>(DbScale::kBalanceRangeDb);
		const float clamped = clampF(d, lo, hi);
		res.db[i] = clamped;
		res.clipped[i] = (d > hi || d < lo) ? 1 : 0;
	}
	return res;
}

} // namespace balancediff

//------------------------------------------------------------------------
// SpectrumAnalyzer
//------------------------------------------------------------------------
SpectrumAnalyzer::SpectrumAnalyzer() = default;

void SpectrumAnalyzer::configure(int fftSize, WindowType window, DbReference ref,
                                 double sampleRateHz)
{
	if (fftSize < 2)
		fftSize = 2;
	if (sampleRateHz <= 0.0)
		sampleRateHz = 44100.0;

	fftSize_ = fftSize;
	window_ = window;
	ref_ = ref;
	sampleRate_ = sampleRateHz;

	// Zero-pad to a power-of-two multiple of N for fast FFTs and fine bin
	// resolution. Fine bins keep the peak aligned to the correct log-frequency
	// display column even where the axis is dense (low end), and avoid
	// scalloping under coherent-gain normalization.
	paddedSize_ = fftSize_;
	while (paddedSize_ < fftSize_ * 16)
		paddedSize_ <<= 1;

	mrfft_.setSize(paddedSize_);

	// Build the analysis window over [0, N) with r = i/(N-1), and its coherent
	// gain sumW used for normalized-reference compensation.
	winBuf_.assign(fftSize_, 1.0f);
	double sumW = 0.0;
	const double denom = static_cast<double>(fftSize_ - 1);
	for (int i = 0; i < fftSize_; ++i)
	{
		const double r = denom > 0.0 ? static_cast<double>(i) / denom : 0.0;
		const float wv = windowValue(window_, r);
		winBuf_[static_cast<std::size_t>(i)] = wv;
		sumW += wv;
	}
	normFactor_ = (sumW > 0.0) ? static_cast<float>(2.0 / sumW) : 1.0f;

	map_ = LogFreqMap(kFMinHz, kFMaxHz, kDisplayColumns);

	history_.assign(static_cast<std::size_t>(fftSize_), 0.0f);
	bins_.resize(static_cast<std::size_t>(paddedSize_) / 2);
	buf_.assign(static_cast<std::size_t>(paddedSize_), 0.0f);
	targetLinear_.assign(kDisplayColumns, 0.0f);
	held_.assign(kDisplayColumns, kDbFloor);
	display_.assign(kDisplayColumns, kDbFloor);
	configured_ = true;
}

void SpectrumAnalyzer::process(const float* samples, int numSamples)
{
	if (!samples || numSamples <= 0 || !configured_)
		return;

	// Hosts may deliver blocks larger than the FFT size: chunk them so the
	// trailing-N history never advances past its own end.
	int offset = 0;
	while (offset < numSamples)
	{
		const int n = std::min(fftSize_, numSamples - offset);
		processFrame(samples + offset, n);
		offset += n;
	}
}

void SpectrumAnalyzer::processFrame(const float* samples, int numSamples)
{
	// Advance the trailing-N history by numSamples (<= fftSize_).
	std::vector<float> shifted(history_.begin() + numSamples, history_.end());
	shifted.insert(shifted.end(), samples, samples + numSamples);
	history_.swap(shifted);

	// Window the frame and zero-pad to paddedSize_.
	std::fill(buf_.begin(), buf_.end(), 0.0f);
	for (int i = 0; i < fftSize_; ++i)
		buf_[static_cast<std::size_t>(i)] =
		    history_[static_cast<std::size_t>(i)] * winBuf_[static_cast<std::size_t>(i)];

	mrfft_.fft(buf_.data(), bins_.data());

	std::fill(targetLinear_.begin(), targetLinear_.end(), 0.0f);
	const double binHz = sampleRate_ / static_cast<double>(paddedSize_);
	const std::size_t halfBins = static_cast<std::size_t>(paddedSize_) / 2;
	for (std::size_t b = 0; b < halfBins; ++b)
	{
		const double f = (static_cast<double>(b) + 0.5) * binHz;
		if (f < kFMinHz)
			continue; // below the log axis start
		const auto& c = bins_[b];
		const float mag = std::hypot(c.real(), c.imag()) * normFactor_;
		const double x = map_.freqToX(f);
		int col = static_cast<int>(std::lround(x));
		if (col < 0) col = 0;
		if (col > kDisplayColumns - 1) col = kDisplayColumns - 1;
		targetLinear_[static_cast<std::size_t>(col)] =
		    std::max(targetLinear_[static_cast<std::size_t>(col)], mag);
	}
	const double dtSec = static_cast<double>(numSamples) / sampleRate_;
	for (int c = 0; c < kDisplayColumns; ++c)
	{
		const float dbTarget = DbScale::magToDb(
		    targetLinear_[static_cast<std::size_t>(c)]);
		held_[static_cast<std::size_t>(c)] =
		    ballistics_.step(held_[static_cast<std::size_t>(c)], dbTarget, dtSec);
		display_[static_cast<std::size_t>(c)] = held_[static_cast<std::size_t>(c)] +
		                                        rawOffset();
	}
}

const std::vector<float>& SpectrumAnalyzer::spectrum() const { return display_; }
int SpectrumAnalyzer::numColumns() const { return configured_ ? kDisplayColumns : 0; }

void SpectrumAnalyzer::setBallistics(double attackSec, double releaseSec)
{
	ballistics_.setTimes(attackSec, releaseSec);
}

float SpectrumAnalyzer::rawOffset() const
{
	return ref_ == DbReference::kRaw ? kRawOffsetDb : 0.0f;
}

} // namespace spectrum
} // namespace vdplg
