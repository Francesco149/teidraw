# teidraw — Claude entry point

MIT-licensed **infinite-canvas whiteboard** with tldraw's UX philosophy and feel:
one purpose, complexity hidden, heuristics that guess intent, and an obsessively
smooth/responsive canvas. Native **C++ · Dear ImGui 1.92 · D3D11 · Win32**,
cross-compiled to a Win64 PE with **mingw-w64 from the nix flake** (the proven
slopstudio pattern), run on the Win11 host via WSLInterop. Windows first; Linux
port later. Primary primitives: **text, arrows, images/gifs/videos, groups** —
deliberately NOT every tldraw feature.

This file auto-loads every session. The repo is the source of truth.
**Read next: `docs/STATUS.md`** — current state + what to build next.

## The feel contract (don't regress these)
- **Swapchain:** DXGI FLIP_DISCARD, 3 buffers, frame-latency waitable object,
  max latency 1, `Present(1,0)`. Loop: wait on waitable → pump input → build →
  present. Lowest-latency vsync without tearing. Never add sleeps or busy loops.
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

## Layout
- `editor/src/main.cpp` — the whole app, single TU (slopstudio pattern; split
  when it hurts). Sections are banner-commented; read the table of contents grep:
  `grep -n "────" editor/src/main.cpp`.
- `editor/Makefile` — cross-build. `flake.nix` — toolchain + pinned imgui +
  static cross libav + fonts. `tools/embed.py` — bakes TTFs into the PE.
- `assets/fonts/` — vendored Shantell Sans (OFL; tldraw's handwriting font).
  Inter/JetBrains Mono/Lora come from nixpkgs via the flake.
- A **board** is a self-contained project dir: `board.json` + `assets/` (every
  imported file is COPIED in) + `undo.jsonl` (snapshot journal; undo history
  survives sessions, cap `g_undoLimit`). Per-user prefs (theme, undo limit,
  recent boards) live in `%APPDATA%/teidraw/settings.json`; a bare launch
  reopens the last board, Ctrl+O = board picker.

## Build / run / verify
```
nix develop --command make -C editor          # → build/teidraw.exe
./build/teidraw.exe [boardDir]                # opens on the Windows host (WSLInterop)
./build/teidraw.exe scratch --shot build/shot.png --frames 8   # headless screenshot
./build/teidraw.exe dir --export out.png       # render board bounds → PNG, exit
./build/teidraw.exe dir --export-txt out.txt   # reading-order text outline, exit
```
Verify visually with `--shot` + Read the PNG. `scratch/` is the gitignored test
board. `make -C editor shot` does the same.

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
