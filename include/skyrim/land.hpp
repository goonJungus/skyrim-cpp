// land.hpp - exterior-cell terrain (LAND records).
//
// An exterior cell's ground is a heightmap stored in a LAND record that lives
// beside the cell's references. The heightmap is a 33x33 vertex grid covering
// the cell's 4096x4096 unit square, so vertices sit 128 units apart.
//
// The heights are difference-encoded: a 4-byte anchor float followed by 1089
// signed bytes. A running sum reconstructs them, and every stored unit is worth
// 8 game units. Row 0 is the south edge and column 0 is the west edge, both
// increasing north/east, which matches the world's +Y = north, +X = east axis.
//
// This header holds only decoded data and the reconstruction maths; reading a
// LAND record out of an ESM lives in land.cpp. Nothing here embeds content:
// the base texture is a FormID into the user's own plugin data.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "skyrim/form_database.hpp"

namespace skyrim {

// 33x33 vertices, i.e. 32x32 quads across a 4096-unit cell (128 units apart).
constexpr int kLandEdge = 33;
constexpr int kLandVertices = kLandEdge * kLandEdge;   // 1089
constexpr float kLandVertexSpacing = 128.0f;           // 4096 / 32
constexpr float kLandHeightScale = 8.0f;               // stored unit -> game units

// Decoded terrain for one exterior cell.
struct Land {
    bool     present = false;
    float    anchor = 0.0f;                            // SW corner height (game units)
    std::array<int8_t, kLandVertices> deltas{};        // difference-encoded heights
    FormID   baseTexture = kFormIDNull;                // BTXT land texture (LTEX), if any
    // Per-quadrant land texture: 0 = bottom-left, 1 = bottom-right, 2 = upper-
    // left, 3 = upper-right. Starts from BTXT and is overridden by the lowest
    // layer of each quadrant's ATXT entries. Null means "no texture known".
    std::array<FormID, 4> quadrantTexture{kFormIDNull, kFormIDNull, kFormIDNull, kFormIDNull};

    // Reconstruct the absolute height of every vertex, in game units, into
    // `out` (kLandVertices floats, row-major: out[row * kLandEdge + col]).
    void heights(float* out) const;
};

// Parse a LAND record's already-decompressed payload. Returns true when a
// usable VHGT heightmap was found (the record is left untouched otherwise).
bool decodeLand(const uint8_t* data, size_t size, Land& out);

}  // namespace skyrim
