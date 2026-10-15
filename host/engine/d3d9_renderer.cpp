// d3d9_renderer.cpp - Win32 window + Direct3D 9 device implementation.

#include "d3d9_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace skyrim::host {
namespace {

constexpr const wchar_t* kClassName = L"SkyrimStandaloneWindow";

// Sky and fog palette. The fog colour matches the horizon so distant geometry
// dissolves into the sky rather than ending in a hard line.
constexpr int kHorizonR = 178, kHorizonG = 196, kHorizonB = 214;
constexpr DWORD kHorizonColor = D3DCOLOR_XRGB(kHorizonR, kHorizonG, kHorizonB);
constexpr DWORD kZenithColor = D3DCOLOR_XRGB(74, 116, 178);

DWORD lerpColor(DWORD a, DWORD b, float t) {
    const int ar = static_cast<int>((a >> 16) & 0xFF);
    const int ag = static_cast<int>((a >> 8) & 0xFF);
    const int ab = static_cast<int>(a & 0xFF);
    const int br = static_cast<int>((b >> 16) & 0xFF);
    const int bg = static_cast<int>((b >> 8) & 0xFF);
    const int bb = static_cast<int>(b & 0xFF);
    const int r = ar + static_cast<int>(static_cast<float>(br - ar) * t + 0.5f);
    const int g = ag + static_cast<int>(static_cast<float>(bg - ag) * t + 0.5f);
    const int bl = ab + static_cast<int>(static_cast<float>(bb - ab) * t + 0.5f);
    return D3DCOLOR_XRGB(r, g, bl);
}

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

    // Fixed-function lighting + fog + texture stages live in one place so the
    // sky dome can temporarily override them and restore them afterwards.
    applyWorldStates();

    // Trilinear + anisotropic filtering, wrapping. uploadTexture() builds a mip
    // chain, so distance detail stays smooth instead of shimmering or clamping
    // to the 1x1 top mip.
    m_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
    m_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    m_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    m_dev->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 8);
    m_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    m_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

    // 1x1 white texture: bound whenever a submesh has no texture, so a flat
    // fallback colour still modulates through the same stage setup.
    if (SUCCEEDED(m_dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                       &m_white, nullptr))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(m_white->LockRect(0, &lr, nullptr, 0))) {
            *reinterpret_cast<uint32_t*>(lr.pBits) = 0xFFFFFFFFu;
            m_white->UnlockRect(0);
        }
    }

    createSky();
    return true;
}

void Renderer::applyWorldStates() {
    m_dev->SetRenderState(D3DRS_LIGHTING, TRUE);
    // A cool sky ambient fills surfaces the sun does not reach. It is
    // deliberately strong: these textures carry fairly dark albedo, so shadows
    // go black without it.
    m_dev->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_XRGB(128, 140, 160));
    m_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    m_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    // Skyrim meshes are authored right-handed; with a left-handed projection the
    // winding flips, so culling is left off rather than guessed at.
    m_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
    m_dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    // Distance fog dissolves the streamed edge into the horizon instead of
    // ending in a hard line against the sky.
    const float fogStart = 26000.0f;
    const float fogEnd = 135000.0f;
    m_dev->SetRenderState(D3DRS_FOGENABLE, TRUE);
    m_dev->SetRenderState(D3DRS_FOGCOLOR, kHorizonColor);
    m_dev->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
    m_dev->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    m_dev->SetRenderState(D3DRS_FOGSTART, *reinterpret_cast<const DWORD*>(&fogStart));
    m_dev->SetRenderState(D3DRS_FOGEND, *reinterpret_cast<const DWORD*>(&fogEnd));

    // The sun is two directional lights on the same vector. A single light is
    // clamped to [0,1], which is not enough to lift dark albedo; two sum to
    // ~1.1 and stay below the clip point for everything but snow.
    const Vec3 d = normalize(Vec3{0.4f, 0.5f, -0.75f});  // world is Z-up
    D3DLIGHT9 light;
    std::memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse = color(0.78f, 0.75f, 0.69f, 1.0f);
    light.Ambient = color(0.16f, 0.17f, 0.20f, 1.0f);
    light.Specular = color(0.0f, 0.0f, 0.0f, 1.0f);
    light.Direction = D3DVECTOR{d.x, d.y, d.z};
    m_dev->SetLight(0, &light);
    m_dev->LightEnable(0, TRUE);

    std::memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse = color(0.42f, 0.40f, 0.37f, 1.0f);
    light.Ambient = color(0.0f, 0.0f, 0.0f, 1.0f);
    light.Specular = color(0.0f, 0.0f, 0.0f, 1.0f);
    light.Direction = D3DVECTOR{d.x, d.y, d.z};
    m_dev->SetLight(1, &light);
    m_dev->LightEnable(1, TRUE);
    m_dev->LightEnable(2, FALSE);
    m_dev->LightEnable(3, FALSE);

    m_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    m_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    m_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    m_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
}

bool Renderer::createSky() {
    if (m_dev == nullptr) return false;

    // A dome around the camera: rings from just below the horizon up to the
    // zenith, shaded from haze at the horizon to deeper blue overhead.
    const int slices = 24;
    const int stacks = 8;
    const float radius = 100000.0f;  // inside the far plane
    const float pi = 3.14159265358979f;
    const float bottom = -0.35f;     // radians below the horizon

    std::vector<SkyVertex> verts;
    verts.reserve(static_cast<size_t>(stacks + 1) * static_cast<size_t>(slices + 1));
    for (int i = 0; i <= stacks; i++) {
        const float t = static_cast<float>(i) / static_cast<float>(stacks);
        const float elev = bottom + (pi * 0.5f - bottom) * t;
        const float ct = std::cos(elev);
        const float st = std::sin(elev);
        const float k = st > 0.0f ? st : 0.0f;  // 0 at/below the horizon, 1 at zenith
        for (int j = 0; j <= slices; j++) {
            const float az = 2.0f * pi * static_cast<float>(j) / static_cast<float>(slices);
            SkyVertex v;
            v.x = radius * ct * std::cos(az);
            v.y = radius * ct * std::sin(az);
            v.z = radius * st;
            v.color = lerpColor(kHorizonColor, kZenithColor, k);
            verts.push_back(v);
        }
    }

    std::vector<uint16_t> idx;
    idx.reserve(static_cast<size_t>(stacks) * static_cast<size_t>(slices) * 6u);
    for (int i = 0; i < stacks; i++) {
        for (int j = 0; j < slices; j++) {
            const uint16_t a = static_cast<uint16_t>(i * (slices + 1) + j);
            const uint16_t b = static_cast<uint16_t>(a + 1);
            const uint16_t c = static_cast<uint16_t>(a + slices + 1);
            const uint16_t d = static_cast<uint16_t>(c + 1);
            idx.push_back(a);
            idx.push_back(c);
            idx.push_back(b);
            idx.push_back(b);
            idx.push_back(c);
            idx.push_back(d);
        }
    }

    const UINT vbBytes = static_cast<UINT>(verts.size() * sizeof(SkyVertex));
    if (FAILED(m_dev->CreateVertexBuffer(vbBytes, D3DUSAGE_WRITEONLY, kSkyFvf,
                                         D3DPOOL_MANAGED, &m_skyVb, nullptr))) {
        return false;
    }
    void* vdst = nullptr;
    if (FAILED(m_skyVb->Lock(0, vbBytes, &vdst, 0))) {
        m_skyVb->Release();
        m_skyVb = nullptr;
        return false;
    }
    std::memcpy(vdst, verts.data(), vbBytes);
    m_skyVb->Unlock();

    const UINT ibBytes = static_cast<UINT>(idx.size() * sizeof(uint16_t));
    if (FAILED(m_dev->CreateIndexBuffer(ibBytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                        D3DPOOL_MANAGED, &m_skyIb, nullptr))) {
        return false;
    }
    void* idst = nullptr;
    if (FAILED(m_skyIb->Lock(0, ibBytes, &idst, 0))) {
        m_skyIb->Release();
        m_skyIb = nullptr;
        return false;
    }
    std::memcpy(idst, idx.data(), ibBytes);
    m_skyIb->Unlock();

    m_skyVertexCount = static_cast<UINT>(verts.size());
    m_skyIndexCount = static_cast<UINT>(idx.size());
    return true;
}

void Renderer::drawSky(const Vec3& eye) {
    if (m_dev == nullptr || m_skyVb == nullptr || m_skyIb == nullptr) return;

    D3DMATRIX world;
    std::memset(&world, 0, sizeof(world));
    world._11 = world._22 = world._33 = world._44 = 1.0f;
    world._41 = eye.x;
    world._42 = eye.y;
    world._43 = eye.z;
    m_dev->SetTransform(D3DTS_WORLD, &world);

    // Unlit, unfogged and drawn without depth so it acts as the background the
    // world is then drawn over.
    m_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    m_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    m_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    m_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    m_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    m_dev->SetTexture(0, nullptr);

    m_dev->SetStreamSource(0, m_skyVb, 0, sizeof(SkyVertex));
    m_dev->SetIndices(m_skyIb);
    m_dev->SetFVF(kSkyFvf);
    m_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, m_skyVertexCount, 0,
                                m_skyIndexCount / 3);

    applyWorldStates();
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
    if (m_skyVb != nullptr) {
        m_skyVb->Release();
        m_skyVb = nullptr;
    }
    if (m_skyIb != nullptr) {
        m_skyIb->Release();
        m_skyIb = nullptr;
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

    m_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kHorizonColor, 1.0f, 0);
    m_dev->BeginScene();
    return true;
}

void Renderer::endScene() {
    if (m_dev != nullptr) m_dev->EndScene();
}

void Renderer::present() {
    if (m_dev != nullptr) m_dev->Present(nullptr, nullptr, nullptr, nullptr);
}

bool Renderer::saveScreenshot(const std::string& path) {
    if (m_dev == nullptr) return false;

    IDirect3DSurface9* back = nullptr;
    if (FAILED(m_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) return false;
    D3DSURFACE_DESC desc;
    if (FAILED(back->GetDesc(&desc))) {
        back->Release();
        return false;
    }

    IDirect3DSurface9* off = nullptr;
    if (FAILED(m_dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                  D3DPOOL_SYSTEMMEM, &off, nullptr))) {
        back->Release();
        return false;
    }
    const bool copied = SUCCEEDED(m_dev->GetRenderTargetData(back, off));
    back->Release();

    D3DLOCKED_RECT lr;
    bool ok = false;
    if (copied && SUCCEEDED(off->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        const int w = static_cast<int>(desc.Width);
        const int h = static_cast<int>(desc.Height);
        const int rowBytes = w * 3;
        const int pad = (4 - (rowBytes % 4)) % 4;
        const uint32_t pixelBytes = static_cast<uint32_t>(rowBytes + pad) * static_cast<uint32_t>(h);
        const uint32_t fileBytes = 54u + pixelBytes;

        FILE* f = std::fopen(path.c_str(), "wb");
        if (f != nullptr) {
            uint8_t header[54] = {};
            header[0] = 'B';
            header[1] = 'M';
            std::memcpy(header + 2, &fileBytes, 4);
            const uint32_t off2 = 54;
            std::memcpy(header + 10, &off2, 4);
            const uint32_t infoSize = 40;
            std::memcpy(header + 14, &infoSize, 4);
            std::memcpy(header + 18, &desc.Width, 4);
            std::memcpy(header + 22, &desc.Height, 4);
            const uint16_t planes = 1, bpp = 24;
            std::memcpy(header + 26, &planes, 2);
            std::memcpy(header + 28, &bpp, 2);
            std::memcpy(header + 34, &pixelBytes, 4);
            std::fwrite(header, 1, sizeof(header), f);

            // BMP rows run bottom to top and are byte-swapped to BGR.
            uint8_t padBytes[4] = {};
            for (int y = h - 1; y >= 0; --y) {
                const uint8_t* src = static_cast<const uint8_t*>(lr.pBits) +
                                     static_cast<size_t>(y) * lr.Pitch;
                for (int x = 0; x < w; ++x) {
                    const uint8_t bgr[3] = {src[x * 4 + 0], src[x * 4 + 1], src[x * 4 + 2]};
                    std::fwrite(bgr, 1, 3, f);
                }
                if (pad > 0) std::fwrite(padBytes, 1, static_cast<size_t>(pad), f);
            }
            std::fclose(f);
            ok = true;
        }
        off->UnlockRect();
    }
    off->Release();
    return ok;
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

    // Build a full box-filtered mip chain on the CPU. Doing it here keeps the
    // library D3DX-free and works in the managed pool (auto-generated mips need
    // D3DUSAGE_AUTOGENMIPMAP + the default pool).
    //
    // The decoder emits RGBA; D3DFMT_A8R8G8B8 stores B,G,R,A in memory, so swap
    // red and blue once up front (averaging is channel-order agnostic). The
    // alternative A8B8G8R8 format is rejected by many D3D9 drivers with
    // D3DERR_INVALIDCALL, which silently drops every texture.
    std::vector<std::vector<uint8_t>> levels;
    std::vector<uint8_t> base(img.rgba.size());
    for (size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        base[i + 0] = img.rgba[i + 2];
        base[i + 1] = img.rgba[i + 1];
        base[i + 2] = img.rgba[i + 0];
        base[i + 3] = img.rgba[i + 3];
    }
    levels.push_back(std::move(base));
    int lw = img.width;
    int lh = img.height;
    while (lw > 1 || lh > 1) {
        const int nw = lw > 1 ? lw / 2 : 1;
        const int nh = lh > 1 ? lh / 2 : 1;
        const std::vector<uint8_t>& src = levels.back();
        std::vector<uint8_t> dst(static_cast<size_t>(nw) * static_cast<size_t>(nh) * 4u);
        for (int y = 0; y < nh; y++) {
            for (int x = 0; x < nw; x++) {
                int r = 0, g = 0, b = 0, a = 0, n = 0;
                for (int dy = 0; dy < 2; dy++) {
                    const int sy = std::min(lh - 1, y * 2 + dy);
                    for (int dx = 0; dx < 2; dx++) {
                        const int sx = std::min(lw - 1, x * 2 + dx);
                        const uint8_t* p =
                            &src[(static_cast<size_t>(sy) * static_cast<size_t>(lw) +
                                  static_cast<size_t>(sx)) * 4u];
                        r += p[0];
                        g += p[1];
                        b += p[2];
                        a += p[3];
                        ++n;
                    }
                }
                uint8_t* d =
                    &dst[(static_cast<size_t>(y) * static_cast<size_t>(nw) +
                          static_cast<size_t>(x)) * 4u];
                d[0] = static_cast<uint8_t>(r / n);
                d[1] = static_cast<uint8_t>(g / n);
                d[2] = static_cast<uint8_t>(b / n);
                d[3] = static_cast<uint8_t>(a / n);
            }
        }
        levels.push_back(std::move(dst));
        lw = nw;
        lh = nh;
    }

    IDirect3DTexture9* tex = nullptr;
    if (FAILED(m_dev->CreateTexture(static_cast<UINT>(img.width),
                                    static_cast<UINT>(img.height),
                                    static_cast<UINT>(levels.size()), 0, D3DFMT_A8R8G8B8,
                                    D3DPOOL_MANAGED, &tex, nullptr))) {
        return nullptr;
    }
    for (size_t lvl = 0; lvl < levels.size(); lvl++) {
        D3DLOCKED_RECT lr;
        if (FAILED(tex->LockRect(static_cast<UINT>(lvl), &lr, nullptr, 0))) {
            tex->Release();
            return nullptr;
        }
        const int w = std::max(1, img.width >> lvl);
        const int h = std::max(1, img.height >> lvl);
        const size_t rowBytes = static_cast<size_t>(w) * 4u;
        for (int y = 0; y < h; y++) {
            std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch,
                        levels[lvl].data() + static_cast<size_t>(y) * rowBytes, rowBytes);
        }
        tex->UnlockRect(static_cast<UINT>(lvl));
    }
    m_textures.emplace(key, tex);
    return tex;
}

}  // namespace skyrim::host
