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

    if (failures == 0) {
        std::printf("ALL WORLD-RENDER TESTS PASSED\n");
        return 0;
    }
    std::printf("%d WORLD-RENDER CHECK(S) FAILED\n", failures);
    return 1;
}
