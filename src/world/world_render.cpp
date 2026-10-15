// world_render.cpp - cell -> placed models -> cached meshes.

#include "skyrim/world_render.hpp"

#include "skyrim/bsa_archive.hpp"

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
    out.vertices.reserve(kLandVertices);
    out.indices.reserve((kLandEdge - 1) * (kLandEdge - 1) * 6);

    for (int row = 0; row < kLandEdge; ++row) {
        for (int col = 0; col < kLandEdge; ++col) {
            const int i = row * kLandEdge + col;
            // Surface normal from central differences of the height grid.
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
            // World-space UVs so seamless land textures tile across the world.
            v.u = v.x / kLandTextureWorldSize;
            v.v = v.y / kLandTextureWorldSize;
            out.vertices.push_back(v);
        }
    }

    // Split the surface into the four quadrants so each can carry its own land
    // texture. Quadrants are emitted in order 0,1,2,3 (bottom-left first), so
    // runs that share a texture merge into one submesh. Indices are reordered
    // accordingly, which the (cull-free) renderer does not mind.
    struct Quad {
        int row0, row1, col0, col1;
    };
    const Quad quads[4] = {
        {0, 16, 0, 16},    // 0 bottom-left
        {0, 16, 16, 32},   // 1 bottom-right
        {16, 32, 0, 16},   // 2 upper-left
        {16, 32, 16, 32},  // 3 upper-right
    };

    out.indices.clear();
    out.indices.reserve((kLandEdge - 1) * (kLandEdge - 1) * 6);
    std::vector<uint32_t> quadStart(4);
    for (int q = 0; q < 4; ++q) {
        const Quad& r = quads[q];
        quadStart[static_cast<size_t>(q)] = static_cast<uint32_t>(out.indices.size());
        for (int row = r.row0; row < r.row1; ++row) {
            for (int col = r.col0; col < r.col1; ++col) {
                const uint32_t a = static_cast<uint32_t>(row * kLandEdge + col);
                const uint32_t b = a + 1;
                const uint32_t c = a + kLandEdge;
                const uint32_t d = c + 1;
                out.indices.push_back(a);
                out.indices.push_back(c);
                out.indices.push_back(b);
                out.indices.push_back(b);
                out.indices.push_back(c);
                out.indices.push_back(d);
            }
        }
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
