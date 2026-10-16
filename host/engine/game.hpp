// game.hpp - the playable slice: load the world, stream cells, drive a player.
//
// The game owns everything the host needs for a session and nothing more: the
// library's world data (form database, cells, archives), a mesh cache and a
// texture cache on top of them, and a first-person player with gravity and
// capsule collision against the streamed geometry.
//
// Rendering is delegated to Renderer; the library never sees a graphics API.
//
// Windows-only (it owns a window and a D3D9 device).

#pragma once

#ifdef _WIN32

#include <memory>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "d3d9_renderer.hpp"
#include "math.hpp"
#include "skyrim/bsa_archive.hpp"
#include "skyrim/cell_system.hpp"
#include "skyrim/collision.hpp"
#include "skyrim/form_database.hpp"
#include "skyrim/mesh.hpp"
#include "skyrim/skyrim_module.hpp"
#include "skyrim/world_render.hpp"

namespace skyrim::host {

struct GameOptions {
    std::string dataDir;                 // user's Data folder (never bundled)
    std::vector<std::string> plugins;    // extra plugins
    std::vector<std::string> archives;   // explicit .bsa list; empty = scan dataDir
    FormID worldspace = 0x0000003C;      // Tamriel
    int gridX = 0;                       // starting cell grid coordinate
    int gridY = 0;
    int radius = 4;                      // cells streamed around the player
    int farRadius = 32;                  // cells out to the distant terrain; 0 = off
    int width = 1280;
    int height = 720;
    int maxFrames = 0;                   // 0 = run until closed; >0 = bounded
                                         // smoke test (skips input, grabs no cursor)
    // Screenshot/inspection aids (all optional).
    std::string shotPath;                // save the final frame here (24-bit BMP)
    bool haveCam = false;                // place the camera instead of spawning
    float camX = 0.0f, camY = 0.0f, camZ = 0.0f;
    float lookYawDeg = 0.0f, lookPitchDeg = 0.0f;
    float fovDeg = 68.0f;                // vertical field of view
    float exposure = 1.8f;               // post-lighting gain (1..2)
};

class Game {
public:
    bool init(const GameOptions& opt, std::string* err);
    int run();
    void shutdown();

private:
    // One streamed instance ready to draw.
    struct DrawItem {
        const Mesh* mesh = nullptr;
        Renderer::GpuMesh* gpu = nullptr;
        Mat4 world;
        std::vector<IDirect3DTexture9*> textures;
        bool terrain = false;   // terrain surface (excluded from box collision)
        bool blend = false;     // drawn in the back-to-front transparent pass
        Vec3 center;            // world-space bounding-sphere centre
        float radius = 0.0f;    // bounding-sphere radius
        Aabb bounds;            // world-space AABB, for frustum culling
    };

    // Player, in world units (1 unit ~ 1.43 cm; the player is ~128 units tall).
    struct Player {
        Vec3 pos{0.0f, 0.0f, 0.0f};
        Vec3 vel{0.0f, 0.0f, 0.0f};
        float yaw = 0.0f;      // about world +Z
        float pitch = 0.0f;    // about the camera's right axis
        bool grounded = false;
        bool flying = false;
    };

    void openArchives(const GameOptions& opt, std::string* err);
    void rebuildStream(float centerX, float centerY);
    void buildCell(int gx, int gy, std::vector<DrawItem>& out);

    // Rebuilds the distant-terrain ring around a cell. It is the view to the
    // horizon: one coarse mesh drawn in a single call, sampled from the same
    // heightmaps the streamed cells use so the two meet without a seam.
    void buildFarMesh(int gx, int gy);

    const Mesh* loadMesh(const std::string& modelPath);
    IDirect3DTexture9* loadTexture(const std::string& texPath);

    // Cached local-space bounds of a mesh; false when it has no vertices.
    bool meshBounds(const Mesh* mesh, Aabb& out);

    // Prints how much of the streamed world resolved to real textures, so a
    // washed-out frame can be told apart from a missing-texture one.
    void reportContent() const;

    void updatePlayer(float dt);

    // Builds the BVH of every collision cell that has new geometry in it, drops
    // the ones that have streamed out of range, and reports what it cost. Called
    // once per stream rebuild: a cell is built when it is first seen and then
    // kept, so walking back and forth across a cell builds nothing.
    void buildCollision();

    // Pushes the player's capsule out of the streamed geometry. Returns the
    // number of contacts resolved.
    int resolvePlayerCollision(Vec3f& feet) const;

    // The highest streamed surface at or below fromZ under (x, y), searching the
    // cells around that point. False when there is nothing but terrain there.
    bool groundHeightNear(float x, float y, float fromZ, float& outZ) const;

    Vec3 cameraEye() const;
    Vec3 cameraForward() const;

    // Ground height of the streamed terrain under a world position, if any
    // exterior cell with a heightmap covers it.
    bool terrainHeightAt(float x, float y, float& outZ) const;

    GameOptions m_opt;
    Renderer m_renderer;
    FormDatabase m_db;
    CellSystem m_cells;
    std::vector<std::unique_ptr<BsaArchive>> m_archives;
    ModelResolver m_resolver;
    MeshCache m_meshCache;

    std::unordered_map<const Mesh*, Aabb> m_meshBounds;   // mesh -> local bounds
    std::unordered_map<std::string, IDirect3DTexture9*> m_texCache;
    // Mean colour of each texture seen so far, for the distant terrain: it shows
    // ground colour without drawing the landscape textures at all.
    std::unordered_map<std::string, std::array<float, 3>> m_texColor;
    // Content diagnostics (see reportContent()).
    size_t m_texRequested = 0;
    size_t m_texLoaded = 0;
    size_t m_texMissing = 0;
    std::vector<std::string> m_missingSample;
    // Per-cell terrain meshes, rebuilt per stream and uploaded once per cell
    // (the renderer caches GPU meshes by key, so revisits cost nothing).
    std::unordered_map<uint64_t, std::unique_ptr<Mesh>> m_terrain;
    // Per-cell terrain blend layers, keyed the same way. One vector per cell.
    std::unordered_map<uint64_t, std::vector<std::unique_ptr<Mesh>>> m_terrainLayers;
    // The distant-terrain ring, rebuilt as the player crosses cells, plus the key
    // its GPU copy is uploaded under so it can be replaced.
    std::unique_ptr<Mesh> m_far;
    std::vector<DrawItem> m_draw;
    std::vector<DrawItem*> m_opaque;    // per-frame draw lists, frustum culled
    std::vector<DrawItem*> m_blended;
    // Collision triangles per streamed cell, keyed by grid coordinate. A cell's
    // geometry depends only on the models placed in it, so it survives a stream
    // rebuild and is only built the first time the player walks into it. The far
    // ring and the terrain are absent on purpose: the ring spans kilometres and
    // the terrain has its own heightmap, which is a better floor than its
    // triangles would be.
    std::unordered_map<uint64_t, CollisionCell> m_collision;
    float m_floorZ = 0.0f;
    double m_frameMs = 0.0;             // accumulated render time (diagnostics)
    size_t m_frameCount = 0;
    size_t m_lastVisible = 0;

    Player m_player;
    int m_centerGx = 1 << 30;   // forces a stream on the first frame
    int m_centerGy = 1 << 30;
    bool m_prevSpace = false;
    bool m_prevF = false;
    bool m_quit = false;
};

}  // namespace skyrim::host

#endif  // _WIN32
