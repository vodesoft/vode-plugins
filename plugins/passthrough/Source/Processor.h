// Vode Plugins — passthrough: audio processor (component).
//
// Template plugin for the Vode Plugins family:
//   - stereo in / stereo out
//   - Gain (dB) and Mix (dry/wet) parameters, sample-accurate automation
//   - no UI (textless), state persistence via FStreamer

#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/utility/sampleaccurate.h"

namespace vdplg {
namespace passthrough {

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
	Steinberg::tresult PLUGIN_API process (Steinberg::Vst::ProcessData& data) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API setupProcessing (Steinberg::Vst::ProcessSetup& newSetup)
		SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API canProcessSampleSize (Steinberg::int32 symbolicSampleSize) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API setState (Steinberg::IBStream* state) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API getState (Steinberg::IBStream* state) SMTG_OVERRIDE;

protected:
	void updateGainLinear ();
	void updateMix ();

	//--- parameters ------------------------------------------------------
	Steinberg::Vst::SampleAccurate::Parameter gainParam_;
	Steinberg::Vst::SampleAccurate::Parameter mixParam_;

	//--- model values ----------------------------------------------------
	double gainDb_ {0.0};
	float gainLinear_ {1.0f};
	float mix_ {1.0f};
};

} // namespace passthrough
} // namespace vdplg
