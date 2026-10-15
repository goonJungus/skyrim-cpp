// land.cpp - decode LAND records into heightmaps (see include/skyrim/land.hpp).
//
// VHGT layout (verified against a real Skyrim.esm):
//   float  anchor          height of the south-west corner, in 8-unit steps
//   int8   deltas[1089]    difference-encoded heights, row-major (south->north,
//                          then west->east within each row)
//   uint8   padding[3]
// Total 1096 bytes. A stored unit is 8 game units.
//
// The anchor is a float and the deltas are signed bytes so the encoding stays
// compact over a 4096-unit cell. Zero deltas are legal and common over flats.
//
// Texturing: BTXT names the base land texture (LTEX) for the whole cell, and
// each ATXT names a texture for one quadrant at a layer index. Cells that carry
// no BTXT (common in the wild) still describe every quadrant through ATXT.

#include "skyrim/land.hpp"

#include <cstring>

namespace skyrim {
namespace {

constexpr uint32_t kSubHeaderSize = 6;

uint32_t rdU16(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
}

uint32_t rdU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

float rdF32(const uint8_t* p) {
    uint32_t bits = rdU32(p);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

bool sameType(const uint8_t* p, char a, char b, char c, char d) {
    return p[0] == static_cast<uint8_t>(a) && p[1] == static_cast<uint8_t>(b) &&
           p[2] == static_cast<uint8_t>(c) && p[3] == static_cast<uint8_t>(d);
}

}  // namespace

void Land::corners(float* out) const {
    // Only the four corner sums are needed, and the encoding chains rows through
    // column 0, so each corner is a short sum over the deltas instead of the full
    // reconstruction heights() performs. See heights() for the layout.
    const float base = anchor * kLandHeightScale;
    float south = 0.0f;   // row 0, columns 0..32
    for (int col = 0; col < kLandEdge; ++col) {
        south += static_cast<float>(deltas[static_cast<size_t>(col)]);
    }
    float west = 0.0f;    // column 0, rows 0..32
    for (int row = 0; row < kLandEdge; ++row) {
        west += static_cast<float>(deltas[static_cast<size_t>(row) * kLandEdge]);
    }
    float north = 0.0f;   // row 32, columns 1..32
    const size_t lastRow = static_cast<size_t>(kLandEdge - 1) * kLandEdge;
    for (int col = 1; col < kLandEdge; ++col) {
        north += static_cast<float>(deltas[lastRow + static_cast<size_t>(col)]);
    }

    const float scale = kLandHeightScale;
    out[0] = base + static_cast<float>(deltas[0]) * scale;   // south-west
    out[1] = base + south * scale;                           // south-east
    out[2] = base + west * scale;                            // north-west
    out[3] = base + (west + north) * scale;                  // north-east
}

void Land::heights(float* out) const {
    // Reconstruct the running row-major height grid. Column 0 of each row
    // accumulates into the shared anchor (so rows chain vertically); the rest
    // of the row accumulates into a per-row offset.
    float anchorUnits = anchor * kLandHeightScale;
    for (int row = 0; row < kLandEdge; ++row) {
        float rowOffset = 0.0f;
        for (int col = 0; col < kLandEdge; ++col) {
            const int i = row * kLandEdge + col;
            const float v = static_cast<float>(deltas[static_cast<size_t>(i)]) * kLandHeightScale;
            if (col == 0) {
                anchorUnits += v;
                rowOffset = 0.0f;
                out[i] = anchorUnits;
            } else {
                rowOffset += v;
                out[i] = anchorUnits + rowOffset;
            }
        }
    }
}

bool decodeLand(const uint8_t* data, size_t size, Land& out) {
    if (data == nullptr || size == 0) return false;

    bool haveVhgt = false;
    out.baseTexture = kFormIDNull;
    out.quadrantTexture = {kFormIDNull, kFormIDNull, kFormIDNull, kFormIDNull};
    out.layers.clear();
    int quadrantLayer[4] = {1 << 30, 1 << 30, 1 << 30, 1 << 30};

    uint32_t rem = static_cast<uint32_t>(size);
    const uint8_t* p = data;
    while (rem >= kSubHeaderSize) {
        const uint32_t s = rdU16(p + 4);
        const uint8_t* body = p + kSubHeaderSize;
        p += kSubHeaderSize;
        rem -= kSubHeaderSize;
        if (rem < s) break;

        if (sameType(p - kSubHeaderSize, 'V', 'H', 'G', 'T')) {
            // VHGT: float anchor + 1089 signed bytes (+3 padding).
            if (s >= 4 + static_cast<uint32_t>(kLandVertices)) {
                out.anchor = rdF32(body);
                std::memcpy(out.deltas.data(), body + 4, kLandVertices);
                haveVhgt = true;
            }
        } else if (sameType(p - kSubHeaderSize, 'B', 'T', 'X', 'T')) {
            if (s >= 4) out.baseTexture = rdU32(body);
        } else if (sameType(p - kSubHeaderSize, 'A', 'T', 'X', 'T')) {
            // ATXT: u32 LTEX FormID, u8 quadrant, u8 unknown, u16 layer.
            if (s >= 8) {
                const uint32_t q = body[4];
                const uint32_t layer = rdU16(body + 6);
                if (q < 4) {
                    LandLayer l;
                    l.texture = rdU32(body);
                    l.quadrant = static_cast<uint8_t>(q);
                    l.layer = static_cast<uint16_t>(layer);
                    out.layers.push_back(std::move(l));
                    if (layer < static_cast<uint32_t>(quadrantLayer[q])) {
                        quadrantLayer[q] = static_cast<int>(layer);
                        out.quadrantTexture[q] = rdU32(body);
                    }
                }
            }
        } else if (sameType(p - kSubHeaderSize, 'V', 'T', 'X', 'T')) {
            // VTXT: sparse opacities for the ATXT just above: u16 index + f32
            // weight, 6 bytes per entry. Attach to the newest ATXT layer.
            if (!out.layers.empty() && s % 6 == 0) {
                LandLayer& l = out.layers.back();
                const uint32_t n = s / 6;
                l.weights.reserve(l.weights.size() + n);
                for (uint32_t i = 0; i < n; i++) {
                    const uint16_t idx = static_cast<uint16_t>(rdU16(body + static_cast<size_t>(i) * 6));
                    const float op = rdF32(body + static_cast<size_t>(i) * 6 + 2);
                    if (idx >= kLandVertices) continue;
                    if (!(op > 0.0f)) continue;   // drop empty samples, ours and NaN
                    LandVertexWeight w;
                    w.index = idx;
                    w.opacity = op > 1.0f ? 1.0f : op;
                    l.weights.push_back(w);
                }
            }
        }

        p += s;
        rem -= s;
    }

    if (!haveVhgt) return false;

    // Resolve per-quadrant textures: BTXT is the baseline everywhere, then any
    // quadrant with no layer of its own borrows an existing one, so no quadrant
    // is left untextured when the cell has any texture at all.
    FormID any = kFormIDNull;
    for (int q = 0; q < 4; ++q) {
        if (out.quadrantTexture[q] == kFormIDNull) out.quadrantTexture[q] = out.baseTexture;
        if (out.quadrantTexture[q] != kFormIDNull && any == kFormIDNull) {
            any = out.quadrantTexture[q];
        }
    }
    if (any != kFormIDNull) {
        for (int q = 0; q < 4; ++q) {
            if (out.quadrantTexture[q] == kFormIDNull) out.quadrantTexture[q] = any;
        }
    }

    out.present = true;
    return true;
}

}  // namespace skyrim
