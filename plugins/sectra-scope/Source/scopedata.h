// Vode Plugins — Sectra Scope: cross-process scope payload.
//
// The processor writes one snapshot of the two log-frequency spectra into a
// data-exchange block each audio block; the edit controller reads the newest
// block and repaints its two scope views. This struct is the fixed-size layout
// shared across the process boundary, so keep it POD and stable.

#pragma once

#include <cstdint>

namespace vdplg {
namespace sectrascope {

struct ScopeData
{
	static constexpr int kNumCols = 720; // matches SpectrumAnalyzer::numColumns()
	float a[kNumCols]; // channel A dB values (L / M depending on mode)
	float b[kNumCols]; // channel B dB values (R / S depending on mode)
};

inline uint32_t scopeDataSize ()
{
	return static_cast<uint32_t> (sizeof (ScopeData));
}

// User context ID identifying this plugin's data-exchange queue. Shared by the
// processor (sender) and controller (receiver); must match across processes.
static constexpr uint32_t kScopeQueueId = 1;

} // namespace sectrascope
} // namespace vdplg
