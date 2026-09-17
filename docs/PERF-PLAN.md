# PERF-PLAN — the optimization frontier, and how to stand on it

*Written 2026-09-17 (session 13). Read this with `docs/STATUS.md` (what is true
now) and the session-13 entry there (what changed and why). This file is the
**next-session runway**: where the remaining cost is, what to measure first,
and which invariants must not regress.*

## 0. Deploy before you measure anything (and after you change anything)

`tools/deploy.sh` updates **both** system-wide installs — Linux
`/usr/local/bin/teidraw` (the `Mod+Shift+T` bind) and the Windows exe at
`<wslop>:/opt/src/teidraw/build/teidraw.exe` (the Start Menu shortcut runs it
straight out of the wslop checkout over `\\wsl.localhost`, so it must be built
*there*, from synced history). Run it after any change under `editor/` or
`assets/`, and commit first — only committed history travels to wslop:

```sh
tools/deploy.sh --check     # report drift, change nothing
tools/deploy.sh             # build + install + verify both
tools/deploy.sh --linux|--win
```

## 1. Measure first — the harness is in-repo

```sh
tools/mkboard.py /tmp/b --images 24 --cols 5 --cam-z 0.42            # images
tools/mkboard.py /tmp/b --videos 16 --cols 4 --cam-z 0.3 --play      # video
tools/mkboard.py /tmp/b --texts 300 --cam-z 1.0                      # text
python3 tools/mkboard.py /tmp/b --images 8 --videos 12 --texts 200 \
        --strokes 30 --arrows 10 --groups 6 --cols 5 --cam-z 0.22 --play  # everything

./build/teidraw /tmp/b --profile 900                 # live window, vsynced
./build/teidraw /tmp/b --profile 900 --novsync       # true cost (no vblank wait)
./build/teidraw /tmp/b --profile 900 --pan-t 3 --play # + movement + playback
SDL_VIDEODRIVER=offscreen ./build/teidraw /tmp/b --profile 900 --novsync  # no window at all
```

Reading the output: `frame` is wall time per frame; `draw`/`drain`/`render`/
`present` are the UI-thread halves; the `decode:` line is the video pool
(frames/s + requests/s + per-frame cost). The **only** trustworthy comparison
is same board + same flags + same machine, before vs after.

Windows (the exe is a Windows binary; pass **Windows paths**, and remember the
wslop shell is fish):

```sh
ssh wslop "cd /opt/src/teidraw/build && ./teidraw.exe 'C:\\path\\board' --profile 900 --play"
```

### Session-13 baselines to beat (Linux iGPU, 1600×1000; Windows on wslop)

| scenario | Linux (120 Hz) | Windows (60 Hz) |
|---|---|---|
| `indonesia` (196 texts, 13 images, 127 strokes) | 0.61 ms frame, 0.16 draw | — |
| 24-image board, panning | 0.94 ms frame | 0.09 ms work/frame |
| 300-text board @ z=1.0 | 0.64 ms frame | — |
| 24 playing videos | 8.44 ms = 1 vblank, 667 decodes/s | 1.25 ms work/frame, 694 decodes/s |
| everything-at-once, panning | 8.41 ms = 1 vblank | 0.61 ms work/frame |
| the manga-page LOD A/B | 31.6 → 112.9 (2:1 ref ~134) | 31.6 → 110.8 |

If a change makes any of these worse, the change is wrong until proven otherwise.

## 2. Where the remaining cost actually is (ranked, with the evidence)

### A. Windows video still uploads RGBA (the biggest known gap)
Linux uploads three R8 YUV planes (1.5 B/px) and converts in a shader; Windows
converts with swscale and uploads 4 B/px, so a 720p frame costs 3.7 MB instead
of 1.4 MB and one CPU pass more. Port the Linux path: a D3D11 vertex+pixel
shader pair for `ImDrawVert` (same attribute layout: Position/UV/Color at 0/1/2),
three SRV slots (R8 textures), matrix+offset as constants, bound through an
`AddCallback` exactly like `video_yuv_cb`. Keep `--shot` deterministic.
*Expected:* ~2.7× less upload traffic and no swscale on Windows; matters most
on boards with several players (`v24`-style). *Risk:* medium — new pipeline
code, plus a `--shot` A/B to confirm colours (compare against an ffmpeg-decode
reference like the Linux port did: 253,0,0 vs 251,0,0 was the pass mark).

### B. PNG decode cost (board fill time, not frame time)
`stbi_load` is ~65 ms for a 1080p PNG, on worker threads. It never hitches the
UI (async pool + low LOD first), but a 175-image board burns ~10 s of CPU
filling in. Options, cheapest first: (1) decode with **libavcodec**, which is
already linked and SIMD-optimized, and use `swscale` to build the mip levels in
one pass; (2) libspng/libpng (2-3× stb); (3) keep stb and accept it.
*Measure it first:* time-to-fully-loaded on `bainstorm` (175 images) — `--profile`
counts uploads, and the `drain` section shows the pacing cost.

### C. A RAM tier under the GPU byte budget
`kTexBudgetBytes` (768 MB, `TEIDRAW_TEX_BUDGET_MB`) evicts full-detail
textures; getting one back costs a **re-decode** (see B). A bounded RAM LRU of
decoded level-0 pixels (say 256 MB) turns re-promotion into an upload. Traded
memory for latency; only worth it on boards that actually exceed the budget.
*Instrument:* count re-requests (`vqReqs`-style counter for images) — if they
are rare in real use, skip this.

### D. Video: decode-ahead, and hardware decode
Today each video has at most one in-flight request, so a slow decode shows the
previous frame (smooth, but a stutter under load). Decode-ahead (2-3 frames
queued per active video, bounded by the same adaptive budget) buys headroom at
the cost of memory and latency (seek response). HW decode (VAAPI/D3D11VA) is
the real ceiling-raiser on laptops — big, invasive, and pointless on the
desktop box; treat as a separate project.

### E. Text/UI: measured, currently *not* worth optimizing
300 texts at z=1.0 cost 0.09 ms of `draw`; `indonesia` 0.16 ms. If a 5000-text
board ever shows up, the hooks are: cache `TextLayout` per shape (key: text,
family, pxW, wrapW, align) and cache the emitted glyph vertex run (key: layout
+ raster px), then transform-copy per frame. Do **not** pre-emptively add this.

### F. Draw-call batching: only if a board has hundreds of distinct textures
55-100 `ImDrawCmd`s per frame at ~2-3 µs each is ~0.2 ms. Batching would mean a
texture array/atlas + custom shader; the YUV callback machinery (A) is the
template. Not justified by any board we have.

### G. GL-side polish (small, safe wins)
- `glTexStorage2D` + `glTexSubImage2D` (immutable textures) for stills.
- PBO/`glMapBufferRange` uploads for the big level-0 uploads (removes the last
  1.6 ms/frame pacing stall on first load).
- `glGenerateMipmap` cost check: if it shows up in the profile, build mips on
  the worker (the `ImgRes` struct already carries a box-filtered low LOD, so
  the pass would extend to a full pyramid).

### H. Frame pacing / multi-monitor
Both backends are vblank-bound now (120 Hz Linux, 60 Hz Windows). Left over:
60→144 Hz behaviour, multi-monitor DPI (`WM_DPICHANGED` restyle, still open
from M3), and whether `SDL_GL_SetSwapInterval(1)` should switch to
`SDL_GL_SetSwapInterval(-1)` (mailbox) on compositors that honour it.

## 3. Invariants — break these and the work is wasted

- **The feel contract** (`CLAUDE.md`): FLIP_DISCARD swapchain + frame-latency
  waitable + `Present(1,0)`; input sampled *late* (after the vblank wait);
  scroll = zoom at cursor; no sleeps/busy waits.
- **`--shot`/`--export` stay deterministic**: they must keep using the
  synchronous image/video paths (`g_headless` / `!g_interactiveFrame`), so a
  shot renders the same frame whether or not a worker finished first. Two
  identical runs must produce byte-identical PNGs (check with `md5sum`).
- **Never touch the user's boards.** Measure on copies (`cp -a` to /tmp) or
  generated boards. Never write `nextId` from a script. Harness runs are
  marked by `g_devHarness` and no longer pollute `recent` boards.
- **Board format is v2 and stays that way** unless there is an explicit
  migration. Nothing in the perf work may change `board.json`'s schema.
- **Uploads stay time-budgeted** (`kUploadBudgetMs`, `kVideoUploadBudgetMs`);
  a "simplification" back to fixed counts per frame is a regression on any
  board with big assets.
- **Textures must never be dropped without a re-request path** — that bug
  (images stuck at the low LOD) is the reason this whole plan exists.

## 4. Open questions for whoever picks this up

1. Does the user's real work ever exceed the 768 MB GPU budget (i.e. is C
   worth doing)? Instrument a counter, then ask.
2. Is Windows video (A) actually visible in the user's boards, or is the 3.7 MB
   upload already free on a desktop dGPU? Measure on wslop first.
3. Is the 2:1 mip softness (box vs Lanczos) ever noticeable in *reading* (not
   in an A/B)? If yes, the lever is a sharper mip kernel on the worker, not a
   LOD bias (the bias trades shimmer while panning).
