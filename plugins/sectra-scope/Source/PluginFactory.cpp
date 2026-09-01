// Vode Plugins — Sectra Scope: VST3 plugin factory entry point.

#include "SectraScopeProcessor.h"
#include "Controller.h"
#include "sectracids.h"
#include "version.h"

#include "public.sdk/source/main/pluginfactory.h"

#define stringPluginName "Sectra Scope"

using namespace Steinberg::Vst;
using namespace vdplg::sectrascope;

//------------------------------------------------------------------------
//  VST Plug-in Entry
//------------------------------------------------------------------------
BEGIN_FACTORY_DEF (stringCompanyName, stringCompanyWeb, stringCompanyEmail)

	//--- component (audio processor) ------------------------------------
	DEF_CLASS2 (INLINE_UID_FROM_FUID (kProcessorUID),
				PClassInfo::kManyInstances,		// cardinality
				kVstAudioEffectClass,			// component category
				stringPluginName,				// plugin name
				Vst::kDistributable,			// component + controller may be distributed
				kSectraScopeVST3Category,		// subcategory
				FULL_VERSION_STR,				// plugin version
				kVstVersionString,				// VST3 SDK version (do not change)
				vdplg::sectrascope::Processor::createInstance)

	//--- controller ------------------------------------------------------
	DEF_CLASS2 (INLINE_UID_FROM_FUID (kControllerUID),
				PClassInfo::kManyInstances,		// cardinality
				kVstComponentControllerClass,	// controller category
				stringPluginName " Controller", // controller name
				0,								// not used
				"",								// not used
				FULL_VERSION_STR,				// plugin version
				kVstVersionString,				// VST3 SDK version (do not change)
				vdplg::sectrascope::Controller::createInstance)

END_FACTORY
