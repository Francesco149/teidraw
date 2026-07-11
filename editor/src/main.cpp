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
//
// Interaction philosophy (tldraw's): hide complexity, guess intent.
//   click child of group → selects the group; click again → drills to child.
//   click text → edit; drag text → move. arrow end dropped on a shape → binds
//   to it (exact anchor remembered) and the line is trimmed at its bounds.
//   right-drag pans, right-click opens the context menu. scroll = zoom, always.

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>      // SHGetKnownFolderPath (Documents → default boards dir)
#include <shobjidl.h>    // IFileOpenDialog (board folder picker)
#include <d3d11.h>
#include <dxgi1_3.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <fstream>
#include <sstream>
#include <algorithm>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "misc/cpp/imgui_stdlib.h"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "fonts_embedded.h"   // font_hand/font_sans/font_mono/font_serif (tools/embed.py)

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ───────────────────────────── small math bits ─────────────────────────────
static ImVec2 operator+(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
static ImVec2 operator-(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }
static ImVec2 operator*(ImVec2 a, float k)  { return ImVec2(a.x * k, a.y * k); }
static float  vlen(ImVec2 v) { return sqrtf(v.x * v.x + v.y * v.y); }

struct WRect { ImVec2 mn{0,0}, mx{0,0};
    bool contains(ImVec2 p) const { return p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y; }
    void include(ImVec2 p) { mn.x = fminf(mn.x, p.x); mn.y = fminf(mn.y, p.y); mx.x = fmaxf(mx.x, p.x); mx.y = fmaxf(mx.y, p.y); }
    void include(const WRect& r) { include(r.mn); include(r.mx); }
    ImVec2 size() const { return mx - mn; }
    ImVec2 center() const { return (mn + mx) * 0.5f; }
};
static ImVec2 rot_about(ImVec2 p, ImVec2 c, float a) {
    float s = sinf(a), co = cosf(a);
    ImVec2 d = p - c;
    return ImVec2(c.x + d.x * co - d.y * s, c.y + d.x * s + d.y * co);
}
static float dist_point_seg(ImVec2 p, ImVec2 a, ImVec2 b) {
    ImVec2 ab = b - a; float d2 = ab.x * ab.x + ab.y * ab.y;
    float t = d2 > 0 ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / d2 : 0;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return vlen(p - (a + ab * t));
}

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

// Read any GPU texture back as opaque RGBA8 via a staging copy.
static bool read_texture_rgba(ID3D11Texture2D* tex, std::vector<unsigned char>& px, int& w, int& h) {
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &staging))) return false;
    g_ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) { staging->Release(); return false; }
    w = (int)d.Width; h = (int)d.Height;
    px.resize((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        memcpy(&px[(size_t)y * w * 4], (unsigned char*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
    g_ctx->Unmap(staging, 0);
    staging->Release();
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;   // force opaque alpha
    return true;
}

// Save the current backbuffer as a PNG (the --shot verification path).
static bool SaveBackbufferPNG(const char* path) {
    ID3D11Texture2D* bb = nullptr;
    g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (!bb) return false;
    std::vector<unsigned char> px; int w = 0, h = 0;
    bool ok = read_texture_rgba(bb, px, w, h);
    bb->Release();
    return ok && stbi_write_png(path, w, h, 4, px.data(), w * 4) != 0;
}

// ───────────────────────────────── theme ───────────────────────────────────
struct Theme {
    ImU32 canvasBg, gridDot, panelBg, panelBorder, accent, textMain, textDim;
    ImU32 selStroke, selFill, handleFill;
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
        g_th.handleFill  = IM_COL32(28, 30, 34, 255);
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
        g_th.handleFill  = IM_COL32(255, 255, 255, 255);
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

// ── ink palette ──
// 12 slots, tuned per theme so a violet reads as violet on both backgrounds.
// Slot 0 = the theme's default ink. Shapes store the INDEX, not the color, so
// boards restyle themselves when the theme flips.
static const ImU32 kPaletteDark[12] = {
    0,                              // 0: theme ink (resolved at draw)
    IM_COL32(148, 152, 162, 255),   // grey
    IM_COL32(196, 154, 108, 255),   // brown
    IM_COL32(178, 132, 255, 255),   // violet
    IM_COL32( 96, 146, 255, 255),   // blue
    IM_COL32( 84, 196, 236, 255),   // cyan
    IM_COL32( 52, 211, 153, 255),   // green
    IM_COL32(163, 230,  53, 255),   // lime
    IM_COL32(250, 204,  21, 255),   // yellow
    IM_COL32(251, 146,  60, 255),   // orange
    IM_COL32(248, 113, 113, 255),   // red
    IM_COL32(244, 114, 182, 255),   // pink
};
static const ImU32 kPaletteLight[12] = {
    0,
    IM_COL32(107, 114, 128, 255),
    IM_COL32(146, 105,  60, 255),
    IM_COL32(124,  58, 237, 255),
    IM_COL32( 37,  99, 235, 255),
    IM_COL32(  8, 145, 178, 255),
    IM_COL32(  5, 150, 105, 255),
    IM_COL32(101, 163,  13, 255),
    IM_COL32(202, 138,   4, 255),
    IM_COL32(234,  88,  12, 255),
    IM_COL32(220,  38,  38, 255),
    IM_COL32(219,  39, 119, 255),
};
static ImU32 palette_color(int idx) {
    if (idx <= 0 || idx >= 12) return g_th.textMain;
    return (g_darkMode ? kPaletteDark : kPaletteLight)[idx];
}
static ImU32 with_opacity(ImU32 c, float op) {
    int a = (int)((c >> 24) * (op < 0 ? 0 : op > 1 ? 1 : op));
    return (c & 0x00FFFFFF) | ((ImU32)a << 24);
}

// ───────────────────────────────── fonts ───────────────────────────────────
// Four families, dynamic atlas (imgui ≥1.92): glyphs rasterize on demand at
// whatever pixel size we push, so canvas text is crisp at every zoom level.
enum FontFamily { FF_HAND = 0, FF_SANS, FF_MONO, FF_SERIF, FF_COUNT };
static ImFont* g_fonts[FF_COUNT] = {};
static const char* kFamilyName[FF_COUNT] = { "hand", "sans", "mono", "serif" };
// four canvas text sizes, big-by-default (M = the old L) per the main use case
static const float kTextSizes[4] = { 24.f, 40.f, 56.f, 80.f };
static const char* kTextSizeName[4] = { "S", "M", "L", "XL" };
static const int   kDefaultTextSize = 1;   // M (40px)
// Glyphs above this rasterized px size get drawn as scaled-up smaller glyphs
// (keeps the dynamic atlas from ballooning when zoomed way in).
static const float kMaxGlyphPx = 320.f;

static void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg; cfg.FontDataOwnedByAtlas = false;
    cfg.RasterizerMultiply = 1.35f;   // heavier coverage — reads bolder everywhere
    struct { const unsigned char* data; unsigned int len; const char* name; } specs[FF_COUNT] = {
        { font_hand,  font_hand_len,  "hand"  },
        { font_sans,  font_sans_len,  "sans"  },
        { font_mono,  font_mono_len,  "mono"  },
        { font_serif, font_serif_len, "serif" },
    };
    for (int i = 0; i < FF_COUNT; i++) {
        snprintf(cfg.Name, sizeof(cfg.Name), "%s", specs[i].name);
        g_fonts[i] = io.Fonts->AddFontFromMemoryTTF((void*)specs[i].data, (int)specs[i].len, 0.f, &cfg);
    }
    io.FontDefault = g_fonts[FF_SANS];   // UI chrome reads better in sans; canvas text defaults to hand
}

// ───────────────────────────────── camera ──────────────────────────────────
// screen = world * zoom + pan   (pan in screen px). Zoom pivots on the cursor.
struct Camera { ImVec2 pan{0, 0}; float zoom = 1.f; };
static Camera g_cam;

static ImVec2 W2S(ImVec2 w) { return ImVec2(w.x * g_cam.zoom + g_cam.pan.x, w.y * g_cam.zoom + g_cam.pan.y); }
static ImVec2 S2W(ImVec2 s) { return ImVec2((s.x - g_cam.pan.x) / g_cam.zoom, (s.y - g_cam.pan.y) / g_cam.zoom); }

// short eased camera flights for the zoom commands (wheel/pan stay immediate
// and cancel any flight in progress — direct input always wins)
struct CamAnim { bool active = false; double t0 = 0; ImVec2 pan0, pan1; float z0 = 1, z1 = 1; };
static CamAnim g_camAnim;

static void ZoomAt(ImVec2 pivot, float factor) {
    g_camAnim.active = false;
    float z = g_cam.zoom * factor;
    z = z < 0.02f ? 0.02f : (z > 64.f ? 64.f : z);
    factor = z / g_cam.zoom;
    g_cam.pan.x = pivot.x - (pivot.x - g_cam.pan.x) * factor;
    g_cam.pan.y = pivot.y - (pivot.y - g_cam.pan.y) * factor;
    g_cam.zoom = z;
}

static bool g_zoomAnim_fwd();   // g_zoomAnim lives with the doc state below
static void start_cam_anim(ImVec2 pan1, float z1) {
    if (!g_zoomAnim_fwd()) { g_cam.pan = pan1; g_cam.zoom = z1; g_camAnim.active = false; return; }
    g_camAnim = { true, ImGui::GetTime(), g_cam.pan, pan1, g_cam.zoom, z1 };
}
static void tick_cam_anim() {
    if (!g_camAnim.active) return;
    const float dur = 0.28f;
    float u = (float)((ImGui::GetTime() - g_camAnim.t0) / dur);
    if (u >= 1.f) { u = 1.f; g_camAnim.active = false; }
    // ease-in-out quart: gentle start, gentle landing — no snap at either end
    float e = u < 0.5f ? 8.f * u * u * u * u : 1.f - powf(-2.f * u + 2.f, 4.f) * 0.5f;
    g_cam.zoom = expf(logf(g_camAnim.z0) + (logf(g_camAnim.z1) - logf(g_camAnim.z0)) * e);
    g_cam.pan = g_camAnim.pan0 + (g_camAnim.pan1 - g_camAnim.pan0) * e;
}

// fly the camera so `r` (world) fits the viewport with a margin, capped at
// 100% — wheel in from there if you want closer
static void zoom_to_rect(const WRect& r, ImVec2 vpSize) {
    ImVec2 sz = r.size();
    if (sz.x < 1 && sz.y < 1) return;
    const float pad = 72.f;
    float z = fminf((vpSize.x - pad * 2) / fmaxf(sz.x, 1.f),
                    (vpSize.y - pad * 2) / fmaxf(sz.y, 1.f));
    z = fminf(fmaxf(z, 0.02f), 1.f);
    ImVec2 c = r.center();
    start_cam_anim(ImVec2(vpSize.x * 0.5f - c.x * z, vpSize.y * 0.5f - c.y * z), z);
}
static void zoom_to_100(ImVec2 vpSize) {
    ImVec2 c = S2W(vpSize * 0.5f);   // keep the view center put
    start_cam_anim(ImVec2(vpSize.x * 0.5f - c.x, vpSize.y * 0.5f - c.y), 1.f);
}

// ─────────────────────────── document model ────────────────────────────────
enum ShapeType { SH_TEXT = 0, SH_ARROW, SH_IMAGE, SH_GROUP };

struct ArrowEnd {
    ImVec2  p{0, 0};        // world point (used when unbound)
    uint64_t bind = 0;      // bound shape id (0 = unbound)
    ImVec2  anchor{0.5f, 0.5f};  // normalized point inside the bound shape's bounds
};

struct Shape {
    uint64_t id = 0;
    ShapeType type = SH_TEXT;
    uint64_t parent = 0;    // enclosing group id (0 = top level)
    int   col = 0;          // palette index (0 = theme default ink)
    float opacity = 1.f;
    // text
    int   align = 0;        // 0 left · 1 center · 2 right
    std::string text;
    int   family = FF_HAND;
    int   tsize = kDefaultTextSize;
    float scale = 1.f;      // continuous scale from corner-resize (multiplies font px)
    ImVec2 pos{0, 0};       // text/image local-rect top-left (world, pre-rotation)
    float rot = 0.f;        // radians, about the local rect's center
    // image / video
    std::string asset;
    ImVec2 size{0, 0};
    ImVec4 crop{0, 0, 1, 1};    // visible sub-rect of the source (u0,v0,u1,v1)
    float loopA = -1, loopB = -1;   // video A-B loop points (seconds; -1 = unset)
    // arrow
    ArrowEnd a, b;
    float bend = 0.f;       // signed offset of the on-curve midpoint ⊥ to the chord
    std::string label;
};

struct Doc {
    std::vector<Shape> shapes;   // draw order = z order
    uint64_t nextId = 1;
};
static Doc g_doc;
static std::string g_projDir;    // project dir (board.json + assets/ + undo.jsonl)

static void delete_shapes(const std::vector<uint64_t>& ids);   // fwd (load-time sanitize)
// id → doc index, memoized: arrows resolve their bind target every frame, so
// a linear scan is O(arrows·shapes). Each hit is validated against the live
// vector (inserts/erases/reorders just fall back to a rescan) — never stale.
static std::unordered_map<uint64_t, int> g_idIndex;
static Shape* find_shape(uint64_t id) {
    if (!id) return nullptr;
    auto it = g_idIndex.find(id);
    if (it != g_idIndex.end()) {
        int i = it->second;
        if (i < (int)g_doc.shapes.size() && g_doc.shapes[i].id == id) return &g_doc.shapes[i];
    }
    for (int i = 0; i < (int)g_doc.shapes.size(); i++)
        if (g_doc.shapes[i].id == id) { g_idIndex[id] = i; return &g_doc.shapes[i]; }
    return nullptr;
}
static int find_index(uint64_t id) {
    for (int i = 0; i < (int)g_doc.shapes.size(); i++) if (g_doc.shapes[i].id == id) return i;
    return -1;
}
// all descendant leaf/group ids of a group (including nested groups' members)
static void collect_members(uint64_t gid, std::vector<uint64_t>& out) {
    for (auto& s : g_doc.shapes)
        if (s.parent == gid) { out.push_back(s.id); if (s.type == SH_GROUP) collect_members(s.id, out); }
}

// do all rotatable members of a group share one rotation? (used to seed a new
// group's frame and to migrate legacy boards to stored group rotation)
static bool members_common_rot(uint64_t gid, float* out) {
    std::vector<uint64_t> m; collect_members(gid, m);
    bool any = false; float r = 0;
    for (auto id : m) {
        Shape* c = find_shape(id);
        if (!c || (c->type != SH_TEXT && c->type != SH_IMAGE)) continue;
        if (!any) { r = c->rot; any = true; }
        else if (fabsf(c->rot - r) > 0.001f) return false;
    }
    if (!any) return false;
    *out = r;
    return true;
}

// ── shape geometry ──
static float text_px(const Shape& s) { return kTextSizes[s.tsize] * s.scale; }

// CalcTextSize is the hot path at 1000s of shapes — bounds, hit tests, draw,
// marquee and snapping all funnel through text_extent. Cache per shape id,
// validated against every input that shapes the metric (deterministic for a
// given font+px+text, so no time-based invalidation needed).
struct TextExt { int family = -1; float px = 0; std::string text; ImVec2 ext; };
static std::unordered_map<uint64_t, TextExt> g_extCache;
static ImVec2 text_extent(const Shape& s) {
    float px = text_px(s);
    auto it = g_extCache.find(s.id);
    if (it != g_extCache.end()) {
        TextExt& c = it->second;
        if (c.family == s.family && c.px == px && c.text == s.text) return c.ext;
    }
    ImFont* f = g_fonts[s.family];
    const char* txt = s.text.empty() ? " " : s.text.c_str();
    ImGui::PushFont(f, px);
    ImVec2 sz = ImGui::CalcTextSize(txt);
    ImGui::PopFont();
    g_extCache[s.id] = { s.family, px, s.text, sz };
    return sz;
}

// Text/image shapes live in a LOCAL axis-aligned rect (pos..pos+size/extent)
// rotated by `rot` about the rect's center. Everything geometric goes through
// these three helpers so rotation stays consistent in one place.
static WRect shape_local_rect(const Shape& s) {
    WRect r; r.mn = s.pos;
    r.mx = s.pos + (s.type == SH_TEXT ? text_extent(s) : s.size);
    return r;
}
static bool has_rot(const Shape& s) { return (s.type == SH_TEXT || s.type == SH_IMAGE) && s.rot != 0.f; }
// rotated corners, order: tl tr br bl (pad in local units expands the rect)
static void shape_obb(const Shape& s, ImVec2 out[4], float pad = 0.f) {
    WRect r = shape_local_rect(s);
    r.mn = r.mn - ImVec2(pad, pad); r.mx = r.mx + ImVec2(pad, pad);
    ImVec2 c = r.center();
    ImVec2 k[4] = { r.mn, ImVec2(r.mx.x, r.mn.y), r.mx, ImVec2(r.mn.x, r.mx.y) };
    for (int i = 0; i < 4; i++) out[i] = s.rot != 0.f ? rot_about(k[i], c, s.rot) : k[i];
}

static WRect shape_bounds(const Shape& s);   // fwd
static ImVec2 arrow_end_pos(const ArrowEnd& e) {
    if (e.bind) {
        Shape* t = find_shape(e.bind);
        if (t) {
            WRect b = shape_local_rect(*t);
            ImVec2 p(b.mn.x + b.size().x * e.anchor.x, b.mn.y + b.size().y * e.anchor.y);
            return t->rot != 0.f ? rot_about(p, b.center(), t->rot) : p;
        }
    }
    return e.p;
}

static WRect shape_bounds(const Shape& s) {
    WRect r;
    switch (s.type) {
    case SH_TEXT: case SH_IMAGE: {
        if (!has_rot(s)) return shape_local_rect(s);
        ImVec2 c[4]; shape_obb(s, c);
        r.mn = r.mx = c[0];
        for (int i = 1; i < 4; i++) r.include(c[i]);
    } break;
    case SH_ARROW: {
        ImVec2 pa = arrow_end_pos(s.a), pb = arrow_end_pos(s.b);
        r.mn = r.mx = pa; r.include(pb);
    } break;
    case SH_GROUP: {
        bool first = true;
        for (auto& c : g_doc.shapes) if (c.parent == s.id) {
            WRect cb = shape_bounds(c);
            if (first) { r = cb; first = false; } else r.include(cb);
        }
    } break;
    }
    return r;
}

// Quadratic-bezier arrow as a polyline (world space), endpoints trimmed at the
// bounds of bound shapes (with a small visual gap) so the line never overlaps
// the shape it points at — however far the user actually dragged.
static void arrow_polyline(const Shape& s, std::vector<ImVec2>& out) {
    ImVec2 A = arrow_end_pos(s.a), B = arrow_end_pos(s.b);
    ImVec2 chord = B - A; float cl = vlen(chord);
    ImVec2 perp = cl > 0.0001f ? ImVec2(-chord.y / cl, chord.x / cl) : ImVec2(0, -1);
    ImVec2 C = (A + B) * 0.5f + perp * (2.f * s.bend);   // control s.t. curve passes mid+bend·perp
    const int N = 48;
    out.clear(); out.reserve(N + 1);
    for (int i = 0; i <= N; i++) {
        float t = (float)i / N, u = 1.f - t;
        out.push_back(A * (u * u) + C * (2 * u * t) + B * (t * t));
    }
    float gap = 6.f / g_cam.zoom;
    auto trim = [&](const ArrowEnd& e, bool fromStart) {
        if (!e.bind) return;
        Shape* t = find_shape(e.bind); if (!t) return;
        // test against the target's OBB in its local frame — the AABB of a
        // rotated shape bulges and would eat the arrow (shrink during rotation)
        WRect b = shape_local_rect(*t);
        ImVec2 c = b.center(); float rt = t->rot;
        b.mn = b.mn - ImVec2(gap, gap); b.mx = b.mx + ImVec2(gap, gap);
        auto inside = [&](ImVec2 p) { return b.contains(rt != 0.f ? rot_about(p, c, -rt) : p); };
        // walk inward from this end, drop samples inside the (padded) box
        if (fromStart) {
            size_t i = 0;
            while (i + 1 < out.size() && inside(out[i])) i++;
            if (i > 0) out.erase(out.begin(), out.begin() + i);
        } else {
            size_t i = out.size();
            while (i > 1 && inside(out[i - 1])) i--;
            if (i < out.size()) out.erase(out.begin() + i, out.end());
        }
    };
    trim(s.a, true);
    trim(s.b, false);
}

// ── selection / interaction state ──
static std::vector<uint64_t> g_sel;
static uint64_t g_drill = 0;        // group id whose CHILDREN are directly selectable
static uint64_t g_editText = 0;     // text shape being edited
static bool     g_editTextTakeFocus = false;
static bool     g_editEverActive = false;   // editor item has held focus at least once
static bool     g_editRemap = false;        // pointer is remapped into a rotated editor's local space
static int      g_editCaretIdx = -1;        // byte offset to force the caret to on open (-1 = leave alone)
static std::string g_editPrev;      // last-frame text (escape-revert workaround)
static uint64_t g_editLabelArrow = 0;   // arrow whose label is being edited

static bool is_selected(uint64_t id) {
    for (auto s : g_sel) if (s == id) return true;
    return false;
}
static void clear_selection() { g_sel.clear(); g_drill = 0; }

static WRect selection_bounds() {
    WRect r; bool first = true;
    for (auto id : g_sel) { Shape* s = find_shape(id); if (!s) continue;
        WRect b = shape_bounds(*s);
        if (first) { r = b; first = false; } else r.include(b); }
    return r;
}

// ── persistence ──
static json shape_to_json(const Shape& s) {
    json j;
    j["id"] = s.id; j["type"] = (int)s.type;
    if (s.parent) j["parent"] = s.parent;
    if (s.col) j["col"] = s.col;
    if (s.opacity != 1.f) j["op"] = s.opacity;
    switch (s.type) {
    case SH_TEXT:
        j["text"] = s.text; j["family"] = s.family; j["tsize"] = s.tsize;
        if (s.scale != 1.f) j["scale"] = s.scale;
        j["x"] = s.pos.x; j["y"] = s.pos.y;
        if (s.rot != 0.f) j["rot"] = s.rot;
        if (s.align) j["algn"] = s.align;
        break;
    case SH_IMAGE:
        j["asset"] = s.asset; j["x"] = s.pos.x; j["y"] = s.pos.y;
        j["w"] = s.size.x; j["h"] = s.size.y;
        if (s.rot != 0.f) j["rot"] = s.rot;
        if (s.crop.x != 0 || s.crop.y != 0 || s.crop.z != 1 || s.crop.w != 1)
            j["crop"] = { s.crop.x, s.crop.y, s.crop.z, s.crop.w };
        if (s.loopA >= 0) j["loopA"] = s.loopA;
        if (s.loopB >= 0) j["loopB"] = s.loopB;
        break;
    case SH_ARROW: {
        auto end = [](const ArrowEnd& e) {
            json k; k["x"] = e.p.x; k["y"] = e.p.y;
            if (e.bind) { k["bind"] = e.bind; k["ax"] = e.anchor.x; k["ay"] = e.anchor.y; }
            return k;
        };
        j["a"] = end(s.a); j["b"] = end(s.b);
        if (s.bend != 0.f) j["bend"] = s.bend;
        if (!s.label.empty()) j["label"] = s.label;
    } break;
    case SH_GROUP:
        if (s.rot != 0.f) j["rot"] = s.rot;   // the group's persistent frame
        break;
    }
    return j;
}
static Shape shape_from_json(const json& j) {
    Shape s;
    s.id = j.value("id", 0ULL); s.type = (ShapeType)j.value("type", 0);
    s.parent = j.value("parent", 0ULL);
    s.col = j.value("col", 0);
    s.opacity = j.value("op", 1.f);
    s.align = j.value("algn", 0);
    switch (s.type) {
    case SH_TEXT:
        s.text = j.value("text", std::string());
        s.family = j.value("family", (int)FF_HAND); s.tsize = j.value("tsize", kDefaultTextSize);
        s.scale = j.value("scale", 1.f);
        s.pos = ImVec2(j.value("x", 0.f), j.value("y", 0.f));
        s.rot = j.value("rot", 0.f);
        break;
    case SH_IMAGE:
        s.asset = j.value("asset", std::string());
        s.pos = ImVec2(j.value("x", 0.f), j.value("y", 0.f));
        s.size = ImVec2(j.value("w", 0.f), j.value("h", 0.f));
        s.rot = j.value("rot", 0.f);
        if (j.contains("crop") && j["crop"].is_array() && j["crop"].size() == 4)
            s.crop = ImVec4(j["crop"][0], j["crop"][1], j["crop"][2], j["crop"][3]);
        s.loopA = j.value("loopA", -1.f);
        s.loopB = j.value("loopB", -1.f);
        break;
    case SH_ARROW: {
        auto end = [](const json& k) {
            ArrowEnd e; e.p = ImVec2(k.value("x", 0.f), k.value("y", 0.f));
            e.bind = k.value("bind", 0ULL);
            e.anchor = ImVec2(k.value("ax", 0.5f), k.value("ay", 0.5f));
            return e;
        };
        if (j.contains("a")) s.a = end(j["a"]);
        if (j.contains("b")) s.b = end(j["b"]);
        s.bend = j.value("bend", 0.f);
        s.label = j.value("label", std::string());
    } break;
    case SH_GROUP:
        s.rot = j.value("rot", 0.f);
        break;
    }
    return s;
}
static bool g_zoomAnim = true;   // eased camera flights for the zoom commands
static bool g_zoomAnim_fwd() { return g_zoomAnim; }
static bool g_settingsFromFile = false;   // %APPDATA% settings.json existed at boot
static std::string doc_to_json_string() {
    json j;
    j["v"] = 2; j["nextId"] = g_doc.nextId;
    j["cam"] = { {"x", g_cam.pan.x}, {"y", g_cam.pan.y}, {"z", g_cam.zoom} };
    json arr = json::array();
    for (auto& s : g_doc.shapes) arr.push_back(shape_to_json(s));
    j["shapes"] = std::move(arr);
    return j.dump();
}
static bool doc_from_json_string(const std::string& str, bool restoreCam) {
    json j = json::parse(str, nullptr, false);
    if (j.is_discarded()) return false;
    Doc d; d.nextId = j.value("nextId", 1ULL);
    for (auto& js : j.value("shapes", json::array())) d.shapes.push_back(shape_from_json(js));
    g_doc = std::move(d);
    // v1 → v2: the size ladder moved up one notch (old L=40px is the new M);
    // shift stored indices so every text keeps its pixel size
    if (j.value("v", 1) < 2)
        for (auto& s : g_doc.shapes)
            if (s.type == SH_TEXT && s.tsize > 0) s.tsize--;
    // ── sanitize ──
    // External edits or old bugs can leave a stale nextId (→ the app then
    // mints DUPLICATE ids: find_shape resolves the wrong shape, selections go
    // haywire) or invisible whitespace-only texts that marquees can catch.
    // Repair here so no session inherits the damage.
    for (auto& s : g_doc.shapes) if (s.id >= g_doc.nextId) g_doc.nextId = s.id + 1;
    {
        std::set<uint64_t> seen;
        for (auto& s : g_doc.shapes) {
            if (seen.count(s.id)) s.id = g_doc.nextId++;   // keep the first; later dupe gets a fresh id
            else seen.insert(s.id);
        }
    }
    {
        std::vector<uint64_t> dead;
        for (auto& s : g_doc.shapes) if (s.type == SH_TEXT && s.id != g_editText) {
            bool empty = true;
            for (char c : s.text) if (!isspace((unsigned char)c)) { empty = false; break; }
            if (empty) dead.push_back(s.id);
        }
        if (!dead.empty()) delete_shapes(dead);
    }
    // legacy boards predating stored group rotation: derive the frame once
    // from a uniform member rotation, so later per-child edits can't lose it
    for (auto& s : g_doc.shapes) if (s.type == SH_GROUP && s.rot == 0.f) {
        float r;
        if (members_common_rot(s.id, &r) && fabsf(r) > 0.0001f) s.rot = r;
    }
    if (restoreCam && j.contains("cam")) {
        g_cam.pan = ImVec2(j["cam"].value("x", 0.f), j["cam"].value("y", 0.f));
        g_cam.zoom = j["cam"].value("z", 1.f);
    }
    // theme prefs are global now (%APPDATA% settings.json); boards written
    // before that still carry them — adopt once when no settings file exists
    if (restoreCam && !g_settingsFromFile) {
        if (j.contains("dark")) g_darkMode = j["dark"];
        g_zoomAnim = j.value("zoomAnim", true);
    }
    // drop selection entries that no longer exist
    std::vector<uint64_t> keep;
    for (auto id : g_sel) if (find_shape(id)) keep.push_back(id);
    g_sel = keep;
    if (g_editText && !find_shape(g_editText)) g_editText = 0;
    if (g_drill && !find_shape(g_drill)) g_drill = 0;
    return true;
}

static double g_saveDueAt = 0;      // autosave debounce deadline (0 = clean)
static void write_file_atomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    { std::ofstream f(tmp, std::ios::binary); f << data; }
    MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}
static void save_board_now() {
    if (g_projDir.empty()) return;   // picker open at boot, no board yet
    write_file_atomic(g_projDir + "/board.json", doc_to_json_string());
    g_saveDueAt = 0;
}

// ── undo: full-document snapshots, journaled to disk so history survives
// sessions. Deliberately memory-piggy (user's call) — capped by undoLimit.
static std::vector<std::string> g_undo;   // snapshot stack; g_undoPos = current
static int g_undoPos = -1;
static int g_undoLimit = 4096;

static void undo_journal_rewrite() {
    if (g_projDir.empty()) return;
    std::string path = g_projDir + "/undo.jsonl";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    for (auto& s : g_undo) f << s << "\n";
}
static void push_undo() {
    if (g_projDir.empty()) return;
    std::string snap = doc_to_json_string();
    if (g_undoPos >= 0 && g_undo[g_undoPos] == snap) return;   // no-op gesture
    bool branched = (g_undoPos + 1 < (int)g_undo.size());
    g_undo.resize(g_undoPos + 1);
    g_undo.push_back(snap);
    if ((int)g_undo.size() > g_undoLimit) g_undo.erase(g_undo.begin());
    g_undoPos = (int)g_undo.size() - 1;
    if (branched) undo_journal_rewrite();
    else { std::ofstream f(g_projDir + "/undo.jsonl", std::ios::binary | std::ios::app); f << snap << "\n"; }
    g_saveDueAt = ImGui::GetTime() + 0.4;
}
static void apply_undo(int dir) {
    int np = g_undoPos + dir;
    if (np < 0 || np >= (int)g_undo.size()) return;
    g_undoPos = np;
    doc_from_json_string(g_undo[np], false);
    g_editText = 0; g_editLabelArrow = 0;
    g_saveDueAt = ImGui::GetTime() + 0.4;
}
static void load_board() {
    CreateDirectoryA(g_projDir.c_str(), nullptr);
    CreateDirectoryA((g_projDir + "/assets").c_str(), nullptr);
    std::ifstream f(g_projDir + "/board.json", std::ios::binary);
    if (f) {
        std::stringstream ss; ss << f.rdbuf();
        doc_from_json_string(ss.str(), true);
    }
    // undo journal: reload past sessions' history
    std::ifstream uj(g_projDir + "/undo.jsonl", std::ios::binary);
    if (uj) {
        std::string line;
        while (std::getline(uj, line)) if (!line.empty()) g_undo.push_back(line);
        if ((int)g_undo.size() > g_undoLimit)
            g_undo.erase(g_undo.begin(), g_undo.begin() + ((int)g_undo.size() - g_undoLimit));
    }
    if (g_undo.empty()) g_undo.push_back(doc_to_json_string());
    g_undoPos = (int)g_undo.size() - 1;
}

// ── document ops ──
static uint64_t new_id() { return g_doc.nextId++; }

static void delete_shapes(const std::vector<uint64_t>& ids) {
    std::vector<uint64_t> all = ids;
    for (auto id : ids) { Shape* s = find_shape(id); if (s && s->type == SH_GROUP) collect_members(id, all); }
    auto gone = [&](uint64_t id) { for (auto d : all) if (d == id) return true; return false; };
    // arrows bound to deleted shapes: freeze the endpoint at its current spot
    for (auto& s : g_doc.shapes) if (s.type == SH_ARROW) {
        if (s.a.bind && gone(s.a.bind)) { s.a.p = arrow_end_pos(s.a); s.a.bind = 0; }
        if (s.b.bind && gone(s.b.bind)) { s.b.p = arrow_end_pos(s.b); s.b.bind = 0; }
    }
    for (int i = (int)g_doc.shapes.size() - 1; i >= 0; i--)
        if (gone(g_doc.shapes[i].id)) g_doc.shapes.erase(g_doc.shapes.begin() + i);
    clear_selection();
}

static void move_shape(Shape& s, ImVec2 d) {
    switch (s.type) {
    case SH_TEXT: case SH_IMAGE: s.pos = s.pos + d; break;
    case SH_ARROW:
        if (!s.a.bind) s.a.p = s.a.p + d;
        if (!s.b.bind) s.b.p = s.b.p + d;
        break;
    case SH_GROUP: break;   // members move themselves
    }
}
static void move_selected(ImVec2 d) {
    std::vector<uint64_t> all = g_sel;
    for (auto id : g_sel) { Shape* s = find_shape(id); if (s && s->type == SH_GROUP) collect_members(id, all); }
    for (auto id : all) { Shape* s = find_shape(id); if (s) move_shape(*s, d); }
}

static void duplicate_selected() {
    std::vector<uint64_t> all = g_sel;
    for (auto id : g_sel) { Shape* s = find_shape(id); if (s && s->type == SH_GROUP) collect_members(id, all); }
    std::vector<std::pair<uint64_t, uint64_t>> remap;   // old → new
    std::vector<Shape> copies;
    for (auto& s : g_doc.shapes) {   // doc order keeps z-order of copies sane
        bool in = false; for (auto id : all) if (id == s.id) { in = true; break; }
        if (!in) continue;
        Shape c = s; c.id = new_id();
        remap.push_back({ s.id, c.id });
        copies.push_back(c);
    }
    auto remapped = [&](uint64_t old) -> uint64_t {
        for (auto& r : remap) if (r.first == old) return r.second;
        return 0;
    };
    ImVec2 off(24.f / g_cam.zoom, 24.f / g_cam.zoom);
    for (auto& c : copies) {
        if (c.parent) { uint64_t np = remapped(c.parent); c.parent = np; }
        if (c.type == SH_ARROW) {
            // keep bindings only when the target came along in the copy
            auto fix = [&](ArrowEnd& e) {
                if (!e.bind) { e.p = e.p + off; return; }
                uint64_t nb = remapped(e.bind);
                if (nb) e.bind = nb; else { e.p = arrow_end_pos(e) + off; e.bind = 0; }
            };
            fix(c.a); fix(c.b);
        } else move_shape(c, off);
        g_doc.shapes.push_back(c);
    }
    g_sel.clear();
    for (auto& r : remap) { Shape* s = find_shape(r.second); if (s && (!s->parent)) g_sel.push_back(r.second); }
}

static void group_selected() {
    if (g_sel.size() < 2) return;
    Shape g; g.id = new_id(); g.type = SH_GROUP;
    uint64_t gid = g.id;
    g_doc.shapes.push_back(g);
    for (auto id : g_sel) { Shape* s = find_shape(id); if (s) s->parent = gid; }
    float r;   // grouping same-tilt shapes: adopt that tilt as the group's frame
    if (members_common_rot(gid, &r) && fabsf(r) > 0.0001f) find_shape(gid)->rot = r;
    g_sel.clear(); g_sel.push_back(gid); g_drill = 0;
}
static void ungroup_selected() {
    std::vector<uint64_t> newSel;
    for (auto id : g_sel) {
        Shape* s = find_shape(id);
        if (!s || s->type != SH_GROUP) { newSel.push_back(id); continue; }
        for (auto& c : g_doc.shapes) if (c.parent == id) { c.parent = s->parent; newSel.push_back(c.id); }
        int i = find_index(id); if (i >= 0) g_doc.shapes.erase(g_doc.shapes.begin() + i);
    }
    g_sel = newSel; g_drill = 0;
}

static void reorder_selected(bool front) {
    std::vector<uint64_t> all = g_sel;
    for (auto id : g_sel) { Shape* s = find_shape(id); if (s && s->type == SH_GROUP) collect_members(id, all); }
    std::vector<Shape> moved, rest;
    for (auto& s : g_doc.shapes) {
        bool in = false; for (auto id : all) if (id == s.id) { in = true; break; }
        (in ? moved : rest).push_back(s);
    }
    g_doc.shapes.clear();
    if (front) { for (auto& s : rest) g_doc.shapes.push_back(s); for (auto& s : moved) g_doc.shapes.push_back(s); }
    else       { for (auto& s : moved) g_doc.shapes.push_back(s); for (auto& s : rest) g_doc.shapes.push_back(s); }
}

// ───────────────────────────── media / textures ────────────────────────────
static HWND g_hwnd = nullptr;

static std::wstring to_w(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
static std::string from_w(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

struct Tex { ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; };
static std::map<std::string, Tex> g_texCache;   // project-relative asset path → tex

static ID3D11ShaderResourceView* make_rgba_srv(const unsigned char* px, int w, int h) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { px, (UINT)(w * 4), 0 };
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&td, &sd, &tex))) return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    g_dev->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    return srv;
}

enum MediaKind { MK_STILL = 0, MK_GIF, MK_VIDEO };
static std::string lower_ext(const std::string& p) {
    size_t d = p.find_last_of('.');
    std::string e = d == std::string::npos ? "" : p.substr(d + 1);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    return e;
}
static MediaKind media_kind(const std::string& p) {
    std::string e = lower_ext(p);
    if (e == "gif") return MK_GIF;
    if (e == "mp4" || e == "mov" || e == "webm" || e == "mkv" || e == "avi" || e == "m4v") return MK_VIDEO;
    return MK_STILL;
}
static bool is_media_ext(const std::string& p) {
    std::string e = lower_ext(p);
    static const char* ok[] = { "png","jpg","jpeg","bmp","tga","gif","mp4","mov","webm","mkv","avi","m4v", nullptr };
    for (int i = 0; ok[i]; i++) if (e == ok[i]) return true;
    return false;
}

static Tex* get_image_tex(const std::string& asset) {
    auto it = g_texCache.find(asset);
    if (it != g_texCache.end()) return &it->second;
    Tex t;
    std::string path = g_projDir + "/" + asset;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (px) { t.srv = make_rgba_srv(px, w, h); t.w = w; t.h = h; stbi_image_free(px); }
    g_texCache[asset] = t;    // failures cached too — no per-frame retry spam
    return &g_texCache[asset];
}

static bool file_exists(const std::string& p) { return GetFileAttributesW(to_w(p).c_str()) != INVALID_FILE_ATTRIBUTES; }

// ─────────────────── video / gif decode (libav, in-process) ────────────────
// GIFs and videos decode in-process through the flake's static libav — the
// slopstudio-proven decoder: format+codec+swscale contexts stay RESIDENT per
// file, decode_index() does keyframe-seek + forward-decode, sequential
// playback reuses the held frame. GIFs autoplay-loop like images that move;
// videos behave exactly like images until hovered (play/stop/seek/AB overlay).
#ifdef TEI_LIBAV
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

struct VideoDecoder {
    AVFormatContext* fmt = nullptr;
    AVCodecContext*  dec = nullptr;
    SwsContext*      sws = nullptr;
    AVFrame*         frame = nullptr;
    AVPacket*        pkt = nullptr;
    int        vstream = -1;
    AVRational tb{0, 1};
    double     fps = 0;
    std::atomic<int> frames{0};   // decode (worker) learns the real count at EOF; UI reads it
    int        w = 0, h = 0;
    int        cur_idx = -1;
    bool       ok = false;
    unsigned long long lru = 0;
    std::mutex mx;   // held around every libav call — decode runs on the worker thread

    bool open(const std::string& path) {
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
        if (avformat_find_stream_info(fmt, nullptr) < 0) return false;
        vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vstream < 0) return false;
        AVStream* st = fmt->streams[vstream];
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) return false;
        dec = avcodec_alloc_context3(codec);
        if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0) return false;
        dec->thread_count = 0;                       // auto multithreaded decode
        if (avcodec_open2(dec, codec, nullptr) < 0) return false;
        tb = st->time_base;
        AVRational r = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
        fps = r.num ? av_q2d(r) : 12.0;
        w = dec->width; h = dec->height;
        if (st->nb_frames > 0) frames = (int)st->nb_frames;
        else {
            double dur = (st->duration > 0 && st->duration != AV_NOPTS_VALUE) ? st->duration * av_q2d(tb)
                       : (fmt->duration != AV_NOPTS_VALUE ? fmt->duration / (double)AV_TIME_BASE : 0);
            frames = dur > 0 ? (int)(dur * fps + 0.5) : 0;
        }
        frame = av_frame_alloc(); pkt = av_packet_alloc();
        ok = frame && pkt && w > 0 && h > 0;
        return ok;
    }
    void close() {
        if (sws) sws_freeContext(sws);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (dec) avcodec_free_context(&dec);
        if (fmt) avformat_close_input(&fmt);
        sws = nullptr; frame = nullptr; pkt = nullptr; dec = nullptr; fmt = nullptr; ok = false;
    }
    double duration() const { return fps > 0 ? frames / fps : 0; }
    bool to_rgba(std::vector<unsigned char>& out) {
        if (!frame || !frame->data[0]) return false;
        sws = sws_getCachedContext(sws, frame->width, frame->height, (AVPixelFormat)frame->format,
                                   w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return false;
        out.resize((size_t)w * h * 4);
        unsigned char* dst[4] = { out.data(), nullptr, nullptr, nullptr };
        int dstStride[4] = { w * 4, 0, 0, 0 };
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
        return true;
    }
    bool decode_index(int idx, std::vector<unsigned char>& out, bool retried = false) {
        if (!ok) return false;
        if (idx < 0) idx = 0;
        if (frames > 0 && idx >= frames) idx = frames - 1;
        if (idx == cur_idx) {
            if (to_rgba(out)) return true;
            cur_idx = -1;
        }
        int64_t target = (int64_t)((double)idx / fps / av_q2d(tb) + 0.5);
        bool needSeek = (cur_idx < 0) || (idx < cur_idx) || (idx - cur_idx > 30);
        if (needSeek) {
            if (av_seek_frame(fmt, vstream, target, AVSEEK_FLAG_BACKWARD) < 0)
                av_seek_frame(fmt, vstream, target, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
            avcodec_flush_buffers(dec);
            cur_idx = -1;
        }
        bool reached = false, eof = false;
        while (!reached && !eof) {
            int rp = av_read_frame(fmt, pkt);
            if (rp < 0) { avcodec_send_packet(dec, nullptr); eof = true; }
            else if (pkt->stream_index != vstream) { av_packet_unref(pkt); continue; }
            else { avcodec_send_packet(dec, pkt); av_packet_unref(pkt); }
            for (;;) {
                int rr = avcodec_receive_frame(dec, frame);
                if (rr == AVERROR(EAGAIN)) break;
                if (rr < 0) { eof = true; break; }
                int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
                cur_idx = (pts != AV_NOPTS_VALUE) ? (int)(pts * av_q2d(tb) * fps + 0.5) : (cur_idx + 1);
                if (pts == AV_NOPTS_VALUE || cur_idx >= idx || (frames > 0 && cur_idx >= frames - 1)) { reached = true; break; }
            }
        }
        // container metadata can overpromise: learn the REAL frame count at EOF
        if (eof && !reached && cur_idx >= 0 && cur_idx + 1 < frames) frames = cur_idx + 1;
        if (to_rgba(out)) return true;
        cur_idx = -1;
        if (!retried && frames > 0) return decode_index(frames - 1, out, true);
        return false;
    }
};

static std::map<std::string, VideoDecoder*> g_decoders;   // asset rel path → resident decoder
static unsigned long long g_decoderClock = 0;
static const size_t kMaxDecoders = 6;
static std::mutex g_decMx;   // guards g_decoders itself (lock order: g_decMx → decoder.mx)

// ── async decode (M4 "keep the UI thread pure") ──
// One worker thread runs every decode: seeks + forward-decode of long GOPs
// used to hitch the frame loop. The UI thread still OPENS decoders (imports
// need w/h synchronously) and decodes a shape's FIRST frame in-line (the
// poster shows the instant a video lands; --shot stays deterministic) —
// every later frame change is a request here. Latest-wins per shape, so
// scrubbing coalesces to the newest index instead of queueing every step.
struct DecodeReq { std::string asset; int idx = 0; unsigned gen = 0; };
struct DecodeRes { uint64_t shape = 0; std::string asset; int idx = 0, w = 0, h = 0;
                   unsigned gen = 0; std::vector<unsigned char> rgba; };
static std::mutex g_vqMx;                       // guards the queues below
static std::condition_variable g_vqCv;
static std::map<uint64_t, DecodeReq> g_vqWant;  // shape id → newest wanted frame
static std::vector<DecodeRes> g_vqDone;         // worker → UI (drained each frame)
static unsigned g_vqGen = 0;                    // bumped per board — stale results dropped
static bool g_vqQuit = false;
static std::thread g_vqWorker;

static void video_worker() {
    for (;;) {
        uint64_t shape; DecodeReq req;
        {
            std::unique_lock<std::mutex> lk(g_vqMx);
            g_vqCv.wait(lk, [] { return g_vqQuit || !g_vqWant.empty(); });
            if (g_vqQuit) return;
            auto it = g_vqWant.begin();
            shape = it->first; req = std::move(it->second);
            g_vqWant.erase(it);
        }
        DecodeRes res; res.shape = shape; res.asset = req.asset; res.idx = req.idx; res.gen = req.gen;
        VideoDecoder* d = nullptr;
        std::unique_lock<std::mutex> dlk;
        {
            // acquire the decoder's mutex while g_decMx pins the map entry, so
            // an evict/close on the UI thread can never delete it under us
            std::lock_guard<std::mutex> lk(g_decMx);
            auto it = g_decoders.find(req.asset);
            if (it != g_decoders.end() && it->second->ok) {
                d = it->second;
                dlk = std::unique_lock<std::mutex>(d->mx);
            }
        }
        if (d) {
            res.w = d->w; res.h = d->h;
            if (!d->decode_index(req.idx, res.rgba)) res.rgba.clear();
            dlk.unlock();
        }
        {   // failures report back too (empty rgba): the UI clears its
            // pending mark so the frame can be re-requested
            std::lock_guard<std::mutex> lk(g_vqMx);
            g_vqDone.push_back(std::move(res));
        }
    }
}

static VideoDecoder* get_decoder(const std::string& asset) {
    std::lock_guard<std::mutex> lk(g_decMx);
    if (!g_vqWorker.joinable()) g_vqWorker = std::thread(video_worker);
    auto it = g_decoders.find(asset);
    if (it != g_decoders.end()) { it->second->lru = ++g_decoderClock; return it->second->ok ? it->second : nullptr; }
    if (g_decoders.size() >= kMaxDecoders) {
        auto v = g_decoders.end();
        for (auto i = g_decoders.begin(); i != g_decoders.end(); ++i)
            if (v == g_decoders.end() || i->second->lru < v->second->lru) v = i;
        if (v != g_decoders.end()) {
            { std::lock_guard<std::mutex> dl(v->second->mx); v->second->close(); }   // wait out an in-flight decode
            delete v->second; g_decoders.erase(v);
        }
    }
    static bool quieted = false;
    if (!quieted) { av_log_set_level(AV_LOG_ERROR); quieted = true; }
    VideoDecoder* d = new VideoDecoder();
    d->lru = ++g_decoderClock;
    if (!d->open(g_projDir + "/" + asset)) d->close();
    g_decoders[asset] = d;
    return d->ok ? d : nullptr;
}
#endif // TEI_LIBAV

// Per-shape playback state; the dynamic texture is updated only when the
// wanted frame index actually changes.
struct PlayState {
    bool playing = false;
    double t = 0;
    int shownIdx = -1;
    int reqIdx = -1;    // frame index requested from the decode worker, not yet delivered
    int w = 0, h = 0;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    void release() { if (srv) srv->Release(); if (tex) tex->Release(); srv = nullptr; tex = nullptr; shownIdx = -1; reqIdx = -1; }
};
static std::map<uint64_t, PlayState> g_play;

#ifdef TEI_LIBAV
static bool upload_rgba(PlayState& ps, const unsigned char* rgba, int w, int h) {
    if (!ps.tex || ps.w != w || ps.h != h) {
        ps.release();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &ps.tex))) return false;
        g_dev->CreateShaderResourceView(ps.tex, nullptr, &ps.srv);
        ps.w = w; ps.h = h;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(ps.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    for (int y = 0; y < ps.h; y++)
        memcpy((unsigned char*)m.pData + (size_t)y * m.RowPitch, rgba + (size_t)y * ps.w * 4, (size_t)ps.w * 4);
    g_ctx->Unmap(ps.tex, 0);
    return true;
}

// Advance playback + return the SRV for this video/gif shape (poster frame 0
// when idle). Called from the draw pass every frame the shape is visible.
static ID3D11ShaderResourceView* video_srv(const Shape& s, MediaKind mk) {
    VideoDecoder* d = get_decoder(s.asset);
    if (!d) return nullptr;
    PlayState& ps = g_play[s.id];
    if (mk == MK_GIF) ps.playing = true;   // gifs just loop, no controls
    if (ps.playing) {
        ps.t += ImGui::GetIO().DeltaTime;
        double dur = d->duration();
        if (mk == MK_VIDEO && s.loopA >= 0 && s.loopB > s.loopA && ps.t > s.loopB) ps.t = s.loopA;
        else if (dur > 0 && ps.t >= dur) ps.t = 0;
    }
    int idx = (int)(ps.t * d->fps + 0.5);
    if (d->frames > 0 && idx >= d->frames) idx = d->frames - 1;
    if (!ps.srv) {
        // first frame in-line (see the worker comment); later ones are async
        std::vector<unsigned char> rgba;
        bool ok;
        { std::lock_guard<std::mutex> lk(d->mx); ok = d->decode_index(idx, rgba); }
        if (ok && !rgba.empty() && upload_rgba(ps, rgba.data(), d->w, d->h)) ps.shownIdx = idx;
    } else if (idx != ps.shownIdx && idx != ps.reqIdx) {
        {
            std::lock_guard<std::mutex> lk(g_vqMx);
            g_vqWant[s.id] = { s.asset, idx, g_vqGen };
        }
        g_vqCv.notify_one();
        ps.reqIdx = idx;
    }
    return ps.srv;
}

// apply worker-decoded frames (called once per frame, before the draw pass)
static void drain_video_results() {
    std::vector<DecodeRes> done;
    { std::lock_guard<std::mutex> lk(g_vqMx); done.swap(g_vqDone); }
    for (auto& r : done) {
        if (r.gen != g_vqGen) continue;   // board switched while this decoded
        auto pit = g_play.find(r.shape);
        Shape* s = find_shape(r.shape);
        if (pit == g_play.end() || !s || s->asset != r.asset) continue;   // shape deleted/replaced
        PlayState& ps = pit->second;
        if (r.idx == ps.reqIdx) ps.reqIdx = -1;
        if (r.rgba.empty()) continue;     // decode failed; a later frame may retry
        // stale (superseded) results still upload — progressive scrub feedback
        if (upload_rgba(ps, r.rgba.data(), r.w, r.h)) ps.shownIdx = r.idx;
    }
}
#endif

// drop playback state (and its GPU texture) + cached text extents for shapes
// that no longer exist
static void sweep_play_states() {
    for (auto it = g_play.begin(); it != g_play.end();) {
        if (!find_shape(it->first)) { it->second.release(); it = g_play.erase(it); }
        else ++it;
    }
    for (auto it = g_extCache.begin(); it != g_extCache.end();) {
        if (!find_shape(it->first)) it = g_extCache.erase(it);
        else ++it;
    }
}

// Copy an external file into the project's assets/ (self-contained project
// dirs: every board carries its own media). ASCII-sanitized destination names
// so downstream ANSI file APIs (stb) never trip on unicode.
static std::string import_asset_file(const std::string& srcPathUtf8) {
    std::string base = srcPathUtf8;
    size_t sl = base.find_last_of("/\\");
    if (sl != std::string::npos) base = base.substr(sl + 1);
    std::string stem = base, ext;
    size_t d = base.find_last_of('.');
    if (d != std::string::npos) { stem = base.substr(0, d); ext = base.substr(d); }
    std::string clean;
    for (char c : stem) clean += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '-';
    if (clean.empty()) clean = "asset";
    std::string rel = "assets/" + clean + ext;
    for (int i = 2; file_exists(g_projDir + "/" + rel); i++)
        rel = "assets/" + clean + "-" + std::to_string(i) + ext;
    if (!CopyFileW(to_w(srcPathUtf8).c_str(), to_w(g_projDir + "/" + rel).c_str(), TRUE))
        return "";
    return rel;
}

static std::string next_paste_name(const char* ext) {
    for (int i = 1;; i++) {
        std::string rel = "assets/paste-" + std::to_string(i) + ext;
        if (!file_exists(g_projDir + "/" + rel)) return rel;
    }
}

// Default display size: fit within a 480-world-unit box, never upscale.
static ImVec2 default_display_size(int w, int h) {
    if (w <= 0 || h <= 0) return ImVec2(320, 240);
    float k = fminf(1.f, 480.f / (float)(w > h ? w : h));
    return ImVec2(w * k, h * k);
}

static uint64_t create_image_shape(const std::string& rel, ImVec2 centerW) {
    int w = 0, h = 0;
    if (media_kind(rel) == MK_STILL) { Tex* t = get_image_tex(rel); w = t->w; h = t->h; }
#ifdef TEI_LIBAV
    else { VideoDecoder* d = get_decoder(rel); if (d) { w = d->w; h = d->h; } }
#endif
    Shape s; s.id = new_id(); s.type = SH_IMAGE; s.asset = rel;
    s.size = default_display_size(w, h);
    s.pos = centerW - s.size * 0.5f;
    g_doc.shapes.push_back(s);
    return s.id;
}

static uint64_t hit_test(ImVec2 w);   // fwd

// Dropping media onto an existing image replaces its contents in place
// (the sketch-evolves workflow: paste a rough image, replace it later).
static void replace_image_contents(Shape& s, const std::string& rel) {
    s.asset = rel;
    Tex* t = get_image_tex(rel);
    if (t->w <= 0 || t->h <= 0) return;
    bool wasCropped = s.crop.x != 0 || s.crop.y != 0 || s.crop.z != 1 || s.crop.w != 1;
    float ia = (float)t->w / (float)t->h;
    if (wasCropped) {
        // the frame was deliberate: keep it exactly, cover-crop the new image
        // into it (centered), which also guarantees the crop stays in bounds
        float fa = s.size.x / fmaxf(s.size.y, 0.001f);
        if (ia > fa) { float cw = fa / ia; s.crop = ImVec4(0.5f - cw * 0.5f, 0, 0.5f + cw * 0.5f, 1); }
        else         { float ch = ia / fa; s.crop = ImVec4(0, 0.5f - ch * 0.5f, 1, 0.5f + ch * 0.5f); }
    } else {
        // keep the frame's area + center, adopt the new aspect
        float area = fmaxf(s.size.x * s.size.y, 1.f);
        ImVec2 c = s.pos + s.size * 0.5f;
        s.size.x = sqrtf(area * ia);
        s.size.y = s.size.x / ia;
        s.pos = c - s.size * 0.5f;
        s.crop = ImVec4(0, 0, 1, 1);
    }
    s.loopA = s.loopB = -1;
}

// The (world) rect the FULL source image projects to, given the current
// display rect + crop window. Fixed while a crop drag is in progress.
static WRect image_full_rect(const Shape& s) {
    ImVec2 cs(fmaxf(s.crop.z - s.crop.x, 0.001f), fmaxf(s.crop.w - s.crop.y, 0.001f));
    ImVec2 fsz(s.size.x / cs.x, s.size.y / cs.y);
    ImVec2 fpos(s.pos.x - fsz.x * s.crop.x, s.pos.y - fsz.y * s.crop.y);
    return { fpos, fpos + fsz };
}

static void import_files_at(const std::vector<std::string>& paths, ImVec2 atW) {
    ImVec2 cursor = atW;
    bool any = false;
    for (auto& p : paths) {
        if (!is_media_ext(p)) continue;
        std::string rel = import_asset_file(p);
        if (rel.empty()) continue;
        uint64_t hitId = paths.size() == 1 ? hit_test(atW) : 0;
        Shape* hitS = find_shape(hitId);
        if (hitS && hitS->type == SH_IMAGE && media_kind(rel) == MK_STILL) {
            replace_image_contents(*hitS, rel);
        } else {
            uint64_t id = create_image_shape(rel, cursor);
            g_sel.clear(); g_sel.push_back(id);
            cursor = cursor + ImVec2(32.f / g_cam.zoom, 32.f / g_cam.zoom);
        }
        any = true;
    }
    if (any) push_undo();
}

// pending drop from WM_DROPFILES, processed inside the frame
static std::vector<std::string> g_dropFiles;
static ImVec2 g_dropPoint;

// ── clipboard ──
static UINT fmt_shapes() { static UINT f = RegisterClipboardFormatA("teidraw_shapes"); return f; }
static UINT fmt_png()    { static UINT f = RegisterClipboardFormatA("PNG"); return f; }

static void copy_selection_to_clipboard(bool cut) {
    if (g_sel.empty()) return;
    std::vector<uint64_t> all = g_sel;
    for (auto id : g_sel) { Shape* s = find_shape(id); if (s && s->type == SH_GROUP) collect_members(id, all); }
    json arr = json::array();
    for (auto& s : g_doc.shapes) {   // doc order preserves z
        for (auto id : all) if (id == s.id) { arr.push_back(shape_to_json(s)); break; }
    }
    std::string payload = arr.dump();
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, payload.size() + 1);
    if (hg) {
        memcpy(GlobalLock(hg), payload.c_str(), payload.size() + 1);
        GlobalUnlock(hg);
        SetClipboardData(fmt_shapes(), hg);
    }
    CloseClipboard();
    if (cut) { delete_shapes(g_sel); push_undo(); }
}

static void paste_shapes_json(const std::string& payload, ImVec2 atW) {
    json arr = json::parse(payload, nullptr, false);
    if (arr.is_discarded() || !arr.is_array() || arr.empty()) return;
    std::vector<Shape> in;
    for (auto& js : arr) in.push_back(shape_from_json(js));
    // center the batch on the paste point
    WRect b; bool first = true;
    for (auto& s : in) if (s.type != SH_GROUP) {
        // bounds of foreign shapes: geometric fields only (no doc lookups for binds)
        WRect r;
        if (s.type == SH_ARROW) { r.mn = r.mx = s.a.p; r.include(s.b.p); }
        else { r.mn = s.pos; r.mx = s.pos + (s.type == SH_IMAGE ? s.size : text_extent(s)); }
        if (first) { b = r; first = false; } else b.include(r);
    }
    ImVec2 off = first ? ImVec2(0, 0) : atW - b.center();
    std::vector<std::pair<uint64_t, uint64_t>> remap;
    for (auto& s : in) { uint64_t old = s.id; s.id = new_id(); remap.push_back({ old, s.id }); }
    auto remapped = [&](uint64_t old) -> uint64_t {
        for (auto& r : remap) if (r.first == old) return r.second;
        return 0;
    };
    g_sel.clear();
    for (auto& s : in) {
        if (s.parent) s.parent = remapped(s.parent);
        if (s.type == SH_ARROW) {
            auto fix = [&](ArrowEnd& e) {
                if (e.bind) { uint64_t nb = remapped(e.bind); if (nb) { e.bind = nb; return; } e.bind = 0; }
                e.p = e.p + off;
            };
            fix(s.a); fix(s.b);
        } else s.pos = s.pos + off;
        g_doc.shapes.push_back(s);
        if (!s.parent) g_sel.push_back(s.id);
    }
    push_undo();
}

static void paste_clipboard(ImVec2 atW) {
    if (!OpenClipboard(g_hwnd)) return;
    bool done = false;

    if (HANDLE h = GetClipboardData(fmt_shapes())) {           // 1) our own shapes
        const char* p = (const char*)GlobalLock(h);
        if (p) { std::string payload(p); GlobalUnlock(h); CloseClipboard(); paste_shapes_json(payload, atW); return; }
    }
    if (HANDLE h = GetClipboardData(fmt_png())) {              // 2) PNG bytes (browsers etc.)
        void* p = GlobalLock(h);
        SIZE_T n = GlobalSize(h);
        if (p && n) {
            std::string rel = next_paste_name(".png");
            std::ofstream f(g_projDir + "/" + rel, std::ios::binary);
            f.write((const char*)p, (std::streamsize)n); f.close();
            GlobalUnlock(h);
            uint64_t id = create_image_shape(rel, atW);
            g_sel.clear(); g_sel.push_back(id);
            push_undo(); done = true;
        } else if (p) GlobalUnlock(h);
    }
    if (!done) if (HANDLE h = GetClipboardData(CF_DIB)) {      // 3) DIB → decode via stb (fake BMP header)
        BITMAPINFOHEADER* bi = (BITMAPINFOHEADER*)GlobalLock(h);
        if (bi) {
            SIZE_T n = GlobalSize(h);
            std::vector<unsigned char> bmp(14 + n);
            bmp[0] = 'B'; bmp[1] = 'M';
            uint32_t total = (uint32_t)(14 + n);
            memcpy(&bmp[2], &total, 4);
            uint32_t maskBytes = bi->biCompression == 3 /*BI_BITFIELDS*/ ? 12 : 0;
            uint32_t pxOff = 14 + bi->biSize + maskBytes +
                             (bi->biBitCount <= 8 ? (bi->biClrUsed ? bi->biClrUsed : (1u << bi->biBitCount)) * 4 : 0);
            memcpy(&bmp[10], &pxOff, 4);
            memcpy(&bmp[14], bi, n);
            GlobalUnlock(h);
            int w = 0, hh = 0, comp = 0;
            unsigned char* px = stbi_load_from_memory(bmp.data(), (int)bmp.size(), &w, &hh, &comp, 4);
            if (px) {
                std::string rel = next_paste_name(".png");
                stbi_write_png((g_projDir + "/" + rel).c_str(), w, hh, 4, px, w * 4);
                stbi_image_free(px);
                uint64_t id = create_image_shape(rel, atW);
                g_sel.clear(); g_sel.push_back(id);
                push_undo(); done = true;
            }
        }
    }
    if (!done) if (HANDLE h = GetClipboardData(CF_HDROP)) {    // 4) copied files
        HDROP hd = (HDROP)h;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::string> paths;
        for (UINT i = 0; i < n; i++) {
            wchar_t buf[MAX_PATH * 2];
            if (DragQueryFileW(hd, i, buf, MAX_PATH * 2)) paths.push_back(from_w(buf));
        }
        CloseClipboard();
        import_files_at(paths, atW);
        return;
    }
    if (!done) if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {   // 5) plain text → text shape
        wchar_t* p = (wchar_t*)GlobalLock(h);
        if (p) {
            std::string txt = from_w(p);
            GlobalUnlock(h);
            // normalize CRLF
            std::string t2; for (char c : txt) if (c != '\r') t2 += c;
            if (!t2.empty()) {
                Shape s; s.id = new_id(); s.type = SH_TEXT; s.text = t2;
                ImVec2 e = text_extent(s);
                s.pos = atW - e * 0.5f;
                g_doc.shapes.push_back(s);
                g_sel.clear(); g_sel.push_back(s.id);
                push_undo();
            }
        }
    }
    CloseClipboard();
}

// ──────────────── global settings (%APPDATA%) · board switching ────────────
// Per-USER prefs — theme, zoom animation, undo limit, boards home and the
// recent-board list — live in %APPDATA%/teidraw/settings.json (boards stay
// self-contained: board.json only holds the document + camera). recent[0] is
// the last board: a bare `teidraw` launch reopens it (first run: the picker).
static std::string g_settingsDir;               // %APPDATA%/teidraw ("" = unavailable)
static std::string g_boardsDir;                 // where "new board" mints dirs
static std::vector<std::string> g_recentBoards; // absolute dirs, most recent first
static bool g_headless = false;                 // --shot/--export: don't touch recents

static bool path_eq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = (char)tolower((unsigned char)a[i]), y = (char)tolower((unsigned char)b[i]);
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
        if (x != y) return false;
    }
    return true;
}
static std::string abs_path(const std::string& p) {
    wchar_t buf[2048];
    DWORD n = GetFullPathNameW(to_w(p).c_str(), 2048, buf, nullptr);
    return (n > 0 && n < 2048) ? from_w(buf) : p;
}
static std::string board_name(const std::string& dir) {
    std::string d = dir;
    while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    size_t sl = d.find_last_of("/\\");
    return sl == std::string::npos ? d : d.substr(sl + 1);
}
static bool board_exists(const std::string& dir) { return file_exists(dir + "/board.json"); }

static void save_settings() {
    if (g_settingsDir.empty()) return;
    json j;
    j["dark"] = g_darkMode;
    j["zoomAnim"] = g_zoomAnim;
    j["undoLimit"] = g_undoLimit;
    j["boardsDir"] = g_boardsDir;
    j["recent"] = g_recentBoards;
    write_file_atomic(g_settingsDir + "/settings.json", j.dump(2));
}
static void load_settings() {
    const char* app = getenv("APPDATA");
    if (app && *app) {
        g_settingsDir = std::string(app) + "/teidraw";
        CreateDirectoryA(g_settingsDir.c_str(), nullptr);
    }
    PWSTR docs = nullptr;   // default new-board home: Documents/teidraw
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        g_boardsDir = from_w(docs) + "\\teidraw";
        CoTaskMemFree(docs);
    } else if (!g_settingsDir.empty()) g_boardsDir = g_settingsDir + "/boards";
    if (g_settingsDir.empty()) return;
    std::ifstream f(g_settingsDir + "/settings.json", std::ios::binary);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded()) return;
    g_settingsFromFile = true;
    g_darkMode  = j.value("dark", true);
    g_zoomAnim  = j.value("zoomAnim", true);
    g_undoLimit = j.value("undoLimit", 4096);
    g_undoLimit = g_undoLimit < 8 ? 8 : (g_undoLimit > 262144 ? 262144 : g_undoLimit);
    g_boardsDir = j.value("boardsDir", g_boardsDir);
    for (auto& r : j.value("recent", json::array()))
        if (r.is_string() && board_exists(r)) g_recentBoards.push_back(r);   // dead dirs pruned here
}
static void note_board_opened(const std::string& absDir) {
    for (int i = (int)g_recentBoards.size() - 1; i >= 0; i--)
        if (path_eq(g_recentBoards[i], absDir)) g_recentBoards.erase(g_recentBoards.begin() + i);
    g_recentBoards.insert(g_recentBoards.begin(), absDir);
    if (g_recentBoards.size() > 10) g_recentBoards.resize(10);
    save_settings();
}

static void set_undo_limit(int n) {
    g_undoLimit = n;
    if ((int)g_undo.size() > n) {
        int drop = (int)g_undo.size() - n;
        g_undo.erase(g_undo.begin(), g_undo.begin() + drop);
        g_undoPos = g_undoPos < drop ? 0 : g_undoPos - drop;
        undo_journal_rewrite();
    }
    save_settings();
}

// Save the current board, tear down every per-board cache, load `dir`.
// A brand-new dir gets its board.json written immediately so it's a valid
// board (and recents entry) from second zero.
static void switch_board(const std::string& dirIn) {
    std::string dir = abs_path(dirIn);
    if (!g_projDir.empty()) {
        if (path_eq(abs_path(g_projDir), dir)) return;
        save_board_now();
    }
    g_doc = Doc{};
    g_undo.clear(); g_undoPos = -1;
    clear_selection(); g_editText = 0; g_editLabelArrow = 0;
    g_cam = Camera{}; g_camAnim.active = false;
    g_saveDueAt = 0;
    for (auto& [rel, t] : g_texCache) if (t.srv) t.srv->Release();
    g_texCache.clear();
    for (auto& [id, ps] : g_play) ps.release();
    g_play.clear();
    g_extCache.clear();
    g_idIndex.clear();
#ifdef TEI_LIBAV
    {   // invalidate in-flight decodes (gen bump) before touching the decoders
        std::lock_guard<std::mutex> lk(g_vqMx);
        g_vqGen++;
        g_vqWant.clear(); g_vqDone.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_decMx);
        for (auto& [rel, d] : g_decoders) {
            { std::lock_guard<std::mutex> dl(d->mx); d->close(); }   // wait out an in-flight decode
            delete d;
        }
        g_decoders.clear();
    }
#endif
    g_projDir = dir;
    load_board();
    if (!board_exists(dir)) save_board_now();
    if (!g_headless) note_board_opened(dir);
    if (g_hwnd) SetWindowTextW(g_hwnd, to_w(board_name(dir) + " — teidraw").c_str());
}

// Native folder dialog: open a board anywhere (an empty folder becomes a new
// board there). Show() pumps its own message loop; our frame just stalls.
static std::string pick_folder_dialog() {
    std::string out;
    HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"open board folder (empty folder = new board)");
        if (SUCCEEDED(dlg->Show(g_hwnd))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR w = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &w))) { out = from_w(w); CoTaskMemFree(w); }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (ci == S_OK || ci == S_FALSE) CoUninitialize();
    return out;
}

// ─────────────────────────────── hit testing ───────────────────────────────
static float screen_px(float px) { return px / g_cam.zoom; }   // px → world units

// topmost leaf under the point (never returns groups)
static uint64_t hit_test(ImVec2 w) {
    static std::vector<ImVec2> pl;   // reused across arrows (single-threaded)
    for (int i = (int)g_doc.shapes.size() - 1; i >= 0; i--) {
        Shape& s = g_doc.shapes[i];
        if (s.type == SH_GROUP) continue;
        if (s.type == SH_ARROW) {
            arrow_polyline(s, pl);
            float th = screen_px(8.f);
            for (size_t k = 0; k + 1 < pl.size(); k++)
                if (dist_point_seg(w, pl[k], pl[k + 1]) < th) return s.id;
            continue;
        }
        WRect b = shape_local_rect(s);
        ImVec2 p = s.rot != 0.f ? rot_about(w, b.center(), -s.rot) : w;
        if (b.contains(p)) return s.id;
    }
    return 0;
}

// Resolve which id a click on `leaf` should select, honoring group drill state:
// normally the outermost group; when drilled into a group, its direct child.
static uint64_t resolve_target(uint64_t leaf) {
    if (!leaf) return 0;
    Shape* s = find_shape(leaf);
    if (!s) return 0;
    // chain from leaf up to root
    std::vector<uint64_t> chain;   // leaf … outermost
    uint64_t cur = leaf;
    while (cur) { chain.push_back(cur); Shape* c = find_shape(cur); cur = c ? c->parent : 0; }
    if (g_drill) {
        // inside the drilled group? select the child of the drill level
        for (size_t i = 0; i + 1 < chain.size(); i++)
            if (chain[i + 1] == g_drill) return chain[i];
        // clicked outside the drilled group → pop drill, select outermost
        g_drill = 0;
    }
    return chain.back();
}

// ─────────────────────────────── rendering ─────────────────────────────────
static float g_dpi = 1.f;

static ImU32 shape_ink(const Shape& s) { return with_opacity(palette_color(s.col), s.opacity); }

// double-strike faux bold: second pass offset a fraction of the glyph size
static void add_text_bold(ImDrawList* dl, ImFont* f, float px, ImVec2 p, ImU32 col,
                          const char* b, const char* e = nullptr) {
    dl->AddText(f, px, p, col, b, e);
    dl->AddText(f, px, ImVec2(p.x + px * 0.03f, p.y), col, b, e);
}

// list lines ("• foo", "12. foo", optionally indented) pin to the left edge
// even when the text block is centered/right-aligned
static bool is_list_line(const char* b, const char* e) {
    while (b < e && *b == ' ') b++;
    if (e - b >= 3 && !memcmp(b, "\xe2\x80\xa2", 3)) return true;
    const char* d = b;
    while (d < e && isdigit((unsigned char)*d)) d++;
    return d > b && d < e && *d == '.' && (d + 1 == e || d[1] == ' ');
}

static void draw_text_shape(ImDrawList* dl, const Shape& s) {
    float px = text_px(s) * g_cam.zoom;
    ImVec2 sp = W2S(s.pos);
    ImU32 col = shape_ink(s);
    ImFont* f = g_fonts[s.family];
    float rp = fminf(px, kMaxGlyphPx);   // raster size; geometry scales the rest
    float k = px / rp;
    int vtx0 = dl->VtxBuffer.Size;
    // draw line by line at the origin so alignment offsets are easy, then
    // scale+translate (and rotate) the whole vertex range into place
    ImGui::PushFont(f, rp);
    float totalW = ImGui::CalcTextSize(s.text.c_str()).x;
    const char* b = s.text.c_str();
    const char* end = b + s.text.size();
    float y = 0;
    while (b < end) {
        const char* e = (const char*)memchr(b, '\n', end - b);
        if (!e) e = end;
        if (e > b) {
            float x = 0.f;
            if (s.align && !is_list_line(b, e)) {
                float w = ImGui::CalcTextSize(b, e).x;
                x = s.align == 1 ? (totalW - w) * 0.5f : (totalW - w);
            }
            add_text_bold(dl, f, rp, ImVec2(x, y), col, b, e);
        }
        y += rp;
        b = e < end ? e + 1 : end;
    }
    ImGui::PopFont();
    for (int i = vtx0; i < dl->VtxBuffer.Size; i++) {
        ImDrawVert& v = dl->VtxBuffer[i];
        v.pos.x = v.pos.x * k + sp.x; v.pos.y = v.pos.y * k + sp.y;
    }
    if (s.rot != 0.f) {
        ImVec2 c = W2S(shape_local_rect(s).center());
        float sn = sinf(s.rot), cs = cosf(s.rot);
        for (int i = vtx0; i < dl->VtxBuffer.Size; i++) {
            ImDrawVert& v = dl->VtxBuffer[i];
            float dx = v.pos.x - c.x, dy = v.pos.y - c.y;
            v.pos.x = c.x + dx * cs - dy * sn;
            v.pos.y = c.y + dx * sn + dy * cs;
        }
    }
}

static void draw_arrow_shape(ImDrawList* dl, const Shape& s, bool ghostEnd = false) {
    static std::vector<ImVec2> pl, sp;   // reused across calls (single-threaded)
    arrow_polyline(s, pl);
    if (pl.size() < 2) return;
    float thick = fmaxf(3.25f * g_cam.zoom, 2.f);
    ImU32 col = shape_ink(s);
    sp.resize(pl.size());
    for (size_t i = 0; i < pl.size(); i++) sp[i] = W2S(pl[i]);
    // reserve room at the tip for the head
    ImVec2 tip = sp.back();
    ImVec2 dir = sp[sp.size() - 1] - sp[sp.size() - 2];
    float dl2 = vlen(dir); if (dl2 > 0.0001f) dir = dir * (1.f / dl2);
    float head = fminf(fmaxf(11.f * g_cam.zoom, 9.f), 28.f);
    // shorten polyline by head length
    float remain = head * 0.8f;
    while (sp.size() > 1 && remain > 0) {
        ImVec2 seg = sp.back() - sp[sp.size() - 2];
        float L = vlen(seg);
        if (L > remain) { sp.back() = sp.back() - seg * (remain / L); break; }
        remain -= L; sp.pop_back();
    }
    dl->AddPolyline(sp.data(), (int)sp.size(), col, 0, thick);
    ImVec2 n(-dir.y, dir.x);
    dl->AddTriangleFilled(tip, tip - dir * head + n * (head * 0.45f), tip - dir * head - n * (head * 0.45f), col);
    // label: centered on the curve midpoint, canvas-colored box blanks the line
    if (!s.label.empty() && s.id != g_editLabelArrow) {
        ImVec2 mid = W2S(pl[pl.size() / 2]);
        float lpx = kTextSizes[0] * g_cam.zoom;
        ImGui::PushFont(g_fonts[s.family], lpx);
        ImVec2 ext = ImGui::CalcTextSize(s.label.c_str());
        ImGui::PopFont();
        ImVec2 pad(6.f * g_cam.zoom, 3.f * g_cam.zoom);
        ImVec2 mn = mid - ext * 0.5f - pad, mx = mid + ext * 0.5f + pad;
        dl->AddRectFilled(mn, mx, g_th.canvasBg, 4.f * g_cam.zoom);
        add_text_bold(dl, g_fonts[s.family], lpx, mid - ext * 0.5f, col, s.label.c_str());
    }
}

// doc order = z order. `only` (when non-null) limits drawing to those ids —
// the selection-export path renders just the selected shapes.
static void draw_doc_shapes(ImDrawList* dl, uint64_t skipId,
                            const std::vector<uint64_t>* only = nullptr) {
    auto in_only = [&](uint64_t id) {
        if (!only) return true;
        for (auto i : *only) if (i == id) return true;
        return false;
    };
    // Viewport culling: skip shapes fully outside the view. The pad covers
    // everything drawn past a shape's bounds (arrowheads, labels, faux-bold).
    // Offscreen gifs/videos also stop decoding here — playback resumes when
    // they scroll back in. (Export renders set DisplaySize to the export rect,
    // so the same test works there.)
    WRect view{ S2W(ImVec2(0, 0)), S2W(ImGui::GetIO().DisplaySize) };
    float pad = 64.f / g_cam.zoom;
    view.mn = view.mn - ImVec2(pad, pad); view.mx = view.mx + ImVec2(pad, pad);
    for (auto& s : g_doc.shapes) {
        if (s.id == skipId || s.type == SH_GROUP || !in_only(s.id)) continue;
        WRect b = shape_bounds(s);
        if (s.type == SH_ARROW && s.bend != 0.f) {
            // a bent curve bulges past the endpoint box: include the bezier
            // control point (the curve stays inside hull(A, C, B))
            ImVec2 A = arrow_end_pos(s.a), B = arrow_end_pos(s.b);
            ImVec2 ch = B - A; float cl = vlen(ch);
            if (cl > 0.0001f) b.include((A + B) * 0.5f + ImVec2(-ch.y / cl, ch.x / cl) * (2.f * s.bend));
        }
        if (b.mx.x < view.mn.x || b.mn.x > view.mx.x || b.mx.y < view.mn.y || b.mn.y > view.mx.y) continue;
        switch (s.type) {
        case SH_TEXT:  draw_text_shape(dl, s); break;
        case SH_ARROW: draw_arrow_shape(dl, s); break;
        case SH_IMAGE: {
            ImVec2 mn = W2S(s.pos), mx = W2S(s.pos + s.size);
            ID3D11ShaderResourceView* srv = nullptr;
            MediaKind mk = media_kind(s.asset);
            if (mk == MK_STILL) srv = get_image_tex(s.asset)->srv;
#ifdef TEI_LIBAV
            else srv = video_srv(s, mk);
#endif
            ImU32 tint = with_opacity(IM_COL32_WHITE, s.opacity);
            if (srv && s.rot == 0.f) {
                dl->AddImageRounded((ImTextureID)(intptr_t)srv, mn, mx,
                                    ImVec2(s.crop.x, s.crop.y), ImVec2(s.crop.z, s.crop.w),
                                    tint, 5.f);
            } else if (srv) {
                ImVec2 c[4]; shape_obb(s, c);
                ImVec2 sc[4]; for (int i = 0; i < 4; i++) sc[i] = W2S(c[i]);
                dl->AddImageQuad((ImTextureID)(intptr_t)srv, sc[0], sc[1], sc[2], sc[3],
                                 ImVec2(s.crop.x, s.crop.y), ImVec2(s.crop.z, s.crop.y),
                                 ImVec2(s.crop.z, s.crop.w), ImVec2(s.crop.x, s.crop.w), tint);
            } else {
                dl->AddRectFilled(mn, mx, IM_COL32(120, 120, 128, 50), 5.f);
                dl->AddRect(mn, mx, IM_COL32(120, 120, 128, 120), 5.f);
                dl->AddText(nullptr, 0.f, mn + ImVec2(10, 10), g_th.textDim, s.asset.c_str());
            }
        } break;
        case SH_GROUP: break;
        }
    }
}

// ──────────────────── LLM export (PNG · text outline) ──────────────────────
// Copy-as-PNG renders the board (or just the selection) offscreen at 2×, so
// a paste into a chat is crisp; "copy as text" dumps a reading-order outline
// an LLM can ingest without vision. Same paths back `--export`/`--export-txt`.

// bounds of every leaf shape on the board (groups contribute via their members)
static bool board_content_bounds(WRect& out) {
    bool first = true;
    for (auto& s : g_doc.shapes) {
        if (s.type == SH_GROUP) continue;
        WRect b = shape_bounds(s);
        if (first) { out = b; first = false; } else out.include(b);
    }
    return !first;
}

// A PNG export needs its own imgui frame (fonts rasterize per-zoom during
// draw), so requests made mid-frame are queued and run right after Present.
struct ExportJob {
    bool  active = false;
    WRect rect;                      // world rect captured at request time
    std::vector<uint64_t> only;      // empty = whole board
    std::string path;                // empty = clipboard
    bool  quit = false;              // CLI: exit when done
};
static ExportJob g_export;

// capture bounds + scope NOW (mid-frame, where text extents are safe to
// measure); the render itself runs between frames
static void request_png_export(bool selOnly, const char* path) {
    ExportJob j;
    if (selOnly && !g_sel.empty()) {
        j.rect = selection_bounds();
        j.only = g_sel;
        for (auto id : g_sel) { Shape* s = find_shape(id);
            if (s && s->type == SH_GROUP) collect_members(id, j.only); }
    } else if (!board_content_bounds(j.rect)) return;   // empty board — nothing to export
    const float pad = 32.f;
    j.rect.mn = j.rect.mn - ImVec2(pad, pad);
    j.rect.mx = j.rect.mx + ImVec2(pad, pad);
    j.active = true;
    if (path) j.path = path;
    g_export = std::move(j);
}

// render a world rect into an offscreen RT via a synthetic imgui frame
static bool render_rect_rgba(const WRect& r, const std::vector<uint64_t>* only,
                             std::vector<unsigned char>& px, int& w, int& h) {
    ImVec2 sz = r.size();
    if (sz.x < 1.f || sz.y < 1.f) return false;
    float scale = fminf(2.f, 8192.f / fmaxf(sz.x, sz.y));   // 2× unless the board is huge
    w = (int)ceilf(sz.x * scale); h = (int)ceilf(sz.y * scale);
    Camera saved = g_cam;
    g_cam.zoom = scale;
    g_cam.pan = ImVec2(-r.mn.x * scale, -r.mn.y * scale);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);   // after the backend snapshot, before NewFrame
    ImGui::NewFrame();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, g_th.canvasBg);
    draw_doc_shapes(dl, 0, only && !only->empty() ? only : nullptr);
    ImGui::Render();
    g_cam = saved;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w; td.Height = (UINT)h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr;
    bool ok = SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &tex)) &&
              SUCCEEDED(g_dev->CreateRenderTargetView(tex, nullptr, &rtv));
    if (ok) {
        ImVec4 bg = ImGui::ColorConvertU32ToFloat4(g_th.canvasBg);
        float c[4] = { bg.x, bg.y, bg.z, 1.f };
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        g_ctx->ClearRenderTargetView(rtv, c);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        ok = read_texture_rgba(tex, px, w, h);
    }
    if (rtv) rtv->Release();
    if (tex) tex->Release();
    return ok;
}

// PNG bytes + a CF_DIB fallback, so both browsers/chat apps and legacy
// Windows apps can paste it
static void set_clipboard_image(const unsigned char* rgba, int w, int h) {
    int plen = 0;
    unsigned char* png = stbi_write_png_to_mem(rgba, w * 4, w, h, 4, &plen);
    if (!OpenClipboard(g_hwnd)) { free(png); return; }
    EmptyClipboard();
    if (png) {
        if (HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, plen)) {
            memcpy(GlobalLock(hg), png, plen);
            GlobalUnlock(hg);
            SetClipboardData(fmt_png(), hg);
        }
        free(png);
    }
    size_t stride = (size_t)w * 4;
    if (HGLOBAL hd = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + stride * h)) {
        BITMAPINFOHEADER* bi = (BITMAPINFOHEADER*)GlobalLock(hd);
        memset(bi, 0, sizeof(*bi));
        bi->biSize = sizeof(*bi); bi->biWidth = w; bi->biHeight = h;   // bottom-up
        bi->biPlanes = 1; bi->biBitCount = 32; bi->biCompression = BI_RGB;
        unsigned char* dst = (unsigned char*)(bi + 1);
        for (int y = 0; y < h; y++) {
            const unsigned char* src = rgba + (size_t)(h - 1 - y) * stride;
            unsigned char* d = dst + (size_t)y * stride;
            for (int x = 0; x < w; x++) {
                d[x * 4 + 0] = src[x * 4 + 2]; d[x * 4 + 1] = src[x * 4 + 1];
                d[x * 4 + 2] = src[x * 4 + 0]; d[x * 4 + 3] = 255;
            }
        }
        GlobalUnlock(hd);
        SetClipboardData(CF_DIB, hd);
    }
    CloseClipboard();
}

// run the queued PNG export (call between frames); returns the job's quit flag
static bool run_pending_export() {
    ExportJob job = std::move(g_export);
    g_export = ExportJob{};
    std::vector<unsigned char> px; int w = 0, h = 0;
    bool ok = render_rect_rgba(job.rect, &job.only, px, w, h);
    if (ok) {
        if (!job.path.empty()) ok = stbi_write_png(job.path.c_str(), w, h, 4, px.data(), w * 4) != 0;
        else set_clipboard_image(px.data(), w, h);
    }
    if (!job.path.empty())
        fprintf(stderr, "teidraw: export %s -> %s\n", job.path.c_str(), ok ? "ok" : "FAILED");
    return job.quit;
}

// ── text outline ──
static std::string first_line_trunc(const std::string& t, size_t maxn = 48) {
    size_t e = t.find('\n');
    std::string s = t.substr(0, e == std::string::npos ? t.size() : e);
    if (s.size() > maxn) {
        s.resize(maxn);
        while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();  // utf-8 boundary
        s += "…";
    }
    return s;
}

static const char* media_word(const Shape& s) {
    MediaKind mk = media_kind(s.asset);
    return mk == MK_STILL ? "image" : mk == MK_GIF ? "gif" : "video";
}

// how an arrow endpoint reads in the outline: the bound shape's text/asset,
// or bare coordinates when unbound
static std::string outline_ref(const ArrowEnd& e) {
    if (Shape* t = find_shape(e.bind)) {
        if (t->type == SH_TEXT)  return "\"" + first_line_trunc(t->text) + "\"";
        if (t->type == SH_IMAGE) return "[" + std::string(media_word(*t)) + " " + t->asset + "]";
        return "[group]";
    }
    char buf[48];
    snprintf(buf, sizeof buf, "(%d, %d)", (int)lroundf(e.p.x), (int)lroundf(e.p.y));
    return buf;
}

// reading order: top→bottom, then left→right
static void outline_sort(std::vector<const Shape*>& v) {
    std::sort(v.begin(), v.end(), [](const Shape* a, const Shape* b) {
        WRect ba = shape_bounds(*a), bb = shape_bounds(*b);
        if (fabsf(ba.mn.y - bb.mn.y) > 1.f) return ba.mn.y < bb.mn.y;
        return ba.mn.x < bb.mn.x;
    });
}

static void outline_emit(const Shape& s, int depth, std::string& out) {
    std::string ind(depth * 2, ' ');
    WRect b = shape_bounds(s);
    char pos[48];
    snprintf(pos, sizeof pos, "(%d, %d)", (int)lroundf(b.mn.x), (int)lroundf(b.mn.y));
    if (s.type == SH_TEXT) {
        out += ind + "- text " + pos;
        if (s.text.find('\n') == std::string::npos) { out += ": \"" + s.text + "\"\n"; return; }
        out += ":\n";
        const char* p = s.text.c_str();
        const char* end = p + s.text.size();
        while (p < end) {
            const char* e = (const char*)memchr(p, '\n', end - p);
            if (!e) e = end;
            out += ind + "    " + std::string(p, e) + "\n";
            p = e < end ? e + 1 : end;
        }
    } else if (s.type == SH_IMAGE) {
        char dim[48];
        snprintf(dim, sizeof dim, "%dx%d", (int)lroundf(s.size.x), (int)lroundf(s.size.y));
        out += ind + "- " + media_word(s) + " " + pos + " " + dim + ": " + s.asset + "\n";
    } else if (s.type == SH_GROUP) {
        out += ind + "- group " + pos + ":\n";
        std::vector<const Shape*> kids;   // members of an included group are all in
        for (auto& c : g_doc.shapes) if (c.parent == s.id && c.type != SH_ARROW) kids.push_back(&c);
        outline_sort(kids);
        for (auto* k : kids) outline_emit(*k, depth + 1, out);
    }
}

// reading-order outline of the board (or the selection): texts verbatim,
// media as placeholders, groups nested, arrows as a connection list
static std::string board_outline(bool selOnly) {
    std::vector<uint64_t> scope;   // top-level ids to include
    bool useSel = selOnly && !g_sel.empty();
    if (useSel) scope = g_sel;
    else for (auto& s : g_doc.shapes) if (!s.parent) scope.push_back(s.id);

    std::vector<uint64_t> all = scope;   // + group members, for arrow collection
    for (auto id : scope) { Shape* s = find_shape(id);
        if (s && s->type == SH_GROUP) collect_members(id, all); }

    std::vector<const Shape*> roots;   // by id, not parent — a drilled selection sits inside a group
    for (auto& s : g_doc.shapes) {
        if (s.type == SH_ARROW) continue;
        for (auto id : scope) if (id == s.id) { roots.push_back(&s); break; }
    }
    outline_sort(roots);

    size_t n = 0;
    for (auto& s : g_doc.shapes) { for (auto id : all) if (id == s.id && s.type != SH_GROUP) { n++; break; } }
    std::string out = "# teidraw board";
    if (!g_projDir.empty()) out += " \"" + g_projDir + "\"";
    out += useSel ? " (selection, " : " (";
    out += std::to_string(n) + " shapes) — positions are canvas px, y grows downward\n\n";
    for (auto* r : roots) outline_emit(*r, 0, out);

    std::string arrows;
    for (auto& s : g_doc.shapes) {
        if (s.type != SH_ARROW) continue;
        bool in = false; for (auto id : all) if (id == s.id) { in = true; break; }
        if (!in) continue;
        arrows += "- " + outline_ref(s.a) + " -> " + outline_ref(s.b);
        if (!s.label.empty()) arrows += " (\"" + s.label + "\")";
        arrows += "\n";
    }
    if (!arrows.empty()) out += "\n## arrows\n" + arrows;
    return out;
}

static void copy_text_to_clipboard(const std::string& utf8) {
    std::wstring w = to_w(utf8);
    std::wstring crlf; crlf.reserve(w.size() + 64);
    for (wchar_t c : w) { if (c == L'\n') crlf += L'\r'; crlf += c; }
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    if (HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (crlf.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(hg), crlf.c_str(), (crlf.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(hg);
        SetClipboardData(CF_UNICODETEXT, hg);
    }
    CloseClipboard();
}

// ─────────────────────────── canvas interaction ────────────────────────────
enum DragMode { DM_NONE = 0, DM_PENDING, DM_MOVE, DM_MARQUEE, DM_HANDLE, DM_CROP, DM_ROTATE,
                DM_ARROW_A, DM_ARROW_B, DM_BEND, DM_NEW_ARROW, DM_PAN_R };
static DragMode g_drag = DM_NONE;
static ImVec2   g_dragStartW, g_dragStartS;    // world/screen at mousedown
static WRect    g_moveStartBounds;             // selection bounds when the move began
static ImVec2   g_moveApplied;                 // offset applied so far (move = absolute, not incremental)
static double   g_nudgeUndoAt = 0;             // arrow-key nudges coalesce into one undo entry
static uint64_t g_downLeaf = 0, g_downTarget = 0;
static bool     g_downWasSelected = false;
static int      g_handleIdx = -1;              // 0 tl, 1 tr, 2 br, 3 bl
static ImVec2   g_handleFixedW;                // resize: fixed (opposite) corner, world
static float    g_handleStartDist = 1.f;       // resize: mousedown distance to fixed corner
static ImVec2   g_rotPivotW;                   // rotate: pivot (selection center), world
static float    g_rotStartAngle = 0.f;
static ImVec2   g_rotCornersW[4];              // selection box at rotate start (world) — the box rotates rigidly

static float current_rot_delta(ImVec2 mw, bool shiftSnap) {
    float ang = atan2f(mw.y - g_rotPivotW.y, mw.x - g_rotPivotW.x);
    float dth = ang - g_rotStartAngle;
    if (shiftSnap) dth = roundf(dth / (IM_PI / 12.f)) * (IM_PI / 12.f);   // 15° steps
    return dth;
}
static std::vector<std::pair<uint64_t, Shape>> g_handleStartShapes;  // id → snapshot
static uint64_t g_newArrowId = 0;
static bool     g_rDrag = false;               // right button turned into a pan

enum Tool { TOOL_SELECT = 0, TOOL_HAND, TOOL_TEXT, TOOL_ARROW, TOOL_COUNT };
static Tool g_tool = TOOL_SELECT;
static bool g_spacePan = false;

static int g_editLastLen = -1;   // auto-list callback: deletion detection across calls

// The clicks that OPENED an editor (select shape → enter edit) must not chain
// into imgui's double/triple-click detection inside it — otherwise the first
// caret click counts as click #3 and select-line/select-word fire "randomly".
static void break_click_chain() { ImGui::GetIO().MouseClickedTime[0] = -1e9; }

static void begin_text_edit(uint64_t id, int caretIdx) {
    g_editText = id; g_editTextTakeFocus = true; g_editEverActive = false;
    g_editLastLen = -1;
    g_editCaretIdx = caretIdx;   // fresh caret every time — no selection carried over
    break_click_chain();
    Shape* s = find_shape(id);
    g_editPrev = s ? s->text : std::string();
    g_editLabelArrow = 0;
}
static void end_text_edit(bool commit) {
    Shape* s = find_shape(g_editText);
    if (s) {
        if (!commit) s->text = g_editPrev;
        // strip trailing whitespace-only content
        std::string t = s->text;
        bool empty = true; for (char c : t) if (!isspace((unsigned char)c)) { empty = false; break; }
        if (empty) { std::vector<uint64_t> one{ s->id }; delete_shapes(one); }
        push_undo();
    }
    g_editText = 0;
}

// the "current style": what the style panel shows with nothing selected, and
// what new shapes are born with (tldraw behavior)
static int   g_curCol = 0, g_curSize = kDefaultTextSize, g_curAlign = 0;
static float g_curOpacity = 1.f;

// run fn over every non-group shape in the selection (descending into groups);
// returns whether anything was touched
template <typename F>
static bool apply_to_selection(F fn) {
    if (g_sel.empty()) return false;
    std::vector<uint64_t> all = g_sel;
    for (auto id : g_sel) { Shape* s = find_shape(id); if (s && s->type == SH_GROUP) collect_members(id, all); }
    bool any = false;
    for (auto id : all) {
        Shape* s = find_shape(id);
        if (s && s->type != SH_GROUP) { fn(*s); any = true; }
    }
    return any;
}

static uint64_t create_text_at(ImVec2 w) {
    Shape s; s.id = new_id(); s.type = SH_TEXT; s.pos = w;
    s.tsize = g_curSize; s.col = g_curCol; s.align = g_curAlign; s.opacity = g_curOpacity;
    // place so the caret sits at the click (top-left minus half a line feels right)
    s.pos.y -= kTextSizes[s.tsize] * 0.5f;
    g_doc.shapes.push_back(s);
    return s.id;
}

// bind an arrow end to whatever shape sits under `w` (excluding the arrow itself)
static void try_bind(ArrowEnd& e, ImVec2 w, uint64_t selfId) {
    e.p = w; e.bind = 0;
    for (int i = (int)g_doc.shapes.size() - 1; i >= 0; i--) {
        Shape& s = g_doc.shapes[i];
        if (s.id == selfId || s.type == SH_ARROW || s.type == SH_GROUP) continue;
        WRect b = shape_local_rect(s);
        ImVec2 p = s.rot != 0.f ? rot_about(w, b.center(), -s.rot) : w;   // anchor in local frame
        if (b.contains(p)) {
            ImVec2 sz = b.size();
            e.bind = s.id;
            e.anchor = ImVec2(sz.x > 0 ? (p.x - b.mn.x) / sz.x : 0.5f,
                              sz.y > 0 ? (p.y - b.mn.y) / sz.y : 0.5f);
            return;
        }
    }
}

// ── move snapping (hold ctrl while dragging) ──
// Edges and centers of the moving bounds pull toward other top-level shapes'
// edges/centers within a screen-px threshold; each matched alignment draws an
// accent guide line through both boxes. Off by default — ctrl opts in; shift
// locks the drag to the dominant axis (both handled by the caller).
static void snap_move(ImVec2& want, const WRect& start, ImDrawList* dl) {
    std::vector<uint64_t> moving = g_sel;
    for (auto id : g_sel) { Shape* s = find_shape(id); if (s && s->type == SH_GROUP) collect_members(id, moving); }
    auto in_moving = [&](uint64_t id) { for (auto m : moving) if (m == id) return true; return false; };
    float th = screen_px(8.f);
    WRect mb{ start.mn + want, start.mx + want };
    float mine[2][3] = { { mb.mn.x, (mb.mn.x + mb.mx.x) * 0.5f, mb.mx.x },
                         { mb.mn.y, (mb.mn.y + mb.mx.y) * 0.5f, mb.mx.y } };
    float bestD[2] = { th, th };
    float bestOff[2] = { 0, 0 }, bestPos[2] = { 0, 0 };
    WRect bestBox[2]; bool hit[2] = { false, false };
    for (auto& s : g_doc.shapes) {
        if (s.parent || s.type == SH_ARROW || in_moving(s.id)) continue;
        WRect b = shape_bounds(s);
        float cand[2][3] = { { b.mn.x, (b.mn.x + b.mx.x) * 0.5f, b.mx.x },
                             { b.mn.y, (b.mn.y + b.mx.y) * 0.5f, b.mx.y } };
        for (int ax = 0; ax < 2; ax++)
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    float diff = cand[ax][i] - mine[ax][j];
                    if (fabsf(diff) < bestD[ax]) {
                        bestD[ax] = fabsf(diff); bestOff[ax] = diff;
                        bestPos[ax] = cand[ax][i]; bestBox[ax] = b; hit[ax] = true;
                    }
                }
    }
    if (hit[0]) want.x += bestOff[0];
    if (hit[1]) want.y += bestOff[1];
    if (!hit[0] && !hit[1]) return;
    WRect fb{ start.mn + want, start.mx + want };   // final (snapped) moving bounds
    if (hit[0]) {
        float y0 = fminf(fb.mn.y, bestBox[0].mn.y), y1 = fmaxf(fb.mx.y, bestBox[0].mx.y);
        dl->AddLine(W2S(ImVec2(bestPos[0], y0)), W2S(ImVec2(bestPos[0], y1)), g_th.accent, 1.f);
    }
    if (hit[1]) {
        float x0 = fminf(fb.mn.x, bestBox[1].mn.x), x1 = fmaxf(fb.mx.x, bestBox[1].mx.x);
        dl->AddLine(W2S(ImVec2(x0, bestPos[1])), W2S(ImVec2(x1, bestPos[1])), g_th.accent, 1.f);
    }
}

// Adaptive dot grid: pick the power-of-two multiple of the base spacing whose
// screen-space pitch lands in a comfy range; fade dots in as they spread out.
static void DrawGrid(ImDrawList* dl, ImVec2 size) {
    const float base = 32.f;
    float pitch = base * g_cam.zoom;
    float lvl = 1.f;
    while (pitch * lvl < 18.f)  lvl *= 2.f;
    while (pitch * lvl > 44.f)  lvl *= 0.5f;
    float step = pitch * lvl;
    float t = (step - 18.f) / (44.f - 18.f);
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

// If every rotatable leaf (text/image) in the selection shares one nonzero
// rotation, that's the selection's natural frame — a rotated group keeps its
// tilt when re-selected instead of snapping to an axis-aligned box. Arrows
// carry no rotation of their own and just tag along.
static bool selection_common_rot(float* out) {
    // each selected TOP-LEVEL entry contributes its frame: texts/images their
    // own rot, groups their STORED rot (so a group keeps its tilt even after
    // a child inside was rotated independently); arrows just tag along
    bool any = false; float r = 0;
    for (auto id : g_sel) {
        Shape* s = find_shape(id);
        if (!s || s->type == SH_ARROW) continue;
        if (!any) { r = s->rot; any = true; }
        else if (fabsf(s->rot - r) > 0.001f) return false;
    }
    if (!any || fabsf(r) < 0.0001f) return false;
    *out = r;
    return true;
}

// selection outline + corner handles (OBB for a single rotated shape or a
// common-rotation selection, AABB otherwise). Returns hover: 0-3 = corner
// handle, 4-7 = the rotate ring just OUTSIDE corner i-4, -1 = none.
// Corner order: tl tr br bl.
static ImVec2 g_selCorners[4];   // screen space
static ImVec2 g_selCenterS;
static int draw_selection_ui(ImDrawList* dl, bool handlesActive) {
    if (g_sel.empty()) return -1;
    if (g_drag == DM_ROTATE) {
        // while rotating, the box captured at gesture start turns rigidly with
        // the selection instead of re-fitting to a (bulging) AABB every frame
        ImGuiIO& io = ImGui::GetIO();
        float dth = current_rot_delta(S2W(io.MousePos), io.KeyShift);
        for (int i = 0; i < 4; i++) g_selCorners[i] = W2S(rot_about(g_rotCornersW[i], g_rotPivotW, dth));
        g_selCenterS = (g_selCorners[0] + g_selCorners[2]) * 0.5f;
        dl->AddPolyline(g_selCorners, 4, g_th.selStroke, ImDrawFlags_Closed, 1.5f);
        for (int i = 0; i < 4; i++) {
            dl->AddCircleFilled(g_selCorners[i], 5.f, g_th.handleFill);
            dl->AddCircle(g_selCorners[i], 5.f, g_th.selStroke, 0, 1.5f);
        }
        return -1;
    }
    Shape* single = g_sel.size() == 1 ? find_shape(g_sel[0]) : nullptr;
    float commonRot = 0;
    if (single && (single->type == SH_TEXT || single->type == SH_IMAGE)) {
        ImVec2 c[4]; shape_obb(*single, c, 4.f / g_cam.zoom);
        for (int i = 0; i < 4; i++) g_selCorners[i] = W2S(c[i]);
    } else if (selection_common_rot(&commonRot)) {
        // OBB at the shared rotation: gather everyone's corner points, fit an
        // axis-aligned box in the rotated frame, rotate the box back
        std::vector<uint64_t> all = g_sel;
        for (auto id : g_sel) { Shape* s = find_shape(id); if (s && s->type == SH_GROUP) collect_members(id, all); }
        std::vector<ImVec2> pts;
        for (auto id : all) {
            Shape* s = find_shape(id); if (!s) continue;
            if (s->type == SH_TEXT || s->type == SH_IMAGE) {
                ImVec2 c[4]; shape_obb(*s, c);
                for (int i = 0; i < 4; i++) pts.push_back(c[i]);
            } else if (s->type == SH_ARROW) {
                pts.push_back(arrow_end_pos(s->a));
                pts.push_back(arrow_end_pos(s->b));
            }
        }
        ImVec2 pivot = pts[0];
        WRect lr; bool first = true;
        for (ImVec2 p : pts) {
            ImVec2 q = rot_about(p, pivot, -commonRot);
            if (first) { lr.mn = lr.mx = q; first = false; } else lr.include(q);
        }
        float pad = 4.f / g_cam.zoom;
        lr.mn = lr.mn - ImVec2(pad, pad); lr.mx = lr.mx + ImVec2(pad, pad);
        ImVec2 k[4] = { lr.mn, ImVec2(lr.mx.x, lr.mn.y), lr.mx, ImVec2(lr.mn.x, lr.mx.y) };
        for (int i = 0; i < 4; i++) g_selCorners[i] = W2S(rot_about(k[i], pivot, commonRot));
    } else {
        WRect b = selection_bounds();
        ImVec2 mn = W2S(b.mn) - ImVec2(4, 4), mx = W2S(b.mx) + ImVec2(4, 4);
        g_selCorners[0] = mn; g_selCorners[1] = ImVec2(mx.x, mn.y);
        g_selCorners[2] = mx; g_selCorners[3] = ImVec2(mn.x, mx.y);
    }
    g_selCenterS = (g_selCorners[0] + g_selCorners[2]) * 0.5f;
    ImVec2 poly[4] = { g_selCorners[0], g_selCorners[1], g_selCorners[2], g_selCorners[3] };
    dl->AddPolyline(poly, 4, g_th.selStroke, ImDrawFlags_Closed, 1.5f);
    if (!handlesActive) return -1;
    int hover = -1;
    ImVec2 m = ImGui::GetIO().MousePos;
    const float r = 5.f;
    for (int i = 0; i < 4; i++)
        if (vlen(m - g_selCorners[i]) < r + 3) hover = i;
    if (hover < 0)   // rotate ring: a band just outside each corner handle
        for (int i = 0; i < 4; i++) {
            float d = vlen(m - g_selCorners[i]);
            if (d >= r + 3 && d < r + 17) hover = 4 + i;
        }
    for (int i = 0; i < 4; i++) {
        dl->AddCircleFilled(g_selCorners[i], r, g_th.handleFill);
        dl->AddCircle(g_selCorners[i], r, g_th.selStroke, 0, 1.5f);
    }
    if (hover >= 4) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return hover;
}

// per-frame arrow gizmos when exactly one arrow is selected: endpoint dots + bend handle
struct ArrowGizmo { bool active = false; ImVec2 pa, pb, mid; };
static ArrowGizmo arrow_gizmo(ImDrawList* dl) {
    ArrowGizmo gz;
    if (g_sel.size() != 1) return gz;
    Shape* s = find_shape(g_sel[0]);
    if (!s || s->type != SH_ARROW) return gz;
    std::vector<ImVec2> pl; arrow_polyline(*s, pl);
    if (pl.size() < 2) return gz;
    gz.active = true;
    gz.pa = W2S(pl.front()); gz.pb = W2S(pl.back()); gz.mid = W2S(pl[pl.size() / 2]);
    for (ImVec2 p : { gz.pa, gz.pb }) {
        dl->AddCircleFilled(p, 5.f, g_th.handleFill);
        dl->AddCircle(p, 5.f, g_th.selStroke, 0, 1.5f);
    }
    dl->AddCircleFilled(gz.mid, 4.f, g_th.selStroke);
    return gz;
}

// ─────────────────────────────── UI chrome ─────────────────────────────────
static const char* kToolLabel[TOOL_COUNT] = { "sel", "hand", "text", "arrow" };
static const char* kToolKey[TOOL_COUNT]   = { "V", "H", "T", "A" };

static void DrawToolbar() {
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

static void DrawZoomPill() {
    ImGui::SetNextWindowPos(ImVec2(16.f, ImGui::GetMainViewport()->Size.y - 16.f), ImGuiCond_Always, ImVec2(0.f, 1.f));
    ImGui::Begin("##zoom", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    char z[32]; snprintf(z, sizeof z, "%d%%", (int)roundf(g_cam.zoom * 100.f));
    if (ImGui::Button(z)) zoom_to_100(ImGui::GetMainViewport()->Size);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("100%% (Shift+0) · fit (Shift+1) · selection (Shift+2)");
    ImGui::End();
}

// ── board picker (Ctrl+O; auto-opens when the app starts with no board) ──
static bool g_pickerWant = false;
static std::string g_newBoardName;

static std::string sanitize_board_name(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_') out += c;
        else if (c == ' ') out += '-';
    }
    return out;
}

// long paths elide from the LEFT — the tail (the board dir) is the news
static std::string elide_left(const std::string& s, ImFont* f, float px, float maxw) {
    if (f->CalcTextSizeA(px, FLT_MAX, 0.f, s.c_str()).x <= maxw) return s;
    std::string tail = s;
    while (tail.size() > 8 &&
           f->CalcTextSizeA(px, FLT_MAX, 0.f, ("…" + tail).c_str()).x > maxw) {
        size_t cut = 1;
        while (cut < tail.size() && (tail[cut] & 0xC0) == 0x80) cut++;   // stay on utf-8 boundaries
        tail.erase(0, cut);
    }
    return "…" + tail;
}

static void DrawBoardPicker() {
    bool noBoard = g_projDir.empty();
    if ((g_pickerWant || noBoard) && !ImGui::IsPopupOpen("boards")) {
        g_newBoardName.clear();
        ImGui::OpenPopup("boards");   // no board open = the picker IS the app
    }
    g_pickerWant = false;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Size * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(540, 0), ImVec2(540, vp->Size.y * 0.85f));
    if (!ImGui::BeginPopupModal("boards", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoTitleBar))
        return;
    // esc closes the picker (only when there's a board to fall back to; while
    // the name field is active esc just cancels that edit — checked before
    // items so ActiveId still reflects last frame)
    if (!noBoard && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();

    ImDrawList* wdl = ImGui::GetWindowDrawList();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_th.textDim), noBoard ? "boards" : "boards · esc to close");
    ImGui::Spacing();
    std::string cur = noBoard ? std::string() : abs_path(g_projDir);
    bool anyRecent = false;
    for (int i = 0; i < (int)g_recentBoards.size(); i++) {
        const std::string& r = g_recentBoards[i];
        if (!board_exists(r)) continue;
        anyRecent = true;
        bool isCur = !cur.empty() && path_eq(r, cur);
        char rid[24]; snprintf(rid, sizeof rid, "##board%d", i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        bool clicked = ImGui::Selectable(rid, isCur, 0, ImVec2(w, 42.f));
        ImFont* f = ImGui::GetFont();
        wdl->AddText(f, 17.f, p + ImVec2(10, 3), g_th.textMain, board_name(r).c_str());
        wdl->AddText(f, 13.f, p + ImVec2(10, 24), g_th.textDim, elide_left(r, f, 13.f, w - 32.f).c_str());
        if (isCur) wdl->AddCircleFilled(p + ImVec2(w - 14.f, 21.f), 3.5f, g_th.accent);
        if (clicked) {
            if (!isCur) switch_board(r);
            ImGui::CloseCurrentPopup();
        }
    }
    if (anyRecent) ImGui::Separator();

    if (ImGui::IsWindowAppearing() && !anyRecent) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-84.f);
    bool create = ImGui::InputTextWithHint("##newboard", "new board name", &g_newBoardName,
                                           ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    create |= ImGui::Button("create", ImVec2(76, 0));
    std::string clean = sanitize_board_name(g_newBoardName);
    if (!g_boardsDir.empty()) {
        std::string where = "in " + (clean.empty() ? g_boardsDir : g_boardsDir + "\\" + clean);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_th.textDim), "%s",
                           elide_left(where, ImGui::GetFont(), ImGui::GetFontSize(),
                                      ImGui::GetContentRegionAvail().x).c_str());
    }
    if (create && !clean.empty() && !g_boardsDir.empty()) {
        CreateDirectoryA(g_boardsDir.c_str(), nullptr);
        switch_board(g_boardsDir + "/" + clean);
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::Button("open folder…")) {
        std::string dir = pick_folder_dialog();
        if (!dir.empty()) { switch_board(dir); ImGui::CloseCurrentPopup(); }
    }
    ImGui::EndPopup();
}

// ── style panel (top right): palette · text size · align · opacity ──
// With a selection it restyles it; always updates the current style that new
// shapes are born with.
static void DrawStylePanel() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Size.x - 16.f, 16.f), ImGuiCond_Always, ImVec2(1.f, 0.f));
    ImGui::Begin("##style", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

    // panel reflects the first styleable selected shape, else the defaults
    int curCol = g_curCol, curSize = g_curSize, curAlign = g_curAlign;
    float curOp = g_curOpacity;
    bool haveText = g_sel.empty();   // no selection → size/align rows always shown
    for (auto id : g_sel) {
        std::vector<uint64_t> all{ id };
        Shape* g = find_shape(id);
        if (g && g->type == SH_GROUP) collect_members(id, all);
        for (auto mid : all) {
            Shape* s = find_shape(mid);
            if (!s || s->type == SH_GROUP) continue;
            curCol = s->col; curOp = s->opacity;
            if (s->type == SH_TEXT) { curSize = s->tsize; curAlign = s->align; haveText = true; }
            goto found;
        }
    }
found:;
    for (auto id : g_sel) { Shape* s = find_shape(id); if (s && s->type == SH_TEXT) haveText = true; }

    ImDrawList* wdl = ImGui::GetWindowDrawList();
    const float cell = 24.f;
    for (int i = 0; i < 12; i++) {
        if (i % 6) ImGui::SameLine(0.f, 4.f);
        char bid[16]; snprintf(bid, sizeof bid, "##col%d", i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton(bid, ImVec2(cell, cell));
        ImVec2 c = p + ImVec2(cell * 0.5f, cell * 0.5f);
        wdl->AddCircleFilled(c, 8.f, i == 0 ? g_th.textMain : palette_color(i));
        if (i == curCol) wdl->AddCircle(c, 10.5f, g_th.accent, 0, 2.f);
        else if (ImGui::IsItemHovered()) wdl->AddCircle(c, 10.5f, g_th.textDim, 0, 1.5f);
        if (clicked) {
            g_curCol = i;
            if (apply_to_selection([&](Shape& s) { if (s.type != SH_IMAGE) s.col = i; })) push_undo();
        }
    }

    if (haveText) {
        for (int z = 0; z < 4; z++) {
            if (z) ImGui::SameLine(0.f, 4.f);
            bool active = (curSize == z);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(g_th.accent));
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
            }
            char lbl[16]; snprintf(lbl, sizeof lbl, "%s##ts%d", kTextSizeName[z], z);
            if (ImGui::Button(lbl, ImVec2(38, 24))) {
                g_curSize = z;
                if (apply_to_selection([&](Shape& s) { if (s.type == SH_TEXT) { s.tsize = z; s.scale = 1.f; } })) push_undo();
            }
            if (active) ImGui::PopStyleColor(2);
        }
        // align: three little line-stacks (left / center / right)
        for (int a = 0; a < 3; a++) {
            if (a) ImGui::SameLine(0.f, 4.f);
            char bid[16]; snprintf(bid, sizeof bid, "##al%d", a);
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::InvisibleButton(bid, ImVec2(52.f, 24.f));
            bool active = (curAlign == a);
            if (active || ImGui::IsItemHovered())
                wdl->AddRectFilled(p, p + ImVec2(52, 24), active ? with_opacity(g_th.accent, 0.9f)
                                                                 : IM_COL32(128, 128, 128, 40), 5.f);
            ImU32 lc = active ? IM_COL32(255, 255, 255, 255) : g_th.textDim;
            float ws[3] = { 30.f, 20.f, 26.f };
            for (int L = 0; L < 3; L++) {
                float y = p.y + 6.f + L * 6.f;
                float x = a == 0 ? p.x + 11.f : a == 1 ? p.x + 26.f - ws[L] * 0.5f : p.x + 41.f - ws[L];
                wdl->AddRectFilled(ImVec2(x, y), ImVec2(x + ws[L], y + 2.5f), lc, 1.f);
            }
            if (clicked) {
                g_curAlign = a;
                if (apply_to_selection([&](Shape& s) { if (s.type == SH_TEXT) s.align = a; })) push_undo();
            }
        }
    }

    float op = curOp * 100.f;
    ImGui::SetNextItemWidth(6 * cell + 5 * 4.f);
    if (ImGui::SliderFloat("##opacity", &op, 5.f, 100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
        g_curOpacity = op / 100.f;
        apply_to_selection([&](Shape& s) { s.opacity = g_curOpacity; });
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) push_undo();

    ImGui::End();
}

// right-click context menu; content adapts to what's under/selected
static ImVec2 g_ctxWorldPos;
static void DrawContextMenu() {
    if (!ImGui::BeginPopup("ctx")) return;
    bool any = !g_sel.empty();
    bool haveText = false, haveGroup = false;
    for (auto id : g_sel) { Shape* s = find_shape(id); if (!s) continue;
        if (s->type == SH_TEXT) haveText = true;
        if (s->type == SH_GROUP) haveGroup = true; }
    if (any) {
        if (ImGui::MenuItem("duplicate", "Ctrl+D")) { duplicate_selected(); push_undo(); }
        if (ImGui::MenuItem("delete", "Del")) { delete_shapes(g_sel); push_undo(); }
        ImGui::Separator();
        if (g_sel.size() >= 2 && ImGui::MenuItem("group", "Ctrl+G")) { group_selected(); push_undo(); }
        if (haveGroup && ImGui::MenuItem("ungroup", "Ctrl+Shift+G")) { ungroup_selected(); push_undo(); }
        if (ImGui::MenuItem("bring to front", "]")) { reorder_selected(true); push_undo(); }
        if (ImGui::MenuItem("send to back", "[")) { reorder_selected(false); push_undo(); }
        ImGui::Separator();
        if (ImGui::MenuItem("copy as PNG", "Ctrl+Shift+C")) request_png_export(true, nullptr);
        if (ImGui::MenuItem("copy as text")) copy_text_to_clipboard(board_outline(true));
        bool haveCrop = false;
        for (auto id : g_sel) { Shape* s = find_shape(id);
            if (s && s->type == SH_IMAGE && (s->crop.x != 0 || s->crop.y != 0 || s->crop.z != 1 || s->crop.w != 1)) haveCrop = true; }
        if (haveCrop && ImGui::MenuItem("reset crop")) {
            for (auto id : g_sel) { Shape* s = find_shape(id);
                if (!s || s->type != SH_IMAGE) continue;
                WRect F = image_full_rect(*s);
                ImVec2 c0 = s->pos + s->size * 0.5f;
                ImVec2 cw = s->rot != 0.f ? rot_about(F.center(), c0, s->rot) : F.center();
                s->size = F.size(); s->pos = cw - s->size * 0.5f;
                s->crop = ImVec4(0, 0, 1, 1);
            }
            push_undo();
        }
        bool haveRot = false;
        for (auto id : g_sel) { Shape* s = find_shape(id); if (s && s->rot != 0.f) haveRot = true; }
        if (haveRot && ImGui::MenuItem("reset rotation")) {
            std::vector<uint64_t> all = g_sel;
            for (auto id : g_sel) { Shape* s = find_shape(id); if (s && s->type == SH_GROUP) collect_members(id, all); }
            for (auto id : all) { Shape* s = find_shape(id); if (s) s->rot = 0.f; }
            push_undo();
        }
        if (haveText) {
            ImGui::Separator();
            if (ImGui::BeginMenu("font")) {
                for (int f = 0; f < FF_COUNT; f++)
                    if (ImGui::MenuItem(kFamilyName[f])) {
                        for (auto id : g_sel) { Shape* s = find_shape(id); if (s && s->type == SH_TEXT) s->family = f; }
                        push_undo();
                    }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("size")) {
                for (int z = 0; z < 4; z++)
                    if (ImGui::MenuItem(kTextSizeName[z])) {
                        for (auto id : g_sel) { Shape* s = find_shape(id); if (s && s->type == SH_TEXT) { s->tsize = z; s->scale = 1.f; } }
                        push_undo();
                    }
                ImGui::EndMenu();
            }
        }
    } else {
        if (ImGui::MenuItem("select all", "Ctrl+A")) {
            g_sel.clear();
            for (auto& s : g_doc.shapes) if (!s.parent) g_sel.push_back(s.id);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("copy board as PNG", "Ctrl+Shift+C")) request_png_export(false, nullptr);
        if (ImGui::MenuItem("copy board as text")) copy_text_to_clipboard(board_outline(false));
        ImGui::Separator();
        if (ImGui::MenuItem(g_darkMode ? "light mode" : "dark mode", "Ctrl+Shift+D")) {
            g_darkMode = !g_darkMode; ApplyTheme(); save_settings();
        }
        if (ImGui::MenuItem("zoom animation", nullptr, g_zoomAnim)) {
            g_zoomAnim = !g_zoomAnim; save_settings();
        }
        if (ImGui::BeginMenu("undo limit")) {
            static const int kLims[] = { 256, 1024, 4096, 16384 };
            for (int v : kLims) {
                char lbl[16]; snprintf(lbl, sizeof lbl, "%d", v);
                if (ImGui::MenuItem(lbl, nullptr, g_undoLimit == v)) set_undo_limit(v);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("boards…", "Ctrl+O")) g_pickerWant = true;
    }
    ImGui::EndPopup();
}

// ── video controls (selected video only) ──
// A selected video shows a MINI pill (play/stop) tucked in its bottom-right
// corner while the pointer is over it; hovering the mini pill escalates to
// the FULL pill (seek bar, time, A-B loop) centered on the video, which drops
// back after a 0.5s timeout once the pointer leaves it. Everything is drawn
// BY HAND on the foreground drawlist and hit-tested manually (no imgui
// window), so the canvas always owns the mouse: dragging anywhere on a pill —
// seek bar included — moves the video (the pills fade out smoothly with the
// drag); a STILL CLICK acts the control under it on release (seek jumps to
// the clicked position). Rotated videos rotate the pills rigidly (vertices
// turned about the video center, hit points inverse-rotated), so hitboxes sit
// exactly on the drawn pixels. Gifs just loop with no chrome.
enum OverlayCtl { OV_PLAY = 0, OV_STOP, OV_SEEK, OV_A, OV_B, OV_CLR, OV_COUNT };
static uint64_t g_overlayVid = 0;       // video whose pills are showing (kept through fade-out)
static float    g_ovAlphaFull = 0.f, g_ovAlphaMini = 0.f;   // fade envelopes
static bool     g_ovLive = false;       // full pill accepts clicks (read by CanvasFrame)
static bool     g_ovMiniLive = false;   // mini pill is shown + would accept clicks
static bool     g_ovFull = false;       // escalated to the full pill
static double   g_ovFullLoseAt = 0;     // de-escalation deadline once the full pill isn't hovered
static ImVec2   g_ovTL, g_ovSize;          // full pill rect (unrotated screen space)
static ImVec2   g_ovMiniTL, g_ovMiniSize;  // mini pill rect
static ImVec2   g_ovPivot;              // rotation pivot = video center (screen)
static float    g_ovRot = 0.f;
static WRect    g_ovCtl[OV_COUNT];      // full-pill control hit rects, pill-local
static WRect    g_ovMiniCtl[2];         // mini-pill buttons (play, stop), mini-local
static int      g_overlayDownCtl = -2;  // at press: control index, -1 = pill dead-space, -2 = press not on a pill

static ImVec2 overlay_unrot(ImVec2 s) { return g_ovRot != 0.f ? rot_about(s, g_ovPivot, -g_ovRot) : s; }
static bool ov_in(ImVec2 s, ImVec2 tl, ImVec2 size) {
    ImVec2 p = overlay_unrot(s) - tl;
    return p.x >= 0 && p.y >= 0 && p.x <= size.x && p.y <= size.y;
}
// the mini AREA keeps responding while the full pill is up: the user just saw
// play/stop there and may click before (or while) the swap animates. Where
// the full pill overlaps it, the full pill's controls win (checked first).
static bool overlay_contains(ImVec2 s) {
    return (g_ovLive && ov_in(s, g_ovTL, g_ovSize)) ||
           ((g_ovMiniLive || g_ovLive) && ov_in(s, g_ovMiniTL, g_ovMiniSize));
}
static int overlay_ctl_at(ImVec2 s) {
    if (g_ovLive && ov_in(s, g_ovTL, g_ovSize)) {
        ImVec2 l = overlay_unrot(s) - g_ovTL;
        for (int i = 0; i < OV_COUNT; i++) if (g_ovCtl[i].contains(l)) return i;
        return -1;
    }
    if ((g_ovMiniLive || g_ovLive) && ov_in(s, g_ovMiniTL, g_ovMiniSize)) {
        ImVec2 l = overlay_unrot(s) - g_ovMiniTL;
        if (g_ovMiniCtl[0].contains(l)) return OV_PLAY;
        if (g_ovMiniCtl[1].contains(l)) return OV_STOP;
        return -1;
    }
    return -1;
}

static void fmt_time(char* buf, size_t n, double t) {
    int s = (int)(t + 0.5);
    snprintf(buf, n, "%d:%02d", s / 60, s % 60);
}

// play / pause / stop glyph centered at c
static void ov_icon(ImDrawList* dl, ImVec2 c, float r, int icon, ImU32 col) {
    if (icon == 0)      dl->AddTriangleFilled(c + ImVec2(-r * 0.7f, -r), c + ImVec2(-r * 0.7f, r), c + ImVec2(r, 0), col);
    else if (icon == 1) { dl->AddRectFilled(c + ImVec2(-r * 0.8f, -r), c + ImVec2(-r * 0.15f, r), col, 1.f);
                          dl->AddRectFilled(c + ImVec2(r * 0.15f, -r), c + ImVec2(r * 0.8f, r), col, 1.f); }
    else                dl->AddRectFilled(c + ImVec2(-r * 0.8f, -r * 0.8f), c + ImVec2(r * 0.8f, r * 0.8f), col, 1.f);
}

static void DrawVideoOverlay() {
#ifdef TEI_LIBAV
    ImGuiIO& io = ImGui::GetIO();
    bool editing = g_editText || g_editLabelArrow;
    bool dragging = g_drag != DM_NONE && g_drag != DM_PENDING;   // pending = a press, maybe on a pill
    Shape* sel = nullptr;   // the single selected video, if that's the selection
    if (!editing && g_sel.size() == 1) {
        Shape* c = find_shape(g_sel[0]);
        if (c && c->type == SH_IMAGE && media_kind(c->asset) == MK_VIDEO) sel = c;
    }
    if (sel) g_overlayVid = sel->id;
    Shape* v = g_overlayVid ? find_shape(g_overlayVid) : nullptr;
    if (v && (v->type != SH_IMAGE || media_kind(v->asset) != MK_VIDEO)) v = nullptr;   // undo/replace churn

    // ── mini/full state machine (pill rects are last frame's layout) ──
    bool pressHold = g_overlayDownCtl != -2;   // a press that started on a pill holds things open
    bool hoverVid = false;
    if (sel) {
        WRect b = shape_local_rect(*sel);
        ImVec2 p = sel->rot != 0.f ? rot_about(S2W(io.MousePos), b.center(), -sel->rot) : S2W(io.MousePos);
        hoverVid = b.contains(p);
    }
    bool hoverMini = g_overlayVid && ov_in(io.MousePos, g_ovMiniTL, g_ovMiniSize);
    bool hoverFull = g_overlayVid && ov_in(io.MousePos, g_ovTL, g_ovSize);
    double now = ImGui::GetTime();
    if (!sel) { g_ovFull = false; g_ovFullLoseAt = 0; }
    else if (!g_ovFull) {
        // hovering the (visible) mini pill escalates to the full controls
        if (hoverMini && g_ovAlphaMini > 0.5f && !pressHold) { g_ovFull = true; g_ovFullLoseAt = 0; }
    } else {
        if (hoverFull || hoverMini || pressHold) g_ovFullLoseAt = 0;
        else if (!g_ovFullLoseAt) g_ovFullLoseAt = now + 0.5;   // timeout starts when the pointer leaves
        else if (now >= g_ovFullLoseAt) { g_ovFull = false; g_ovFullLoseAt = 0; }
    }
    bool wantFull = sel && !dragging && g_ovFull;
    bool wantMini = sel && !dragging && !g_ovFull && (hoverVid || hoverMini || pressHold);
    auto fade = [&](float a, bool want) {
        return want ? fminf(1.f, a + io.DeltaTime / 0.12f) : fmaxf(0.f, a - io.DeltaTime / 0.15f);
    };
    g_ovAlphaFull = fade(g_ovAlphaFull, wantFull);
    g_ovAlphaMini = fade(g_ovAlphaMini, wantMini);
    g_ovLive = wantFull && v != nullptr;
    g_ovMiniLive = wantMini && v != nullptr;
    if (!v || (g_ovAlphaFull <= 0.f && g_ovAlphaMini <= 0.f)) {
        if (!v) g_overlayVid = 0;
        g_ovLive = g_ovMiniLive = false;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_overlayDownCtl = -2;
        return;
    }
    VideoDecoder* d = get_decoder(v->asset);
    if (!d) { g_overlayVid = 0; g_ovLive = g_ovMiniLive = false; return; }
    PlayState& ps = g_play[v->id];
    double dur = d->duration();

    // ── layout (unrotated screen space; control rects pill-local) ──
    WRect lb = shape_local_rect(*v);
    ImVec2 cs = W2S(lb.center());
    ImFont* f = ImGui::GetFont(); float fs = ImGui::GetFontSize();
    char t0[16], t1[16]; fmt_time(t0, 16, ps.t); fmt_time(t1, 16, dur);
    char times[40]; snprintf(times, sizeof times, "%s / %s", t0, t1);
    const float pad = 10.f, bh = 28.f, seekH = 16.f, gap = 6.f, small = 18.f;
    float timeW = f->CalcTextSizeA(fs, FLT_MAX, 0.f, times).x;
    float row1W = bh + gap + bh + 10.f + timeW;
    float sliderW = fminf(fmaxf(lb.size().x * g_cam.zoom * 0.75f, 200.f), 340.f);
    bool haveLoop = v->loopA >= 0 || v->loopB >= 0;
    float W = fmaxf(sliderW, row1W) + pad * 2;
    float y1 = pad + bh + gap;        // seek row
    float y2 = y1 + seekH + gap;      // loop row
    g_ovSize = ImVec2(W, y2 + small + pad);
    g_ovTL = cs - g_ovSize * 0.5f;
    g_ovPivot = cs;
    g_ovRot = v->rot;
    g_ovCtl[OV_PLAY] = { ImVec2(pad, pad), ImVec2(pad + bh, pad + bh) };
    g_ovCtl[OV_STOP] = { ImVec2(pad + bh + gap, pad), ImVec2(pad + bh + gap + bh, pad + bh) };
    g_ovCtl[OV_SEEK] = { ImVec2(pad, y1), ImVec2(W - pad, y1 + seekH) };
    g_ovCtl[OV_A]    = { ImVec2(pad, y2), ImVec2(pad + 22.f, y2 + small) };
    g_ovCtl[OV_B]    = { ImVec2(pad + 26.f, y2), ImVec2(pad + 48.f, y2 + small) };
    g_ovCtl[OV_CLR]  = haveLoop ? WRect{ ImVec2(pad + 52.f, y2), ImVec2(pad + 74.f, y2 + small) }
                                : WRect{ ImVec2(-9999.f, -9999.f), ImVec2(-9999.f, -9999.f) };
    // mini pill: bottom-right corner of the video, inset 8px
    const float mb = 22.f, mpad = 4.f;
    g_ovMiniSize = ImVec2(mpad + mb + 2.f + mb + mpad, mb + mpad * 2);
    g_ovMiniTL = W2S(lb.mx) - g_ovMiniSize - ImVec2(8.f, 8.f);
    g_ovMiniCtl[0] = { ImVec2(mpad, mpad), ImVec2(mpad + mb, mpad + mb) };
    g_ovMiniCtl[1] = { ImVec2(mpad + mb + 2.f, mpad), ImVec2(mpad + mb + 2.f + mb, mpad + mb) };

    // ── still-click actions. Press bookkeeping is CanvasFrame's: it targets
    // the video for drags and hands us the pressed control index. ──
    if (g_overlayDownCtl != -2 && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        int ctl = g_overlayDownCtl; g_overlayDownCtl = -2;
        if (vlen(io.MousePos - g_dragStartS) < 4.f) switch (ctl) {
        case OV_PLAY: ps.playing = !ps.playing; break;
        case OV_STOP: ps.playing = false; ps.t = 0; break;
        case OV_SEEK: if (dur > 0) {
            float u = (overlay_unrot(io.MousePos).x - g_ovTL.x - g_ovCtl[OV_SEEK].mn.x) / fmaxf(g_ovCtl[OV_SEEK].size().x, 1.f);
            ps.t = fminf(fmaxf(u, 0.f), 1.f) * dur;
        } break;
        case OV_A: v->loopA = (float)ps.t; if (v->loopB >= 0 && v->loopB <= v->loopA) v->loopB = -1; push_undo(); break;
        case OV_B: v->loopB = (float)ps.t; if (v->loopA >= 0 && v->loopA >= v->loopB) v->loopA = -1; push_undo(); break;
        case OV_CLR: v->loopA = v->loopB = -1; push_undo(); break;
        }
    }

    // ── draw (unrotated, then each pill's vertex range is rotated + faded) ──
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 um = overlay_unrot(io.MousePos);
    bool mdown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    int vtxM0 = dl->VtxBuffer.Size;
    if (g_ovAlphaMini > 0.f) {
        ImVec2 TL = g_ovMiniTL;
        dl->AddRectFilled(TL, TL + g_ovMiniSize, g_th.panelBg, 8.f);
        dl->AddRect(TL, TL + g_ovMiniSize, g_th.panelBorder, 8.f);
        for (int i = 0; i < 2; i++) {
            int id = i == 0 ? OV_PLAY : OV_STOP;
            ImVec2 p = TL + g_ovMiniCtl[i].mn, sz = g_ovMiniCtl[i].size();
            bool hv = g_ovMiniLive && (mdown ? g_overlayDownCtl == id : g_ovMiniCtl[i].contains(um - TL));
            if (hv) dl->AddRectFilled(p, p + sz, IM_COL32(255, 255, 255, mdown ? 36 : 20), 5.f);
            ov_icon(dl, p + sz * 0.5f, 5.f, i == 0 ? (ps.playing ? 1 : 0) : 2, g_th.textMain);
        }
    }
    int vtxF0 = dl->VtxBuffer.Size;
    if (g_ovAlphaFull > 0.f) {
        ImVec2 TL = g_ovTL;
        ImVec2 lm = um - TL;
        auto hot = [&](int i) {   // hover highlight; while pressing only the pressed control lights
            if (!g_ovLive) return false;
            return mdown ? g_overlayDownCtl == i : g_ovCtl[i].contains(lm);
        };
        dl->AddRectFilled(TL, TL + g_ovSize, g_th.panelBg, 10.f);
        dl->AddRect(TL, TL + g_ovSize, g_th.panelBorder, 10.f);
        auto icon_button = [&](int i, int icon) {
            ImVec2 p = TL + g_ovCtl[i].mn, sz = g_ovCtl[i].size();
            if (hot(i)) dl->AddRectFilled(p, p + sz, IM_COL32(255, 255, 255, mdown ? 36 : 20), 6.f);
            ov_icon(dl, p + sz * 0.5f, 6.f, icon, g_th.textMain);
        };
        icon_button(OV_PLAY, ps.playing ? 1 : 0);
        icon_button(OV_STOP, 2);
        dl->AddText(f, fs, TL + ImVec2(g_ovCtl[OV_STOP].mx.x + 10.f, pad + (bh - fs) * 0.5f), g_th.textDim, times);
        {   // seek bar: track + progress + A/B marks + knob
            WRect sk = g_ovCtl[OV_SEEK];
            float ty = (sk.mn.y + sk.mx.y) * 0.5f;
            ImVec2 tmn = TL + ImVec2(sk.mn.x, ty - 3.f), tmx = TL + ImVec2(sk.mx.x, ty + 3.f);
            dl->AddRectFilled(tmn, tmx, IM_COL32(128, 128, 128, 70), 3.f);
            float u = dur > 0 ? (float)(ps.t / dur) : 0.f;
            u = u < 0 ? 0 : (u > 1 ? 1 : u);
            float kx = tmn.x + (tmx.x - tmn.x) * u;
            dl->AddRectFilled(tmn, ImVec2(kx, tmx.y), g_th.accent, 3.f);
            auto mark = [&](float sec, ImU32 col) {
                if (sec < 0 || dur <= 0) return;
                float x = tmn.x + (tmx.x - tmn.x) * (float)(sec / dur);
                dl->AddRectFilled(ImVec2(x - 1.5f, tmn.y - 3.f), ImVec2(x + 1.5f, tmx.y + 3.f), col, 1.f);
            };
            mark(v->loopA, IM_COL32(120, 220, 120, 230));
            mark(v->loopB, IM_COL32(235, 120, 120, 230));
            dl->AddCircleFilled(ImVec2(kx, TL.y + ty), hot(OV_SEEK) ? 7.f : 5.5f, g_th.textMain);
        }
        auto text_button = [&](int i, const char* lbl) {
            ImVec2 p = TL + g_ovCtl[i].mn, q = TL + g_ovCtl[i].mx;
            dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, hot(i) ? 36 : 14), 4.f);
            ImVec2 ts = f->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0.f, lbl);
            dl->AddText(f, fs * 0.85f, (p + q) * 0.5f - ts * 0.5f, g_th.textMain, lbl);
        };
        text_button(OV_A, "A");
        text_button(OV_B, "B");
        if (haveLoop) {
            text_button(OV_CLR, "x");
            dl->AddText(f, fs * 0.85f, TL + ImVec2(pad + 80.f, y2 + 2.f), g_th.textDim, "loop");
        }
    }
    int vtxEnd = dl->VtxBuffer.Size;
    if (v->rot != 0.f) {
        float sn = sinf(v->rot), cn = cosf(v->rot);
        for (int i = vtxM0; i < vtxEnd; i++) {
            ImDrawVert& vx = dl->VtxBuffer[i];
            float dx = vx.pos.x - cs.x, dy = vx.pos.y - cs.y;
            vx.pos.x = cs.x + dx * cn - dy * sn;
            vx.pos.y = cs.y + dx * sn + dy * cn;
        }
    }
    auto fade_range = [&](int a, int b, float alpha) {
        if (alpha >= 1.f) return;
        for (int i = a; i < b; i++) {
            ImDrawVert& vx = dl->VtxBuffer[i];
            vx.col = (vx.col & 0x00FFFFFF) | ((ImU32)((vx.col >> 24) * alpha) << 24);
        }
    };
    fade_range(vtxM0, vtxF0, g_ovAlphaMini);
    fade_range(vtxF0, vtxEnd, g_ovAlphaFull);
#endif
}

// ── auto lists ──
// "- " / "* " at line start becomes a bullet; Enter continues the list
// ("• " again, or the next number for "12. " items); Enter on an EMPTY item
// strips the marker and ends the list. Markdown-editor muscle memory.
static const char* kBullet = "\xe2\x80\xa2 ";   // "• " (4 bytes)

static void text_edit_apply(ImGuiInputTextCallbackData* d, bool deleted) {
    if (d->BufTextLen <= 0) return;
    int cur = d->CursorPos;
    if (cur < 1 || cur > d->BufTextLen) return;
    auto line_start = [&](int from) { int i = from; while (i > 0 && d->Buf[i - 1] != '\n') i--; return i; };

    if (deleted) {
        // backspaced down to a bare marker ("• " lost its space): remove the
        // marker AND the newline before it — back to the previous line
        int ls = line_start(cur);
        int i = ls; while (i < cur && d->Buf[i] == ' ') i++;
        bool bareBullet = (cur - i == 3 && !memcmp(d->Buf + i, "\xe2\x80\xa2", 3));
        int j = i; while (j < cur - 1 && isdigit((unsigned char)d->Buf[j])) j++;
        bool bareNumber = (j > i && j == cur - 1 && d->Buf[j] == '.');
        if (bareBullet || bareNumber) {
            int from = ls > 0 ? ls - 1 : ls;    // include the preceding '\n' if any
            d->DeleteChars(from, cur - from);
        }
        return;
    }

    if (d->Buf[cur - 1] == ' ' && cur >= 2) {
        // just typed the space of a "- " / "* " marker at line start?
        int ls = line_start(cur - 1);
        int i = ls; while (i < cur - 2 && d->Buf[i] == ' ') i++;
        if (i == cur - 2 && (d->Buf[i] == '-' || d->Buf[i] == '*')) {
            d->DeleteChars(i, 2);
            d->InsertChars(i, kBullet);
        }
        return;
    }
    if (d->Buf[cur - 1] == '\n') {
        // Enter: continue (or end) the previous line's list
        int pe = cur - 1;
        int ps = line_start(pe);
        int i = ps; while (i < pe && d->Buf[i] == ' ') i++;
        std::string indent(d->Buf + ps, d->Buf + i);
        if (pe - i >= 4 && !memcmp(d->Buf + i, kBullet, 4)) {
            // empty item → drop marker AND the fresh newline: stay on the
            // same (now plain) line, list mode off
            if (pe - i == 4) d->DeleteChars(i, cur - i);
            else { std::string ins = indent + kBullet; d->InsertChars(d->CursorPos, ins.c_str()); }
            return;
        }
        int j = i; while (j < pe && j - i < 9 && isdigit((unsigned char)d->Buf[j])) j++;
        if (j > i && j < pe && d->Buf[j] == '.' && (j + 1 == pe || d->Buf[j + 1] == ' ')) {
            if (j + 2 >= pe) d->DeleteChars(i, cur - i);       // "12." / "12. " alone → stay, end list
            else {
                long n = strtol(std::string(d->Buf + i, d->Buf + j).c_str(), nullptr, 10);
                char num[32]; snprintf(num, sizeof num, "%ld. ", n + 1);
                std::string ins = indent + num;
                d->InsertChars(d->CursorPos, ins.c_str());
            }
        }
    }
}

static int TextEditCallback(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag != ImGuiInputTextFlags_CallbackEdit) return 0;
    bool deleted = g_editLastLen >= 0 && d->BufTextLen < g_editLastLen;
    text_edit_apply(d, deleted);
    g_editLastLen = d->BufTextLen;   // after our own edits, so they don't read as user deletions
    return 0;
}

// ── WYSIWYG editing state ──
// The live editor must look exactly like the committed shape: bold strike,
// per-line alignment (bullets pinned left), rotation. imgui's InputText is
// left-aligned/single-strike, so after it draws we transform its finished
// vertices per line, and before each frame we remap the pointer inversely so
// caret clicks land where the eye says (same pattern as rotated editing).
static std::vector<float> g_editLineDx;        // per-line x offsets, committed layout
static bool  g_editShiftActive = false;
static float g_editShiftTopY = 0.f, g_editShiftPx = 1.f;
// WYSIWYG-transform editing is OFF (both parts): any pre-frame pointer remap
// (per-line align shift AND the rotation remap) makes imgui's InputText see
// jittering coordinates while the button is held → phantom drag-selections
// (user reports, confirmed rotation-only after the align revert). Rotated
// text edits axis-aligned as a placeholder. The real fix is a custom
// stb_textedit-based editor that owns its hit-testing — scoped for later,
// after the M3 LLM export. The draw-side transforms below are proven and
// stay for that future editor.
static const bool kWysiwygAlignEdit = false;
static const bool kWysiwygRotEdit = false;

// map a canvas click to a byte offset in the shape's text (committed layout:
// alignment + list-pinning + rotation aware) so the editor opens with the
// caret exactly under the mouse
static int caret_index_from_click(const Shape& s, ImVec2 clickW) {
    if (s.text.empty()) return 0;
    WRect lr = shape_local_rect(s);
    ImVec2 p = s.rot != 0.f ? rot_about(clickW, lr.center(), -s.rot) : clickW;
    float lh = text_px(s);
    ImGui::PushFont(g_fonts[s.family], lh);
    float totalW = ImGui::CalcTextSize(s.text.c_str()).x;
    const char* base = s.text.c_str();
    const char* end = base + s.text.size();
    int line = (int)floorf((p.y - lr.mn.y) / lh);
    const char* b = base;
    for (int li = 0; li < line; li++) {
        const char* nl = (const char*)memchr(b, '\n', end - b);
        if (!nl) break;
        b = nl + 1;
    }
    const char* e = (const char*)memchr(b, '\n', end - b);
    if (!e) e = end;
    float dx = 0.f;
    if (s.align && !is_list_line(b, e)) {
        float w = ImGui::CalcTextSize(b, e).x;
        dx = s.align == 1 ? (totalW - w) * 0.5f : (totalW - w);
    }
    float x = (p.x - lr.mn.x) - dx;
    const char* c = b;
    while (c < e) {   // nearest inter-character boundary wins
        int cl = (*c & 0x80) == 0 ? 1 : (*c & 0xE0) == 0xC0 ? 2 : (*c & 0xF0) == 0xE0 ? 3 : 4;
        const char* n = (c + cl > e) ? e : c + cl;
        float w0 = ImGui::CalcTextSize(b, c).x;
        float w1 = ImGui::CalcTextSize(b, n).x;
        if (x < (w0 + w1) * 0.5f) break;
        c = n;
    }
    ImGui::PopFont();
    return (int)(c - base);
}

static void edit_line_offsets(const std::string& text, ImFont* f, float px, int align, std::vector<float>& out) {
    out.clear();
    ImGui::PushFont(f, px);
    float totalW = ImGui::CalcTextSize(text.empty() ? " " : text.c_str()).x;
    const char* b = text.c_str();
    const char* end = b + text.size();
    for (;;) {
        const char* e = (const char*)memchr(b, '\n', end - b);
        if (!e) e = end;
        float dx = 0.f;
        if (align && !is_list_line(b, e)) {
            float w = e > b ? ImGui::CalcTextSize(b, e).x : 0.f;   // empty line: caret sits where text would
            dx = align == 1 ? (totalW - w) * 0.5f : (totalW - w);
        }
        out.push_back(dx);
        if (e >= end) break;
        b = e + 1;
    }
    ImGui::PopFont();
}

// in-place text editor overlay (canvas text + arrow labels share it)
static void DrawTextEditOverlay() {
    uint64_t id = g_editText ? g_editText : g_editLabelArrow;
    if (!id) { g_editShiftActive = false; return; }
    Shape* s = find_shape(id);
    if (!s) { g_editText = g_editLabelArrow = 0; g_editShiftActive = false; return; }
    bool isLabel = (g_editLabelArrow != 0);
    std::string* str = isLabel ? &s->label : &s->text;

    float px = (isLabel ? kTextSizes[0] : text_px(*s)) * g_cam.zoom;
    px = fminf(px, kMaxGlyphPx);
    ImFont* font = g_fonts[s->family];

    ImVec2 anchor;
    if (isLabel) {
        std::vector<ImVec2> pl; arrow_polyline(*s, pl);
        anchor = W2S(pl.empty() ? arrow_end_pos(s->a) : pl[pl.size() / 2]);
    } else anchor = W2S(s->pos);

    bool rotated = !isLabel && s->rot != 0.f && kWysiwygRotEdit;
    ImVec2 rotC = W2S(shape_local_rect(*s).center());   // rotation pivot (screen)
    float  rotA = s->rot;

    ImGui::PushFont(font, px);
    ImVec2 ext = ImGui::CalcTextSize(str->empty() ? " " : str->c_str());
    if (!str->empty() && str->back() == '\n') ext.y += px;   // CalcTextSize drops the trailing empty line
    float minW = ImGui::CalcTextSize("MM").x;
    ImVec2 box(fmaxf(ext.x, minW) + px * 0.75f, ext.y + px * 0.5f);
    ImVec2 winPos = isLabel ? anchor - box * 0.5f : anchor;

    ImGui::SetNextWindowPos(winPos - ImVec2(8, 8));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(shape_ink(*s)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, isLabel ? ImGui::ColorConvertU32ToFloat4(g_th.canvasBg) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 1.f);   // no scrollbar flash on growth frames
    ImGui::Begin("##textedit", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav);
    if (g_editTextTakeFocus) { ImGui::SetKeyboardFocusHere(); g_editTextTakeFocus = false; }
    // Text reflow must not swing bound arrows around: normalized anchors
    // stretch with the rect, so remember each bound end's world point now and
    // re-derive its anchor after the edit — an arrow bound near the left edge
    // stays put while typing extends the right edge.
    std::vector<std::pair<ArrowEnd*, ImVec2>> pinned;
    if (!isLabel)
        for (auto& a : g_doc.shapes) if (a.type == SH_ARROW) {
            if (a.a.bind == s->id) pinned.push_back({ &a.a, arrow_end_pos(a.a) });
            if (a.b.bind == s->id) pinned.push_back({ &a.b, arrow_end_pos(a.b) });
        }
    std::string before = *str;
    ImGui::InputTextMultiline("##t", str, box,
                              ImGuiInputTextFlags_NoHorizontalScroll | ImGuiInputTextFlags_CallbackEdit,
                              TextEditCallback);
    if (!pinned.empty() && *str != before) {
        WRect nb = shape_local_rect(*s);
        ImVec2 nsz = nb.size();
        for (auto& [e, w] : pinned) {
            ImVec2 p = s->rot != 0.f ? rot_about(w, nb.center(), -s->rot) : w;
            e->anchor.x = nsz.x > 0 ? fminf(fmaxf((p.x - nb.mn.x) / nsz.x, 0.f), 1.f) : 0.5f;
            e->anchor.y = nsz.y > 0 ? fminf(fmaxf((p.y - nb.mn.y) / nsz.y, 0.f), 1.f) : 0.5f;
        }
    }
    // ── WYSIWYG pass: make the live editor match the committed rendering ──
    // 1) faux-bold second strike  2) per-line alignment offsets applied to
    // the editor's finished vertices (glyphs, caret, selection quads shift
    // with their line)  3) everything rotated into place for rotated shapes.
    // The pointer was remapped inversely before NewFrame (main loop).
    // force the caret to the opening click (also guarantees no selection is
    // carried over from a previous edit of any shape — same widget id)
    if (g_editCaretIdx >= 0) {
        if (ImGuiInputTextState* st = ImGui::GetInputTextState(ImGui::GetItemID())) {
            // STB_TexteditState's public head (cursor/select_start/select_end
            // are its first members); the full type isn't visible outside
            // imgui_widgets.cpp, so poke it through a mirror of that head
            struct StbHead { int cursor, select_start, select_end; };
            StbHead* stb = reinterpret_cast<StbHead*>(st->Stb);
            int ci = g_editCaretIdx > (int)str->size() ? (int)str->size() : g_editCaretIdx;
            stb->cursor = stb->select_start = stb->select_end = ci;
            st->CursorFollow = true;
            st->CursorAnimReset();
            g_editCaretIdx = -1;   // state exists ⇒ item is live; consumed
        }
    }
    ImVec2 itemMin = ImGui::GetItemRectMin();
    ImU32 ink = shape_ink(*s);
    if (!isLabel && kWysiwygAlignEdit) edit_line_offsets(*str, font, px, s->align, g_editLineDx);
    else g_editLineDx.clear();
    int nLines = (int)g_editLineDx.size();
    bool anyShift = false;
    for (float d : g_editLineDx) if (d != 0.f) { anyShift = true; break; }

    ImDrawList* pdl = ImGui::GetWindowDrawList();
    int strikeVtx0 = pdl->VtxBuffer.Size;
    {   // bold strike (parent drawlist; same ink so draw order is moot)
        const char* b = str->c_str();
        const char* end = b + str->size();
        float bo = px * 0.03f;
        int li = 0;
        while (b < end) {
            const char* e = (const char*)memchr(b, '\n', end - b);
            if (!e) e = end;
            if (e > b) {
                float dx = li < nLines ? g_editLineDx[li] : 0.f;
                pdl->AddText(font, px, ImVec2(itemMin.x + dx + bo, itemMin.y + li * px), ink, b, e);
            }
            li++;
            if (e >= end) break;
            b = e + 1;
        }
    }

    ImGuiWindow* parentWin = ImGui::GetCurrentWindow();
    ImGuiID cid = ImGui::GetID("##t");
    char nm[256];
    snprintf(nm, sizeof nm, "%s/##t", parentWin->Name);
    ImGuiWindow* child = ImGui::FindWindowByName(nm);
    if (!child) { snprintf(nm, sizeof nm, "%s/##t_%08X", parentWin->Name, cid); child = ImGui::FindWindowByName(nm); }
    if (child && child->DrawList) {
        ImDrawList* cdl = child->DrawList;
        if (anyShift) {
            // shift per QUAD (4 consecutive verts) so no glyph shears when a
            // descender pokes past the line box
            for (int i = 0; i + 3 < cdl->VtxBuffer.Size; i += 4) {
                float cy = (cdl->VtxBuffer[i].pos.y + cdl->VtxBuffer[i + 2].pos.y) * 0.5f;
                int li = (int)floorf((cy - itemMin.y) / px);
                li = li < 0 ? 0 : (li >= nLines ? nLines - 1 : li);
                for (int k = 0; k < 4; k++) cdl->VtxBuffer[i + k].pos.x += g_editLineDx[li];
            }
        }
        if (rotated) {
            float sn = sinf(rotA), cs = cosf(rotA);
            for (int i = 0; i < cdl->VtxBuffer.Size; i++) {
                ImDrawVert& v = cdl->VtxBuffer[i];
                float dx = v.pos.x - rotC.x, dy = v.pos.y - rotC.y;
                v.pos.x = rotC.x + dx * cs - dy * sn;
                v.pos.y = rotC.y + dx * sn + dy * cs;
            }
            for (int i = strikeVtx0; i < pdl->VtxBuffer.Size; i++) {
                ImDrawVert& v = pdl->VtxBuffer[i];
                float dx = v.pos.x - rotC.x, dy = v.pos.y - rotC.y;
                v.pos.x = rotC.x + dx * cs - dy * sn;
                v.pos.y = rotC.y + dx * sn + dy * cs;
            }
            ImVec2 ds = ImGui::GetIO().DisplaySize;
            for (int i = 0; i < cdl->CmdBuffer.Size; i++)
                cdl->CmdBuffer[i].ClipRect = ImVec4(0, 0, ds.x, ds.y);
            for (int i = 0; i < pdl->CmdBuffer.Size; i++)
                pdl->CmdBuffer[i].ClipRect = ImVec4(0, 0, ds.x, ds.y);
        }
    }
    // published for the pre-NewFrame pointer remap
    g_editShiftActive = anyShift && !isLabel;
    g_editShiftTopY = itemMin.y;
    g_editShiftPx = px;

    bool active = ImGui::IsItemActive();
    if (active) g_editEverActive = true;
    // focus takes a frame to land: never evaluate commit conditions before the
    // item has actually been active, or the creating click kills the editor
    bool deactivated = g_editEverActive && ImGui::IsItemDeactivated();
    if (!g_editEverActive) { g_editPrev = *str; ImGui::End(); ImGui::PopStyleVar(3); ImGui::PopStyleColor(4); ImGui::PopFont(); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        // imgui's escape reverts the buffer to its value at activation; we want
        // escape = commit-what-you-see, so restore last frame's text
        *str = g_editPrev;
        if (isLabel) { g_editLabelArrow = 0; push_undo(); }
        else end_text_edit(true);
    } else if (deactivated || (!active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered())) {
        if (isLabel) { g_editLabelArrow = 0; push_undo(); }
        else end_text_edit(true);
    } else {
        g_editPrev = *str;
        (void)before;
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    ImGui::PopFont();
}

// ───────────────────────────── the canvas frame ────────────────────────────
static double g_lastClickTime = -1; static ImVec2 g_lastClickPos; static uint64_t g_lastClickLeaf = 0;

static void CanvasFrame() {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    tick_cam_anim();
    dl->AddRectFilled(vp->Pos, vp->Pos + vp->Size, g_th.canvasBg);
    DrawGrid(dl, vp->Size);

    // files dropped from explorer land here (point captured at WM_DROPFILES)
    if (!g_dropFiles.empty()) {
        import_files_at(g_dropFiles, S2W(g_dropPoint));
        g_dropFiles.clear();
    }

    // ── draw shapes (doc order = z) ──
    draw_doc_shapes(dl, g_editText);   // editor overlay draws the edited text

    bool uiHot = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) ||
                 ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup) ||
                 io.WantCaptureMouse;
    bool editing = (g_editText || g_editLabelArrow);

    // selection UI + gizmos (draw even when uiHot, interaction gated below)
    bool showHandles = (g_tool == TOOL_SELECT) && !editing;
    int hoverHandle = -1;
    ArrowGizmo agz;
    if (showHandles) {
        bool selIsSingleArrow = g_sel.size() == 1 && find_shape(g_sel[0]) && find_shape(g_sel[0])->type == SH_ARROW;
        if (selIsSingleArrow) agz = arrow_gizmo(dl);
        else hoverHandle = draw_selection_ui(dl, true);
    }

    // ── keyboard ── (all off while the board picker modal is up)
    if (!io.WantTextInput && !ImGui::IsPopupOpen("boards")) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) g_pickerWant = true;
        if (ImGui::IsKeyPressed(ImGuiKey_V)) g_tool = TOOL_SELECT;
        if (ImGui::IsKeyPressed(ImGuiKey_H)) g_tool = TOOL_HAND;
        if (ImGui::IsKeyPressed(ImGuiKey_T)) g_tool = TOOL_TEXT;
        if (ImGui::IsKeyPressed(ImGuiKey_A) && !io.KeyCtrl) g_tool = TOOL_ARROW;
        g_spacePan = ImGui::IsKeyDown(ImGuiKey_Space);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            if (!g_sel.empty()) { delete_shapes(g_sel); push_undo(); }
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) apply_undo(io.KeyShift ? +1 : -1);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) apply_undo(+1);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
            g_sel.clear(); for (auto& s : g_doc.shapes) if (!s.parent) g_sel.push_back(s.id);
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) { duplicate_selected(); push_undo(); }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
            if (io.KeyShift) request_png_export(!g_sel.empty(), nullptr);   // selection, else whole board
            else copy_selection_to_clipboard(false);
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X)) copy_selection_to_clipboard(true);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
            ImVec2 at = uiHot ? S2W(vp->Size * 0.5f) : S2W(io.MousePos);
            paste_clipboard(at);
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G)) {
            if (io.KeyShift) ungroup_selected(); else group_selected();
            push_undo();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) { reorder_selected(true); push_undo(); }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))  { reorder_selected(false); push_undo(); }
        if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_1)) {           // zoom to fit everything
            WRect r; bool first = true;
            for (auto& s : g_doc.shapes) {
                if (s.type == SH_GROUP) continue;
                WRect b = shape_bounds(s);
                if (first) { r = b; first = false; } else r.include(b);
            }
            if (!first) zoom_to_rect(r, vp->Size);
        }
        if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_2) && !g_sel.empty())   // zoom to selection
            zoom_to_rect(selection_bounds(), vp->Size);
        if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_0)) zoom_to_100(vp->Size);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (g_drill) g_drill = 0;
            else clear_selection();
        }
        // arrow keys nudge the selection (1 canvas px, shift = 10); repeats
        // while held, and the burst coalesces into a single undo entry
        if (!g_sel.empty() && g_drag == DM_NONE && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)) {
            float step = io.KeyShift ? 10.f : 1.f;
            ImVec2 nd(0, 0);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  nd.x -= step;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) nd.x += step;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    nd.y -= step;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  nd.y += step;
            if (nd.x != 0 || nd.y != 0) { move_selected(nd); g_nudgeUndoAt = ImGui::GetTime() + 0.6; }
        }
    } else g_spacePan = false;
    if (g_nudgeUndoAt > 0 && ImGui::GetTime() >= g_nudgeUndoAt) { push_undo(); g_nudgeUndoAt = 0; }

    if (g_editRemap) return;   // pointer lives in a rotated editor's space; canvas input suspended
    if (uiHot && g_drag == DM_NONE) return;

    ImVec2 mw = S2W(io.MousePos);

    // ── wheel = zoom at cursor (THE departure from tldraw: scroll never pans) ──
    if (io.MouseWheel != 0.f && !uiHot)
        ZoomAt(io.MousePos, powf(1.16f, io.MouseWheel));

    // ── middle drag / hand / space: pan ──
    bool handActive = (g_tool == TOOL_HAND) || g_spacePan;
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.f) ||
        (handActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f))) {
        g_camAnim.active = false;
        g_cam.pan = g_cam.pan + io.MouseDelta;
        return;
    }
    if (handActive) return;

    // ── right button: drag = pan, click = context menu ──
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !uiHot) g_rDrag = false;
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.f)) { g_camAnim.active = false; g_cam.pan = g_cam.pan + io.MouseDelta; g_rDrag = true; }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !g_rDrag && !uiHot) {
        uint64_t leaf = hit_test(mw);
        uint64_t target = resolve_target(leaf);
        if (target && !is_selected(target)) { g_sel.clear(); g_sel.push_back(target); }
        if (!target) clear_selection();
        g_ctxWorldPos = mw;
        ImGui::OpenPopup("ctx");
    }

    // ── left button state machine ──
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !uiHot) {
        g_dragStartS = io.MousePos; g_dragStartW = mw;
        g_overlayDownCtl = -2;   // fresh press; the pill branch below may claim it

        if (g_tool == TOOL_TEXT) {
            uint64_t id = create_text_at(mw);
            g_sel.clear(); g_sel.push_back(id);
            begin_text_edit(id, 0);
            g_tool = TOOL_SELECT;
            g_drag = DM_NONE;
        } else if (g_tool == TOOL_ARROW) {
            Shape s; s.id = new_id(); s.type = SH_ARROW;
            s.col = g_curCol; s.opacity = g_curOpacity;
            try_bind(s.a, mw, s.id); s.b.p = mw;
            g_doc.shapes.push_back(s);
            g_newArrowId = s.id;
            g_drag = DM_NEW_ARROW;
        } else {   // select tool
            if (agz.active) {
                float r = 9.f;
                if (vlen(io.MousePos - agz.pa) < r) { g_drag = DM_ARROW_A; goto down_done; }
                if (vlen(io.MousePos - agz.pb) < r) { g_drag = DM_ARROW_B; goto down_done; }
                if (vlen(io.MousePos - agz.mid) < r) { g_drag = DM_BEND; goto down_done; }
            }
            if (hoverHandle >= 0 && hoverHandle < 4 && io.KeyCtrl && g_sel.size() == 1) {
                // ctrl+corner on a single image = crop
                Shape* s = find_shape(g_sel[0]);
                if (s && s->type == SH_IMAGE) {
                    g_drag = DM_CROP; g_handleIdx = hoverHandle;
                    g_handleStartShapes.clear();
                    g_handleStartShapes.push_back({ s->id, *s });
                    goto down_done;
                }
            }
            if (hoverHandle >= 0) {
                g_handleStartShapes.clear();
                std::vector<uint64_t> all = g_sel;
                for (auto id : g_sel) { Shape* s = find_shape(id); if (s && s->type == SH_GROUP) collect_members(id, all); }
                for (auto id : all) { Shape* s = find_shape(id); if (s) g_handleStartShapes.push_back({ id, *s }); }
                if (hoverHandle < 4) {
                    g_drag = DM_HANDLE; g_handleIdx = hoverHandle;
                    g_handleFixedW = S2W(g_selCorners[(hoverHandle + 2) & 3]);
                    g_handleStartDist = fmaxf(vlen(mw - g_handleFixedW), 0.001f);
                } else {
                    g_drag = DM_ROTATE;
                    g_rotPivotW = S2W(g_selCenterS);
                    g_rotStartAngle = atan2f(mw.y - g_rotPivotW.y, mw.x - g_rotPivotW.x);
                    for (int i = 0; i < 4; i++) g_rotCornersW[i] = S2W(g_selCorners[i]);
                }
                goto down_done;
            }
            if (overlay_contains(io.MousePos)) {
                // press on the video pill: a drag moves the video; a still
                // click acts the control on release (DrawVideoOverlay applies
                // it; the release handler below skips selection semantics)
                g_overlayDownCtl = overlay_ctl_at(io.MousePos);
                g_downLeaf = g_downTarget = g_overlayVid;
                g_downWasSelected = true;
                g_drag = DM_PENDING;
                goto down_done;
            }
            g_downLeaf = hit_test(mw);
            g_downTarget = resolve_target(g_downLeaf);
            if (!g_downTarget) {
                // empty space inside a selected group's box still grabs the
                // group (drag anywhere in the bounds; a plain click deselects)
                for (auto id : g_sel) {
                    Shape* s = find_shape(id);
                    if (s && s->type == SH_GROUP && shape_bounds(*s).contains(mw)) { g_downTarget = id; break; }
                }
            }
            g_downWasSelected = g_downTarget && is_selected(g_downTarget);
            g_drag = DM_PENDING;
        }
    down_done:;
    }

    if (g_drag == DM_PENDING && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.f)) {
        if (g_downTarget) {
            if (!is_selected(g_downTarget)) {
                if (!io.KeyShift) g_sel.clear();
                g_sel.push_back(g_downTarget);
            }
            g_drag = DM_MOVE;
            g_moveStartBounds = selection_bounds();
            g_moveApplied = ImVec2(0, 0);
        } else {
            g_drag = DM_MARQUEE;
        }
    }

    if (g_drag == DM_MOVE) {
        // absolute offset from the drag origin (not incremental deltas), so
        // shift axis-lock and snapping can adjust it without accumulating drift
        ImVec2 want = mw - g_dragStartW;
        if (io.KeyShift) { if (fabsf(want.x) >= fabsf(want.y)) want.y = 0; else want.x = 0; }
        if (io.KeyCtrl) snap_move(want, g_moveStartBounds, dl);   // ctrl = snap to other shapes
        ImVec2 d = want - g_moveApplied;
        if (d.x != 0 || d.y != 0) move_selected(d);
        g_moveApplied = want;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    if (g_drag == DM_MARQUEE) {
        ImVec2 mn(fminf(g_dragStartW.x, mw.x), fminf(g_dragStartW.y, mw.y));
        ImVec2 mx(fmaxf(g_dragStartW.x, mw.x), fmaxf(g_dragStartW.y, mw.y));
        dl->AddRectFilled(W2S(mn), W2S(mx), g_th.selFill, 2.f);
        dl->AddRect(W2S(mn), W2S(mx), g_th.selStroke, 2.f, 0, 1.f);
        if (!io.KeyShift) g_sel.clear();
        WRect mr{ mn, mx };
        // partial overlap selects (hands-on feedback) — plain AABB intersection
        auto touches = [&](const WRect& b) {
            return b.mx.x >= mr.mn.x && b.mn.x <= mr.mx.x && b.mx.y >= mr.mn.y && b.mn.y <= mr.mx.y;
        };
        // dedupe only against the shift-kept selection: the two passes below
        // are disjoint, and an O(sel) scan per shape goes quadratic on big boards
        std::unordered_set<uint64_t> pre(g_sel.begin(), g_sel.end());
        for (auto& s : g_doc.shapes) {
            if (s.parent || s.type == SH_GROUP) continue;
            if (touches(shape_bounds(s)) && !pre.count(s.id)) g_sel.push_back(s.id);
        }
        for (auto& s : g_doc.shapes) {
            if (s.type != SH_GROUP || s.parent) continue;
            if (touches(shape_bounds(s)) && !pre.count(s.id)) g_sel.push_back(s.id);
        }
    }

    if (g_drag == DM_HANDLE) {
        // corner resize: uniform scale about the opposite corner (distance
        // ratio — rotation-agnostic); text scales its continuous font multiplier
        ImVec2 fixed = g_handleFixedW;
        float k = fmaxf(0.05f, vlen(mw - fixed) / g_handleStartDist);
        for (auto& [id, snap] : g_handleStartShapes) {
            Shape* s = find_shape(id); if (!s) continue;
            auto sc = [&](ImVec2 p) { return fixed + (p - fixed) * k; };
            switch (s->type) {
            case SH_TEXT:  s->pos = sc(snap.pos); s->scale = snap.scale * k; break;
            case SH_IMAGE: s->pos = sc(snap.pos); s->size = snap.size * k; break;
            case SH_ARROW:
                if (!snap.a.bind) s->a.p = sc(snap.a.p);
                if (!snap.b.bind) s->b.p = sc(snap.b.p);
                s->bend = snap.bend * k;
                break;
            case SH_GROUP: break;
            }
        }
        ImGui::SetMouseCursor(g_handleIdx % 2 == 0 ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
    }

    if (g_drag == DM_ROTATE) {
        float dth = current_rot_delta(mw, io.KeyShift);
        for (auto& [id, snap] : g_handleStartShapes) {
            Shape* s = find_shape(id); if (!s) continue;
            switch (s->type) {
            case SH_TEXT: case SH_IMAGE: {
                ImVec2 c0 = shape_local_rect(snap).center();
                ImVec2 c1 = rot_about(c0, g_rotPivotW, dth);
                s->rot = snap.rot + dth;
                s->pos = snap.pos + (c1 - c0);
            } break;
            case SH_ARROW:
                if (!snap.a.bind) s->a.p = rot_about(snap.a.p, g_rotPivotW, dth);
                if (!snap.b.bind) s->b.p = rot_about(snap.b.p, g_rotPivotW, dth);
                break;
            case SH_GROUP: s->rot = snap.rot + dth; break;   // the group's frame turns too
            }
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    if (g_drag == DM_CROP && !g_handleStartShapes.empty()) {
        Shape* s = find_shape(g_handleStartShapes[0].first);
        const Shape& snap = g_handleStartShapes[0].second;
        if (s) {
            // all crop math happens in the shape's LOCAL (unrotated) frame;
            // c0 = the snapshot's rotation pivot, fixed for the whole drag
            ImVec2 c0 = snap.pos + snap.size * 0.5f;
            float rt = snap.rot;
            ImVec2 ml = rt != 0.f ? rot_about(mw, c0, -rt) : mw;
            WRect F = image_full_rect(snap);   // full-image projection (local), stays fixed
            // ghost: the whole source at low alpha (+outline), rotated into place
            ImVec2 fc[4] = { F.mn, ImVec2(F.mx.x, F.mn.y), F.mx, ImVec2(F.mn.x, F.mx.y) };
            ImVec2 gp[4];
            for (int i = 0; i < 4; i++) gp[i] = W2S(rt != 0.f ? rot_about(fc[i], c0, rt) : fc[i]);
            Tex* t = get_image_tex(s->asset);
            if (t->srv)
                dl->AddImageQuad((ImTextureID)(intptr_t)t->srv, gp[0], gp[1], gp[2], gp[3],
                                 ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1),
                                 IM_COL32(255, 255, 255, 80));
            dl->AddPolyline(gp, 4, g_th.selStroke, ImDrawFlags_Closed, 1.f);
            // drag the grabbed corner of the display rect, clamped inside F
            ImVec2 mn = snap.pos, mx = snap.pos + snap.size;
            ImVec2 p(fminf(fmaxf(ml.x, F.mn.x), F.mx.x), fminf(fmaxf(ml.y, F.mn.y), F.mx.y));
            float minSz = 8.f;
            if (g_handleIdx == 0)      { mn.x = fminf(p.x, mx.x - minSz); mn.y = fminf(p.y, mx.y - minSz); }
            else if (g_handleIdx == 1) { mx.x = fmaxf(p.x, mn.x + minSz); mn.y = fminf(p.y, mx.y - minSz); }
            else if (g_handleIdx == 2) { mx.x = fmaxf(p.x, mn.x + minSz); mx.y = fmaxf(p.y, mn.y + minSz); }
            else                       { mn.x = fminf(p.x, mx.x - minSz); mx.y = fmaxf(p.y, mn.y + minSz); }
            s->size = mx - mn;
            // reposition so the full-image projection is visually unmoved even
            // though the rotation pivot migrates to the new rect's center
            ImVec2 cl = (mn + mx) * 0.5f;
            ImVec2 cw = rt != 0.f ? rot_about(cl, c0, rt) : cl;
            s->pos = cw - s->size * 0.5f;
            ImVec2 fsz = F.size();
            s->crop = ImVec4((mn.x - F.mn.x) / fsz.x, (mn.y - F.mn.y) / fsz.y,
                             (mx.x - F.mn.x) / fsz.x, (mx.y - F.mn.y) / fsz.y);
            ImGui::SetMouseCursor(g_handleIdx % 2 == 0 ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
        }
    }

    if (g_drag == DM_ARROW_A || g_drag == DM_ARROW_B || g_drag == DM_NEW_ARROW || g_drag == DM_BEND) {
        Shape* s = find_shape(g_drag == DM_NEW_ARROW ? g_newArrowId : (g_sel.empty() ? 0 : g_sel[0]));
        if (s && s->type == SH_ARROW) {
            if (g_drag == DM_BEND) {
                ImVec2 A = arrow_end_pos(s->a), B = arrow_end_pos(s->b);
                ImVec2 chord = B - A; float cl = vlen(chord);
                if (cl > 0.0001f) {
                    ImVec2 perp(-chord.y / cl, chord.x / cl);
                    ImVec2 rel = mw - (A + B) * 0.5f;
                    float d = rel.x * perp.x + rel.y * perp.y;
                    s->bend = fabsf(d) < screen_px(6.f) ? 0.f : d;   // snap straight near the chord
                }
            } else {
                ArrowEnd& e = (g_drag == DM_ARROW_A) ? s->a : s->b;
                try_bind(e, mw, s->id);
            }
        }
    }

    // ── release ──
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        double now = ImGui::GetTime();
        bool moved = vlen(io.MousePos - g_dragStartS) >= 4.f;

        if (g_drag == DM_PENDING && !moved && g_overlayDownCtl == -2) {
            // pure click: selection semantics (pill presses skip this —
            // DrawVideoOverlay acts the clicked control instead)
            bool dbl = (now - g_lastClickTime < 0.32) && vlen(io.MousePos - g_lastClickPos) < 6.f
                       && g_lastClickLeaf == g_downLeaf;
            Shape* leafS = find_shape(g_downLeaf);
            if (!g_downLeaf) {
                if (!io.KeyShift) clear_selection();
            } else if (io.KeyShift) {
                // toggle
                if (is_selected(g_downTarget)) {
                    for (size_t i = 0; i < g_sel.size(); i++) if (g_sel[i] == g_downTarget) { g_sel.erase(g_sel.begin() + i); break; }
                } else g_sel.push_back(g_downTarget);
            } else if (leafS && leafS->type == SH_ARROW && dbl) {
                // double-click arrow → edit its label
                g_sel.clear(); g_sel.push_back(g_downLeaf);
                g_editLabelArrow = g_downLeaf; g_editTextTakeFocus = true; g_editEverActive = false;
                g_editCaretIdx = (int)leafS->label.size();   // caret at end, no stale selection
                g_editPrev = leafS->label;
                break_click_chain();
            } else if (g_downWasSelected && g_downTarget != g_downLeaf) {
                // click again on a selected group → drill one level toward the leaf
                std::vector<uint64_t> chain;   // leaf … outermost
                uint64_t cur = g_downLeaf;
                while (cur) { chain.push_back(cur); Shape* c = find_shape(cur); cur = c ? c->parent : 0; }
                for (size_t i = 0; i + 1 < chain.size(); i++)
                    if (chain[i + 1] == g_downTarget) {
                        g_drill = g_downTarget;
                        g_sel.clear(); g_sel.push_back(chain[i]);
                        break;
                    }
            } else if (leafS && leafS->type == SH_TEXT && g_downTarget == g_downLeaf &&
                       g_downWasSelected && g_sel.size() == 1) {
                // click on already-selected text → edit, caret under the click
                begin_text_edit(g_downLeaf, caret_index_from_click(*leafS, mw));
            } else {
                g_sel.clear(); g_sel.push_back(g_downTarget);
            }
            g_lastClickTime = now; g_lastClickPos = io.MousePos; g_lastClickLeaf = g_downLeaf;
        }

        if (g_drag == DM_MOVE || g_drag == DM_HANDLE || g_drag == DM_CROP ||
            g_drag == DM_ROTATE || g_drag == DM_ARROW_A || g_drag == DM_ARROW_B || g_drag == DM_BEND)
            push_undo();

        if (g_drag == DM_NEW_ARROW) {
            Shape* s = find_shape(g_newArrowId);
            if (s) {
                ImVec2 pa = arrow_end_pos(s->a), pb = arrow_end_pos(s->b);
                if (vlen(pb - pa) < screen_px(6.f)) {
                    std::vector<uint64_t> one{ g_newArrowId }; delete_shapes(one);
                } else {
                    g_sel.clear(); g_sel.push_back(g_newArrowId);
                    push_undo();
                }
            }
            g_newArrowId = 0;
            g_tool = TOOL_SELECT;   // arrows are usually one-offs; bounce back to select
        }
        g_drag = DM_NONE; g_downLeaf = g_downTarget = 0; g_handleIdx = -1;
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
    case WM_DROPFILES: {
        HDROP hd = (HDROP)w;
        POINT pt; DragQueryPoint(hd, &pt);
        g_dropPoint = ImVec2((float)pt.x, (float)pt.y);
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t buf[MAX_PATH * 2];
            if (DragQueryFileW(hd, i, buf, MAX_PATH * 2)) g_dropFiles.push_back(from_w(buf));
        }
        DragFinish(hd);
    } return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    // teidraw [projectDir] [--shot out.png | --export out.png | --export-txt out.txt] [--frames N]
    // No projectDir: reopen the last board (recent[0]); first run = the picker.
    const char* shotPath = nullptr; int shotFrames = 8;
    const char* exportPng = nullptr; const char* exportTxt = nullptr;
    std::string boardArg; bool forcePicker = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shotPath = argv[++i];
        else if (!strcmp(argv[i], "--export") && i + 1 < argc) exportPng = argv[++i];
        else if (!strcmp(argv[i], "--export-txt") && i + 1 < argc) exportTxt = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) shotFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--picker")) forcePicker = true;   // dev: shot the picker UI
        else if (argv[i][0] != '-') boardArg = argv[i];
    }
    bool headless = shotPath || exportPng || exportTxt;
    g_headless = headless;
    load_settings();
    if (boardArg.empty() && headless && !forcePicker) boardArg = "scratch";
    if (boardArg.empty() && !forcePicker && !g_recentBoards.empty()) boardArg = g_recentBoards[0];
    g_pickerWant = forcePicker;

    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_OWNDC, WndProc, 0, 0, GetModuleHandleW(nullptr),
                       nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"teidraw", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(L"teidraw", L"teidraw", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1600, 1000, nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd)) { fprintf(stderr, "teidraw: D3D11 init failed\n"); return 1; }
    g_hwnd = hwnd;
    DragAcceptFiles(hwnd, TRUE);
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;                       // no imgui.ini litter; layout is ours
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    g_dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    LoadFonts();
    ImGui::GetStyle().FontSizeBase = 15.f * g_dpi;
    if (!boardArg.empty()) switch_board(boardArg);   // empty = picker opens over a blank canvas
    ApplyTheme();
    ImGui::GetStyle().ScaleAllSizes(g_dpi);

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
        // Editing a ROTATED text: imgui runs the editor in the shape's
        // unrotated space. Inverse-rotate the pointer here (after the backend
        // wrote it, before NewFrame snapshots it); the overlay rotates the
        // editor's draw output back into place.
        g_editRemap = false;
        if (g_editText) {
            Shape* es = find_shape(g_editText);
            if (es) {
                if (es->rot != 0.f && kWysiwygRotEdit) {
                    ImVec2 c = W2S(shape_local_rect(*es).center());
                    io.MousePos = rot_about(io.MousePos, c, -es->rot);
                    g_editRemap = true;
                }
                // per-line alignment: un-shift the pointer by the offset of
                // the line it hovers (last frame's layout — 1 frame of lag)
                if (g_editShiftActive && !g_editLineDx.empty()) {
                    int li = (int)floorf((io.MousePos.y - g_editShiftTopY) / g_editShiftPx);
                    li = li < 0 ? 0 : (li >= (int)g_editLineDx.size() ? (int)g_editLineDx.size() - 1 : li);
                    io.MousePos.x -= g_editLineDx[li];
                    g_editRemap = true;
                }
            }
        }
        ImGui::NewFrame();

#ifdef TEI_LIBAV
        drain_video_results();   // worker-decoded frames land before the draw pass
#endif
        CanvasFrame();
        DrawContextMenu();
        DrawVideoOverlay();
        DrawTextEditOverlay();
        DrawToolbar();
        DrawStylePanel();
        DrawZoomPill();
        DrawBoardPicker();
        sweep_play_states();

        if (!io.WantTextInput && io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_D)) {
            g_darkMode = !g_darkMode; ApplyTheme(); save_settings();
        }

        // autosave debounce
        if (g_saveDueAt > 0 && ImGui::GetTime() >= g_saveDueAt) save_board_now();

        // CLI exports: text extents need a live frame, so requests are made
        // here (mid-frame) once media has had shotFrames frames to decode
        if ((exportPng || exportTxt) && framesDone + 1 >= shotFrames) {
            if (exportTxt) {
                std::ofstream f(exportTxt, std::ios::binary);
                std::string outline = board_outline(false);
                f.write(outline.c_str(), (std::streamsize)outline.size());
                fprintf(stderr, "teidraw: export %s -> %s\n", exportTxt, f.good() ? "ok" : "FAILED");
                exportTxt = nullptr;
                if (!exportPng) done = true;
            }
            if (exportPng) {
                request_png_export(false, exportPng);
                if (g_export.active) g_export.quit = true;
                else { fprintf(stderr, "teidraw: export %s -> FAILED (empty board)\n", exportPng); done = true; }
                exportPng = nullptr;
            }
        }

        ImGui::Render();
        float bg[4] = { 0, 0, 0, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, bg);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_sc->Present(headless ? 0 : 1, 0);
        framesDone++;

        if (shotPath && framesDone >= shotFrames) {
            bool ok = SaveBackbufferPNG(shotPath);
            fprintf(stderr, "teidraw: shot %s -> %s\n", shotPath, ok ? "ok" : "FAILED");
            done = true;
        }
        // queued copy-as-PNG / --export: render offscreen between frames
        if (g_export.active && run_pending_export()) done = true;
    }

#ifdef TEI_LIBAV
    if (g_vqWorker.joinable()) {
        { std::lock_guard<std::mutex> lk(g_vqMx); g_vqQuit = true; }
        g_vqCv.notify_all();
        g_vqWorker.join();
    }
#endif
    save_board_now();
    save_settings();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(L"teidraw", wc.hInstance);
    return 0;
}
