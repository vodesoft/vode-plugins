// Vode Plugins — passthrough: audio processor implementation.

#include "Processor.h"
#include "passthroughcids.h"
#include "passthroughparamids.h"

#include "public.sdk/source/vst/vsthelpers.h"
#include "public.sdk/source/vst/vstaudioprocessoralgo.h"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include "base/source/fstreamer.h"

#include "vdplg/dsp.h"

#include <cstring>

namespace vdplg {
namespace passthrough {

using namespace Steinberg;
using namespace Steinberg::Vst;

//------------------------------------------------------------------------
// constants
//------------------------------------------------------------------------
static constexpr ParamValue kGainMinDb = -60.0;
static constexpr ParamValue kGainMaxDb = +24.0;

//------------------------------------------------------------------------
Processor::Processor ()
{
	setControllerClass (kControllerUID);

	// Gain: -60..+24 dB, default 0 dB (normalized 2/3).
	gainParam_.setParamID (kGainId);
	gainParam_.setValue (vdplg::dbToNormalized (0.0, kGainMinDb, kGainMaxDb));

	// Mix: 0..1, default 1 (full wet).
	mixParam_.setParamID (kMixId);
	mixParam_.setValue (1.0);

	updateGainLinear ();
	updateMix ();
}

//------------------------------------------------------------------------
void Processor::updateGainLinear ()
{
	gainDb_ = vdplg::normalizedToDb (gainParam_.getValue (), kGainMinDb, kGainMaxDb);
	gainLinear_ = static_cast<float> (vdplg::dbToLinear (gainDb_));
}

//------------------------------------------------------------------------
void Processor::updateMix ()
{
	mix_ = static_cast<float> (mixParam_.getValue ());
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
tresult PLUGIN_API Processor::terminate ()
{
	return AudioEffect::terminate ();
}

//------------------------------------------------------------------------
tresult PLUGIN_API Processor::setActive (TBool state)
{
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
tresult PLUGIN_API Processor::process (ProcessData& data)
{
	//--- 1) sample-accurate parameter changes ---------------------------
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
				case kGainId:
					gainParam_.beginChanges (queue);
					break;
				case kMixId:
					mixParam_.beginChanges (queue);
					break;
				default:
					break;
			}
		}
	}

	//--- 2) process audio -----------------------------------------------
	if (data.numInputs == 0 || data.numOutputs == 0)
	{
		gainParam_.endChanges ();
		mixParam_.endChanges ();
		updateGainLinear ();
		updateMix ();
		return kResultOk;
	}

	int32 numChannels = data.inputs[0].numChannels;
	uint32 sampleFramesSize = getSampleFramesSizeInBytes (processSetup, data.numSamples);
	void** in = getChannelBuffersPointer (processSetup, data.inputs[0]);
	void** out = getChannelBuffersPointer (processSetup, data.outputs[0]);

	if (data.symbolicSampleSize != kSample32)
	{
		gainParam_.endChanges ();
		mixParam_.endChanges ();
		updateGainLinear ();
		updateMix ();
		return kResultFalse;
	}

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
		gainParam_.endChanges ();
		mixParam_.endChanges ();
		updateGainLinear ();
		updateMix ();
		return kResultOk;
	}

	data.outputs[0].silenceFlags = 0;

	for (int32 sample = 0; sample < data.numSamples; ++sample)
	{
		float g = gainLinear_;
		float m = mix_;
		if (gainParam_.hasChanges () || mixParam_.hasChanges ())
		{
			gainParam_.advance (1);
			mixParam_.advance (1);
			g = static_cast<float> (vdplg::dbToLinear (
			    vdplg::normalizedToDb (gainParam_.getValue (), kGainMinDb, kGainMaxDb)));
			m = static_cast<float> (mixParam_.getValue ());
		}
		for (int32 ch = 0; ch < numChannels; ++ch)
		{
			Sample32 dry = in32[ch][sample];
			Sample32 wet = dry * g;
			out32[ch][sample] = dry + (wet - dry) * m;
		}
	}

	gainParam_.endChanges ();
	mixParam_.endChanges ();
	updateGainLinear ();
	updateMix ();
	return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API Processor::setState (IBStream* state)
{
	IBStreamer streamer (state, kLittleEndian);
	streamer.writeDouble (gainDb_);
	streamer.writeFloat (mix_);
	return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API Processor::getState (IBStream* state)
{
	IBStreamer streamer (state, kLittleEndian);
	double gainDb = 0.0;
	float mix = 1.0f;
	if (!streamer.readDouble (gainDb))
		return kInvalidArgument;
	if (!streamer.readFloat (mix))
		return kInvalidArgument;

	gainDb_ = gainDb;
	gainParam_.setValue (vdplg::dbToNormalized (gainDb, kGainMinDb, kGainMaxDb));
	updateGainLinear ();
	mix_ = mix;
	mixParam_.setValue (mix);
	return kResultOk;
}

} // namespace passthrough
} // namespace vdplg
