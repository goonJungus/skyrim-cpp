// world_render.hpp - turns cells into renderable, placed models.
//
// The cell system gives world structure (which objects stand where); the mesh
// loader gives geometry (what an object looks like). This is the bridge between
// them, and it is deliberately the only place that knows how a placed reference
// becomes something a renderer can draw:
//
//   Cell + REFR  --(NAME)-->  base record  --(MODL)-->  model path
//                                                        |
//                                          MeshCache ----+---- BsaArchive -> NIF
//
// Nothing here knows about a graphics API. The host pulls PlacedModel lists and
// Mesh pointers, uploads them however it likes, and stays swappable.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "skyrim/cell_system.hpp"
#include "skyrim/form_database.hpp"
#include "skyrim/mesh.hpp"

namespace skyrim {

class BsaArchive;

// One model instance placed in the world. Rotation is stored as three Euler
// angles in radians, matching the cell reader's DATA subrecord.
struct PlacedModel {
    std::string modelPath;            // MODL path as authored, e.g. "Architecture\\Whiterun\\X.nif"
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float rotX = 0.0f, rotY = 0.0f, rotZ = 0.0f;
    float scale = 1.0f;
    FormID baseObject = kFormIDNull;
};

// Resolves a base-object FormID to the model path on its MODL subrecord. Works
// for any record type that carries a model (STAT, MSTT, ACTI, FURN, DOOR, ...),
// so it needs no per-type table: a base either has a model or it does not.
class ModelResolver {
public:
    void init(const FormDatabase* db) { m_db = db; }
    bool ready() const { return m_db != nullptr; }

    // Empty when the base is unknown or carries no model.
    std::string modelPath(FormID base) const;

    // Diffuse texture path for a landscape texture (LTEX): follows
    // LTEX -> TNAM (texture set) -> TXST -> TX00. Empty when unresolved.
    std::string texturePath(FormID landTexture) const;

private:
    const FormDatabase* m_db = nullptr;
};

// Every placed model in a cell that resolves to a model path, in cell-object
// order. Objects whose base is unresolved or modelless are skipped.
std::vector<PlacedModel> instancesForCell(const Cell& cell,
                                          const ModelResolver& resolver);

// Build an exterior cell's terrain as a mesh in world space: a 32x32 quad grid
// spanning the cell square, lifted by the cell's LAND heightmap. One submesh
// carries the base land texture's diffuse path (empty when unresolved).
// Returns false when the cell has no terrain (interiors, or a missing LAND).
bool buildTerrainMesh(const Cell& cell, const ModelResolver& resolver, Mesh& out);

// Loads and caches meshes by model path, reading from one or more BSA archives.
// Lookups are case-insensitive and tolerate a missing "meshes\\" prefix, since
// MODL paths omit it while archives store it.
class MeshCache {
public:
    void addArchive(const BsaArchive* bsa) { m_archives.push_back(bsa); }
    void clear();

    // The parsed mesh, or nullptr when the model is absent or fails to parse.
    // The returned pointer is owned by the cache and stable until clear().
    const Mesh* get(const std::string& modelPath);

    size_t loadedCount() const { return m_meshes.size(); }
    size_t hitCount() const { return m_hits; }
    size_t missCount() const { return m_misses; }

private:
    std::vector<const BsaArchive*> m_archives;
    std::unordered_map<std::string, Mesh> m_meshes;
    std::unordered_map<std::string, bool> m_failed;
    size_t m_hits = 0;
    size_t m_misses = 0;
};

}  // namespace skyrim
