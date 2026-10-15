// land_test.cpp - verifies LAND heightmap decoding without any fixture.
//
// The LAND payload is built in memory so the test never needs a game install:
// a VHGT subrecord with a known anchor and a handful of non-zero deltas, plus a
// BTXT base-texture link. It checks the difference-encoded reconstruction
// (including the column-0 row chaining), the texture link, rejection of
// malformed records, and terrain-mesh generation.
//
// Usage: land_test

#include "skyrim/land.hpp"

#include "skyrim/mesh.hpp"
#include "skyrim/world_render.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace skyrim;

namespace {

int failures = 0;

void expect(bool cond, const std::string& what) {
    if (!cond) { std::printf("    FAIL: %s\n", what.c_str()); failures++; }
}

void expectNear(float got, float want, const std::string& what) {
    const float d = got - want;
    if (d > 1e-3f || d < -1e-3f) {
        std::printf("    FAIL: %s (got %.3f, want %.3f)\n", what.c_str(), got, want);
        failures++;
    }
}

void putU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xff));
}

void putF32(std::vector<uint8_t>& v, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    putU32(v, bits);
}

void putSub(std::vector<uint8_t>& payload, const char* type,
            const std::vector<uint8_t>& data) {
    payload.insert(payload.end(), type, type + 4);
    const uint16_t sz = static_cast<uint16_t>(data.size());
    payload.push_back(static_cast<uint8_t>(sz & 0xff));
    payload.push_back(static_cast<uint8_t>((sz >> 8) & 0xff));
    payload.insert(payload.end(), data.begin(), data.end());
}

// A VHGT payload: anchor + 1089 deltas + 3 padding bytes.
std::vector<uint8_t> makeVhgt(float anchor, const std::vector<int>& deltaPatches) {
    std::vector<uint8_t> v;
    putF32(v, anchor);
    std::vector<int8_t> deltas(kLandVertices, 0);
    for (size_t i = 0; i + 1 < deltaPatches.size(); i += 2) {
        deltas[static_cast<size_t>(deltaPatches[i])] = static_cast<int8_t>(deltaPatches[i + 1]);
    }
    for (int8_t d : deltas) v.push_back(static_cast<uint8_t>(d));
    v.push_back(0);
    v.push_back(0);
    v.push_back(0);
    return v;
}

}  // namespace

int main() {
    // --- reconstruction -----------------------------------------------------
    // anchor 100 => 800 game units. deltas are worth 8 units each.
    //   index 1  (row 0, col 1) = +3  => row 0 climbs 24 units after col 0.
    //   index 33 (row 1, col 0) = +5  => the anchor steps up 40 for row 1.
    //   index 66 (row 2, col 0) = -2  => rows below inherit the new anchor.
    std::vector<uint8_t> payload;
    putSub(payload, "VHGT", makeVhgt(100.0f, {1, 3, 33, 5, 66, -2}));
    std::vector<uint8_t> btxt;
    putU32(btxt, 0x00013428);   // LTEX FormID
    putU32(btxt, 0xffffa100);
    putSub(payload, "BTXT", btxt);

    Land land;
    expect(decodeLand(payload.data(), payload.size(), land), "decodeLand succeeds");
    expect(land.present, "land marked present");
    expectNear(land.anchor, 100.0f, "anchor read");
    expect(land.baseTexture == 0x00013428, "BTXT base texture read");
    expect(land.quadrantTexture[0] == 0x00013428 &&
           land.quadrantTexture[1] == 0x00013428 &&
           land.quadrantTexture[2] == 0x00013428 &&
           land.quadrantTexture[3] == 0x00013428,
           "BTXT fills every quadrant");

    float h[kLandVertices];
    land.heights(h);
    expectNear(h[0], 800.0f, "vertex (0,0) = anchor * 8");
    expectNear(h[1], 824.0f, "vertex (0,1) picks up the +3 delta");
    expectNear(h[32], 824.0f, "row 0 stays level after the one delta");
    expectNear(h[33], 840.0f, "row 1 col 0 steps the anchor by +5 * 8");
    expectNear(h[34], 840.0f, "row 1 stays level after the anchor step");
    expectNear(h[66], 824.0f, "row 2 col 0 = 840 - 2*8");
    expectNear(h[67], 824.0f, "row 2 stays level");

    // --- ATXT-only cell (no BTXT): each quadrant takes its lowest layer -----
    std::vector<uint8_t> atxtPayload;
    putSub(atxtPayload, "VHGT", makeVhgt(0.0f, {}));
    auto atxt = [](uint32_t ltex, uint8_t quadrant, uint16_t layer) {
        std::vector<uint8_t> v;
        putU32(v, ltex);
        v.push_back(quadrant);
        v.push_back(0xa4);
        v.push_back(static_cast<uint8_t>(layer & 0xff));
        v.push_back(static_cast<uint8_t>(layer >> 8));
        return v;
    };
    putSub(atxtPayload, "ATXT", atxt(0x0000089b, 0, 1));   // layer 1: not the base
    putSub(atxtPayload, "ATXT", atxt(0x0000089b, 0, 0));   // layer 0: wins
    putSub(atxtPayload, "ATXT", atxt(0x0008b01e, 1, 0));
    putSub(atxtPayload, "ATXT", atxt(0x0008b01e, 2, 0));
    Land atxtLand;
    expect(decodeLand(atxtPayload.data(), atxtPayload.size(), atxtLand),
           "ATXT-only cell decodes");
    if (atxtLand.present) {
        expect(atxtLand.baseTexture == kFormIDNull, "no BTXT means no base texture");
        expect(atxtLand.quadrantTexture[0] == 0x0000089b, "quadrant 0 lowest ATXT layer");
        expect(atxtLand.quadrantTexture[1] == 0x0008b01e, "quadrant 1 ATXT layer");
        expect(atxtLand.quadrantTexture[2] == 0x0008b01e, "quadrant 2 ATXT layer");
        expect(atxtLand.quadrantTexture[3] != kFormIDNull, "untextured quadrant borrows one");
    }

    // --- malformed records --------------------------------------------------
    Land bad;
    expect(!decodeLand(nullptr, 0, bad), "null payload rejected");
    std::vector<uint8_t> noVhgt;
    putSub(noVhgt, "BTXT", btxt);
    expect(!decodeLand(noVhgt.data(), noVhgt.size(), bad), "missing VHGT rejected");
    std::vector<uint8_t> shortVhgt;
    putSub(shortVhgt, "VHGT", std::vector<uint8_t>(16, 0));
    expect(!decodeLand(shortVhgt.data(), shortVhgt.size(), bad), "undersized VHGT rejected");

    // --- terrain mesh -------------------------------------------------------
    Cell cell;
    cell.land = land;
    cell.gridX = 4;
    cell.gridY = -20;
    cell.worldMinX = 4.0f * kCellSize;
    cell.worldMinY = -20.0f * kCellSize;
    cell.worldMaxX = cell.worldMinX + kCellSize;
    cell.worldMaxY = cell.worldMinY + kCellSize;
    cell.isInterior = false;

    ModelResolver plain;   // not initialised: textures resolve to empty
    Mesh mesh;
    expect(buildTerrainMesh(cell, plain, mesh), "buildTerrainMesh succeeds");
    expect(mesh.valid(), "terrain mesh valid");
    if (mesh.valid()) {
        expect(mesh.vertices.size() == static_cast<size_t>(kLandVertices),
               "terrain vertex count is 33x33");
        expect(mesh.indices.size() == 32u * 32u * 6u, "terrain index count");
        expect(mesh.submeshes.size() == 1, "terrain has one submesh");
        expect(mesh.vertices[0].x == cell.worldMinX, "first vertex at cell west edge");
        expect(mesh.vertices[0].y == cell.worldMinY, "first vertex at cell south edge");
        expectNear(mesh.vertices[0].z, 800.0f, "first vertex height");
        expectNear(mesh.vertices[1].z, 824.0f, "second vertex height");
        // A vertex well away from the deltas sits on flat ground.
        const int flat = 10 * kLandEdge + 10;
        expectNear(mesh.vertices[flat].z, 824.0f, "flat vertex height");
        expectNear(mesh.vertices[flat].nz, 1.0f, "flat normal points up");
    }

    // A cell with no terrain must not produce a mesh.
    Cell empty;
    empty.isInterior = false;
    Mesh none;
    expect(!buildTerrainMesh(empty, plain, none), "cell without LAND yields no mesh");

    if (failures == 0) {
        std::printf("ALL LAND TESTS PASSED\n");
        return 0;
    }
    std::printf("%d LAND CHECK(S) FAILED\n", failures);
    return 1;
}
