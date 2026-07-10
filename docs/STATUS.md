# STATUS — live front

*Updated: 2026-07-10 (end of session 3). History lives in `git log` — this
file only describes NOW.*

## Where things stand
The app is a daily-drivable whiteboard, hands-on tested by the user ("UX is
solid", "feels good"): text / arrows / images / gifs / videos / groups, smart
selection (group drill, select-then-edit text), rotation everywhere (ring
outside the corner handles, shift = 15° steps; groups store a persistent
frame; rotated text edits in place via pointer remap + draw-output rotation),
crop with ghost, marquee on partial overlap, auto bullet/numbered lists with
backspace-join, clipboard in/out, autosave + cross-session undo, dark/light,
**style panel** (12 theme-aware palette indices · S/M/L/XL = 24/40/56/80 with
M default · text align L/C/R · opacity 5–100%), zoom commands (Shift+1 fit /
Shift+2 selection / Shift+0 100%) flying an ease-in-out-quart 0.28s camera
(context-menu toggle, persisted), flip-model low-latency swapchain. Board
format **v2** (v1 tsize indices migrate at load); loads sanitize duplicate
ids / stale nextId / empty texts. Single TU, builds warning-free.

## Build & verify
```
nix develop --command make -C editor            # build/teidraw.exe
./build/teidraw.exe scratch                     # interactive on the Win host
./build/teidraw.exe scratch --shot build/s.png --frames 8   # headless verify
```
`scratch/` is the user's live test board — do NOT script-edit it (and NEVER
write nextId from a script; the load-time sanitizer exists because that
minted duplicate ids once). Use a throwaway dir under the scratchpad for
render tests.

## NEXT TASK → M3 (docs/ROADMAP.md)
1. **LLM export** — copy-as-PNG (clipboard, board or selection; plus
   `--export out.png` CLI rendering board bounds offscreen) and a text
   outline dump (`--export-txt` / context menu "copy as text").
2. Board picker + global settings file (%APPDATA%), undo-limit option.
3. Snapping guides + arrow-key nudge + shift axis-lock drag.

## Known rough edges / to keep in mind
- While editing a remapped text (rotated OR center/right-aligned multiline),
  clicks on other UI route with the remapped pointer — they mostly just
  commit the edit; harmless but noted.
- Editing WYSIWYG: bold strike + rotation are ON; **per-line align-while-
  editing is OFF** (`kWysiwygAlignEdit=false`) — its pointer remap caused
  phantom drag-selections (user report 2026-07-11). STRETCH GOAL, revisit
  AFTER the M3 LLM export: likely needs the remap applied inside the widget's
  hit-testing rather than globally pre-frame, or the custom stb_textedit
  editor. Surveyed 2026-07: NO off-the-shelf imgui rich-text editor exists
  (only code editors: ImGuiColorTextEdit / ImTextEdit) — in-house is the way.
- Editors always open with the caret at the click (caret_index_from_click →
  ImGuiInputTextState public-head poke) and never inherit a previous edit's
  selection. If the caret ever lands off by a char, suspect that mapping.
- The editor transforms reach into the InputTextMultiline child window
  (`FindWindowByName("<win>/##t")` + per-quad vertex shifts + clip-rect
  patch) — re-verify if the imgui pin (1.92.4) ever moves.
- List lines ("• ", "N. ") always pin left even in centered/right text — a
  deliberate heuristic, shared by renderer + editor via is_list_line().
- Default board dir is `./scratch` relative to cwd; fine until the picker.
- Escape-while-crop-dragging commits instead of canceling.
- No audio on videos yet (M4). Faux bold is double-strike, not a real weight
  (real SemiBold statics would be the upgrade if it ever looks mushy).
