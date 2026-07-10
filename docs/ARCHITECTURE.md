# teidraw architecture

Goal: tldraw's polish in a native app. Every decision below serves either
**latency**, **crispness**, or **the guess-what-I-want UX**.

## Stack
- **C++17, single translation unit** (`editor/src/main.cpp`), Dear ImGui
  drawlists for ALL canvas rendering, D3D11 backend, Win32 windowing.
  Cross-compiled Win64 PE via mingw-w64 from the nix flake; runs on the
  Windows host through WSLInterop (no WSLg compositor tax).
- **imgui pinned ≥1.92** (flake `fetchFromGitHub`): the dynamic font atlas
  (`RendererHasTextures`) rasterizes glyphs on demand at any pixel size —
  the load-bearing feature for crisp text at arbitrary zoom.
- **libav (static, cross)** for gif+video decode in-process; stb_image for
  stills; stb_image_write for `--shot`/exports. nlohmann for board JSON.

## Presentation / latency
Flip-model swapchain: `FLIP_DISCARD`, 3 buffers, `FRAME_LATENCY_WAITABLE_OBJECT`,
`SetMaximumFrameLatency(1)`, `Present(1,0)`, `DXGI_SCALING_NONE`,
`MakeWindowAssociation(NO_ALT_ENTER)`. Frame order: **wait on the waitable →
pump Win32 messages → build imgui frame → render → present**. Waiting *before*
input sampling is the whole trick: input is at most one refresh old, the GPU
queue never grows, vsync prevents tearing. `--shot` presents with vsync off.

## Coordinate model
World units = px at zoom 1. `screen = world * zoom + pan`. Zoom pivots on the
cursor (`ZoomAt`), clamped 0.02–64. Camera persists per board.

## Document model
Flat `std::vector<Shape>`; vector order = z order. `Shape` is a tagged struct
(TEXT / ARROW / IMAGE / GROUP) with a `parent` group id (groups are real shapes
with derived bounds, no geometry). Arrows: two `ArrowEnd`s, each either a world
point or a **binding** `{shapeId, normalized anchor}` + `bend` (signed ⊥ offset
of the on-curve midpoint; control point = `mid + 2·bend·perp`). Rendering
samples the quadratic bezier into a polyline, then **trims each bound end at
the target's padded bbox** — the anchor stays exact, the visible line never
enters the shape. Images: display rect `pos/size` + normalized **crop UV**
window + `loopA/loopB` for video A-B loops.

## Persistence & undo
A board is a directory: `board.json` (atomic tmp+rename autosave, 400 ms
debounce after each gesture) + `assets/` (imports are copied in — boards are
self-contained and portable) + `undo.jsonl`. Undo = **full-document JSON
snapshots** (deliberately memory-piggy for simplicity/robustness; capped by
`g_undoLimit`, default 4096). The snapshot stack journals to `undo.jsonl`
(append on push, rewrite on branch), so **undo history survives sessions**.

## Media pipeline
Stills: stb → immutable SRV, cached per asset path (failures cached too).
Gif/video: resident `VideoDecoder` per asset (LRU cap 6) — avformat seek +
avcodec decode + swscale→RGBA, forward-decode reuse for sequential playback —
into one `D3D11_USAGE_DYNAMIC` texture per playing shape, mapped WRITE_DISCARD
only when the wanted frame index changes. Gifs autoplay-loop chrome-free;
videos get the hover pill (play/pause/stop/seek/A-B). No audio yet (roadmap).

## Input / interaction
One explicit drag state machine (`DragMode`): PENDING → MOVE/MARQUEE at a 4 px
threshold; HANDLE (aspect-locked scale about the opposite corner — text scales
a continuous font multiplier); CROP (ctrl+corner, display-rect corner clamped
inside the fixed full-image projection, ghost at 30 % alpha); ARROW_A/B
(rebind on release position), BEND, NEW_ARROW (tool auto-returns to select).
Selection resolves clicks through the group chain (`resolve_target`), with a
one-group drill level (`g_drill`). Text editing is an imgui
`InputTextMultiline` overlay with transparent chrome pushed at the exact
canvas font size — imgui's stb_textedit gives caret/selection for free
(escape = commit-what-you-see via a shadow copy, countering imgui's revert).

## Fonts
Embedded into the PE by `tools/embed.py` (single-file exe): Shantell Sans
(handwriting default — what tldraw uses; vendored, OFL), Inter (sans),
JetBrains Mono, Lora (serif) from nixpkgs. Four canvas sizes S/M/L/XL =
20/28/40/56 world px, default L (big text preferred), times a continuous
resize scale.

## Rejected / deferred
- nixpkgs imgui 1.91 (no dynamic fonts) — pinned 1.92.4 instead.
- Legacy DISCARD swapchain (slopstudio) — flip model is strictly better here.
- Per-op undo deltas — snapshots are simpler and RAM is cheap (user's call).
- freetype rasterizer, NVDEC/d3d11va hw decode, audio, decode thread —
  quality/perf upgrades that don't change any contract; see ROADMAP.
