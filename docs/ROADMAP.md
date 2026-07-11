# roadmap

Phases land as vertical slices; each ends buildable + `--shot`-verified.
M0–M2 are DONE (see STATUS.md for the live front).

## M0 — shell ✅
Flake toolchain (mingw cross, pinned imgui 1.92.4, static cross libav, fonts),
flip-model waitable swapchain, dark/light theme, adaptive dot grid,
scroll-zoom-at-cursor, pan, toolbar/zoom chrome, `--shot`.

## M1 — core whiteboard ✅
Text (4 families × 4 sizes + continuous scale), bezier arrows (bind/trim/
bend/labels), groups + drill selection, marquee, corner scale, context menu,
autosave, session-surviving snapshot undo, quick keybinds.

## M2 — media ✅
Images: drop/paste/copy-into-project, default sizing, replace-in-place,
ctrl+corner crop with ghost. Gif/video via libav: autoplay gifs, hover
play/stop/seek/A-B pill, dynamic-texture upload. Shape clipboard.

## M3 — daily-driver gaps (next)
- **LLM export** ✅: copy board/selection as PNG to clipboard (+ `--export
  out.png` CLI); reading-order text outline (context menu / `--export-txt`).
- **Global settings + board picker** ✅: %APPDATA%/teidraw/settings.json
  (theme, zoom anim, undo limit, boards dir, recent boards); picker overlay
  (Ctrl+O / auto on bare launch: recents, new-board, native folder dialog).
- Drag **snapping** to other shapes' edges/centers + distance guides
  (tldraw's alignment polish); arrow-key nudge; shift-drag axis lock.
- Text wrap width (drag side handles to set a wrap box).
- Multi-monitor DPI changes (WM_DPICHANGED restyle).


## M4 — polish & performance
- Viewport culling + text-extent caching (only matters at 1000s of shapes).
- Decode thread for video (keep UI thread pure); NVDEC/d3d11va option.
- Video audio (WASAPI + swresample; the flake libav already has swresample).
- More shapes (rect/ellipse/frame) IF daily use demands (rotation: done, session 2).
- freetype glyph rasterizer for even better small-text rendering.

## M5 — linux port
SDL3 (or Win32→X11/Wayland via GLFW) + Vulkan or GL backend behind the same
canvas code; mailbox present mode ≈ flip model. The canvas/doc layers are
platform-clean already; platform bits are isolated at the top of main.cpp.

## parking lot
Collaborative/multiplayer: out of scope. Freehand ink: not a primary
primitive, only if wanted later. Rich text: no.
