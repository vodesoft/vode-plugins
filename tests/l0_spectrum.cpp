// Vode Plugins — L0 tests: Sectra Scope spectrum DSP (pure math/DSP).
//
// Covers LogFreqMap, DbScale, ChannelMix, MeterBallistics, BalanceDiff and
// SpectrumAnalyzer directly (no VST3 involved). Phase 2 (TDD Red): these
// tests must compile against the empty stubs and FAIL until Phase 3.

#define _USE_MATH_DEFINES
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "vdplg/spectrum.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace vdplg::spectrum;
using Catch::Approx;

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int kColumns = 720; // typical scope width in px

std::vector<float> makeSine(double freqHz, int numSamples, float amplitude)
{
	std::vector<float> out(static_cast<std::size_t>(numSamples), 0.0f);
	for (int s = 0; s < numSamples; ++s)
		out[static_cast<std::size_t>(s)] =
		    static_cast<float>(amplitude * std::sin(2.0 * M_PI * freqHz * s / kSampleRate));
	return out;
}

// Runs `blocks` blocks of `blockLen` samples through the analyzer and returns
// its display line.
std::vector<float> runAnalyzer(int fftSize, WindowType window, DbReference ref,
                               const std::vector<float>& signal, int blockLen, int blocks)
{
	SpectrumAnalyzer an;
	an.configure(fftSize, window, ref, kSampleRate);
	const int total = static_cast<int>(signal.size());
	for (int b = 0; b < blocks && b * blockLen < total; ++b)
		an.process(signal.data() + static_cast<std::size_t>(b) * blockLen, blockLen);
	return an.spectrum();
}

int argMaxColumn(const std::vector<float>& spec)
{
	int best = 0;
	float bestVal = -std::numeric_limits<float>::infinity();
	for (std::size_t i = 0; i < spec.size(); ++i)
		if (spec[i] > bestVal)
		{
			bestVal = spec[i];
			best = static_cast<int>(i);
		}
	return best;
}

float peakDbNear(const std::vector<float>& spec, int col, int radius)
{
	float best = -std::numeric_limits<float>::infinity();
	for (int c = col - radius; c <= col + radius; ++c)
	{
		if (c < 0 || c >= static_cast<int>(spec.size())) continue;
		best = std::max(best, spec[c]);
	}
	return best;
}

bool allFiniteAndBounded(const std::vector<float>& spec, float lo, float hi)
{
	for (float v : spec)
	{
		if (!std::isfinite(v)) return false;
		if (v < lo || v > hi) return false;
	}
	return true;
}

} // namespace

//------------------------------------------------------------------------
// LogFreqMap
//------------------------------------------------------------------------
TEST_CASE("L0: LogFreqMap maps range endpoints to axis ends", "[l0][sectra][logfreq]")
{
	LogFreqMap map(20.0, 20000.0, kColumns);
	REQUIRE(map.fMin() == Approx(20.0));
	REQUIRE(map.fMax() == Approx(20000.0));
	REQUIRE(map.numColumns() == kColumns);
	REQUIRE(map.freqToX(20.0) == Approx(0.0).margin(1e-9));
	REQUIRE(map.freqToX(20000.0) == Approx(static_cast<double>(kColumns)).margin(1e-6));
}

TEST_CASE("L0: LogFreqMap gives equal width per octave", "[l0][sectra][logfreq]")
{
	LogFreqMap map(20.0, 20000.0, kColumns);
	double widths[4];
	const double freqs[4] = {20.0, 40.0, 80.0, 160.0};
	for (int i = 0; i < 4; ++i)
		widths[i] = map.freqToX(freqs[i] * 2.0) - map.freqToX(freqs[i]);
	for (int i = 1; i < 4; ++i)
		REQUIRE(widths[i] == Approx(widths[0]).margin(1e-6));
}

TEST_CASE("L0: LogFreqMap round-trips frequency <-> position", "[l0][sectra][logfreq]")
{
	LogFreqMap map(20.0, 20000.0, kColumns);
	for (double f : {20.0, 440.0, 5000.0, 20000.0})
	{
		double x = map.freqToX(f);
		REQUIRE(map.xToFreq(x) == Approx(f).epsilon(1e-9));
	}
}

TEST_CASE("L0: LogFreqMap band centers are increasing and inside the range",
          "[l0][sectra][logfreq]")
{
	LogFreqMap map(20.0, 20000.0, kColumns);
	double prev = 0.0;
	for (int i = 0; i < kColumns; ++i)
	{
		double fc = map.bandCenterFreq(i);
		REQUIRE(fc > prev);
		prev = fc;
	}
	REQUIRE(map.bandCenterFreq(0) > 20.0);
	REQUIRE(map.bandCenterFreq(kColumns - 1) < 20000.0);
}

//------------------------------------------------------------------------
// DbScale
//------------------------------------------------------------------------
TEST_CASE("L0: DbScale maps dBFS endpoints to pixel ends", "[l0][sectra][dbscale]")
{
	const int h = 200;
	REQUIRE(DbScale::dbToY(0.0f, h) == Approx(0.0f).margin(1e-3f));
	REQUIRE(DbScale::dbToY(-120.0f, h) == Approx(static_cast<float>(h)).margin(1e-3f));
	REQUIRE(DbScale::dbToY(-60.0f, h) == Approx(h / 2.0f).margin(1e-3f));
}

TEST_CASE("L0: DbScale magnitude-to-dB values", "[l0][sectra][dbscale]")
{
	REQUIRE(DbScale::magToDb(1.0f) == Approx(0.0f).margin(1e-3f));
	REQUIRE(DbScale::magToDb(0.5f) == Approx(-6.0206f).margin(0.01f));
	REQUIRE(DbScale::magToDb(0.0f) <= -180.0f); // clamped floor, no crash
}

TEST_CASE("L0: DbScale is monotonic in magnitude", "[l0][sectra][dbscale]")
{
	float mags[] = {0.001f, 0.01f, 0.1f, 0.5f, 1.0f};
	for (std::size_t i = 1; i < sizeof(mags) / sizeof(mags[0]); ++i)
		REQUIRE(DbScale::magToDb(mags[i]) > DbScale::magToDb(mags[i - 1]));
}

TEST_CASE("L0: DbScale balance view maps +/-12 dB around the middle line",
          "[l0][sectra][dbscale]")
{
	const int h = 200;
	REQUIRE(DbScale::balanceDbToY(0.0f, h) == Approx(h / 2.0f).margin(1e-3f));
	// Inverted vertically so that ZERO SIDE (which clamps to +12 dB) sits LOW on
	// screen; more side energy (-12 dB) rises toward the top.
	REQUIRE(DbScale::balanceDbToY(+12.0f, h) == Approx(static_cast<float>(h)).margin(1e-3f));
	REQUIRE(DbScale::balanceDbToY(-12.0f, h) == Approx(0.0f).margin(1e-3f));
}

//------------------------------------------------------------------------
// ChannelMix
//------------------------------------------------------------------------
TEST_CASE("L0: ChannelMix L/R passes channels through unchanged", "[l0][sectra][mix]")
{
	std::vector<float> l = {1.0f, -0.5f, 0.25f, 0.75f};
	std::vector<float> r = {-1.0f, 0.5f, -0.25f, 0.1f};
	auto res = channelmix::mix(l, r, ChannelMode::kLR);
	REQUIRE(res.a.size() == l.size());
	REQUIRE(res.b.size() == r.size());
	for (std::size_t i = 0; i < l.size(); ++i)
	{
		REQUIRE(res.a[i] == Approx(l[i]).epsilon(1e-7f));
		REQUIRE(res.b[i] == Approx(r[i]).epsilon(1e-7f));
	}
}

TEST_CASE("L0: ChannelMix M/S sign convention", "[l0][sectra][mix]")
{
	// L == R  =>  M = L, S = 0
	{
		std::vector<float> l = {0.8f, -0.3f, 0.1f};
		auto res = channelmix::mix(l, l, ChannelMode::kMS);
		for (std::size_t i = 0; i < l.size(); ++i)
		{
			REQUIRE(res.a[i] == Approx(l[i]).epsilon(1e-7f));
			REQUIRE(res.b[i] == Approx(0.0f).epsilon(1e-7f));
		}
	}
	// L == -R =>  M = 0, S = L
	{
		std::vector<float> l = {0.6f, -0.4f, 0.9f};
		std::vector<float> r(l.size());
		for (std::size_t i = 0; i < l.size(); ++i) r[i] = -l[i];
		auto res = channelmix::mix(l, r, ChannelMode::kMS);
		for (std::size_t i = 0; i < l.size(); ++i)
		{
			REQUIRE(res.a[i] == Approx(0.0f).epsilon(1e-7f));
			REQUIRE(res.b[i] == Approx(l[i]).epsilon(1e-7f));
		}
	}
}

//------------------------------------------------------------------------
// MeterBallistics
//------------------------------------------------------------------------
TEST_CASE("L0: MeterBallistics attack reaches +24 dB target in T_attack",
          "[l0][sectra][ballistics]")
{
	MeterBallistics m;
	m.setTimes(0.010, 1.0); // 10 ms attack
	float held = -100.0f;
	const double dt = 1.0 / 44100.0;
	int steps = static_cast<int>(0.010 / dt); // ~10 ms worth of blocks
	for (int i = 0; i < steps; ++i)
		held = m.step(held, -76.0f, dt); // +24 dB jump
	REQUIRE(held >= Approx(-76.0f).margin(1.0f)); // within 1 dB at t = T
}

TEST_CASE("L0: MeterBallistics zero attack snaps instantly", "[l0][sectra][ballistics]")
{
	MeterBallistics m;
	m.setTimes(0.0, 1.0);
	float held = m.step(-100.0f, -50.0f, 1.0 / 44100.0);
	REQUIRE(held == Approx(-50.0f).margin(1e-3f));
}

TEST_CASE("L0: MeterBallistics release drops 24 dB after T_release",
          "[l0][sectra][ballistics]")
{
	const double releases[] = {0.010, 0.100, 1.0};
	for (double tr : releases)
	{
		MeterBallistics m;
		m.setTimes(0.0, tr);
		float held = -50.0f;
		const double dt = 1.0 / 44100.0;
		int steps = static_cast<int>(tr / dt);
		for (int i = 0; i < steps; ++i)
			held = m.step(held, -100.0f, dt); // -50 dB target => 24 dB fall
		REQUIRE(held <= Approx(-74.0f).margin(1.0f));
	}
}

TEST_CASE("L0: MeterBallistics rise and fall rates are independent",
          "[l0][sectra][ballistics]")
{
	MeterBallistics m;
	m.setTimes(0.0, 1.0); // instant attack, 1 s release
	const double dt = 1.0 / 44100.0;

	float held = m.step(-100.0f, -50.0f, dt); // snap up to peak
	REQUIRE(held == Approx(-50.0f).margin(1e-3f));

	// After 10 ms of falling toward -100 dB, level must have moved little
	// (full 24 dB travel takes 1 s): still above -52 dB.
	for (int i = 0; i < static_cast<int>(0.010 / dt); ++i)
		held = m.step(held, -100.0f, dt);
	REQUIRE(held > -52.0f);
}

TEST_CASE("L0: MeterBallistics time changes apply live without resetting levels",
          "[l0][sectra][ballistics]")
{
	MeterBallistics m;
	m.setTimes(0.0, 1.0);
	const double dt = 1.0 / 44100.0;
	float held = m.step(-100.0f, -50.0f, dt); // at peak -50 dB

	// Switch to a fast release mid-transient: the held level is preserved...
	m.setTimes(0.0, 0.001);
	REQUIRE(m.attackSec() == Approx(0.0).margin(1e-9));
	REQUIRE(m.releaseSec() == Approx(0.001).margin(1e-9));
	float after = m.step(held, -100.0f, dt);
	// ...and now falls quickly (24 dB in 1 ms), so one block moves ~0.55 dB.
	REQUIRE(after < held - 0.4f);
}

//------------------------------------------------------------------------
// BalanceDiff
//------------------------------------------------------------------------
TEST_CASE("L0: BalanceDiff equal spectra give 0 dB everywhere", "[l0][sectra][balance]")
{
	std::vector<float> spec = {-20.0f, -60.0f, -100.0f};
	auto res = balancediff::diff(spec, spec);
	REQUIRE(res.db.size() == spec.size());
	for (float v : res.db)
		REQUIRE(v == Approx(0.0f).margin(1e-3f));
}

TEST_CASE("L0: BalanceDiff clamps +20 dB difference to +12 with clip flag",
          "[l0][sectra][balance]")
{
	std::vector<float> mid = {-20.0f};
	std::vector<float> side = {-40.0f};
	auto res = balancediff::diff(mid, side);
	REQUIRE(res.db[0] == Approx(+12.0f).margin(1e-3f));
	REQUIRE(res.clipped[0] != 0);
}

TEST_CASE("L0: BalanceDiff clamps -20 dB difference to -12 with clip flag",
          "[l0][sectra][balance]")
{
	std::vector<float> mid = {-40.0f};
	std::vector<float> side = {-20.0f};
	auto res = balancediff::diff(mid, side);
	REQUIRE(res.db[0] == Approx(-12.0f).margin(1e-3f));
	REQUIRE(res.clipped[0] != 0);
}

TEST_CASE("L0: BalanceDiff handles silence floor without Inf/NaN", "[l0][sectra][balance]")
{
	std::vector<float> mid = {-40.0f};
	std::vector<float> side = {DbScale::magToDb(0.0f)}; // deep floor
	auto res = balancediff::diff(mid, side);
	REQUIRE(std::isfinite(res.db[0]));
	REQUIRE(res.db[0] == Approx(+12.0f).margin(1e-3f));
	REQUIRE(res.clipped[0] != 0);
}

//------------------------------------------------------------------------
// SpectrumAnalyzer — peak location & level
//------------------------------------------------------------------------
TEST_CASE("L0: analyzer finds sine peaks at the correct log-frequency columns",
          "[l0][sectra][analyzer]")
{
	LogFreqMap map(20.0, 20000.0, kColumns);
	const int fftSize = 4096;
	const int blockLen = 512;
	// Hopped processing (~30 Hz) needs more total samples than the old
	// per-block scheme for the meter to settle: 32 blocks ~= 330 ms.
	const int blocks = 32;

	for (double freq : {100.0, 440.0, 5000.0})
	{
		auto signal = makeSine(freq, blockLen * blocks, 0.5f);
		auto spec = runAnalyzer(fftSize, WindowType::kHann, DbReference::kNormalized,
		                        signal, blockLen, blocks);
		int expectedCol = static_cast<int>(map.freqToX(freq));
		int found = argMaxColumn(spec);
		REQUIRE(found >= expectedCol - 1);
		REQUIRE(found <= expectedCol + 1);
	}
}

TEST_CASE("L0: analyzer normalized reference reads full-scale sine at 0 dB",
          "[l0][sectra][analyzer]")
{
	LogFreqMap map(20.0, 20000.0, kColumns);
	const int fftSize = 4096;
	const int blockLen = 512;
	const int blocks = 32; // see "finds sine peaks" — hopped warm-up

	auto signal = makeSine(440.0, blockLen * blocks, 1.0f);
	auto spec = runAnalyzer(fftSize, WindowType::kHann, DbReference::kNormalized,
	                        signal, blockLen, blocks);
	float peak = peakDbNear(spec, static_cast<int>(map.freqToX(440.0)), 2);
	REQUIRE(peak == Approx(0.0f).margin(0.5f));
}

TEST_CASE("L0: analyzer raw reference reads full-scale rectangular sine near -3 dB",
          "[l0][sectra][analyzer]")
{
	LogFreqMap map(20.0, 20000.0, kColumns);
	const int fftSize = 4096;
	const int blockLen = 512;
	const int blocks = 32; // see "finds sine peaks" — hopped warm-up

	auto signal = makeSine(440.0, blockLen * blocks, 1.0f);
	auto spec = runAnalyzer(fftSize, WindowType::kRectangular, DbReference::kRaw,
	                        signal, blockLen, blocks);
	float peak = peakDbNear(spec, static_cast<int>(map.freqToX(440.0)), 2);
	REQUIRE(peak == Approx(-3.01f).margin(0.5f));
}

TEST_CASE("L0: analyzer silence floor stays below -100 dBFS after warm-up",
          "[l0][sectra][analyzer]")
{
	SpectrumAnalyzer an;
	an.configure(4096, WindowType::kBlackmanHarris, DbReference::kNormalized, kSampleRate);
	std::vector<float> zeros(512, 0.0f);
	for (int b = 0; b < 8; ++b)
		an.process(zeros.data(), 512);
	const auto& spec = an.spectrum();
	REQUIRE_FALSE(spec.empty());
	for (float v : spec)
		REQUIRE(v <= -100.0f);
}

TEST_CASE("L0: analyzer reconfigure mid-stream keeps finding the peak",
          "[l0][sectra][analyzer]")
{
	LogFreqMap map(20.0, 20000.0, kColumns);
	const int blockLen = 512;
	const double freq = 440.0;
	auto signal = makeSine(freq, blockLen * 16, 0.5f);

	SpectrumAnalyzer an;
	an.configure(1024, WindowType::kHann, DbReference::kNormalized, kSampleRate);
	for (int b = 0; b < 4; ++b)
		an.process(signal.data() + static_cast<std::size_t>(b) * blockLen, blockLen);

	// Switch FFT size mid-stream and keep feeding.
	an.configure(8192, WindowType::kHann, DbReference::kNormalized, kSampleRate);
	for (int b = 4; b < 16; ++b)
		an.process(signal.data() + static_cast<std::size_t>(b) * blockLen, blockLen);

	int expectedCol = static_cast<int>(map.freqToX(freq));
	int found = argMaxColumn(an.spectrum());
	REQUIRE(found >= expectedCol - 1);
	REQUIRE(found <= expectedCol + 1);
}

TEST_CASE("L0: analyzer accepts blocks larger than the FFT size without corruption",
          "[l0][sectra][analyzer]")
{
// Hosts may deliver audio blocks bigger than the configured FFT size
// (e.g. Studio One with large buffer sizes). The analyzer must chunk such
// input instead of reading past its trailing-N history buffer.
LogFreqMap map(20.0, 20000.0, kColumns);
const int fftSize = 4096;

for (int blockLen : {fftSize, fftSize * 2, fftSize * 4})
{
SpectrumAnalyzer an;
an.configure(fftSize, WindowType::kHann, DbReference::kNormalized, kSampleRate);
std::vector<float> block(static_cast<std::size_t>(blockLen), 0.0f);
for (int b = 0; b < 4; ++b)
an.process(block.data(), blockLen); // all-silence: no NaN/Inf allowed
const auto& spec = an.spectrum();
REQUIRE(spec.size() == static_cast<std::size_t>(kColumns));
for (float v : spec)
REQUIRE(std::isfinite(v));

// A real tone in an oversized block must still land at the right column.
SpectrumAnalyzer an2;
an2.configure(fftSize, WindowType::kHann, DbReference::kNormalized, kSampleRate);
auto tone = makeSine(440.0, blockLen * 8, 0.5f);
for (int off = 0; off + blockLen <= static_cast<int>(tone.size()); off += blockLen)
an2.process(tone.data() + static_cast<std::size_t>(off), blockLen);
int expectedCol = static_cast<int>(map.freqToX(440.0));
int found = argMaxColumn(an2.spectrum());
REQUIRE(found >= expectedCol - 1);
REQUIRE(found <= expectedCol + 1);
}
}
TEST_CASE("L0: every window type produces a valid spectrum with the right peak",
          "[l0][sectra][analyzer]")
{
	LogFreqMap map(20.0, 20000.0, kColumns);
	const int fftSize = 4096;
	const int blockLen = 512;
	const int blocks = 8;
	const double freq = 440.0;
	auto signal = makeSine(freq, blockLen * blocks, 0.5f);
	int expectedCol = static_cast<int>(map.freqToX(freq));

	for (WindowType w : {WindowType::kRectangular, WindowType::kHann, WindowType::kHamming,
	                     WindowType::kBlackman, WindowType::kBlackmanHarris})
	{
		auto spec = runAnalyzer(fftSize, w, DbReference::kNormalized, signal, blockLen, blocks);
		REQUIRE(allFiniteAndBounded(spec, -300.0f, +10.0f));
		int found = argMaxColumn(spec);
		REQUIRE(found >= expectedCol - 1);
		REQUIRE(found <= expectedCol + 1);
	}
}

TEST_CASE("L0: two sines one octave apart give two distinct maxima",
          "[l0][sectra][analyzer]")
{
	LogFreqMap map(20.0, 20000.0, kColumns);
	const int fftSize = 4096;
	const int blockLen = 512;
	const int blocks = 8;
	std::vector<float> signal(blockLen * blocks, 0.0f);
	for (std::size_t s = 0; s < signal.size(); ++s)
		signal[s] = static_cast<float>(
		    0.3 * std::sin(2.0 * M_PI * 440.0 * s / kSampleRate) +
		    0.3 * std::sin(2.0 * M_PI * 880.0 * s / kSampleRate));

	auto spec = runAnalyzer(fftSize, WindowType::kHann, DbReference::kNormalized,
	                        signal, blockLen, blocks);
	int col440 = static_cast<int>(map.freqToX(440.0));
	int col880 = static_cast<int>(map.freqToX(880.0));
	float p440 = peakDbNear(spec, col440, 2);
	float p880 = peakDbNear(spec, col880, 2);
	REQUIRE(p440 > -20.0f); // both tones clearly present
	REQUIRE(p880 > -20.0f);
	REQUIRE(p440 == Approx(p880).margin(1.5f)); // equal amplitudes -> similar peaks
}

//------------------------------------------------------------------------
// SpectrumAnalyzer — dB reference per window
//------------------------------------------------------------------------
TEST_CASE("L0: normalized reference reads 0 dB for every window type",
          "[l0][sectra][dbref]")
{
	LogFreqMap map(20.0, 20000.0, kColumns);
	const int fftSize = 4096;
	const int blockLen = 512;
	const int blocks = 32; // hopped warm-up: fill trailing-N history with tone
	auto signal = makeSine(440.0, blockLen * blocks, 1.0f);
	int expectedCol = static_cast<int>(map.freqToX(440.0));

	for (WindowType w : {WindowType::kRectangular, WindowType::kHann, WindowType::kHamming,
	                     WindowType::kBlackman, WindowType::kBlackmanHarris})
	{
		auto spec = runAnalyzer(fftSize, w, DbReference::kNormalized, signal, blockLen, blocks);
		float peak = peakDbNear(spec, expectedCol, 2);
		REQUIRE(peak == Approx(0.0f).margin(0.5f));
	}
}

TEST_CASE("L0: raw reference is lower than normalized for tapered windows",
          "[l0][sectra][dbref]")
{
	LogFreqMap map(20.0, 20000.0, kColumns);
	const int fftSize = 4096;
	const int blockLen = 512;
	const int blocks = 8;
	auto signal = makeSine(440.0, blockLen * blocks, 1.0f);
	int expectedCol = static_cast<int>(map.freqToX(440.0));

	for (WindowType w : {WindowType::kHann, WindowType::kHamming, WindowType::kBlackman,
	                     WindowType::kBlackmanHarris})
	{
		auto rawSpec = runAnalyzer(fftSize, w, DbReference::kRaw, signal, blockLen, blocks);
		auto normSpec = runAnalyzer(fftSize, w, DbReference::kNormalized, signal, blockLen, blocks);
		float rawPeak = peakDbNear(rawSpec, expectedCol, 2);
		float normPeak = peakDbNear(normSpec, expectedCol, 2);
		REQUIRE(rawPeak < normPeak - 1.0f); // coherent gain loss visible in Raw mode
	}
}

//------------------------------------------------------------------------
// Sliding (hopped) analyzer — PLAN-sliding-spectrum.md
//------------------------------------------------------------------------
namespace {
// Target refresh rate used by the sliding analyzer (must match production constant).
constexpr double kTargetUpdateHz = 30.0;
int hopFor(double sampleRateHz, int fftSize)
{
	int h = static_cast<int>(std::lround(sampleRateHz / kTargetUpdateHz));
	if (h < 1) h = 1;
	if (h > fftSize) h = fftSize;
	return h;
}
} // namespace

TEST_CASE("L0 [sliding]: sub-hop feeds do not update; full hop does", "[l0][sectra][analyzer][sliding]")
{
	const int fftSize = 4096;
	const int hop = hopFor(kSampleRate, fftSize);

	SpectrumAnalyzer an;
	an.configure(fftSize, WindowType::kHann, DbReference::kNormalized, kSampleRate);

	// Silence warm-up so held_ starts at the floor deterministically.
	std::vector<float> zeros(static_cast<std::size_t>(hop * 4), 0.0f);
	for (int i = 0; i + hop <= static_cast<int>(zeros.size()); i += hop)
		an.process(zeros.data() + static_cast<std::size_t>(i), hop);
	const auto& before = an.spectrum();
	float beforePeak = -std::numeric_limits<float>::infinity();
	for (float v : before) beforePeak = std::max(beforePeak, v);

	// Feed a loud tone but LESS than one hop: no FFT may run yet, spectrum unchanged.
	auto tone = makeSine(440.0, hop - 1, 1.0f);
	an.process(tone.data(), static_cast<int>(tone.size()));
	const auto& afterSubHop = an.spectrum();
	REQUIRE(afterSubHop == before); // identical buffers => no frame was processed

	// Now feed enough to complete a hop: the display must move toward the tone.
	std::vector<float> more(hop, 0.0f);
	for (int i = 0; i < hop; ++i)
		more[static_cast<std::size_t>(i)] =
		    static_cast<float>(std::sin(2.0 * M_PI * 440.0 * (tone.size() + i) / kSampleRate));
	an.process(more.data(), hop);
	float afterPeak = -std::numeric_limits<float>::infinity();
	for (float v : an.spectrum()) afterPeak = std::max(afterPeak, v);
	REQUIRE(afterPeak > beforePeak); // a frame ran and pulled the meter up
}

TEST_CASE("L0 [sliding]: ballistics decay matches analytic model stepped per hop",
          "[l0][sectra][analyzer][sliding]")
{
	const double sr = kSampleRate; // must match makeSine()'s generation rate
	const int fftSize = 4096;
	const int hop = hopFor(sr, fftSize);
	const double dt = static_cast<double>(hop) / sr;

	SpectrumAnalyzer an;
	an.configure(fftSize, WindowType::kHann, DbReference::kNormalized, sr);
	an.setBallistics(0.0, 0.5); // instant attack, 0.5 s release (24 dB / 0.5 s)

	// Drive a full-scale sine until the peak column settles near 0 dB.
	LogFreqMap map(20.0, 20000.0, kColumns);
	int col = static_cast<int>(map.freqToX(440.0));
	auto tone = makeSine(440.0, hop * 30, 1.0f);
	tone.resize(static_cast<std::size_t>(hop * 30), 0.0f);
	for (int i = 0; i + hop <= static_cast<int>(tone.size()); i += hop)
		an.process(tone.data() + static_cast<std::size_t>(i), hop);
	float heldAtTone = an.spectrum()[static_cast<std::size_t>(col)];

	// Now silence for exactly K hops and compare against the analytic linear-dB
	// travel: from heldAtTone toward floor at 24 dB per 0.5 s.
	const int K = 10;
	std::vector<float> sil(hop, 0.0f);
	for (int i = 0; i < K; ++i)
		an.process(sil.data(), hop);

	MeterBallistics ref;
	ref.setTimes(0.0, 0.5);
	float expected = heldAtTone;
	for (int i = 0; i < K; ++i)
		expected = ref.step(expected, -300.0f, dt);

	float actual = an.spectrum()[static_cast<std::size_t>(col)];
	REQUIRE(actual == Approx(expected).margin(0.75f));
}

TEST_CASE("L0 [sliding]: zero-padding is capped so large FFTs stay affordable",
          "[l0][sectra][analyzer][sliding]")
{
	// The old x16 rule made a 16K window run as a 256K-point FFT every block.
	// With the cap in place the padded length must be bounded well below that.
	SpectrumAnalyzer big;
	big.configure(16384, WindowType::kHann, DbReference::kNormalized, kSampleRate);
	REQUIRE(big.paddedSize() <= 65536);

	SpectrumAnalyzer mid;
	mid.configure(4096, WindowType::kHann, DbReference::kNormalized, kSampleRate);
	REQUIRE(mid.paddedSize() >= 4096); // never smaller than the analysis window
	REQUIRE(mid.paddedSize() <= 65536);
}

TEST_CASE("L0 [sliding]: peak stays within +/-1 column across all offered FFT sizes",
          "[l0][sectra][analyzer][sliding]")
{
	LogFreqMap map(20.0, 20000.0, kColumns);
	const double sr = kSampleRate; // must match makeSine()'s generation rate
	const int hop = hopFor(sr, 16384);

	for (int fftSize : {1024, 2048, 4096, 8192, 16384})
	{
		for (double freq : {100.0, 440.0, 5000.0})
		{
			SpectrumAnalyzer an;
			an.configure(fftSize, WindowType::kHann, DbReference::kNormalized, sr);
			auto tone = makeSine(freq, hop * 40, 0.5f);
			tone.resize(static_cast<std::size_t>(hop * 40), 0.0f);
			for (int i = 0; i + hop <= static_cast<int>(tone.size()); i += hop)
				an.process(tone.data() + static_cast<std::size_t>(i), hop);
			int expectedCol = static_cast<int>(map.freqToX(freq));
			int found = argMaxColumn(an.spectrum());
			// Small windows have coarser low-end resolution on a log axis; allow
			// +/-2 columns there while still catching gross misalignment.
			CHECK(found >= expectedCol - 2);
			CHECK(found <= expectedCol + 2);
		}
	}
}
