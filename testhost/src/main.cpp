// Vode Plugins — vst3testhost: minimal offline VST3 test host (L3).
//
// Loads a .vst3 bundle, instantiates the audio component, pumps WAV input
// through process() in blocks, applies sample-accurate parameter automation
// via IParameterChanges, captures the output, and evaluates assertions.
//
// Usage (direct):
//   vst3testhost --plugin X.vst3 --input in.wav [--blocksize 512]
//                [--automation "Gain=6dB@0s,Mix=0@0.5s"] [--output out.wav]
//                [--assert "peak(out) <= 0dBFS"] ... 
//                [--compare-golden ref.wav --tolerance 1e-6] [--report json]
//
// Usage (case file, driven by CTest):
//   vst3testhost --case testdata/cases/unity_gain.json
//
// Exit code: 0 = all assertions passed, 1 = failure, 2 = usage/load error.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include "vdplg/analysis.h"
#include "vdplg/dsp.h"
#include "vdplg/wavio.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// ---------------------------------------------------------------------------
// tiny JSON parser (objects, arrays, strings, numbers, bools, null)
// ---------------------------------------------------------------------------
struct JsonValue
{
	enum Type { Null, Bool, Number, String, Array, Object };
	Type type = Null;
	bool boolean = false;
	double number = 0.0;
	std::string str;
	std::vector<JsonValue> array;
	std::vector<std::pair<std::string, JsonValue>> object;

	const JsonValue* find(const std::string& key) const
	{
		for (const auto& kv : object)
			if (kv.first == key)
				return &kv.second;
		return nullptr;
	}
};

class JsonParser
{
public:
	explicit JsonParser(const std::string& text) : s_(text) {}

	bool parse(JsonValue& out, std::string& err)
	{
		skipWs();
		if (!parseValue(out, err))
			return false;
		skipWs();
		if (pos_ != s_.size())
		{
			err = "trailing characters at offset " + std::to_string(pos_);
			return false;
		}
		return true;
	}

private:
	void skipWs()
	{
		while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_])))
			++pos_;
	}

	bool parseValue(JsonValue& v, std::string& err)
	{
		skipWs();
		if (pos_ >= s_.size())
		{
			err = "unexpected end of input";
			return false;
		}
		char c = s_[pos_];
		if (c == '{')
			return parseObject(v, err);
		if (c == '[')
			return parseArray(v, err);
		if (c == '"')
		{
			v.type = JsonValue::String;
			return parseString(v.str, err);
		}
		if (c == 't' || c == 'f')
		{
			if (s_.compare(pos_, 4, "true") == 0)
			{
				v.type = JsonValue::Bool;
				v.boolean = true;
				pos_ += 4;
				return true;
			}
			if (s_.compare(pos_, 5, "false") == 0)
			{
				v.type = JsonValue::Bool;
				v.boolean = false;
				pos_ += 5;
				return true;
			}
			err = "invalid literal";
			return false;
		}
		if (c == 'n')
		{
			if (s_.compare(pos_, 4, "null") == 0)
			{
				v.type = JsonValue::Null;
				pos_ += 4;
				return true;
			}
			err = "invalid literal";
			return false;
		}
		// number
		std::size_t start = pos_;
		while (pos_ < s_.size() &&
		       (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '-' ||
		        s_[pos_] == '+' || s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E'))
			++pos_;
		if (start == pos_)
		{
			err = "invalid value at offset " + std::to_string(pos_);
			return false;
		}
		v.type = JsonValue::Number;
		try
		{
			v.number = std::stod(s_.substr(start, pos_ - start));
		}
		catch (...)
		{
			err = "invalid number";
			return false;
		}
		return true;
	}

	bool parseString(std::string& out, std::string& err)
	{
		if (s_[pos_] != '"')
		{
			err = "expected string";
			return false;
		}
		++pos_;
		out.clear();
		while (pos_ < s_.size() && s_[pos_] != '"')
		{
			char c = s_[pos_];
			if (c == '\\' && pos_ + 1 < s_.size())
			{
				++pos_;
				char e = s_[pos_];
				switch (e)
				{
					case 'n': out.push_back('\n'); break;
					case 't': out.push_back('\t'); break;
					case 'r': out.push_back('\r'); break;
					case '\\': out.push_back('\\'); break;
					case '"': out.push_back('"'); break;
					case '/': out.push_back('/'); break;
					default: out.push_back(e); break;
				}
			}
			else
			{
				out.push_back(c);
			}
			++pos_;
		}
		if (pos_ >= s_.size())
		{
			err = "unterminated string";
			return false;
		}
		++pos_; // closing quote
		return true;
	}

	bool parseArray(JsonValue& v, std::string& err)
	{
		v.type = JsonValue::Array;
		++pos_; // [
		skipWs();
		if (pos_ < s_.size() && s_[pos_] == ']')
		{
			++pos_;
			return true;
		}
		for (;;)
		{
			JsonValue item;
			if (!parseValue(item, err))
				return false;
			v.array.push_back(std::move(item));
			skipWs();
			if (pos_ >= s_.size())
			{
				err = "unterminated array";
				return false;
			}
			if (s_[pos_] == ',')
			{
				++pos_;
				continue;
			}
			if (s_[pos_] == ']')
			{
				++pos_;
				return true;
			}
			err = "expected ',' or ']' in array";
			return false;
		}
	}

	bool parseObject(JsonValue& v, std::string& err)
	{
		v.type = JsonValue::Object;
		++pos_; // {
		skipWs();
		if (pos_ < s_.size() && s_[pos_] == '}')
		{
			++pos_;
			return true;
		}
		for (;;)
		{
			skipWs();
			std::string key;
			if (!parseString(key, err))
				return false;
			skipWs();
			if (pos_ >= s_.size() || s_[pos_] != ':')
			{
				err = "expected ':' in object";
				return false;
			}
			++pos_;
			JsonValue val;
			if (!parseValue(val, err))
				return false;
			v.object.emplace_back(std::move(key), std::move(val));
			skipWs();
			if (pos_ >= s_.size())
			{
				err = "unterminated object";
				return false;
			}
			if (s_[pos_] == ',')
			{
				++pos_;
				continue;
			}
			if (s_[pos_] == '}')
			{
				++pos_;
				return true;
			}
			err = "expected ',' or '}' in object";
			return false;
		}
	}

	// NOTE: must OWN the string (not hold a reference). A reference member
	// bound to a temporary (e.g. JsonParser p(ss.str())) dangles once the
	// temporary is destroyed at the end of the full-expression.
	std::string s_;
	std::size_t pos_ {0};
};

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
std::string toLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

std::string trim(const std::string& s)
{
	std::size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return "";
	std::size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

std::vector<std::string> split(const std::string& s, char sep)
{
	std::vector<std::string> out;
	std::string cur;
	for (char c : s)
	{
		if (c == sep)
		{
			out.push_back(trim(cur));
			cur.clear();
		}
		else
			cur.push_back(c);
	}
	out.push_back(trim(cur));
	return out;
}

// Interleave planar buffer (ch * frames + s) into sample-major (s * ch + ch).
std::vector<float> interleave(const std::vector<float>& planar, int channels, int frames)
{
	std::vector<float> out(static_cast<std::size_t>(frames) * channels);
	for (int s = 0; s < frames; ++s)
		for (int ch = 0; ch < channels; ++ch)
			out[static_cast<std::size_t>(s) * channels + ch] =
			    planar[static_cast<std::size_t>(ch) * frames + s];
	return out;
}

// ---------------------------------------------------------------------------
// automation: "Name=value@time" entries, comma separated.
// value: normalized [0..1] or "<n>dB"; time: "<n>s" or "<n>samples"
// ---------------------------------------------------------------------------
struct AutomationEvent
{
	std::string name;
	double valueNormalized = 0.0;
	int64 sampleOffset = 0;
	ParamID paramId = 0;
};

bool parseAutomation(const std::string& spec, double sampleRate,
                     const std::vector<std::pair<std::string, ParamID>>& paramNames,
                     std::vector<AutomationEvent>& out, std::string& err)
{
	for (const auto& entry : split(spec, ','))
	{
		if (entry.empty())
			continue;
		auto atPos = entry.rfind('@');
		if (atPos == std::string::npos)
		{
			err = "automation entry missing '@': " + entry;
			return false;
		}
		std::string lhs = trim(entry.substr(0, atPos));
		std::string timeStr = trim(entry.substr(atPos + 1));

		auto eqPos = lhs.find('=');
		if (eqPos == std::string::npos)
		{
			err = "automation entry missing '=': " + entry;
			return false;
		}
		std::string name = trim(lhs.substr(0, eqPos));
		std::string valueStr = trim(lhs.substr(eqPos + 1));

		ParamID id = 0;
		bool found = false;
		for (const auto& [n, pid] : paramNames)
		{
			if (toLower(n) == toLower(name))
			{
				id = pid;
				found = true;
				break;
			}
		}
		if (!found)
		{
			err = "unknown parameter in automation: " + name;
			return false;
		}

		double value = 0.0;
		std::string vLower = toLower(valueStr);
		if (vLower.size() >= 2 && vLower.compare(vLower.size() - 2, 2, "db") == 0)
		{
			// dB value: assume the plugin's Gain-style range is unknown here, so
			// interpret as normalized only when no 'dB' suffix; with 'dB' we map
			// through a -60..+24 dB window (matches passthrough). Test cases for
			// other plugins should use normalized values.
			try
			{
				double db = std::stod(valueStr.substr(0, valueStr.size() - 2));
				value = vdplg::dbToNormalized(db, -60.0, 24.0);
			}
			catch (...)
			{
				err = "bad dB value: " + valueStr;
				return false;
			}
		}
		else
		{
			try
			{
				value = std::stod(valueStr);
			}
			catch (...)
			{
				err = "bad value: " + valueStr;
				return false;
			}
		}

		int64 offset = 0;
		std::string tLower = toLower(timeStr);
		if (tLower.size() >= 7 && tLower.compare(tLower.size() - 7, 7, "samples") == 0)
		{
			offset = static_cast<int64>(std::stod(tLower.substr(0, tLower.size() - 7)));
		}
		else if (!tLower.empty() && tLower.back() == 's')
		{
			offset = static_cast<int64>(std::stod(tLower.substr(0, tLower.size() - 1)) * sampleRate);
		}
		else
		{
			offset = static_cast<int64>(std::stod(tLower)); // bare number = samples
		}

		out.push_back({name, value, id == 0 ? 0 : offset});
		out.back().paramId = id;
	}
	return true;
}

// ---------------------------------------------------------------------------
// assertions: "<metric>(<in|out>[,<arg>]) <op> <rhs>"
// metrics: peak, rms, dc, length, spectral_peak(<hz>), thd(<hz>)
// rhs: number, optional unit dBFS / dB, or a metric expression
// ---------------------------------------------------------------------------
struct AssertionResult
{
	std::string text;
	bool ok = false;
	std::string detail;
};

double evalMetric(const std::string& expr, const vdplg::WavFile& in, const vdplg::WavFile& out,
                  int paramCount, std::string& err)
{
	auto parseExpr = [](const std::string& e, std::string& metric, std::string& target,
                        std::string& arg) {
		auto open = e.find('(');
		auto close = e.rfind(')');
		metric = trim(e.substr(0, open));
		std::string inner = (open != std::string::npos && close != std::string::npos)
		                        ? e.substr(open + 1, close - open - 1)
		                        : "";
		auto comma = inner.find(',');
		target = trim(comma == std::string::npos ? inner : inner.substr(0, comma));
		arg = trim(comma == std::string::npos ? "" : inner.substr(comma + 1));
	};

	std::string auto_metric, auto_target, auto_arg;
	parseExpr(trim(expr), auto_metric, auto_target, auto_arg);
	if (toLower(auto_metric) == "param_count")
		return static_cast<double>(paramCount);
	const vdplg::WavFile* wav = nullptr;
	if (toLower(auto_target) == "in")
		wav = &in;
	else if (toLower(auto_target) == "out")
		wav = &out;
	else
	{
		err = "unknown target in metric: " + expr;
		return 0.0;
	}
	if (!wav)
	{
		err = "unknown target in metric: " + expr;
		return 0.0;
	}

	std::string m = toLower(auto_metric);
	if (m == "peak" || m == "rms" || m == "dc")
	{
		auto res = vdplg::analyzeLevels(wav->samples);
		if (m == "peak")
			return res.peak;
		if (m == "rms")
			return res.rms;
		return res.dcOffset;
	}
	if (m == "length")
		return static_cast<double>(wav->frames());
	if (m == "spectral_peak")
	{
		double hz = std::stod(auto_arg);
		return vdplg::fftMagnitudeAtHz(wav->samples, wav->channels, wav->sampleRate, hz);
	}
	if (m == "thd")
	{
		double hz = std::stod(auto_arg);
		return vdplg::thdPercentAtHz(wav->samples, wav->channels, wav->sampleRate, hz);
	}
	err = "unknown metric: " + expr;
	return 0.0;
}

double parseRhs(const std::string& rhs, const vdplg::WavFile& in, const vdplg::WavFile& out,
                 int paramCount,
                std::string& err)
{
	std::string t = trim(rhs);
	std::string tLower = toLower(t);
	// dBFS / dB suffixes are just units of the number itself.
	if (tLower.size() > 4 && tLower.compare(tLower.size() - 4, 4, "dbfs") == 0)
		return std::stod(t.substr(0, t.size() - 4));
	if (tLower.size() > 2 && tLower.compare(tLower.size() - 2, 2, "db") == 0)
		return std::stod(t.substr(0, t.size() - 2));
	if (t.find('(') != std::string::npos)
		return evalMetric(t, in, out, paramCount, err);
	try
	{
		return std::stod(t);
	}
	catch (...)
	{
		err = "bad assertion rhs: " + rhs;
		return 0.0;
	}
}

bool evaluateAssertion(const std::string& text, const vdplg::WavFile& in, const vdplg::WavFile& out,
                       int paramCount,
                       AssertionResult& result)
{
	result.text = text;
	std::string err;

	// find comparison operator
	std::size_t opPos = std::string::npos;
	std::string op;
	for (const char* cand : {"<=", ">=", "==", "!=", "<", ">"})
	{
		auto p = text.find(cand);
		if (p != std::string::npos && (opPos == std::string::npos || p < opPos))
		{
			opPos = p;
			op = cand;
		}
	}
	if (opPos == std::string::npos)
	{
		result.detail = "no comparison operator found";
		return false;
	}

	std::string lhsExpr = trim(text.substr(0, opPos));
	std::string rhsExpr = trim(text.substr(opPos + op.size()));

	double lhs = evalMetric(lhsExpr, in, out, paramCount, err);
	if (!err.empty())
	{
		result.detail = err;
		return false;
	}
	double rhs = parseRhs(rhsExpr, in, out, paramCount, err);
	if (!err.empty())
	{
		result.detail = err;
		return false;
	}

	bool ok = false;
	if (op == "<=")
		ok = lhs <= rhs;
	else if (op == ">=")
		ok = lhs >= rhs;
	else if (op == "<")
		ok = lhs < rhs;
	else if (op == ">")
		ok = lhs > rhs;
	else if (op == "==")
		ok = std::abs(lhs - rhs) < 1e-9;
	else if (op == "!=")
		ok = std::abs(lhs - rhs) >= 1e-9;

	result.ok = ok;
	char buf[128];
	std::snprintf(buf, sizeof(buf), "%s = %.6g, expected %s %.6g", lhsExpr.c_str(), lhs,
	              op.c_str(), rhs);
	result.detail = buf;
	return ok;
}

// ---------------------------------------------------------------------------
// parameter name lookup via the plugin's controller
// ---------------------------------------------------------------------------
// Convert a VST3 String128 (UTF-16) to an UTF-8 std::string.
std::string utf16ToUtf8(const char16* s)
{
	std::string out;
	if (!s)
		return out;
	for (uint32 i = 0; i < 64 && s[i] != 0; ++i)
	{
		uint16 cp = s[i];
		if (cp < 0x80)
			out.push_back(static_cast<char>(cp));
		else if (cp < 0x800)
		{
			out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
		else
		{
			out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
	}
	return out;
}

std::vector<std::pair<std::string, ParamID>> collectParamNames(
    const VST3::Hosting::Module& module, const VST3::UID& processorClassId)
{
	std::vector<std::pair<std::string, ParamID>> names;
	auto component = module.getFactory().createInstance<IComponent>(processorClassId);
	if (!component)
		return names;

	TUID controllerUid {};
	if (component->getControllerClassId(controllerUid) != kResultOk)
		return names;

	auto controller = module.getFactory().createInstance<IEditController>(
	    VST3::UID(controllerUid));
	if (!controller)
		return names;

	if (controller->initialize(nullptr) != kResultOk)
		return names;

	int32 count = controller->getParameterCount();
	for (int32 i = 0; i < count; ++i)
	{
		ParameterInfo info {};
		if (controller->getParameterInfo(i, info) != kResultOk)
			continue;
		std::string title = utf16ToUtf8(info.title);
		names.emplace_back(title, info.id);
	}
	return names;
}

// ---------------------------------------------------------------------------
// test case (JSON)
// ---------------------------------------------------------------------------
struct TestCase
{
	std::string plugin;
	std::string pluginCli; // set when --plugin is given explicitly (overrides case file)
	std::string input;
	int blockSize = 512;
	std::string automation;
	std::string output;
	std::vector<std::string> assertions;
	std::string compareGolden;
	double tolerance = 1e-6;
	bool reportJson = false;
};

bool loadCaseFile(const std::string& path, TestCase& tc, std::string& err)
{
	std::ifstream f(path);
	if (!f)
	{
		err = "cannot open case file: " + path;
		return false;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	if (ss.str().empty())
	{
		err = "case file is empty or unreadable: " + path;
		return false;
	}
	JsonValue root;
	JsonParser parser(ss.str());
	if (!parser.parse(root, err))
	{
		err = "JSON parse error in " + path + ": " + err;
		return false;
	}
	if (root.type != JsonValue::Object)
	{
		err = "case file root must be an object";
		return false;
	}
	if (auto* v = root.find("plugin"))
		tc.plugin = v->str;
	if (auto* v = root.find("input"))
		tc.input = v->str;
	if (auto* v = root.find("blockSize"))
		tc.blockSize = static_cast<int>(v->number);
	if (auto* v = root.find("automation"))
		tc.automation = v->str;
	if (auto* v = root.find("output"))
		tc.output = v->str;
	if (auto* v = root.find("assertions"))
		for (const auto& a : v->array)
			tc.assertions.push_back(a.str);
	if (auto* v = root.find("compareGolden"))
		tc.compareGolden = v->str;
	if (auto* v = root.find("tolerance"))
		tc.tolerance = v->number;
	if (auto* v = root.find("reportJson"))
		tc.reportJson = v->boolean;
	return true;
}

// ---------------------------------------------------------------------------
// the offline run
// ---------------------------------------------------------------------------
struct RunResult
{
	bool ok = false;
	std::string error;
	vdplg::WavFile output; // interleaved (sample-major) samples, like WavFile convention
	uint32 latencySamples = 0;
	int paramCount = 0; // number of parameters registered by the plugin's controller
};

RunResult runOffline(const TestCase& tc)
{
	RunResult rr;

	//--- load input -------------------------------------------------------
	vdplg::WavFile input;
	std::string err;
	if (!vdplg::loadWav(tc.input, input, err))
	{
		rr.error = "failed to load input WAV: " + err;
		return rr;
	}

	//--- load module ------------------------------------------------------
	std::string moduleErr;
	auto module = VST3::Hosting::Module::create(tc.plugin, moduleErr);
	if (!module)
	{
		rr.error = "failed to load plugin module: " + moduleErr;
		return rr;
	}

	//--- find the audio effect class --------------------------------------
	VST3::UID processorClassId;
	bool found = false;
	for (const auto& info : module->getFactory().classInfos())
	{
		if (info.category() == kVstAudioEffectClass)
		{
			processorClassId = info.ID();
			found = true;
			break;
		}
	}
	if (!found)
	{
		rr.error = "no audio effect component found in plugin";
		return rr;
	}

	//--- parameter names (via controller) ---------------------------------
	auto paramNames = collectParamNames(*module, processorClassId);

	rr.paramCount = static_cast<int>(paramNames.size());

	//--- automation events --------------------------------------------------
	std::vector<AutomationEvent> events;
	if (!tc.automation.empty())
	{
		if (!parseAutomation(tc.automation, input.sampleRate, paramNames, events, err))
		{
			rr.error = err;
			return rr;
		}
	}

	//--- instantiate & initialize ------------------------------------------
	auto component = module->getFactory().createInstance<IComponent>(processorClassId);
	if (!component)
	{
		rr.error = "failed to create component instance";
		return rr;
	}
	IAudioProcessor* processorPtr = nullptr;
	if (component->queryInterface(IAudioProcessor::iid, (void**)&processorPtr) != kResultOk)
		processorPtr = nullptr;
	Steinberg::IPtr<IAudioProcessor> processor(processorPtr);
	if (!processor)
	{
		rr.error = "component does not support IAudioProcessor";
		return rr;
	}

	if (component->initialize(nullptr) != kResultOk)
	{
		rr.error = "component initialize failed";
		return rr;
	}

	SpeakerArrangement inArr = SpeakerArr::kStereo;
	SpeakerArrangement outArr = SpeakerArr::kStereo;
	processor->setBusArrangements(&inArr, 1, &outArr, 1);
	component->setIoMode(kOfflineProcessing);

	ProcessSetup setup;
	setup.processMode = kOffline;
	setup.symbolicSampleSize = kSample32;
	setup.maxSamplesPerBlock = tc.blockSize;
	setup.sampleRate = static_cast<SampleRate>(input.sampleRate);
	if (processor->setupProcessing(setup) != kResultOk)
	{
		rr.error = "setupProcessing failed";
		return rr;
	}

	if (component->setActive(true) != kResultOk)
	{
		rr.error = "setActive(true) failed";
		return rr;
	}
	processor->setProcessing(true);
	rr.latencySamples = processor->getLatencySamples();

	//--- pump audio ---------------------------------------------------------
	const int frames = static_cast<int>(input.frames());
	const int channels = input.channels;
	std::vector<float> outPlanar(static_cast<std::size_t>(frames) * channels, 0.0f);

	ParameterChanges paramChanges(4);
	paramChanges.clearQueue();

	for (int blockStart = 0; blockStart < frames; blockStart += tc.blockSize)
	{
		const int numSamples = std::min(tc.blockSize, frames - blockStart);

		AudioBusBuffers inBus;
		inBus.numChannels = channels;
		inBus.channelBuffers32 = new Sample32*[channels];
		for (int ch = 0; ch < channels; ++ch)
			inBus.channelBuffers32[ch] =
			    input.samples.data() + static_cast<std::size_t>(ch) * frames + blockStart;

		AudioBusBuffers outBus;
		outBus.numChannels = channels;
		outBus.channelBuffers32 = new Sample32*[channels];
		for (int ch = 0; ch < channels; ++ch)
			outBus.channelBuffers32[ch] =
			    outPlanar.data() + static_cast<std::size_t>(ch) * frames + blockStart;

		// schedule automation events falling inside this block
		paramChanges.clearQueue();
		bool hasChanges = false;
		for (const auto& ev : events)
		{
			int64 rel = ev.sampleOffset - blockStart;
			if (rel >= 0 && rel < numSamples)
			{
				int32 idx = 0;
				IParamValueQueue* queue = paramChanges.addParameterData(ev.paramId, idx);
				if (queue)
				{
					int32 pidx = 0;
					queue->addPoint(static_cast<int32>(rel), ev.valueNormalized, pidx);
					hasChanges = true;
				}
			}
		}

		ProcessData data;
		data.processMode = kOffline;
		data.symbolicSampleSize = kSample32;
		data.numSamples = numSamples;
		data.inputs = &inBus;
		data.outputs = &outBus;
		data.numInputs = 1;
		data.numOutputs = 1;
		data.inputParameterChanges = hasChanges ? &paramChanges : nullptr;

		if (processor->process(data) != kResultOk)
		{
			delete[] inBus.channelBuffers32;
			delete[] outBus.channelBuffers32;
			rr.error = "process() failed at block starting sample " + std::to_string(blockStart);
			return rr;
		}

		delete[] inBus.channelBuffers32;
		delete[] outBus.channelBuffers32;
	}

	//--- teardown -----------------------------------------------------------
	processor->setProcessing(false);
	component->setActive(false);

	rr.output.sampleRate = input.sampleRate;
	rr.output.channels = channels;
	rr.output.samples = std::move(outPlanar);
	rr.ok = true;
	return rr;
}

// ---------------------------------------------------------------------------
// JSON report
// ---------------------------------------------------------------------------
std::string jsonEscape(const std::string& s)
{
	std::string out;
	for (char c : s)
	{
		switch (c)
		{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\t': out += "\\t"; break;
			default: out.push_back(c); break;
		}
	}
	return out;
}

void printJsonReport(const std::string& caseName, const RunResult& run,
                     const std::vector<AssertionResult>& results, bool goldenChecked,
                     const std::string& goldenError, double tolerance,
                     const vdplg::WavFile* in, const vdplg::WavFile* out)
{
	bool allOk = run.ok && goldenChecked;
	for (const auto& r : results)
		allOk = allOk && r.ok;

	auto levels = out ? vdplg::analyzeLevels(out->samples) : vdplg::AnalysisResult{};

	std::printf("{\n");
	std::printf("  \"case\": \"%s\",\n", jsonEscape(caseName).c_str());
	std::printf("  \"status\": \"%s\",\n", allOk ? "pass" : "fail");
	std::printf("  \"latencySamples\": %u,\n", run.latencySamples);
	if (!run.ok)
		std::printf("  \"error\": \"%s\",\n", jsonEscape(run.error).c_str());
	std::printf("  \"output\": {\"peak\": %.9g, \"rms\": %.9g, \"dcOffset\": %.9g, "
	             "\"hasNaNOrInf\": %s},\n",
	             levels.peak, levels.rms, levels.dcOffset, levels.hasNaNOrInf ? "true" : "false");
	std::printf("  \"assertions\": [\n");
	for (std::size_t i = 0; i < results.size(); ++i)
	{
		const auto& r = results[i];
		std::printf("    {\"text\": \"%s\", \"ok\": %s, \"detail\": \"%s\"}%s\n",
		            jsonEscape(r.text).c_str(), r.ok ? "true" : "false",
		            jsonEscape(r.detail).c_str(), i + 1 < results.size() ? "," : "");
	}
	std::printf("  ],\n");
	std::printf("  \"golden\": {\"checked\": %s, \"tolerance\": %.9g, \"error\": \"%s\"}\n",
	             goldenChecked ? "true" : "false", tolerance, jsonEscape(goldenError).c_str());
	std::printf("}\n");
}

} // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
	TestCase tc;
	std::string caseFile;
	std::vector<std::string> cliAssertions;
	std::string automationCli;
	int blockSize = 512;
	double tolerance = 1e-6;
	bool reportJson = false;

	for (int i = 1; i < argc; ++i)
	{
		std::string a = argv[i];
		auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
		if (a == "--plugin")
		{
			tc.plugin = next();
			tc.pluginCli = tc.plugin;
		}
		else if (a == "--input")
			tc.input = next();
		else if (a == "--blocksize")
			blockSize = std::atoi(next());
		else if (a == "--automation")
			automationCli = next();
		else if (a == "--output")
			tc.output = next();
		else if (a == "--assert")
			cliAssertions.push_back(next());
		else if (a == "--compare-golden")
			tc.compareGolden = next();
		else if (a == "--tolerance")
			tolerance = std::atof(next());
		else if (a == "--report")
		{
			std::string mode = next();
			reportJson = (toLower(mode) == "json");
		}
		else if (a == "--case")
			caseFile = next();
		else
		{
			std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
			return 2;
		}
	}

	std::string caseName = caseFile;
	if (!caseFile.empty())
	{
		std::string err;
		if (!loadCaseFile(caseFile, tc, err))
		{
			std::fprintf(stderr, "ERROR: %s\n", err.c_str());
			return 2;
		}
		// An explicit --plugin on the command line overrides the case file
		// (CTest passes --plugin so each case can target its own bundle).
		if (!tc.pluginCli.empty())
			tc.plugin = tc.pluginCli;
		// Resolve relative paths in the case file against its own directory so
		// CTest can run from any working directory.
		auto dirOf = [](std::string p) {
			auto pos = p.find_last_of("/\\");
			return (pos == std::string::npos) ? std::string(".") : p.substr(0, pos);
		};
		auto joinPath = [](const std::string& a, const std::string& b) {
			if (a.empty() || a == ".")
				return b;
			char last = a.back();
			if (last == '/' || last == '\\')
				return a + b;
			return a + "/" + b;
		};
		auto isRelative = [](const std::string& p) {
			return p.size() < 2 || (p[1] != ':' && p[0] != '/' && p[0] != '\\');
		};
		std::string caseDir = dirOf(caseFile);
		// NOTE: guard against empty strings — joinPath(dir, "") would produce
		// "dir/" which later looks like a valid (but bogus) relative path.
		if (!tc.input.empty() && isRelative(tc.input))
			tc.input = joinPath(caseDir, tc.input);
		if (!tc.plugin.empty() && isRelative(tc.plugin))
			tc.plugin = joinPath(caseDir, tc.plugin);
		if (!tc.compareGolden.empty() && isRelative(tc.compareGolden))
			tc.compareGolden = joinPath(caseDir, tc.compareGolden);
		if (!tc.output.empty() && isRelative(tc.output))
			tc.output = joinPath(caseDir, tc.output);
	}
	else
	{
		tc.automation = automationCli;
		tc.blockSize = blockSize;
		tc.assertions = cliAssertions;
		tc.tolerance = tolerance;
	}

	if (tc.plugin.empty() || tc.input.empty())
	{
		std::fprintf(stderr,
		             "usage: vst3testhost --case <file.json>\n"
		             "       vst3testhost --plugin <x.vst3> --input <in.wav> [options]\n");
		return 2;
	}

	//--- run ----------------------------------------------------------------
	RunResult run = runOffline(tc);

	std::vector<AssertionResult> results;
	std::string goldenError;
	bool goldenChecked = true;

	if (run.ok)
	{
		vdplg::WavFile inWav;
		std::string err;
		vdplg::loadWav(tc.input, inWav, err);

		for (const auto& a : tc.assertions)
		{
			AssertionResult ar;
			evaluateAssertion(a, inWav, run.output, run.paramCount, ar);
			results.push_back(std::move(ar));
		}

		if (!tc.compareGolden.empty())
		{
			vdplg::WavFile golden;
			if (!vdplg::loadWav(tc.compareGolden, golden, err))
			{
				goldenChecked = false;
				goldenError = "failed to load golden WAV: " + err;
			}
			else
			{
				std::string cmpErr;
				if (!vdplg::compareGolden(run.output.samples, golden.samples, tolerance, cmpErr))
				{
					goldenChecked = false;
					goldenError = cmpErr;
				}
			}
		}

		if (!tc.output.empty())
		{
			std::string saveErr;
			if (!vdplg::saveWav(tc.output, run.output, saveErr))
			{
				std::fprintf(stderr, "WARNING: failed to save output WAV: %s\n", saveErr.c_str());
			}
		}
	}

	//--- report ---------------------------------------------------------------
	if (reportJson)
	{
		printJsonReport(caseName, run, results, goldenChecked, goldenError, tolerance, nullptr,
		                run.ok ? &run.output : nullptr);
	}
	else
	{
		if (!run.ok)
			std::printf("FAIL: %s\n", run.error.c_str());
		for (const auto& r : results)
			std::printf("%s: %s (%s)\n", r.ok ? "PASS" : "FAIL", r.text.c_str(),
			            r.detail.c_str());
		if (!tc.compareGolden.empty() && !goldenChecked)
			std::printf("FAIL: golden comparison: %s\n", goldenError.c_str());

		bool allOk = run.ok && goldenChecked;
		for (const auto& r : results)
			allOk = allOk && r.ok;
		std::printf(allOk ? "RESULT: PASS\n" : "RESULT: FAIL\n");
	}

	bool allOk = run.ok && goldenChecked;
	for (const auto& r : results)
		allOk = allOk && r.ok;
	return allOk ? 0 : 1;
}
