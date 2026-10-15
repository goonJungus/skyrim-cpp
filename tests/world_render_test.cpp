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

const char* kHouseModel = "Architecture\\Fixture\\house01.nif";
const char* kBarrelModel = "Clutter\\Fixture\\barrel01.nif";

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

    if (failures == 0) {
        std::printf("ALL WORLD-RENDER TESTS PASSED\n");
        return 0;
    }
    std::printf("%d WORLD-RENDER CHECK(S) FAILED\n", failures);
    return 1;
}
