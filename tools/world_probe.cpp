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

// Every land layer of one cell, with where its weighted vertices actually sit.
// Ground truth for the quadrant question that the aggregate counters above only
// hint at: for each layer, the quadrant its ATXT names and a count of weighted
// vertices per cell quadrant (row >= 16 is the upper half, col >= 16 the right).
void dumpLandLayers(const Cell& c) {
    std::printf("land dump: cell (%d %d), %zu layers, base texture %06x\n", c.gridX, c.gridY,
                c.land.layers.size(), c.land.baseTexture);
    for (size_t i = 0; i < c.land.layers.size(); i++) {
        const LandLayer& l = c.land.layers[i];
        size_t perQuad[4] = {0, 0, 0, 0};
        float maxOpacity = 0.0f, sumOpacity = 0.0f;
        for (const LandVertexWeight& w : l.weights) {
            const int row = static_cast<int>(w.index) / kLandEdge;
            const int col = static_cast<int>(w.index) % kLandEdge;
            if (row >= 0 && row < kLandEdge && col >= 0 && col < kLandEdge) {
                ++perQuad[(row >= 16 ? 2 : 0) + (col >= 16 ? 1 : 0)];
            }
            sumOpacity += w.opacity;
            if (w.opacity > maxOpacity) maxOpacity = w.opacity;
        }
        std::printf("  %2zu q=%u layer=%2u tex=%06x weights=%4zu perQuad=%zu/%zu/%zu/%zu "
                    "opacity max=%.2f mean=%.2f\n",
                    i, static_cast<unsigned>(l.quadrant), static_cast<unsigned>(l.layer), l.texture,
                    l.weights.size(), perQuad[0], perQuad[1], perQuad[2], perQuad[3], maxOpacity,
                    l.weights.empty() ? 0.0f : sumOpacity / static_cast<float>(l.weights.size()));
        if (l.weights.size() > 0 && l.weights.size() <= 24) {
            std::printf("      indices:");
            for (const LandVertexWeight& w : l.weights) {
                std::printf(" %u(%.2f)", static_cast<unsigned>(w.index), w.opacity);
            }
            std::printf("\n");
        }
    }
}

// Weighted vertices of terrain blend layers sit along the border between two
// surfaces, so they arrive in runs that touch their neighbours. Read the VTXT
// index as cell-wide (0..1088) or as quadrant-local (0..288, ATXT's quadrant
// giving the half) and count how often two weighted vertices end up adjacent on
// the 33x33 grid: the right reading produces long coherent patches, the wrong
// one scatters them at random, so its neighbour count collapses.
void dumpLayerCoherence(const CellSystem& cells, FormID ws, int gx, int gy, int span) {
    size_t weights = 0, pairsRaw = 0, pairsLocal = 0, overLocal = 0;
    for (const Cell& c : cells.cells()) {
        if (c.isInterior || !c.land.present || c.worldspace != ws) continue;
        if (c.gridX < gx || c.gridX >= gx + span || c.gridY < gy || c.gridY >= gy + span) continue;
        for (const LandLayer& l : c.land.layers) {
            if (l.quadrant >= 4 || l.weights.empty()) continue;
            const int row0 = (l.quadrant & 2) != 0 ? 16 : 0;
            const int col0 = (l.quadrant & 1) != 0 ? 16 : 0;
            std::vector<uint8_t> raw(kLandVertices, 0), local(kLandVertices, 0);
            for (const LandVertexWeight& w : l.weights) {
                ++weights;
                if (w.index < kLandVertices) raw[w.index] = 1;
                if (w.index >= 289) ++overLocal;
                if (w.index < 289) {
                    const int row = row0 + w.index / 17;
                    const int col = col0 + w.index % 17;
                    local[static_cast<size_t>(row * kLandEdge + col)] = 1;
                }
            }
            for (size_t i = 0; i < kLandVertices; i++) {
                const int row = static_cast<int>(i) / kLandEdge;
                const int col = static_cast<int>(i) % kLandEdge;
                if (row + 1 < kLandEdge && raw[static_cast<size_t>(i)] && raw[static_cast<size_t>(i + kLandEdge)]) ++pairsRaw;
                if (row + 1 < kLandEdge && local[static_cast<size_t>(i)] && local[static_cast<size_t>(i + kLandEdge)]) ++pairsLocal;
                if (col + 1 < kLandEdge && raw[static_cast<size_t>(i)] && raw[static_cast<size_t>(i + 1)]) ++pairsRaw;
                if (col + 1 < kLandEdge && local[static_cast<size_t>(i)] && local[static_cast<size_t>(i + 1)]) ++pairsLocal;
            }
        }
    }
    std::printf("layer coherence: %zu weights, %zu above 288, neighbour pairs cell-wide %zu, "
                "quadrant-local %zu\n",
                weights, overLocal, pairsRaw, pairsLocal);
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
        // Do a layer's weighted vertices actually sit in the quadrant its ATXT
        // names? Checked under both readings of the quadrant bits, because a
        // layer drawn outside its own quadrant covers nothing.
        size_t layersA = 0, layersB = 0, weightsA = 0, weightsB = 0, weightsTotal = 0;
        size_t indexBucket[4] = {0, 0, 0, 0};
        size_t quadOfLayer[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        size_t weightOfLayer[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
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
                if (l.quadrant >= 4) continue;
                // A: bit 1 is the row half, bit 0 the column half. B: the reverse.
                const int rowA = (l.quadrant & 2) != 0 ? 16 : 0;
                const int colA = (l.quadrant & 1) != 0 ? 16 : 0;
                const int rowB = colA;
                const int colB = rowA;
                bool inA = false, inB = false;
                size_t perQuad[4] = {0, 0, 0, 0};
                for (const LandVertexWeight& vw : l.weights) {
                    const int row = static_cast<int>(vw.index) / kLandEdge;
                    const int col = static_cast<int>(vw.index) % kLandEdge;
                    ++perQuad[(row >= 16 ? 2 : 0) + (col >= 16 ? 1 : 0)];
                    if (row >= rowA && row < rowA + 16 && col >= colA && col < colA + 16) {
                        ++weightsA;
                        inA = true;
                    }
                    if (row >= rowB && row < rowB + 16 && col >= colB && col < colB + 16) {
                        ++weightsB;
                        inB = true;
                    }
                    ++weightsTotal;
                    // 17x17 = 289: a quadrant-local index cannot reach it, a
                    // cell-wide one spends three quarters of its entries above it.
                    ++indexBucket[vw.index < 289 ? 0 : (vw.index < 578 ? 1
                                              : (vw.index < 867 ? 2 : 3))];
                    if (vw.index < minPos) minPos = vw.index;
                    if (vw.index > maxPos) maxPos = vw.index;
                    if (vw.index >= 289) ++posOverQuadrant;
                }
                if (inA) ++layersA;
                if (inB) ++layersB;
                size_t best = 0;
                for (int q = 1; q < 4; q++) {
                    if (perQuad[q] > perQuad[best]) best = static_cast<size_t>(q);
                }
                ++quadOfLayer[l.quadrant][best];
                weightOfLayer[l.quadrant][best] += perQuad[best];
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
        std::printf("land: weights inside the named quadrant: reading A %zu/%zu in %zu layers, "
                    "reading B %zu/%zu in %zu layers\n",
                    weightsA, weightsTotal, layersA, weightsB, weightsTotal, layersB);
        std::printf("land: weight index buckets (<289 / <578 / <867 / rest): %zu %zu %zu %zu\n",
                    indexBucket[0], indexBucket[1], indexBucket[2], indexBucket[3]);
        std::printf("land: declared quadrant (row) against the quadrant holding most of a "
                    "layer's weights (col), in layers / in weights:\n        ");
        for (size_t d = 0; d < 4; d++) {
            std::printf("  declared %zu:", d);
            for (size_t b = 0; b < 4; b++) {
                std::printf(" %zu/%zu", quadOfLayer[d][b], weightOfLayer[d][b]);
            }
            std::printf("\n        ");
        }
        std::printf("\n");
        std::printf("land: layers-per-cell histogram:");
        for (size_t i = 0; i < 9; i++) {
            if (layerHist[i] != 0) std::printf(" %zu:%zu", i, layerHist[i]);
        }
        std::printf("\n");
        if (haveWs && haveGrid == 2) {
            const Cell* sampled = cells.findCell(wantWs, wantX, wantY);
            if (sampled != nullptr && sampled->land.present) dumpLandLayers(*sampled);
            dumpLayerCoherence(cells, wantWs, wantX, wantY, 24);
        }
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
    if (landStats && cell->land.present) dumpLandLayers(*cell);

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
