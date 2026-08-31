// Vode Plugins — passthrough: edit controller (textless).

#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"

namespace vdplg {
namespace passthrough {

class Controller : public Steinberg::Vst::EditController
{
public:
	Controller () = default;
	~Controller () override = default;

	static Steinberg::FUnknown* createInstance (void*)
	{
		return static_cast<Steinberg::Vst::IEditController*> (new Controller);
	}

	//--- EditController overrides ------------------------------------------
	Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) SMTG_OVERRIDE;

	// Textless plugin: no view (base class already returns nullptr).
};

} // namespace passthrough
} // namespace vdplg
