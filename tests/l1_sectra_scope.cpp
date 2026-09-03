// Vode Plugins — L1 tests: Sectra Scope processor driven through process().
//
// Instantiates vdplg::sectrascope::Processor directly (no DAW, no factory)
// and feeds synthetic buffers through the VST3 process() path. Phase 2
// (TDD Red): passthrough + six-parameter plumbing must already work; the
// analyzer wiring lands in Phase 3.

#define _USE_MATH_DEFINES
#ifndef NOMINMAX
# define NOMINMAX
#endif
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <objbase.h> // CoInitializeEx (excluded by WIN32_LEAN_AND_MEAN)
#endif

#include "SectraScopeProcessor.h"
#include "Controller.h"
#include "ScopeView.h"
#include "scopedata.h"
#include "sectraparamids.h"

#include "public.sdk/source/vst/vstparameters.h"
#include "vstgui/uidescription/uiattributes.h"
#include "vstgui/lib/cframe.h"
#include "vstgui/lib/coffscreencontext.h"
#include "vstgui/lib/platform/platformfactory.h"
#ifdef _WIN32
#  include "vstgui/lib/platform/win32/win32factory.h"
#endif
#include "vstgui/lib/vstguiinit.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace vdplg::sectrascope;
using Catch::Approx;

namespace {

constexpr int32 kNumChannels = 2;
constexpr int32 kNumSamples = 512;
constexpr double kSampleRate = 44100.0;

// One-time-per-process headless VSTGUI setup (defined below with the other
// [ui] helpers).
void ensureVstguiReady ();

std::vector<float> makeSignalPlanar(int32 numSamples, int32 numChannels)
{
	std::vector<float> buf(static_cast<std::size_t>(numSamples) * numChannels, 0.0f);
	const double freqs[2] = {440.0, 880.0};
	for (int32 ch = 0; ch < numChannels; ++ch)
		for (int32 s = 0; s < numSamples; ++s)
			buf[static_cast<std::size_t>(ch) * numSamples + s] =
			    static_cast<float>(std::sin(2.0 * M_PI * freqs[ch] * s / kSampleRate)) * 0.5f;
	return buf;
}

std::vector<float> toInterleaved(const std::vector<float>& planar,
                                 int32 numSamples, int32 numChannels)
{
	std::vector<float> out(static_cast<std::size_t>(numSamples) * numChannels);
	for (int32 s = 0; s < numSamples; ++s)
		for (int32 ch = 0; ch < numChannels; ++ch)
			out[static_cast<std::size_t>(s) * numChannels + ch] =
			    planar[static_cast<std::size_t>(ch) * numSamples + s];
	return out;
}

class TestValueQueue : public FUnknown, public IParamValueQueue
{
public:
	TestValueQueue(ParamID id, int32 sampleOffset, ParamValue value)
	{
		id_ = id;
		points_.push_back({sampleOffset, value});
	}

	//--- IParamValueQueue ----------------------------------------------
	ParamID PLUGIN_API getParameterId() SMTG_OVERRIDE { return id_; }
	int32 PLUGIN_API getPointCount() SMTG_OVERRIDE { return static_cast<int32>(points_.size()); }
	tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& value) SMTG_OVERRIDE
	{
		if (index < 0 || index >= static_cast<int32>(points_.size()))
			return kResultFalse;
		sampleOffset = points_[index].offset;
		value = points_[index].value;
		return kResultTrue;
	}
	tresult PLUGIN_API addPoint(int32 sampleOffset, ParamValue value, int32& index) SMTG_OVERRIDE
	{
		index = static_cast<int32>(points_.size());
		points_.push_back({sampleOffset, value});
		return kResultOk;
	}

	//--- FUnknown --------------------------------------------------------
	tresult PLUGIN_API queryInterface(const TUID iid, void** obj) SMTG_OVERRIDE
	{
		if (iid == FUnknown::iid || iid == IParamValueQueue::iid)
		{
			addRef();
			*obj = static_cast<IParamValueQueue*>(this);
			return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}
	uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refCount_; }
	uint32 PLUGIN_API release() SMTG_OVERRIDE
	{
		uint32 r = --refCount_;
		if (r == 0)
			delete this;
		return r;
	}

private:
	struct Point
	{
		int32 offset;
		ParamValue value;
	};
	ParamID id_ {0};
	std::vector<Point> points_;
	uint32 refCount_ {1};
};

class TestParameterChanges : public FUnknown, public IParameterChanges
{
public:
	void addPoint(ParamID id, int32 sampleOffset, ParamValue value)
	{
		queues_.push_back(new TestValueQueue(id, sampleOffset, value));
	}

	//--- IParameterChanges ---------------------------------------------
	int32 PLUGIN_API getParameterCount() SMTG_OVERRIDE { return static_cast<int32>(queues_.size()); }
	IParamValueQueue* PLUGIN_API getParameterData(int32 index) SMTG_OVERRIDE
	{
		if (index < 0 || index >= static_cast<int32>(queues_.size()))
			return nullptr;
		// Caller takes ownership of a reference (VST3 convention).
		queues_[index]->addRef();
		return queues_[index];
	}
	IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) SMTG_OVERRIDE
	{
		auto* q = new TestValueQueue(id, 0, 0.0);
		index = static_cast<int32>(queues_.size());
		queues_.push_back(q);
		return q;
	}

	//--- FUnknown --------------------------------------------------------
	tresult PLUGIN_API queryInterface(const TUID iid, void** obj) SMTG_OVERRIDE
	{
		if (iid == FUnknown::iid || iid == IParameterChanges::iid)
		{
			addRef();
			*obj = static_cast<IParameterChanges*>(this);
			return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}
	uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refCount_; }
	uint32 PLUGIN_API release() SMTG_OVERRIDE
	{
		uint32 r = --refCount_;
		if (r == 0)
		{
			for (auto* q : queues_)
				q->release();
			delete this;
		}
		return r;
	}

private:
	std::vector<IParamValueQueue*> queues_;
	uint32 refCount_ {1};
};

struct ProcessResult
{
	tresult initResult = kResultFalse;
	tresult processResult = kResultFalse;
	std::vector<float> output; // interleaved
};

ProcessResult runBlock(Processor& proc, const std::vector<float>& input,
                       const std::vector<std::pair<ParamID, ParamValue>>& paramPoints)
{
	ProcessResult res;

	res.initResult = proc.initialize(nullptr);
	if (res.initResult != kResultOk)
		return res;

	SpeakerArrangement inArr = SpeakerArr::kStereo;
	SpeakerArrangement outArr = SpeakerArr::kStereo;
	proc.setBusArrangements(&inArr, 1, &outArr, 1);

	ProcessSetup setup;
	setup.processMode = kOffline;
	setup.symbolicSampleSize = kSample32;
	setup.maxSamplesPerBlock = kNumSamples;
	setup.sampleRate = static_cast<SampleRate>(kSampleRate);
	proc.setupProcessing(setup);
	proc.setActive(true);

	std::vector<float> outBuf(static_cast<std::size_t>(kNumSamples) * kNumChannels, 0.0f);

	AudioBusBuffers inBus;
	inBus.numChannels = kNumChannels;
	inBus.channelBuffers32 = new Sample32*[kNumChannels];
	for (int32 ch = 0; ch < kNumChannels; ++ch)
		inBus.channelBuffers32[ch] =
		    const_cast<Sample32*>(reinterpret_cast<const Sample32*>(input.data())) +
		    static_cast<std::size_t>(ch) * kNumSamples;

	AudioBusBuffers outBus;
	outBus.numChannels = kNumChannels;
	outBus.channelBuffers32 = new Sample32*[kNumChannels];
	for (int32 ch = 0; ch < kNumChannels; ++ch)
		outBus.channelBuffers32[ch] =
		    reinterpret_cast<Sample32*>(outBuf.data()) + static_cast<std::size_t>(ch) * kNumSamples;

	ProcessData data;
	data.processMode = kOffline;
	data.symbolicSampleSize = kSample32;
	data.numSamples = kNumSamples;
	data.inputs = &inBus;
	data.outputs = &outBus;
	data.numInputs = 1;
	data.numOutputs = 1;

	if (!paramPoints.empty())
	{
		TestParameterChanges* changes = new TestParameterChanges();
		for (const auto& [id, value] : paramPoints)
			changes->addPoint(id, 0, value);
		data.inputParameterChanges = changes;
	}

	res.processResult = proc.process(data);

	res.output.resize(static_cast<std::size_t>(kNumSamples) * kNumChannels);
	for (int32 s = 0; s < kNumSamples; ++s)
		for (int32 ch = 0; ch < kNumChannels; ++ch)
			res.output[static_cast<std::size_t>(s) * kNumChannels + ch] =
			    outBuf[static_cast<std::size_t>(ch) * kNumSamples + s];

	delete[] inBus.channelBuffers32;
	delete[] outBus.channelBuffers32;
	if (data.inputParameterChanges)
		data.inputParameterChanges->release();

	proc.setActive(false);
	proc.terminate();
	return res;
}

// One normalized value per parameter, exercising every choice point and both
// continuous extremes.
std::vector<std::pair<ParamID, ParamValue>> allParamValues()
{
	return {
	    {kFFTSizeId, fftSizeToNormalized(8192)},
	    {kWindowTypeId, static_cast<ParamValue>(1) / static_cast<ParamValue>(kNumWindows - 1)}, // Hann
	    {kModeId, static_cast<ParamValue>(2) / static_cast<ParamValue>(kNumModes - 1)},        // M/(M-S)
	    {kAttackId, attackMsToNormalized(5.0)},
	    {kReleaseId, releaseMsToNormalized(1000.0)},
	    {kDbRefId, 1.0}, // Raw
	};
}

} // namespace

TEST_CASE("L1: sectra scope is a sample-exact passthrough", "[l1][sectrascope]")
{
	Processor proc;
	auto input = makeSignalPlanar(kNumSamples, kNumChannels);
	auto inputI = toInterleaved(input, kNumSamples, kNumChannels);
	auto res = runBlock(proc, input, {});

	REQUIRE(res.initResult == kResultOk);
	REQUIRE(res.processResult == kResultOk);
	REQUIRE(res.output.size() == inputI.size());
	for (std::size_t i = 0; i < input.size(); ++i)
		REQUIRE(res.output[i] == Approx(inputI[i]).epsilon(1e-7f));
}

TEST_CASE("L1: silence in produces silence out with silence flags", "[l1][sectrascope]")
{
	Processor proc;
	std::vector<float> silence(static_cast<std::size_t>(kNumSamples) * kNumChannels, 0.0f);
	auto res = runBlock(proc, silence, {});

	REQUIRE(res.processResult == kResultOk);
	for (float v : res.output)
		REQUIRE(v == 0.0f);
}

TEST_CASE("L1: changing each of the six params keeps passthrough intact", "[l1][sectrascope]")
{
	const std::vector<std::pair<ParamID, ParamValue>> values = {
	    {kFFTSizeId, fftSizeToNormalized(16384)},
	    {kWindowTypeId, 1.0},
	    {kModeId, static_cast<ParamValue>(1) / static_cast<ParamValue>(kNumModes - 1)},
	    {kAttackId, 1.0},
	    {kReleaseId, 0.0},
	    {kDbRefId, 0.0},
	};

	for (const auto& [id, value] : values)
	{
		Processor proc;
		auto input = makeSignalPlanar(kNumSamples, kNumChannels);
		auto inputI = toInterleaved(input, kNumSamples, kNumChannels);
		auto res = runBlock(proc, input, {{id, value}});

		REQUIRE(res.processResult == kResultOk);
		for (std::size_t i = 0; i < input.size(); ++i)
			REQUIRE(res.output[i] == Approx(inputI[i]).epsilon(1e-7f));
	}
}

TEST_CASE("L1: all six params changed at once keeps passthrough intact", "[l1][sectrascope]")
{
	Processor proc;
	auto input = makeSignalPlanar(kNumSamples, kNumChannels);
	auto inputI = toInterleaved(input, kNumSamples, kNumChannels);
	auto res = runBlock(proc, input, allParamValues());

	REQUIRE(res.processResult == kResultOk);
	for (std::size_t i = 0; i < input.size(); ++i)
		REQUIRE(res.output[i] == Approx(inputI[i]).epsilon(1e-7f));
}

TEST_CASE("L1: sample-rate change mid-stream does not break passthrough", "[l1][sectrascope]")
{
	Processor proc;
	proc.initialize(nullptr); // Component::initialize returns void

	SpeakerArrangement inArr = SpeakerArr::kStereo;
	SpeakerArrangement outArr = SpeakerArr::kStereo;
	proc.setBusArrangements(&inArr, 1, &outArr, 1);

	auto setupAt = [&](double rate) {
		ProcessSetup setup;
		setup.processMode = kOffline;
		setup.symbolicSampleSize = kSample32;
		setup.maxSamplesPerBlock = kNumSamples;
		setup.sampleRate = static_cast<SampleRate>(rate);
		return proc.setupProcessing(setup);
	};

	REQUIRE(setupAt(44100.0) == kResultOk);
	proc.setActive(true);

	std::vector<float> inBuf(static_cast<std::size_t>(kNumSamples) * kNumChannels, 0.0f);
	std::vector<float> outBuf(inBuf.size(), 0.0f);
	const double freqs[2] = {440.0, 880.0};
	for (int32 ch = 0; ch < kNumChannels; ++ch)
		for (int32 s = 0; s < kNumSamples; ++s)
			inBuf[static_cast<std::size_t>(ch) * kNumSamples + s] =
			    static_cast<float>(std::sin(2.0 * M_PI * freqs[ch] * s / 44100.0)) * 0.5f;

	AudioBusBuffers inBus;
	inBus.numChannels = kNumChannels;
	inBus.channelBuffers32 = new Sample32*[kNumChannels];
	for (int32 ch = 0; ch < kNumChannels; ++ch)
		inBus.channelBuffers32[ch] =
		    const_cast<Sample32*>(reinterpret_cast<const Sample32*>(inBuf.data())) +
		    static_cast<std::size_t>(ch) * kNumSamples;
	AudioBusBuffers outBus;
	outBus.numChannels = kNumChannels;
	outBus.channelBuffers32 = new Sample32*[kNumChannels];
	for (int32 ch = 0; ch < kNumChannels; ++ch)
		outBus.channelBuffers32[ch] =
		    reinterpret_cast<Sample32*>(outBuf.data()) + static_cast<std::size_t>(ch) * kNumSamples;

	// Block 1 at 44.1 kHz.
	{
		ProcessData data;
		data.processMode = kOffline;
		data.symbolicSampleSize = kSample32;
		data.numSamples = kNumSamples;
		data.inputs = &inBus;
		data.outputs = &outBus;
		data.numInputs = 1;
		data.numOutputs = 1;
		REQUIRE(proc.process(data) == kResultOk);
	}

	// Switch to 48 kHz mid-stream and process again.
	REQUIRE(setupAt(48000.0) == kResultOk);
	{
		ProcessData data;
		data.processMode = kOffline;
		data.symbolicSampleSize = kSample32;
		data.numSamples = kNumSamples;
		data.inputs = &inBus;
		data.outputs = &outBus;
		data.numInputs = 1;
		data.numOutputs = 1;
		REQUIRE(proc.process(data) == kResultOk);
	}

	// Output must still equal the input, sample for sample.
	for (std::size_t i = 0; i < inBuf.size(); ++i)
		REQUIRE(outBuf[i] == Approx(inBuf[i]).epsilon(1e-7f));

	delete[] inBus.channelBuffers32;
	delete[] outBus.channelBuffers32;
	proc.setActive(false);
	proc.terminate();
}

//------------------------------------------------------------------------
// Studio One crash regressions:
//  - dbA_ buffer sizing: brace-init {kNumCols, -120.f} bound to
//    std::vector's initializer_list ctor and created a 2-element buffer;
//    setData then wrote 720 floats past the end -> STATUS_HEAP_CORRUPTION.
//  - Editor lifetime: VSTGUI owns the views returned from createCustomView()
//    and destroys them when the editor closes (VST3Editor::close -> willClose
//    -> frame removeAll). The controller must drop its raw ScopeView* list in
//    willClose(); ScopeView carries a debug poison guard so a stale access
//    throws instead of corrupting the heap.
namespace {

void feedScopeBlock(Controller& c)
{
	// A host registers the custom views once per open editor; only do so
	// when no generation is currently registered (fresh editor open).
	if (c.scopeCount () == 0)
	{
		VSTGUI::UTF8StringPtr names[2] = {"scopeA", "scopeB"};
		VSTGUI::UIAttributes attrs[2]; // no attributes: default size used
		c.createCustomView (names[0], attrs[0], nullptr, nullptr);
		c.createCustomView (names[1], attrs[1], nullptr, nullptr);
	}

	ScopeData data {};
	for (int i = 0; i < ScopeData::kNumCols; ++i)
	{
		data.a[i] = -60.f + static_cast<float> (i % 7);
		data.b[i] = -55.f + static_cast<float> (i % 5);
	}

	Steinberg::uint32 blockSize = scopeDataSize ();
	Steinberg::Vst::DataExchangeBlock block {static_cast<void*> (&data), blockSize, 42};
	TBool background = false;
	c.queueOpened (kScopeQueueId, blockSize, background);
	c.onDataExchangeBlocksReceived (kScopeQueueId, 1, &block, false);
	c.queueClosed (kScopeQueueId);
}

} // namespace

TEST_CASE("L1: [iso] construct and destroy two bare ScopeViews", "[l1][sectrascope]")
{
	VSTGUI::CRect r {0, 0, 720, 200};
	for (int i = 0; i < 3; ++i)
	{
		auto* v1 = new ScopeView (r);
		v1->setLabel ("L");
		REQUIRE(v1->numStoredColumns () == ScopeData::kNumCols);
		delete v1;
		auto* v2 = new ScopeView (r);
		v2->setLabel ("R");
		REQUIRE(v2->numStoredColumns () == ScopeData::kNumCols);
		delete v2;
	}
	SUCCEED();
}

TEST_CASE("L1: Sectra Scope controller drops scope views when the editor closes", "[l1][sectrascope]")
{
	Controller c;
	REQUIRE(c.initialize (nullptr) == kResultOk);

	feedScopeBlock (c); // open editor #1 + first spectrum update
	REQUIRE(c.scopeCount () == 2);

	// Host closes the editor window: willClose fires, then VSTGUI deletes all
	// views of the frame. Capture the live pointers first, then simulate both
	// halves exactly as VST3Editor::close() does them.
	auto* gen1A = c.scopeViews ().at (0);
	auto* gen1B = c.scopeViews ().at (1);
	c.willClose (nullptr);
	delete gen1A;
	delete gen1B;

	// Playback continues: another spectrum update must not touch the dead
	// views. Pre-fix this wrote into freed memory (heap corruption in Studio
	// One); with the poison guard it would throw instead.
	REQUIRE_NOTHROW(feedScopeBlock (c));

	c.terminate ();
}

TEST_CASE("L1: Sectra Scope keeps feeding only live views after an editor close/reopen cycle", "[l1][sectrascope]")
{
	Controller c;
	REQUIRE(c.initialize (nullptr) == kResultOk);

	feedScopeBlock (c); // editor generation 1
	REQUIRE(c.scopeCount () == 2);

	auto* gen1A = c.scopeViews ().at (0);
	auto* gen1B = c.scopeViews ().at (1);
	c.willClose (nullptr);
	delete gen1A;
	delete gen1B;

	feedScopeBlock (c); // host reopens the editor -> fresh views registered
	REQUIRE(c.scopeCount () == 2); // no unbounded growth across generations

	// A further update reaches exactly the two live views and nothing else.
	REQUIRE_NOTHROW(feedScopeBlock (c));
	REQUIRE(c.scopeCount () == 2);

	c.terminate ();
}

//------------------------------------------------------------------------
// F1 (PLAN-ui-screenshots.md): headless UI screenshot capture.
// The controller must render its attached frame to PNG when it receives the
// "vdplg.debug.screenshot" message with a "path" attribute — the same
// offscreen-render recipe Steinberg uses in VST3Editor::saveScreenshot().
namespace {

// Minimal IMessage + IAttributeList for driving Controller::notify() without
// a full host implementation (the SDK ships no concrete Message class).
class MinimalMessage : public FUnknown, public IMessage
{
public:
	class Attrs;

	explicit MinimalMessage(const char* id)
	{
		std::strncpy(id_, id, sizeof(id_) - 1);
		id_[sizeof(id_) - 1] = '\0';
	}

	FIDString PLUGIN_API getMessageID() override { return id_; }
	void PLUGIN_API setMessageID(FIDString newId) override { std::strcpy(id_, newId); }
	IAttributeList* PLUGIN_API getAttributes() override { return &attrs_; }

	IAttributeList& attrs() { return attrs_; }

	tresult PLUGIN_API queryInterface(const TUID iid, void** obj) SMTG_OVERRIDE
	{
		if (iid == FUnknown::iid || iid == IMessage::iid)
		{
			addRef();
			*obj = static_cast<IMessage*>(this);
			return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}
	uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refCount_; }
	uint32 PLUGIN_API release() SMTG_OVERRIDE
	{
		uint32 r = --refCount_;
		if (r == 0)
			delete this;
		return r;
	}

private:
	char id_[64];
	uint32 refCount_ {1};

	class Attrs : public FUnknown, public IAttributeList
	{
	public:
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
			std::lock_guard<std::mutex> lock(mutex_);
			strings_[key] = std::move(value);
			return kResultTrue;
		}
		tresult PLUGIN_API getString(AttrID id, TChar* out, uint32 sizeInBytes) override
		{
			if (!id || !out)
				return kInvalidArgument;
			std::string key(id);
			std::lock_guard<std::mutex> lock(mutex_);
			auto it = strings_.find(key);
			if (it == strings_.end())
				return kResultFalse;
			const auto& s = it->second;
			uint32 n = static_cast<uint32>(std::min(s.size(), static_cast<size_t>(sizeInBytes / sizeof(TChar)) - 1));
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

		tresult PLUGIN_API queryInterface(const TUID iid, void** obj) SMTG_OVERRIDE
		{
			if (iid == FUnknown::iid || iid == IAttributeList::iid)
			{
				addRef();
				*obj = static_cast<IAttributeList*>(this);
				return kResultOk;
			}
			*obj = nullptr;
			return kNoInterface;
		}
		uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return ++refCount_; }
		uint32 PLUGIN_API release() SMTG_OVERRIDE
		{
			uint32 r = --refCount_;
			if (r == 0)
				delete this;
			return r;
		}

	private:
		std::map<std::string, std::string> strings_;
		std::mutex mutex_;
		uint32 refCount_ {1};
	};

	Attrs attrs_;
};

// PNG header check: signature + IHDR width/height.
struct PngInfo
{
	bool validSignature = false;
	int width = 0;
	int height = 0;
	std::vector<unsigned char> bytes;
};

PngInfo readPng(const std::string& path)
{
	PngInfo info;
		std::ifstream f(path, static_cast<std::ios_base::openmode>(std::ios::binary));
	if (!f)
		return info;
	info.bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	if (info.bytes.size() >= 8 && std::memcmp(info.bytes.data(), sig, 8) == 0)
		info.validSignature = true;
	if (info.bytes.size() >= 24 && info.bytes[12] == 'I' && info.bytes[13] == 'H' &&
	    info.bytes[14] == 'D' && info.bytes[15] == 'R')
	{
		const auto* p = info.bytes.data() + 16;
		info.width = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
		info.height = (p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
	}
	return info;
}

std::string tempShotPath(const char* name)
{
	char dir[MAX_PATH] {};
	GetTempPathA(MAX_PATH, dir);
	std::string full(dir);
	full += "vdplg_";
	full += name;
	full += ".png";
	return full;
}

void cleanupShot(const std::string& path)
{
	DeleteFileA(path.c_str());
}

// One-time-per-process setup so COffscreenContext can render without a host.
// Windows prerequisites (verified against local VSTGUI source):
//  1. COM apartment must exist BEFORE VSTGUI::init — the Win32Factory ctor
//     calls CoCreateInstance(CLSID_WICImagingFactory) unchecked; without an
//     apartment the WIC factory stays null and createBitmap crashes.
//  2. A D2D device must be registered: on Windows devices only appear via
//     DirectComposition or Win32Factory::createGraphicsDeviceContext(HWND).
//     We register one through a hidden off-screen popup window.
struct HeadlessState
{
	bool initialized = false;
};

// Leaked on purpose: must outlive every static destructor in the process.
HeadlessState& ensureHeadlessState()
{
	static HeadlessState* state = new HeadlessState();
	return *state;
}

void ensureVstguiReady()
{
	auto& st = ensureHeadlessState();
	if (st.initialized)
		return;
#ifdef _WIN32
	// COM apartment first: Win32Factory's ctor creates the WIC imaging
	// factory via an unchecked CoCreateInstance; fails without one.
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
	VSTGUI::init(nullptr);
#ifdef _WIN32
	// Register a D2D device through a temporary hidden window. The device
	// itself lives in the platform factory afterwards, so the window may be
	// destroyed immediately — no long-lived HWND to tear down at exit.
	WNDCLASSEXA wc {};
	wc.cbSize = sizeof(WNDCLASSEXA);
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(nullptr);
	wc.lpszClassName = "vdplg_ui_test";
	RegisterClassExA(&wc);
	const HWND hwnd = CreateWindowExA(WS_EX_TOOLWINDOW, "vdplg_ui_test", nullptr, WS_POPUP,
	                           -20000, -20000, 720, 400, nullptr, nullptr, wc.hInstance,
	                           nullptr);
	if (hwnd)
	{
		auto gdc = VSTGUI::getPlatformFactory().asWin32Factory()->createGraphicsDeviceContext(hwnd);
		gdc = VSTGUI::PlatformGraphicsDeviceContextPtr(); // release; device stays registered
		DestroyWindow(hwnd);
	}
#endif
	st.initialized = true;
}

// Explicit shutdown hook called from main() AFTER all tests have run.
// Rationale: letting VSTGUI's cross-TU statics (font cache, D2D/WIC COM
// objects, task-executor message window) tear down during process exit has
// proven unreliable here — destruction order across translation units left a
// dangling pointer that crashed in a static destructor (null-deref on
// [rax] observed in Debug). Shutting VSTGUI down while main() is still alive
// makes the ordering deterministic. Safe because no test runs afterwards.
extern "C" void vdplg_tests_shutdown_ui ()
{
	auto& st = ensureHeadlessState();
	if (!st.initialized)
		return;
	st.initialized = false; // guard against double exit
#ifdef _WIN32
	VSTGUI::exit();
	CoUninitialize();
#else
	VSTGUI::exit();
#endif
}

// Build a frame with two scope views holding the given spectra and attach it
// to the controller exactly as didOpen() would for a live editor.
VSTGUI::CFrame* buildDebugFrame(Controller& c, const float* spectrumA, const float* spectrumB)
{
	auto* frame = new VSTGUI::CFrame(VSTGUI::CRect(0, 0, 720, 400), nullptr);
	frame->setViewSize(VSTGUI::CRect(0, 0, 720, 400));
	VSTGUI::UTF8StringPtr names[2] = {"scopeA", "scopeB"};
	VSTGUI::UIAttributes attrs[2];
	for (int i = 0; i < 2; ++i)
	{
		auto* view = c.createCustomView(names[i], attrs[i], nullptr, nullptr);
		// CRect is (left, top, right, bottom): panel i spans rows i*200..(i+1)*200.
		view->setViewSize(VSTGUI::CRect(0, static_cast<VSTGUI::CCoord>(i * 200), 720,
		                               static_cast<VSTGUI::CCoord>((i + 1) * 200)));
		frame->addView(view);
	}
	c.scopeViews().at(0)->setData(spectrumA, ScopeData::kNumCols);
	c.scopeViews().at(1)->setData(spectrumB, ScopeData::kNumCols);
	c.attachDebugFrame(frame); // transfers ownership to the controller
	return frame;
}

std::vector<float> fillRamp(int offset)
{
	std::vector<float> v(ScopeData::kNumCols);
	for (int i = 0; i < ScopeData::kNumCols; ++i)
		v[i] = -90.f + static_cast<float>((i + offset) % 37) * 2.f;
	return v;
}

std::basic_string<Vst::TChar> toTChar(const std::string& s)
{
	std::basic_string<Vst::TChar> out;
	out.reserve(s.size() + 1);
	for (char ch : s)
		out += static_cast<Vst::TChar>(ch);
	return out;
}

} // namespace

TEST_CASE("L1: [ui] screenshot message renders attached frame to a valid PNG", "[l1][sectrascope][ui]")
{
	ensureVstguiReady();
	Controller c;
	REQUIRE(c.initialize(nullptr) == kResultOk);

	auto specA = fillRamp(0);
	auto specB = fillRamp(5);
	buildDebugFrame(c, specA.data(), specB.data());

	const std::string path = tempShotPath("f1_valid");
	MinimalMessage msg("vdplg.debug.screenshot");
	msg.attrs().setString("path", toTChar(path).c_str());

	REQUIRE(c.notify(&msg) == kResultTrue);

	PngInfo info = readPng(path);
	REQUIRE(info.validSignature);
	REQUIRE(info.width == 720);
	REQUIRE(info.height == 400);
	REQUIRE(info.bytes.size() > 5000); // real content, not an empty canvas

	c.detachDebugFrame();
	c.terminate();
	cleanupShot(path);
}

TEST_CASE("L1: [ui] different spectra produce different screenshots", "[l1][sectrascope][ui]")
{
	ensureVstguiReady();
	Controller c;
	REQUIRE(c.initialize(nullptr) == kResultOk);

	auto flat = fillRamp(0);
	std::fill(flat.begin(), flat.end(), -120.f); // silence spectrum
	auto loud = fillRamp(11);                     // distinct pattern
	buildDebugFrame(c, flat.data(), loud.data());

	const std::string p1 = tempShotPath("f1_a");
	MinimalMessage m1("vdplg.debug.screenshot");
	m1.attrs().setString("path", toTChar(p1).c_str());
	REQUIRE(c.notify(&m1) == kResultTrue);

	// Replace the frame with one holding clearly different data.
	c.detachDebugFrame();
	auto swapped = fillRamp(0);
	std::vector<float> silent(ScopeData::kNumCols, -120.f);
	buildDebugFrame(c, swapped.data(), silent.data());

	const std::string p2 = tempShotPath("f1_b");
	MinimalMessage m2("vdplg.debug.screenshot");
	m2.attrs().setString("path", toTChar(p2).c_str());
	REQUIRE(c.notify(&m2) == kResultTrue);

	PngInfo a = readPng(p1);
	PngInfo b = readPng(p2);
	REQUIRE(a.validSignature);
	REQUIRE(b.validSignature);
	CHECK(a.bytes != b.bytes);

	c.detachDebugFrame();
	c.terminate();
	cleanupShot(p1);
	cleanupShot(p2);
}

TEST_CASE("L1: [ui] screenshot message without attached frame fails cleanly", "[l1][sectrascope][ui]")
{
	ensureVstguiReady();
	Controller c;
	REQUIRE(c.initialize(nullptr) == kResultOk);

	const std::string path = tempShotPath("f1_noframe");
	MinimalMessage msg("vdplg.debug.screenshot");
	msg.attrs().setString("path", toTChar(path).c_str());

	REQUIRE(c.notify(&msg) == kResultFalse);
	REQUIRE(!std::ifstream(path));

	// Unrelated messages must fall through to the base implementation.
	MinimalMessage other("SomeOtherMessage");
	REQUIRE_NOTHROW(c.notify(&other));

	c.terminate();
	cleanupShot(path);
}
