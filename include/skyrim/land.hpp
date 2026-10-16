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
#include <vector>

#include "skyrim/form_database.hpp"

namespace skyrim {

// 33x33 vertices, i.e. 32x32 quads across a 4096-unit cell (128 units apart).
constexpr int kLandEdge = 33;
constexpr int kLandVertices = kLandEdge * kLandEdge;   // 1089
constexpr float kLandVertexSpacing = 128.0f;           // 4096 / 32
constexpr float kLandHeightScale = 8.0f;               // stored unit -> game units

// The cell splits into four 16x16-quad quadrants, each its own 17x17 vertices.
// A quadrant shares its inner border with its neighbours, so the grid is 33 wide
// rather than 34.
constexpr int kLandQuadrantEdge = kLandEdge / 2 + 1;                 // 17
constexpr int kLandQuadrantVertices = kLandQuadrantEdge * kLandQuadrantEdge;   // 289
constexpr int kLandQuadrantOffset = kLandEdge - kLandQuadrantEdge;   // 16

// One vertex sample of a land-texture layer: which heightmap vertex it applies
// to and how strongly the layer shows there. VTXT stores these sparsely, so a
// layer only carries entries for the vertices it actually covers.
struct LandVertexWeight {
    uint16_t index = 0;      // 0 .. kLandVertices-1
    float opacity = 0.0f;    // 0 .. 1
};

// One land-texture layer of a cell: an ATXT entry plus the VTXT opacities that
// belong to it. Layers composite bottom-up within a quadrant, so layer 0 is the
// ground and higher layers blend on top of it.
struct LandLayer {
    FormID texture = kFormIDNull;   // LTEX the layer draws with
    uint8_t quadrant = 0;           // 0 bottom-left, 1 bottom-right, 2 upper-left, 3 upper-right
    uint16_t layer = 0;             // layer index inside the quadrant
    std::vector<LandVertexWeight> weights;   // sparse, VTXT
};

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
    // Every ATXT/VTXT layer, in the order the record stores them. Used to blend
    // the ground the way the game does instead of picking one texture per
    // quadrant. Empty for cells that only carry BTXT.
    std::vector<LandLayer> layers;

    // Reconstruct the absolute height of every vertex, in game units, into
    // `out` (kLandVertices floats, row-major: out[row * kLandEdge + col]).
    void heights(float* out) const;

    // Absolute heights of the four cell corners, in game units, in the order
    // south-west, south-east, north-west, north-east. A coarse mesh that samples
    // terrain at cell corners wants only these, and getting them this way costs
    // a hundredth of reconstructing the full grid.
    void corners(float* out) const;
};

// Parse a LAND record's already-decompressed payload. Returns true when a
// usable VHGT heightmap was found (the record is left untouched otherwise).
bool decodeLand(const uint8_t* data, size_t size, Land& out);

}  // namespace skyrim
