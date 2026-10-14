// cell_system.hpp - Skyrim world cells and their contents.
//
// A Cell is the unit of world streaming. Each exterior cell covers a 4096x4096
// unit square on the world grid and contains object references (REFR) and
// placed actors (ACHR) -- statues, furniture, doors, NPCs, and so on.
//
// Two coordinates are needed to name an exterior cell, not one: gridX/gridY
// are unique only *within a worldspace*. Tamriel (0x0000003C) and, say,
// WhiterunWorld both define a cell at grid (0,0), and every worldspace also
// carries a "persistent cell" pseudo-cell at (0,0) that holds its always-loaded
// references. Spatial lookups therefore key on
// (worldspace FormID, gridX, gridY), never on the grid pair alone.
//
// The form database stores records flat, so CELL -> REFR parentage is not
// available there. This system re-walks the ESM files with parent tracking to
// build the cell hierarchy, then indexes cells for spatial queries.
//
// Record layout relied upon here (verified against real Skyrim.esm):
//   CELL subrecords: EDID (editor id), FULL (name ref), DATA (flags),
//                    XCLC (int32 x, int32 y grid coordinate, exterior only)
//   REFR/ACHR subrecords: EDID, NAME (base object FormID),
//                    DATA (float x,y,z position + float rotX,rotY,rotZ)
//
// ESM group tree for cells:
//   GRUP "CELL" (top level)
//     GRUP gtype 2 (interior block)  -> GRUP gtype 3 (sub-block) -> CELL ...
//   GRUP "WRLD" (top level)
//     WRLD record, then a sibling GRUP gtype 1 whose label IS the WRLD's
//     FormID (its "world children"), holding the exterior blocks:
//     GRUP gtype 4 (block) -> GRUP gtype 5 (sub-block) -> CELL ...
//   A CELL record is immediately followed by its own gtype 6/8/9 child GRUPs
//   ("cell children") that carry the REFR/ACHR records.
//
// A REFR's position is in world units; gridX = floor(x / 4096), same for y.

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "skyrim/form_database.hpp"

namespace skyrim {

// World-space size of one exterior cell, in game units (Skyrim units).
constexpr float kCellSize = 4096.0f;

// ESM group type of a worldspace's "children" group; its label is the FormID
// of the WRLD record it belongs to.
constexpr int32_t kGroupTypeWorldChildren = 1;

// One object reference inside a cell (a placed REFR or ACHR).
struct CellObject {
    FormID formId = kFormIDNull;      // the reference's own FormID
    FormID baseObject = kFormIDNull;  // NAME -> the static/item/NPC it instantiates
    float x = 0.0f, y = 0.0f, z = 0.0f;       // world position
    float rotX = 0.0f, rotY = 0.0f, rotZ = 0.0f; // world rotation (radians)
    bool  isActor = false;            // true for ACHR, false for REFR
    std::string editorId;             // may be empty
};

// A world cell.
struct Cell {
    FormID formId = kFormIDNull;
    // Owning worldspace. Null (0) for interior cells and the top-level CELL
    // group, which belong to no worldspace.
    FormID worldspace = kFormIDNull;
    bool isInterior = false;
    // The worldspace's "persistent cell": a pseudo-cell that holds every
    // always-loaded reference for the worldspace, stored at grid (0,0) but
    // whose references sit anywhere on the map. It is a direct child of the
    // worldspace's children group and is excluded from spatial indexing so it
    // cannot collide with a real cell that also occupies grid (0,0).
    bool isPersistent = false;
    // Grid coordinate (exterior cells only). Valid when isInterior == false.
    int32_t gridX = 0;
    int32_t gridY = 0;
    // World-space bounds (exterior cells only).
    float worldMinX = 0.0f, worldMinY = 0.0f;
    float worldMaxX = 0.0f, worldMaxY = 0.0f;
    // Center of the cell in world space.
    float centerX = 0.0f, centerY = 0.0f;

    LocalizedString name;   // FULL, resolved later by StringResolver
    std::string editorId;   // EDID

    std::vector<CellObject> objects;
};

// Spatial index over cells. Exterior cells are keyed by worldspace and grid
// coordinate; every cell is also reachable by its FormID.
class CellSystem {
public:
    CellSystem() = default;
    ~CellSystem() = default;
    CellSystem(const CellSystem&) = delete;
    CellSystem& operator=(const CellSystem&) = delete;

    // Build the cell index by walking the ESM files with parent tracking.
    // Returns Ok on success. On failure the system is left empty.
    Status load(const SkyrimConfig& config);

    void clear();
    bool loaded() const { return m_loaded; }

    size_t cellCount() const { return m_cells.size(); }
    size_t exteriorCellCount() const { return m_gridIndex.size(); }
    size_t persistentCellCount() const { return m_persistentCellCount; }
    size_t worldspaceCount() const { return m_worldspaceCells.size(); }
    size_t totalObjectCount() const;

    // All loaded cells.
    const std::deque<Cell>& cells() const { return m_cells; }

    // Find an exterior cell by worldspace and exact grid coordinate.
    // Returns nullptr when absent.
    const Cell* findCell(FormID worldspace, int32_t gridX, int32_t gridY) const;

    // Find any cell (interior or exterior) by its FormID. When a later plugin
    // overrides the same FormID, the override wins.
    const Cell* findCellById(FormID id) const;

    // Exterior cells of `worldspace` whose grid square overlaps the given
    // world-space AABB.
    std::vector<const Cell*> queryBounds(FormID worldspace, float minX, float minY,
                                         float maxX, float maxY) const;

    // worldspace FormID -> number of exterior cells, sorted by count descending.
    std::vector<std::pair<FormID, size_t>> worldspaceHistogram() const;

    // Resolve cell names through a string resolver (FULL is a string-table id
    // on Skyrim SE). Call after load(); safe to call repeatedly.
    void resolveNames(const class StringResolver& resolver);

private:
    // Grid key: a worldspace plus a cell coordinate. Both are needed because
    // coordinates repeat across worldspaces.
    struct GridKey {
        FormID  worldspace = kFormIDNull;
        int32_t gridX = 0;
        int32_t gridY = 0;
        bool operator==(const GridKey& o) const {
            return worldspace == o.worldspace && gridX == o.gridX && gridY == o.gridY;
        }
    };
    struct GridKeyHash {
        size_t operator()(const GridKey& k) const noexcept;
    };

    bool m_loaded = false;
    // deque: push_back never invalidates pointers to existing elements, which
    // findCell/queryBounds return to callers.
    std::deque<Cell> m_cells;
    // (worldspace, gridX, gridY) -> pointer into m_cells, exterior cells only.
    std::unordered_map<GridKey, const Cell*, GridKeyHash> m_gridIndex;
    // FormID -> pointer into m_cells, every cell.
    std::unordered_map<FormID, const Cell*> m_byFormId;
    // worldspace FormID -> number of exterior cells (diagnostics).
    std::unordered_map<FormID, size_t> m_worldspaceCells;
    size_t m_persistentCellCount = 0;
};

}  // namespace skyrim
