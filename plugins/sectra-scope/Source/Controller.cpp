// Vode Plugins — Sectra Scope: edit controller implementation.
//
// Phase 2 (TDD Red): textless stub registering all six parameters with
// explicit string conversion (SDK 3.8.1 has no pointLabels). The VSTGUI
// editor lands in Phase 4.

#include "Controller.h"
#include "sectraparamids.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace vdplg {
namespace sectrascope {

using namespace Steinberg;
using namespace Steinberg::Vst;

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

} // namespace sectrascope
} // namespace vdplg
