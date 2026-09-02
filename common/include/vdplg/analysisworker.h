// Vode Plugins — AnalysisWorker: off-thread spectrum analysis.
//
// Moves all FFT / ballistics work off the audio thread onto a dedicated
// background thread fed by a bounded single-producer/single-consumer queue.
// The audio thread only performs an O(numSamples) copy + enqueue in feed().
//
// This class is intentionally free of any VST3 SDK dependency so it can be
// unit-tested directly from the L0 Catch2 suite.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "vdplg/spectrum.h"

namespace vdplg {
namespace spectrum {

class AnalysisWorker
{
public:
	struct Config
	{
		int fftSize = 4096;
		WindowType window = WindowType::kBlackmanHarris;
		DbReference ref = DbReference::kNormalized;
		double sampleRateHz = 48000.0;
		ChannelMode mode = ChannelMode::kLR;
		double attackSec = 0.0;
		double releaseSec = 0.1;
	};

	// Invoked on the worker thread after each processed block. `a`/`b` are the
	// two smoothed spectra; `balance` holds the clamped ±12 dB mid-minus-side
	// difference (valid only in kMBalance mode). All arrays have numCols entries.
	using ScopeSink = std::function<void(const float* a, const float* b,
	                                     const float* balance, int numCols)>;

	AnalysisWorker();
	~AnalysisWorker();

	// Non-copyable, non-movable (owns a thread).
	AnalysisWorker(const AnalysisWorker&) = delete;
	AnalysisWorker& operator=(const AnalysisWorker&) = delete;

	/// Start (or restart) the worker with the given configuration.
	/// If already running, reconfigures analyzers in-place without restarting.
	void start(const Config& cfg, int queueCapacity = 64);

	/// Stop the worker thread and clear pending blocks. Idempotent.
	void stop();

	bool running() const;

	/// Audio-thread entry point. Copies L/R samples into the bounded queue
	/// and returns immediately. If the queue is full the oldest pending block
	/// is dropped. Never blocks. Safe to call when not running (no-op).
	void feed(const float* left, const float* right, int numSamples);

	/// Set the callback invoked on the worker thread after each processed
	/// block delivers its scope snapshot. Must be called before start().
	void setScopeSink(ScopeSink sink);

	//--- Test / introspection accessors (worker-thread state) --------------
	const std::vector<float>& spectrumA() const;
	const std::vector<float>& spectrumB() const;
	const std::vector<float>& balanceDb() const;
	/// Number of columns currently held in the balance snapshot. Must equal
	/// numCols delivered to the sink once any spectrum exists (regression guard
	/// for the M/(M-S) mode-switch crash where balance[] was still empty).
	int balanceNumCols() const;

private:
	void loop();
	void processBlock(const std::vector<float>& l, const std::vector<float>& r);
	// Rebuild analyzer buffers/ballistics from the current config. Called ONLY on
	// the worker thread so analyzer state is never mutated concurrently with
	// processBlock(). Caller must hold mutex_.
	void applyConfigLocked (const Config& cfg); // caller holds mutex_
	// If a config was posted by start(), apply it now (worker thread only).
	bool maybeApplyPendingConfig ();            // caller holds mutex_

	Config config_;
	// Last structurally-applied config + whether any config has been applied
	// yet. Used to decide if a reconfigure needs a full analyzer rebuild (which
	// resets meter state) or can refresh ballistics in place without wiping the
	// visible spectrum.
	Config lastCfg_ {};
	bool haveAppliedCfg_{false};
	std::atomic<bool> pendingConfig_{false};
	SpectrumAnalyzer analyzerA_;
	SpectrumAnalyzer analyzerB_;

	// Bounded SPSC queue (mutex-guarded for simplicity; contention is minimal
	// because the producer only pushes once per audio block).
	std::deque<std::pair<std::vector<float>, std::vector<float>>> queue_;
	int queueCapacity_{64};
	std::mutex mutex_;
	std::condition_variable cv_;

	std::thread thread_;
	std::atomic<bool> running_{false};
	std::atomic<bool> exitRequested_{false};

	ScopeSink sink_;

	// Latest computed snapshots (for test introspection + sink delivery).
	std::vector<float> lastA_;
	std::vector<float> lastB_;
	std::vector<float> lastBalance_;
};

} // namespace spectrum
} // namespace vdplg
