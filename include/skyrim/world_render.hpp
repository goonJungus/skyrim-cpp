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
#include <functional>
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

// Terrain blend layers for an exterior cell. Each returned mesh is the cell's
// full 33x33 grid restricted to one quadrant, with the layer's per-vertex
// opacity in Vertex::a, so the host can draw it alpha-blended over the base
// terrain. Layers come back in the order they must be composited (bottom first)
// and at most kMaxTerrainLayers are returned -- the heaviest, since a cell can
// carry two dozen that mostly cover a few vertices each. Empty when the cell has
// no layered terrain.
//
// Six is chosen from the data: over a third of Tamriel's terrain cells carry
// more than three layers, and a layer that is dropped is ground that is drawn
// with the wrong surface. Quads a layer does not cover are not emitted at all,
// so the extra layers cost far less fill than their count suggests.
constexpr size_t kMaxTerrainLayers = 6;
bool buildTerrainLayers(const Cell& cell, const ModelResolver& resolver,
                        std::vector<Mesh>& out);

// Distant terrain: one coarse mesh that carries the whole visible landscape, so
// an open view costs a single draw call instead of streaming every cell out to
// the horizon.
//
// Every cell's heightmap is already in memory, so the far ground is sampled on
// cell corners -- one vertex per 4096 units instead of one per 128 -- and each
// vertex carries the colour of the land texture under it. That is what makes a
// mountain three kilometres away read as rock or snow without its texture.
//
// The near terrain covers the cells around the player, so the builder leaves a
// hole for it and emits only the ring around it. Both meshes sample the same
// heightmaps at shared cell corners, so they meet without a seam.
struct FarTerrainOptions {
    // Half-extent of the far square, in cells. 32 is a little under two km.
    int radiusCells = 32;
    // Cells around the centre left to the streamed terrain. This must match the
    // stream radius used by the caller, or the two meshes will overlap. Negative
    // keeps every quad, which is what a caller with no streamed terrain wants.
    int holeRadiusCells = 3;
    // Terrain of one exterior cell, or nullptr where there is none (a missing
    // LAND, an interior, or an unloaded cell).
    std::function<const Land*(int gridX, int gridY)> landAt;
    // Diffuse colour of a land texture (0..1 per channel). False when the
    // texture is unknown, where the builder falls back to a neutral tone.
    std::function<bool(const std::string& path, float& r, float& g, float& b)> colorOf;
};

// Builds the ring of far terrain around one cell. `out` is a single submesh with
// no texture: its colour comes entirely from its vertex colours, which the host
// draws through its flat-tint path. Returns false when no cell around the centre
// carries terrain.
bool buildFarTerrain(int centerGridX, int centerGridY, const ModelResolver& resolver,
                     const FarTerrainOptions& opt, Mesh& out);

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
