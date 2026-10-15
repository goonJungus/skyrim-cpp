// game.hpp - the playable slice: load the world, stream cells, drive a player.
//
// The game owns everything the host needs for a session and nothing more: the
// library's world data (form database, cells, archives), a mesh cache and a
// texture cache on top of them, and a first-person player with gravity and
// axis-aligned collision against the streamed geometry.
//
// Rendering is delegated to Renderer; the library never sees a graphics API.
//
// Windows-only (it owns a window and a D3D9 device).

#pragma once

#ifdef _WIN32

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "d3d9_renderer.hpp"
#include "math.hpp"
#include "skyrim/bsa_archive.hpp"
#include "skyrim/cell_system.hpp"
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
    int radius = 1;                      // cells streamed around the player
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

    const Mesh* loadMesh(const std::string& modelPath);
    IDirect3DTexture9* loadTexture(const std::string& texPath);

    // Prints how much of the streamed world resolved to real textures, so a
    // washed-out frame can be told apart from a missing-texture one.
    void reportContent() const;

    void updatePlayer(float dt);
    void updateColliders();
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
    // Content diagnostics (see reportContent()).
    size_t m_texRequested = 0;
    size_t m_texLoaded = 0;
    size_t m_texMissing = 0;
    std::vector<std::string> m_missingSample;
    // Per-cell terrain meshes, rebuilt per stream and uploaded once per cell
    // (the renderer caches GPU meshes by key, so revisits cost nothing).
    std::unordered_map<uint64_t, std::unique_ptr<Mesh>> m_terrain;
    std::vector<DrawItem> m_draw;
    std::vector<Aabb> m_colliders;
    float m_floorZ = 0.0f;

    Player m_player;
    int m_centerGx = 1 << 30;   // forces a stream on the first frame
    int m_centerGy = 1 << 30;
    bool m_prevSpace = false;
    bool m_prevF = false;
    bool m_quit = false;
};

}  // namespace skyrim::host

#endif  // _WIN32
