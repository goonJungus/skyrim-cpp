// collision.hpp - world geometry you can stand on and walk into.
//
// A box per model is cheap and wrong in both directions that matter: it stops
// you a tree's width away from a trunk, and it lets you walk through the gaps
// inside a roof. What a player actually touches is the triangle soup, so that is
// what this keeps: the triangles of the meshes the host streamed, in world space,
// in a per-cell acceleration structure.
//
// One cell's geometry is built once and reused for as long as the player stays
// in and around it, which is what keeps the cost off the frame: crossing a cell
// boundary builds at most a ring of new cells, and walking around inside one
// builds nothing.
//
// Nothing here knows about rendering or about the player. The host decides which
// meshes are solid (it knows what it drew and how it was tinted), hands them over
// transformed into world space, and asks where a capsule ends up.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "skyrim/mesh.hpp"

namespace skyrim {

// Deliberately its own three floats rather than the host's Vec3: the library
// must not depend on the host's renderer-side math to answer a collision query.
struct Vec3f {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// One world-space triangle, wound counter-clockwise seen from the front.
struct Triangle {
    Vec3f a, b, c;
};

// Axis-aligned bounds of a cell's collision geometry.
struct CollisionBounds {
    float minX = 1e30f, minY = 1e30f, minZ = 1e30f;
    float maxX = -1e30f, maxY = -1e30f, maxZ = -1e30f;
    bool valid() const { return minX <= maxX; }
    void expand(const Vec3f& p);
    void expand(const CollisionBounds& o);
    bool overlaps(float x0, float y0, float z0, float x1, float y1, float z1) const;
};

// Accumulates one cell's triangles. A cell is finished once and then queried
// many times a frame, so the builder keeps the filters to a minimum and the
// finished cell keeps the acceleration structure.
class CollisionCell {
public:
    void clear();

    // Adds the triangles of one placed mesh. `m` is a row-major 4x4 row-vector
    // transform (v * M, translation in the last row), the same convention the
    // renderer hands to D3D. Triangles in alpha-blended submeshes are skipped:
    // leaves, grass and banners are drawn over the world, not walked into.
    // `minRadius` skips geometry whose bounding sphere is smaller than it, which
    // is how clutter and pebbles stay out of the soup without a name list.
    // Returns the number of triangles added.
    size_t addMesh(const Mesh& mesh, const float m[16], float minRadius);

    // Builds the acceleration structure. Queries before this walk every triangle,
    // which is correct but only worth doing for a cell nobody walks in.
    void build();

    size_t triangleCount() const { return m_triangles.size(); }
    size_t nodeCount() const { return m_nodes.size(); }
    bool built() const { return m_built; }   // false again once a mesh is added
    const CollisionBounds& bounds() const { return m_bounds; }

    // Pushes a vertical capsule out of the geometry. `pos` is the capsule's feet
    // and is updated in place; `height` is the total height including the round
    // caps, `radius` the cap radius. Contacts are resolved from deepest to
    // shallowest over a few passes, so a corner where two walls meet leaves the
    // capsule in the gap rather than pushed out through one of them. Returns the
    // number of contacts resolved.
    int resolveCapsule(Vec3f& pos, float radius, float height) const;

    // The highest surface at or below `fromZ` under (x, y), searching triangles
    // within `radius` of that column. This is the floor: the query the player
    // stands on, whether it is a bridge deck, a roof, or a rock ledge. False when
    // nothing is underfoot.
    bool groundHeight(float x, float y, float fromZ, float radius, float& outZ) const;

private:
    // A BVH node. A leaf covers `count` triangles of m_order starting at
    // `first`; an interior node (count 0) points at its two children. The
    // children are named rather than assumed adjacent, because a subtree is
    // built depth-first and the right child does not follow the left one.
    struct Node {
        CollisionBounds bounds;
        uint32_t first = 0;
        uint32_t count = 0;
        uint32_t left = 0;
        uint32_t right = 0;
    };

    int32_t buildRange(uint32_t first, uint32_t count, uint32_t depth);
    void collect(const CollisionBounds& query, std::vector<uint32_t>& out) const;
    void collectNode(uint32_t index, const CollisionBounds& query,
                     std::vector<uint32_t>& out) const;

    std::vector<Triangle> m_triangles;
    std::vector<uint32_t> m_order;      // triangle indices, sorted by BVH
    std::vector<Node> m_nodes;
    CollisionBounds m_bounds;
    bool m_built = false;
};

}  // namespace skyrim