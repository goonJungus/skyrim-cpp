// game.cpp - session setup, cell streaming, player controller, frame loop.

#include "game.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace skyrim::host {
namespace {

// Archives key on lowercase, backslash-separated paths.
std::string normalizePath(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '/') {
            out.push_back('\\');
        } else {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

bool keyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

constexpr float kCellSize = 4096.0f;   // world units per exterior cell
constexpr float kEyeHeight = 120.0f;   // world units (the player is ~128 tall)
constexpr float kPlayerRadius = 40.0f;
constexpr float kPlayerHeight = 128.0f;
constexpr float kStepHeight = 50.0f;
constexpr float kGravity = 800.0f;     // world units / s^2

// The distant-terrain ring is drawn as one mesh, uploaded under this key so a
// rebuild can replace the GPU copy instead of leaking it.
constexpr const char* kFarMeshKey = "far_terrain";

// Stable key for a cell's terrain mesh, from its grid coordinate.
uint64_t gridKey(int gx, int gy) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(gx)) << 32) |
           static_cast<uint64_t>(static_cast<uint32_t>(gy));
}

std::string terrainMeshKey(int gx, int gy) {
    return "terrain_" + std::to_string(gx) + "_" + std::to_string(gy);
}

// Blend layers of a cell's terrain, keyed alongside the base mesh.
std::string terrainLayerKey(int gx, int gy, size_t layer) {
    return "terrain_" + std::to_string(gx) + "_" + std::to_string(gy) + "_l" +
           std::to_string(layer);
}

// World-space AABB of a local AABB under a transform (the box's eight corners).
Aabb transformBounds(const Mat4& m, const Aabb& local) {
    Aabb world;
    if (!local.valid()) return world;
    for (int i = 0; i < 8; i++) {
        const Vec3 corner{(i & 1) != 0 ? local.mx.x : local.mn.x,
                          (i & 2) != 0 ? local.mx.y : local.mn.y,
                          (i & 4) != 0 ? local.mx.z : local.mn.z};
        world.add(transformPoint(m, corner));
    }
    return world;
}

// Bounding sphere that just contains an AABB, for frustum culling.
void boundsToSphere(const Aabb& b, Vec3& center, float& radius) {
    center = Vec3{(b.mn.x + b.mx.x) * 0.5f, (b.mn.y + b.mx.y) * 0.5f,
                  (b.mn.z + b.mx.z) * 0.5f};
    const Vec3 d{b.mx.x - b.mn.x, b.mx.y - b.mn.y, b.mx.z - b.mn.z};
    radius = 0.5f * std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

// Local-space AABB of a mesh, computed fresh. Terrain meshes are rebuilt per
// stream (and the allocator reuses their addresses), so they must not go through
// the pointer-keyed cache that model meshes use.
Aabb localBounds(const Mesh& mesh) {
    Aabb b;
    for (const Vertex& v : mesh.vertices) b.add(Vec3{v.x, v.y, v.z});
    return b;
}

}  // namespace

bool Game::init(const GameOptions& opt, std::string* err) {
    m_opt = opt;
    const auto fail = [&](const std::string& what) {
        std::printf("error: %s\n", what.c_str());
        if (err != nullptr) *err = what;
        return false;
    };

    std::printf("skyrim: loading world data from %s\n", opt.dataDir.c_str());
    SkyrimConfig config;
    config.dataDirectory = opt.dataDir;
    config.plugins = opt.plugins;
    config.loadArchives = false;
    if (m_db.load(config) != Status::Ok) return fail("form database load failed");
    if (m_cells.load(config) != Status::Ok) return fail("cell system load failed");
    std::printf("skyrim: %zu forms, %zu cells, %zu refs\n", m_db.formCount(),
                m_cells.cellCount(), m_cells.totalObjectCount());

    m_resolver.init(&m_db);

    openArchives(opt, err);
    if (m_archives.empty()) return fail("no readable BSA archives found");
    for (auto& a : m_archives) m_meshCache.addArchive(a.get());

    if (!m_renderer.init(opt.width, opt.height, "Skyrim (skyrim-cpp)", err)) {
        return false;
    }

    // Stream the starting cell, then drop the player in from above so the
    // collision settles them onto the geometry.
    const float cx = static_cast<float>(opt.gridX) * kCellSize + kCellSize * 0.5f;
    const float cy = static_cast<float>(opt.gridY) * kCellSize + kCellSize * 0.5f;
    rebuildStream(cx, cy);
    float top = m_floorZ;
    for (const Aabb& b : m_colliders) {
        if (b.valid() && b.mx.z > top) top = b.mx.z;
    }
    float spawnZ = top + 400.0f;
    float terrainZ = 0.0f;
    if (terrainHeightAt(cx, cy, terrainZ)) {
        spawnZ = std::max(spawnZ, terrainZ + 400.0f);
    }
    m_player.pos = Vec3{cx, cy, spawnZ};
    m_player.yaw = 0.0f;
    m_player.pitch = -0.15f;
    if (opt.haveCam) {
        m_player.pos = Vec3{opt.camX, opt.camY, opt.camZ};
        m_player.yaw = opt.lookYawDeg * 3.14159265f / 180.0f;
        m_player.pitch = opt.lookPitchDeg * 3.14159265f / 180.0f;
        rebuildStream(m_player.pos.x, m_player.pos.y);
    }
    std::printf("skyrim: %zu instances, %zu colliders, spawn z=%.0f\n", m_draw.size(),
                m_colliders.size(), m_player.pos.z);
    // Point the fog at the streamed extent: the world then fades into the sky
    // where the streamed cells actually run out, instead of showing a hard edge
    // (range too short) or dissolving into a distant void (range too long).
    // Point the fog at the distant ring, not at the streamed cells: aiming it at
    // the stream would crush the view into a foggy bubble a couple of hundred
    // metres wide and hide the landscape the ring exists to show. Without a ring
    // (interiors, a worldspace with no terrain) fall back to the stream extent.
    if (m_far != nullptr) {
        const float farExtent = static_cast<float>(m_opt.farRadius) * kCellSize;
        m_renderer.setFog(farExtent * 0.35f, farExtent * 1.05f);
    } else {
        const float streamExtent = (static_cast<float>(m_opt.radius) + 0.5f) * kCellSize;
        m_renderer.setFog(streamExtent * 0.45f, streamExtent * 1.1f);
    }
    m_renderer.setExposure(m_opt.exposure);

    reportContent();
    return true;
}

void Game::openArchives(const GameOptions& opt, std::string* /*err*/) {
    std::vector<std::string> paths = opt.archives;
    if (paths.empty()) {
        std::error_code ec;
        std::filesystem::directory_iterator it(opt.dataDir, ec);
        const std::filesystem::directory_iterator end;
        if (!ec) {
            for (; it != end; it.increment(ec)) {
                if (ec) break;
                const std::filesystem::path& p = it->path();
                std::string ext = p.extension().string();
                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext == ".bsa") paths.push_back(p.string());
            }
        }
    }
    std::sort(paths.begin(), paths.end());

    size_t opened = 0;
    for (const std::string& path : paths) {
        auto bsa = std::make_unique<BsaArchive>();
        if (!bsa->open(path)) continue;
        opened += bsa->fileCount();
        m_archives.push_back(std::move(bsa));
    }
    std::printf("skyrim: opened %zu archives, %zu files total\n", m_archives.size(), opened);
}

const Mesh* Game::loadMesh(const std::string& modelPath) {
    return m_meshCache.get(modelPath);
}

IDirect3DTexture9* Game::loadTexture(const std::string& texPath) {
    if (texPath.empty()) return nullptr;
    const std::string key = normalizePath(texPath);
    auto it = m_texCache.find(key);
    if (it != m_texCache.end()) return it->second;
    ++m_texRequested;

    // NIF paths carry the "textures\" root; land-texture paths (TX00) do not,
    // so try both spellings against the archives.
    std::vector<std::string> candidates{key};
    if (key.compare(0, 9, "textures\\") != 0) candidates.push_back("textures\\" + key);

    IDirect3DTexture9* result = nullptr;
    for (auto& bsa : m_archives) {
        for (const std::string& cand : candidates) {
            if (!bsa->has(cand)) continue;
            const std::vector<uint8_t> data = bsa->read(cand);
            if (data.empty()) continue;
            Image img;
            if (decodeDDS(data.data(), data.size(), img, nullptr) && img.valid()) {
                // Remember the mean colour of the texture. The distant terrain
                // paints itself with these instead of the textures themselves,
                // so a mountain two kilometres away still reads as the same
                // ground the streamed cells show in detail.
                const size_t px = img.rgba.size() / 4;
                if (px > 0) {
                    double sr = 0.0, sg = 0.0, sb = 0.0;
                    for (size_t p = 0; p < px; p++) {
                        sr += img.rgba[p * 4 + 0];
                        sg += img.rgba[p * 4 + 1];
                        sb += img.rgba[p * 4 + 2];
                    }
                    const double inv = 1.0 / (static_cast<double>(px) * 255.0);
                    m_texColor[key] = {static_cast<float>(sr * inv),
                                       static_cast<float>(sg * inv),
                                       static_cast<float>(sb * inv)};
                }
                result = m_renderer.uploadTexture(key, img);
                break;
            }
        }
        if (result != nullptr) break;
    }
    if (result != nullptr) {
        ++m_texLoaded;
    } else {
        ++m_texMissing;
        if (m_missingSample.size() < 16) m_missingSample.push_back(key);
    }
    m_texCache.emplace(key, result);
    return result;
}

bool Game::meshBounds(const Mesh* mesh, Aabb& out) {
    if (mesh == nullptr) return false;
    auto it = m_meshBounds.find(mesh);
    if (it == m_meshBounds.end()) {
        Aabb local;
        for (const Vertex& v : mesh->vertices) local.add(Vec3{v.x, v.y, v.z});
        it = m_meshBounds.emplace(mesh, local).first;
    }
    out = it->second;
    return out.valid();
}

void Game::reportContent() const {
    // Count submeshes the frame will actually draw, split by whether they got a
    // real texture or fall through to the flat fallback colour, and tally the
    // material features that decide how they are shaded.
    size_t subTextured = 0, subFlat = 0, subEmpty = 0, subVertexColor = 0, subBlend = 0;
    size_t subTest = 0;
    size_t blendedItems = 0, terrainItems = 0, terrainLayerItems = 0, blendedModels = 0;
    size_t subColors = 0;
    double tintSum = 0.0;
    size_t tintCount = 0, tintDark = 0, tintNearWhite = 0;
    for (const DrawItem& d : m_draw) {
        if (d.mesh == nullptr) continue;
        if (d.blend) ++blendedItems;
        if (d.terrain) {
            ++terrainItems;
            if (d.blend) ++terrainLayerItems;
        } else if (d.blend) {
            ++blendedModels;
        }
        for (size_t i = 0; i < d.mesh->submeshes.size(); i++) {
            const Submesh& sm = d.mesh->submeshes[i];
            if (sm.indexCount == 0) continue;
            if (sm.vertexColors != 0) ++subVertexColor;
            if (sm.colorsPresent != 0) ++subColors;
            if (sm.colorsPresent != 0) {
                const double t = (static_cast<double>(sm.fallbackR) + sm.fallbackG +
                                  sm.fallbackB) / 3.0;
                tintSum += t;
                ++tintCount;
                if (t < 0.55) ++tintDark;
                if (t >= 0.99) ++tintNearWhite;
            }
            if (sm.alphaBlend != 0) ++subBlend;
            if (sm.alphaTest != 0) ++subTest;
            const bool haveTex = i < d.textures.size() && d.textures[i] != nullptr;
            if (haveTex) {
                ++subTextured;
            } else if (sm.texturePath.empty()) {
                ++subEmpty;
            } else {
                ++subFlat;
            }
        }
    }
    std::printf("skyrim: textures %zu/%zu resolved (%zu missing), submeshes %zu textured, "
                "%zu untextured (no path), %zu failed to load\n",
                m_texLoaded, m_texRequested, m_texMissing, subTextured, subEmpty, subFlat);
    std::printf("skyrim: %zu instances (%zu blended: %zu terrain layers + %zu models), "
                "%zu submeshes carry vertex colour (%zu applied), %zu blend, %zu alpha-test\n",
                m_draw.size(), blendedItems, terrainLayerItems, blendedModels, subColors,
                subVertexColor, subBlend, subTest);
    std::printf("skyrim: %zu terrain items over %zu cached cells\n", terrainItems,
                m_terrain.size());
    if (tintCount > 0) {
        std::printf("skyrim: vertex-colour tints: mean %.3f over %zu, %zu dark(<0.55), "
                    "%zu white(>=0.99)\n",
                    tintSum / static_cast<double>(tintCount), tintCount, tintDark, tintNearWhite);
    }
    // Only the shader flags the host does something with (or is about to) are
    // worth a line; the full 16-bit map was all zeroes with a handful of hits.
    size_t flagSkinned = 0, flagVertexAlpha = 0, flagSpecular = 0, flagEnvMap = 0,
           flagRefraction = 0;
    for (const DrawItem& d : m_draw) {
        if (d.mesh == nullptr) continue;
        for (const Submesh& sm : d.mesh->submeshes) {
            if (sm.indexCount == 0) continue;
            if (sm.shaderFlags1 & (1u << 1)) ++flagSkinned;
            if (sm.shaderFlags1 & (1u << 3)) ++flagVertexAlpha;
            if (sm.shaderFlags1 & (1u << 0)) ++flagSpecular;
            if (sm.shaderFlags1 & (1u << 7)) ++flagEnvMap;
            if (sm.shaderFlags1 & (1u << 15)) ++flagRefraction;
        }
    }
    std::printf("skyrim: shader flags: %zu specular, %zu skinned, %zu vertex-alpha, "
                "%zu env-map, %zu refraction\n",
                flagSpecular, flagSkinned, flagVertexAlpha, flagEnvMap, flagRefraction);
    for (const std::string& s : m_missingSample) {
        std::printf("skyrim:   missing texture: %s\n", s.c_str());
    }
}

void Game::buildCell(int gx, int gy, std::vector<DrawItem>& out) {
    const Cell* cell = m_cells.findCell(m_opt.worldspace, gx, gy);
    if (cell == nullptr) return;

    // Terrain first, so it sits under everything placed on it.
    if (cell->land.present && !cell->isInterior) {
        auto& slot = m_terrain[gridKey(gx, gy)];
        if (!slot) {
            auto mesh = std::make_unique<Mesh>();
            if (buildTerrainMesh(*cell, m_resolver, *mesh) && mesh->valid()) {
                slot = std::move(mesh);
            }
        }
        if (slot) {
            Renderer::GpuMesh* gpu = m_renderer.uploadMesh(terrainMeshKey(gx, gy), *slot);
            if (gpu != nullptr) {
                DrawItem item;
                item.mesh = slot.get();
                item.gpu = gpu;
                item.world = Mat4{};   // world-space vertices; identity transform
                item.terrain = true;
                item.textures.reserve(slot->submeshes.size());
                for (const Submesh& sm : slot->submeshes) {
                    item.textures.push_back(loadTexture(sm.texturePath));
                }
                item.bounds = localBounds(*item.mesh);
                boundsToSphere(item.bounds, item.center, item.radius);
                out.push_back(std::move(item));
            }
        }

        // Blend layers over the base so the ground fades between land textures
        // instead of switching hard at a quadrant border.
        auto& layers = m_terrainLayers[gridKey(gx, gy)];
        if (layers.empty()) {
            std::vector<Mesh> built;
            if (buildTerrainLayers(*cell, m_resolver, built)) {
                layers.reserve(built.size());
                for (Mesh& m : built) {
                    layers.push_back(std::make_unique<Mesh>(std::move(m)));
                }
            }
        }
        for (size_t k = 0; k < layers.size(); k++) {
            const Mesh* lm = layers[k].get();
            Renderer::GpuMesh* gpu = m_renderer.uploadMesh(terrainLayerKey(gx, gy, k), *lm);
            if (gpu == nullptr) continue;
            DrawItem item;
            item.mesh = lm;
            item.gpu = gpu;
            item.terrain = true;
            item.blend = true;
            if (!lm->submeshes.empty()) {
                item.textures.push_back(loadTexture(lm->submeshes[0].texturePath));
            }
            item.bounds = localBounds(*lm);
            boundsToSphere(item.bounds, item.center, item.radius);
            out.push_back(std::move(item));
        }
    }

    const std::vector<PlacedModel> inst = instancesForCell(*cell, m_resolver);
    for (const PlacedModel& m : inst) {
        const Mesh* mesh = loadMesh(m.modelPath);
        if (mesh == nullptr) continue;
        Renderer::GpuMesh* gpu = m_renderer.uploadMesh(normalizePath(m.modelPath), *mesh);
        if (gpu == nullptr) continue;
        DrawItem item;
        item.mesh = mesh;
        item.gpu = gpu;
        item.world = instanceMatrix(m.x, m.y, m.z, m.rotX, m.rotY, m.rotZ, m.scale);
        item.textures.reserve(mesh->submeshes.size());
        for (const Submesh& sm : mesh->submeshes) {
            item.textures.push_back(loadTexture(sm.texturePath));
            if (sm.alphaBlend != 0) item.blend = true;
        }
        Aabb local;
        if (meshBounds(mesh, local)) {
            item.bounds = transformBounds(item.world, local);
            boundsToSphere(item.bounds, item.center, item.radius);
        }
        out.push_back(std::move(item));
    }
}

void Game::buildFarMesh(int gx, int gy) {
    // Rebuilt on every stream: the ring's hole has to follow the streamed cells,
    // and a few thousand height samples cost nothing next to the frame it saves.
    m_renderer.releaseMesh(kFarMeshKey);
    m_far.reset();
    if (m_opt.farRadius <= 0) return;   // ring switched off (--far 0)

    FarTerrainOptions opt;
    opt.radiusCells = m_opt.farRadius;
    opt.holeRadiusCells = m_opt.radius;
    opt.landAt = [this](int cx, int cy) -> const Land* {
        const Cell* cell = m_cells.findCell(m_opt.worldspace, cx, cy);
        if (cell == nullptr || cell->isInterior || !cell->land.present) return nullptr;
        return &cell->land;
    };
    opt.colorOf = [this](const std::string& path, float& r, float& g, float& b) {
        loadTexture(path);   // resolves and decodes it the first time it is seen
        const auto it = m_texColor.find(normalizePath(path));
        if (it == m_texColor.end()) return false;
        r = it->second[0];
        g = it->second[1];
        b = it->second[2];
        return true;
    };

    auto mesh = std::make_unique<Mesh>();
    if (!buildFarTerrain(gx, gy, m_resolver, opt, *mesh) || !mesh->valid()) return;
    Renderer::GpuMesh* gpu = m_renderer.uploadMesh(kFarMeshKey, *mesh);
    if (gpu == nullptr) return;
    m_far = std::move(mesh);

    DrawItem item;
    item.mesh = m_far.get();
    item.gpu = gpu;
    item.terrain = true;    // ground, and never a collider: it spans kilometres
    item.textures.resize(m_far->submeshes.size(), nullptr);
    item.bounds = localBounds(*item.mesh);
    boundsToSphere(item.bounds, item.center, item.radius);
    m_draw.push_back(std::move(item));
}

void Game::rebuildStream(float centerX, float centerY) {
    const auto t0 = std::chrono::steady_clock::now();
    m_draw.clear();
    m_terrain.clear();
    m_terrainLayers.clear();
    const int gx = static_cast<int>(std::floor(centerX / kCellSize));
    const int gy = static_cast<int>(std::floor(centerY / kCellSize));
    m_centerGx = gx;
    m_centerGy = gy;
    // The ring goes in first: it is the view to the horizon, and drawing it
    // before the streamed cells keeps the near detail on top of it.
    buildFarMesh(gx, gy);
    const auto t1 = std::chrono::steady_clock::now();
    for (int dy = -m_opt.radius; dy <= m_opt.radius; dy++) {
        for (int dx = -m_opt.radius; dx <= m_opt.radius; dx++) {
            buildCell(gx + dx, gy + dy, m_draw);
        }
    }
    updateColliders();
    const auto t2 = std::chrono::steady_clock::now();
    // A stream rebuild happens whenever the player crosses a cell, so its cost
    // is the hitch they feel while walking. Keep it in sight.
    const double farMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double cellMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::printf("skyrim: stream at (%d %d): %d cells in %.1f ms (far ring %.1f ms, "
                "%zu items)\n",
                gx, gy, (2 * m_opt.radius + 1) * (2 * m_opt.radius + 1), farMs + cellMs,
                farMs, m_draw.size());
}

void Game::updateColliders() {
    m_colliders.clear();
    m_colliders.reserve(m_draw.size());
    bool have = false;
    float floorZ = 0.0f;
    // buildCell already worked out each item's world bounds, so this is just a
    // gather rather than another eight-corner transform per instance.
    for (const DrawItem& d : m_draw) {
        if (d.terrain) continue;   // terrain is a surface, not a box
        if (!d.bounds.valid()) continue;
        m_colliders.push_back(d.bounds);
        if (!have || d.bounds.mn.z < floorZ) {
            floorZ = d.bounds.mn.z;
            have = true;
        }
    }
    m_floorZ = have ? floorZ : 0.0f;
}

Vec3 Game::cameraForward() const {
    const float cp = std::cos(m_player.pitch);
    const float sp = std::sin(m_player.pitch);
    return Vec3{cp * std::cos(m_player.yaw), cp * std::sin(m_player.yaw), sp};
}

Vec3 Game::cameraEye() const {
    return Vec3{m_player.pos.x, m_player.pos.y, m_player.pos.z + kEyeHeight};
}

bool Game::terrainHeightAt(float x, float y, float& outZ) const {
    const int gx = static_cast<int>(std::floor(x / kCellSize));
    const int gy = static_cast<int>(std::floor(y / kCellSize));
    const Cell* cell = m_cells.findCell(m_opt.worldspace, gx, gy);
    if (cell == nullptr || cell->isInterior || !cell->land.present) return false;

    float h[kLandVertices];
    cell->land.heights(h);

    // Bilinear sample of the 33x33 vertex grid; clamp so edge points stay in.
    const float fx = (x - cell->worldMinX) / kLandVertexSpacing;
    const float fy = (y - cell->worldMinY) / kLandVertexSpacing;
    int col = static_cast<int>(std::floor(fx));
    int row = static_cast<int>(std::floor(fy));
    if (col < 0) col = 0;
    if (col > kLandEdge - 2) col = kLandEdge - 2;
    if (row < 0) row = 0;
    if (row > kLandEdge - 2) row = kLandEdge - 2;
    const float tx = fx - static_cast<float>(col);
    const float ty = fy - static_cast<float>(row);

    const float h00 = h[row * kLandEdge + col];
    const float h10 = h[row * kLandEdge + col + 1];
    const float h01 = h[(row + 1) * kLandEdge + col];
    const float h11 = h[(row + 1) * kLandEdge + col + 1];
    const float a = h00 + (h10 - h00) * tx;
    const float b = h01 + (h11 - h01) * tx;
    outZ = a + (b - a) * ty;
    return true;
}

void Game::updatePlayer(float dt) {
    const Vec3 fwdH{std::cos(m_player.yaw), std::sin(m_player.yaw), 0.0f};
    const Vec3 right = normalize(cross(Vec3{0.0f, 0.0f, 1.0f}, fwdH));

    if (m_player.flying) {
        const Vec3 fwd = cameraForward();
        Vec3 move{0.0f, 0.0f, 0.0f};
        if (keyDown('W')) move = move + fwd;
        if (keyDown('S')) move = move - fwd;
        if (keyDown('D')) move = move + right;
        if (keyDown('A')) move = move - right;
        if (keyDown(VK_SPACE)) move.z += 1.0f;
        if (keyDown(VK_CONTROL)) move.z -= 1.0f;
        const float speed = keyDown(VK_SHIFT) ? 3000.0f : 1200.0f;
        m_player.pos = m_player.pos + normalize(move) * (speed * dt);
        m_player.vel = Vec3{0.0f, 0.0f, 0.0f};
        m_player.grounded = false;
        return;
    }

    const float speed = keyDown(VK_SHIFT) ? 700.0f : 320.0f;
    Vec3 wish{0.0f, 0.0f, 0.0f};
    if (keyDown('W')) wish = wish + fwdH;
    if (keyDown('S')) wish = wish - fwdH;
    if (keyDown('D')) wish = wish + right;
    if (keyDown('A')) wish = wish - right;

    // Horizontal collide-and-slide against the streamed AABBs.
    Vec3 np = m_player.pos;
    const Vec3 horiz = normalize(wish) * (speed * dt);
    np.x += horiz.x;
    np.y += horiz.y;
    const float bodyLo = m_player.pos.z;
    const float bodyHi = m_player.pos.z + kPlayerHeight;
    for (const Aabb& b : m_colliders) {
        if (b.mx.z <= bodyLo + 1.0f) continue;   // low enough to step over
        if (b.mn.z >= bodyHi) continue;          // entirely above the head
        const float bx0 = b.mn.x - kPlayerRadius, bx1 = b.mx.x + kPlayerRadius;
        const float by0 = b.mn.y - kPlayerRadius, by1 = b.mx.y + kPlayerRadius;
        if (np.x <= bx0 || np.x >= bx1 || np.y <= by0 || np.y >= by1) continue;
        const float px0 = np.x - bx0, px1 = bx1 - np.x;
        const float py0 = np.y - by0, py1 = by1 - np.y;
        if (std::min(px0, px1) < std::min(py0, py1)) {
            np.x += (px0 < px1) ? -px0 : px1;
        } else {
            np.y += (py0 < py1) ? -py0 : py1;
        }
    }
    m_player.pos.x = np.x;
    m_player.pos.y = np.y;

    // Vertical: gravity, then settle onto the highest surface under the feet.
    m_player.vel.z -= kGravity * dt;
    m_player.pos.z += m_player.vel.z * dt;

    float ground = -1e30f;
    for (const Aabb& b : m_colliders) {
        if (m_player.pos.x <= b.mn.x - kPlayerRadius || m_player.pos.x >= b.mx.x + kPlayerRadius) continue;
        if (m_player.pos.y <= b.mn.y - kPlayerRadius || m_player.pos.y >= b.mx.y + kPlayerRadius) continue;
        if (b.mx.z <= m_player.pos.z + kStepHeight && b.mx.z > ground) ground = b.mx.z;
    }
    // Terrain underfoot: the heightmap is the real floor in the open world. It
    // counts as ground while its surface is within step height of the feet, so
    // the player climbs slopes instead of clipping through them.
    float terrainZ = 0.0f;
    if (terrainHeightAt(m_player.pos.x, m_player.pos.y, terrainZ) &&
        terrainZ <= m_player.pos.z + kStepHeight && terrainZ > ground) {
        ground = terrainZ;
    }
    m_player.grounded = false;
    if (ground > -1e29f && m_player.pos.z <= ground) {
        m_player.pos.z = ground;
        m_player.vel.z = 0.0f;
        m_player.grounded = true;
    }
    if (m_player.pos.z < m_floorZ - 400.0f) {
        m_player.pos.z = m_floorZ + 400.0f;
        m_player.vel.z = 0.0f;
    }
    if (m_player.grounded && keyDown(VK_SPACE)) {
        m_player.vel.z = 320.0f;
        m_player.grounded = false;
    }
}

int Game::run() {
    using clock = std::chrono::steady_clock;
    auto prev = clock::now();
    int frame = 0;

    while (m_renderer.pumpMessages()) {
        const auto now = clock::now();
        float dt = std::chrono::duration<float>(now - prev).count();
        prev = now;
        if (dt > 0.1f) dt = 0.1f;

        // Mouse look: measure the cursor's offset from the window centre and
        // re-centre it, which needs no raw-input plumbing and works windowed.
        // Only while the window is foreground, so it never fights the pointer
        // when the player alt-tabs away; and skipped entirely in bounded runs.
        POINT pt;
        if (m_opt.maxFrames <= 0 && GetForegroundWindow() == m_renderer.hwnd() &&
            GetCursorPos(&pt) != 0) {
            RECT rc;
            GetClientRect(m_renderer.hwnd(), &rc);
            POINT tl{rc.left, rc.top};
            ClientToScreen(m_renderer.hwnd(), &tl);
            const int cx = tl.x + (rc.right - rc.left) / 2;
            const int cy = tl.y + (rc.bottom - rc.top) / 2;
            m_player.yaw += static_cast<float>(pt.x - cx) * 0.0022f;
            m_player.pitch -= static_cast<float>(pt.y - cy) * 0.0022f;
            const float lim = 1.5f;
            if (m_player.pitch > lim) m_player.pitch = lim;
            if (m_player.pitch < -lim) m_player.pitch = -lim;
            SetCursorPos(cx, cy);
        }

        const bool f = keyDown('F');
        if (f && !m_prevF) {
            m_player.flying = !m_player.flying;
            m_player.vel = Vec3{0.0f, 0.0f, 0.0f};
            std::printf("skyrim: %s\n", m_player.flying ? "fly mode" : "walk mode");
        }
        m_prevF = f;

        if (keyDown(VK_ESCAPE)) break;   // quit

        if (!m_opt.haveCam) {
            updatePlayer(dt);

            const int gx = static_cast<int>(std::floor(m_player.pos.x / kCellSize));
            const int gy = static_cast<int>(std::floor(m_player.pos.y / kCellSize));
            if (gx != m_centerGx || gy != m_centerGy) {
                rebuildStream(m_player.pos.x, m_player.pos.y);
            }
        }

        if (m_renderer.beginFrame()) {
            const auto renderT0 = clock::now();
            const float aspect =
                static_cast<float>(m_renderer.width()) / static_cast<float>(m_renderer.height());
            const float fov = m_opt.fovDeg * 3.14159265f / 180.0f;
            const Mat4 proj = perspectiveFovLH(fov, aspect, 5.0f, 150000.0f);
            const Vec3 eye = cameraEye();
            const Mat4 view = lookAtLH(eye, eye + cameraForward(), Vec3{0.0f, 0.0f, 1.0f});
            m_renderer.setCamera(view, proj);
            m_renderer.drawSky(eye);

            // Cull to the view frustum, then split into the opaque pass and the
            // blended pass. Opaque geometry goes first so terrain layers and
            // translucent materials blend over finished ground; blended items are
            // sorted back-to-front because they share the depth buffer.
            const Frustum frustum = frustumFromViewProj(multiply(view, proj));
            m_opaque.clear();
            m_blended.clear();
            for (DrawItem& d : m_draw) {
                if (!sphereInFrustum(frustum, d.center, d.radius)) continue;
                if (d.blend) {
                    m_blended.push_back(&d);
                } else {
                    m_opaque.push_back(&d);
                }
            }
            std::sort(m_blended.begin(), m_blended.end(),
                      [&](const DrawItem* a, const DrawItem* b) {
                          const Vec3 da = a->center - eye;
                          const Vec3 db = b->center - eye;
                          return dot(da, da) > dot(db, db);
                      });
            for (const DrawItem* d : m_opaque) {
                m_renderer.drawMesh(d->gpu, *d->mesh, d->world, d->textures);
            }
            for (const DrawItem* d : m_blended) {
                m_renderer.drawMesh(d->gpu, *d->mesh, d->world, d->textures);
            }
            m_lastVisible = m_opaque.size() + m_blended.size();
            m_frameMs +=
                std::chrono::duration<double, std::milli>(clock::now() - renderT0).count();
            ++m_frameCount;
            m_renderer.endScene();

            ++frame;
            const bool lastFrame = m_opt.maxFrames > 0 && frame >= m_opt.maxFrames;
            if (lastFrame && !m_opt.shotPath.empty()) {
                if (m_renderer.saveScreenshot(m_opt.shotPath)) {
                    std::printf("skyrim: wrote %s\n", m_opt.shotPath.c_str());
                } else {
                    std::printf("skyrim: failed to write %s\n", m_opt.shotPath.c_str());
                }
            }
            m_renderer.present();
        }

        if (m_opt.maxFrames > 0 && frame >= m_opt.maxFrames) break;
    }
    if (m_opt.maxFrames > 0) {
        const double avgMs = m_frameCount > 0 ? m_frameMs / static_cast<double>(m_frameCount) : 0.0;
        std::printf("skyrim: rendered %d frames, %zu items visible, %.2f ms/frame (%.0f fps), "
                    "%zu draw calls\n",
                    frame, m_lastVisible, avgMs,
                    avgMs > 0.0 ? 1000.0 / avgMs : 0.0, m_renderer.drawCalls());
    }
    return 0;
}

void Game::shutdown() {
    m_draw.clear();
    m_terrain.clear();
    m_terrainLayers.clear();
    m_opaque.clear();
    m_blended.clear();
    m_colliders.clear();
    m_texCache.clear();
    m_meshBounds.clear();
    m_renderer.shutdown();
    m_archives.clear();
}

}  // namespace skyrim::host
