// Vode Plugins — Sectra Scope: edit controller implementation.
//
// Phase 2 (TDD Red): textless stub registering all six parameters with
// explicit string conversion (SDK 3.8.1 has no pointLabels). The VSTGUI
// editor lands in Phase 4.

#include "Controller.h"
#include "sectraparamids.h"

#include "vstgui/lib/cbitmap.h"
#include "vstgui/lib/cframe.h"
#include "vstgui/uidescription/cstream.h"
#include "vstgui/lib/coffscreencontext.h"
#include "vstgui/lib/platform/platformfactory.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace vdplg {
namespace sectrascope {

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace VSTGUI;

namespace {

const char* const kFftSizeNames[kNumFftSizes] = {"1024", "2048", "4096", "8192", "16384"};
const char* const kWindowNames[kNumWindows] =
    {"Rect", "Hann", "Hamming", "Blackman", "Blackman-Harris"};
const char* const kModeNames[kNumModes] = {"L/R", "Mid/Side", "Mid/(Mid-Side)"};
const char* const kDbRefNames[kNumDbRefs] = {"Norm", "Raw"};

int choiceIndexFromNormalized(ParamValue v, int numPoints)
{
	int idx = static_cast<int>(std::lround(v * (numPoints - 1)));
	if (idx < 0) idx = 0;
	if (idx >= numPoints) idx = numPoints - 1;
	return idx;
}

void formatMs(char* out, size_t capacity, double ms)
{
	if (ms < 1000.0)
		std::snprintf(out, capacity, "%.1f ms", ms);
	else
		std::snprintf(out, capacity, "%.2f s", ms / 1000.0);
}

bool parseMs(const TChar* str, double& msOut)
{
	char buf[64];
	size_t n = 0;
	while (str[n] && n + 1 < sizeof(buf))
		buf[n] = static_cast<char>(str[n]);
	buf[n] = '\0';

	double value = std::atof(buf);
	const char* unit = buf;
	while (*unit == ' ') ++unit;
	while (*unit && (*unit == '.' || (*unit >= '0' && *unit <= '9'))) ++unit;
	while (*unit == ' ') ++unit;
	if (unit[0] == 's' && (unit[1] == '\0')) // seconds
		value *= 1000.0;
	msOut = value;
	return true;
}

// All parameter strings in this plugin are pure ASCII; copy them verbatim
// into the UTF-16 destination.
void asciiToUtf16(String128 dst, const char* src)
{
	for (uint32 i = 0; i < 127 && src[i] != '\0'; ++i)
		dst[i] = static_cast<TChar>(src[i]);
	dst[std::min<uint32>(static_cast<uint32>(std::strlen(src)), 127)] = 0;
}

// Message id used by tests / test host to request a UI screenshot.
const FIDString kScreenshotMessageId = "vdplg.debug.screenshot";

// Renders a VSTGUI frame off-screen and writes it as PNG to `path`.
// Mirrors VST3Editor::saveScreenshot() (vstgui4/plugin-bindings/vst3editor.cpp).
bool renderFrameToPng(CFrame* frame, const std::string& path)
{
	if (!frame || path.empty())
		return false;

	const auto size = frame->getViewSize().getSize();
	auto offscreen = COffscreenContext::create(size, 1.);
	if (!offscreen)
		return false;

	offscreen->beginDraw();
	frame->draw(offscreen);
	offscreen->endDraw();

	auto data = getPlatformFactory().createBitmapMemoryPNGRepresentation(
	    offscreen->getBitmap()->getPlatformBitmap());
	if (data.empty())
		return false;

	CFileStream stream;
	if (!stream.open(path.c_str(), CFileStream::kWriteMode | CFileStream::kTruncateMode |
	                          CFileStream::kBinaryMode))
	{
		return false;
	}
	stream.writeRaw(data.data(), static_cast<uint32_t>(data.size()));
	return true;
}

} // namespace

//------------------------------------------------------------------------
tresult PLUGIN_API Controller::initialize (FUnknown* context)
{
	tresult result = EditController::initialize (context);
	if (result != kResultOk)
		return result;

	parameters.addParameter (STR16 ("FFT Size"), nullptr, kNumFftSizes - 1,
	                        fftSizeToNormalized (kFftSizes[kDefaultFftSizeIndex]),
	                        ParameterInfo::kCanAutomate | ParameterInfo::kIsList, kFFTSizeId);

	parameters.addParameter (STR16 ("Window"), nullptr, kNumWindows - 1,
	                        static_cast<ParamValue> (kDefaultWindowIndex) /
	                            static_cast<ParamValue> (kNumWindows - 1),
	                        ParameterInfo::kCanAutomate | ParameterInfo::kIsList, kWindowTypeId);

	parameters.addParameter (STR16 ("Channel Mode"), nullptr, kNumModes - 1,
	                        static_cast<ParamValue> (kDefaultModeIndex) /
	                            static_cast<ParamValue> (kNumModes - 1),
	                        ParameterInfo::kCanAutomate | ParameterInfo::kIsList, kModeId);

	parameters.addParameter (STR16 ("Attack"), STR16 ("ms"), kStepCountContinuous,
	                        attackMsToNormalized (0.0), ParameterInfo::kCanAutomate, kAttackId);

	parameters.addParameter (STR16 ("Release"), STR16 ("ms"), kStepCountContinuous,
	                        releaseMsToNormalized (100.0), ParameterInfo::kCanAutomate, kReleaseId);

	parameters.addParameter (STR16 ("dB Ref"), nullptr, kNumDbRefs - 1,
	                        static_cast<ParamValue> (kDefaultDbRefIndex) /
	                            static_cast<ParamValue> (kNumDbRefs - 1),
	                        ParameterInfo::kCanAutomate | ParameterInfo::kIsList, kDbRefId);

	return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API Controller::getParamStringByValue (ParamID tag, ParamValue v, String128 string)
{
	char buf[32];
	switch (tag)
	{
		case kFFTSizeId:
			std::snprintf(buf, sizeof(buf), "%s", kFftSizeNames[choiceIndexFromNormalized(v, kNumFftSizes)]);
			break;
		case kWindowTypeId:
			std::snprintf(buf, sizeof(buf), "%s", kWindowNames[choiceIndexFromNormalized(v, kNumWindows)]);
			break;
		case kModeId:
			std::snprintf(buf, sizeof(buf), "%s", kModeNames[choiceIndexFromNormalized(v, kNumModes)]);
			break;
		case kAttackId:
			formatMs(buf, sizeof(buf), attackMsFromNormalized(v));
			break;
		case kReleaseId:
			formatMs(buf, sizeof(buf), releaseMsFromNormalized(v));
			break;
		case kDbRefId:
			std::snprintf(buf, sizeof(buf), "%s", kDbRefNames[choiceIndexFromNormalized(v, kNumDbRefs)]);
			break;
		default:
			return EditController::getParamStringByValue(tag, v, string);
	}
	asciiToUtf16(string, buf);
	return kResultTrue;
}

//------------------------------------------------------------------------
tresult PLUGIN_API Controller::getParamValueByString (ParamID tag, TChar* string,
                                                      ParamValue& valueNormalized)
{
	double ms = 0.0;
	switch (tag)
	{
		case kAttackId:
			if (!parseMs(string, ms)) return kResultFalse;
			valueNormalized = attackMsToNormalized(ms);
			return kResultTrue;
		case kReleaseId:
			if (!parseMs(string, ms)) return kResultFalse;
			valueNormalized = releaseMsToNormalized(ms);
			return kResultTrue;
		default:
			return EditController::getParamValueByString(tag, string, valueNormalized);
	}
}

//------------------------------------------------------------------------
tresult PLUGIN_API Controller::setParamNormalized (ParamID tag, ParamValue valueNormalized)
{
	auto result = EditController::setParamNormalized (tag, valueNormalized);
	if (tag == kModeId)
	{
		modeIndex_ = choiceIndexFromNormalized (valueNormalized, kNumModes);
		updateScopeLabels ();
	}
	return result;
}

//------------------------------------------------------------------------
void Controller::updateScopeLabels ()
{
	const bool balance = (modeIndex_ == static_cast<int> (spectrum::ChannelMode::kMBalance));
	static const char* lrA[] = {"L", "M"};
	static const char* lrB[] = {"R", "S"};
	for (size_t i = 0; i < scopes_.size (); ++i)
	{
		if (!scopes_[i])
			continue;
		scopes_[i]->setLabel ((i == 0) ? lrA[modeIndex_] : lrB[modeIndex_]);
		scopes_[i]->setBalanceMode (balance && (i == 1));
	}
}

//------------------------------------------------------------------------
IPlugView* PLUGIN_API Controller::createView (const char* name)
{
	std::string_view viewName (name);
	if (viewName == ViewType::kEditor)
	{
		auto* editor = new VST3Editor (this, "view", "editor.uidesc");
		return editor;
	}
	return nullptr;
}

//------------------------------------------------------------------------
CView* Controller::createCustomView (UTF8StringPtr name, const UIAttributes& attributes,
                                     const IUIDescription* /*description*/, VST3Editor* /*editor*/)
{
	// The factory never applies "origin" to delegate-created views (it only
	// runs applyAttributeValues for class-based creation), so position the
	// view here; otherwise every custom view lands at (0,0).
	CPoint origin {0, 0};
	attributes.getPointAttribute ("origin", origin);
	CRect size {0, 0, 720, 200};
	attributes.getRectAttribute ("size", size);
	if (std::string_view (name) == "scopeA")
	{
		auto* view = new ScopeView (size);
		view->setViewSize (CRect (origin, size.getSize ()));
		view->setLabel ("L");
		view->setFrameColor ({90, 160, 230, 255}); // blue tint (channel A)
		scopes_.push_back (view);
		return view;
	}
	if (std::string_view (name) == "scopeB")
	{
		auto* view = new ScopeView (size);
		view->setViewSize (CRect (origin, size.getSize ()));
		view->setLabel ("R");
		view->setFrameColor ({230, 170, 80, 255}); // amber tint (channel B)
		scopes_.push_back (view);
		return view;
	}
	return nullptr;
}

//------------------------------------------------------------------------
void Controller::didOpen (VST3Editor* editor)
{
	updateScopeLabels ();
	// The live editor's frame is also exposed as the debug frame so that a
	// "vdplg.debug.screenshot" message works against the real UI too.
	debugFrame_ = editor ? editor->getFrame () : nullptr;
}

//------------------------------------------------------------------------
void Controller::willClose (VST3Editor* /*editor*/)
{
	// VSTGUI destroys every view of the frame right after this callback
	// (VST3Editor::close -> getFrame()->removeAll(true)), so our raw pointers
	// are dead from here on. Drop them; the next editor open re-registers via
	// createCustomView(). Without this, onDataExchangeBlocksReceived would keep
	// writing spectra into freed memory while playback runs (heap corruption).
	scopes_.clear ();
	debugFrame_ = nullptr;
}

//------------------------------------------------------------------------
tresult PLUGIN_API Controller::notify (IMessage* message)
{
	// NOTE: FIDString is const char*, so compare by content, not by address.
	if (message && std::strcmp(message->getMessageID (), kScreenshotMessageId) == 0)
	{
		IAttributeList* attrs = message->getAttributes ();
		String128 pathW {};
		if (!debugFrame_ || !attrs ||
		    attrs->getString ("path", pathW, sizeof(pathW)) != kResultTrue)
			return kResultFalse;
		std::string path;
		for (uint32 i = 0; i < 127 && pathW[i]; ++i)
			path += static_cast<char>(pathW[i]);
		return renderFrameToPng (debugFrame_, path) ? kResultTrue : kResultFalse;
	}
	if (dataExchange_.onMessage (message))
		return kResultTrue;
	return EditController::notify (message);
}

//------------------------------------------------------------------------
void Controller::attachDebugFrame (CFrame* frame)
{
	debugFrame_ = frame; // ownership stays with the caller
}

//------------------------------------------------------------------------
void Controller::detachDebugFrame ()
{
	debugFrame_ = nullptr;
}

//------------------------------------------------------------------------
void PLUGIN_API Controller::queueOpened (DataExchangeUserContextID userContextID, uint32 blockSize,
                                         TBool& dispatchOnBackgroundThread)
{
	dispatchOnBackgroundThread = false; // keep it simple: repaint on the main thread
}

//------------------------------------------------------------------------
void PLUGIN_API Controller::queueClosed (DataExchangeUserContextID userContextID) {}

//------------------------------------------------------------------------
void PLUGIN_API Controller::onDataExchangeBlocksReceived (DataExchangeUserContextID userContextID,
                                                          uint32 numBlocks, DataExchangeBlock* blocks,
                                                          TBool onBackgroundThread)
{
	if (userContextID != kScopeQueueId || numBlocks == 0)
		return;
	const auto* data = reinterpret_cast<const ScopeData*> (blocks[numBlocks - 1].data);
	for (auto* scope : scopes_)
	{
		if (!scope)
			continue;
		scope->setData ((scope == scopes_[0]) ? data->a : data->b, ScopeData::kNumCols);
	}
}

} // namespace sectrascope
} // namespace vdplg
