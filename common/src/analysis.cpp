#include "vdplg/analysis.h"

#include <complex>
#include <cmath>
#include <algorithm>

#include "signalsmith-dsp/fft.h"

namespace vdplg {

AnalysisResult analyzeLevels(const std::vector<float>& interleaved)
{
    AnalysisResult r;
    if (interleaved.empty()) return r;

    double sum = 0.0;
    double sumSq = 0.0;
    for (float s : interleaved)
    {
        const double v = static_cast<double>(s);
        if (!std::isfinite(v)) { r.hasNaNOrInf = true; break; }
        const double a = std::fabs(v);
        if (a > r.peak) r.peak = a;
        sum += v;
        sumSq += v * v;
    }
    if (!r.hasNaNOrInf)
    {
        const double n = static_cast<double>(interleaved.size());
        r.dcOffset = sum / n;
        r.rms = std::sqrt(sumSq / n);
    }
    return r;
}

namespace {

// Windowed complex FFT of the first channel; returns magnitudes for bins 0..n/2.
std::vector<double> windowedMagnitudes(const std::vector<float>& interleaved, int channels,
                                       int fftSize)
{
    std::vector<std::complex<double>> in(fftSize), out(fftSize);
    const int take = static_cast<int>(
        std::min<std::size_t>(static_cast<std::size_t>(fftSize),
                              interleaved.size() / static_cast<std::size_t>(std::max(1, channels))));
    for (int i = 0; i < take; ++i)
    {
        // Hann window
        const double w = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (fftSize - 1)));
        in[i] = {static_cast<double>(interleaved[static_cast<std::size_t>(i * channels)]) * w, 0.0};
    }
    signalsmith::fft::FFT<double> fft(static_cast<std::size_t>(fftSize));
    fft.fft(in.begin(), out.begin());
    std::vector<double> mags(fftSize / 2 + 1);
    for (int i = 0; i <= fftSize / 2; ++i)
        mags[i] = std::abs(out[i]);
    return mags;
}

} // namespace

double fftMagnitudeAtHz(const std::vector<float>& interleaved, int channels,
                        int sampleRate, double freqHz, int fftSize)
{
    if (freqHz <= 0.0 || freqHz >= sampleRate / 2.0) return 0.0;
    auto mags = windowedMagnitudes(interleaved, channels, fftSize);
    const double bin = freqHz * static_cast<double>(fftSize) / sampleRate;
    const int i0 = static_cast<int>(bin);
    if (i0 >= fftSize / 2) return 0.0;
    // Linear interpolation between adjacent bins.
    const double frac = bin - i0;
    const double m1 = mags[i0];
    const double m2 = (i0 + 1 < static_cast<int>(mags.size())) ? mags[i0 + 1] : 0.0;
    return m1 * (1.0 - frac) + m2 * frac;
}

double thdPercentAtHz(const std::vector<float>& interleaved, int channels,
                      int sampleRate, double fundamentalHz, int fftSize)
{
    const double fund = fftMagnitudeAtHz(interleaved, channels, sampleRate, fundamentalHz, fftSize);
    if (fund < 1e-9) return -1.0;
    double harm = 0.0;
    for (int h = 2; h <= 7; ++h)
        harm += fftMagnitudeAtHz(interleaved, channels, sampleRate, fundamentalHz * h, fftSize);
    return 100.0 * harm / fund;
}

bool compareGolden(const std::vector<float>& a, const std::vector<float>& b,
                   double tolerance, std::string& error)
{
    if (a.size() != b.size())
    {
        error = "length mismatch: " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]))
        {
            error = "non-finite sample at index " + std::to_string(i);
            return false;
        }
        if (std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])) > tolerance)
        {
            error = "sample mismatch at index " + std::to_string(i);
            return false;
        }
    }
    return true;
}

} // namespace vdplg
