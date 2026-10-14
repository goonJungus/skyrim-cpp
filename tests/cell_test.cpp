// cell_test.cpp - verifies CellSystem against a synthetic ESM fixture.
//
// The fixture (see make_cell_fixture.py) is our own synthetic data written to
// the documented TES4 container layout. It deliberately includes the cases that
// are easy to get wrong against real files:
//
//   * a compressed CELL whose body is u32 size + zlib stream;
//   * two worldspaces with a cell at the same grid coordinate;
//   * a worldspace persistent cell colliding at (0,0) with a real cell;
//   * interior cells with no grid coordinate;
//   * REFR and ACHR references with NAME and DATA.
//
// Usage: cell_test <fixture-dir>

#include "skyrim/cell_system.hpp"

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

constexpr FormID kWsA = 0x00000100;
constexpr FormID kWsB = 0x00000200;

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: cell_test <fixture-dir>\n");
        return 2;
    }

    SkyrimConfig config;
    config.dataDirectory = argv[1];
    config.loadArchives = false;

    CellSystem cells;
    const Status st = cells.load(config);
    expect(st == Status::Ok, "load returned not-Ok");
    if (st != Status::Ok) {
        std::printf("FAILED TO LOAD FIXTURE\n");
        return 1;
    }

    // 3 exterior cells of worldspace A (persistent 0x300 + 0x301 + 0x302),
    // 1 exterior of B (0x400), 1 interior (0x500).
    expectEq(cells.cellCount(), 5, "cellCount");
    expectEq(cells.exteriorCellCount(), 3, "exteriorCellCount (grid-indexed)");
    expectEq(cells.persistentCellCount(), 1, "persistentCellCount");
    expectEq(cells.worldspaceCount(), 2, "worldspaceCount");
    expectEq(cells.totalObjectCount(), 5, "totalObjectCount");

    // --- worldspace scoping -------------------------------------------------
    const Cell* a00 = cells.findCell(kWsA, 0, 0);
    const Cell* b00 = cells.findCell(kWsB, 0, 0);
    expect(a00 != nullptr, "worldspace A (0,0) missing");
    expect(b00 != nullptr, "worldspace B (0,0) missing");
    expect(a00 && a00->editorId == "WorldACell00",
           "worldspace A (0,0) should be the real cell, not the persistent one");
    expect(b00 && b00->editorId == "WorldBCell00", "worldspace B (0,0) wrong cell");
    expect(a00 != b00, "same coordinate in two worldspaces must be distinct cells");
    expect(a00 && !a00->isPersistent, "(0,0) real cell must not be persistent");

    // --- compressed CELL inflation -----------------------------------------
    // WorldAPersistent is compressed and is found by FormID (not grid).
    const Cell* pers = cells.findCellById(0x00000300);
    expect(pers != nullptr, "persistent cell not found by FormID");
    expect(pers && pers->editorId == "WorldAPersistent",
           "compressed cell EDID not inflated/parsed");
    expect(pers && pers->isPersistent, "persistent cell not flagged persistent");
    expect(pers && pers->objects.size() == 1, "persistent cell reference count");
    expect(pers && pers->objects.size() == 1 &&
           pers->objects[0].baseObject == 0x00000910,
           "persistent cell REFR base object");

    // --- compressed cell with references -----------------------------------
    const Cell* a01 = cells.findCell(kWsA, 0, 1);
    expect(a01 != nullptr, "worldspace A (0,1) missing");
    expect(a01 && a01->editorId == "WorldACell01",
           "compressed exterior cell EDID not parsed");
    expect(a01 && a01->objects.size() == 2, "worldspace A (0,1) reference count");
    if (a01 && a01->objects.size() == 2) {
        bool sawRefr = false, sawAchr = false;
        for (const auto& o : a01->objects) {
            if (!o.isActor && o.baseObject == 0x00000921 && o.x == 100.0f &&
                o.y == 4100.0f && o.z == 50.0f) {
                sawRefr = true;
            }
            if (o.isActor && o.baseObject == 0x00000922 && o.editorId == "FixtureAchr") {
                sawAchr = true;
            }
        }
        expect(sawRefr, "REFR base/position not parsed");
        expect(sawAchr, "ACHR not parsed as an actor reference");
    }

    // --- interior cell ------------------------------------------------------
    const Cell* interior = cells.findCellById(0x00000500);
    expect(interior != nullptr, "interior cell not found by FormID");
    expect(interior && interior->isInterior, "interior cell not flagged interior");
    expect(interior && interior->worldspace == kFormIDNull,
           "interior cell should have no worldspace");
    expect(interior && interior->objects.size() == 1, "interior cell reference count");
    expect(interior && interior->objects.size() == 1 &&
           interior->objects[0].baseObject == 0x00000930,
           "interior cell REFR base object");

    // --- spatial query ------------------------------------------------------
    const auto q = cells.queryBounds(kWsA, -1.0f, -1.0f, 1.0f, 8192.0f);
    // Cells (0,0) and (0,1) of worldspace A; persistent cell excluded.
    expectEq(q.size(), 2, "queryBounds count");

    // A query in worldspace B must not return A's cells.
    const auto qb = cells.queryBounds(kWsB, -1.0f, -1.0f, 4096.0f, 4096.0f);
    expectEq(qb.size(), 1, "queryBounds count in worldspace B");

    if (failures == 0) {
        std::printf("\nALL CELL TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CELL CHECK(S) FAILED\n", failures);
    return 1;
}
