# STATUS — live front

*Updated: 2026-07-10 (session 2)*

## Where things stand
M0+M1+M2 landed and `--shot`-verified: the app is a working whiteboard —
text/arrows/images/gifs/videos/groups, smart selection, crop, clipboard,
autosave + cross-session undo, dark/light, flip-model low-latency swapchain.
Single TU, builds clean (no warnings) with the flake toolchain.
Session 2 (after first hands-on pass, "UX is solid"): fixed text-tool clicks
instantly committing the empty editor (focus-frame race); click semantics are
now select-first / edit-on-second-click; added **rotation** (drag the ring
just outside a corner handle, shift = 15° steps) across render/bounds/hit/
gizmos/crop/arrow-anchors, groups rotate about the selection center.

## Build & verify
```
nix develop --command make -C editor            # build/teidraw.exe
./build/teidraw.exe scratch                     # interactive on the Win host
./build/teidraw.exe scratch --shot build/s.png --frames 8   # headless verify
```
`scratch/` currently holds a demo board exercising every shape type
(text families/sizes, bound+curved+labeled arrows, group, image w/ crop,
mp4, gif).

## NEXT TASK → M3 (docs/ROADMAP.md)
Suggested order:
1. **LLM export** — copy-as-PNG (clipboard CF_DIB/PNG of board or selection,
   plus `--export out.png` CLI rendering the board bounds offscreen) and a
   text outline dump (`--export-txt` / context menu "copy as text").
2. Board picker + global settings (%APPDATA%), undo-limit option.
3. Snapping guides + nudge keys + shift axis-lock.

## Known rough edges / to keep in mind
- Editing a ROTATED text draws the edit overlay axis-aligned (the shape snaps
  visually straight while editing, back to rotated on commit).
- Arrow trim on rotated targets uses the AABB, slightly loose on fat angles.
- Default board dir is `./scratch` relative to cwd — double-clicking the exe
  on Windows creates it next to the exe; fine until the board picker lands.
- Video overlay steals canvas input while visible (by design via imgui
  hover); check it doesn't fight the move-drag on small videos.
- Escape-while-crop-dragging commits instead of canceling.
- No audio on videos yet (M4).
- imgui deprecation horizon: dynamic-font API is 1.92-stable; if bumping the
  pin, re-check `PushFont(font, px)` + backend `RendererHasTextures`.
