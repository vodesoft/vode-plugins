// Vode Plugins — small shared DSP helpers.
#pragma once

#include <cmath>

namespace vdplg {

constexpr double kDbMin = -300.0; // floor for dB conversions of zero/near-zero values

inline double dbToLinear(double db) { return std::pow(10.0, db / 20.0); }

inline double linearToDb(double linear)
{
    if (linear <= 0.0) return kDbMin;
    return 20.0 * std::log10(linear);
}

// Normalized [0,1] <-> dB for a parameter range [minDb, maxDb].
inline double dbToNormalized(double db, double minDb, double maxDb)
{
    if (maxDb == minDb) return 0.5;
    double n = (db - minDb) / (maxDb - minDb);
    return n < 0.0 ? 0.0 : (n > 1.0 ? 1.0 : n);
}

inline double normalizedToDb(double norm, double minDb, double maxDb)
{
    return minDb + norm * (maxDb - minDb);
}

} // namespace vdplg
