// Vode Plugins — Sectra Scope: custom VSTGUI spectrum view.
//
// A plain CView subclass drawn on the UI thread. Holds one channel's log-
// frequency spectrum (720 dB values) plus a short channel label ("L", "R",
// "M", "S"). In balance mode (Mid/(Mid-Side)) the second slot shows the
// Mid−Side difference scaled to ±12 dB around a centered zero line instead of
// the usual −120…0 dB scale.

#pragma once

#include <vector>

#include "vstgui/lib/cview.h"
#include "vdplg/spectrum.h"

namespace vdplg {
namespace sectrascope {

class ScopeView : public VSTGUI::CView
{
public:
	ScopeView (const VSTGUI::CRect& size);

	void setData (const float* dbValues, int numCols); // UI thread only
	void setLabel (const char* label);                 // UI thread only
	void setBalanceMode (bool enabled);                // UI thread only

protected:
	void draw (VSTGUI::CDrawContext* pContext) override;

private:
	static constexpr int kNumCols = 720;

	std::vector<float> dbA_{kNumCols, -120.f};
	char label_[8] {'?'};
	bool balanceMode_ {false};
	vdplg::spectrum::LogFreqMap freqMap_;
};

} // namespace sectrascope
} // namespace vdplg
