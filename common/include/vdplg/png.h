// vdplg::png - minimal PNG read/compare helpers (F4 golden-image compare).
//
// Decoder supports the subset of PNG that our toolchain produces:
//   - bit depths 8 and 16
//   - color types 0 (grayscale), 2 (RGB), 4 (gray+alpha), 6 (RGBA)
//   - all five scanline filters (None/Sub/Up/Average/Paeth)
// Everything is decoded to 8-bit RGBA so comparisons are uniform.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vdplg {

struct PngImage
{
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgba; // width*height*4, row-major top-to-bottom
};

// Decode a PNG file into an 8-bit RGBA buffer.
bool loadPng(const std::string& path, PngImage& out, std::string& err);

// Count differing pixels between two same-size images. A pixel differs when
// any channel deviates by more than `tolerance` (0..255 scale). Returns false
// on size mismatch or decode failure (err filled).
bool countPixelDiff(const PngImage& a, const PngImage& b, int tolerance,
                    uint64_t& diffCount, std::string& err);

// Convenience: load both files from disk and report whether the fraction of
// differing pixels exceeds `maxFraction`. ok=false means "differs too much"
// OR a hard error (see err).
bool compareGoldenUi(const std::string& capturedPath, const std::string& refPath,
                     double maxFraction, int tolerance, bool& withinTolerance,
                     double& diffFraction, std::string& err);

} // namespace vdplg
