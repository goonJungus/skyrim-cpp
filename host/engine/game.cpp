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
    m_player.pos = Vec3{cx, cy, top + 400.0f};
    m_player.yaw = 0.0f;
    m_player.pitch = -0.15f;
    std::printf("skyrim: %zu instances, %zu colliders, spawn z=%.0f\n", m_draw.size(),
                m_colliders.size(), m_player.pos.z);
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

    IDirect3DTexture9* result = nullptr;
    for (auto& bsa : m_archives) {
        if (!bsa->has(key)) continue;
        const std::vector<uint8_t> data = bsa->read(key);
        if (data.empty()) continue;
        Image img;
        if (decodeDDS(data.data(), data.size(), img, nullptr) && img.valid()) {
            result = m_renderer.uploadTexture(key, img);
            break;
        }
    }
    m_texCache.emplace(key, result);
    return result;
}

void Game::buildCell(int gx, int gy, std::vector<DrawItem>& out) {
    const Cell* cell = m_cells.findCell(m_opt.worldspace, gx, gy);
    if (cell == nullptr) return;
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
        }
        out.push_back(std::move(item));
    }
}

void Game::rebuildStream(float centerX, float centerY) {
    m_draw.clear();
    const int gx = static_cast<int>(std::floor(centerX / kCellSize));
    const int gy = static_cast<int>(std::floor(centerY / kCellSize));
    m_centerGx = gx;
    m_centerGy = gy;
    for (int dy = -m_opt.radius; dy <= m_opt.radius; dy++) {
        for (int dx = -m_opt.radius; dx <= m_opt.radius; dx++) {
            buildCell(gx + dx, gy + dy, m_draw);
        }
    }
    updateColliders();
}

void Game::updateColliders() {
    m_colliders.clear();
    m_colliders.reserve(m_draw.size());
    bool have = false;
    float floorZ = 0.0f;
    for (const DrawItem& d : m_draw) {
        Aabb local;
        auto it = m_meshBounds.find(d.mesh);
        if (it == m_meshBounds.end()) {
            for (const Vertex& v : d.mesh->vertices) local.add(Vec3{v.x, v.y, v.z});
            it = m_meshBounds.emplace(d.mesh, local).first;
        }
        local = it->second;
        if (!local.valid()) continue;

        Aabb world;
        for (int i = 0; i < 8; i++) {
            const Vec3 corner{(i & 1) != 0 ? local.mx.x : local.mn.x,
                              (i & 2) != 0 ? local.mx.y : local.mn.y,
                              (i & 4) != 0 ? local.mx.z : local.mn.z};
            world.add(transformPoint(d.world, corner));
        }
        if (!world.valid()) continue;
        m_colliders.push_back(world);
        if (!have || world.mn.z < floorZ) {
            floorZ = world.mn.z;
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

        updatePlayer(dt);

        const int gx = static_cast<int>(std::floor(m_player.pos.x / kCellSize));
        const int gy = static_cast<int>(std::floor(m_player.pos.y / kCellSize));
        if (gx != m_centerGx || gy != m_centerGy) {
            rebuildStream(m_player.pos.x, m_player.pos.y);
        }

        if (m_renderer.beginFrame()) {
            const float aspect =
                static_cast<float>(m_renderer.width()) / static_cast<float>(m_renderer.height());
            const Mat4 proj = perspectiveFovLH(1.2f, aspect, 5.0f, 150000.0f);
            const Vec3 eye = cameraEye();
            const Mat4 view = lookAtLH(eye, eye + cameraForward(), Vec3{0.0f, 0.0f, 1.0f});
            m_renderer.setCamera(view, proj);
            for (const DrawItem& d : m_draw) {
                m_renderer.drawMesh(d.gpu, *d.mesh, d.world, d.textures);
            }
            m_renderer.endFrame();
        }

        if (m_opt.maxFrames > 0 && ++frame >= m_opt.maxFrames) break;
    }
    if (m_opt.maxFrames > 0) {
        std::printf("skyrim: rendered %d frames, exiting\n", frame);
    }
    return 0;
}

void Game::shutdown() {
    m_draw.clear();
    m_colliders.clear();
    m_texCache.clear();
    m_meshBounds.clear();
    m_renderer.shutdown();
    m_archives.clear();
}

}  // namespace skyrim::host
