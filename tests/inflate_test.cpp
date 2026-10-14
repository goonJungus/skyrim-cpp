// inflate_test.cpp - verifies the module's DEFLATE decoder against
// reference streams produced by zlib (see make_fixtures.py).
//
// These fixtures are synthetic patterns we generated ourselves; no game
// content is involved. The point is a cross-implementation check: Python's
// zlib compresses, our decoder decompresses, results must match byte for byte.

#include "world/inflate.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> readFile(const std::string& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { ok = false; return {}; }
    f.seekg(0, std::ios::end);
    const auto size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (size > 0) f.read(reinterpret_cast<char*>(buf.data()), size);
    ok = static_cast<bool>(f) || f.eof();
    return buf;
}

int failures = 0;

void expect(bool cond, const std::string& what) {
    if (!cond) { std::printf("    FAIL: %s\n", what.c_str()); failures++; }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: inflate_test <fixture-dir>\n");
        return 2;
    }
    const std::string dir = argv[1];

    struct Case { const char* name; };
    const Case cases[] = {
        {"text"}, {"runs"}, {"random"}, {"tiny"}, {"empty"}, {"records"},
    };

    for (const Case& c : cases) {
        bool okRaw = false, okZ = false;
        const std::vector<uint8_t> raw = readFile(dir + "/" + c.name + ".raw", okRaw);
        const std::vector<uint8_t> z   = readFile(dir + "/" + c.name + ".z", okZ);
        if (!okRaw || !okZ) {
            std::printf("  %-10s SKIP (fixture unreadable)\n", c.name);
            continue;
        }

        // --- zlib wrapper ---
        std::vector<uint8_t> out;
        const bool zok = skyrim::zip::zlibInflate(z.data(), z.size(), out);
        expect(zok, std::string(c.name) + ": zlibInflate returned false");
        expect(out.size() == raw.size(),
               std::string(c.name) + ": size " + std::to_string(out.size()) +
               " != " + std::to_string(raw.size()));
        if (zok && out.size() == raw.size()) {
            expect(out == raw, std::string(c.name) + ": content mismatch");
        }
        std::printf("  %-10s zlib    %s  (%zu bytes)\n", c.name,
                    zok ? "ok" : "FAILED", out.size());

        // --- raw deflate (strip 2-byte header and 4-byte Adler-32) ---
        if (z.size() >= 6) {
            std::vector<uint8_t> out2;
            const bool rok = skyrim::zip::rawInflate(z.data() + 2, z.size() - 6, out2);
            expect(rok, std::string(c.name) + ": rawInflate returned false");
            expect(out2 == raw, std::string(c.name) + ": raw content mismatch");
            std::printf("  %-10s raw     %s  (%zu bytes)\n", c.name,
                        rok ? "ok" : "FAILED", out2.size());
        }
    }

    // --- malformed input must be rejected, not crash or over-read ---
    std::printf("\n  malformed-input checks:\n");
    std::vector<uint8_t> out;
    const uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFF, 0xFF, 0xFF, 0xFF};
    expect(!skyrim::zip::zlibInflate(garbage, sizeof(garbage), out), "garbage rejected");
    expect(!skyrim::zip::rawInflate(garbage, sizeof(garbage), out), "garbage raw rejected");
    expect(!skyrim::zip::zlibInflate(nullptr, 0, out), "null rejected");
    expect(!skyrim::zip::zlibInflate(garbage, 3, out), "short rejected");

    // A valid zlib header over a truncated deflate body must fail cleanly.
    const uint8_t truncated[] = {0x78, 0x9C, 0x63, 0x60};
    expect(!skyrim::zip::zlibInflate(truncated, sizeof(truncated), out),
           "truncated stream rejected");

    // Wrong CM (compression method) must be refused.
    const uint8_t wrongMethod[] = {0x79, 0x9C, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
    expect(!skyrim::zip::zlibInflate(wrongMethod, sizeof(wrongMethod), out),
           "non-deflate CM rejected");

    // Corrupt Adler-32 must be caught on a well-formed stream.
    {
        bool ok = false;
        std::vector<uint8_t> z = readFile(dir + "/text.z", ok);
        if (ok && z.size() > 4) {
            std::vector<uint8_t> bad = z;
            bad[bad.size() - 1] ^= 0xFF;
            expect(!skyrim::zip::zlibInflate(bad.data(), bad.size(), out),
                   "bad Adler-32 rejected");
        }
    }

    // Decompression limit must be honoured (bomb guard).
    {
        bool ok = false;
        std::vector<uint8_t> z = readFile(dir + "/runs.z", ok);
        if (ok) {
            expect(!skyrim::zip::zlibInflate(z.data(), z.size(), out, 1024),
                   "output limit enforced");
        }
    }
    std::printf("    done\n");

    if (failures == 0) {
        std::printf("\nALL INFLATE TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d INFLATE CHECK(S) FAILED\n", failures);
    return 1;
}
