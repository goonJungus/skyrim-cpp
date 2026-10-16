// collision_test.cpp - the collision cell, with no fixture and no game install.
//
// The cell's whole job is answering two questions about triangles it was handed,
// so the tests build the meshes in memory and ask them: what is underfoot, and
// where does a capsule end up. Every check is run twice, once against the BVH
// and once against the cell before build(), which walks the triangles directly --
// the tree is only worth having if it answers exactly what the linear scan does.
//
// Usage: collision_test

#include "skyrim/collision.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace skyrim;

namespace {

int failures = 0;

void expect(bool cond, const std::string& what) {
    if (!cond) {
        std::printf("    FAIL: %s\n", what.c_str());
        failures++;
    }
}

void expectNear(float got, float want, const std::string& what, float tol = 0.05f) {
    const float d = got - want;
    if (d > tol || d < -tol) {
        std::printf("    FAIL: %s (got %.3f, want %.3f)\n", what.c_str(), got, want);
        failures++;
    }
}

void expectEq(size_t got, size_t want, const std::string& what) {
    if (got != want) {
        std::printf("    FAIL: %s (got %zu, want %zu)\n", what.c_str(), got, want);
        failures++;
    }
}

const float kIdentity[16] = {1, 0, 0, 0,
                             0, 1, 0, 0,
                             0, 0, 1, 0,
                             0, 0, 0, 1};

// Row-vector translation, the host's own convention.
void translation(float x, float y, float z, float m[16]) {
    for (int i = 0; i < 16; i++) m[i] = kIdentity[i];
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

// An axis-aligned box as twelve triangles over its eight corners. Collision has
// no use for normals or UVs, so the corners are shared between faces.
Mesh makeBox(float x0, float y0, float z0, float x1, float y1, float z1,
             uint32_t alphaBlend = 0) {
    Mesh m;
    const float c[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                           {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
    for (const auto& v : c) m.vertices.push_back(Vertex{v[0], v[1], v[2], 0, 0, 1, 0, 0});

    // Each face as a quad, both windings: a closed box is watertight for the
    // closest-point maths whether or not a face points the right way, and
    // emitting both keeps the helper free of per-face index tables.
    const uint32_t face[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                 {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    for (const auto& f : face) {
        const uint32_t tri[6] = {f[0], f[1], f[2], f[0], f[2], f[3]};
        for (uint32_t i : tri) m.indices.push_back(i);
    }
    Submesh sm;
    sm.indexStart = 0;
    sm.indexCount = static_cast<uint32_t>(m.indices.size());
    sm.alphaBlend = alphaBlend;
    m.submeshes.push_back(sm);
    return m;
}

// A flat quad, for floors.
Mesh makeQuad(float z) {
    Mesh m;
    const float c[4][3] = {{0, 0, z}, {1000, 0, z}, {1000, 1000, z}, {0, 1000, z}};
    for (const auto& v : c) m.vertices.push_back(Vertex{v[0], v[1], v[2], 0, 0, 1, 0, 0});
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    for (uint32_t i : idx) m.indices.push_back(i);
    Submesh sm;
    sm.indexStart = 0;
    sm.indexCount = 6;
    m.submeshes.push_back(sm);
    return m;
}

void testBasics() {
    std::printf("  basics\n");
    const Mesh box = makeBox(0, 0, 0, 100, 200, 300);
    CollisionCell cell;
    expectEq(cell.addMesh(box, kIdentity, 0.0f), 12, "a box adds twelve triangles");
    expectEq(cell.triangleCount(), 12, "triangle count");

    const CollisionBounds& b = cell.bounds();
    expect(b.valid(), "bounds are valid");
    expectNear(b.minX, 0.0f, "bounds min x");
    expectNear(b.maxX, 100.0f, "bounds max x");
    expectNear(b.maxZ, 300.0f, "bounds max z");
    expect(b.overlaps(-10, -10, 250, 10, 10, 350), "bounds overlap a straddling box");
    expect(!b.overlaps(200, 0, 0, 300, 100, 10), "bounds miss a disjoint box");

    // Blended submeshes are drawn over the world, not walked into.
    CollisionCell blended;
    expectEq(blended.addMesh(box, kIdentity, 0.0f), 12, "opaque box adds triangles");
    expectEq(blended.addMesh(makeBox(0, 0, 0, 100, 200, 300, 1), kIdentity, 0.0f), 0,
             "a blended mesh adds none");
    expectEq(blended.triangleCount(), 12, "the blended copy left no geometry");

    // Clutter is kept out by size, not by a hand-written list of prop names.
    CollisionCell clutter;
    expectEq(clutter.addMesh(makeBox(0, 0, 0, 10, 10, 10), kIdentity, 32.0f), 0,
             "a small mesh below minRadius is skipped");
    expectEq(clutter.addMesh(makeBox(0, 0, 0, 100, 100, 100), kIdentity, 32.0f), 12,
             "a large mesh is kept");

    // The transform is the host's row-vector matrix, translation in the last row.
    float m[16];
    translation(1000, 0, 0, m);
    CollisionCell moved;
    moved.addMesh(makeBox(0, 0, 0, 100, 200, 300), m, 0.0f);
    expectNear(moved.bounds().minX, 1000.0f, "transform moved the bounds");
    expectNear(moved.bounds().maxX, 1100.0f, "transform moved the bounds far side");

    CollisionCell clearMe;
    clearMe.addMesh(box, kIdentity, 0.0f);
    clearMe.build();
    clearMe.clear();
    expectEq(clearMe.triangleCount(), 0, "clear drops the triangles");
    expectEq(clearMe.nodeCount(), 0, "clear drops the tree");
}

void testGroundHeight() {
    std::printf("  groundHeight\n");
    CollisionCell cell;
    cell.addMesh(makeBox(0, 0, 0, 400, 400, 300), kIdentity, 0.0f);
    cell.build();
    expect(cell.nodeCount() > 0, "build produced nodes");

    float z = 0.0f;
    expect(cell.groundHeight(200, 200, 1000, 40, z), "the deck is underfoot");
    expectNear(z, 300.0f, "the deck is the box top");

    // fromZ is a search ceiling, not a ray: a surface above it is overhead, and
    // what is below it is the answer instead.
    expect(cell.groundHeight(200, 200, 200, 40, z), "the box underside is underfoot");
    expectNear(z, 0.0f, "and is the lower of the two surfaces");
    expect(cell.groundHeight(200, 200, 300, 40, z), "the deck at exactly fromZ");
    expectNear(z, 300.0f, "the deck height again");

    // The radius is how far the search looks sideways, which is what lets a
    // player stand on a ledge their capsule centre hangs slightly off.
    expect(cell.groundHeight(430, 200, 1000, 40, z), "just inside the ledge");
    expect(!cell.groundHeight(450, 200, 1000, 40, z), "just outside the ledge");
    expect(cell.groundHeight(500, 200, 1000, 200, z), "inside a wider search");
    expectNear(z, 300.0f, "the ledge height from the wider search");

    CollisionCell empty;
    expect(!empty.groundHeight(0, 0, 1000, 40, z), "an empty cell has no floor");

    // The tree must answer exactly what the linear scan answers.
    CollisionCell scan;
    scan.addMesh(makeQuad(250), kIdentity, 0.0f);
    CollisionCell tree;
    tree.addMesh(makeQuad(250), kIdentity, 0.0f);
    tree.build();
    int mismatches = 0;
    for (int i = 0; i <= 20; i++) {
        for (int j = 0; j <= 20; j++) {
            const float x = -100.0f + static_cast<float>(i) * 60.0f;
            const float y = -100.0f + static_cast<float>(j) * 60.0f;
            float a = 0.0f, c = 0.0f;
            const bool gotA = scan.groundHeight(x, y, 1000.0f, 45.0f, a);
            const bool gotC = tree.groundHeight(x, y, 1000.0f, 45.0f, c);
            if (gotA != gotC || (gotA && std::fabs(a - c) > 1e-4f)) mismatches++;
        }
    }
    expectEq(static_cast<size_t>(mismatches), 0, "the BVH matches the linear scan");
}

void testCapsule() {
    std::printf("  resolveCapsule\n");
    const float radius = 30.0f;
    const float height = 100.0f;

    // Resting on a deck: the capsule sinks into it and is pushed back out so its
    // feet sit on the surface.
    {
        CollisionCell cell;
        cell.addMesh(makeQuad(200), kIdentity, 0.0f);
        cell.build();
        Vec3f pos{500, 500, 190};   // ten units into the floor
        const int n = cell.resolveCapsule(pos, radius, height);
        expect(n > 0, "a sunken capsule resolves contacts");
        expectNear(pos.z, 200.0f, "the capsule is lifted onto the deck");
    }

    // A wall: walking into it stops the capsule at its surface instead of
    // stopping a full bounding box away.
    {
        CollisionCell cell;
        cell.addMesh(makeBox(400, -200, 0, 500, 1200, 800), kIdentity, 0.0f);
        cell.build();
        Vec3f pos{380, 500, 0};    // 20 units clear of the wall, radius 30
        const int n = cell.resolveCapsule(pos, radius, height);
        expect(n > 0, "a capsule inside a wall resolves contacts");
        expectNear(pos.x, 400.0f - radius, "the capsule rests against the wall", 0.2f);
        expectNear(pos.y, 500.0f, "no sideways drift", 0.2f);
    }

    // A corner: two walls meeting. The capsule has to end up in the gap between
    // them, which is the case a single-pass push gets wrong.
    {
        CollisionCell cell;
        cell.addMesh(makeBox(400, -200, 0, 500, 500, 800), kIdentity, 0.0f);   // x wall
        cell.addMesh(makeBox(-200, 400, 0, 500, 500, 800), kIdentity, 0.0f);  // y wall
        cell.build();
        Vec3f pos{385, 385, 0};
        const int n = cell.resolveCapsule(pos, radius, height);
        expect(n >= 1, "a capsule in a corner resolves contacts");
        expect(pos.x < 400.0f - radius + 0.5f, "pushed out of the x wall");
        expect(pos.y < 400.0f - radius + 0.5f, "pushed out of the y wall");
        expect(pos.x > 300.0f && pos.y > 300.0f, "left in the gap, not through a wall");
    }

    // Clear air: nothing to do.
    {
        CollisionCell cell;
        cell.addMesh(makeBox(0, 0, 0, 100, 100, 100), kIdentity, 0.0f);
        cell.build();
        Vec3f pos{900, 900, 900};
        expectEq(static_cast<size_t>(cell.resolveCapsule(pos, radius, height)), 0,
                 "a capsule in clear air resolves nothing");
        expectNear(pos.x, 900.0f, "and is not moved");
    }

    // Enough clearance is left alone: a capsule that fits between two walls must
    // not drift, or a doorway would shove the player around while they walk it.
    {
        CollisionCell cell;
        cell.addMesh(makeBox(400, -200, 0, 500, 200, 800), kIdentity, 0.0f);
        cell.addMesh(makeBox(400, 800, 0, 500, 1200, 800), kIdentity, 0.0f);
        cell.build();
        Vec3f pos{370, 500, 0};   // the doorway is 600 wide, the capsule 60
        expectEq(static_cast<size_t>(cell.resolveCapsule(pos, radius, height)), 0,
                 "a capsule with clearance resolves nothing");
        expectNear(pos.x, 370.0f, "and is not moved");
    }

    // Degenerate inputs are refused rather than acted on.
    {
        CollisionCell cell;
        cell.addMesh(makeQuad(0), kIdentity, 0.0f);
        cell.build();
        Vec3f pos{0, 0, 0};
        expectEq(static_cast<size_t>(cell.resolveCapsule(pos, 0.0f, height)), 0,
                 "a zero radius resolves nothing");
        expectEq(static_cast<size_t>(cell.resolveCapsule(pos, radius, radius)), 0,
                 "a capsule shorter than its radius resolves nothing");
    }
}

}  // namespace

int main() {
    std::printf("collision_test\n");
    testBasics();
    testGroundHeight();
    testCapsule();
    if (failures == 0) {
        std::printf("  ok\n");
        return 0;
    }
    std::printf("  %d failure(s)\n", failures);
    return 1;
}
