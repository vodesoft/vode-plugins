// Vode Plugins — common WAV I/O wrapper over dr_wav (public domain).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vdplg {

struct WavFile
{
    int sampleRate = 0;
    int channels = 0;
    // Interleaved float32 samples, size == frames * channels.
    std::vector<float> samples;

    std::size_t frames() const { return channels ? samples.size() / static_cast<std::size_t>(channels) : 0; }
};

// Loads a WAV file into float32. Supports 16/24/32-bit integer PCM and 32-bit float.
// Returns false and fills `error` on failure.
bool loadWav(const std::string& path, WavFile& out, std::string& error);

// Saves interleaved float32 as a 32-bit float WAV.
// Returns false and fills `error` on failure.
bool saveWav(const std::string& path, const WavFile& wav, std::string& error);

} // namespace vdplg
