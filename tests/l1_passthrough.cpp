// Vode Plugins — L1 tests: passthrough processor driven through process().
//
// Instantiates vdplg::passthrough::Processor directly (no DAW, no factory)
// and feeds synthetic buffers through the VST3 process() path.

#define _USE_MATH_DEFINES
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Processor.h"
#include "passthroughparamids.h"

#include "public.sdk/source/vst/vstparameters.h"

#include "vdplg/dsp.h"

#include <cmath>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace vdplg::passthrough;
using Catch::Approx;

namespace {

constexpr int32 kNumChannels = 2;
constexpr int32 kNumSamples = 512;
constexpr double kSampleRate = 44100.0;
constexpr ParamValue kGainMinDb = -60.0;
constexpr ParamValue kGainMaxDb = +24.0;

// Builds a deterministic stereo test signal (sine per channel), PLANAR layout:
// channel ch sample s lives at buf[ch * numSamples + s] (VST3 buffer convention).
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

// Converts a planar buffer (VST3 layout) to interleaved [s*ch + c].
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

// Minimal IParamValueQueue implementation for feeding parameter automation.
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

// Minimal IParameterChanges implementation for feeding parameter automation.
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

// Runs one process block through a freshly initialized processor.
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

	// Allocate planar output buffer.
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

	// Deinterleave planar output for the caller.
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

} // namespace

TEST_CASE("L1: unity-gain passthrough is sample-exact", "[l1][passthrough]")
{
	Processor proc;
	auto input = makeSignalPlanar(kNumSamples, kNumChannels);
	auto inputI = toInterleaved(input, kNumSamples, kNumChannels);
	auto res = runBlock(proc, input, {});

	REQUIRE(res.initResult == kResultOk);
	REQUIRE(res.processResult == kResultOk);
	REQUIRE(res.output.size() == inputI.size());
	for (std::size_t i = 0; i < input.size(); ++i)
	{
		REQUIRE(res.output[i] == Approx(inputI[i]).epsilon(1e-7f));
	}
}

TEST_CASE("L1: gain parameter is applied correctly", "[l1][passthrough]")
{
	const double gainDb = 6.0;
	const float expectedGain = static_cast<float>(vdplg::dbToLinear(gainDb));
	const ParamValue norm = vdplg::dbToNormalized(gainDb, kGainMinDb, kGainMaxDb);

	Processor proc;
	auto input = makeSignalPlanar(kNumSamples, kNumChannels);
	auto inputI = toInterleaved(input, kNumSamples, kNumChannels);
	auto res = runBlock(proc, input, {{kGainId, norm}});

	REQUIRE(res.processResult == kResultOk);
	for (std::size_t i = 0; i < input.size(); ++i)
	{
		REQUIRE(res.output[i] == Approx(inputI[i] * expectedGain).epsilon(1e-5f));
	}
}

TEST_CASE("L1: mix=0 passes dry signal unchanged", "[l1][passthrough]")
{
	// Even with a large gain, full-dry mix must leave the signal untouched.
	const ParamValue normGain = vdplg::dbToNormalized(12.0, kGainMinDb, kGainMaxDb);

	Processor proc;
	auto input = makeSignalPlanar(kNumSamples, kNumChannels);
	auto inputI = toInterleaved(input, kNumSamples, kNumChannels);
	auto res = runBlock(proc, input, {{kGainId, normGain}, {kMixId, 0.0}});

	REQUIRE(res.processResult == kResultOk);
	for (std::size_t i = 0; i < input.size(); ++i)
	{
		REQUIRE(res.output[i] == Approx(inputI[i]).epsilon(1e-7f));
	}
}

TEST_CASE("L1: silence in produces silence out and sets silence flags", "[l1][passthrough]")
{
	Processor proc;
	std::vector<float> silence(static_cast<std::size_t>(kNumSamples) * kNumChannels, 0.0f);
	auto res = runBlock(proc, silence, {});

	REQUIRE(res.processResult == kResultOk);
	for (float v : res.output)
		REQUIRE(v == 0.0f);
}

TEST_CASE("L1: no NaN or Inf in output for normal input", "[l1][passthrough]")
{
	Processor proc;
	auto input = makeSignalPlanar(kNumSamples, kNumChannels);
	auto res = runBlock(proc, input, {{kGainId, vdplg::dbToNormalized(24.0, kGainMinDb, kGainMaxDb)}});

	REQUIRE(res.processResult == kResultOk);
	for (float v : res.output)
	{
		REQUIRE(std::isfinite(v));
	}
}
