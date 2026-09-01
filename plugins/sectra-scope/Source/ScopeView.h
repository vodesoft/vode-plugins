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
	~ScopeView () override; // marks the object dead (debug poison guard)

	void setData (const float* dbValues, int numCols); // UI thread only
	void setLabel (const char* label);                 // UI thread only
	void setBalanceMode (bool enabled);                // UI thread only
	int numStoredColumns () const;                    // for tests

protected:
	void draw (VSTGUI::CDrawContext* pContext) override;

private:
	void assertAlive (); // debug-only: throws if called after destruction
	static constexpr int kNumCols = 720;

	// Sized in the ctor body: neither brace-init {n, v} (binds to
	// initializer_list -> 2-element buffer!) nor paren-init (n, v) (vexing
	// parse -> function declaration) works as a default member initializer.
	std::vector<float> dbA_;
	char label_[8] {'?'};
	bool balanceMode_ {false};
	bool dead_ {false}; // poison flag: any method call after ~ScopeView throws (tests)
	vdplg::spectrum::LogFreqMap freqMap_;
};

inline int ScopeView::numStoredColumns () const
{
	return static_cast<int> (dbA_.size ());
}

} // namespace sectrascope
} // namespace vdplg
