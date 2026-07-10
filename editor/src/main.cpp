// teidraw — MIT-licensed infinite-canvas whiteboard with tldraw-grade feel.
// Single translation unit (the slopstudio pattern): Dear ImGui + D3D11 + Win32,
// cross-compiled to a Win64 PE with mingw-w64. Split into files when it hurts.
//
// Responsiveness contract (why the main loop looks the way it does):
//   flip-model swapchain (FLIP_DISCARD, 3 buffers) + frame-latency waitable
//   object with max latency 1 + Present(1,0). Loop order each frame:
//   wait on the waitable (until DXGI can accept a frame) → THEN pump input →
//   build UI → render → present. Input is sampled as late as possible, so
//   pointer-to-photon latency is minimal, vsynced, and tear-free.

#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "misc/cpp/imgui_stdlib.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "fonts_embedded.h"   // font_hand/font_sans/font_mono/font_serif (tools/embed.py)

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ───────────────────────────── D3D11 / swapchain ───────────────────────────
static ID3D11Device*           g_dev = nullptr;
static ID3D11DeviceContext*    g_ctx = nullptr;
static IDXGISwapChain2*        g_sc  = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static HANDLE                  g_frameWaitable = nullptr;
static UINT  g_resizeW = 0, g_resizeH = 0;   // pending WM_SIZE
static const UINT kSwapFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

static void CreateBackbufferRTV() {
    ID3D11Texture2D* bb = nullptr;
    g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
}

static bool CreateDeviceD3D(HWND hwnd) {
    D3D_FEATURE_LEVEL lvls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, lvls, 2,
                                   D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, lvls, 2,
                               D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;

    IDXGIDevice* dxgiDev = nullptr;  g_dev->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    IDXGIAdapter* adapter = nullptr; dxgiDev->GetAdapter(&adapter);
    IDXGIFactory2* factory = nullptr; adapter->GetParent(IID_PPV_ARGS(&factory));
    dxgiDev->Release(); adapter->Release();

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.Scaling = DXGI_SCALING_NONE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = kSwapFlags;

    IDXGISwapChain1* sc1 = nullptr;
    hr = factory->CreateSwapChainForHwnd(g_dev, hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) { factory->Release(); return false; }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    sc1->QueryInterface(IID_PPV_ARGS(&g_sc));
    sc1->Release();
    if (!g_sc) return false;

    g_sc->SetMaximumFrameLatency(1);
    g_frameWaitable = g_sc->GetFrameLatencyWaitableObject();
    CreateBackbufferRTV();
    return true;
}

static void DestroyDeviceD3D() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_frameWaitable) { CloseHandle(g_frameWaitable); g_frameWaitable = nullptr; }
    if (g_sc)  { g_sc->Release();  g_sc = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

// Save the current backbuffer as a PNG (the --shot verification path).
static bool SaveBackbufferPNG(const char* path) {
    ID3D11Texture2D* bb = nullptr;
    g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (!bb) return false;
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &staging))) { bb->Release(); return false; }
    g_ctx->CopyResource(staging, bb);
    bb->Release();
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) { staging->Release(); return false; }
    std::vector<unsigned char> px((size_t)d.Width * d.Height * 4);
    for (UINT y = 0; y < d.Height; y++)
        memcpy(&px[(size_t)y * d.Width * 4], (unsigned char*)m.pData + (size_t)y * m.RowPitch, (size_t)d.Width * 4);
    g_ctx->Unmap(staging, 0);
    staging->Release();
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;   // force opaque alpha
    return stbi_write_png(path, (int)d.Width, (int)d.Height, 4, px.data(), (int)d.Width * 4) != 0;
}

// ───────────────────────────────── theme ───────────────────────────────────
struct Theme {
    ImU32 canvasBg, gridDot, panelBg, panelBorder, accent, textMain, textDim;
    ImU32 selStroke, selFill;
};
static bool  g_darkMode = true;
static Theme g_th;

static void ApplyTheme() {
    if (g_darkMode) {
        g_th.canvasBg    = IM_COL32(16, 16, 17, 255);     // tldraw-dark-ish near-black
        g_th.gridDot     = IM_COL32(255, 255, 255, 26);
        g_th.panelBg     = IM_COL32(35, 37, 41, 242);
        g_th.panelBorder = IM_COL32(255, 255, 255, 18);
        g_th.accent      = IM_COL32(76, 146, 255, 255);
        g_th.textMain    = IM_COL32(240, 240, 240, 255);
        g_th.textDim     = IM_COL32(160, 162, 168, 255);
        g_th.selStroke   = IM_COL32(76, 146, 255, 255);
        g_th.selFill     = IM_COL32(76, 146, 255, 26);
    } else {
        g_th.canvasBg    = IM_COL32(249, 250, 251, 255);
        g_th.gridDot     = IM_COL32(0, 0, 0, 30);
        g_th.panelBg     = IM_COL32(255, 255, 255, 245);
        g_th.panelBorder = IM_COL32(0, 0, 0, 22);
        g_th.accent      = IM_COL32(46, 116, 235, 255);
        g_th.textMain    = IM_COL32(28, 28, 30, 255);
        g_th.textDim     = IM_COL32(110, 112, 118, 255);
        g_th.selStroke   = IM_COL32(46, 116, 235, 255);
        g_th.selFill     = IM_COL32(46, 116, 235, 22);
    }
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 10.f; s.PopupRounding = 8.f; s.FrameRounding = 6.f;
    s.WindowBorderSize = 1.f; s.PopupBorderSize = 1.f;
    s.WindowPadding = ImVec2(10, 8); s.ItemSpacing = ImVec2(6, 6);
    ImVec4* c = s.Colors;
    auto v4 = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
    c[ImGuiCol_WindowBg] = v4(g_th.panelBg);
    c[ImGuiCol_PopupBg]  = v4(g_th.panelBg);
    c[ImGuiCol_Border]   = v4(g_th.panelBorder);
    c[ImGuiCol_Text]     = v4(g_th.textMain);
    c[ImGuiCol_Button]        = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ButtonHovered] = g_darkMode ? ImVec4(1, 1, 1, 0.08f) : ImVec4(0, 0, 0, 0.06f);
    c[ImGuiCol_ButtonActive]  = g_darkMode ? ImVec4(1, 1, 1, 0.14f) : ImVec4(0, 0, 0, 0.10f);
    c[ImGuiCol_FrameBg]        = g_darkMode ? ImVec4(1, 1, 1, 0.06f) : ImVec4(0, 0, 0, 0.04f);
    c[ImGuiCol_FrameBgHovered] = g_darkMode ? ImVec4(1, 1, 1, 0.10f) : ImVec4(0, 0, 0, 0.07f);
    c[ImGuiCol_FrameBgActive]  = g_darkMode ? ImVec4(1, 1, 1, 0.14f) : ImVec4(0, 0, 0, 0.10f);
    c[ImGuiCol_HeaderHovered]  = v4(g_th.accent); c[ImGuiCol_HeaderHovered].w = 0.25f;
    c[ImGuiCol_HeaderActive]   = v4(g_th.accent); c[ImGuiCol_HeaderActive].w = 0.35f;
}

// ───────────────────────────────── fonts ───────────────────────────────────
// Four families, dynamic atlas (imgui ≥1.92): glyphs rasterize on demand at
// whatever pixel size we push, so canvas text is crisp at every zoom level.
enum FontFamily { FONT_HAND = 0, FONT_SANS, FONT_MONO, FONT_SERIF, FONT_COUNT };
static ImFont* g_fonts[FONT_COUNT] = {};

static void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg; cfg.FontDataOwnedByAtlas = false;
    struct { const unsigned char* data; unsigned int len; const char* name; } specs[FONT_COUNT] = {
        { font_hand,  font_hand_len,  "hand"  },
        { font_sans,  font_sans_len,  "sans"  },
        { font_mono,  font_mono_len,  "mono"  },
        { font_serif, font_serif_len, "serif" },
    };
    for (int i = 0; i < FONT_COUNT; i++) {
        snprintf(cfg.Name, sizeof(cfg.Name), "%s", specs[i].name);
        g_fonts[i] = io.Fonts->AddFontFromMemoryTTF((void*)specs[i].data, (int)specs[i].len, 0.f, &cfg);
    }
    io.FontDefault = g_fonts[FONT_SANS];   // UI chrome reads better in sans; canvas text defaults to hand
}

// ───────────────────────────────── camera ──────────────────────────────────
// screen = world * zoom + pan   (pan in screen px). Zoom pivots on the cursor.
struct Camera { ImVec2 pan{0, 0}; float zoom = 1.f; };
static Camera g_cam;

static ImVec2 W2S(ImVec2 w) { return ImVec2(w.x * g_cam.zoom + g_cam.pan.x, w.y * g_cam.zoom + g_cam.pan.y); }
static ImVec2 S2W(ImVec2 s) { return ImVec2((s.x - g_cam.pan.x) / g_cam.zoom, (s.y - g_cam.pan.y) / g_cam.zoom); }

static void ZoomAt(ImVec2 pivot, float factor) {
    float z = g_cam.zoom * factor;
    z = z < 0.02f ? 0.02f : (z > 64.f ? 64.f : z);
    factor = z / g_cam.zoom;
    g_cam.pan.x = pivot.x - (pivot.x - g_cam.pan.x) * factor;
    g_cam.pan.y = pivot.y - (pivot.y - g_cam.pan.y) * factor;
    g_cam.zoom = z;
}

// Adaptive dot grid: pick the power-of-two multiple of the base spacing whose
// screen-space pitch lands in a comfy range; fade dots in as they spread out.
static void DrawGrid(ImDrawList* dl, ImVec2 size) {
    const float base = 32.f;
    float pitch = base * g_cam.zoom;
    float lvl = 1.f;
    while (pitch * lvl < 18.f)  lvl *= 2.f;
    while (pitch * lvl > 44.f)  lvl *= 0.5f;
    float step = pitch * lvl;                       // px between dots
    float t = (step - 18.f) / (44.f - 18.f);        // 0 near fade-out, 1 spread out
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    ImU32 col = g_th.gridDot;
    int alpha = (int)((col >> 24) * (0.35f + 0.65f * t));
    col = (col & 0x00FFFFFF) | ((ImU32)alpha << 24);
    float x0 = fmodf(g_cam.pan.x, step); if (x0 < 0) x0 += step;
    float y0 = fmodf(g_cam.pan.y, step); if (y0 < 0) y0 += step;
    float r = g_cam.zoom < 1.f ? 1.f : 1.25f;
    for (float y = y0; y < size.y; y += step)
        for (float x = x0; x < size.x; x += step)
            dl->AddRectFilled(ImVec2(x - r, y - r), ImVec2(x + r, y + r), col, r);
}

// ─────────────────────────────── tools / UI ────────────────────────────────
enum Tool { TOOL_SELECT = 0, TOOL_HAND, TOOL_TEXT, TOOL_ARROW, TOOL_COUNT };
static Tool g_tool = TOOL_SELECT;
static bool g_spacePan = false;      // space held → temporary hand tool

static const char* kToolLabel[TOOL_COUNT] = { "sel", "hand", "text", "arrow" };
static const char* kToolKey[TOOL_COUNT]   = { "V", "H", "T", "A" };

// Floating bottom-center toolbar, tldraw-style.
static void DrawToolbar(ImVec2 display) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Size.x * 0.5f, vp->Size.y - 16.f), ImGuiCond_Always, ImVec2(0.5f, 1.f));
    ImGui::Begin("##toolbar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                 ImGuiWindowFlags_NoNav);
    for (int i = 0; i < TOOL_COUNT; i++) {
        if (i) ImGui::SameLine();
        bool active = (g_tool == (Tool)i);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(g_th.accent));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
        }
        char lbl[32]; snprintf(lbl, sizeof lbl, "%s##tool%d", kToolLabel[i], i);
        if (ImGui::Button(lbl, ImVec2(52, 36))) g_tool = (Tool)i;
        if (active) ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s (%s)", kToolLabel[i], kToolKey[i]);
    }
    ImGui::End();
}

// Bottom-left zoom pill: click → 100%.
static void DrawZoomPill() {
    ImGui::SetNextWindowPos(ImVec2(16.f, ImGui::GetMainViewport()->Size.y - 16.f), ImGuiCond_Always, ImVec2(0.f, 1.f));
    ImGui::Begin("##zoom", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    char z[32]; snprintf(z, sizeof z, "%d%%", (int)roundf(g_cam.zoom * 100.f));
    if (ImGui::Button(z)) {
        ImVec2 c = ImVec2(ImGui::GetMainViewport()->Size.x * 0.5f, ImGui::GetMainViewport()->Size.y * 0.5f);
        ZoomAt(c, 1.f / g_cam.zoom);
    }
    ImGui::End();
}

// ─────────────────────────── canvas interaction ────────────────────────────
static void CanvasFrame() {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), g_th.canvasBg);
    DrawGrid(dl, vp->Size);

    bool uiHot = io.WantCaptureMouse && (ImGui::GetHoveredID() != 0 || ImGui::IsAnyItemHovered() ||
                                         ImGui::GetTopMostPopupModal() || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));
    // Any imgui window under the mouse (toolbar, pills, popups) owns the input.
    uiHot = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);

    // keyboard: tool switching + space-pan (ignored while typing in a widget)
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_V)) g_tool = TOOL_SELECT;
        if (ImGui::IsKeyPressed(ImGuiKey_H)) g_tool = TOOL_HAND;
        if (ImGui::IsKeyPressed(ImGuiKey_T)) g_tool = TOOL_TEXT;
        if (ImGui::IsKeyPressed(ImGuiKey_A)) g_tool = TOOL_ARROW;
        g_spacePan = ImGui::IsKeyDown(ImGuiKey_Space);
    } else g_spacePan = false;

    if (uiHot) return;

    // wheel = zoom at cursor (THE departure from tldraw: scroll never pans)
    if (io.MouseWheel != 0.f)
        ZoomAt(io.MousePos, powf(1.16f, io.MouseWheel));

    // pan: middle-drag always; left-drag with hand tool or space held
    bool handActive = (g_tool == TOOL_HAND) || g_spacePan;
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.f) ||
        (handActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f))) {
        g_cam.pan.x += io.MouseDelta.x;
        g_cam.pan.y += io.MouseDelta.y;
    }
}

// ───────────────────────────────── win32 ───────────────────────────────────
static LRESULT WINAPI WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
    switch (m) {
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) { g_resizeW = LOWORD(l); g_resizeH = HIWORD(l); }
        return 0;
    case WM_SYSCOMMAND:
        if ((w & 0xfff0) == SC_KEYMENU) return 0;   // no alt menu beep
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    // --shot <png> [--frames N]: render N frames then save the backbuffer and exit.
    const char* shotPath = nullptr; int shotFrames = 8;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shotPath = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) shotFrames = atoi(argv[++i]);
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_OWNDC, WndProc, 0, 0, GetModuleHandleW(nullptr),
                       nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"teidraw", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(L"teidraw", L"teidraw", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1600, 1000, nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd)) { fprintf(stderr, "teidraw: D3D11 init failed\n"); return 1; }
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;                       // no imgui.ini litter; layout is ours
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    LoadFonts();
    ImGui::GetStyle().FontSizeBase = 15.f * dpi;
    ApplyTheme();
    ImGui::GetStyle().ScaleAllSizes(dpi);

    int framesDone = 0;
    bool done = false;
    while (!done) {
        // Wait until the swapchain can accept a frame, THEN read input → lowest latency.
        if (g_frameWaitable) WaitForSingleObjectEx(g_frameWaitable, 1000, TRUE);

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_resizeW) {
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
            g_sc->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, kSwapFlags);
            CreateBackbufferRTV();
            g_resizeW = g_resizeH = 0;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        CanvasFrame();
        DrawToolbar(io.DisplaySize);
        DrawZoomPill();

        // theme toggle (Ctrl+Shift+D) — proper settings UI comes with the context menu
        if (!io.WantTextInput && io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_D)) {
            g_darkMode = !g_darkMode; ApplyTheme();
        }

        ImGui::Render();
        float bg[4] = { 0, 0, 0, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, bg);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_sc->Present(shotPath ? 0 : 1, 0);

        if (shotPath && ++framesDone >= shotFrames) {
            bool ok = SaveBackbufferPNG(shotPath);
            fprintf(stderr, "teidraw: shot %s -> %s\n", shotPath, ok ? "ok" : "FAILED");
            done = true;
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(L"teidraw", wc.hInstance);
    return 0;
}
