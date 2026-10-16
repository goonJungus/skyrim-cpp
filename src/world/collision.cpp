// collision.cpp - world-space triangles you can stand on and walk into.
//
// The BVH is a plain binary tree over the cell's triangles, leaves of a few
// triangles each, split on the longest axis of the centroid spread. Queries walk
// it with an AABB test per node; everything below that is ordinary closest-point
// maths. Nothing here knows about a player: the host moves a capsule and asks
// where it ends up.

#include "skyrim/collision.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace skyrim {
namespace {

// A leaf this many triangles keeps the tree small -- a cell's nodes cost more
// memory than its triangles do -- while still cutting a query down to a few
// dozen triangle tests.
constexpr uint32_t kLeafTriangles = 8;
// Depth cap for a degenerate cell (every triangle on one plane, say).
constexpr uint32_t kMaxDepth = 24;
// Enough passes to settle a corner where several surfaces meet at once; each
// pass applies only the deepest contact found.
constexpr int kResolvePasses = 8;

Vec3f sub(const Vec3f& a, const Vec3f& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3f add(const Vec3f& a, const Vec3f& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3f mul(const Vec3f& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot3(const Vec3f& a, const Vec3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3f cross3(const Vec3f& a, const Vec3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float lengthOf(const Vec3f& v) { return std::sqrt(dot3(v, v)); }

// Row-vector 4x4, m[row * 4 + col], translation in the last row: the same
// convention the renderer hands to D3D, so the host passes its own matrices
// straight through.
Vec3f transform(const float m[16], float x, float y, float z) {
    return {x * m[0] + y * m[4] + z * m[8] + m[12],
            x * m[1] + y * m[5] + z * m[9] + m[13],
            x * m[2] + y * m[6] + z * m[10] + m[14]};
}

// Longest the transform can stretch a unit vector, i.e. the scale factor that
// turns the local bounding sphere into the world one.
float transformScale(const float m[16]) {
    float s = 0.0f;
    for (int r = 0; r < 3; r++) {
        s = std::max(s, std::sqrt(m[4 * r + 0] * m[4 * r + 0] + m[4 * r + 1] * m[4 * r + 1] +
                                  m[4 * r + 2] * m[4 * r + 2]));
    }
    return s;
}

Vec3f clampToSegment(const Vec3f& p, const Vec3f& a, const Vec3f& b) {
    const Vec3f ab = sub(b, a);
    const float len2 = dot3(ab, ab);
    if (len2 <= 1e-12f) return a;
    float t = dot3(sub(p, a), ab) / len2;
    t = std::min(1.0f, std::max(0.0f, t));
    return add(a, mul(ab, t));
}

// Closest point to `p` on the triangle (Ericson, Real-Time Collision Detection).
Vec3f closestOnTriangle(const Triangle& t, const Vec3f& p) {
    const Vec3f ab = sub(t.b, t.a);
    const Vec3f ac = sub(t.c, t.a);
    const Vec3f ap = sub(p, t.a);
    const float d1 = dot3(ab, ap);
    const float d2 = dot3(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return t.a;

    const Vec3f bp = sub(p, t.b);
    const float d3 = dot3(ab, bp);
    const float d4 = dot3(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return t.b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return add(t.a, mul(ab, d1 / (d1 - d3)));

    const Vec3f cp = sub(p, t.c);
    const float d5 = dot3(ab, cp);
    const float d6 = dot3(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return t.c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return add(t.a, mul(ac, d2 / (d2 - d6)));

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        return add(t.b, mul(sub(t.c, t.b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
    }

    const float denom = 1.0f / (va + vb + vc);
    return add(t.a, add(mul(ab, vb * denom), mul(ac, vc * denom)));
}

// Barycentric weights (wa, wb, wc) of the point on the triangle's XY projection
// closest to (px, py). The triangle is edge-on to the ground when its projected
// area vanishes -- a wall, or a quad split down its seam -- so that case falls
// back to the nearest vertex rather than dividing by zero.
void closestBary2D(const Triangle& t, float px, float py, float w[3]) {
    float area2 = (t.b.x - t.a.x) * (t.c.y - t.a.y) - (t.b.y - t.a.y) * (t.c.x - t.a.x);
    const float sgn = area2 < 0.0f ? -1.0f : 1.0f;
    area2 *= sgn;

    if (std::fabs(area2) > 1e-12f) {
        // Inside test on the counter-clockwise winding (hence sgn). `u` measures
        // the point against the b->c corner and `v` against the c->a one, so u
        // and v are the weights of c and a respectively.
        const float u = sgn * ((t.b.x - t.a.x) * (py - t.a.y) - (t.b.y - t.a.y) * (px - t.a.x));
        const float v = sgn * ((t.c.x - t.b.x) * (py - t.b.y) - (t.c.y - t.b.y) * (px - t.b.x));
        if (u >= 0.0f && v >= 0.0f && u + v <= area2) {
            w[0] = v / area2;
            w[2] = u / area2;
            w[1] = 1.0f - w[0] - w[2];
            return;
        }
    }

    // Outside the triangle (or edge-on): the closest point is on an edge, or on
    // a vertex when the projected edges are degenerate too.
    const float vx[3] = {t.a.x, t.b.x, t.c.x};
    const float vy[3] = {t.a.y, t.b.y, t.c.y};
    float best = std::numeric_limits<float>::max();
    for (int e = 0; e < 3; e++) {
        const int n = (e + 1) % 3;
        const float ex = vx[n] - vx[e];
        const float ey = vy[n] - vy[e];
        const float len2 = ex * ex + ey * ey;
        float s = len2 <= 1e-12f ? 0.0f : ((px - vx[e]) * ex + (py - vy[e]) * ey) / len2;
        s = std::min(1.0f, std::max(0.0f, s));
        const float cx = vx[e] + ex * s;
        const float cy = vy[e] + ey * s;
        const float d = (cx - px) * (cx - px) + (cy - py) * (cy - py);
        if (d < best) {
            best = d;
            w[0] = w[1] = w[2] = 0.0f;
            w[e] = 1.0f - s;
            w[n] = s;
        }
    }
}

}  // namespace

void CollisionBounds::expand(const Vec3f& p) {
    if (p.x < minX) minX = p.x;
    if (p.y < minY) minY = p.y;
    if (p.z < minZ) minZ = p.z;
    if (p.x > maxX) maxX = p.x;
    if (p.y > maxY) maxY = p.y;
    if (p.z > maxZ) maxZ = p.z;
}

void CollisionBounds::expand(const CollisionBounds& o) {
    if (!o.valid()) return;
    if (o.minX < minX) minX = o.minX;
    if (o.minY < minY) minY = o.minY;
    if (o.minZ < minZ) minZ = o.minZ;
    if (o.maxX > maxX) maxX = o.maxX;
    if (o.maxY > maxY) maxY = o.maxY;
    if (o.maxZ > maxZ) maxZ = o.maxZ;
}

bool CollisionBounds::overlaps(float x0, float y0, float z0, float x1, float y1,
                               float z1) const {
    return minX <= x1 && maxX >= x0 && minY <= y1 && maxY >= y0 && minZ <= z1 && maxZ >= z0;
}

void CollisionCell::clear() {
    m_triangles.clear();
    m_order.clear();
    m_nodes.clear();
    m_bounds = CollisionBounds{};
    m_built = false;
}

size_t CollisionCell::addMesh(const Mesh& mesh, const float m[16], float minRadius) {
    if (mesh.indices.empty() || mesh.vertices.empty()) return 0;

    // The mesh's own bounding sphere, grown by the transform's largest scale.
    // Clutter and pebbles are rejected here rather than by a name list, which
    // would need a hand-written allowlist of every prop in the game.
    Vec3f center{0.0f, 0.0f, 0.0f};
    for (const Vertex& v : mesh.vertices) center = add(center, Vec3f{v.x, v.y, v.z});
    center = mul(center, 1.0f / static_cast<float>(mesh.vertices.size()));
    float radius = 0.0f;
    for (const Vertex& v : mesh.vertices) {
        radius = std::max(radius, lengthOf(sub(Vec3f{v.x, v.y, v.z}, center)));
    }
    if (radius * transformScale(m) < minRadius) return 0;

    const size_t before = m_triangles.size();
    // One submesh pass over the shared vertex array: the indices a submesh owns
    // are a contiguous run, and the triangle soup is built in world space so no
    // query has to know the transform any more.
    for (const Submesh& sm : mesh.submeshes) {
        if (sm.indexCount < 3) continue;
        if (sm.alphaBlend != 0) continue;   // leaves, grass, banners: not solid
        const size_t end =
            std::min<size_t>(mesh.indices.size(),
                             static_cast<size_t>(sm.indexStart) + sm.indexCount);
        for (size_t i = sm.indexStart; i + 2 < end; i += 3) {
            const uint32_t ia = mesh.indices[i];
            const uint32_t ib = mesh.indices[i + 1];
            const uint32_t ic = mesh.indices[i + 2];
            if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() ||
                ic >= mesh.vertices.size()) {
                continue;
            }
            const Vertex& a = mesh.vertices[ia];
            const Vertex& b = mesh.vertices[ib];
            const Vertex& c = mesh.vertices[ic];
            Triangle t;
            t.a = transform(m, a.x, a.y, a.z);
            t.b = transform(m, b.x, b.y, b.z);
            t.c = transform(m, c.x, c.y, c.z);
            m_triangles.push_back(t);
        }
    }
    if (m_triangles.size() == before) return 0;

    // Adding geometry invalidates whatever tree was built over it.
    m_built = false;
    for (size_t i = before; i < m_triangles.size(); i++) {
        m_bounds.expand(m_triangles[i].a);
        m_bounds.expand(m_triangles[i].b);
        m_bounds.expand(m_triangles[i].c);
    }
    return m_triangles.size() - before;
}

void CollisionCell::build() {
    m_order.resize(m_triangles.size());
    for (size_t i = 0; i < m_order.size(); i++) m_order[i] = static_cast<uint32_t>(i);
    m_nodes.clear();
    if (m_order.empty()) {
        m_built = true;
        return;
    }
    m_nodes.reserve(2 * (m_order.size() / kLeafTriangles + 1));
    buildRange(0, static_cast<uint32_t>(m_order.size()), 0);
    m_built = true;
}

int32_t CollisionCell::buildRange(uint32_t first, uint32_t count, uint32_t depth) {
    const int32_t self = static_cast<int32_t>(m_nodes.size());
    m_nodes.push_back(Node{});

    Node node;
    node.first = first;
    node.count = count;
    for (uint32_t i = 0; i < count; i++) {
        const Triangle& t = m_triangles[m_order[first + i]];
        node.bounds.expand(t.a);
        node.bounds.expand(t.b);
        node.bounds.expand(t.c);
    }

    // A small leaf, or a tree deep enough that the split is no longer paying.
    if (count <= kLeafTriangles || depth >= kMaxDepth) {
        m_nodes[static_cast<size_t>(self)] = node;
        return self;
    }

    // Split the longest axis of the centroid spread: the axis that actually
    // separates the triangles is the one with the widest spread, and splitting
    // anywhere else would build a chain.
    Vec3f lo{1e30f, 1e30f, 1e30f};
    Vec3f hi{-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < count; i++) {
        const Triangle& t = m_triangles[m_order[first + i]];
        const Vec3f c = mul(add(add(t.a, t.b), t.c), 1.0f / 3.0f);
        lo.x = std::min(lo.x, c.x); lo.y = std::min(lo.y, c.y); lo.z = std::min(lo.z, c.z);
        hi.x = std::max(hi.x, c.x); hi.y = std::max(hi.y, c.y); hi.z = std::max(hi.z, c.z);
    }
    const Vec3f spread = sub(hi, lo);
    const int axis = (spread.x >= spread.y && spread.x >= spread.z) ? 0
                                                                     : (spread.y >= spread.z ? 1 : 2);
    auto centroid = [axis](const Triangle& t) {
        const Vec3f c = mul(add(add(t.a, t.b), t.c), 1.0f / 3.0f);
        return axis == 0 ? c.x : (axis == 1 ? c.y : c.z);
    };
    auto* mid = m_order.data() + first;
    std::nth_element(mid, mid + count / 2, mid + count,
                     [&](uint32_t a, uint32_t b) {
                         return centroid(m_triangles[a]) < centroid(m_triangles[b]);
                     });
    const uint32_t leftCount = count / 2;
    // A split that moved nothing (coincident centroids) would recurse forever
    // on the same range, so take what we have as a leaf.
    if (mid[0] == mid[count - 1]) {
        m_nodes[static_cast<size_t>(self)] = node;
        return self;
    }

    node.count = 0;   // interior: the children index the tree
    m_nodes[static_cast<size_t>(self)] = node;
    const int32_t left = buildRange(first, leftCount, depth + 1);
    const int32_t right = buildRange(first + leftCount, count - leftCount, depth + 1);
    m_nodes[static_cast<size_t>(self)].left = static_cast<uint32_t>(left);
    m_nodes[static_cast<size_t>(self)].right = static_cast<uint32_t>(right);
    return self;
}

void CollisionCell::collect(const CollisionBounds& query, std::vector<uint32_t>& out) const {
    out.clear();
    if (m_triangles.empty()) return;
    if (!m_built || m_nodes.empty()) {
        // An unbuilt cell has no tree to walk, so the honest answer is every
        // triangle. Correct, and only ever paid for by a cell nobody built.
        for (uint32_t i = 0; i < m_triangles.size(); i++) out.push_back(i);
        return;
    }
    collectNode(0, query, out);
}

void CollisionCell::collectNode(uint32_t index, const CollisionBounds& query,
                                std::vector<uint32_t>& out) const {
    const Node& n = m_nodes[index];
    if (!n.bounds.overlaps(query.minX, query.minY, query.minZ, query.maxX, query.maxY,
                          query.maxZ)) {
        return;
    }
    if (n.count == 0) {
        collectNode(n.left, query, out);
        collectNode(n.right, query, out);
        return;
    }
    for (uint32_t i = 0; i < n.count; i++) out.push_back(m_order[n.first + i]);
}

int CollisionCell::resolveCapsule(Vec3f& pos, float radius, float height) const {
    if (m_triangles.empty()) return 0;
    if (radius <= 0.0f || height < 2.0f * radius) return 0;

    // The capsule is the segment between its two cap centres, which sit one
    // radius in from each end of the feet-to-head span. The query box is the
    // whole capsule rather than that segment, so a floor the feet have sunk
    // below is still inside it.
    const float half = 0.5f * height - radius;
    const float midZ = pos.z + 0.5f * height;
    const Vec3f lo{pos.x, pos.y, midZ - half};
    const Vec3f hi{pos.x, pos.y, midZ + half};

    int resolved = 0;
    std::vector<uint32_t> hits;
    for (int pass = 0; pass < kResolvePasses; pass++) {
        CollisionBounds query;
        query.expand(Vec3f{pos.x - radius, pos.y - radius, pos.z});
        query.expand(Vec3f{pos.x + radius, pos.y + radius, pos.z + height});
        collect(query, hits);
        if (hits.empty()) break;

        // Deepest contact wins this pass. Applying only one and re-querying is
        // what lets a corner where two walls meet leave the capsule in the gap
        // rather than shoved out through one of them.
        float deepest = 0.0f;
        Vec3f push{0.0f, 0.0f, 0.0f};
        for (uint32_t ti : hits) {
            const Triangle& t = m_triangles[ti];
            const Vec3f a{pos.x, pos.y, lo.z};
            const Vec3f b{pos.x, pos.y, hi.z};
            // Closest point pair between the segment and the triangle. Three
            // refinement rounds from the ends and the middle converge on the
            // pair for the surfaces a player actually touches; anything the
            // sweep misses only leaves a contact for the next frame.
            Vec3f seg = mul(add(a, b), 0.5f);
            Vec3f onTri = t.a;
            for (int iter = 0; iter < 3; iter++) {
                onTri = closestOnTriangle(t, seg);
                seg = clampToSegment(onTri, a, b);
            }
            onTri = closestOnTriangle(t, seg);
            const Vec3f delta = sub(seg, onTri);
            const float dist = lengthOf(delta);
            if (dist >= radius) continue;
            const float depth = radius - dist;
            if (depth <= deepest) continue;
            deepest = depth;
            if (dist > 1e-5f) {
                push = mul(delta, 1.0f / dist);
            } else {
                // Dead centre: fall back to the face normal so there is a
                // direction to leave by.
                const Vec3f n = cross3(sub(t.b, t.a), sub(t.c, t.a));
                push = lengthOf(n) > 1e-5f ? mul(n, 1.0f / lengthOf(n)) : Vec3f{0.0f, 0.0f, 1.0f};
            }
        }
        if (deepest <= 0.0f) break;
        pos = add(pos, mul(push, deepest + 1e-3f));
        resolved++;
    }
    return resolved;
}

bool CollisionCell::groundHeight(float x, float y, float fromZ, float radius,
                                 float& outZ) const {
    if (m_triangles.empty()) return false;
    CollisionBounds query;
    query.expand(Vec3f{x - radius, y - radius, -std::numeric_limits<float>::max()});
    query.expand(Vec3f{x + radius, y + radius, fromZ});
    std::vector<uint32_t> hits;
    collect(query, hits);

    bool found = false;
    float best = -std::numeric_limits<float>::max();
    for (uint32_t ti : hits) {
        const Triangle& t = m_triangles[ti];
        // The whole triangle has to be at or below the search height, or the
        // surface is overhead rather than underfoot.
        if (std::max(std::max(t.a.z, t.b.z), t.c.z) > fromZ) continue;
        float w[3];
        closestBary2D(t, x, y, w);
        const float dx = w[0] * t.a.x + w[1] * t.b.x + w[2] * t.c.x - x;
        const float dy = w[0] * t.a.y + w[1] * t.b.y + w[2] * t.c.y - y;
        if (dx * dx + dy * dy > radius * radius) continue;
        const float z = w[0] * t.a.z + w[1] * t.b.z + w[2] * t.c.z;
        if (z > best) {
            best = z;
            found = true;
        }
    }
    if (!found) return false;
    outZ = best;
    return true;
}

}  // namespace skyrim
