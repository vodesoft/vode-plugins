// Vode Plugins — L0 tests: AnalysisWorker (off-thread spectrum analysis).
//
// Regression guard for the "playback speed changes with FFT size" bug: the
// audio-thread entry point (feed) must stay O(numSamples) no matter how large
// the FFT is, while all FFT work happens on the worker thread.

#include <catch2/catch_test_macros.hpp>

#include "vdplg/analysisworker.h"
#include "vdplg/spectrum.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

using namespace vdplg::spectrum;

namespace {

constexpr double kSampleRate = 48000.0;

std::vector<float> sine(int numSamples, double freqHz, float amp, double sampleRate)
{
	std::vector<float> out(static_cast<size_t>(numSamples));
	const double w = 2.0 * M_PI * freqHz / sampleRate;
	for (int i = 0; i < numSamples; ++i)
		out[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(w * i));
	return out;
}

// Poll until pred() is true or timeout elapses. Returns true if satisfied.
bool waitFor(std::function<bool()> pred, int timeoutMs = 3000)
{
	const auto deadline = std::chrono::steady_clock::now() +
	                      std::chrono::milliseconds(timeoutMs);
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (pred()) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return pred();
}

AnalysisWorker::Config makeConfig(ChannelMode mode, int fftSize)
{
	AnalysisWorker::Config cfg;
	cfg.fftSize = fftSize;
	cfg.window = WindowType::kBlackmanHarris;
	cfg.ref = DbReference::kNormalized;
	cfg.sampleRateHz = kSampleRate;
	cfg.mode = mode;
	cfg.attackSec = 0.0;   // instant attack so levels settle quickly in tests
	cfg.releaseSec = 0.05;
	return cfg;
}

double peakOf(const std::vector<float>& v)
{
	double best = -999.0;
	for (size_t c = 0; c < v.size(); ++c)
		best = std::max(best, static_cast<double>(v[c]));
	return best;
}

} // namespace

TEST_CASE("AnalysisWorker: feed produces spectrum energy at expected frequency", "[l0][analysisworker]")
{
	AnalysisWorker worker;
	worker.start(makeConfig(ChannelMode::kLR, 1024));

	const int blockLen = 1024;
	auto left = sine(blockLen, 1000.0, 0.5f, kSampleRate);
	auto right = sine(blockLen, 1000.0, 0.5f, kSampleRate);

	bool sawEnergy = false;
	for (int n = 0; n < 64 && !sawEnergy; ++n)
	{
		worker.feed(left.data(), right.data(), blockLen);
		sawEnergy = waitFor([&] {
			return peakOf(worker.spectrumA()) > -40.0;
		}, 2000);
	}
	worker.stop();

	REQUIRE(sawEnergy);
}

TEST_CASE("AnalysisWorker: sink always delivers same-length arrays across live mode switch", "[l0][analysisworker]")
{
	// Regression guard for the Studio One crash (AV in pushScopeToExchange):
	// switching INTO M/(M-S) while audio flows used to deliver callbacks whose
	// balance[] pointed at an empty buffer (lastBalance_ only filled once the
	// worker applied the new mode), so the processor indexed past its end.
	// Invariant under test: EVERY sink call sees a/b/balance of identical size.
	AnalysisWorker worker;
	auto cfg = makeConfig(ChannelMode::kLR, 1024); // start in L/R
	cfg.attackSec = 0.0;
	cfg.releaseSec = 0.05;
	worker.start(cfg, /*queueCapacity=*/8);

	std::atomic<int> calls{0};
	// Per callback: {numCols delivered to sink, balanceNumCols() at that instant}.
	std::vector<std::pair<int, int>> perCall;
	std::mutex m;
	worker.setScopeSink([&](const float* a, const float* b, const float* bal, int n) {
		std::lock_guard<std::mutex> lk(m);
		perCall.emplace_back(n, worker.balanceNumCols());
		calls.fetch_add(1);
	});

	const int blockLen = 1024;
	auto left = sine(blockLen, 1000.0, 0.5f, kSampleRate);
	auto right = sine(blockLen, 1000.0, 0.5f, kSampleRate);

	// Steady state in L/R — several callbacks must have fired with full spectra.
	bool steady = false;
	for (int n = 0; n < 64 && !steady; ++n)
	{
		worker.feed(left.data(), right.data(), blockLen);
		steady = waitFor([&] { return calls.load() >= 4; }, 2000);
	}
	REQUIRE(steady);

	// Live switch INTO M/(M-S). The transition window (callbacks still running
	// under the old mode) is where the empty-balance bug lived.
	auto cfg2 = makeConfig(ChannelMode::kMBalance, 1024);
	cfg2.attackSec = 0.0;
	cfg2.releaseSec = 0.05;
	worker.start(cfg2);

	for (int n = 0; n < 96; ++n)
		worker.feed(left.data(), right.data(), blockLen);

	// Wait until balance data is actually flowing (post-transition), then stop.
	bool balanceLive = waitFor([&] {
		return worker.balanceNumCols() > 0;
	}, 3000);
	worker.stop();
	REQUIRE(balanceLive);

	// Invariant: whenever the sink was told there are N columns, the balance
	// buffer held exactly N entries too — otherwise pushScopeToExchange would
	// index past its end (the Studio One AV on switching into M/(M-S)).
	int checked = 0;
	{
		std::lock_guard<std::mutex> lk(m);
		for (const auto& p : perCall)
		{
			CHECK(p.first == p.second); // <-- fails pre-fix during LR->balance window
			if (p.first > 0)
				checked++;
		}
	}
	REQUIRE(checked > 0);
}

TEST_CASE("AnalysisWorker: feed never blocks when queue is full (drop-oldest)", "[l0][analysisworker]")
{
	AnalysisWorker worker;
	// Large FFT => slow consumer; small queue capacity => guaranteed overflow.
	worker.start(makeConfig(ChannelMode::kLR, 16384), /*queueCapacity=*/2);

	const int blockLen = 128;
	std::vector<float> l(blockLen, 0.0f), r(blockLen, 0.0f);

	// Hammer feed() far beyond capacity without any processing time on this
	// thread. If feed ever blocked or grew unboundedly this would hang/OOM.
	const auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < 4096; ++i)
		worker.feed(l.data(), r.data(), blockLen);
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - t0).count();

	worker.stop();

	// 4096 non-blocking pushes of 128 samples must complete in well under a second.
	REQUIRE(elapsed < 1000);
}

TEST_CASE("AnalysisWorker: audio-thread cost stays O(numSamples) for large FFT", "[l0][analysisworker]")
{
	AnalysisWorker worker;
	worker.start(makeConfig(ChannelMode::kLR, 16384)); // worst-case FFT size

	const int blockLen = 128;
	auto left = sine(blockLen, 440.0, 0.5f, kSampleRate);
	auto right = sine(blockLen, 440.0, 0.5f, kSampleRate);

	// Warm up.
	for (int i = 0; i < 16; ++i)
		worker.feed(left.data(), right.data(), blockLen);

	const int iters = 2000;
	const auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < iters; ++i)
		worker.feed(left.data(), right.data(), blockLen);
	const double perFeedUs =
		std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0)
			.count() / iters;

	worker.stop();

	// A single padded-FFT frame at N=262144 costs tens of ms; one feed() call
	// must be orders of magnitude cheaper than that — it may only copy+enqueue.
	// Budget: 50 us per feed is generous for a 128-sample memcpy + lock push.
	REQUIRE(perFeedUs < 50.0);
}

TEST_CASE("AnalysisWorker: M/(M-S) mode produces clamped balance values", "[l0][analysisworker]")
{
	AnalysisWorker worker;
	worker.start(makeConfig(ChannelMode::kMBalance, 1024));

	const int blockLen = 1024;
	// Identical L/R (mono) => mid full-strength, side ~0 => M-S near +12 clamp.
	auto left = sine(blockLen, 1000.0, 0.6f, kSampleRate);
	auto right = left; // same signal on both channels

	bool sawBalance = false;
	for (int n = 0; n < 64 && !sawBalance; ++n)
	{
		worker.feed(left.data(), right.data(), blockLen);
		sawBalance = waitFor([&] {
			const auto& bal = worker.balanceDb();
			if (bal.empty()) return false;
			return peakOf(bal) > 8.0; // close to the +12 clamp
		}, 2000);
	}
	worker.stop();

	REQUIRE(sawBalance);
}

TEST_CASE("AnalysisWorker: start/stop lifecycle is idempotent and clean", "[l0][analysisworker]")
{
	AnalysisWorker worker;
	REQUIRE_FALSE(worker.running());

	worker.start(makeConfig(ChannelMode::kLR, 1024));
	REQUIRE(worker.running());

	// Double start must be harmless.
	worker.start(makeConfig(ChannelMode::kLR, 1024));
	REQUIRE(worker.running());

	worker.stop();
	REQUIRE_FALSE(worker.running());

	// Double stop must be harmless.
	worker.stop();
	REQUIRE_FALSE(worker.running());

	// Restart after stop works.
	worker.start(makeConfig(ChannelMode::kMS, 1024));
	REQUIRE(worker.running());
	worker.stop();
	REQUIRE_FALSE(worker.running());
}

TEST_CASE("AnalysisWorker: live reconfigure applies ballistics to running analyzers", "[l0][analysisworker]")
{
	AnalysisWorker worker;
	auto cfg = makeConfig(ChannelMode::kLR, 1024);
	cfg.attackSec = 0.0;
	cfg.releaseSec = 0.05; // fast release
	worker.start(cfg);

	const int blockLen = 1024;
	auto left = sine(blockLen, 1000.0, 0.5f, kSampleRate);
	std::vector<float> right(left.size(), 0.0f);

	// Build up level on scope A.
	bool builtUp = false;
	for (int n = 0; n < 64 && !builtUp; ++n)
	{
		worker.feed(left.data(), right.data(), blockLen);
		builtUp = waitFor([&] {
			return peakOf(worker.spectrumA()) > -30.0;
		}, 2000);
	}
	REQUIRE(builtUp);

	// Switch to a very slow release while running. If the reconfigure is NOT
	// applied, the old 50 ms release decays the level below -30 dB within a few
	// blocks of silence; with a 5 s release it stays high for hundreds of blocks.
	auto cfg2 = cfg;
	cfg2.releaseSec = 5.0;
	worker.start(cfg2); // must apply in place

	std::vector<float> silent(blockLen, 0.0f);
	int fedSilent = 0;
	bool stayedHigh = false;
	for (; fedSilent < 400; ++fedSilent)
	{
		worker.feed(silent.data(), silent.data(), blockLen);
		stayedHigh = waitFor([&] {
			return peakOf(worker.spectrumA()) > -30.0;
		}, 500);
		if (!stayedHigh)
			break;
	}
	worker.stop();

	// With a 5 s release, 400 blocks (~8.5 s total at 48 kHz/1024) far exceeds
	// any fast-release decay.
	REQUIRE(stayedHigh);
}

TEST_CASE("AnalysisWorker: live mode switch changes channel mixing", "[l0][analysisworker]")
{
	AnalysisWorker worker;
	auto cfg = makeConfig(ChannelMode::kLR, 1024); // start in L/R
	cfg.attackSec = 0.0;
	cfg.releaseSec = 0.05;
	worker.start(cfg);

	// L carries a tone, R is silence. In L/R mode scope A = L (loud), scope B = R (silent).
	const int blockLen = 1024;
	auto left = sine(blockLen, 1000.0, 0.5f, kSampleRate);
	std::vector<float> right(blockLen, 0.0f);

	// Establish steady state in L/R: A loud, B silent.
	bool lrSteady = false;
	for (int n = 0; n < 64 && !lrSteady; ++n)
	{
		worker.feed(left.data(), right.data(), blockLen);
		lrSteady = waitFor([&] { return peakOf(worker.spectrumA()) > -30.0; }, 2000);
	}
	REQUIRE(lrSteady);
	REQUIRE(peakOf(worker.spectrumB()) < -70.0); // R channel is silence

	// Live switch to M/S while running. Now side = (L-R)/2 ≈ full tone, so scope B
	// MUST become loud. If the reconfigure does not reach the analyzers, B stays silent.
	auto cfg2 = makeConfig(ChannelMode::kMS, 1024);
	cfg2.attackSec = 0.0;
	cfg2.releaseSec = 0.05;
	worker.start(cfg2);

	bool sawEnergy = false;
	for (int n = 0; n < 64 && !sawEnergy; ++n)
	{
		worker.feed(left.data(), right.data(), blockLen);
		sawEnergy = waitFor([&] { return peakOf(worker.spectrumB()) > -30.0; }, 2000);
	}
	worker.stop();

	REQUIRE(sawEnergy);
}

