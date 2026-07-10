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
#include <d3d11.h>
#include <dxgi1_3.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "misc/cpp/imgui_stdlib.h"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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

// ───────────────────────────────── fonts ───────────────────────────────────
// Four families, dynamic atlas (imgui ≥1.92): glyphs rasterize on demand at
// whatever pixel size we push, so canvas text is crisp at every zoom level.
enum FontFamily { FF_HAND = 0, FF_SANS, FF_MONO, FF_SERIF, FF_COUNT };
static ImFont* g_fonts[FF_COUNT] = {};
static const char* kFamilyName[FF_COUNT] = { "hand", "sans", "mono", "serif" };
// four canvas text sizes, big-by-default (L) per the main use case
static const float kTextSizes[4] = { 20.f, 28.f, 40.f, 56.f };
static const char* kTextSizeName[4] = { "S", "M", "L", "XL" };
static const int   kDefaultTextSize = 2;   // L
// Glyphs above this rasterized px size get drawn as scaled-up smaller glyphs
// (keeps the dynamic atlas from ballooning when zoomed way in).
static const float kMaxGlyphPx = 320.f;

static void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg; cfg.FontDataOwnedByAtlas = false;
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

static void ZoomAt(ImVec2 pivot, float factor) {
    float z = g_cam.zoom * factor;
    z = z < 0.02f ? 0.02f : (z > 64.f ? 64.f : z);
    factor = z / g_cam.zoom;
    g_cam.pan.x = pivot.x - (pivot.x - g_cam.pan.x) * factor;
    g_cam.pan.y = pivot.y - (pivot.y - g_cam.pan.y) * factor;
    g_cam.zoom = z;
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
    // text
    std::string text;
    int   family = FF_HAND;
    int   tsize = kDefaultTextSize;
    float scale = 1.f;      // continuous scale from corner-resize (multiplies font px)
    ImVec2 pos{0, 0};       // text/image top-left (world)
    // image / video (M2)
    std::string asset;
    ImVec2 size{0, 0};
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

static Shape* find_shape(uint64_t id) {
    if (!id) return nullptr;
    for (auto& s : g_doc.shapes) if (s.id == id) return &s;
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

// ── shape geometry ──
static float text_px(const Shape& s) { return kTextSizes[s.tsize] * s.scale; }

static ImVec2 text_extent(const Shape& s) {
    ImFont* f = g_fonts[s.family];
    const char* txt = s.text.empty() ? " " : s.text.c_str();
    ImGui::PushFont(f, text_px(s));
    ImVec2 sz = ImGui::CalcTextSize(txt);
    ImGui::PopFont();
    return sz;
}

static WRect shape_bounds(const Shape& s);   // fwd
static ImVec2 arrow_end_pos(const ArrowEnd& e) {
    if (e.bind) {
        Shape* t = find_shape(e.bind);
        if (t) { WRect b = shape_bounds(*t);
                 return ImVec2(b.mn.x + b.size().x * e.anchor.x, b.mn.y + b.size().y * e.anchor.y); }
    }
    return e.p;
}

static WRect shape_bounds(const Shape& s) {
    WRect r;
    switch (s.type) {
    case SH_TEXT: { ImVec2 e = text_extent(s); r.mn = s.pos; r.mx = s.pos + e; } break;
    case SH_IMAGE: r.mn = s.pos; r.mx = s.pos + s.size; break;
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
        WRect b = shape_bounds(*t);
        b.mn = b.mn - ImVec2(gap, gap); b.mx = b.mx + ImVec2(gap, gap);
        // walk inward from this end, drop samples inside the (padded) bbox
        if (fromStart) {
            size_t i = 0;
            while (i + 1 < out.size() && b.contains(out[i])) i++;
            if (i > 0) out.erase(out.begin(), out.begin() + i);
        } else {
            size_t i = out.size();
            while (i > 1 && b.contains(out[i - 1])) i--;
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
    switch (s.type) {
    case SH_TEXT:
        j["text"] = s.text; j["family"] = s.family; j["tsize"] = s.tsize;
        if (s.scale != 1.f) j["scale"] = s.scale;
        j["x"] = s.pos.x; j["y"] = s.pos.y;
        break;
    case SH_IMAGE:
        j["asset"] = s.asset; j["x"] = s.pos.x; j["y"] = s.pos.y;
        j["w"] = s.size.x; j["h"] = s.size.y;
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
    case SH_GROUP: break;
    }
    return j;
}
static Shape shape_from_json(const json& j) {
    Shape s;
    s.id = j.value("id", 0ULL); s.type = (ShapeType)j.value("type", 0);
    s.parent = j.value("parent", 0ULL);
    switch (s.type) {
    case SH_TEXT:
        s.text = j.value("text", std::string());
        s.family = j.value("family", (int)FF_HAND); s.tsize = j.value("tsize", kDefaultTextSize);
        s.scale = j.value("scale", 1.f);
        s.pos = ImVec2(j.value("x", 0.f), j.value("y", 0.f));
        break;
    case SH_IMAGE:
        s.asset = j.value("asset", std::string());
        s.pos = ImVec2(j.value("x", 0.f), j.value("y", 0.f));
        s.size = ImVec2(j.value("w", 0.f), j.value("h", 0.f));
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
    case SH_GROUP: break;
    }
    return s;
}
static std::string doc_to_json_string() {
    json j;
    j["v"] = 1; j["nextId"] = g_doc.nextId; j["dark"] = g_darkMode;
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
    if (restoreCam && j.contains("cam")) {
        g_cam.pan = ImVec2(j["cam"].value("x", 0.f), j["cam"].value("y", 0.f));
        g_cam.zoom = j["cam"].value("z", 1.f);
    }
    if (j.contains("dark") && restoreCam) { g_darkMode = j["dark"]; }
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
    write_file_atomic(g_projDir + "/board.json", doc_to_json_string());
    g_saveDueAt = 0;
}

// ── undo: full-document snapshots, journaled to disk so history survives
// sessions. Deliberately memory-piggy (user's call) — capped by undoLimit.
static std::vector<std::string> g_undo;   // snapshot stack; g_undoPos = current
static int g_undoPos = -1;
static int g_undoLimit = 4096;

static void undo_journal_rewrite() {
    std::string path = g_projDir + "/undo.jsonl";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    for (auto& s : g_undo) f << s << "\n";
}
static void push_undo() {
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
    g_doc.shapes.push_back(g);
    for (auto id : g_sel) { Shape* s = find_shape(id); if (s) s->parent = g.id; }
    g_sel.clear(); g_sel.push_back(g.id); g_drill = 0;
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

// ─────────────────────────────── hit testing ───────────────────────────────
static float screen_px(float px) { return px / g_cam.zoom; }   // px → world units

// topmost leaf under the point (never returns groups)
static uint64_t hit_test(ImVec2 w) {
    for (int i = (int)g_doc.shapes.size() - 1; i >= 0; i--) {
        Shape& s = g_doc.shapes[i];
        if (s.type == SH_GROUP) continue;
        if (s.type == SH_ARROW) {
            std::vector<ImVec2> pl; arrow_polyline(s, pl);
            float th = screen_px(8.f);
            for (size_t k = 0; k + 1 < pl.size(); k++)
                if (dist_point_seg(w, pl[k], pl[k + 1]) < th) return s.id;
            continue;
        }
        WRect b = shape_bounds(s);
        if (b.contains(w)) return s.id;
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

static void draw_text_shape(ImDrawList* dl, const Shape& s) {
    float px = text_px(s) * g_cam.zoom;
    ImVec2 sp = W2S(s.pos);
    ImU32 col = g_th.textMain;
    if (px <= kMaxGlyphPx) {
        dl->AddText(g_fonts[s.family], px, sp, col, s.text.c_str());
    } else {
        // zoomed way in: draw at capped raster size, scale geometry up
        float k = px / kMaxGlyphPx;
        int vtx0 = dl->VtxBuffer.Size;
        dl->AddText(g_fonts[s.family], kMaxGlyphPx, ImVec2(0, 0), col, s.text.c_str());
        for (int i = vtx0; i < dl->VtxBuffer.Size; i++) {
            ImDrawVert& v = dl->VtxBuffer[i];
            v.pos.x = v.pos.x * k + sp.x; v.pos.y = v.pos.y * k + sp.y;
        }
    }
}

static void draw_arrow_shape(ImDrawList* dl, const Shape& s, bool ghostEnd = false) {
    std::vector<ImVec2> pl; arrow_polyline(s, pl);
    if (pl.size() < 2) return;
    float thick = fmaxf(2.f * g_cam.zoom, 1.2f);
    ImU32 col = g_th.textMain;
    std::vector<ImVec2> sp(pl.size());
    for (size_t i = 0; i < pl.size(); i++) sp[i] = W2S(pl[i]);
    // reserve room at the tip for the head
    ImVec2 tip = sp.back();
    ImVec2 dir = sp[sp.size() - 1] - sp[sp.size() - 2];
    float dl2 = vlen(dir); if (dl2 > 0.0001f) dir = dir * (1.f / dl2);
    float head = fminf(fmaxf(10.f * g_cam.zoom, 8.f), 26.f);
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
        dl->AddText(g_fonts[s.family], lpx, mid - ext * 0.5f, g_th.textMain, s.label.c_str());
    }
}

// ─────────────────────────── canvas interaction ────────────────────────────
enum DragMode { DM_NONE = 0, DM_PENDING, DM_MOVE, DM_MARQUEE, DM_HANDLE,
                DM_ARROW_A, DM_ARROW_B, DM_BEND, DM_NEW_ARROW, DM_PAN_R };
static DragMode g_drag = DM_NONE;
static ImVec2   g_dragStartW, g_dragStartS;    // world/screen at mousedown
static ImVec2   g_lastDragW;
static uint64_t g_downLeaf = 0, g_downTarget = 0;
static bool     g_downWasSelected = false;
static int      g_handleIdx = -1;              // 0 tl, 1 tr, 2 br, 3 bl
static WRect    g_handleStartBounds;
static std::vector<std::pair<uint64_t, Shape>> g_handleStartShapes;  // id → snapshot
static uint64_t g_newArrowId = 0;
static bool     g_rDrag = false;               // right button turned into a pan

enum Tool { TOOL_SELECT = 0, TOOL_HAND, TOOL_TEXT, TOOL_ARROW, TOOL_COUNT };
static Tool g_tool = TOOL_SELECT;
static bool g_spacePan = false;

static void begin_text_edit(uint64_t id) {
    g_editText = id; g_editTextTakeFocus = true;
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

static uint64_t create_text_at(ImVec2 w) {
    Shape s; s.id = new_id(); s.type = SH_TEXT; s.pos = w;
    s.tsize = kDefaultTextSize;
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
        WRect b = shape_bounds(s);
        if (b.contains(w)) {
            ImVec2 sz = b.size();
            e.bind = s.id;
            e.anchor = ImVec2(sz.x > 0 ? (w.x - b.mn.x) / sz.x : 0.5f,
                              sz.y > 0 ? (w.y - b.mn.y) / sz.y : 0.5f);
            return;
        }
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

// selection outline + corner handles; returns hovered handle (-1 none)
static int draw_selection_ui(ImDrawList* dl, bool handlesActive) {
    if (g_sel.empty()) return -1;
    WRect b = selection_bounds();
    ImVec2 mn = W2S(b.mn) - ImVec2(4, 4), mx = W2S(b.mx) + ImVec2(4, 4);
    dl->AddRect(mn, mx, g_th.selStroke, 4.f, 0, 1.5f);
    int hover = -1;
    if (!handlesActive) return -1;
    ImVec2 corners[4] = { mn, ImVec2(mx.x, mn.y), mx, ImVec2(mn.x, mx.y) };
    ImVec2 m = ImGui::GetIO().MousePos;
    for (int i = 0; i < 4; i++) {
        float r = 5.f;
        bool h = fabsf(m.x - corners[i].x) < r + 3 && fabsf(m.y - corners[i].y) < r + 3;
        if (h) hover = i;
        dl->AddCircleFilled(corners[i], r, g_th.handleFill);
        dl->AddCircle(corners[i], r, g_th.selStroke, 0, 1.5f);
    }
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
    if (ImGui::Button(z)) {
        ImVec2 c = ImVec2(ImGui::GetMainViewport()->Size.x * 0.5f, ImGui::GetMainViewport()->Size.y * 0.5f);
        ZoomAt(c, 1.f / g_cam.zoom);
    }
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
        if (ImGui::MenuItem(g_darkMode ? "light mode" : "dark mode", "Ctrl+Shift+D")) {
            g_darkMode = !g_darkMode; ApplyTheme(); g_saveDueAt = ImGui::GetTime() + 0.4;
        }
    }
    ImGui::EndPopup();
}

// in-place text editor overlay (canvas text + arrow labels share it)
static void DrawTextEditOverlay() {
    uint64_t id = g_editText ? g_editText : g_editLabelArrow;
    if (!id) return;
    Shape* s = find_shape(id);
    if (!s) { g_editText = g_editLabelArrow = 0; return; }
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

    ImGui::PushFont(font, px);
    ImVec2 ext = ImGui::CalcTextSize(str->empty() ? " " : str->c_str());
    float minW = ImGui::CalcTextSize("MM").x;
    ImVec2 box(fmaxf(ext.x, minW) + px * 0.75f, ext.y + px * 0.5f);
    ImVec2 winPos = isLabel ? anchor - box * 0.5f : anchor;

    ImGui::SetNextWindowPos(winPos - ImVec2(8, 8));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, isLabel ? ImGui::ColorConvertU32ToFloat4(g_th.canvasBg) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::Begin("##textedit", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav);
    if (g_editTextTakeFocus) { ImGui::SetKeyboardFocusHere(); g_editTextTakeFocus = false; }
    std::string before = *str;
    ImGui::InputTextMultiline("##t", str, box, ImGuiInputTextFlags_NoHorizontalScroll);
    bool active = ImGui::IsItemActive();
    bool deactivated = ImGui::IsItemDeactivated();
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
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    ImGui::PopFont();
}

// ───────────────────────────── the canvas frame ────────────────────────────
static double g_lastClickTime = -1; static ImVec2 g_lastClickPos; static uint64_t g_lastClickLeaf = 0;

static void CanvasFrame() {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    dl->AddRectFilled(vp->Pos, vp->Pos + vp->Size, g_th.canvasBg);
    DrawGrid(dl, vp->Size);

    // ── draw shapes (doc order = z) ──
    for (auto& s : g_doc.shapes) {
        if (s.id == g_editText) continue;    // editor overlay draws it
        switch (s.type) {
        case SH_TEXT:  draw_text_shape(dl, s); break;
        case SH_ARROW: draw_arrow_shape(dl, s); break;
        case SH_IMAGE: {   // placeholder until M2 media lands
            WRect b = shape_bounds(s);
            dl->AddRectFilled(W2S(b.mn), W2S(b.mx), IM_COL32(120, 120, 128, 60), 6.f);
        } break;
        case SH_GROUP: break;
        }
    }

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

    // ── keyboard ──
    if (!io.WantTextInput) {
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
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G)) {
            if (io.KeyShift) ungroup_selected(); else group_selected();
            push_undo();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) { reorder_selected(true); push_undo(); }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))  { reorder_selected(false); push_undo(); }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (g_drill) g_drill = 0;
            else clear_selection();
        }
    } else g_spacePan = false;

    if (uiHot && g_drag == DM_NONE) return;

    ImVec2 mw = S2W(io.MousePos);

    // ── wheel = zoom at cursor (THE departure from tldraw: scroll never pans) ──
    if (io.MouseWheel != 0.f && !uiHot)
        ZoomAt(io.MousePos, powf(1.16f, io.MouseWheel));

    // ── middle drag / hand / space: pan ──
    bool handActive = (g_tool == TOOL_HAND) || g_spacePan;
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.f) ||
        (handActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f))) {
        g_cam.pan = g_cam.pan + io.MouseDelta;
        return;
    }
    if (handActive) return;

    // ── right button: drag = pan, click = context menu ──
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !uiHot) g_rDrag = false;
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.f)) { g_cam.pan = g_cam.pan + io.MouseDelta; g_rDrag = true; }
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
        g_dragStartS = io.MousePos; g_dragStartW = mw; g_lastDragW = mw;

        if (g_tool == TOOL_TEXT) {
            uint64_t id = create_text_at(mw);
            g_sel.clear(); g_sel.push_back(id);
            begin_text_edit(id);
            g_tool = TOOL_SELECT;
            g_drag = DM_NONE;
        } else if (g_tool == TOOL_ARROW) {
            Shape s; s.id = new_id(); s.type = SH_ARROW;
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
            if (hoverHandle >= 0) {
                g_drag = DM_HANDLE; g_handleIdx = hoverHandle;
                g_handleStartBounds = selection_bounds();
                g_handleStartShapes.clear();
                std::vector<uint64_t> all = g_sel;
                for (auto id : g_sel) { Shape* s = find_shape(id); if (s && s->type == SH_GROUP) collect_members(id, all); }
                for (auto id : all) { Shape* s = find_shape(id); if (s) g_handleStartShapes.push_back({ id, *s }); }
                goto down_done;
            }
            g_downLeaf = hit_test(mw);
            g_downTarget = resolve_target(g_downLeaf);
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
        } else {
            g_drag = DM_MARQUEE;
        }
    }

    if (g_drag == DM_MOVE) {
        ImVec2 d = mw - g_lastDragW;
        if (d.x != 0 || d.y != 0) move_selected(d);
        g_lastDragW = mw;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    if (g_drag == DM_MARQUEE) {
        ImVec2 mn(fminf(g_dragStartW.x, mw.x), fminf(g_dragStartW.y, mw.y));
        ImVec2 mx(fmaxf(g_dragStartW.x, mw.x), fmaxf(g_dragStartW.y, mw.y));
        dl->AddRectFilled(W2S(mn), W2S(mx), g_th.selFill, 2.f);
        dl->AddRect(W2S(mn), W2S(mx), g_th.selStroke, 2.f, 0, 1.f);
        if (!io.KeyShift) g_sel.clear();
        WRect mr{ mn, mx };
        for (auto& s : g_doc.shapes) {
            if (s.parent || s.type == SH_GROUP) continue;
            WRect b = shape_bounds(s);
            if (b.mn.x >= mr.mn.x && b.mx.x <= mr.mx.x && b.mn.y >= mr.mn.y && b.mx.y <= mr.mx.y)
                if (!is_selected(s.id)) g_sel.push_back(s.id);
        }
        // top-level groups fully inside get selected as groups
        for (auto& s : g_doc.shapes) {
            if (s.type != SH_GROUP || s.parent) continue;
            WRect b = shape_bounds(s);
            if (b.mn.x >= mr.mn.x && b.mx.x <= mr.mx.x && b.mn.y >= mr.mn.y && b.mx.y <= mr.mx.y)
                if (!is_selected(s.id)) g_sel.push_back(s.id);
        }
    }

    if (g_drag == DM_HANDLE) {
        // corner resize: scale about the opposite corner; text scales its font
        WRect& b0 = g_handleStartBounds;
        ImVec2 fixed = g_handleIdx == 0 ? b0.mx : g_handleIdx == 1 ? ImVec2(b0.mn.x, b0.mx.y)
                     : g_handleIdx == 2 ? b0.mn : ImVec2(b0.mx.x, b0.mn.y);
        ImVec2 sz0 = b0.size();
        float k = 1.f;
        if (sz0.x > 1 || sz0.y > 1) {
            ImVec2 d0 = (g_handleIdx == 0 ? b0.mn : g_handleIdx == 1 ? ImVec2(b0.mx.x, b0.mn.y)
                        : g_handleIdx == 2 ? b0.mx : ImVec2(b0.mn.x, b0.mx.y)) - fixed;
            ImVec2 d1 = mw - fixed;
            float k0 = fabsf(d0.x) > fabsf(d0.y) ? d1.x / d0.x : d1.y / d0.y;
            k = fmaxf(0.05f, k0);
        }
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

        if (g_drag == DM_PENDING && !moved) {
            // pure click: selection semantics
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
                g_editLabelArrow = g_downLeaf; g_editTextTakeFocus = true;
                g_editPrev = leafS->label;
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
                       (g_downWasSelected || !leafS->parent || g_drill)) {
                // click text → edit it (grouped text needs its drill click first)
                g_sel.clear(); g_sel.push_back(g_downLeaf);
                begin_text_edit(g_downLeaf);
            } else {
                g_sel.clear(); g_sel.push_back(g_downTarget);
            }
            g_lastClickTime = now; g_lastClickPos = io.MousePos; g_lastClickLeaf = g_downLeaf;
        }

        if (g_drag == DM_MOVE || g_drag == DM_HANDLE || g_drag == DM_ARROW_A ||
            g_drag == DM_ARROW_B || g_drag == DM_BEND)
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
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    // teidraw [projectDir] [--shot out.png [--frames N]]
    const char* shotPath = nullptr; int shotFrames = 8;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shotPath = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) shotFrames = atoi(argv[++i]);
        else if (argv[i][0] != '-') g_projDir = argv[i];
    }
    if (g_projDir.empty()) g_projDir = "scratch";

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
    g_dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    LoadFonts();
    ImGui::GetStyle().FontSizeBase = 15.f * g_dpi;
    load_board();
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
        ImGui::NewFrame();

        CanvasFrame();
        DrawContextMenu();
        DrawTextEditOverlay();
        DrawToolbar();
        DrawZoomPill();

        if (!io.WantTextInput && io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_D)) {
            g_darkMode = !g_darkMode; ApplyTheme(); g_saveDueAt = ImGui::GetTime() + 0.4;
        }

        // autosave debounce
        if (g_saveDueAt > 0 && ImGui::GetTime() >= g_saveDueAt) save_board_now();

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

    save_board_now();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(L"teidraw", wc.hInstance);
    return 0;
}
