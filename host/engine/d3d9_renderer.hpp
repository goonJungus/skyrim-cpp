// d3d9_renderer.hpp - Win32 window + Direct3D 9 device, GPU caches, drawing.
//
// This is the only place in the project that knows about a graphics API. It
// lives in the host, not the library, so the library stays renderer-free and can
// be reused with a different backend.
//
// The renderer speaks in library types (skyrim::Mesh, skyrim::Image): it uploads
// them once, keyed by model/texture path, and draws them with a caller-supplied
// world matrix. Fixed-function lighting and texture stages are used on purpose:
// they are dependable and keep this file short enough to reason about.
//
// Windows-only. The rest of the engine stubs itself out elsewhere.

#pragma once

#ifdef _WIN32

// Keep Windows.h from defining min/max macros that would break std::min and
// std::max throughout the engine, and trim the headers we do not need.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <d3d9.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "math.hpp"
#include "skyrim/mesh.hpp"
#include "skyrim/texture.hpp"

namespace skyrim::host {

// The vertex layout the renderer uploads: position, normal, a packed diffuse
// colour (the NIF vertex colour for models, the layer opacity for terrain), and
// one texture coordinate. The diffuse stage carries the blend weight, so the
// per-vertex float colour the library produces is packed to ARGB on upload.
constexpr DWORD kVertexFvf =
    D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;

struct RtVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    DWORD diffuse = 0xFFFFFFFFu;
    float u = 0.0f, v = 0.0f;
};

// The sky dome is unlit and vertex-coloured.
constexpr DWORD kSkyFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;

struct SkyVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    DWORD color = 0xFFFFFFFFu;
};

class Renderer {
public:
    bool init(int width, int height, const std::string& title, std::string* err);
    void shutdown();

    // Pumps the Win32 message queue. Returns false once the window is closed.
    bool pumpMessages();

    bool beginFrame();
    // endScene()/present() are split so a screenshot can be grabbed from the
    // back buffer after drawing but before Present() discards it.
    void endScene();
    void present();
    void setCamera(const Mat4& view, const Mat4& proj);

    // Draws the vertex-coloured sky dome centred on `eye`. Call once per frame
    // after setCamera() and before any mesh draws; it leaves the pipe in the
    // normal lit/fogged world state.
    void drawSky(const Vec3& eye);

    // Grabs the back buffer to a 24-bit BMP. Must be called after endScene()
    // and before present(). Returns false if the device or file is unavailable.
    bool saveScreenshot(const std::string& path);

    struct GpuMesh {
        IDirect3DVertexBuffer9* vb = nullptr;
        IDirect3DIndexBuffer9* ib = nullptr;
    };

    // Uploads (once, keyed by `key`) and returns the GPU mesh, or nullptr on
    // failure. The returned pointer is stable until shutdown().
    GpuMesh* uploadMesh(const std::string& key, const Mesh& mesh);

    // Drops a uploaded mesh so a later uploadMesh() with the same key uploads the
    // new geometry. Used by meshes that are rebuilt as the player moves.
    void releaseMesh(const std::string& key);

    // Distance fog range, in world units. The game points this at the streamed
    // extent so the world dissolves into the sky at its real edge rather than
    // ending in a hard line out in the open.
    void setFog(float start, float end);

    // Scalar applied to lit world geometry after the lighting clamp, standing in
    // for the tone-map exposure the original engine runs. The scene's albedo is
    // dark (land textures average 0.15-0.45), so lighting alone cannot lift it
    // past its own albedo; this is what makes daylight read as daylight. Values
    // are clamped to [1, 2]; the sky is deliberately left unexposed.
    void setExposure(float exposure);

    // Draws every submesh of `mesh` at `world`. `textures` is indexed in
    // parallel with mesh.submeshes; a null entry uses a flat white texture.
    // Alpha test / blend come from each submesh's own flags.
    void drawMesh(GpuMesh* gpu, const Mesh& mesh, const Mat4& world,
                  const std::vector<IDirect3DTexture9*>& textures);

    // Number of DrawIndexedPrimitive calls issued since beginFrame().
    size_t drawCalls() const { return m_drawCalls; }

    // Uploads (once, keyed by `key`) a decoded image as a managed texture.
    IDirect3DTexture9* uploadTexture(const std::string& key, const Image& img);

    HWND hwnd() const { return m_hwnd; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    IDirect3DDevice9* device() { return m_dev; }
    const std::string& lastError() const { return m_err; }

private:
    bool createWindow(int width, int height, const std::string& title);
    bool createDevice();
    bool createSky();
    // Restores the lit + fogged state used for world geometry (the sky dome
    // overrides several of these while it draws).
    void applyWorldStates();

    HWND m_hwnd = nullptr;
    IDirect3D9* m_d3d = nullptr;
    IDirect3DDevice9* m_dev = nullptr;
    IDirect3DTexture9* m_white = nullptr;
    IDirect3DVertexBuffer9* m_skyVb = nullptr;
    IDirect3DIndexBuffer9* m_skyIb = nullptr;
    UINT m_skyVertexCount = 0;
    UINT m_skyIndexCount = 0;
    int m_width = 0;
    int m_height = 0;
    bool m_closed = false;
    size_t m_drawCalls = 0;
    float m_fogStart = 26000.0f;
    float m_fogEnd = 135000.0f;
    float m_exposure = 1.8f;
    D3DCOLOR m_exposureFactor = 0;
    std::string m_err;

    std::unordered_map<std::string, GpuMesh> m_meshes;
    std::unordered_map<std::string, IDirect3DTexture9*> m_textures;
};

}  // namespace skyrim::host

#endif  // _WIN32
