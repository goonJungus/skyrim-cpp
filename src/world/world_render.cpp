// world_render.cpp - cell -> placed models -> cached meshes.

#include "skyrim/world_render.hpp"

#include "skyrim/bsa_archive.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace skyrim {
namespace {

constexpr RecordType kMODL = makeRecordType('M', 'O', 'D', 'L');

// Landscape textures are seamless and tile at a fixed world scale, so terrain
// UVs come from world position rather than from the cell. That keeps same-
// textured ground continuous across cell and quadrant boundaries. One tile per
// 2048 units is a tunable stand-in for the engine's own scale.
constexpr float kLandTextureWorldSize = 2048.0f;

uint32_t rdU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Copy a NUL-terminated (or fixed-length) path out of a subrecord.
std::string readString(const Subrecord& sr) {
    const char* p = reinterpret_cast<const char*>(sr.data);
    size_t n = 0;
    while (n < sr.size && p[n] != '\0') n++;
    return std::string(p, n);
}

bool hasPrefixIgnoreCase(const std::string& s, const char* prefix) {
    size_t n = 0;
    while (prefix[n] != '\0') n++;
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

// Shared terrain surface for one cell: the 33x33 vertex grid lifted by the
// LAND heightmap, with world-space UVs so a seamless land texture keeps tiling
// across cell and quadrant borders. Normals come from central differences of the
// height grid. Colours start white; a layer pass overwrites the alpha it needs.
void buildTerrainGrid(const Cell& cell, const float* h, std::vector<Vertex>& out) {
    out.clear();
    out.reserve(kLandVertices);
    for (int row = 0; row < kLandEdge; ++row) {
        for (int col = 0; col < kLandEdge; ++col) {
            const int i = row * kLandEdge + col;
            const int r0 = (row > 0) ? row - 1 : row;
            const int r1 = (row < kLandEdge - 1) ? row + 1 : row;
            const int c0 = (col > 0) ? col - 1 : col;
            const int c1 = (col < kLandEdge - 1) ? col + 1 : col;
            const float dzdx = (h[row * kLandEdge + c1] - h[row * kLandEdge + c0]) /
                               (static_cast<float>(c1 - c0) * kLandVertexSpacing);
            const float dzdy = (h[r1 * kLandEdge + col] - h[r0 * kLandEdge + col]) /
                               (static_cast<float>(r1 - r0) * kLandVertexSpacing);
            float nx = -dzdx, ny = -dzdy, nz = 1.0f;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }

            Vertex v;
            v.x = cell.worldMinX + static_cast<float>(col) * kLandVertexSpacing;
            v.y = cell.worldMinY + static_cast<float>(row) * kLandVertexSpacing;
            v.z = h[i];
            v.nx = nx;
            v.ny = ny;
            v.nz = nz;
            v.u = v.x / kLandTextureWorldSize;
            v.v = v.y / kLandTextureWorldSize;
            out.push_back(v);
        }
    }
}

// One 16x16 quadrant of the terrain grid, as a flat triangle list. Quadrant k
// covers bit 1 of k on the row axis and bit 0 on the column axis: 0 bottom-left,
// 1 bottom-right, 2 upper-left, 3 upper-right.
//
// A quad whose four corners are all fully transparent is left out. The base
// terrain is opaque throughout, and a blend layer that only covers part of its
// quadrant would otherwise spend its fill on ground it never touches -- and with
// layers sorted back to front that is fill the scene pays every frame.
void appendQuadrantIndices(std::vector<uint32_t>& idx, int quadrant,
                           const std::vector<Vertex>& v) {
    const int row0 = (quadrant & 2) != 0 ? 16 : 0;
    const int col0 = (quadrant & 1) != 0 ? 16 : 0;
    for (int row = row0; row < row0 + 16; ++row) {
        for (int col = col0; col < col0 + 16; ++col) {
            const uint32_t a = static_cast<uint32_t>(row * kLandEdge + col);
            const uint32_t b = a + 1;
            const uint32_t c = a + kLandEdge;
            const uint32_t d = c + 1;
            if (v[a].a <= 0.0f && v[b].a <= 0.0f && v[c].a <= 0.0f && v[d].a <= 0.0f) {
                continue;
            }
            idx.push_back(a);
            idx.push_back(c);
            idx.push_back(b);
            idx.push_back(b);
            idx.push_back(c);
            idx.push_back(d);
        }
    }
}

// One corner sample of the far grid: the ground colour of a quadrant of the cell
// that owns that corner, resolved through the caller's texture-colour lookup.
//
// A far grid asks about the same handful of land textures hundreds of times, and
// every miss walks the form database and decodes an image, so the answers are
// kept in `cache` and each texture is resolved once.
struct LtexColor {
    bool known = false;
    bool found = false;
    float r = 0.0f, g = 0.0f, b = 0.0f;
};

bool quadrantColor(const ModelResolver& resolver, const FarTerrainOptions& opt,
                   const Land& land, int quadrant,
                   std::unordered_map<FormID, LtexColor>& cache, float& r, float& g,
                   float& b) {
    const FormID ltex = land.quadrantTexture[static_cast<size_t>(quadrant)];
    if (ltex == kFormIDNull) return false;
    LtexColor& c = cache[ltex];
    if (!c.known) {
        c.known = true;
        const std::string path = resolver.texturePath(ltex);
        if (!path.empty()) c.found = opt.colorOf(path, c.r, c.g, c.b);
    }
    if (!c.found) return false;
    r = c.r;
    g = c.g;
    b = c.b;
    return true;
}

}  // namespace

std::string ModelResolver::modelPath(FormID base) const {
    if (m_db == nullptr || base == kFormIDNull) return {};
    const TESForm* form = m_db->find(base);
    if (form == nullptr) return {};

    std::string path;
    form->forEachSubrecord([&](const Subrecord& sr) {
        if (sr.type != kMODL) return true;
        // MODL is a NUL-terminated path in the record's own encoding.
        path = readString(sr);
        return false;  // stop at the first MODL
    });
    return path;
}

std::string ModelResolver::texturePath(FormID landTexture) const {
    if (m_db == nullptr || landTexture == kFormIDNull) return {};
    const TESForm* ltex = m_db->find(landTexture);
    if (ltex == nullptr) return {};

    // LTEX -> TNAM: the FormID of the texture set this layer draws with.
    FormID set = kFormIDNull;
    ltex->forEachSubrecord([&](const Subrecord& sr) {
        if (sr.type != sub::kTNAM || sr.size < 4) return true;
        set = rdU32(sr.data);
        return false;
    });
    if (set == kFormIDNull) return {};

    // TXST -> TX00: the diffuse texture path (relative to the Data folder).
    const TESForm* txst = m_db->find(set);
    if (txst == nullptr) return {};
    std::string path;
    txst->forEachSubrecord([&](const Subrecord& sr) {
        if (sr.type != sub::kTX00) return true;
        path = readString(sr);
        return false;
    });
    return path;
}

std::vector<PlacedModel> instancesForCell(const Cell& cell,
                                          const ModelResolver& resolver) {
    std::vector<PlacedModel> out;
    out.reserve(cell.objects.size());
    for (const CellObject& o : cell.objects) {
        std::string path = resolver.modelPath(o.baseObject);
        if (path.empty()) continue;
        PlacedModel m;
        m.modelPath = std::move(path);
        m.x = o.x;
        m.y = o.y;
        m.z = o.z;
        m.rotX = o.rotX;
        m.rotY = o.rotY;
        m.rotZ = o.rotZ;
        m.baseObject = o.baseObject;
        out.push_back(std::move(m));
    }
    return out;
}

bool buildTerrainMesh(const Cell& cell, const ModelResolver& resolver, Mesh& out) {
    if (cell.isInterior || !cell.land.present) return false;

    float h[kLandVertices];
    cell.land.heights(h);

    out.vertices.clear();
    out.indices.clear();
    out.submeshes.clear();
    out.indices.reserve((kLandEdge - 1) * (kLandEdge - 1) * 6);
    buildTerrainGrid(cell, h, out.vertices);

    // Split the surface into the four quadrants so each can carry its own land
    // texture. Quadrants are emitted in order 0,1,2,3 (bottom-left first), so
    // runs that share a texture merge into one submesh. Indices are reordered
    // accordingly, which the (cull-free) renderer does not mind.
    std::vector<uint32_t> quadStart(4);
    for (int q = 0; q < 4; ++q) {
        quadStart[static_cast<size_t>(q)] = static_cast<uint32_t>(out.indices.size());
        appendQuadrantIndices(out.indices, q, out.vertices);
    }

    for (int q = 0; q < 4; ++q) {
        const uint32_t start = quadStart[static_cast<size_t>(q)];
        const uint32_t end = (q < 3) ? quadStart[static_cast<size_t>(q + 1)]
                                     : static_cast<uint32_t>(out.indices.size());
        if (end <= start) continue;
        const std::string path = resolver.texturePath(cell.land.quadrantTexture[static_cast<size_t>(q)]);
        if (!out.submeshes.empty() && out.submeshes.back().indexStart + out.submeshes.back().indexCount == start &&
            out.submeshes.back().texturePath == path) {
            out.submeshes.back().indexCount += end - start;   // merge a shared texture
        } else {
            Submesh sm;
            sm.indexStart = start;
            sm.indexCount = end - start;
            sm.texturePath = path;
            out.submeshes.push_back(std::move(sm));
        }
    }
    return true;
}

bool buildTerrainLayers(const Cell& cell, const ModelResolver& resolver,
                        std::vector<Mesh>& out) {
    out.clear();
    if (cell.isInterior || !cell.land.present || cell.land.layers.empty()) return false;

    // Each quadrant's ground is its lowest-index layer -- the one buildTerrainMesh
    // draws opaque. Every layer above that blends on top of it.
    int baseLayer[4] = {-1, -1, -1, -1};
    for (const LandLayer& l : cell.land.layers) {
        if (l.quadrant >= 4) continue;
        const int q = static_cast<int>(l.quadrant);
        if (baseLayer[q] < 0 || static_cast<int>(l.layer) < baseLayer[q]) {
            baseLayer[q] = static_cast<int>(l.layer);
        }
    }

    struct Cand {
        size_t index;
        float coverage;
    };
    std::vector<Cand> cands;
    for (size_t i = 0; i < cell.land.layers.size(); ++i) {
        const LandLayer& l = cell.land.layers[i];
        if (l.quadrant >= 4 || l.weights.empty()) continue;
        if (static_cast<int>(l.layer) <= baseLayer[l.quadrant]) continue;
        float total = 0.0f;
        for (const LandVertexWeight& w : l.weights) total += w.opacity;
        if (total > 0.0f) cands.push_back(Cand{i, total});
    }
    if (cands.empty()) return false;

    // Keep the heaviest few, then composite them in layer order so an upper layer
    // lands on top of a lower one the way the record describes it.
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.coverage > b.coverage; });
    if (cands.size() > kMaxTerrainLayers) cands.resize(kMaxTerrainLayers);
    std::sort(cands.begin(), cands.end(), [&](const Cand& a, const Cand& b) {
        const LandLayer& la = cell.land.layers[a.index];
        const LandLayer& lb = cell.land.layers[b.index];
        if (la.quadrant != lb.quadrant) return la.quadrant < lb.quadrant;
        return la.layer < lb.layer;
    });

    float h[kLandVertices];
    cell.land.heights(h);

    for (const Cand& c : cands) {
        const LandLayer& l = cell.land.layers[c.index];
        const std::string path = resolver.texturePath(l.texture);
        if (path.empty()) continue;   // nothing to draw this layer with

        Mesh mesh;
        buildTerrainGrid(cell, h, mesh.vertices);
        // A layer is transparent everywhere the record does not mention: VTXT is
        // sparse (about nine vertices per layer in Tamriel), so leaving the grid's
        // default alpha in place would make every layer paint its whole quadrant
        // opaquely and the topmost layer would win everywhere instead of blending
        // only over the ground it actually covers.
        for (Vertex& v : mesh.vertices) v.a = 0.0f;
        for (const LandVertexWeight& w : l.weights) {
            if (w.index < mesh.vertices.size()) {
                mesh.vertices[w.index].a = w.opacity;
            }
        }
        appendQuadrantIndices(mesh.indices, l.quadrant, mesh.vertices);

        Submesh sm;
        sm.indexStart = 0;
        sm.indexCount = static_cast<uint32_t>(mesh.indices.size());
        sm.texturePath = path;
        sm.alphaBlend = 1;
        mesh.submeshes.push_back(std::move(sm));
        out.push_back(std::move(mesh));
    }
    return !out.empty();
}

namespace {

// One cell of the far grid: the height of its four corners, reconstructed once
// so the grid does not rebuild the same heightmap for each of its corners.
struct FarCell {
    const Land* land = nullptr;
    bool present = false;
    float corner[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // SW, SE, NW, NE
};

// Where a far-grid corner is read from: the neighbouring cell that carries it,
// which of that cell's corners it is, and the land quadrant covering it.
struct CornerRef {
    int dx, dy;    // cell offset from the corner
    int corner;    // index into FarCell::corner
    int quadrant;  // land quadrant
};

// The corner is the south-west cell's low corner, the west cell's south-east
// corner, the south cell's north-east corner, or the south-west cell's high
// corner. Each cell is asked for the quadrant that actually covers the corner.
constexpr CornerRef kCornerRefs[4] = {
    {0, 0, 0, 0},     // south-west cell, SW corner, bottom-left quadrant
    {-1, 0, 1, 1},    // west cell, SE corner, bottom-right quadrant
    {0, -1, 3, 2},    // south cell, NE corner, upper-left quadrant
    {-1, -1, 2, 3},   // south-west cell, NW corner, upper-right quadrant
};

}  // namespace

bool buildFarTerrain(int centerGridX, int centerGridY, const ModelResolver& resolver,
                     const FarTerrainOptions& opt, Mesh& out) {
    out.vertices.clear();
    out.indices.clear();
    out.submeshes.clear();
    if (opt.radiusCells <= 0 || !opt.landAt || !opt.colorOf) return false;

    const int n = opt.radiusCells * 2 + 1;   // grid corners per axis
    const int gx0 = centerGridX - opt.radiusCells;
    const int gy0 = centerGridY - opt.radiusCells;
    const int hole = opt.holeRadiusCells;    // negative keeps every quad

    std::vector<FarCell> cells(static_cast<size_t>(n) * static_cast<size_t>(n));
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            FarCell& c = cells[static_cast<size_t>(j) * static_cast<size_t>(n) + i];
            c.land = opt.landAt(gx0 + i, gy0 + j);
            if (c.land == nullptr || !c.land->present) {
                c.land = nullptr;
                continue;
            }
            c.present = true;
            c.land->corners(c.corner);   // SW, SE, NW, NE
        }
    }

    // Height and ground colour of every grid corner. A corner sits inside as many
    // as four cells; the first that has terrain supplies the height, and every
    // cell that has a texture contributes to the colour.
    const size_t cornerCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    std::vector<float> z(cornerCount, 0.0f);
    std::vector<float> colR(cornerCount, 0.38f), colG(cornerCount, 0.36f),
        colB(cornerCount, 0.32f);
    std::vector<uint8_t> ok(cornerCount, 0);
    std::unordered_map<FormID, LtexColor> colorCache;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const size_t at = static_cast<size_t>(j) * static_cast<size_t>(n) + i;
            float r = 0.0f, g = 0.0f, b = 0.0f, weight = 0.0f;
            for (const CornerRef& ref : kCornerRefs) {
                const int ci = i + ref.dx;
                const int cj = j + ref.dy;
                if (ci < 0 || cj < 0 || ci >= n || cj >= n) continue;
                const FarCell& c = cells[static_cast<size_t>(cj) * static_cast<size_t>(n) + ci];
                if (!c.present) continue;
                if (ok[at] == 0) {
                    z[at] = c.corner[ref.corner];
                    ok[at] = 1;
                }
                float cr = 0.0f, cg = 0.0f, cb = 0.0f;
                if (quadrantColor(resolver, opt, *c.land, ref.quadrant, colorCache, cr, cg,
                                  cb)) {
                    r += cr;
                    g += cg;
                    b += cb;
                    weight += 1.0f;
                }
            }
            if (weight > 0.0f) {
                colR[at] = r / weight;
                colG[at] = g / weight;
                colB[at] = b / weight;
            }
        }
    }

    // Grid corners are one cell apart, so a far view costs a fortieth of the
    // vertices the streamed terrain would. Normals come from central differences
    // of the heights, as they do for the streamed grid.
    const float spacing = kLandVertexSpacing * static_cast<float>(kLandEdge - 1);
    out.vertices.reserve(cornerCount);
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const size_t at = static_cast<size_t>(j) * static_cast<size_t>(n) + i;
            const int i0 = i > 0 ? i - 1 : i;
            const int i1 = i < n - 1 ? i + 1 : i;
            const int j0 = j > 0 ? j - 1 : j;
            const int j1 = j < n - 1 ? j + 1 : j;
            const float dzdx =
                (z[static_cast<size_t>(j) * static_cast<size_t>(n) + i1] -
                 z[static_cast<size_t>(j) * static_cast<size_t>(n) + i0]) /
                (static_cast<float>(i1 - i0) * spacing);
            const float dzdy =
                (z[static_cast<size_t>(j1) * static_cast<size_t>(n) + i] -
                 z[static_cast<size_t>(j0) * static_cast<size_t>(n) + i]) /
                (static_cast<float>(j1 - j0) * spacing);
            float nx = -dzdx, ny = -dzdy, nz = 1.0f;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-6f) {
                nx /= len;
                ny /= len;
                nz /= len;
            }

            Vertex v;
            v.x = static_cast<float>(gx0 + i) * spacing;
            v.y = static_cast<float>(gy0 + j) * spacing;
            v.z = z[at];
            v.nx = nx;
            v.ny = ny;
            v.nz = nz;
            v.r = colR[at];
            v.g = colG[at];
            v.b = colB[at];
            v.a = 1.0f;
            out.vertices.push_back(v);
        }
    }

    // Quads everywhere but the hole the streamed terrain fills. A quad touching a
    // corner with no terrain is dropped rather than guessed at.
    for (int j = 0; j + 1 < n; ++j) {
        for (int i = 0; i + 1 < n; ++i) {
            const int gx = gx0 + i;
            const int gy = gy0 + j;
            if (hole >= 0 && std::abs(gx - centerGridX) <= hole &&
                std::abs(gy - centerGridY) <= hole) {
                continue;
            }
            const size_t a = static_cast<size_t>(j) * static_cast<size_t>(n) + i;
            const size_t b = a + 1;
            const size_t c = a + static_cast<size_t>(n);
            const size_t d = c + 1;
            if (ok[a] == 0 || ok[b] == 0 || ok[c] == 0 || ok[d] == 0) continue;
            out.indices.push_back(static_cast<uint32_t>(a));
            out.indices.push_back(static_cast<uint32_t>(c));
            out.indices.push_back(static_cast<uint32_t>(b));
            out.indices.push_back(static_cast<uint32_t>(b));
            out.indices.push_back(static_cast<uint32_t>(c));
            out.indices.push_back(static_cast<uint32_t>(d));
        }
    }
    if (out.indices.empty()) return false;

    // Untextured on purpose: the ground colour rides the vertex colours, which
    // the host multiplies by the lighting exactly as it does for a textured
    // surface, so the far ground meets the streamed ground at the same exposure.
    Submesh sm;
    sm.indexStart = 0;
    sm.indexCount = static_cast<uint32_t>(out.indices.size());
    out.submeshes.push_back(std::move(sm));
    return true;
}

void MeshCache::clear() {
    m_meshes.clear();
    m_failed.clear();
    m_hits = 0;
    m_misses = 0;
}

const Mesh* MeshCache::get(const std::string& modelPath) {
    if (modelPath.empty()) return nullptr;

    // Archives key on lowercase, backslash-separated paths.
    std::string key;
    key.reserve(modelPath.size());
    for (char c : modelPath) {
        if (c == '/') {
            key.push_back('\\');
        } else {
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }

    auto it = m_meshes.find(key);
    if (it != m_meshes.end()) {
        m_hits++;
        return &it->second;
    }
    if (m_failed.find(key) != m_failed.end()) {
        m_misses++;
        return nullptr;
    }

    std::vector<std::string> candidates;
    candidates.push_back(key);
    // MODL paths omit the "meshes\\" root that archives store.
    if (!hasPrefixIgnoreCase(key, "meshes\\")) candidates.push_back("meshes\\" + key);

    for (const BsaArchive* bsa : m_archives) {
        if (bsa == nullptr) continue;
        for (const std::string& cand : candidates) {
            if (!bsa->has(cand)) continue;
            const std::vector<uint8_t> data = bsa->read(cand);
            if (data.empty()) continue;
            Mesh mesh;
            if (loadNif(data.data(), data.size(), mesh, nullptr) && mesh.valid()) {
                auto res = m_meshes.emplace(key, std::move(mesh));
                m_hits++;
                return &res.first->second;
            }
        }
    }

    m_failed[key] = true;
    m_misses++;
    return nullptr;
}

}  // namespace skyrim
