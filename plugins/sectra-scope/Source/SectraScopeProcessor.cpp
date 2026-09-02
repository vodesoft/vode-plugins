// Vode Plugins — Sectra Scope: audio processor implementation.
//
// Phase 2 (TDD Red): pure passthrough + six-parameter plumbing. The two
// SpectrumAnalyzers are wired into process() in Phase 3 (Green).

#include "SectraScopeProcessor.h"
#include "sectracids.h"
#include "sectraparamids.h"

#include "public.sdk/source/vst/vsthelpers.h"
#include "public.sdk/source/vst/vstaudioprocessoralgo.h"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include "base/source/fstreamer.h"

#include <cmath>
#include <cstring>

namespace vdplg {
namespace sectrascope {

using namespace Steinberg;
using namespace Steinberg::Vst;

//------------------------------------------------------------------------
Processor::Processor ()
	: scopeExchange_ (this, [this] (auto& config, const auto&) {
		  config.blockSize = scopeDataSize ();
		  config.numBlocks = 8;
		  config.userContextID = static_cast<DataExchangeUserContextID> (kScopeQueueId);
		  return true;
	  })
{
	setControllerClass (kControllerUID);

	fftSizeParam_.setParamID (kFFTSizeId);
	fftSizeParam_.setValue (fftSizeToNormalized (kFftSizes[kDefaultFftSizeIndex]));

	windowTypeParam_.setParamID (kWindowTypeId);
	windowTypeParam_.setValue (static_cast<ParamValue> (kDefaultWindowIndex) /
	                           static_cast<ParamValue> (kNumWindows - 1));

	modeParam_.setParamID (kModeId);
	modeParam_.setValue (static_cast<ParamValue> (kDefaultModeIndex) /
	                     static_cast<ParamValue> (kNumModes - 1));

	attackParam_.setParamID (kAttackId);
	attackParam_.setValue (attackMsToNormalized (0.0)); // default: instant

	releaseParam_.setParamID (kReleaseId);
	releaseParam_.setValue (releaseMsToNormalized (100.0)); // default: 100 ms

	dbRefParam_.setParamID (kDbRefId);
	dbRefParam_.setValue (static_cast<ParamValue> (kDefaultDbRefIndex) /
	                      static_cast<ParamValue> (kNumDbRefs - 1));

	// Worker delivers each processed snapshot on its own thread; we forward it
	// into the data-exchange queue for the controller.
	analysis_.setScopeSink ([this] (const float* a, const float* b,
	                                const float* balance, int numCols) {
		pushScopeToExchange (a, b, balance, numCols);
	});

	resyncParams ();
}

//------------------------------------------------------------------------
void Processor::resyncParams ()
{
	fftSize_ = fftSizeFromNormalized (fftSizeParam_.getValue ());
	windowIndex_ = static_cast<int> (std::lround (windowTypeParam_.getValue () *
	                                             (kNumWindows - 1)));
	if (windowIndex_ < 0) windowIndex_ = 0;
	if (windowIndex_ >= kNumWindows) windowIndex_ = kNumWindows - 1;

	modeIndex_ = static_cast<int> (std::lround (modeParam_.getValue () * (kNumModes - 1)));
	if (modeIndex_ < 0) modeIndex_ = 0;
	if (modeIndex_ >= kNumModes) modeIndex_ = kNumModes - 1;

	attackMs_ = attackMsFromNormalized (attackParam_.getValue ());
	releaseMs_ = releaseMsFromNormalized (releaseParam_.getValue ());

	dbRefIndex_ = static_cast<int> (std::lround (dbRefParam_.getValue () * (kNumDbRefs - 1)));
	if (dbRefIndex_ < 0) dbRefIndex_ = 0;
	if (dbRefIndex_ >= kNumDbRefs) dbRefIndex_ = kNumDbRefs - 1;
}

//------------------------------------------------------------------------
tresult PLUGIN_API Processor::initialize (FUnknown* context)
{
	tresult result = AudioEffect::initialize (context);
	if (result != kResultOk)
		return result;

	addAudioInput (STR16 ("Stereo In"), SpeakerArr::kStereo);
	addAudioOutput (STR16 ("Stereo Out"), SpeakerArr::kStereo);

	return kResultOk;
}

//------------------------------------------------------------------------
void Processor::syncAnalyzers (double sampleRateHz)
{
	using namespace vdplg::spectrum;
	const bool changed = (cfgFftSize_ != fftSize_) ||
	                     (cfgWindowIndex_ != windowIndex_) ||
	                     (cfgModeIndex_ != modeIndex_.load ()) ||
	                     (cfgAttackMs_ != attackMs_) ||
	                     (cfgReleaseMs_ != releaseMs_) ||
	                     (cfgDbRefIndex_ != dbRefIndex_) ||
	                     (cfgSampleRate_ != sampleRateHz);
	if (!changed)
		return;

	cfgFftSize_ = fftSize_;
	cfgWindowIndex_ = windowIndex_;
	cfgModeIndex_ = modeIndex_.load ();
	cfgAttackMs_ = attackMs_;
	cfgReleaseMs_ = releaseMs_;
	cfgDbRefIndex_ = dbRefIndex_;
	cfgSampleRate_ = sampleRateHz;

	// Start or reconfigure the off-thread worker. If it is already running this
	// applies the new config in place (no thread restart). All FFT work happens
	// on the worker thread, never here on the audio thread.
	analysis_.start (buildWorkerConfig ());
}

//------------------------------------------------------------------------
vdplg::spectrum::AnalysisWorker::Config Processor::buildWorkerConfig () const
{
	using namespace vdplg::spectrum;
	vdplg::spectrum::AnalysisWorker::Config cfg;
	cfg.fftSize = fftSize_;
	cfg.window = static_cast<WindowType> (windowIndex_);
	cfg.ref = (dbRefIndex_ == 1) ? DbReference::kRaw : DbReference::kNormalized;
	cfg.sampleRateHz = processSetup.sampleRate;
	cfg.mode = static_cast<ChannelMode> (modeIndex_.load ());
	cfg.attackSec = attackMs_ / 1000.0;
	cfg.releaseSec = releaseMs_ / 1000.0;
	return cfg;
}


//------------------------------------------------------------------------
//------------------------------------------------------------------------
tresult PLUGIN_API Processor::terminate ()
{
	return AudioEffect::terminate ();
}

//------------------------------------------------------------------------
tresult PLUGIN_API Processor::connect (IConnectionPoint* other)
{
	auto result = AudioEffect::connect (other);
	scopeExchange_.onConnect (other, getHostContext ());
	return result;
}

//------------------------------------------------------------------------
tresult PLUGIN_API Processor::disconnect (IConnectionPoint* other)
{
	scopeExchange_.onDisconnect (other);
	return AudioEffect::disconnect (other);
}

//------------------------------------------------------------------------
tresult PLUGIN_API Processor::setActive (TBool state)
{
	if (state)
		scopeExchange_.onActivate (processSetup);
	else
	{
		// Stop the analysis worker while inactive (no audio flowing). It is
		// restarted lazily by syncAnalyzers() on the next processed block.
		analysis_.stop ();
		scopeExchange_.onDeactivate ();
	}
	return AudioEffect::setActive (state);
}

//------------------------------------------------------------------------
tresult PLUGIN_API Processor::setupProcessing (ProcessSetup& newSetup)
{
	return AudioEffect::setupProcessing (newSetup);
}

//------------------------------------------------------------------------
tresult PLUGIN_API Processor::canProcessSampleSize (int32 symbolicSampleSize)
{
	if (symbolicSampleSize == kSample32)
		return kResultTrue;
	return kResultFalse;
}

//------------------------------------------------------------------------
void Processor::pushScopeToExchange (const float* a, const float* b,
                                     const float* balance, int numCols)
{
	if (!scopeExchange_.isEnabled ())
		return;
	auto block = scopeExchange_.getCurrentOrNewBlock ();
	if (block.blockID == InvalidDataExchangeBlockID)
		return; // queue full — drop this frame's snapshot
	auto* data = reinterpret_cast<ScopeData*> (block.data);
	std::memset (data, 0, sizeof (ScopeData));
	const int n = (numCols < ScopeData::kNumCols) ? numCols : ScopeData::kNumCols;
	for (int i = 0; i < n; ++i)
		data->a[i] = a[i];
		if (modeIndex_.load () == static_cast<int> (spectrum::ChannelMode::kMBalance))
	{
		// M/(M-S): scope B shows the precomputed clamped ±12 dB balance view.
		for (int i = 0; i < n; ++i)
			data->b[i] = balance[i];
	}
	else
	{
		for (int i = 0; i < n; ++i)
			data->b[i] = b[i];
	}
	scopeExchange_.sendCurrentBlock ();
}

//------------------------------------------------------------------------
tresult PLUGIN_API Processor::process (ProcessData& data)
{
	//--- 1) parameter changes --------------------------------------------
	if (data.inputParameterChanges)
	{
		int32 numParamsChanged = data.inputParameterChanges->getParameterCount ();
		for (int32 i = 0; i < numParamsChanged; ++i)
		{
			IParamValueQueue* queue = data.inputParameterChanges->getParameterData (i);
			if (!queue)
				continue;
			switch (queue->getParameterId ())
			{
				case kFFTSizeId: fftSizeParam_.beginChanges (queue); break;
				case kWindowTypeId: windowTypeParam_.beginChanges (queue); break;
				case kModeId: modeParam_.beginChanges (queue); break;
				case kAttackId: attackParam_.beginChanges (queue); break;
				case kReleaseId: releaseParam_.beginChanges (queue); break;
				case kDbRefId: dbRefParam_.beginChanges (queue); break;
				default: break;
			}
		}
	}

	auto endAllChanges = [this] () {
		fftSizeParam_.endChanges ();
		windowTypeParam_.endChanges ();
		modeParam_.endChanges ();
		attackParam_.endChanges ();
		releaseParam_.endChanges ();
		dbRefParam_.endChanges ();
		resyncParams ();
	};

	if (data.numInputs == 0 || data.numOutputs == 0)
	{
		endAllChanges ();
		return kResultOk;
	}

	if (data.symbolicSampleSize != kSample32)
	{
		endAllChanges ();
		return kResultFalse;
	}

	int32 numChannels = data.inputs[0].numChannels;
	syncAnalyzers (processSetup.sampleRate);
	uint32 sampleFramesSize = getSampleFramesSizeInBytes (processSetup, data.numSamples);
	void** in = getChannelBuffersPointer (processSetup, data.inputs[0]);
	void** out = getChannelBuffersPointer (processSetup, data.outputs[0]);
	auto* in32 = reinterpret_cast<Sample32**> (in);
	auto* out32 = reinterpret_cast<Sample32**> (out);

	// silence passthrough
	if (data.inputs[0].silenceFlags == getChannelMask (numChannels))
	{
		data.outputs[0].silenceFlags = data.inputs[0].silenceFlags;
		for (int32 ch = 0; ch < numChannels; ++ch)
		{
			if (in32[ch] != out32[ch])
				std::memset (out32[ch], 0, sampleFramesSize);
		}
		endAllChanges ();
		return kResultOk;
	}

	data.outputs[0].silenceFlags = 0;

	// Pure passthrough — analysis never alters the audio.
	for (int32 ch = 0; ch < numChannels; ++ch)
	{
		if (in32[ch] != out32[ch])
			std::memcpy (out32[ch], in32[ch], sampleFramesSize);
	}

	// Analysis path (read-only on input buffers).
	// Hand the block to the off-thread worker. This only copies samples into a
	// bounded queue (O(numSamples)) and returns immediately — no FFT here.
	const Sample32* right = (numChannels > 1) ? in32[1] : in32[0];
	analysis_.feed (in32[0], right, data.numSamples);

	endAllChanges ();
	return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API Processor::setState (IBStream* state)
{
	IBStreamer streamer (state, kLittleEndian);
	streamer.writeInt32 (fftSize_);
	streamer.writeInt32 (windowIndex_);
	streamer.writeInt32 (modeIndex_.load ());
	streamer.writeDouble (attackMs_);
	streamer.writeDouble (releaseMs_);
	streamer.writeInt32 (dbRefIndex_);
	return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API Processor::getState (IBStream* state)
{
	IBStreamer streamer (state, kLittleEndian);
	int32 fftSize = 4096;
	int32 windowIndex = kDefaultWindowIndex;
	int32 modeIndex = kDefaultModeIndex;
	double attackMs = 0.0;
	double releaseMs = 100.0;
	int32 dbRefIndex = kDefaultDbRefIndex;
	if (!streamer.readInt32 (fftSize)) return kInvalidArgument;
	if (!streamer.readInt32 (windowIndex)) return kInvalidArgument;
	if (!streamer.readInt32 (modeIndex)) return kInvalidArgument;
	if (!streamer.readDouble (attackMs)) return kInvalidArgument;
	if (!streamer.readDouble (releaseMs)) return kInvalidArgument;
	if (!streamer.readInt32 (dbRefIndex)) return kInvalidArgument;

	fftSizeParam_.setValue (fftSizeToNormalized (fftSize));
	windowTypeParam_.setValue (static_cast<ParamValue> (windowIndex) /
	                           static_cast<ParamValue> (kNumWindows - 1));
	modeParam_.setValue (static_cast<ParamValue> (modeIndex) /
	                     static_cast<ParamValue> (kNumModes - 1));
	attackParam_.setValue (attackMsToNormalized (attackMs));
	releaseParam_.setValue (releaseMsToNormalized (releaseMs));
	dbRefParam_.setValue (static_cast<ParamValue> (dbRefIndex) /
	                      static_cast<ParamValue> (kNumDbRefs - 1));
	resyncParams ();
	return kResultOk;
}

} // namespace sectrascope
} // namespace vdplg
