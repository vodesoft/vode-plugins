// Vode Plugins — AnalysisWorker implementation.
//
// All heavy DSP (windowing, zero-padded FFT, ballistics) runs on the worker
// thread. The audio thread only copies samples into a bounded queue via
// feed(), which is O(numSamples) regardless of FFT size.

#include "vdplg/analysisworker.h"

namespace vdplg {
namespace spectrum {

//------------------------------------------------------------------------
AnalysisWorker::AnalysisWorker () = default;

AnalysisWorker::~AnalysisWorker ()
{
	stop ();
}

//------------------------------------------------------------------------
void AnalysisWorker::start (const Config& cfg, int queueCapacity)
{
	std::lock_guard<std::mutex> lock (mutex_);
	queueCapacity_ = (queueCapacity > 0) ? queueCapacity : 64;

	if (running_.load ())
	{
		// Already running: post the new config for the worker to apply at the top
		// of its next iteration, where it has exclusive access to the analyzers.
		config_ = cfg;
		pendingConfig_ = true;
		cv_.notify_one ();
		return;
	}

	config_ = cfg;
	exitRequested_ = false;
	applyConfigLocked (cfg);
	pendingConfig_ = false;
	running_ = true;
	thread_ = std::thread ([this] { loop (); });
}

//------------------------------------------------------------------------
void AnalysisWorker::stop ()
{
	bool wasRunning = running_.exchange (false);
	exitRequested_ = true;
	cv_.notify_all ();
	if (wasRunning && thread_.joinable ())
		thread_.join ();
	std::lock_guard<std::mutex> lock (mutex_);
	queue_.clear ();
}

//------------------------------------------------------------------------
bool AnalysisWorker::running () const
{
	return running_.load ();
}

//------------------------------------------------------------------------
void AnalysisWorker::feed (const float* left, const float* right, int numSamples)
{
	if (!left || !right || numSamples <= 0)
		return;
	std::vector<float> l (left, left + numSamples);
	std::vector<float> r (right, right + numSamples);

	{
		std::lock_guard<std::mutex> lock (mutex_);
		if (!running_.load ())
			return; // not active — drop silently
		while (static_cast<int> (queue_.size ()) >= queueCapacity_)
			queue_.pop_front (); // drop-oldest: keep freshest data for the meter
		queue_.emplace_back (std::move (l), std::move (r));
	}
	cv_.notify_one ();
}

//------------------------------------------------------------------------
void AnalysisWorker::setScopeSink (ScopeSink sink)
{
	sink_ = std::move (sink);
}

//------------------------------------------------------------------------
const std::vector<float>& AnalysisWorker::spectrumA () const
{
	return lastA_;
}

const std::vector<float>& AnalysisWorker::spectrumB () const
{
	return lastB_;
}

const std::vector<float>& AnalysisWorker::balanceDb () const
{
	return lastBalance_;
}

int AnalysisWorker::balanceNumCols () const
{
	return static_cast<int> (lastBalance_.size ());
}

//------------------------------------------------------------------------
void AnalysisWorker::applyConfigLocked (const Config& cfg)
{
	// A full configure() unconditionally resets held/display to the floor, so we
	// only do one when a *structural* parameter changed (or on first apply).
	// Ballistics/mode-only tweaks refresh timing in place and leave the visible
	// spectrum untouched — otherwise nudging Attack/Release would dip the meter.
	const bool structuralChanged = !haveAppliedCfg_ ||
	    (lastCfg_.fftSize != cfg.fftSize) ||
	    (lastCfg_.window != cfg.window) ||
	    (lastCfg_.ref != cfg.ref) ||
	    (lastCfg_.sampleRateHz != cfg.sampleRateHz);
	if (structuralChanged)
	{
		analyzerA_.configure (cfg.fftSize, cfg.window, cfg.ref, cfg.sampleRateHz);
		analyzerB_.configure (cfg.fftSize, cfg.window, cfg.ref, cfg.sampleRateHz);
	}
	else
	{
		analyzerA_.setBallistics (cfg.attackSec, cfg.releaseSec);
		analyzerB_.setBallistics (cfg.attackSec, cfg.releaseSec);
	}
	lastCfg_ = cfg;
	haveAppliedCfg_ = true;
}

//------------------------------------------------------------------------
void AnalysisWorker::loop ()
{
	for (;;)
	{
		std::pair<std::vector<float>, std::vector<float>> block;
		bool haveBlock = false;
		{
			std::unique_lock<std::mutex> lock (mutex_);
			cv_.wait (lock, [this] {
				return exitRequested_.load () || !queue_.empty () ||
				       pendingConfig_.load ();
			});
			if (maybeApplyPendingConfig ())
				continue; // analyzers just reset — pick up fresh work
			if (exitRequested_.load () && queue_.empty ())
				break;
			if (!queue_.empty ())
			{
				block = std::move (queue_.front ());
				queue_.pop_front ();
				haveBlock = true;
			}
		}
		if (haveBlock)
			processBlock (block.first, block.second);
	}
}

//------------------------------------------------------------------------
bool AnalysisWorker::maybeApplyPendingConfig ()
{
	if (!pendingConfig_.exchange (false))
		return false;
	applyConfigLocked (config_);
	return true;
}

//------------------------------------------------------------------------
void AnalysisWorker::processBlock (const std::vector<float>& l, const std::vector<float>& r)
{
	const int n = static_cast<int> (l.size ());
	if (n <= 0)
		return;

	// Mix channels per mode into the two analyzer inputs.
	std::vector<float> mixA (static_cast<size_t> (n));
	std::vector<float> mixB (static_cast<size_t> (n));
	switch (config_.mode)
	{
		case ChannelMode::kLR:
			mixA = l;
			mixB = r;
			break;
		case ChannelMode::kMS:
		case ChannelMode::kMBalance:
			for (int i = 0; i < n; ++i)
			{
				mixA[static_cast<size_t> (i)] = 0.5f * (l[static_cast<size_t> (i)] +
				                                        r[static_cast<size_t> (i)]);
				mixB[static_cast<size_t> (i)] = 0.5f * (l[static_cast<size_t> (i)] -
				                                        r[static_cast<size_t> (i)]);
			}
			break;
	}

	analyzerA_.process (mixA.data (), n);
	analyzerB_.process (mixB.data (), n);

	lastA_ = analyzerA_.spectrum ();
	lastB_ = analyzerB_.spectrum ();
	// ALWAYS keep the balance snapshot in lockstep with the spectra, even when
	// not in M/(M-S) mode. The sink delivers all three arrays with one numCols,
	// so if lastBalance_ lagged behind (e.g. still empty right after a live
	// switch INTO kMBalance), the processor would index past its end → AV.
	// Computing it unconditionally costs O(numCols) per block — negligible.
	lastBalance_ = balancediff::diff (analyzerA_.spectrum (),
	                                  analyzerB_.spectrum ()).db;

	if (sink_)
		sink_ (lastA_.data (), lastB_.data (), lastBalance_.data (),
		       static_cast<int> (lastA_.size ()));
}

} // namespace spectrum
} // namespace vdplg
