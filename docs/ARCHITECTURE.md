# teidraw architecture

Goal: tldraw's polish in a native app. Every decision below serves either
**latency**, **crispness**, or **the guess-what-I-want UX**.

## Stack
- **C++17, single translation unit** (`editor/src/main.cpp`), Dear ImGui
  drawlists for ALL canvas rendering, with two platform backends behind
  `#ifdef _WIN32` seams:
  - **Windows (primary)**: D3D11 + Win32, cross-compiled Win64 PE via
    mingw-w64 from the nix flake; runs on the Windows host through
    WSLInterop (no WSLg compositor tax).
  - **Linux**: SDL3 + SDL_Renderer (SDL picks GL/Vulkan). SDL3 covers
    every platform service in one dependency: window/renderer, mime-typed
    clipboard (the PNG/shape payloads work on X11 AND Wayland), file drops,
    pen pressure, audio streams, the folder dialog (portal-backed, async —
    the board picker polls the callback's result), display scale. Headless
    runs prefer SDL's `offscreen` video driver: `--shot`/`--export` open no
    window and steal no focus.
- **imgui pinned ≥1.92** (flake `fetchFromGitHub`): the dynamic font atlas
  (`RendererHasTextures`) rasterizes glyphs on demand at any pixel size —
  the load-bearing feature for crisp text at arbitrary zoom (both the DX11
  and SDLRenderer3 backends support it).
- **libav** for gif+video decode in-process (static cross build on Windows;
  pkg-config on Linux — static in release CI); stb_image for stills;
  stb_image_write for `--shot`/exports. nlohmann for board JSON.

## Presentation / latency
Flip-model swapchain: `FLIP_DISCARD`, 3 buffers, `FRAME_LATENCY_WAITABLE_OBJECT`,
`SetMaximumFrameLatency(1)`, `Present(1,0)`, `DXGI_SCALING_NONE`,
`MakeWindowAssociation(NO_ALT_ENTER)`. Frame order: **wait on the waitable →
pump Win32 messages → build imgui frame → render → present**. Waiting *before*
input sampling is the whole trick: input is at most one refresh old, the GPU
queue never grows, vsync prevents tearing. `--shot` presents with vsync off.

Linux: vsynced `SDL_RenderPresent` blocks until the frame is consumed and
events are pumped right after it returns — the same "sample input as late as
possible" order, one seam lower. (A frame-latency waitable has no SDL
equivalent; if latency ever disagrees with feel here, a GL/Vulkan backend
with explicit fencing is the upgrade path.)

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
Stills: stb → immutable texture (`TexH`: D3D11 SRV / SDL_Texture), cached per
asset path (failures cached too). Gif/video: resident `VideoDecoder` per
asset (LRU cap 6) — avformat seek + avcodec decode + swscale→RGBA,
forward-decode reuse for sequential playback — into one dynamic/streaming
texture per playing shape, uploaded only when the wanted frame index changes.
Gifs autoplay-loop chrome-free; videos get the hover pill
(play/pause/stop/seek/A-B/sound). Audio is per-shape opt-in (speaker toggle,
persisted): a playing sounding video owns an `AudioOut` thread — its OWN
avformat context (video decoder seeks stay untouched) → swresample to the
device format → shared-mode WASAPI on Windows / an SDL3 audio stream on
Linux (both OS-mix, no in-app mixer). While live, the audio device clock
DRIVES `ps.t`, so A/V can't drift; UI seeks and A-B wraps request an audio
re-seek and the video free-runs on DeltaTime until it lands. A video with
`loopA` set opens (and stops back) at A.

## Input / interaction
One explicit drag state machine (`DragMode`): PENDING → MOVE/MARQUEE at a 4 px
threshold; HANDLE (aspect-locked scale about the opposite corner — text scales
a continuous font multiplier, and its wrap box with it); CROP (ctrl+corner,
display-rect corner clamped inside the fixed full-image projection, ghost at
30 % alpha); WRAP (side handles on a single text set `wrapW`, rotation-safe
via the crop trick; quick second press = auto-size); ARROW_A/B (rebind on
release position), BEND, NEW_ARROW (tool auto-returns to select). Selection
resolves clicks through the group chain (`resolve_target`), with a one-group
drill level (`g_drill`).

## Text layout + editing
`layout_text()` is the single line-breaking engine (soft wrap, per-line
alignment, list pinning); rendering, extents, caret mapping and the editor
all read the same `TextLayout`, so editing cannot desync from the committed
look. The editor (`DrawTextEditor` + `g_ted`) is in-house — no imgui widget:
it draws through `draw_text_shape`, hit-tests the mouse in the shape's local
(inverse-rotated) frame, owns caret/selection/blink/in-session-undo, ports
the auto-list heuristics, pins bound arrow ends across reflows and keeps a
rotated text's world top-left fixed while its extent changes. Escape and
click-outside both commit; a whitespace-only commit deletes the shape.

## Fonts & icon
Embedded into the binary by `tools/embed.py` (single-file exe): Shantell Sans
(handwriting default — what tldraw uses), Inter (sans), JetBrains Mono, Lora
(serif) — all four vendored in `assets/fonts/` (OFL, license files alongside).
Four canvas sizes S/M/L/XL = 20/28/40/56 world px, default L (big text
preferred), times a continuous resize scale. The icon (`tools/make-icon.sh`)
ships as an `.ico` PE resource on Windows and an embedded PNG →
`SDL_SetWindowIcon` on Linux.

## Rejected / deferred
- nixpkgs imgui 1.91 (no dynamic fonts) — pinned 1.92.4 instead.
- Legacy DISCARD swapchain (slopstudio) — flip model is strictly better here.
- Per-op undo deltas — snapshots are simpler and RAM is cheap (user's call).
- freetype rasterizer, NVDEC/d3d11va hw decode, audio, decode thread —
  quality/perf upgrades that don't change any contract; see ROADMAP.
