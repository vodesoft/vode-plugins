// L0: vdplg::png — PNG decode + golden-image comparison (F4).
//
// Tests build tiny PNGs in memory (filter 0 only) with exact known pixels,
// so decode correctness is checked value-by-value rather than against a
// committed binary fixture.
#include "vdplg/png.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
using Catch::Approx;

uint32_t crc32(const uint8_t* data, size_t len)
{
	static uint32_t table[256];
	static bool init = false;
	if (!init)
	{
		for (uint32_t i = 0; i < 256; ++i)
		{
			uint32_t c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		init = true;
	}
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; ++i)
		c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

uint32_t adler32(const uint8_t* data, size_t len)
{
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < len; ++i)
	{
		a = (a + data[i]) % 65521u;
		b = (b + a) % 65521u;
	}
	return (b << 16) | a;
}

void appendU32BE(std::vector<uint8_t>& v, uint32_t x)
{
	v.push_back(uint8_t(x >> 24));
	v.push_back(uint8_t(x >> 16));
	v.push_back(uint8_t(x >> 8));
	v.push_back(uint8_t(x));
}

void appendChunk(std::vector<uint8_t>& v, const char type[4], const std::vector<uint8_t>& payload)
{
	appendU32BE(v, static_cast<uint32_t>(payload.size()));
	const uint8_t* tp = reinterpret_cast<const uint8_t*>(type);
	v.insert(v.end(), tp, tp + 4);
	v.insert(v.end(), payload.begin(), payload.end());
	std::vector<uint8_t> crcInput(4 + payload.size());
	std::memcpy(crcInput.data(), tp, 4);
	std::memcpy(crcInput.data() + 4, payload.data(), payload.size());
	appendU32BE(v, crc32(crcInput.data(), crcInput.size()));
}

// Build a non-interlaced PNG. rgba is w*h*4; gray/gray+alpha use R as the
// gray value and A for alpha. bitDepth 8 or 16 (16-bit stored big-endian).
std::vector<uint8_t> makePng(int w, int h, int colorType, int bitDepth, const std::vector<uint8_t>& rgba)
{
	std::vector<uint8_t> out {137, 80, 78, 71, 13, 10, 26, 10};

	std::vector<uint8_t> ihdr;
	appendU32BE(ihdr, static_cast<uint32_t>(w));
	appendU32BE(ihdr, static_cast<uint32_t>(h));
	ihdr.push_back(static_cast<uint8_t>(bitDepth));
	ihdr.push_back(static_cast<uint8_t>(colorType));
	ihdr.push_back(0); // compression
	ihdr.push_back(0); // filter
	ihdr.push_back(0); // interlace
	appendChunk(out, "IHDR", ihdr);

	const int channels = colorType == 0 ? 1 : colorType == 2 ? 3 : colorType == 4 ? 2 : 4;
	const int bytesPerCh = bitDepth / 8;
	const size_t stride = static_cast<size_t>(w) * channels * bytesPerCh;
	std::vector<uint8_t> raw;
	for (int y = 0; y < h; ++y)
	{
		raw.push_back(0); // filter: None
		for (int x = 0; x < w; ++x)
		{
			const uint8_t* p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
			auto emitChan = [&](int chIdx) {
				uint16_t val = 0;
				switch (colorType)
				{
					case 0: val = p[0]; break;
					case 4: val = chIdx == 0 ? p[0] : p[3]; break;
					default: val = p[chIdx]; break;
				}
				if (bytesPerCh == 2)
					val = static_cast<uint16_t>(val << 8); // fixture stores hi byte only
				if (bytesPerCh == 2)
				{
					raw.push_back(static_cast<uint8_t>(val >> 8));
					raw.push_back(val);
				}
				else
					raw.push_back(val);
			};
			for (int c = 0; c < channels; ++c)
				emitChan(c);
		}
	}
	// IDAT must carry a zlib stream: 2-byte header + stored block + adler32.
	std::vector<uint8_t> idat {0x78, 0x01}; // CMF/FLG (deflate, no dict)
	idat.push_back(0x01); // BFINAL=1, BTYPE=00 (stored)
	const uint16_t len16 = static_cast<uint16_t>(raw.size());
	idat.push_back(uint8_t(len16 & 0xFF));
	idat.push_back(uint8_t((len16 >> 8) & 0xFF));
	idat.push_back(uint8_t(~len16 & 0xFF));
	idat.push_back(uint8_t((~len16 >> 8) & 0xFF));
	idat.insert(idat.end(), raw.begin(), raw.end());
	const uint32_t adler = adler32(raw.data(), raw.size());
	idat.push_back(uint8_t(adler >> 24));
	idat.push_back(uint8_t(adler >> 16));
	idat.push_back(uint8_t(adler >> 8));
	idat.push_back(uint8_t(adler));
	appendChunk(out, "IDAT", idat);
	appendChunk(out, "IEND", {});
	return out;
}

std::string tempPngPath(const std::string& name)
{
	static int counter = 0;
	std::ostringstream oss;
	oss << "vdplg_test_" << ++counter << "_" << name << ".png";
	return oss.str();
}

void writePngFile(const std::string& path, const std::vector<uint8_t>& data)
{
	std::ofstream f(path, std::ios::binary);
	f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

bool removeQuietly(const std::string& path)
{
	return std::remove(path.c_str()) == 0;
}

} // namespace

TEST_CASE("png: decode RGB 8-bit image with exact pixels", "[ui][l0]")
{
	// 2x2 RGB: red, green / blue, white. Alpha ignored on encode, must be 255.
	std::vector<uint8_t> px = {
		255, 0, 0, 255,   0, 200, 0, 255,
		0, 0, 255, 255,   255, 255, 255, 255,
	};
	const auto png = makePng(2, 2, /*colorType=*/2, /*bitDepth=*/8, px);
	const std::string path = tempPngPath("rgb");
	REQUIRE_FALSE(path.empty());
	writePngFile(path, png);

	vdplg::PngImage img;
	std::string err;
	REQUIRE(vdplg::loadPng(path, img, err));
	CHECK(img.width == 2);
	CHECK(img.height == 2);
	CHECK(img.rgba.size() == 16u);
	const uint8_t expect[16] = {
		255, 0, 0, 255,   0, 200, 0, 255,
		0, 0, 255, 255,   255, 255, 255, 255,
	};
	for (int i = 0; i < 16; ++i)
		CHECK(img.rgba[i] == expect[i]);
	removeQuietly(path);
}

TEST_CASE("png: decode gray+alpha and 16-bit RGB", "[ui][l0]")
{
	// Gray+alpha 4x1: gray values 0/64/128/255, alpha 255/128/0/255.
	std::vector<uint8_t> ga = {
		0, 0, 0, 255,   64, 0, 0, 128,   128, 0, 0, 0,   255, 0, 0, 255,
	};
	auto pngGa = makePng(4, 1, /*colorType=*/4, /*bitDepth=*/8, ga);
	std::string pGa = tempPngPath("ga");
	REQUIRE_FALSE(pGa.empty());
	writePngFile(pGa, pngGa);
	vdplg::PngImage gaImg;
	std::string err;
	REQUIRE(vdplg::loadPng(pGa, gaImg, err));
	CHECK(gaImg.rgba[0 * 4 + 0] == 0);
	CHECK(gaImg.rgba[0 * 4 + 3] == 255);
	CHECK(gaImg.rgba[1 * 4 + 0] == 64);
	CHECK(gaImg.rgba[1 * 4 + 3] == 128);
	CHECK(gaImg.rgba[2 * 4 + 0] == 128);
	CHECK(gaImg.rgba[2 * 4 + 3] == 0);
	CHECK(gaImg.rgba[3 * 4 + 0] == 255);
	removeQuietly(pGa);

	// 16-bit RGB 2x1: decode keeps the high byte ((hi<<8|lo)>>8).
	// Fixture rows are 4 bytes holding the HIGH byte per channel (+A ignored).
	std::vector<uint8_t> hi = {
		0x10, 0x20, 0x30, 0x00, // -> 0x10,0x20,0x30
		0xFF, 0xFE, 0xFD, 0x00, // -> 0xFF,0xFE,0xFD
	};
	auto pngHi = makePng(2, 1, /*colorType=*/2, /*bitDepth=*/16, hi);
	std::string pHi = tempPngPath("hi");
	REQUIRE_FALSE(pHi.empty());
	writePngFile(pHi, pngHi);
	vdplg::PngImage hiImg;
	REQUIRE(vdplg::loadPng(pHi, hiImg, err));
	CHECK(hiImg.rgba[0] == 0x10);
	CHECK(hiImg.rgba[1] == 0x20);
	CHECK(hiImg.rgba[2] == 0x30);
	CHECK(hiImg.rgba[3] == 255);
	CHECK(hiImg.rgba[4] == 0xFF);
	CHECK(hiImg.rgba[5] == 0xFE);
	CHECK(hiImg.rgba[6] == 0xFD);
	removeQuietly(pHi);
}

TEST_CASE("png: loadPng rejects missing file and non-PNG data", "[ui][l0]")
{
	vdplg::PngImage img;
	std::string err;
	CHECK_FALSE(vdplg::loadPng("vdplg_definitely_missing_file_xyz.png", img, err));
	CHECK_FALSE(err.empty());

	const std::string path = tempPngPath("bad");
	REQUIRE_FALSE(path.empty());
	{
		std::ofstream f(path, std::ios::binary);
		f << "this is not a png at all";
	}
	CHECK_FALSE(vdplg::loadPng(path, img, err));
	CHECK_FALSE(err.empty());
	removeQuietly(path);
}

TEST_CASE("png: countPixelDiff tolerance behavior", "[ui][l0]")
{
	std::vector<uint8_t> base(8 * 4, 0);
	for (int i = 0; i < 8; ++i)
		base[i * 4 + 3] = 255; // opaque black 2x4
	auto pngA = makePng(2, 4, 2, 8, base);
	std::string pA = tempPngPath("a");
	std::string pB = tempPngPath("b");
	REQUIRE_FALSE(pA.empty());
	REQUIRE_FALSE(pB.empty());
	writePngFile(pA, pngA);

	// Identical -> 0 diffs.
	writePngFile(pB, pngA);
	vdplg::PngImage a, b;
	std::string err;
	REQUIRE(vdplg::loadPng(pA, a, err));
	REQUIRE(vdplg::loadPng(pB, b, err));
	uint64_t diff = 99;
	REQUIRE(vdplg::countPixelDiff(a, b, /*tolerance=*/0, diff, err));
	CHECK(diff == 0u);

	// Bump one pixel's red by 1: within tol=1, outside tol=0.
	std::vector<uint8_t> mod = base;
	mod[0] = 1;
	writePngFile(pB, makePng(2, 4, 2, 8, mod));
	REQUIRE(vdplg::loadPng(pB, b, err));
	REQUIRE(vdplg::countPixelDiff(a, b, /*tolerance=*/1, diff, err));
	CHECK(diff == 0u);
	REQUIRE(vdplg::countPixelDiff(a, b, /*tolerance=*/0, diff, err));
	CHECK(diff == 1u);

	// Dimension mismatch -> false.
	std::vector<uint8_t> other(4 * 4, 0);
	vdplg::PngImage c;
	writePngFile(pB, makePng(2, 2, 2, 8, other));
	REQUIRE(vdplg::loadPng(pB, c, err));
	CHECK_FALSE(vdplg::countPixelDiff(a, c, 0, diff, err));
	CHECK_FALSE(err.empty());
	removeQuietly(pA);
	removeQuietly(pB);
}

TEST_CASE("png: compareGoldenUi fraction math", "[ui][l0]")
{
	// 4x4 image; change exactly 1 of 16 pixels beyond tolerance.
	std::vector<uint8_t> base(16 * 4, 0);
	for (int i = 0; i < 16; ++i)
		base[i * 4 + 3] = 255;
	auto pngRef = makePng(4, 4, 2, 8, base);
	std::string pRef = tempPngPath("ref");
	std::string pCap = tempPngPath("cap");
	REQUIRE_FALSE(pRef.empty());
	REQUIRE_FALSE(pCap.empty());
	writePngFile(pRef, pngRef);

	std::vector<uint8_t> cap = base;
	cap[4] = 200; // first pixel of row 1, red channel
	writePngFile(pCap, makePng(4, 4, 2, 8, cap));

	bool ok = false;
	double frac = -1.0;
	std::string err;
	// maxFraction 0.0625 == 1/16 -> within (<=).
	REQUIRE(vdplg::compareGoldenUi(pCap, pRef, 1.0 / 16.0, /*tolerance=*/0, ok, frac, err));
	CHECK(ok);
	CHECK(frac == Approx(1.0 / 16.0));
	// maxFraction just below -> fail with explanatory error.
	err.clear();
	REQUIRE(vdplg::compareGoldenUi(pCap, pRef, 0.01, 0, ok, frac, err));
	CHECK_FALSE(ok);
	CHECK(frac == Approx(1.0 / 16.0));
	CHECK_FALSE(err.empty());
	// Identical capture -> 0 fraction, passes any limit.
	writePngFile(pCap, pngRef);
	REQUIRE(vdplg::compareGoldenUi(pCap, pRef, 0.0, 0, ok, frac, err));
	CHECK(ok);
	CHECK(frac == 0.0);
	// Missing reference -> hard failure.
	CHECK_FALSE(vdplg::compareGoldenUi(pCap, "vdplg_missing_ref_xyz.png", 0.5, 0, ok, frac, err));
	CHECK_FALSE(err.empty());
	removeQuietly(pRef);
	removeQuietly(pCap);
}
