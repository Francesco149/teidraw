# teidraw — Claude entry point

MIT-licensed **infinite-canvas whiteboard** with tldraw's UX philosophy and feel:
one purpose, complexity hidden, heuristics that guess intent, and an obsessively
smooth/responsive canvas. Native **C++ · Dear ImGui 1.92**, two backends in one
TU behind `#ifdef _WIN32`: **D3D11 · Win32** (primary; cross-compiled to a Win64
PE with **mingw-w64 from the nix flake** — the proven slopstudio pattern — run
on the Win11 host via WSLInterop) and **SDL3 · SDL_Renderer** for Linux
(`make -C editor linux`; newer, less battle-tested). Primary primitives:
**text, arrows, images/gifs/videos, groups, freehand strokes** — deliberately
NOT every tldraw feature.

This file auto-loads every session. The repo is the source of truth.
**Read next: `docs/STATUS.md`** — current state + what to build next.

## The feel contract (don't regress these)
- **Swapchain (Windows):** DXGI FLIP_DISCARD, 3 buffers, frame-latency waitable
  object, max latency 1, `Present(1,0)`. Loop: wait on waitable → pump input →
  build → present. Lowest-latency vsync without tearing. Never add sleeps or
  busy loops. (Linux: vsynced `SDL_RenderPresent` blocks, input pumped right
  after — same sample-input-late order.)
- **Text:** imgui ≥1.92 dynamic font atlas (pinned from GitHub in the flake —
  nixpkgs 1.91 is too old). Canvas text rasterizes at `size × zoom` px, so it's
  crisp at every zoom; glyphs cap at `kMaxGlyphPx` then scale geometrically.
- **Scroll = zoom at cursor. Always.** (The one deliberate tldraw departure.)
  Pan = middle-drag / space+drag / right-DRAG (right-CLICK = context menu).
- Anything that isn't an immediate primitive lives in the right-click menu.

## Interaction heuristics implemented (the "guess what I want" list)
Click group-child → selects group; click again → drills to child (Esc pops).
Click text → selects; click the selected text again → edits; drag → moves.
Corner handles resize; the ring just OUTSIDE a corner rotates (shift = 15°
steps). Arrow end dropped over a shape → binds
with the exact anchor remembered, line auto-trimmed at the target's bounds.
Drag arrow middle → bend (snaps straight near chord). Double-click arrow →
label that blanks the line under it. Corner drag → aspect-locked scale;
ctrl+corner on image → crop with full-image ghost. Drop image onto image →
replace contents (cropped frames keep the frame, cover-crop the new source).
Paste = shapes > PNG > DIB > files > plain text (becomes a text shape).
Draw tool (D): stroke fast → thinner ink (real pen pressure wins when a
tablet reports it); shift+press chains a straight segment onto the LAST
stroke, and toggling shift mid-drag flips straight/freehand — one shape.
Strokes are baked ink: move/scale/rotate/delete, never reshape.

## Layout
- `editor/src/main.cpp` — the whole app, single TU (slopstudio pattern; split
  when it hurts). Sections are banner-commented; read the table of contents grep:
  `grep -n "────" editor/src/main.cpp`.
- `editor/Makefile` — both targets (default = Windows cross, `linux` = native
  SDL3). `flake.nix` — toolchain + pinned imgui + static cross libav + sdl3.
  `tools/embed.py` — bakes TTFs + icon into the binary. `tools/make-icon.sh`
  regenerates `assets/icon-256.png` + `assets/icon.ico` (linked into the PE
  via `editor/teidraw.rc`).
- `assets/fonts/` — ALL four fonts vendored (OFL, license files alongside):
  Shantell Sans (tldraw's handwriting font), Inter, JetBrains Mono, Lora.
- A **board** is a self-contained project dir: `board.json` + `assets/` (every
  imported file is COPIED in) + `undo.jsonl` (snapshot journal; undo history
  survives sessions, cap `g_undoLimit`). Per-user prefs (theme, undo limit,
  recent boards) live in `%APPDATA%/teidraw/settings.json` on Windows,
  `~/.config/teidraw/settings.json` on Linux; a bare launch reopens the last
  board, Ctrl+O = board picker.

## Build / run / verify
```
nix develop --command make -C editor          # → build/teidraw.exe (Windows, default)
nix develop --command make -C editor linux    # → build/teidraw (native SDL3)
./build/teidraw.exe [boardDir]                # opens on the Windows host (WSLInterop)
./build/teidraw.exe scratch --shot build/shot.png --frames 8   # headless screenshot
./build/teidraw.exe dir --export out.png       # render board bounds → PNG, exit
./build/teidraw.exe dir --export-txt out.txt   # reading-order text outline, exit
```
Verify visually with `--shot` + Read the PNG. `scratch/` is the gitignored test
board. `make -C editor shot` does the same (`shot-linux` for the SDL build —
truly headless via SDL's offscreen driver, no window/focus steal; prefer it
for render-only checks).

## Conventions
- Everything runs inside `nix develop` (`command not found` ⇒ you forgot it).
- Commit logical units as you go; build + `--shot` first. No `git add -A` on
  mixed trees. Push only when asked (no remote yet).
- Durable knowledge → `docs/`; update docs in the same change that makes them
  stale. `docs/STATUS.md` is the live front — keep it current.
- Co-author line on commits: `Co-Authored-By: Claude <model> <noreply@anthropic.com>`.

## Where to read next
- **Current state + next task (READ FIRST): `docs/STATUS.md`**
- Phase plan: `docs/ROADMAP.md`
- Design + rationale: `docs/ARCHITECTURE.md`
