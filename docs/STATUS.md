# STATUS — live front

*Updated: 2026-07-11 (session 9). History lives in `git log` — this file only
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
quick second press on a side edge = auto-size); list items HANG: wrapped
continuations indent to the text column after the marker, numbered markers
right-align their dots to a shared single-digit column ("10." pokes out the
LEFT of the box, tldraw-style — text_left_overhang() widens render/cull/
export bounds for it, the BOX stays put), the marker fuses with its first
word and a too-wide word hard-cuts at the box edge (no ellipsis, never a
stranded "•"); rotation-safe via the crop trick (mouse in the snapshot's
local frame, new rect's center placed through the old frame). Corner resize
scales wrapW with the glyphs. draw_text_shape composes lines at the origin
before its vertex transform, so it draws under a wide-open clip rect —
AddText would otherwise cull negative-x glyphs at ADD time. **DrawTextEditor replaced imgui's InputTextMultiline entirely**:
caret/selection/undo live in `g_ted`, mouse maps into the shape's local frame
(rotation inverse, layout lines, nearest-boundary x), text mutations go
through `ed_mutate` (arrow anchors pinned to world points, rotated texts keep
their world top-left so glyphs don't swing per keystroke), auto-lists ported
verbatim + **numbered-list renumbering** (ed_renumber: inserting/removing an
item rewrites the contiguous run anchored at its first number; splitting a
list — Enter on an empty item or on a gap line — restarts the lower half at
the ORIGINAL list's first number, so 0-started lists split into 0-started
halves; all inside the same undo record), in-session ctrl+Z/Y with burst
coalescing, click/word/line double/triple-click drag selection, clipboard,
IME caret positioning via PlatformImeData. Headless editor testing: --edit
--caret --enter --bs drive scripted edits, board saves on exit for text
assertions. Editing rotated/aligned/wrapped text is now exactly
WYSIWYG — the kWysiwyg flags, pre-frame pointer remap and phantom-selection
saga are deleted. Also: **video play state persists** (`play` in board.json;
a video left playing resumes when the board reopens, transport clicks
autosave without undo entries; headless still renders posters).

Session-9 = **freehand draw tool** (D / toolbar "draw"; promoted out of the
roadmap parking lot on user request). `SH_DRAW` shapes are baked ink: point
centers + per-point pressure local to a pos/size/rot rect (same frame math as
images), stored as a flat quantized `pts` triple array in board.json —
selectable/movable/scalable/rotatable/deletable, never reshaped. Capture
streamlines the cursor (~25ms exp. smoothing) and simulates pressure from
speed (normalized by stroke width → fast = thin, the tldraw look); a real pen
overrides it via WM_POINTER (`g_penPressure`; no EnableMouseInPointer, so
imgui's mouse path is untouched — needs Windows Ink on in the tablet driver).
**Shift chains strokes**: shift+press with the draw tool appends a straight
rubber-band segment onto the LAST stroke (same shape, from its endpoint), and
toggling shift mid-drag flips straight/freehand within one gesture
(`g_drawSegBase` marks the live segment; straight mode rewrites its tail each
frame). One undo per gesture. Strokes use the S/M/L/XL ladder as width
(`kDrawSizes` 2.5/4.5/7/12 world px, style panel size row applies, align is
text-only), palette color + opacity work. Rendering (`draw_stroke_shape`):
pressure-radius rails around each point, filled as ONE hand-built mesh at
every opacity — strip triangles + cap fans + a single 1px outward AA fringe
via PrimReserve. One mesh is load-bearing twice (user-reported lessons):
overlapping-primitive fills STACK their AA fringes (a quads+discs-per-point
first cut multi-blended every edge pixel to ~full alpha = hard aliased edges
despite AA "on" — diagnosed by pixel-scanning exports for ramp values), and
translucent ink must fill exactly once or joints blotch. AddConcavePolyFilled
is also out: it ear-clips, and outlines self-intersect at curls tighter than
the pen radius → giant filled blobs. Simulated pressure runs on SCREEN-space
hand speed (zoom-independent) through an EXPONENTIAL curve (`exp(-speed/1400)`
→ careful ~0.8, easy ~0.5, brisk ~0.24), radius sweep 0.25–1.15x of nominal;
two earlier cuts read as "pressure does nothing" — one normalized by stroke
width in world px and saturated thin at real speeds, one ramped linearly to a
flick speed and parked every ordinary stroke in the top fifth of the range.
Tuning knobs live in draw_update (curve constant, `dt*30` adaptation) and
draw_radius (sweep).
Pen: stderr prints "pen pressure active" on first Windows Ink pointer event —
if it never prints while inking, enable Windows Ink in the tablet driver.
Hit-testing walks the ink polyline (max(radius, 6px) threshold, box
prefilter), not the rect.
`draw_recalc_bounds` re-hugs the rect to the ink after every append / width
change, keeping ink world-stationary under rotation via the crop/wrap
center-remap trick. Corner resize scales pts+scale in lockstep so the box
scales like an image. Text outline export says `- drawing (x, y) WxH`.
**NaN-poisoning postmortem (fixed same session)**: losing focus mid-stroke
(a `--shot` run stealing focus on the host, alt-tab) invalidates io.MousePos
to −FLT_MAX; one S2W of that is ±inf, and ONE bad point NaN-poisons the whole
stroke through draw_recalc_bounds' renormalization (inf − inf). nlohmann then
dumps NaN as **null**, and `j.value()` THROWS on present-but-null keys → the
board crashes on every subsequent load. Guards now: `draw_pt_ok` rejects
invalid pointer positions at capture (begin/update/end + commit-time sweep),
the stroke serializer never writes a non-finite number, and the stroke loader
reads tolerantly (bad points dropped, an all-bad stroke deletes itself via
the load sanitizer, zeroed w/h re-derives from ink). A board corrupted by the
pre-fix binary heals itself on open — the user's test2-remastered did (bad
stroke lost, all other shapes intact; `.bak-corrupt-20260711` copies of the
poisoned files left in the board dir).

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
(linux port) is polish on user feedback — the session-8 editor and the
session-9 draw tool both need hands-on testing (draw: stroke feel/thinning
constants in the `── freehand stroke capture ──` block, shift chaining, pen
pressure on the user's Wacom CTL-480 with Windows Ink on).

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
- RESOLVED (session 8, was the "renders twice / bolder" report): AddText
  truncates positions to whole px, so a FRACTIONAL per-line x (wrap widths
  are user-dragged floats → right/center align offsets go fractional) could
  put the faux-bold's two strikes in different truncation buckets — 2px
  apart instead of 1 = visibly doubled glyphs, zoom- and line-dependent.
  add_text_bold now snaps the origin and keeps the strike offset integral.
  Repro recipe that finally caught it: real board copy + `--shot` at the
  user's exact zoom (1.0), 600% magnified crops, A/B against a pre-refactor
  worktree build.
- Bare launch reopens recent[0] from settings.json; headless (`--shot`/
  `--export*`) never touches recents and still defaults to `./scratch`.
  `--picker` (dev flag) forces the picker open — used for headless UI shots.
  More dev flags: `--sel <id>` selects a shape (selection-UI shots),
  `--edit <id>` opens the text editor on it, `--bs <frame>` presses Backspace
  on that frame. CAVEAT: headless runs still open a real focused window on
  the host (stray keystrokes land in it, and it STEALS FOCUS — it cut a live
  editor's stroke mid-gesture once, see the session-9 NaN postmortem; warn
  the user before shooting while they have a session open) and SAVE the
  board on exit — never point repeated scripted runs at a board whose exact
  content matters.
  Settings tests can point the app at a fake `%APPDATA%` via
  `WSLENV=APPDATA/p APPDATA=<dir> ./build/teidraw.exe …`.
- Escape-while-crop-dragging commits instead of canceling.
- Faux bold is double-strike, not a real weight (real SemiBold statics would
  be the upgrade if it ever looks mushy).
