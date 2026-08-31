// Vode Plugins — passthrough: edit controller implementation.
//
// The controller is intentionally minimal (textless plugin): it exists so
// the DAW can instantiate the component + controller pair and automate
// parameters. No UI is created.

#include "Controller.h"
#include "passthroughparamids.h"

namespace vdplg {
namespace passthrough {

using namespace Steinberg;
using namespace Steinberg::Vst;

//------------------------------------------------------------------------
tresult PLUGIN_API Controller::initialize (FUnknown* context)
{
	tresult result = EditController::initialize (context);
	if (result != kResultOk)
		return result;

	// Gain: -60..+24 dB, default 0 dB -> normalized 2/3.
	parameters.addParameter (STR16 ("Gain"), STR16 ("dB"), kStepCountContinuous,
	                        2.0 / 3.0, ParameterInfo::kCanAutomate, kGainId);

	// Mix: 0..1, default 1 (full wet).
	parameters.addParameter (STR16 ("Mix"), nullptr, kStepCountContinuous, 1.0,
	                        ParameterInfo::kCanAutomate, kMixId);

	return kResultOk;
}

} // namespace passthrough
} // namespace vdplg
