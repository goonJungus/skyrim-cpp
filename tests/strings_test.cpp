// strings_test.cpp - verify the .STRINGS reader against a real file.
//
// The format (confirmed against Mutagen.Bethesda.Core/Strings/StringsWriter.cs):
//   u32 count
//   u32 contentLength
//   (u32 index, u32 offset) * count
//   char data[]                        NUL-terminated strings
//
// Offsets are cumulative encoded-byte positions relative to the start of data[].

#include "skyrim/string_table.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;

void expect(bool cond, const std::string& what) {
    if (!cond) { std::printf("  FAIL: %s\n", what.c_str()); failures++; }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: strings_test <file.STRINGS>\n");
        return 2;
    }
    const std::string path = argv[1];

    skyrim::StringTable t;
    if (!t.loadFile(path)) {
        std::printf("FAIL: could not load %s\n", path.c_str());
        return 1;
    }

    std::printf("loaded %s\n", path.c_str());
    std::printf("  entries:  %zu\n", t.entryCount());
    std::printf("  resolved: %.1f%%\n", t.resolvedFraction() * 100.0f);

    // These ids are known to exist in Skyrim_English.STRINGS (verified by probe).
    struct { uint32_t id; const char* want; } probes[] = {
        {1,  "The Ratway Vaults"},
        {2,  "Windhelm Stable Services"},
        {3,  "Razelan"},
        {6,  "Why are you looking for a Dragonborn?"},
        {9,  "Stand"},
    };

    for (const auto& pr : probes) {
        const std::string got = t.lookup(pr.id);
        const bool ok = (got == pr.want);
        std::printf("  id=%-6u %-40s %s\n", pr.id, got.c_str(), ok ? "OK" : "MISMATCH");
        if (!ok) {
            std::printf("    expected: %s\n", pr.want);
            failures++;
        }
    }

    // A nonexistent id must return empty.
    expect(t.lookup(0xFFFFFFFEu).empty(), "nonexistent id returns empty");

    if (failures == 0) {
        std::printf("ALL STRING TABLE TESTS PASSED\n");
        return 0;
    }
    std::printf("%d STRING TABLE CHECK(S) FAILED\n", failures);
    return 1;
}
