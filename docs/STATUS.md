# STATUS — live front

*Updated: 2026-07-11 (session 8). History lives in `git log` — this file only
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
inside still deselects). **Video controls, final shape (imgui-window
versions ripped out)**: a selected video shows a MINI pill (play/stop) in
its bottom-right corner while the pointer is over the video; hovering the
mini pill escalates to the FULL pill (seek, time, A-B loop) centered on
the video, which de-escalates 0.5s after the pointer leaves it. Both are
drawn BY HAND on the foreground drawlist and hit-tested manually — no
imgui window, so the canvas always owns the mouse. Dragging anywhere on a
pill (seek bar included) moves the video, pills fading out smoothly with
the drag; a STILL CLICK (<4px) acts the control under it on release —
seek jumps to the clicked position, play/stop/A/B/x likewise. The mini
AREA stays click-responsive while the full pill is up (user just saw
play/stop there; overlapping full controls win). CanvasFrame's mousedown
claims pill presses (overlay_contains → g_overlayDownCtl, downTarget =
the video) and its release skips selection semantics for them;
DrawVideoOverlay applies the action. Rotation is rigid vertex rotation
about the video center with inverse-rotated hit points — hitboxes sit on
the drawn pixels by construction. **Snap-move guides** (HOLD CTRL to snap — off by
default per user; moving bounds' edges+centers pull to other top-level
shapes' edges+centers, 8 screen px, accent guide lines through both
boxes), **shift = axis-lock drag**, **arrow-key nudge** (1 px, shift 10,
key-repeat, bursts coalesce into one undo via a 0.6s debounce). DM_MOVE
now applies an absolute offset from the drag origin (g_moveApplied)
instead of incremental deltas so lock/snap can't drift.

Session-6 = the M4 perf pass. **Big boards**: draw_doc_shapes culls shapes
outside the padded view rect (bent arrows include the bezier control point),
text_extent caches CalcTextSize per shape id (validated on family/px/text),
find_shape memoizes id→index (validated per lookup, never stale), marquee
dedupes via a hash set — a 3000-shape stress board went ~6ms → ~0.03ms per
frame in a working view, `--shot` byte-identical before/after. **Video decode
thread**: a single worker owns every libav decode behind per-decoder mutexes
(lock order g_decMx → decoder.mx); the UI thread still opens decoders
(imports need w/h synchronously) and decodes each shape's FIRST frame
in-line (poster shows instantly, --shot stays deterministic), then every
frame change is a latest-wins request (g_vqWant, keyed by shape) drained
once per frame before the draw pass (drain_video_results). Seeks/A-B wraps —
the old UI hitches — now happen off-thread; a superseded scrub result still
uploads for progressive feedback. switch_board bumps g_vqGen so in-flight
results from the old board can't land on the new one. Side effect of
culling: offscreen gifs/videos stop decoding (playback time also pauses);
they resume when scrolled back in — deliberate.

Session-7 = **video audio** (the last big M4 item). Per-shape opt-in: a
speaker toggle on the full pill (shown only when the file has an audio
stream), OFF by default, persisted in board.json (`sound`) and reset when the
video is replaced. A playing sounding video owns an `AudioOut` thread: its
OWN AVFormatContext (audio seeks never disturb the video decoder), decode →
swresample to the device mix format → shared-mode WASAPI render (Windows
mixes, so overlapping videos need no in-app mixer). While the stream is live
its hardware clock DRIVES `ps.t` (audio master — A/V can't drift); UI seeks /
stop / A-B wraps request an audio re-seek (`ps.audioSeek` / wrap path) and
the video free-runs on DeltaTime until `pending` drains back to 0. Streams
pause when the video pauses or culls offscreen (`lastTick` sweep in
`audio_sweep`), die on sound-off/delete/replace/board-switch/exit. Fixed
alongside: a video with an A-B loop now OPENS at A (poster + first play), and
stop returns to A instead of 0 when a loop start is set (only-A-set videos
also wrap to A at end of file).

Session-8 = **text wrap + the in-house WYSIWYG editor** (the M3 finale).
`layout_text()` is now THE text engine: line breaking (soft wrap via
`ImFont::CalcWordWrapPosition`, blanks eaten at wrap points), per-line
alignment, list pinning (decided per HARD line so a wrapped bullet's
continuations stay pinned) — rendering, `text_extent` (cache keyed on wrap
too), caret mapping and the editor all read the same `TextLayout`, so breaks
cannot diverge between draw and hit-testing. `Shape.wrapW` (world units,
`wrap` in board.json, 0 = auto-size): on a single selected text the whole
LEFT/RIGHT edge drags the wrap box and the whole TOP/BOTTOM edge scales
(square midpoint gizmos mark all four; corners + rotate ring keep priority;
quick second press on a side edge = auto-size); a list line's marker and
first word wrap as ONE unit (no stranded "•"); rotation-safe via the crop
trick (mouse in the snapshot's local frame, new rect's center placed through
the old frame). Corner resize scales wrapW with the glyphs. **DrawTextEditor replaced imgui's InputTextMultiline entirely**:
caret/selection/undo live in `g_ted`, mouse maps into the shape's local frame
(rotation inverse, layout lines, nearest-boundary x), text mutations go
through `ed_mutate` (arrow anchors pinned to world points, rotated texts keep
their world top-left so glyphs don't swing per keystroke), auto-lists ported
verbatim, in-session ctrl+Z/Y with burst coalescing, click/word/line
double/triple-click drag selection, clipboard, IME caret positioning via
PlatformImeData. Editing rotated/aligned/wrapped text is now exactly
WYSIWYG — the kWysiwyg flags, pre-frame pointer remap and phantom-selection
saga are deleted. Also: **video play state persists** (`play` in board.json;
a video left playing resumes when the board reopens, transport clicks
autosave without undo entries; headless still renders posters).

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

## NEXT TASK (docs/ROADMAP.md)
M3 is DONE (wrap + WYSIWYG editor landed session 8) except multi-monitor DPI
(WM_DPICHANGED restyle). M4 is essentially done too. What's left before M5
(linux port) is polish on user feedback — the new editor needs hands-on
testing (typing feel, selection, IME, auto-lists, wrapped/rotated editing).

## Known rough edges / to keep in mind
- **The text editor is in-house** (`DrawTextEditor` + `g_ted`): no imgui
  widget, no io.WantTextInput until a frame after open (CanvasFrame gates on
  `editing` too). It draws through draw_text_shape and hit-tests via
  layout_text — if editing ever disagrees with committed rendering, the bug
  is NOT a desync, both read the same layout.
- Soft-wrap caret affinity: a caret exactly on a mid-word cut boundary (word
  wider than the box) displays at the END of the upper line; Home on the
  lower line shows it there too. Only reachable when a single word exceeds
  the wrap box — punted (needs an affinity bit).
- List lines ("• ", "N. ") always pin left even in centered/right text — a
  deliberate heuristic; wrapped continuations of a list item inherit the pin
  (decided per hard line in layout_text).
- OPEN (user report, session 8): while typing, a line can look "rendered
  twice with a slight offset / bolder" on the live canvas. NOT reproduced
  headless: committed renders are byte-stable, per-frame layout logs are
  stable post-edit, single-frame --edit/--bs shots look clean. Waiting on a
  live screenshot + zoom level; suspect list: something temporal (frame
  alternation) or dynamic-font-atlas UV churn under real session pressure.
- Bare launch reopens recent[0] from settings.json; headless (`--shot`/
  `--export*`) never touches recents and still defaults to `./scratch`.
  `--picker` (dev flag) forces the picker open — used for headless UI shots.
  More dev flags: `--sel <id>` selects a shape (selection-UI shots),
  `--edit <id>` opens the text editor on it, `--bs <frame>` presses Backspace
  on that frame. CAVEAT: headless runs still open a real focused window on
  the host (stray keystrokes land in it) and SAVE the board on exit — never
  point repeated scripted runs at a board whose exact content matters.
  Settings tests can point the app at a fake `%APPDATA%` via
  `WSLENV=APPDATA/p APPDATA=<dir> ./build/teidraw.exe …`.
- Escape-while-crop-dragging commits instead of canceling.
- Faux bold is double-strike, not a real weight (real SemiBold statics would
  be the upgrade if it ever looks mushy).
