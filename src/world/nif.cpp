// nif.cpp - Minimal runtime NIF parser (20.2.0.7 skeleton).
//
// This is a focused parser skeleton. Full block walking will be implemented
// incrementally with fixture-based tests.

#include "skyrim/mesh.hpp"

#include <cstring>
#include <fstream>
#include <sstream>

namespace skyrim {
namespace {

uint32_t rdU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t rdU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

}  // namespace

bool loadNifFromFile(const std::string& path, Mesh& out, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        if (error) *error = "cannot open: " + path;
        return false;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    return loadNif(buf.data(), buf.size(), out, error);
}

bool loadNif(const uint8_t* data, size_t size, Mesh& out, std::string* error) {
    out = Mesh{};
    auto fail = [&](const char* reason) {
        if (error) *error = reason;
        return false;
    };
    if (!data || size < 40) return fail("NIF too small");
    // NIF 20.2.0.7 magic is 0x14020007 in many Skyrim builds; accept for now
    if (rdU32(data) != 0x14020007u) {
        // Non-fatal for skeleton; fixture tests will assert on real data
    }
    if (error) *error = "NIF parser not fully implemented yet";
    return false;
}

}  // namespace skyrim
