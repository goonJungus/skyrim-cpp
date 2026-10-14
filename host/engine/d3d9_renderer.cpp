// d3d9_renderer.cpp - Win32 window + Direct3D 9 device implementation.

#include "d3d9_renderer.hpp"

#include <cstdint>
#include <cstring>

namespace skyrim::host {
namespace {

constexpr const wchar_t* kClassName = L"SkyrimStandaloneWindow";

D3DCOLORVALUE color(float r, float g, float b, float a) {
    D3DCOLORVALUE c;
    c.r = r;
    c.g = g;
    c.b = b;
    c.a = a;
    return c;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* closed = reinterpret_cast<bool*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CLOSE:
            if (closed != nullptr) *closed = true;
            return 0;
        case WM_DESTROY:
            if (closed != nullptr) *closed = true;
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool registerWindowClass() {
    static bool done = false;
    if (done) return true;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0) return false;
    done = true;
    return true;
}

}  // namespace

bool Renderer::init(int width, int height, const std::string& title, std::string* err) {
    auto fail = [&](const char* what) {
        m_err = what;
        if (err != nullptr) *err = m_err;
        return false;
    };
    m_width = width;
    m_height = height;
    if (!registerWindowClass()) return fail("RegisterClassEx failed");
    if (!createWindow(width, height, title)) return fail("CreateWindow failed");
    if (!createDevice()) return fail(m_err.c_str());

    // Fixed-function pipeline: one directional light, textured + lit surfaces.
    m_dev->SetRenderState(D3DRS_LIGHTING, TRUE);
    m_dev->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_XRGB(72, 78, 92));
    m_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    // Skyrim meshes are authored right-handed; with a left-handed projection the
    // winding flips, so culling is left off rather than guessed at.
    m_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
    m_dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    D3DLIGHT9 light;
    std::memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse = color(1.0f, 0.98f, 0.92f, 1.0f);
    light.Ambient = color(0.25f, 0.26f, 0.30f, 1.0f);
    light.Specular = color(0.0f, 0.0f, 0.0f, 1.0f);
    const Vec3 d = normalize(Vec3{0.4f, 0.5f, -0.75f});  // world is Z-up
    light.Direction = D3DVECTOR{d.x, d.y, d.z};
    m_dev->SetLight(0, &light);
    m_dev->LightEnable(0, TRUE);

    m_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    m_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    m_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    m_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);

    // 1x1 white texture: bound whenever a submesh has no texture, so a flat
    // fallback colour still modulates through the same stage setup.
    if (SUCCEEDED(m_dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8B8G8R8, D3DPOOL_MANAGED,
                                       &m_white, nullptr))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(m_white->LockRect(0, &lr, nullptr, 0))) {
            *reinterpret_cast<uint32_t*>(lr.pBits) = 0xFFFFFFFFu;
            m_white->UnlockRect(0);
        }
    }
    return true;
}

bool Renderer::createWindow(int width, int height, const std::string& title) {
    RECT r = {0, 0, width, height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    m_hwnd = CreateWindowExW(0, kClassName, widen(title).c_str(), WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr),
                             nullptr);
    if (m_hwnd == nullptr) return false;
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&m_closed));
    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return true;
}

bool Renderer::createDevice() {
    m_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (m_d3d == nullptr) {
        m_err = "Direct3DCreate9 failed";
        return false;
    }

    D3DPRESENT_PARAMETERS pp;
    std::memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = static_cast<UINT>(m_width);
    pp.BackBufferHeight = static_cast<UINT>(m_height);
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.hDeviceWindow = m_hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
    HRESULT hr = m_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, m_hwnd, flags,
                                     &pp, &m_dev);
    if (FAILED(hr)) {
        flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
        hr = m_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, m_hwnd, flags, &pp,
                                 &m_dev);
    }
    if (FAILED(hr)) {
        m_err = "IDirect3D9::CreateDevice failed";
        return false;
    }
    return true;
}

void Renderer::shutdown() {
    for (auto& kv : m_meshes) {
        if (kv.second.vb != nullptr) kv.second.vb->Release();
        if (kv.second.ib != nullptr) kv.second.ib->Release();
    }
    m_meshes.clear();
    for (auto& kv : m_textures) {
        if (kv.second != nullptr) kv.second->Release();
    }
    m_textures.clear();
    if (m_white != nullptr) {
        m_white->Release();
        m_white = nullptr;
    }
    if (m_dev != nullptr) {
        m_dev->Release();
        m_dev = nullptr;
    }
    if (m_d3d != nullptr) {
        m_d3d->Release();
        m_d3d = nullptr;
    }
    if (m_hwnd != nullptr) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

bool Renderer::pumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != 0) {
        if (msg.message == WM_QUIT) m_closed = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !m_closed;
}

bool Renderer::beginFrame() {
    if (m_dev == nullptr) return false;
    const HRESULT coop = m_dev->TestCooperativeLevel();
    if (coop == D3DERR_DEVICELOST) return false;
    if (coop == D3DERR_DEVICENOTRESET) {
        D3DPRESENT_PARAMETERS pp;
        std::memset(&pp, 0, sizeof(pp));
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferWidth = static_cast<UINT>(m_width);
        pp.BackBufferHeight = static_cast<UINT>(m_height);
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
        pp.hDeviceWindow = m_hwnd;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        if (FAILED(m_dev->Reset(&pp))) return false;
    }

    m_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                 D3DCOLOR_XRGB(140, 165, 200), 1.0f, 0);
    m_dev->BeginScene();
    return true;
}

void Renderer::endFrame() {
    if (m_dev == nullptr) return;
    m_dev->EndScene();
    m_dev->Present(nullptr, nullptr, nullptr, nullptr);
}

void Renderer::setCamera(const Mat4& view, const Mat4& proj) {
    if (m_dev == nullptr) return;
    m_dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(&view));
    m_dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(&proj));
}

Renderer::GpuMesh* Renderer::uploadMesh(const std::string& key, const Mesh& mesh) {
    auto it = m_meshes.find(key);
    if (it != m_meshes.end()) return &it->second;
    if (m_dev == nullptr || !mesh.valid()) return nullptr;

    static_assert(sizeof(RtVertex) == sizeof(Vertex), "vertex layouts must match");

    GpuMesh gpu;
    const UINT vbytes = static_cast<UINT>(mesh.vertices.size() * sizeof(RtVertex));
    if (FAILED(m_dev->CreateVertexBuffer(vbytes, D3DUSAGE_WRITEONLY, kVertexFvf,
                                         D3DPOOL_MANAGED, &gpu.vb, nullptr))) {
        return nullptr;
    }
    void* vp = nullptr;
    if (FAILED(gpu.vb->Lock(0, 0, &vp, 0))) {
        gpu.vb->Release();
        return nullptr;
    }
    std::memcpy(vp, mesh.vertices.data(), vbytes);
    gpu.vb->Unlock();

    const UINT ibytes = static_cast<UINT>(mesh.indices.size() * sizeof(uint32_t));
    if (FAILED(m_dev->CreateIndexBuffer(ibytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32,
                                        D3DPOOL_MANAGED, &gpu.ib, nullptr))) {
        gpu.vb->Release();
        return nullptr;
    }
    void* ip = nullptr;
    if (FAILED(gpu.ib->Lock(0, 0, &ip, 0))) {
        gpu.vb->Release();
        gpu.ib->Release();
        return nullptr;
    }
    std::memcpy(ip, mesh.indices.data(), ibytes);
    gpu.ib->Unlock();

    auto res = m_meshes.emplace(key, gpu);
    return &res.first->second;
}

void Renderer::drawMesh(GpuMesh* gpu, const Mesh& mesh, const Mat4& world,
                        const std::vector<IDirect3DTexture9*>& textures) {
    if (m_dev == nullptr || gpu == nullptr || gpu->vb == nullptr || gpu->ib == nullptr) {
        return;
    }
    m_dev->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(&world));
    m_dev->SetStreamSource(0, gpu->vb, 0, sizeof(RtVertex));
    m_dev->SetIndices(gpu->ib);
    m_dev->SetFVF(kVertexFvf);

    for (size_t i = 0; i < mesh.submeshes.size(); i++) {
        const Submesh& sm = mesh.submeshes[i];
        if (sm.indexCount == 0) continue;
        const bool haveTex = i < textures.size() && textures[i] != nullptr;
        m_dev->SetTexture(0, haveTex ? textures[i] : m_white);

        D3DMATERIAL9 mat;
        std::memset(&mat, 0, sizeof(mat));
        if (haveTex) {
            mat.Diffuse = color(1.0f, 1.0f, 1.0f, 1.0f);
        } else {
            mat.Diffuse = color(sm.fallbackR, sm.fallbackG, sm.fallbackB, 1.0f);
        }
        mat.Ambient = mat.Diffuse;
        m_dev->SetMaterial(&mat);

        if (sm.alphaTest != 0) {
            m_dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
            m_dev->SetRenderState(D3DRS_ALPHAREF, 0x00000080);
            m_dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        } else {
            m_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        }

        m_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                                    static_cast<UINT>(mesh.vertices.size()), sm.indexStart,
                                    sm.indexCount / 3);
    }
}

IDirect3DTexture9* Renderer::uploadTexture(const std::string& key, const Image& img) {
    auto it = m_textures.find(key);
    if (it != m_textures.end()) return it->second;
    if (m_dev == nullptr || img.width <= 0 || img.height <= 0 || img.rgba.empty()) {
        return nullptr;
    }
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(m_dev->CreateTexture(static_cast<UINT>(img.width),
                                    static_cast<UINT>(img.height), 1, 0, D3DFMT_A8B8G8R8,
                                    D3DPOOL_MANAGED, &tex, nullptr))) {
        return nullptr;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) {
        tex->Release();
        return nullptr;
    }
    const size_t rowBytes = static_cast<size_t>(img.width) * 4u;
    for (int y = 0; y < img.height; y++) {
        std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch,
                    img.rgba.data() + static_cast<size_t>(y) * rowBytes, rowBytes);
    }
    tex->UnlockRect(0);
    m_textures.emplace(key, tex);
    return tex;
}

}  // namespace skyrim::host
