// Vode Plugins — L1 tests: Sectra Scope processor driven through process().
//
// Instantiates vdplg::sectrascope::Processor directly (no DAW, no factory)
// and feeds synthetic buffers through the VST3 process() path. Phase 2
// (TDD Red): passthrough + six-parameter plumbing must already work; the
// analyzer wiring lands in Phase 3.

#define _USE_MATH_DEFINES
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "SectraScopeProcessor.h"
#include "Controller.h"
#include "ScopeView.h"
#include "scopedata.h"
#include "sectraparamids.h"

#include "public.sdk/source/vst/vstparameters.h"
#include "vstgui/uidescription/uiattributes.h"

#include <cmath>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace vdplg::sectrascope;
using Catch::Approx;

namespace {

constexpr int32 kNumChannels = 2;
constexpr int32 kNumSamples = 512;
constexpr double kSampleRate = 44100.0;

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
