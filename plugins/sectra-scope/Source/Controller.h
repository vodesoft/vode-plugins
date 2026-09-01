// Vode Plugins — Sectra Scope: edit controller + VSTGUI editor.
//
// Registers the six parameters (with string conversion for the host), creates
// the VSTGUI editor (two stacked spectrum scopes + a control strip of knobs),
// and receives live spectra snapshots from the audio processor over the SDK
// data-exchange queue (host-mediated cross-process shared memory). The newest
// snapshot drives both ScopeViews each frame.

#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"
#include "public.sdk/source/vst/utility/dataexchange.h"
#include "pluginterfaces/vst/ivstdataexchange.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "vstgui/plugin-bindings/vst3editor.h"
#include "vstgui/uidescription/uiattributes.h"

#include "sectraparamids.h"
#include "scopedata.h"
#include "ScopeView.h"

namespace vdplg {
namespace sectrascope {

class Controller : public Steinberg::Vst::EditController,
                   public Steinberg::Vst::IDataExchangeReceiver,
                   public VSTGUI::VST3EditorDelegate
{
	OBJ_METHODS (Controller, EditController)
	DEFINE_INTERFACES
		DEF_INTERFACE (Steinberg::Vst::IDataExchangeReceiver)
	END_DEFINE_INTERFACES (EditController)
	REFCOUNT_METHODS (EditController)

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
	Steinberg::tresult PLUGIN_API setParamNormalized (Steinberg::Vst::ParamID tag,
	                                                  Steinberg::Vst::ParamValue valueNormalized)
		SMTG_OVERRIDE;
	Steinberg::IPlugView* PLUGIN_API createView (const char* name) SMTG_OVERRIDE;

	//--- IDataExchangeReceiver ---------------------------------------------
	void PLUGIN_API queueOpened (Steinberg::Vst::DataExchangeUserContextID userContextID,
	                             Steinberg::uint32 blockSize, Steinberg::TBool& dispatchOnBackgroundThread)
		SMTG_OVERRIDE;
	void PLUGIN_API queueClosed (Steinberg::Vst::DataExchangeUserContextID userContextID)
		SMTG_OVERRIDE;
	void PLUGIN_API onDataExchangeBlocksReceived (
	    Steinberg::Vst::DataExchangeUserContextID userContextID, Steinberg::uint32 numBlocks,
	    Steinberg::Vst::DataExchangeBlock* blocks, Steinberg::TBool onBackgroundThread)
		SMTG_OVERRIDE;

	//--- IConnectionPoint / message routing ---------------------------------
	Steinberg::tresult PLUGIN_API notify (Steinberg::Vst::IMessage* message) SMTG_OVERRIDE;

	//--- VST3EditorDelegate --------------------------------------------------
	VSTGUI::CView* createCustomView (VSTGUI::UTF8StringPtr name, const VSTGUI::UIAttributes& attributes,
	                                 const VSTGUI::IUIDescription* description, VSTGUI::VST3Editor* editor)
		SMTG_OVERRIDE;
	void didOpen (VSTGUI::VST3Editor* editor) SMTG_OVERRIDE;

private:
	void updateScopeLabels ();

	Steinberg::Vst::DataExchangeReceiverHandler dataExchange_{this};
	std::vector<ScopeView*> scopes_; // [0] = channel A slot, [1] = channel B slot
	int modeIndex_ {kDefaultModeIndex};
};

} // namespace sectrascope
} // namespace vdplg
