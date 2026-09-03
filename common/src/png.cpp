// vdplg::png - minimal PNG reader + golden-image comparison (F4).
//
// Decoding uses miniz's tinfl (public domain) for IDAT inflation; we define
// MINIZ_NO_* for every API family we don't need so only the inflate core is
// compiled. Supports: bit depths 8/16, color types 0/2/4/6, all five
// scanline filters, non-interlaced images. Output is normalized 8-bit RGBA.
#include "vdplg/png.h"

#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_DEFLATE_APIS
extern "C" {
#include "../../third_party/miniz/miniz.h"
}

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace vdplg {
namespace {

struct Reader
{
	const std::vector<char>& buf;
	std::size_t pos = 0;

	bool readBytes(void* dst, std::size_t n)
	{
		if (pos + n > buf.size())
			return false;
		std::memcpy(dst, buf.data() + pos, n);
		pos += n;
		return true;
	}
	uint32_t readU32BE()
	{
		unsigned char b[4];
		if (!readBytes(b, 4))
			return 0;
		return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
	}
};

inline int paeth(int a, int b, int c)
{
	const int p = a + b - c;
	const int pa = std::abs(p - a);
	const int pb = std::abs(p - b);
	const int pc = std::abs(p - c);
	if (pa <= pb && pa <= pc)
		return a;
	if (pb <= pc)
		return b;
	return c;
}

bool unfilterRow(const uint8_t* prevRow, const uint8_t* filtered, std::size_t stride, int bpp,
                 std::vector<uint8_t>& out)
{
	out.assign(stride, 0);
	const uint8_t type = filtered[0];
	for (std::size_t x = 0; x < stride; ++x)
	{
		const int a = (x >= static_cast<std::size_t>(bpp)) ? out[x - bpp] : 0;
		const int b = prevRow ? prevRow[x] : 0;
		const int c = (prevRow && x >= static_cast<std::size_t>(bpp)) ? prevRow[x - bpp] : 0;
		int recon = 0;
		switch (type)
		{
			case 0: recon = filtered[x + 1]; break;
			case 1: recon = filtered[x + 1] + a; break;
			case 2: recon = filtered[x + 1] + b; break;
			case 3: recon = filtered[x + 1] + (a + b) / 2; break;
			case 4: recon = filtered[x + 1] + paeth(a, b, c); break;
			default: return false; // unknown filter
		}
		out[x] = static_cast<uint8_t>(recon & 0xFF);
	}
	return true;
}

// Expand one raw pixel into RGBA bytes. bitDepth is 8 or 16.
void expandPixel(const uint8_t* src, int bitDepth, int colorType, uint8_t* rgbaOut)
{
	auto chan8 = [&](int ch) -> uint8_t {
		if (bitDepth == 16)
			return static_cast<uint8_t>((src[ch * 2] << 8 | src[ch * 2 + 1]) >> 8);
		return src[ch];
	};
	rgbaOut[3] = 255;
	switch (colorType)
	{
		case 0: { // gray
			const uint8_t g = chan8(0);
			rgbaOut[0] = rgbaOut[1] = rgbaOut[2] = g;
			break;
		}
		case 2: // RGB
			for (int i = 0; i < 3; ++i)
				rgbaOut[i] = chan8(i);
			break;
		case 4: { // gray+alpha
			rgbaOut[0] = rgbaOut[1] = rgbaOut[2] = chan8(0);
			rgbaOut[3] = chan8(1);
			break;
		}
		case 6: // RGBA
			for (int i = 0; i < 4; ++i)
				rgbaOut[i] = chan8(i);
			break;
		default: break;
	}
}

bool inflateAll(const std::vector<uint8_t>& idat, std::vector<uint8_t>& out, std::string& err)
{
	// Deflate can expand up to ~1033x per 64 KB block, so size the buffer by
	// the worst-case expansion ratio rather than a fixed multiple.
	const size_t cap = (idat.size() / 65536 + 1) * 65536u * 1033u + 64 * 1024;
	std::vector<uint8_t> tmp(cap);
	const size_t n = tinfl_decompress_mem_to_mem(tmp.data(), tmp.size(), idat.data(), idat.size(),
	                                             TINFL_FLAG_PARSE_ZLIB_HEADER);
	if (n == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED)
	{
		err = "inflate failed while decoding IDAT";
		return false;
	}
	out.assign(tmp.begin(), tmp.begin() + n);
	return true;
}

} // namespace

bool loadPng(const std::string& path, PngImage& out, std::string& err)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
	{
		err = "cannot open PNG: " + path;
		return false;
	}
	std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	f.close();

	static const unsigned char sig[] = {137, 80, 78, 71, 13, 10, 26, 10};
	if (buf.size() < 8 || std::memcmp(buf.data(), sig, 8) != 0)
	{
		err = "not a PNG file: " + path;
		return false;
	}

	Reader r {buf};
	r.pos = 8;

	int width = 0, height = 0, bitDepth = 0, colorType = 0, interlace = 0;
	bool haveIhdr = false;
	std::vector<uint8_t> idat;

	while (r.pos + 8 <= buf.size())
	{
		const uint32_t len = r.readU32BE();
		char type[5] {};
		if (!r.readBytes(type, 4))
			break;
		type[4] = '\0';
		const std::size_t payloadStart = r.pos;
		if (len > buf.size() - r.pos)
		{
			err = "truncated chunk in " + path;
			return false;
		}
		if (std::strcmp(type, "IHDR") == 0 && len >= 13)
		{
			width = static_cast<int>(r.readU32BE());
			height = static_cast<int>(r.readU32BE());
			unsigned char bd = 0, ct = 0, comp = 0, filt = 0, il = 0;
			r.readBytes(&bd, 1);
			r.readBytes(&ct, 1);
			r.readBytes(&comp, 1);
			r.readBytes(&filt, 1);
			r.readBytes(&il, 1);
			bitDepth = bd;
			colorType = ct;
			interlace = il;
			haveIhdr = true;
		}
		else if (std::strcmp(type, "IDAT") == 0)
		{
			idat.insert(idat.end(), reinterpret_cast<const uint8_t*>(buf.data()) + payloadStart,
			            reinterpret_cast<const uint8_t*>(buf.data()) + payloadStart + len);
		}
		else if (std::strcmp(type, "IEND") == 0)
		{
			break;
		}
		r.pos = payloadStart + len + 4; // skip payload + CRC
	}

	if (!haveIhdr || width <= 0 || height <= 0)
	{
		err = "missing or invalid IHDR in " + path;
		return false;
	}
	if (interlace != 0)
	{
		err = "interlaced PNG not supported: " + path;
		return false;
	}
	const int channels = colorType == 0 ? 1 : colorType == 2 ? 3 : colorType == 4 ? 2 : colorType == 6 ? 4 : -1;
	if (channels < 0)
	{
		err = "unsupported color type " + std::to_string(colorType) + " in " + path;
		return false;
	}
	if (bitDepth != 8 && bitDepth != 16)
	{
		err = "unsupported bit depth " + std::to_string(bitDepth) + " in " + path;
		return false;
	}

	std::vector<uint8_t> raw;
	if (!inflateAll(idat, raw, err))
		return false;

	const int bytesPerCh = bitDepth / 8;
	const int bpp = channels * bytesPerCh; // bytes per pixel before filtering
	const std::size_t stride = static_cast<std::size_t>(width) * bpp;
	if (raw.size() < static_cast<std::size_t>(height) * (stride + 1))
	{
		err = "decoded IDAT too small for image dimensions";
		return false;
	}

	out.width = width;
	out.height = height;
	out.rgba.resize(static_cast<std::size_t>(width) * height * 4);

	std::vector<uint8_t> prevRow(stride), curRow;
	for (int y = 0; y < height; ++y)
	{
		const uint8_t* filtered = raw.data() + static_cast<std::size_t>(y) * (stride + 1);
		if (!unfilterRow(y > 0 ? prevRow.data() : nullptr, filtered, stride, bpp, curRow))
		{
			err = "unknown scanline filter in row " + std::to_string(y);
			return false;
		}
		uint8_t* dst = out.rgba.data() + static_cast<std::size_t>(y) * width * 4;
		for (int x = 0; x < width; ++x)
			expandPixel(curRow.data() + static_cast<std::size_t>(x) * bpp, bitDepth, colorType, dst + x * 4);
		prevRow.swap(curRow);
	}
	return true;
}

bool countPixelDiff(const PngImage& a, const PngImage& b, int tolerance, uint64_t& diffCount, std::string& err)
{
	diffCount = 0;
	if (a.width != b.width || a.height != b.height)
	{
		err = "dimension mismatch: " + std::to_string(a.width) + "x" + std::to_string(a.height) +
		      " vs " + std::to_string(b.width) + "x" + std::to_string(b.height);
		return false;
	}
	const auto& pa = a.rgba;
	const auto& pb = b.rgba;
	for (std::size_t i = 0; i < pa.size(); i += 4)
	{
		bool differs = false;
		for (int c = 0; c < 4; ++c)
			if (std::abs(static_cast<int>(pa[i + c]) - static_cast<int>(pb[i + c])) > tolerance)
			{
				differs = true;
				break;
			}
		if (differs)
			++diffCount;
	}
	return true;
}

bool compareGoldenUi(const std::string& capturedPath, const std::string& refPath, double maxFraction,
                     int tolerance, bool& withinTolerance, double& diffFraction, std::string& err)
{
	withinTolerance = false;
	diffFraction = 0.0;
	PngImage cap, ref;
	std::string e1, e2;
	if (!loadPng(capturedPath, cap, e1))
	{
		err = e1;
		return false;
	}
	if (!loadPng(refPath, ref, e2))
	{
		err = e2;
		return false;
	}
	uint64_t diff = 0;
	if (!countPixelDiff(cap, ref, tolerance, diff, err))
		return false;
	const double total = static_cast<double>(cap.width) * static_cast<double>(cap.height);
	diffFraction = total > 0 ? static_cast<double>(diff) / total : 0.0;
	withinTolerance = diffFraction <= maxFraction;
	if (!withinTolerance)
		err = "pixel diff fraction " + std::to_string(diffFraction) + " exceeds limit " +
		      std::to_string(maxFraction);
	return true;
}

} // namespace vdplg
