// world_probe.cpp - end-to-end proof of the world->renderable pipeline on a
// real install: ESM cells -> REFR -> base MODL -> BSA lookup -> NIF -> mesh.
//
// It loads the cell system and form database, picks a busy exterior cell,
// resolves its placed models, loads each model's mesh from an archive, and
// reports how many resolve and how much geometry the cell amounts to.
//
// Usage:
//   world_probe <DataFolder> [--meshes <archive.bsa>] [--ws 0xHEX --grid X Y]
//
// Nothing is hardcoded; every path is a runtime argument.

#include "skyrim/bsa_archive.hpp"
#include "skyrim/cell_system.hpp"
#include "skyrim/form_database.hpp"
#include "skyrim/world_render.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace skyrim;

namespace {

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
        std::printf("usage: world_probe <DataFolder> [--meshes <archive.bsa>] "
                    "[--ws 0xHEX --grid X Y]\n");
        return 2;
    }
    const std::string dataDir = argv[1];
    std::string meshes;
    bool haveWs = false;
    FormID wantWs = 0;
    int wantX = 0, wantY = 0, haveGrid = 0;
    bool landStats = false;
    for (int i = 2; i < argc; i++) {
        const std::string k = argv[i];
        if (k == "--meshes" && i + 1 < argc) {
            meshes = argv[++i];
        } else if (k == "--ws" && i + 1 < argc) {
            wantWs = static_cast<FormID>(std::strtoul(argv[++i], nullptr, 16));
            haveWs = true;
        } else if (k == "--grid" && i + 2 < argc) {
            wantX = std::atoi(argv[++i]);
            wantY = std::atoi(argv[++i]);
            haveGrid = 2;
        } else if (k == "--landstats") {
            landStats = true;
        }
    }

    SkyrimConfig config;
    config.dataDirectory = dataDir;
    config.loadArchives = false;

    FormDatabase db;
    if (db.load(config) != Status::Ok) {
        std::printf("FAIL: form database load\n");
        return 1;
    }
    CellSystem cells;
    if (cells.load(config) != Status::Ok) {
        std::printf("FAIL: cell system load\n");
        return 1;
    }
    ModelResolver resolver;
    resolver.init(&db);

    std::printf("forms=%zu cells=%zu refs=%zu\n", db.formCount(), cells.cellCount(),
                cells.totalObjectCount());

    if (landStats) {
        size_t withLand = 0, withLayers = 0, withWeights = 0;
        size_t totalLayers = 0, maxLayers = 0;
        size_t totalWeights = 0, maxWeights = 0;
        size_t layerHist[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        size_t bytesWeights = 0;
        size_t minPos = 0xFFFF, maxPos = 0, posOverQuadrant = 0;
        size_t maxLayerWeights = 0;
        for (const Cell& c : cells.cells()) {
            if (c.isInterior || !c.land.present) continue;
            ++withLand;
            const size_t nl = c.land.layers.size();
            if (nl > 0) ++withLayers;
            layerHist[nl < 8 ? nl : 8]++;
            totalLayers += nl;
            if (nl > maxLayers) maxLayers = nl;
            size_t w = 0;
            for (const LandLayer& l : c.land.layers) {
                w += l.weights.size();
                if (l.weights.size() > maxLayerWeights) maxLayerWeights = l.weights.size();
                for (const LandVertexWeight& vw : l.weights) {
                    if (vw.index < minPos) minPos = vw.index;
                    if (vw.index > maxPos) maxPos = vw.index;
                    if (vw.index >= 289) ++posOverQuadrant;
                }
            }
            if (w > 0) ++withWeights;
            totalWeights += w;
            if (w > maxWeights) maxWeights = w;
            bytesWeights += w * sizeof(LandVertexWeight);
        }
        std::printf("land: weight vertex index min=%zu max=%zu  entries>=289: %zu  max per layer=%zu\n",
                    minPos, maxPos, posOverQuadrant, maxLayerWeights);
        std::printf("land: %zu cells with terrain, %zu carry layers, %zu carry weights\n",
                    withLand, withLayers, withWeights);
        std::printf("land: layers total=%zu max=%zu  weights total=%zu max=%zu (%.1f MB)\n",
                    totalLayers, maxLayers, totalWeights, maxWeights,
                    static_cast<double>(bytesWeights) / (1024.0 * 1024.0));
        std::printf("land: layers-per-cell histogram:");
        for (size_t i = 0; i < 9; i++) {
            if (layerHist[i] != 0) std::printf(" %zu:%zu", i, layerHist[i]);
        }
        std::printf("\n");
        if (meshes.empty()) return 0;
    }

    // Choose the cell to inspect.
    const Cell* cell = nullptr;
    if (haveWs && haveGrid == 2) {
        cell = cells.findCell(wantWs, wantX, wantY);
    } else {
        const auto hist = cells.worldspaceHistogram();
        if (!hist.empty()) {
            size_t best = 0;
            for (const auto& c : cells.cells()) {
                if (c.isInterior || c.isPersistent || c.worldspace != hist[0].first) continue;
                const size_t n = inBoundsCount(c);
                if (n > best) { best = n; cell = &c; }
            }
        }
    }
    if (cell == nullptr) {
        std::printf("no cell selected\n");
        return 1;
    }
    std::printf("cell 0x%08X (%d,%d) edid=%s refs=%zu\n", cell->worldspace,
                cell->gridX, cell->gridY,
                cell->editorId.empty() ? "(none)" : cell->editorId.c_str(),
                cell->objects.size());

    // Resolve placed models.
    const std::vector<PlacedModel> instances = instancesForCell(*cell, resolver);
    std::printf("placed models: %zu of %zu references resolved to a MODL path\n",
                instances.size(), cell->objects.size());

    // Bucket by model path to show what the cell is built from.
    std::vector<std::pair<std::string, size_t>> distinct;
    for (const PlacedModel& m : instances) {
        bool found = false;
        for (auto& d : distinct) {
            if (d.first == m.modelPath) { d.second++; found = true; break; }
        }
        if (!found) distinct.emplace_back(m.modelPath, 1);
    }
    std::printf("distinct models: %zu\n", distinct.size());
    std::sort(distinct.begin(), distinct.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < distinct.size() && i < 10; i++) {
        std::printf("  x%-4zu %s\n", distinct[i].second, distinct[i].first.c_str());
    }

    if (meshes.empty()) {
        std::printf("(no --meshes archive given; skipping mesh load)\n");
        return 0;
    }

    // Load meshes from the archive.
    BsaArchive bsa;
    if (!bsa.open(meshes)) {
        std::printf("FAIL: open archive %s\n", meshes.c_str());
        return 1;
    }
    MeshCache cache;
    cache.addArchive(&bsa);

    size_t loaded = 0, missing = 0;
    uint64_t verts = 0, tris = 0;
    for (const PlacedModel& m : instances) {
        const Mesh* mesh = cache.get(m.modelPath);
        if (mesh == nullptr) { missing++; continue; }
        loaded++;
        verts += mesh->vertices.size();
        tris += mesh->indices.size() / 3;
    }
    std::printf("meshes: %zu loaded, %zu missing; %llu verts, %llu tris "
                "(cache hit=%zu miss=%zu)\n",
                loaded, missing,
                static_cast<unsigned long long>(verts),
                static_cast<unsigned long long>(tris),
                cache.hitCount(), cache.missCount());
    return 0;
}
