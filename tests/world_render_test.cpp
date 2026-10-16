// world_render_test.cpp - verifies the cell -> base -> MODL -> model chain
// against a synthetic ESM (make_worldrender_fixture.py). The fixture carries
// STAT records with MODL paths, plus two references that must contribute no
// geometry: one whose base is missing and one whose base has no MODL.
//
// Usage: world_render_test <fixture-dir>

#include "skyrim/world_render.hpp"

#include "skyrim/cell_system.hpp"
#include "skyrim/form_database.hpp"

#include <cstdio>
#include <string>

using namespace skyrim;

namespace {

int failures = 0;

void expect(bool cond, const std::string& what) {
    if (!cond) { std::printf("    FAIL: %s\n", what.c_str()); failures++; }
}

void expectEq(size_t got, size_t want, const std::string& what) {
    if (got != want) {
        std::printf("    FAIL: %s (got %zu, want %zu)\n", what.c_str(), got, want);
        failures++;
    }
}

constexpr FormID kWs = 0x00000100;
constexpr FormID kStatHouse = 0x00001000;
constexpr FormID kStatBarrel = 0x00001001;
constexpr FormID kStatNoModel = 0x00001002;
constexpr FormID kMissing = 0x00009999;
constexpr FormID kFixtureLtex = 0x00002000;
constexpr FormID kFixtureLtexNoSet = 0x00002002;   // no TNAM: resolves to nothing

const char* kHouseModel = "Architecture\\Fixture\\house01.nif";
const char* kBarrelModel = "Clutter\\Fixture\\barrel01.nif";
const char* kFixtureDiffuse = "textures\\land\\fixture01.dds";

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: world_render_test <fixture-dir>\n");
        return 2;
    }

    SkyrimConfig config;
    config.dataDirectory = argv[1];
    config.loadArchives = false;

    FormDatabase db;
    if (db.load(config) != Status::Ok) {
        std::printf("FAILED TO LOAD FORM DATABASE\n");
        return 1;
    }
    CellSystem cells;
    if (cells.load(config) != Status::Ok) {
        std::printf("FAILED TO LOAD CELLS\n");
        return 1;
    }

    ModelResolver resolver;
    resolver.init(&db);
    expect(resolver.ready(), "resolver ready");

    // --- base object -> MODL path ------------------------------------------
    expect(resolver.modelPath(kStatHouse) == kHouseModel, "STAT house MODL path");
    expect(resolver.modelPath(kStatBarrel) == kBarrelModel, "STAT barrel MODL path");
    expect(resolver.modelPath(kStatNoModel).empty(), "base without MODL yields empty");
    expect(resolver.modelPath(kMissing).empty(), "missing base yields empty");
    expect(resolver.modelPath(kFormIDNull).empty(), "null FormID yields empty");

    // --- cell -> placed models ---------------------------------------------
    const Cell* cell = cells.findCell(kWs, 0, 0);
    expect(cell != nullptr, "cell (0,0) found");
    if (cell != nullptr) {
        const std::vector<PlacedModel> inst = instancesForCell(*cell, resolver);
        expectEq(inst.size(), 2, "renderable instances (modelless + missing skipped)");
        if (inst.size() == 2) {
            expect(inst[0].modelPath == kHouseModel, "instance 0 path");
            expect(inst[0].x == 100.0f && inst[0].y == 200.0f && inst[0].z == 0.0f,
                   "instance 0 position");
            expect(inst[0].baseObject == kStatHouse, "instance 0 base");
            expect(inst[1].modelPath == kBarrelModel, "instance 1 path");
            expect(inst[1].z == 10.0f, "instance 1 height");
        }
    }

    // --- mesh cache with no archives: every lookup misses, nothing crashes --
    MeshCache cache;
    expect(cache.get(kHouseModel) == nullptr, "cache miss without an archive");
    expectEq(cache.missCount(), 1, "miss counted");
    expectEq(cache.loadedCount(), 0, "nothing loaded");
    expect(cache.get("") == nullptr, "empty model path handled");
    cache.clear();
    expectEq(cache.missCount(), 0, "clear resets counters");

    // --- distant terrain ----------------------------------------------------
    // The ring takes its terrain through a callback, so it can be exercised
    // without any fixture: a flat plain of cells half a cell above zero. Only a
    // corner's height matters here, and a zeroed heightmap puts every height at
    // anchor * 8.
    const float kFlat = 16384.0f;   // anchor 2048 * 8
    auto flatLand = []() {
        Land land;
        land.present = true;
        land.anchor = 2048.0f;
        land.deltas.fill(0);
        return land;
    };
    std::vector<Land> plain;
    plain.reserve(64);
    for (int i = 0; i < 64; i++) plain.push_back(flatLand());

    auto landAt = [&plain](int gx, int gy) -> const Land* {
        if (gx < -4 || gx > 3 || gy < -4 || gy > 3) return nullptr;
        return &plain[static_cast<size_t>((gy + 4) * 8 + (gx + 4))];
    };
    int colorCalls = 0;
    auto colorOf = [&colorCalls](const std::string&, float& r, float& g, float& b) {
        colorCalls++;
        r = 0.5f;
        g = 0.25f;
        b = 1.0f;
        return true;
    };

    FarTerrainOptions far;
    far.radiusCells = 2;        // 5x5 corners covering cells -2..1
    far.holeRadiusCells = -1;    // no hole: the whole grid is built
    far.landAt = landAt;
    far.colorOf = colorOf;

    Mesh ring;
    expect(buildFarTerrain(0, 0, resolver, far, ring), "far ring builds");
    expectEq(ring.vertices.size(), 25, "far ring corners");
    expectEq(ring.indices.size(), 96, "far ring quads (4x4, no hole)");
    expectEq(ring.submeshes.size(), 1, "far ring is one submesh");
    expect(ring.submeshes.size() == 1 && ring.submeshes[0].texturePath.empty(),
           "far ring carries no texture");
    expect(ring.vertices.size() == 25 && ring.vertices[0].x == -8192.0f &&
               ring.vertices[0].y == -8192.0f && ring.vertices[0].z == kFlat,
           "far ring corner position and height");
    expect(ring.vertices.size() == 25 && ring.vertices[24].z == kFlat,
           "far ring far corner height");
    expect(ring.vertices.size() == 25 && ring.vertices[12].nx == 0.0f &&
               ring.vertices[12].ny == 0.0f && ring.vertices[12].nz == 1.0f,
           "far ring normal on flat ground");
    // The quadrant textures are unresolved, so the lookup is never reached and
    // the neutral ground tone comes through instead.
    expectEq(static_cast<size_t>(colorCalls), 0, "unresolved land textures skip the colour lookup");
    expect(ring.vertices.size() == 25 && ring.vertices[6].r > 0.3f &&
               ring.vertices[6].r < 0.45f && ring.vertices[6].b < ring.vertices[6].r,
           "far ring falls back to a neutral ground tone");

    // A one-cell hole drops the nine quads it covers, three per axis.
    far.holeRadiusCells = 1;
    Mesh holed;
    expect(buildFarTerrain(0, 0, resolver, far, holed), "far ring builds with a hole");
    expectEq(holed.indices.size(), 42, "far ring quads (4x4 minus a 3x3 hole)");

    far.holeRadiusCells = -1;

    // A gap in the middle is bridged by the cells around it, so the ring stays
    // whole. Only ground no cell backs at all is left out.
    auto gapAt = [&plain](int gx, int gy) -> const Land* {
        if (gx == -2 && gy == 0) return nullptr;
        if (gx < -4 || gx > 3 || gy < -4 || gy > 3) return nullptr;
        return &plain[static_cast<size_t>((gy + 4) * 8 + (gx + 4))];
    };
    far.landAt = gapAt;
    Mesh bridged;
    expect(buildFarTerrain(0, 0, resolver, far, bridged), "far ring builds around a gap");
    expectEq(bridged.indices.size(), 96, "far ring bridges a gap in the middle");

    // Terrain only to the west of the centre: the corners beyond it have no cell
    // left to come from, so their quads go with them instead of being invented.
    auto westAt = [&plain](int gx, int gy) -> const Land* {
        if (gx >= 0 || gx < -4 || gy < -4 || gy > 3) return nullptr;
        return &plain[static_cast<size_t>((gy + 4) * 8 + (gx + 4))];
    };
    far.landAt = westAt;
    Mesh partial;
    expect(buildFarTerrain(0, 0, resolver, far, partial),
           "far ring builds over a partial world");
    expect(!partial.indices.empty() && partial.indices.size() < 96,
           "far ring leaves out ground that no cell backs");

    // Nothing anywhere: nothing to build.
    far.landAt = [](int, int) -> const Land* { return nullptr; };
    Mesh emptyWorld;
    expect(!buildFarTerrain(0, 0, resolver, far, emptyWorld), "far ring refuses an empty world");

    // A hole that swallows the whole ring also leaves nothing to build.
    far.holeRadiusCells = 2;
    far.landAt = landAt;
    Mesh hollow;
    expect(!buildFarTerrain(0, 0, resolver, far, hollow),
           "far ring refuses to build when the hole covers it");

    // --- terrain blend layers ------------------------------------------------
    // Layers over two quadrants of a synthetic cell. A quadrant's lowest layer is
    // its ground, which the opaque base mesh already draws, so only the layers
    // above it come back here. VTXT is sparse, so an upper layer must be
    // transparent everywhere the record is silent -- and must not spend fill on
    // ground it does not cover.
    expect(resolver.texturePath(kFixtureLtex) == kFixtureDiffuse, "land texture path");
    expect(resolver.texturePath(kFixtureLtexNoSet).empty(), "land texture without a set");
    Cell landCell;
    landCell.isInterior = false;
    landCell.land = flatLand();

    LandLayer ground;
    ground.texture = kFixtureLtex;
    ground.quadrant = 0;
    ground.layer = 0;
    ground.weights.push_back(LandVertexWeight{0, 1.0f});
    landCell.land.layers.push_back(ground);

    LandLayer corner;
    corner.texture = kFixtureLtex;
    corner.quadrant = 0;
    corner.layer = 1;
    corner.weights.push_back(LandVertexWeight{0, 1.0f});
    corner.weights.push_back(LandVertexWeight{1, 1.0f});
    landCell.land.layers.push_back(corner);

    // The second quadrant starts at layer 1, so this patch is its ground and must
    // not come back as a blend layer.
    LandLayer patch;
    patch.texture = kFixtureLtex;
    patch.quadrant = 2;
    patch.layer = 1;
    for (int i = 0; i < 5; i++) {
        patch.weights.push_back(
            LandVertexWeight{static_cast<uint16_t>(17 * kLandEdge + i), 1.0f});
    }
    landCell.land.layers.push_back(patch);

    std::vector<Mesh> layers;
    expect(buildTerrainLayers(landCell, resolver, layers), "terrain layers build");
    expectEq(layers.size(), 1, "terrain layers (a quadrant's ground is not a blend layer)");
    if (layers.size() == 1) {
        // The corner layer touches two quads of a 16x16 quadrant -- one per weighted
        // vertex, plus the quad its neighbour shares -- not all 256.
        expectEq(layers[0].indices.size(), 12, "terrain layer emits only covered quads");
        expect(layers[0].vertices[0].a == 1.0f && layers[0].vertices[1].a == 1.0f &&
                   layers[0].vertices[2].a == 0.0f,
               "terrain layer is transparent where the record is silent");
        expect(layers[0].submeshes.size() == 1 &&
                   layers[0].submeshes[0].alphaBlend == 1 &&
                   layers[0].submeshes[0].texturePath == kFixtureDiffuse &&
                   layers[0].submeshes[0].indexCount == layers[0].indices.size(),
               "terrain layer submesh blends with the resolved texture");
    }

    // Give that quadrant a layer below the patch and it blends like any other:
    // the patch covers five consecutive vertices, which is three quads.
    LandLayer ground2 = ground;
    ground2.quadrant = 2;
    landCell.land.layers.push_back(ground2);
    expect(buildTerrainLayers(landCell, resolver, layers), "terrain layers build with a ground");
    expectEq(layers.size(), 2, "terrain layers (both quadrants blend)");
    if (layers.size() == 2) {
        expectEq(layers[1].indices.size(), 60,
               "terrain layer keeps every quad its vertices touch");
        // Composite order: quadrant 0 first, then quadrant 2.
        expect(layers[0].indices[0] < static_cast<uint32_t>(kLandEdge) * 16u + 16u &&
                   layers[1].indices[0] >= static_cast<uint32_t>(kLandEdge) * 16u,
               "terrain layers come back in composite order");
    }

    // A layer with no texture path is dropped rather than drawn untextured.
    landCell.land.layers.push_back(LandLayer());
    landCell.land.layers.back().texture = kFixtureLtexNoSet;
    landCell.land.layers.back().quadrant = 1;
    landCell.land.layers.back().layer = 1;
    landCell.land.layers.back().weights.push_back(LandVertexWeight{17, 1.0f});
    std::vector<Mesh> withDead;
    expect(buildTerrainLayers(landCell, resolver, withDead), "terrain layers build with a dead layer");
    expectEq(withDead.size(), 2, "terrain layer without a texture is dropped");

    // A cell with terrain but no layers has nothing to blend.
    Cell bare;
    bare.isInterior = false;
    bare.land = flatLand();
    std::vector<Mesh> none;
    expect(!buildTerrainLayers(bare, resolver, none), "no layers without any");
    expectEq(none.size(), 0, "no layer meshes without any");

    if (failures == 0) {
        std::printf("ALL WORLD-RENDER TESTS PASSED\n");
        return 0;
    }
    std::printf("%d WORLD-RENDER CHECK(S) FAILED\n", failures);
    return 1;
}
