// d3d9_renderer.hpp - Win32 window + Direct3D 9 device, GPU caches, drawing.
//
// This is the only place in the project that knows about a graphics API. It
// lives in the host, not the library, so the module stays renderer-free and can
// be reused by the integration host with a different backend.
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

// The vertex layout the renderer uploads: position, normal, texture coordinate.
constexpr DWORD kVertexFvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;

struct RtVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    float u = 0.0f, v = 0.0f;
};

class Renderer {
public:
    bool init(int width, int height, const std::string& title, std::string* err);
    void shutdown();

    // Pumps the Win32 message queue. Returns false once the window is closed.
    bool pumpMessages();

    bool beginFrame();
    void endFrame();
    void setCamera(const Mat4& view, const Mat4& proj);

    struct GpuMesh {
        IDirect3DVertexBuffer9* vb = nullptr;
        IDirect3DIndexBuffer9* ib = nullptr;
    };

    // Uploads (once, keyed by `key`) and returns the GPU mesh, or nullptr on
    // failure. The returned pointer is stable until shutdown().
    GpuMesh* uploadMesh(const std::string& key, const Mesh& mesh);

    // Draws every submesh of `mesh` at `world`. `textures` is indexed in
    // parallel with mesh.submeshes; a null entry uses a flat white texture.
    void drawMesh(GpuMesh* gpu, const Mesh& mesh, const Mat4& world,
                  const std::vector<IDirect3DTexture9*>& textures);

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

    HWND m_hwnd = nullptr;
    IDirect3D9* m_d3d = nullptr;
    IDirect3DDevice9* m_dev = nullptr;
    IDirect3DTexture9* m_white = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_closed = false;
    std::string m_err;

    std::unordered_map<std::string, GpuMesh> m_meshes;
    std::unordered_map<std::string, IDirect3DTexture9*> m_textures;
};

}  // namespace skyrim::host

#endif  // _WIN32
