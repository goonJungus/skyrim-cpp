// cell_demo.cpp - load real Skyrim ESM masters and report cell/object data.
//
// This is the end-to-end proof that the world data pipeline works:
//   ESM file -> CellSystem walk -> cells with grid coords and object lists
//
// Usage:
//   cell_demo <DataFolder> [editorIdSubstring]
//
// Reports:
//   - total cells (interior + exterior), worldspaces, object references
//   - the worldspaces by exterior cell count
//   - the densest exterior cell of the largest worldspace, with a spatial
//     query and grid lookup around it
//   - cells matching an optional editor-id substring, with their grid coords
//
// The data folder is supplied at runtime; nothing here hardcodes a path.

#include "skyrim/cell_system.hpp"
#include "skyrim/string_table.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

using namespace skyrim;

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return s;
}

// How many of a cell's references actually fall inside its own 4096-unit
// square. The worldspace "persistent cell" (always at grid 0,0) holds every
// always-loaded reference regardless of position, so its in-bounds count is
// near zero -- this metric quietly skips it when picking a busy cell.
size_t inBoundsCount(const Cell& cell) {
    size_t n = 0;
    for (const auto& obj : cell.objects) {
        if (obj.x >= cell.worldMinX && obj.x < cell.worldMaxX &&
            obj.y >= cell.worldMinY && obj.y < cell.worldMaxY) {
            ++n;
        }
    }
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: cell_demo <DataFolder> [editorIdSubstring]\n");
        return 2;
    }
    const std::string dataDir = argv[1];
    const std::string needle = argc >= 3 ? toLower(argv[2]) : std::string();

    SkyrimConfig config;
    config.dataDirectory = dataDir;
    config.loadArchives = false;  // cells come from the ESM, not the BSA

    CellSystem cells;
    const Status st = cells.load(config);
    if (st != Status::Ok) {
        std::printf("FAIL: CellSystem::load returned %s\n", toString(st));
        return 1;
    }

    std::printf("=== CellSystem loaded ===\n");
    std::printf("  total cells:        %zu\n", cells.cellCount());
    std::printf("  exterior cells:     %zu\n", cells.exteriorCellCount());
    std::printf("  persistent cells:   %zu\n", cells.persistentCellCount());
    std::printf("  worldspaces:        %zu\n", cells.worldspaceCount());
    std::printf("  total references:   %zu\n", cells.totalObjectCount());
    std::printf("\n");

    // Largest worldspaces by exterior cell count.
    const auto hist = cells.worldspaceHistogram();
    std::printf("=== worldspaces by exterior cell count ===\n");
    for (size_t i = 0; i < hist.size() && i < 8; ++i) {
        std::printf("  worldspace 0x%08X  exterior cells=%zu\n",
                    hist[i].first, hist[i].second);
    }
    std::printf("\n");

    if (!hist.empty()) {
        const FormID ws = hist[0].first;

        // Densest real exterior cell in that worldspace (by in-bounds refs).
        // Persistent pseudo-cells are skipped: their references are scattered
        // across the whole worldspace, not placed in their own grid square.
        const Cell* hub = nullptr;
        size_t hubCount = 0;
        for (const auto& cell : cells.cells()) {
            if (cell.isInterior || cell.isPersistent || cell.worldspace != ws) continue;
            const size_t n = inBoundsCount(cell);
            if (n > hubCount) { hubCount = n; hub = &cell; }
        }

        if (hub) {
            std::printf("=== densest exterior cell of 0x%08X ===\n", ws);
            std::printf("  cell (%d, %d)  refs=%zu  in-bounds=%zu  edid=%s\n",
                        hub->gridX, hub->gridY, hub->objects.size(), hubCount,
                        hub->editorId.empty() ? "(none)" : hub->editorId.c_str());
            std::printf("  world bounds  [%.0f,%.0f]..[%.0f,%.0f]  center (%.0f,%.0f)\n",
                        hub->worldMinX, hub->worldMinY, hub->worldMaxX, hub->worldMaxY,
                        hub->centerX, hub->centerY);
            std::printf("  first references:\n");
            size_t n = 0;
            for (const auto& obj : hub->objects) {
                std::printf("    %s edid=%-28s base=0x%08x pos=(%.1f, %.1f, %.1f)\n",
                            obj.isActor ? "ACHR" : "REFR",
                            obj.editorId.empty() ? "(none)" : obj.editorId.c_str(),
                            obj.baseObject, obj.x, obj.y, obj.z);
                if (++n >= 6) break;
            }
            std::printf("\n");

            // Spatial query: the 7x7 block of cells centred on the hub.
            const float cx = hub->centerX, cy = hub->centerY;
            const auto near = cells.queryBounds(ws,
                    cx - 3.0f * kCellSize, cy - 3.0f * kCellSize,
                    cx + 3.0f * kCellSize, cy + 3.0f * kCellSize);
            size_t refs = 0;
            for (const auto* c : near) refs += c->objects.size();
            std::printf("=== spatial query: 7x7 block centred on hub ===\n");
            std::printf("  cells found: %zu, references: %zu\n", near.size(), refs);
            std::printf("\n");

            // Exact lookup by (worldspace, grid).
            std::printf("=== lookup: cell 0x%08X (%d, %d) ===\n", ws,
                        hub->gridX, hub->gridY);
            const Cell* c = cells.findCell(ws, hub->gridX, hub->gridY);
            std::printf("  %s (refs=%zu)\n", c ? "found" : "not found",
                        c ? c->objects.size() : 0);

            // Same grid coordinate in a *different* worldspace must be a
            // different cell (proves coordinates are worldspace-scoped).
            for (const auto& p : hist) {
                if (p.first == ws) continue;
                const Cell* other = cells.findCell(p.first, hub->gridX, hub->gridY);
                if (other) {
                    std::printf("  same grid in worldspace 0x%08X -> distinct cell "
                                "(refs=%zu)\n", p.first, other->objects.size());
                    break;
                }
            }
            std::printf("\n");
        }
    }

    // Optional editor-id search over every loaded cell (interior + exterior).
    if (!needle.empty()) {
        std::printf("=== cells matching \"%s\" ===\n", needle.c_str());
        size_t matches = 0, shown = 0;
        for (const auto& cell : cells.cells()) {
            if (toLower(cell.editorId).find(needle) == std::string::npos) continue;
            ++matches;
            if (shown < 25) {
                if (cell.isInterior) {
                    std::printf("  [interior]            edid=%s refs=%zu\n",
                                cell.editorId.c_str(), cell.objects.size());
                } else {
                    std::printf("  (0x%08X %5d,%5d)   edid=%s refs=%zu\n",
                                cell.worldspace, cell.gridX, cell.gridY,
                                cell.editorId.c_str(), cell.objects.size());
                }
                ++shown;
            }
        }
        std::printf("  total matches: %zu\n", matches);
    }

    return 0;
}
