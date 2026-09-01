// Vode Plugins — Sectra Scope: edit controller.
//
// Phase 2 (TDD Red): textless stub registering all six parameters with
// string conversion. The VSTGUI editor (two stacked scopes + control strip)
// lands in Phase 4.

#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"

namespace vdplg {
namespace sectrascope {

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
	Steinberg::tresult PLUGIN_API getParamStringByValue (Steinberg::Vst::ParamID tag,
	                                                     Steinberg::Vst::ParamValue valueNormalized,
	                                                     Steinberg::Vst::String128 string) SMTG_OVERRIDE;
	Steinberg::tresult PLUGIN_API getParamValueByString (Steinberg::Vst::ParamID tag,
	                                                     Steinberg::Vst::TChar* string,
	                                                     Steinberg::Vst::ParamValue& valueNormalized)
		SMTG_OVERRIDE;
};

} // namespace sectrascope
} // namespace vdplg
