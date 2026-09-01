// Vode Plugins — Sectra Scope: parameter IDs + value mapping helpers.
//
// ParamIDs are fixed forever (changing them breaks existing presets).
//
// Value mappings (normalized [0,1] <-> plain):
//   Attack  : v == 0        -> 0 ms (instant)
//             v in (0, 1]    -> 20 ms * v^2          (quadratic, perceptual)
//   Release : v in [0, 1]   -> 1 ms * 1000^v         (true log, 1 ms .. 1 s)
//   Choices : point index i of N points -> i / (N - 1)

#pragma once

#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include <cmath>

namespace vdplg {
namespace sectrascope {

static constexpr Steinberg::Vst::ParamID kFFTSizeId = 1;    // discrete choice
static constexpr Steinberg::Vst::ParamID kWindowTypeId = 2; // discrete choice
static constexpr Steinberg::Vst::ParamID kModeId = 3;       // discrete choice
static constexpr Steinberg::Vst::ParamID kAttackId = 4;     // continuous
static constexpr Steinberg::Vst::ParamID kReleaseId = 5;    // continuous
static constexpr Steinberg::Vst::ParamID kDbRefId = 6;      // discrete choice (2)

//--- FFT sizes -----------------------------------------------------------
static constexpr int kNumFftSizes = 5;
static constexpr int kFftSizes[kNumFftSizes] = {1024, 2048, 4096, 8192, 16384};
static constexpr int kDefaultFftSizeIndex = 2; // 4096

inline int fftSizeFromNormalized(Steinberg::Vst::ParamValue v)
{
	int idx = static_cast<int>(std::lround(v * (kNumFftSizes - 1)));
	if (idx < 0) idx = 0;
	if (idx >= kNumFftSizes) idx = kNumFftSizes - 1;
	return kFftSizes[idx];
}

inline Steinberg::Vst::ParamValue fftSizeToNormalized(int size)
{
	for (int i = 0; i < kNumFftSizes; ++i)
		if (kFftSizes[i] == size)
			return static_cast<Steinberg::Vst::ParamValue>(i) /
			       static_cast<Steinberg::Vst::ParamValue>(kNumFftSizes - 1);
	return 0.0;
}

//--- window types ----------------------------------------------------------
static constexpr int kNumWindows = 5; // matches vdplg::spectrum::kNumWindowTypes
static constexpr int kDefaultWindowIndex = 4; // Blackman-Harris

//--- channel modes ---------------------------------------------------------
static constexpr int kNumModes = 3; // matches vdplg::spectrum::kNumChannelModes
static constexpr int kDefaultModeIndex = 0; // L/R

//--- attack -----------------------------------------------------------------
static constexpr double kAttackMaxMs = 20.0;

inline double attackMsFromNormalized(Steinberg::Vst::ParamValue v)
{
	if (v <= 0.0) return 0.0;
	if (v > 1.0) v = 1.0;
	return kAttackMaxMs * v * v;
}

inline Steinberg::Vst::ParamValue attackMsToNormalized(double ms)
{
	if (ms <= 0.0) return 0.0;
	if (ms > kAttackMaxMs) ms = kAttackMaxMs;
	return static_cast<Steinberg::Vst::ParamValue>(std::sqrt(ms / kAttackMaxMs));
}

//--- release ------------------------------------------------------------------
static constexpr double kReleaseMinMs = 1.0;
static constexpr double kReleaseMaxMs = 1000.0;

inline double releaseMsFromNormalized(Steinberg::Vst::ParamValue v)
{
	if (v < 0.0) v = 0.0;
	if (v > 1.0) v = 1.0;
	return kReleaseMinMs * std::pow(kReleaseMaxMs / kReleaseMinMs, v);
}

inline Steinberg::Vst::ParamValue releaseMsToNormalized(double ms)
{
	if (ms < kReleaseMinMs) ms = kReleaseMinMs;
	if (ms > kReleaseMaxMs) ms = kReleaseMaxMs;
	return static_cast<Steinberg::Vst::ParamValue>(
	    std::log(ms / kReleaseMinMs) / std::log(kReleaseMaxMs / kReleaseMinMs));
}

//--- dB reference ------------------------------------------------------------
static constexpr int kNumDbRefs = 2; // 0 = Normalized, 1 = Raw
static constexpr int kDefaultDbRefIndex = 0;

} // namespace sectrascope
} // namespace vdplg
