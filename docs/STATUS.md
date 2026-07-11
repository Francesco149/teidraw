# STATUS — live front

*Updated: 2026-07-11 (session 4). History lives in `git log` — this file only
describes NOW.*

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
(context-menu toggle, persisted), flip-model low-latency swapchain, **LLM
export** (copy board/selection as PNG at 2× — offscreen render, clipboard PNG
+ CF_DIB — Ctrl+Shift+C or context menu; "copy as text" = reading-order
outline with texts verbatim, media placeholders, group nesting, arrows as a
connection list), **global settings** (%APPDATA%/teidraw/settings.json:
theme, zoom anim, undo limit + context-menu presets, boards dir, recent
boards — theme prefs moved OUT of board.json, adopted once from boards that
still carry them), **board picker** (modal via Ctrl+O / context menu, esc
closes it when a board is open; auto on bare launch with no last board:
recents with left-elided paths, new-board under Documents/teidraw, native
folder dialog; switch_board tears down all per-board caches). Bare `teidraw`
reopens the last board; window title shows the board name. Board format
**v2** (v1 tsize indices migrate at load); loads sanitize duplicate ids /
stale nextId / empty texts. Single TU, builds warning-free.

Session-5 polish + M3 move feel: arrows bound to a text stay pinned to their
world point while the text reflows (anchors re-normalized per edit frame);
a selected group drags from anywhere inside its bounding box (plain click
inside still deselects). **Video pill, third iteration (imgui-window
versions ripped out)**: the pill only exists while the video is the single
selected shape, is drawn BY HAND on the foreground drawlist and hit-tested
manually — no imgui window, so the canvas always owns the mouse. Dragging
anywhere on the pill (seek bar included) moves the video; a STILL CLICK
(<4px) acts the control under it on release — seek jumps to the clicked
position, play/stop/A/B/x likewise. CanvasFrame's mousedown claims pill
presses (overlay_contains → g_overlayDownCtl, downTarget = the video) and
its release skips selection semantics for them; DrawVideoOverlay applies
the action. Rotation is rigid vertex rotation about the video center with
inverse-rotated hit points — hitboxes sit on the drawn pixels by
construction. Fades 0.12s in / 0.15s out, instant-hide during real drags
(DM_PENDING keeps it). **Snap-move guides** (HOLD CTRL to snap — off by
default per user; moving bounds' edges+centers pull to other top-level
shapes' edges+centers, 8 screen px, accent guide lines through both
boxes), **shift = axis-lock drag**, **arrow-key nudge** (1 px, shift 10,
key-repeat, bursts coalesce into one undo via a 0.6s debounce). DM_MOVE
now applies an absolute offset from the drag origin (g_moveApplied)
instead of incremental deltas so lock/snap can't drift.

## Build & verify
```
nix develop --command make -C editor            # build/teidraw.exe
./build/teidraw.exe scratch                     # interactive on the Win host
./build/teidraw.exe scratch --shot build/s.png --frames 8   # headless verify
./build/teidraw.exe dir --export out.png        # board bounds → PNG, then exit
./build/teidraw.exe dir --export-txt out.txt    # text outline, then exit
```
`scratch/` is the user's live test board — do NOT script-edit it (and NEVER
write nextId from a script; the load-time sanitizer exists because that
minted duplicate ids once). Use a throwaway dir under the scratchpad for
render tests.

## NEXT TASK → M3 (docs/ROADMAP.md)
1. Text wrap width; then the custom WYSIWYG canvas editor (see below).
   (Snapping guides + nudge + shift axis-lock landed session 5.)

## Known rough edges / to keep in mind
- **WYSIWYG-transform editing is fully OFF** (`kWysiwygAlignEdit` and
  `kWysiwygRotEdit`, both false): ANY pre-frame pointer remap (align shift,
  rotation) makes InputText see jittering held-button coordinates → phantom
  drag-selections (user-confirmed rotation-only after the align revert).
  Rotated text edits axis-aligned as a PLACEHOLDER (snaps straight while
  typing, back on commit); the bold strike stays on. THE plan for "edit
  exactly as it appears": a custom stb_textedit-based canvas editor that owns
  its own hit-testing (so rotation/alignment can't desync imgui's mouse
  mapping) — scoped for AFTER the M3 LLM export. The draw-side vertex
  transforms are proven and kept behind the flags for that editor. Surveyed
  2026-07: NO off-the-shelf imgui rich-text editor exists (only code
  editors: ImGuiColorTextEdit / ImTextEdit) — in-house is the way.
- Editor click-chain: begin_text_edit resets io.MouseClickedTime so the two
  clicks that open an editor can't chain into double/triple-click selection
  on the first caret click (that was the OTHER phantom-selection source).
- Editors always open with the caret at the click (caret_index_from_click →
  ImGuiInputTextState public-head poke) and never inherit a previous edit's
  selection. If the caret ever lands off by a char, suspect that mapping.
- The editor transforms reach into the InputTextMultiline child window
  (`FindWindowByName("<win>/##t")` + per-quad vertex shifts + clip-rect
  patch) — re-verify if the imgui pin (1.92.4) ever moves.
- List lines ("• ", "N. ") always pin left even in centered/right text — a
  deliberate heuristic, shared by renderer + editor via is_list_line().
- Bare launch reopens recent[0] from settings.json; headless (`--shot`/
  `--export*`) never touches recents and still defaults to `./scratch`.
  `--picker` (dev flag) forces the picker open — used for headless UI shots.
  Settings tests can point the app at a fake `%APPDATA%` via
  `WSLENV=APPDATA/p APPDATA=<dir> ./build/teidraw.exe …`.
- Escape-while-crop-dragging commits instead of canceling.
- No audio on videos yet (M4). Faux bold is double-strike, not a real weight
  (real SemiBold statics would be the upgrade if it ever looks mushy).
