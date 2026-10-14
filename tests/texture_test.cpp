// texture_test.cpp - verifies the DDS decoder against synthetic textures whose
// bytes and expected pixels we built ourselves (see make_dds_fixture.py).
//
// The DXT1 block under test is c0=red, c1=green with indices 0,1,2,3 across
// the top row, so the decoded top row must be the four palette entries:
//   red, green, (2/3 red + 1/3 green), (1/3 red + 2/3 green)
// This is checked by hand below, so a wrong palette formula fails loudly.

#include "skyrim/texture.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool cond, const std::string& what) {
    if (!cond) { std::printf("  FAIL: %s\n", what.c_str()); failures++; }
}

struct Pixel { uint8_t r, g, b, a; };

Pixel at(const skyrim::Image& img, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * img.width + x) * 4u;
    return Pixel{img.rgba[i], img.rgba[i + 1], img.rgba[i + 2], img.rgba[i + 3]};
}

bool pxEq(Pixel p, Pixel q) {
    return p.r == q.r && p.g == q.g && p.b == q.b && p.a == q.a;
}

std::string pxStr(Pixel p) {
    char b[64];
    std::snprintf(b, sizeof(b), "(%u,%u,%u,%u)", p.r, p.g, p.b, p.a);
    return b;
}

void expectPixel(const skyrim::Image& img, int x, int y, Pixel want,
                 const std::string& what) {
    if (!img.valid()) { expect(false, what + " (image invalid)"); return; }
    const Pixel got = at(img, x, y);
    if (!pxEq(got, want)) {
        expect(false, what + ": want " + pxStr(want) + " got " + pxStr(got));
    }
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

// Expected 4x4 result of the shared DXT1 colour block, with a chosen alpha.
void check4x4Colours(const skyrim::Image& img, uint8_t alpha, const std::string& tag) {
    const Pixel red{255, 0, 0, alpha};
    const Pixel green{0, 255, 0, alpha};
    const Pixel pal2{170, 85, 0, alpha};
    const Pixel pal3{85, 170, 0, alpha};
    expect(img.width == 4 && img.height == 4, tag + " is 4x4");
    expectPixel(img, 0, 0, red, tag + " (0,0)");
    expectPixel(img, 1, 0, green, tag + " (1,0)");
    expectPixel(img, 2, 0, pal2, tag + " (2,0)");
    expectPixel(img, 3, 0, pal3, tag + " (3,0)");
    for (int y = 1; y < 4; y++)
        for (int x = 0; x < 4; x++)
            expectPixel(img, x, y, red, tag + " row " + std::to_string(y));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: texture_test <fixture-dir>\n");
        return 2;
    }
    const std::string dir = argv[1];

    // ---- header inspection ----
    {
        const auto bytes = readFile(dir + "/dxt1.dds");
        expect(!bytes.empty(), "dxt1.dds readable");
        skyrim::TextureInfo info;
        std::string err;
        expect(skyrim::inspectDDS(bytes.data(), bytes.size(), info, &err),
               "inspect dxt1: " + err);
        expect(info.width == 4 && info.height == 4, "dxt1 is 4x4");
        expect(info.format == skyrim::TextureFormat::BC1, "dxt1 format BC1");
        expect(info.compressed && info.blockBytes == 8, "dxt1 block 8 bytes");
    }

    // ---- uncompressed 2x2 BGRA ----
    {
        const auto bytes = readFile(dir + "/rgba32.dds");
        skyrim::Image img;
        std::string err;
        expect(skyrim::decodeDDS(bytes.data(), bytes.size(), img, &err),
               "decode rgba32: " + err);
        expect(img.valid(), "rgba32 valid");
        expectPixel(img, 0, 0, Pixel{10, 20, 30, 255}, "rgba32 (0,0)");
        expectPixel(img, 1, 0, Pixel{200, 100, 50, 128}, "rgba32 (1,0)");
        expectPixel(img, 0, 1, Pixel{0, 255, 0, 255}, "rgba32 (0,1)");
        expectPixel(img, 1, 1, Pixel{255, 255, 255, 64}, "rgba32 (1,1)");
    }

    // ---- BC1 / DXT1 ----
    {
        const auto bytes = readFile(dir + "/dxt1.dds");
        skyrim::Image img;
        std::string err;
        expect(skyrim::decodeDDS(bytes.data(), bytes.size(), img, &err),
               "decode dxt1: " + err);
        check4x4Colours(img, 255, "dxt1");
    }

    // ---- BC2 / DXT3 (explicit 4-bit alpha = 0xFF) ----
    {
        const auto bytes = readFile(dir + "/dxt3.dds");
        skyrim::Image img;
        std::string err;
        expect(skyrim::decodeDDS(bytes.data(), bytes.size(), img, &err),
               "decode dxt3: " + err);
        check4x4Colours(img, 255, "dxt3");
    }

    // ---- BC3 / DXT5 (interpolated alpha, all indices 0 -> a0 = 200) ----
    {
        const auto bytes = readFile(dir + "/dxt5.dds");
        skyrim::Image img;
        std::string err;
        expect(skyrim::decodeDDS(bytes.data(), bytes.size(), img, &err),
               "decode dxt5: " + err);
        check4x4Colours(img, 200, "dxt5");
    }

    // ---- BC1 behind a DX10 extended header ----
    {
        const auto bytes = readFile(dir + "/dx10_bc1.dds");
        skyrim::Image img;
        std::string err;
        expect(skyrim::decodeDDS(bytes.data(), bytes.size(), img, &err),
               "decode dx10 bc1: " + err);
        check4x4Colours(img, 255, "dx10_bc1");
    }

    // ---- robustness ----
    {
        const auto bytes = readFile(dir + "/bad_magic.dds");
        skyrim::Image img;
        std::string err;
        expect(!skyrim::decodeDDS(bytes.data(), bytes.size(), img, &err),
               "bad magic rejected");
        expect(!err.empty(), "bad magic gives a reason");
    }
    {
        const auto bytes = readFile(dir + "/truncated.dds");
        skyrim::Image img;
        std::string err;
        expect(!skyrim::decodeDDS(bytes.data(), bytes.size(), img, &err),
               "truncated body rejected");
    }
    {
        skyrim::Image img;
        std::string err;
        expect(!skyrim::decodeDDS(nullptr, 0, img, &err), "null buffer rejected");
    }

    if (failures == 0) {
        std::printf("ALL TEXTURE TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEXTURE CHECK(S) FAILED\n", failures);
    return 1;
}
