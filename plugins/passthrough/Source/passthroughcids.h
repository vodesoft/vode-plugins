// Vode Plugins — passthrough: class IDs.
//
// UIDs generated for this project (fixed forever; changing them breaks
// existing DAW patch bays / presets).

#pragma once

#include "pluginterfaces/base/funknown.h"

namespace vdplg {
namespace passthrough {

	static const Steinberg::FUID kProcessorUID (0x5A1C7E2B, 0x9D4F4C81, 0xA3E60B57, 0x2F8D41C9);
	static const Steinberg::FUID kControllerUID (0x7B3E9F4D, 0x1C6A4E02, 0xB8D53A96, 0x4E0F72DA);

#define kPassthroughVST3Category "Fx"

} // namespace passthrough
} // namespace vdplg
