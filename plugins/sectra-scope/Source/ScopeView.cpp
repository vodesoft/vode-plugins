// Vode Plugins — Sectra Scope: custom VSTGUI spectrum view (implementation).

#include "ScopeView.h"

#include "vstgui/lib/cdrawcontext.h"
#include "vstgui/lib/cgraphicspath.h"

#include <cmath>
#include <cstdio>

namespace vdplg {
namespace sectrascope {

using namespace VSTGUI;
using namespace vdplg::spectrum;

//------------------------------------------------------------------------
ScopeView::ScopeView (const CRect& size)
	: CView (size)
	, freqMap_ (20.0, 20000.0, kNumCols)
{
	setWantsFocus (false);
}

//------------------------------------------------------------------------
void ScopeView::setData (const float* dbValues, int numCols)
{
	if (!dbValues)
		return;
	for (int i = 0; i < kNumCols && i < numCols; ++i)
		dbA_[i] = dbValues[i];
	invalid ();
}

//------------------------------------------------------------------------
void ScopeView::setLabel (const char* label)
{
	int n = 0;
	while (label && label[n] && n + 1 < static_cast<int> (sizeof (label_)))
		label_[n] = label[n++];
	label_[n] = '\0';
	invalid ();
}

//------------------------------------------------------------------------
void ScopeView::setBalanceMode (bool enabled)
{
	balanceMode_ = enabled;
	invalid ();
}

//------------------------------------------------------------------------
void ScopeView::draw (CDrawContext* pContext)
{
	const CRect bounds = getViewSize ();
	const int w = static_cast<int> (bounds.getWidth ());
	const int h = static_cast<int> (bounds.getHeight ());
	if (w <= 4 || h <= 4)
		return;

	//--- layout -----------------------------------------------------------
	const int leftMargin = 36; // room for dB labels
	const int plotLeft = leftMargin;
	const int plotRight = w - 4;
	const int plotWidth = plotRight - plotLeft;
	if (plotWidth <= 4)
		return;

	//--- background ---------------------------------------------------------
	pContext->setFillColor ({30, 30, 30, 255}); // #1E1E1E
	pContext->drawRect (CRect (0, 0, w, h), kDrawFilled);

	//--- grid ----------------------------------------------------------------
	auto colX = [this, plotLeft, plotWidth] (double xNorm) -> CCoord {
		return plotLeft + static_cast<CCoord> (xNorm * plotWidth);
	};

	pContext->setFontColor ({110, 110, 110, 255});
	pContext->setFrameColor ({70, 70, 70, 255});
	pContext->setLineWidth (1.f);

	if (!balanceMode_)
	{
		// horizontal lines every 6 dB, labels every 12 dB
		for (int db = 0; db >= DbScale::kDbMin; db -= 6)
		{
			float y = DbScale::dbToY (static_cast<float> (db), h);
			pContext->drawLine (CPoint (plotLeft, y), CPoint (plotRight, y));
			if (db % 12 == 0)
			{
				char buf[8];
				std::snprintf (buf, sizeof (buf), "%d", db);
				pContext->drawString (buf, CPoint (2, y - 7));
			}
		}
	}
	else
	{
		// balance scale: +12 … −12 dB around a centered zero line
		for (int db = 12; db >= -12; db -= 6)
		{
			float y = DbScale::balanceDbToY (static_cast<float> (db), h);
			if (db == 0)
				pContext->setFrameColor ({150, 150, 150, 255}); // emphasized
			else
				pContext->setFrameColor ({70, 70, 70, 255});
			pContext->drawLine (CPoint (plotLeft, y), CPoint (plotRight, y));
			char buf[8];
			std::snprintf (buf, sizeof (buf), "%+d", db);
			pContext->drawString (buf, CPoint (2, y - 7));
			pContext->setFrameColor ({70, 70, 70, 255});
		}
	}

	// octave ticks at 20/40/80/…/10 kHz
	const double octaves[] = {20.0, 40.0, 80.0, 160.0, 320.0, 640.0, 1250.0,
	                          2500.0, 5000.0, 10000.0};
	pContext->setFontColor ({90, 90, 90, 255});
	for (double f : octaves)
	{
		double xNorm = freqMap_.freqToX (f) / kNumCols;
		CCoord x = colX (xNorm);
		pContext->drawLine (CPoint (x, static_cast<CCoord> (h - 1)),
		                    CPoint (x, static_cast<CCoord> (h - 6)));
		char buf[12];
		if (f < 1000.0)
			std::snprintf (buf, sizeof (buf), "%.0f", f);
		else
			std::snprintf (buf, sizeof (buf), "%.0fk", f / 1000.0);
		pContext->drawString (buf, CPoint (x - 6, static_cast<CCoord> (h - 16)));
	}

	//--- spectrum -------------------------------------------------------------
	auto toY = [this, h] (float db) -> CCoord {
		return balanceMode_ ? DbScale::balanceDbToY (db, h)
		                    : DbScale::dbToY (db, h);
	};

	CGraphicsPath* fillPath = pContext->createGraphicsPath ();
	fillPath->beginSubpath (colX (0.0), static_cast<CCoord> (h));
	for (int i = 0; i < kNumCols; ++i)
	{
		double xNorm = (static_cast<double> (i) + 0.5) / kNumCols; // bin center
		float y = toY (dbA_[i]);
		fillPath->addLine (colX (xNorm), y);
	}
	fillPath->addLine (colX (1.0), static_cast<CCoord> (h));
	fillPath->closeSubpath ();

	pContext->setFillColor ({70, 160, 220, 110}); // semi-transparent blue fill
	pContext->drawGraphicsPath (fillPath, CDrawContext::kPathFilled);
	fillPath->forget ();

	// stroke the top edge
	CGraphicsPath* linePath = pContext->createGraphicsPath ();
	linePath->beginSubpath (colX (0.0), toY (dbA_[0]));
	for (int i = 1; i < kNumCols; ++i)
	{
		double xNorm = (static_cast<double> (i) + 0.5) / kNumCols;
		linePath->addLine (colX (xNorm), toY (dbA_[i]));
	}
	pContext->setFrameColor ({140, 210, 255, 255});
	pContext->setLineWidth (1.f);
	pContext->drawGraphicsPath (linePath, CDrawContext::kPathStroked);
	linePath->forget ();

	//--- channel label ----------------------------------------------------------
	pContext->setFontColor ({230, 230, 230, 255});
	pContext->drawString (label_, CPoint (plotLeft + 6, 4));
}

} // namespace sectrascope
} // namespace vdplg
