// Vode Plugins — L0 tests: Signalsmith DSP building blocks + vdplg helpers.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "signalsmith-dsp/filters.h"
#include "signalsmith-dsp/envelopes.h"
#include "signalsmith-dsp/delay.h"

#include "vdplg/dsp.h"

#include <cmath>
#include <vector>

using namespace signalsmith::filters;
using namespace signalsmith::envelopes;
using namespace signalsmith::delay;
using Catch::Approx;

TEST_CASE("L0: dB <-> linear round-trip", "[l0][dsp]")
{
	for (double db : {-60.0, -12.0, -6.0, -3.0, 0.0, 3.0, 6.0, 12.0, 24.0})
	{
		double lin = vdplg::dbToLinear(db);
		REQUIRE(vdplg::linearToDb(lin) == Approx(db).margin(1e-9));
	}
}

TEST_CASE("L0: dB normalization is monotonic and clamped", "[l0][dsp]")
{
	const double minDb = -60.0, maxDb = 24.0;
	REQUIRE(vdplg::dbToNormalized(minDb, minDb, maxDb) == Approx(0.0));
	REQUIRE(vdplg::dbToNormalized(maxDb, minDb, maxDb) == Approx(1.0));
	REQUIRE(vdplg::dbToNormalized(-18.0, minDb, maxDb) == Approx(0.5));
	REQUIRE(vdplg::dbToNormalized(-100.0, minDb, maxDb) == Approx(0.0)); // clamp low
	REQUIRE(vdplg::dbToNormalized(100.0, minDb, maxDb) == Approx(1.0));  // clamp high

	// round trip
	for (double n : {0.0, 0.25, 0.5, 0.75, 1.0})
	{
		double db = vdplg::normalizedToDb(n, minDb, maxDb);
		REQUIRE(vdplg::dbToNormalized(db, minDb, maxDb) == Approx(n).margin(1e-9));
	}
}

TEST_CASE("L0: biquad peak filter response matches design gain at center", "[l0][biquad]")
{
	// 1 kHz peak, +6 dB, 1 octave bandwidth, 44.1 kHz sample rate.
	const double sampleRate = 44100.0;
	const double freqHz = 1000.0;
	const double gainDb = 6.0;

	BiquadStatic<float> bq;
	// NOTE: peak() takes LINEAR gain; peakDb() takes dB.
	bq.peakDb(freqHz / sampleRate, gainDb, 1.0);

	// At the center frequency the response must equal the design gain.
	double atCenter = bq.responseDb(static_cast<float>(freqHz / sampleRate));
	REQUIRE(atCenter == Approx(gainDb).margin(0.1));

	// Far from center the response should be near 0 dB (within a few dB).
	double farAway = bq.responseDb(static_cast<float>(100.0 / sampleRate));
	REQUIRE(std::abs(farAway) < 3.0);
}

TEST_CASE("L0: biquad lowpass attenuates high frequencies", "[l0][biquad]")
{
	const double sampleRate = 44100.0;

	BiquadStatic<float> lp;
	lp.lowpass(1000.0 / sampleRate);

	double passband = lp.responseDb(static_cast<float>(100.0 / sampleRate));
	double stopband = lp.responseDb(static_cast<float>(10000.0 / sampleRate));

	REQUIRE(passband > -1.0);   // ~0 dB below cutoff
	REQUIRE(stopband < -10.0);  // strong attenuation above cutoff
	REQUIRE(stopband < passband);
}

TEST_CASE("L0: box filter converges to steady-state mean of constant input", "[l0][boxfilter]")
{
	BoxFilter<double> box(64);
	box.reset();

	double out = 0.0;
	for (int i = 0; i < 1024; ++i)
		out = box(1.0);

	// Moving average of a constant signal equals that constant once filled.
	REQUIRE(out == Approx(1.0).margin(1e-12));
}

TEST_CASE("L0: delay line round-trip returns delayed input", "[l0][delay]")
{
	Delay<float> d(1024);

	const int N = 512;
	std::vector<float> in(N), out(N);
	for (int i = 0; i < N; ++i)
		in[i] = static_cast<float>(i % 7) - 3.0f;

	// Prime the line with silence so reads before writes are defined.
	for (int i = 0; i < 1024; ++i)
		d.write(0.0f);

	const int delaySamples = 16;
	for (int i = 0; i < N; ++i)
	{
		d.write(in[i]);
		out[i] = d.read(delaySamples);
	}

	// After the initial transient, output must equal input shifted by `delaySamples`.
	for (int i = delaySamples; i < N; ++i)
	{
		REQUIRE(out[i] == Approx(in[i - delaySamples]).epsilon(1e-6f));
	}
}
