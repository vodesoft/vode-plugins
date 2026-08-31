// Vode Plugins — audio analysis shared by unit tests and vst3testhost.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace vdplg {

struct AnalysisResult
{
    double peak = 0.0;        // max |sample| over all channels
    double rms = 0.0;         // root-mean-square over all channels
    double dcOffset = 0.0;    // mean over all channels
    bool hasNaNOrInf = false;
};

// Whole-buffer level analysis over interleaved float32 data.
AnalysisResult analyzeLevels(const std::vector<float>& interleaved);

// Magnitude (linear, unnormalized) of the spectrum at `freqHz`, computed from a
// Hann-windowed FFT of the first channel. Returns 0 if freqHz is out of range.
double fftMagnitudeAtHz(const std::vector<float>& interleaved, int channels,
                        int sampleRate, double freqHz, int fftSize = 4096);

// Total harmonic distortion (%) at `fundamentalHz`: sum of magnitudes at
// harmonics 2..7 relative to the fundamental. Returns -1 if the fundamental
// is not detectable (magnitude ~ 0).
double thdPercentAtHz(const std::vector<float>& interleaved, int channels,
                      int sampleRate, double fundamentalHz, int fftSize = 4096);

// Golden comparison: true when both buffers have equal length and every sample
// differs by at most `tolerance` (absolute, per sample).
bool compareGolden(const std::vector<float>& a, const std::vector<float>& b,
                   double tolerance, std::string& error);

} // namespace vdplg
