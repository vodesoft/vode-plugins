// One-shot helper: generates the static WAV inputs used by L3 test cases.
// Build & run once (see testdata/README.md):
//   cl /EHsc /O2 gen_wavs.cpp ..\..\common\dr_wav\dr_wav.c ... 
// Simpler: compiled as part of the build tree via a small CMake target is not
// needed; this file is standalone using dr_wav directly.
//
// Outputs (into the directory given as argv[1], default "."):
//   sine_440.wav  - 1 s, 44.1 kHz, stereo, 440 Hz sine, amplitude 0.5
//   noise.wav     - 1 s, 44.1 kHz, stereo, white noise (deterministic), amp 0.25
//   silence.wav   - 1 s, 44.1 kHz, stereo, digital silence

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// C wrappers from common/src/dr_wav_impl.c
extern "C" {
int vdplg_drwav_save_float(const char* path, int sampleRate, int channels,
						   const float* samples, std::size_t numSamples);
}

namespace {

constexpr int kSampleRate = 44100;
constexpr int kChannels = 2;
constexpr int kSeconds = 1;
constexpr int kFrames = kSampleRate * kSeconds;

std::vector<float> makeSine440()
{
	std::vector<float> samples(static_cast<std::size_t>(kFrames) * kChannels, 0.0f);
	const double twoPi = 6.283185307179586;
	for (int s = 0; s < kFrames; ++s)
	{
		float v = static_cast<float>(0.5 * std::sin(twoPi * 440.0 * s / kSampleRate));
		samples[static_cast<std::size_t>(s) * kChannels + 0] = v;
		samples[static_cast<std::size_t>(s) * kChannels + 1] = v;
	}
	return samples;
}

std::vector<float> makeNoise()
{
	// Deterministic LCG so golden files stay reproducible across machines.
	std::vector<float> samples(static_cast<std::size_t>(kFrames) * kChannels, 0.0f);
	std::uint64_t state = 0x123456789ABCDEFULL;
	auto next = [&state]() -> float {
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		// map to [-1, 1)
		std::uint32_t u = static_cast<std::uint32_t>((state >> 40) & 0xFFFFFF);
		return static_cast<float>(u) / static_cast<float>(0x7FFFFF) - 1.0f;
	};
	for (auto& v : samples)
		v = 0.25f * next();
	return samples;
}

std::vector<float> makeSilence()
{
	return std::vector<float>(static_cast<std::size_t>(kFrames) * kChannels, 0.0f);
}

bool writeWav(const std::string& path, const std::vector<float>& interleaved)
{
	bool ok = vdplg_drwav_save_float(path.c_str(), kSampleRate, kChannels,
	                                  interleaved.data(), interleaved.size()) != 0;
	if (!ok)
		std::fprintf(stderr, "failed to write %s\n", path.c_str());
	return ok;
}

} // namespace

int main(int argc, char** argv)
{
	std::string dir = (argc > 1) ? argv[1] : ".";
	auto join = [&dir](const char* name) {
		if (dir.empty() || dir == ".")
			return std::string(name);
		char last = dir.back();
		if (last == '/' || last == '\\')
			return dir + name;
		return dir + "/" + name;
	};

	bool ok = true;
	ok &= writeWav(join("sine_440.wav"), makeSine440());
	ok &= writeWav(join("noise.wav"), makeNoise());
	ok &= writeWav(join("silence.wav"), makeSilence());
	std::printf(ok ? "generated 3 WAV files in %s\n" : "FAILED generating WAVs\n",
	            dir.c_str());
	return ok ? 0 : 1;
}
