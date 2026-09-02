// Vode Plugins — Sectra Scope: audio processor (component).
//
// Real-time spectrum analyzer:
//   - stereo in / stereo out, pure passthrough (analysis never alters audio)
//   - 6 parameters: FFT size, window, channel mode, attack, release, dB ref
//
// Phase 2 (TDD Red): passthrough + param plumbing only; the analyzers are
// wired into process() in Phase 3.

#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/utility/sampleaccurate.h"
#include "public.sdk/source/vst/utility/dataexchange.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include "vdplg/spectrum.h"
#include "vdplg/analysisworker.h"
#include "scopedata.h"
#include <atomic>

namespace vdplg {
namespace sectrascope {

class Processor : public Steinberg::Vst::AudioEffect
{
public:
	Processor ();
	~Processor () override = default;

	static Steinberg::FUnknown* createInstance (void*)
	{
		return static_cast<Steinberg::Vst::IAudioProcessor*> (new Processor);
	}

	//--- AudioEffect overrides -------------------------------------------
	Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API terminate () SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API setActive (Steinberg::TBool state) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API connect (Steinberg::Vst::IConnectionPoint* other) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API disconnect (Steinberg::Vst::IConnectionPoint* other) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API process (Steinberg::Vst::ProcessData& data) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API setupProcessing (Steinberg::Vst::ProcessSetup& newSetup)
		SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API canProcessSampleSize (Steinberg::int32 symbolicSampleSize)
		SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API setState (Steinberg::IBStream* state) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API getState (Steinberg::IBStream* state) SMTG_OVERRIDE;

protected:
	void resyncParams ();
	// Rebuild analyzer config if fft/window/sample-rate changed since last call.
	void syncAnalyzers (double sampleRateHz);
	// Build an AnalysisWorker::Config from current cached model values.
	vdplg::spectrum::AnalysisWorker::Config buildWorkerConfig () const;
	// Push the latest spectra snapshot into a data-exchange block for the UI.
	void pushScopeToExchange (const float* a, const float* b,
	                          const float* balance, int numCols);

	//--- parameters ------------------------------------------------------
	Steinberg::Vst::SampleAccurate::Parameter fftSizeParam_;
	Steinberg::Vst::SampleAccurate::Parameter windowTypeParam_;
	Steinberg::Vst::SampleAccurate::Parameter modeParam_;
	Steinberg::Vst::SampleAccurate::Parameter attackParam_;
	Steinberg::Vst::SampleAccurate::Parameter releaseParam_;
	Steinberg::Vst::SampleAccurate::Parameter dbRefParam_;

	//--- cached model values ---------------------------------------------
	int fftSize_ {4096};
	int windowIndex_ {4}; // Blackman-Harris
	std::atomic<int> modeIndex_ {0}; // L/R (read by worker thread)
	double attackMs_ {0.0};
	double releaseMs_ {100.0};
	int dbRefIndex_ {0};  // Normalized

	//--- analysis --------------------------------------------------------
	// Off-thread spectrum analysis. The audio thread only feeds samples here;
	// all FFT/ballistics work happens on the worker thread.
	vdplg::spectrum::AnalysisWorker analysis_;

	//--- data exchange (processor -> controller) -------------------------
	Steinberg::Vst::DataExchangeHandler scopeExchange_;

	// last applied analyzer config (for change detection)
	int cfgFftSize_ {0};
	int cfgWindowIndex_ {-1};
	int cfgModeIndex_ {-1};
	double cfgAttackMs_ {-1.0};
	double cfgReleaseMs_ {-1.0};
	int cfgDbRefIndex_ {-1};
	double cfgSampleRate_ {0.0};
};

} // namespace sectrascope
} // namespace vdplg
