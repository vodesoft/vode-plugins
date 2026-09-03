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
#if defined(_WIN32)
// windows.h min/max macros break std::min below.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h> // CoInitializeEx / CoUninitialize
#endif
#include <sstream>
#include <string>
#include <map>
#include <vector>

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include "vdplg/analysis.h"
#include "vdplg/png.h"
#include "pluginterfaces/vst/ivstmessage.h" // IMessage, IConnectionPoint
#include "pluginterfaces/gui/iplugview.h"   // IPlugView, IPlugFrame, kPlatformTypeHWND
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
// UI screenshot capture (hidden editor window + notify-based PNG render)
// ---------------------------------------------------------------------------
struct ScreenshotSpec
{
	std::string at;   // "end" | "<seconds>s" | "<samples>"
	std::string file; // output PNG path
};

// Golden-image regression compare (F4): captured shot vs committed baseline.
struct GoldenUiSpec
{
	std::string file;          // captured screenshot to check
	std::string ref;           // baseline PNG under testdata/golden/
	double maxFraction = 0.01; // fail if diff fraction exceeds this
	int tolerance = 8;         // per-channel byte tolerance
};

// TChar is Steinberg::char16 (not wchar_t): convert byte-by-byte (ASCII paths).
std::basic_string<TChar> toTCharStr(const std::string& s)
{
	std::basic_string<TChar> out;
	out.reserve(s.size());
	for (unsigned char ch : s)
		out += static_cast<TChar>(ch);
	return out;
}

class ScreenshotMessage : public FUnknown, public IMessage
{
public:
	explicit ScreenshotMessage(FIDString id)
	{
		strncpy_s(id_, sizeof(id_), id, _TRUNCATE);
	}

	uint32 PLUGIN_API addRef() override { return ++refCount_; }
	uint32 PLUGIN_API release() override
	{
		if (--refCount_ == 0)
		{
			delete this;
			return 0;
		}
		return refCount_;
	}
	tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
	{
		if (iid == FUnknown::iid || iid == IMessage::iid)
		{
			addRef();
			*obj = static_cast<IMessage*>(this);
			return kResultTrue;
		}
		*obj = nullptr;
		return kResultFalse;
	}

	FIDString PLUGIN_API getMessageID() override { return id_; }
	void PLUGIN_API setMessageID(FIDString id) override
	{
		strncpy_s(id_, sizeof(id_), id, _TRUNCATE);
	}
	IAttributeList* PLUGIN_API getAttributes() override { return &attrs_; }

	class Attrs : public FUnknown, public IAttributeList
	{
	public:
		explicit Attrs(ScreenshotMessage* owner) : owner_(owner) {}
		uint32 PLUGIN_API addRef() override { return ++owner_->refCount_; }
		uint32 PLUGIN_API release() override { return owner_->release(); }
		tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
		{
			if (iid == FUnknown::iid || iid == IAttributeList::iid)
			{
				addRef();
				*obj = static_cast<IAttributeList*>(this);
				return kResultTrue;
			}
			*obj = nullptr;
			return kResultFalse;
		}

		void setString(AttrID key, const std::string& value)
		{
			strings_[key] = value;
		}

		tresult PLUGIN_API setInt(AttrID, int64) override { return kResultFalse; }
		tresult PLUGIN_API getInt(AttrID, int64&) override { return kResultFalse; }
		tresult PLUGIN_API setFloat(AttrID, double) override { return kResultFalse; }
		tresult PLUGIN_API getFloat(AttrID, double&) override { return kResultFalse; }
		tresult PLUGIN_API setString(AttrID id, const TChar* str) override
		{
			if (!id || !str)
				return kInvalidArgument;
			std::string key(id);
			std::string value;
			for (uint32 i = 0; str[i]; ++i)
				value += static_cast<char>(str[i]);
			strings_[key] = std::move(value);
			return kResultTrue;
		}
		tresult PLUGIN_API getString(AttrID id, TChar* out, uint32 sizeInBytes) override
		{
			if (!id || !out)
				return kInvalidArgument;
			std::string key(id);
			auto it = strings_.find(key);
			if (it == strings_.end())
				return kResultFalse;
			const auto& s = it->second;
			uint32 n = static_cast<uint32>(std::min(s.size(),
			                                         static_cast<size_t>(sizeInBytes / sizeof(TChar)) - 1));
			for (uint32 i = 0; i < n; ++i)
				out[i] = static_cast<TChar>(s[i]);
			out[n] = 0;
			return kResultTrue;
		}
		tresult PLUGIN_API setBinary(AttrID, const void*, uint32) override { return kResultFalse; }
		tresult PLUGIN_API getBinary(AttrID, const void*& data, uint32& size) override
		{
			data = nullptr;
			size = 0;
			return kResultFalse;
		}

	private:
		ScreenshotMessage* owner_;
		std::map<std::string, std::string> strings_;
		friend class ScreenshotMessage;
	};

private:
	char id_[64] {};
	uint32 refCount_ = 1;
	Attrs attrs_ {this};
};

#if defined(_WIN32)
// Host-side IPlugFrame: VSTGUIEditor::open() calls plugFrame->resizeView()
// when editing is disabled. A no-op implementation satisfies that call.
class HostPlugFrame : public FUnknown, public IPlugFrame
{
public:
	uint32 PLUGIN_API addRef() override { return ++refCount_; }
	uint32 PLUGIN_API release() override
	{
		if (--refCount_ == 0)
		{
			delete this;
			return 0;
		}
		return refCount_;
	}
	tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
	{
		if (iid == FUnknown::iid || iid == IPlugFrame::iid)
		{
			addRef();
			*obj = static_cast<IPlugFrame*>(this);
			return kResultTrue;
		}
		*obj = nullptr;
		return kResultFalse;
	}
	tresult PLUGIN_API resizeView(IPlugView*, ViewRect*) override { return kResultTrue; }

private:
	uint32 refCount_ = 1;
};
#else
class HostPlugFrame
{
public:
	tresult PLUGIN_API resizeView(void*, void*) { return kResultTrue; }
	void release() {}
};
#endif // _WIN32

struct UiCapture
{
	Steinberg::IPtr<IEditController> controller;
	Steinberg::IPtr<IPlugView> view;
	HostPlugFrame* plugFrame_ = nullptr;
	IConnectionPoint* compCp_ = nullptr;
	IConnectionPoint* ctrlCp_ = nullptr;
#if defined(_WIN32)
	HWND hwnd = nullptr;
#endif
	bool open = false;
	std::vector<bool> fired;

	static int64 parseAt(const std::string& at, double sampleRate)
	{
		if (at == "end")
			return INT64_MAX;
		try
		{
			std::size_t pos = 0;
			double v = std::stod(at, &pos);
			if (pos < at.size() && at[pos] == 's')
				return static_cast<int64>(v * sampleRate);
			return static_cast<int64>(v); // bare number = samples
		}
		catch (...)
		{
			return -1;
		}
	}

	bool init(VST3::Hosting::Module::Ptr module, const TUID& controllerUid,
	          IComponent* component)
	{
		controller = module->getFactory().createInstance<IEditController>(
		    VST3::UID(controllerUid));
		if (!controller)
			return false;
		if (controller->initialize(nullptr) != kResultOk)
			return false;

#if defined(_WIN32)
		WNDCLASSEXA wc {};
		wc.cbSize = sizeof(WNDCLASSEXA);
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = DefWindowProcA;
		wc.hInstance = GetModuleHandleA(nullptr);
		wc.lpszClassName = "vdplg_testhost_ui";
		RegisterClassExA(&wc);
		hwnd = CreateWindowExA(WS_EX_TOOLWINDOW, "vdplg_testhost_ui", "", WS_POPUP,
		                        -20000, -20000, 720, 560, nullptr, nullptr,
		                        GetModuleHandleA(nullptr), nullptr);
		if (!hwnd)
			return false;
#endif

		// Connect component <-> controller via IConnectionPoint so the
		// controller receives parameter changes and can send messages back.
		void* obj = nullptr;
		if (component->queryInterface(IConnectionPoint::iid, &obj) != kResultOk || !obj)
			return false;
		compCp_ = static_cast<IConnectionPoint*>(obj);
		obj = nullptr;
		if (controller->queryInterface(IConnectionPoint::iid, &obj) != kResultOk || !obj)
		{
			compCp_->release();
			compCp_ = nullptr;
			return false;
		}
		ctrlCp_ = static_cast<IConnectionPoint*>(obj);
		compCp_->connect(ctrlCp_);
		ctrlCp_->connect(compCp_);

		view = controller->createView("editor");
		if (!view)
		{
			shutdown();
			return false;
		}

		plugFrame_ = new HostPlugFrame();
		view->setFrame(plugFrame_);

		if (view->attached(reinterpret_cast<void*>(hwnd), Steinberg::kPlatformTypeHWND) != kResultOk)
		{
			shutdown();
			return false;
		}
		open = true;
		return true;
	}

	std::vector<std::size_t> dueShots(const std::vector<ScreenshotSpec>& shots,
	                                  const int64* atSamples, int blockStart) const
	{
		std::vector<std::size_t> due;
		for (std::size_t i = 0; i < shots.size(); ++i)
		{
			if (fired[i])
				continue;
			const int64 t = atSamples[i];
			if (t <= 0 || t >= INT64_MAX / 2) // skip invalid + "end"
				continue;
			if (blockStart >= t)
				due.push_back(i);
		}
		return due;
	}

	bool fire(std::size_t index, const std::string& path)
	{
#if defined(_WIN32)
		auto msg = new ScreenshotMessage("vdplg.debug.screenshot");
		msg->getAttributes()->setString("path", toTCharStr(path).c_str());
		tresult res = ctrlCp_->notify(msg);
		msg->release();
		fired[index] = true;
		return res == kResultTrue;
#else
		(void)index;
		(void)path;
		return false;
#endif
	}

	bool firedAt(std::size_t index) const { return fired[index]; }

	void shutdown()
	{
		if (view)
		{
			view->removed();
			view = nullptr;
		}
		if (plugFrame_)
		{
			plugFrame_->release();
			plugFrame_ = nullptr;
		}
		if (compCp_ && ctrlCp_)
		{
			compCp_->disconnect(ctrlCp_);
			ctrlCp_->disconnect(compCp_);
		}
		if (ctrlCp_)
		{
			ctrlCp_->release();
			ctrlCp_ = nullptr;
		}
		if (compCp_)
		{
			compCp_->release();
			compCp_ = nullptr;
		}
		controller = nullptr;
#if defined(_WIN32)
		// NOTE: intentionally NOT calling DestroyWindow/CoUninitialize here.
		// Tearing down the window while the plugin's VSTGUI frame may still
		// reference it triggers an AV at process exit; the OS reclaims both
		// at exit. (KnownIssue: exit-time heap corruption family.)
#endif
		open = false;
	}
};

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
	bool ui = false;
	std::vector<ScreenshotSpec> screenshots;
	std::vector<std::string> mustDiffer; // files that must pairwise-differ
	GoldenUiSpec goldenUi;               // optional golden-image compare (F4)
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
	if (auto* v = root.find("ui"))
		tc.ui = v->boolean;
	if (auto* v = root.find("screenshots"))
		for (const auto& s : v->array)
		{
			ScreenshotSpec shot;
			if (auto* at = s.find("at"))
				shot.at = at->str;
			if (auto* file = s.find("file"))
				shot.file = file->str;
			tc.screenshots.push_back(std::move(shot));
		}
	if (auto* v = root.find("mustDiffer"))
		for (const auto& f : v->array)
			tc.mustDiffer.push_back(f.str);
	if (auto* v = root.find("goldenUi"))
	{
		if (auto* f = v->find("file"))
			tc.goldenUi.file = f->str;
		if (auto* r = v->find("ref"))
			tc.goldenUi.ref = r->str;
		if (auto* m = v->find("maxFraction"))
			tc.goldenUi.maxFraction = m->number;
		if (auto* t = v->find("tolerance"))
			tc.goldenUi.tolerance = static_cast<int>(t->number);
	}
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
	struct Shot
	{
		std::string file;
		bool valid = false;
	};
	std::vector<Shot> shots;
	struct Diff
	{
		std::string a;
		std::string b;
		bool differ = false; // true when the two files are not byte-identical
	};
	std::vector<Diff> diffs;
	struct Golden
	{
		std::string file;
		std::string ref;
		double maxFraction = 0.01;
		int tolerance = 8;
		bool ok = false;
		double diffFraction = 0.0;
	};
	std::vector<Golden> goldens;
};

// Minimal PNG sanity check: signature, IHDR dimensions, minimum size.
bool validateScreenshot(const std::string& path, int expectW, int expectH, std::string& err)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
	{
		err = "cannot open screenshot: " + path;
		return false;
	}
	std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	f.close();
	const unsigned char pngSig[] = {137, 80, 78, 71, 13, 10, 26, 10};
	if (buf.size() < 24 || std::memcmp(buf.data(), pngSig, 8) != 0)
	{
		err = "not a PNG file: " + path;
		return false;
	}
	if (std::memcmp(buf.data() + 12, "IHDR", 4) != 0)
	{
		err = "missing IHDR chunk in " + path;
		return false;
	}
	auto be32 = [&](size_t off) {
		return (static_cast<uint32>(static_cast<unsigned char>(buf[off])) << 24) |
		       (static_cast<uint32>(static_cast<unsigned char>(buf[off + 1])) << 16) |
		       (static_cast<uint32>(static_cast<unsigned char>(buf[off + 2])) << 8) |
		       static_cast<uint32>(static_cast<unsigned char>(buf[off + 3]));
	};
	if (be32(16) != static_cast<uint32>(expectW) || be32(20) != static_cast<uint32>(expectH))
	{
		err = "unexpected screenshot size " + std::to_string(be32(16)) + "x" +
		      std::to_string(be32(20)) + ", expected " + std::to_string(expectW) + "x" +
		      std::to_string(expectH);
		return false;
	}
	if (buf.size() < 5 * 1024)
	{
		err = "suspiciously small PNG (" + std::to_string(buf.size()) + " bytes): " + path;
		return false;
	}
	return true;
}

// Byte-level compare of two captured PNGs: proves the UI actually changed
// between two capture points (e.g. channel-mode labels).
bool pngFilesDiffer(const std::string& a, const std::string& b, std::string& err)
{
	auto readAll = [](const std::string& p, std::vector<char>& out, std::string& e) {
		std::ifstream f(p, std::ios::binary);
		if (!f)
		{
			e = "cannot open " + p;
			return false;
		}
		out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		return true;
	};
	std::vector<char> ba, bb;
	if (!readAll(a, ba, err))
		return false;
	if (!readAll(b, bb, err))
		return false;
	return ba != bb;
}

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
#if defined(_WIN32)
	// The plugin DLL's VSTGUI static initializers call CoCreateInstance during
	// DllMain, so the COM apartment must exist BEFORE the DLL is loaded.
	if (tc.ui)
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
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

	//--- open hidden editor for screenshot capture --------------------------
	UiCapture ui;
	ui.fired.assign(tc.screenshots.size(), false);
	if (tc.ui && !tc.screenshots.empty())
	{
		TUID controllerUid {};
		if (component->getControllerClassId(controllerUid) != kResultOk ||
		    !ui.init(module, controllerUid, component.get()))
		{
			ui.shutdown();
			rr.error = "failed to open hidden editor";
			return rr;
		}
	}
	std::vector<int64> atSamples;
	for (const auto& s : tc.screenshots)
		atSamples.push_back(UiCapture::parseAt(s.at, input.sampleRate));

	//--- pump audio ---------------------------------------------------------
	const int frames = static_cast<int>(input.frames());
	const int channels = input.channels;
	std::vector<float> outPlanar(static_cast<std::size_t>(frames) * channels, 0.0f);

	ParameterChanges paramChanges(4);
	paramChanges.clearQueue();

	for (int blockStart = 0; blockStart < frames; blockStart += tc.blockSize)
	{
		const int numSamples = std::min(tc.blockSize, frames - blockStart);

		// fire any screenshots whose time has come
		for (std::size_t i : ui.dueShots(tc.screenshots, atSamples.data(), blockStart))
			rr.shots.push_back({tc.screenshots[i].file, ui.fire(i, tc.screenshots[i].file)});

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
				// keep the editor in sync with the automated value
				if (ui.open)
					ui.controller->setParamNormalized(ev.paramId, ev.valueNormalized);
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
	// "end" screenshots fire after the last block was processed
	for (std::size_t i = 0; i < tc.screenshots.size(); ++i)
	{
		if (!ui.firedAt(i) && atSamples[i] >= INT64_MAX / 2)
			rr.shots.push_back({tc.screenshots[i].file, ui.fire(i, tc.screenshots[i].file)});
	}

	// validate captured PNGs
	for (auto& s : rr.shots)
	{
		std::string vErr;
		s.valid = ui.open && validateScreenshot(s.file, 720, 560, vErr);
		if (!s.valid)
			s.file += " [" + vErr + "]";
	}

	// evaluate mustDiffer pairs: both files must exist and NOT be identical
	for (const auto& fa : tc.mustDiffer)
		for (const auto& fb : tc.mustDiffer)
		{
			if (fa >= fb)
				continue;
			RunResult::Diff d {fa, fb};
			std::string dErr;
			d.differ = pngFilesDiffer(fa, fb, dErr);
			if (!d.differ)
				d.a += " [" + dErr + "]";
			rr.diffs.push_back(std::move(d));
		}

	// evaluate golden-image compares (F4): captured shot vs committed baseline
	if (!tc.goldenUi.file.empty() && !tc.goldenUi.ref.empty())
	{
		RunResult::Golden g {tc.goldenUi.file, tc.goldenUi.ref, tc.goldenUi.maxFraction,
		                     tc.goldenUi.tolerance};
		std::string gErr;
		bool within = false;
		const bool cmpOk = vdplg::compareGoldenUi(g.file, g.ref, g.maxFraction, g.tolerance, within,
		                              g.diffFraction, gErr);
		g.ok = cmpOk && within;
		if (!g.ok && gErr.empty())
			gErr = "pixel diff exceeds limit";
		if (!g.ok)
			g.file += " [" + gErr + "]";
		rr.goldens.push_back(std::move(g));
	}

	ui.shutdown();
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
	for (const auto& s : run.shots)
		allOk = allOk && s.valid;
	for (const auto& d : run.diffs)
		allOk = allOk && d.differ;
	for (const auto& g : run.goldens)
		allOk = allOk && g.ok;

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
	if (!run.shots.empty())
	{
		std::printf("  \"screenshots\": [\n");
		for (std::size_t i = 0; i < run.shots.size(); ++i)
			std::printf("    {\"file\": \"%s\", \"ok\": %s}%s\n",
			            jsonEscape(run.shots[i].file).c_str(),
			            run.shots[i].valid ? "true" : "false",
			            i + 1 < run.shots.size() ? "," : "");
		std::printf("  ],\n");
	}
	if (!run.diffs.empty())
	{
		std::printf("  \"mustDiffer\": [\n");
		for (std::size_t i = 0; i < run.diffs.size(); ++i)
			std::printf("    {\"a\": \"%s\", \"b\": \"%s\", \"ok\": %s}%s\n",
			            jsonEscape(run.diffs[i].a).c_str(),
			            jsonEscape(run.diffs[i].b).c_str(),
			            run.diffs[i].differ ? "true" : "false",
			            i + 1 < run.diffs.size() ? "," : "");
		std::printf("  ],\n");
	}
		if (!run.goldens.empty())
		{
			std::printf("  \"goldenUi\": [\n");
			for (std::size_t i = 0; i < run.goldens.size(); ++i)
				std::printf("    {\"file\": \"%s\", \"ref\": \"%s\", \"ok\": %s, "
				            "\"diffFraction\": %.9g, \"maxFraction\": %.9g}%s\n",
				            jsonEscape(run.goldens[i].file).c_str(),
				            jsonEscape(run.goldens[i].ref).c_str(),
				            run.goldens[i].ok ? "true" : "false",
				            run.goldens[i].diffFraction, run.goldens[i].maxFraction,
				            i + 1 < run.goldens.size() ? "," : "");
			std::printf("  ],\n");
		}
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
	bool uiCli = false;
	std::vector<std::string> shotCli;
	std::string goldenRefCli; // --compare-golden-ui <ref.png> (F4)

	for (int i = 1; i < argc; ++i)
	{
		std::string a = argv[i];
		auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
		if (a == "--plugin")
		{
			tc.plugin = next();
			tc.pluginCli = tc.plugin;
		}
		else if (a == "--ui")
			uiCli = true;
		else if (a == "--compare-golden-ui")
			goldenRefCli = next();
		else if (a == "--screenshot")
			shotCli.push_back(next());
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
		for (auto& s : tc.screenshots)
			if (!s.file.empty() && isRelative(s.file))
				s.file = joinPath(caseDir, s.file);
		for (auto& f : tc.mustDiffer)
			if (!f.empty() && isRelative(f))
				f = joinPath(caseDir, f);
		if (!tc.goldenUi.file.empty() && isRelative(tc.goldenUi.file))
			tc.goldenUi.file = joinPath(caseDir, tc.goldenUi.file);
		if (!tc.goldenUi.ref.empty() && isRelative(tc.goldenUi.ref))
			tc.goldenUi.ref = joinPath(caseDir, tc.goldenUi.ref);
	}
	else
	{
		tc.automation = automationCli;
		tc.blockSize = blockSize;
		tc.assertions = cliAssertions;
		tc.tolerance = tolerance;
	}
	if (uiCli)
		tc.ui = true;
	for (const auto& p : shotCli)
		tc.screenshots.push_back({"end", p});
	if (!goldenRefCli.empty())
	{
		// CLI form: compare the last scheduled capture against the baseline.
		if (tc.goldenUi.ref.empty())
			tc.goldenUi.ref = goldenRefCli;
		if (tc.goldenUi.file.empty() && !tc.screenshots.empty())
			tc.goldenUi.file = tc.screenshots.back().file;
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
				goldenError = "failed to load golden WAV: " + err;
				goldenChecked = false;
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
		for (const auto& s : run.shots)
			std::printf("%s: screenshot %s\n", s.valid ? "PASS" : "FAIL", s.file.c_str());
		for (const auto& d : run.diffs)
			std::printf("%s: differ %s vs %s\n", d.differ ? "PASS" : "FAIL",
			            d.a.c_str(), d.b.c_str());
		for (const auto& g : run.goldens)
			std::printf("%s: golden-ui %s vs %s (diff %.9g, max %.9g)\n",
			            g.ok ? "PASS" : "FAIL", g.file.c_str(), g.ref.c_str(),
			            g.diffFraction, g.maxFraction);

		bool allOk = run.ok && goldenChecked;
		for (const auto& r : results)
			allOk = allOk && r.ok;
		for (const auto& s : run.shots)
			allOk = allOk && s.valid;
		for (const auto& d : run.diffs)
			allOk = allOk && d.differ;
		for (const auto& g : run.goldens)
			allOk = allOk && g.ok;
		std::printf(allOk ? "RESULT: PASS\n" : "RESULT: FAIL\n");
	}

	bool allOk = run.ok && goldenChecked;
	for (const auto& r : results)
		allOk = allOk && r.ok;
	for (const auto& s : run.shots)
		allOk = allOk && s.valid;
	for (const auto& d : run.diffs)
		allOk = allOk && d.differ;
	for (const auto& g : run.goldens)
		allOk = allOk && g.ok;
	return allOk ? 0 : 1;
}
