#include "vdplg/wavio.h"

#include <fstream>

// C wrappers implemented in dr_wav_impl.c (keeps dr_wav.h out of C++ TUs).
extern "C" {
int vdplg_drwav_load_float(const char* path, int* sampleRate, int* channels,
                           float* buffer, std::size_t bufferSize, std::size_t* framesOut);
int vdplg_drwav_save_float(const char* path, int sampleRate, int channels,
                           const float* samples, std::size_t numSamples);
}

namespace vdplg {

bool loadWav(const std::string& path, WavFile& out, std::string& error)
{
    // First pass: probe size by loading into a scratch buffer is wasteful, so
    // do a two-step approach — read with a generous upper bound derived from
    // file size (4 bytes per float sample).
    std::ifstream probe(path, std::ios::binary | std::ios::ate);
    if (!probe)
    {
        error = "failed to open WAV file: " + path;
        return false;
    }
    const std::size_t fileSize = static_cast<std::size_t>(probe.tellg());
    probe.close();

    std::vector<float> scratch(fileSize / 4 + 1, 0.0f);
    int sr = 0, ch = 0;
    std::size_t frames = 0;
    if (!vdplg_drwav_load_float(path.c_str(), &sr, &ch, scratch.data(), scratch.size(), &frames))
    {
        error = "failed to decode WAV file: " + path;
        return false;
    }

    out.sampleRate = sr;
    out.channels = ch;
    out.samples.assign(scratch.begin(), scratch.begin() + frames * static_cast<std::size_t>(ch));
    return true;
}

bool saveWav(const std::string& path, const WavFile& wav, std::string& error)
{
    if (!vdplg_drwav_save_float(path.c_str(), wav.sampleRate, wav.channels,
                                wav.samples.data(), wav.samples.size()))
    {
        error = "failed to write WAV file: " + path;
        return false;
    }
    return true;
}

} // namespace vdplg
