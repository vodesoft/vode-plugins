// Vode Plugins — passthrough: parameter IDs.
//
// ParamIDs are fixed forever (changing them breaks existing presets).

#pragma once

#include "pluginterfaces/vst/ivstaudioprocessor.h"

namespace vdplg {
namespace passthrough {

static constexpr Steinberg::Vst::ParamID kGainId = 1;   // Gain in dB
static constexpr Steinberg::Vst::ParamID kMixId = 2;    // Mix dry/wet (0..1)

} // namespace passthrough
} // namespace vdplg
