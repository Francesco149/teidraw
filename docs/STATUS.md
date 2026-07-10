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
Session 3: arrows stay **rigid under rotation** (trim now tests the target's
OBB in its local frame — the rotated AABB used to bulge and eat the line);
marquee selects on **partial overlap**; **auto lists** in the text editor
("- "/"* " → "• ", Enter continues bullets and increments "12. " numbering,
Enter on an empty item ends the list — bullet glyph verified in all 4 fonts);
**rotated text edits in place**: the pointer is inverse-rotated into the
editor's local space each frame (canvas input suspended meanwhile) and the
editor's draw output (glyphs/caret/selection) is rotated back + clip rects
opened, so the shape never snaps straight while editing.
Session 3b: backspace on an empty list item deletes the marker AND joins the
previous line (deletion detected via buffer-length tracking in the edit
callback); fixed the one-line-short editor box after Enter (CalcTextSize
drops a trailing empty line → transient scrollbar) + scrollbar hidden in the
overlay; the selection box turns rigidly during multi-selection rotation
(corners captured at gesture start) instead of re-fitting a bulging AABB;
**zoom commands** Shift+1 fit / Shift+2 selection / Shift+0 100%, all flying
the camera with a 180 ms ease-out (log-zoom lerp; any wheel/pan cancels).

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
- While editing a rotated text, clicks on OTHER ui (toolbar etc.) route with
  the remapped pointer — mostly they just commit the edit; harmless but noted.
- The rotated-editor draw-output rotation reaches into the InputTextMultiline
  child window (`FindWindowByName("<win>/##t")` + vertex transform + clip-rect
  patch) — re-verify if the imgui pin ever moves.
- Default board dir is `./scratch` relative to cwd — double-clicking the exe
  on Windows creates it next to the exe; fine until the board picker lands.
- Video overlay steals canvas input while visible (by design via imgui
  hover); check it doesn't fight the move-drag on small videos.
- Escape-while-crop-dragging commits instead of canceling.
- No audio on videos yet (M4).
- imgui deprecation horizon: dynamic-font API is 1.92-stable; if bumping the
  pin, re-check `PushFont(font, px)` + backend `RendererHasTextures`.
