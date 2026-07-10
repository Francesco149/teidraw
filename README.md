# teidraw

MIT-licensed infinite-canvas whiteboard with a tldraw-style UX: one purpose,
done smoothly. Native Dear ImGui + D3D11, cross-compiled from NixOS to a
single self-contained Windows exe (Linux port planned).

- **Feel first**: flip-model waitable swapchain (1-frame latency, vsynced,
  no tearing), text crisp at every zoom via imgui's dynamic font atlas.
- **Scroll zooms** (at the cursor). Middle/space/right-drag pans. Right-click
  for everything that isn't a basic primitive.
- **Primitives**: text (handwriting/sans/mono/serif × 4 sizes, big default),
  arrows (bind to shapes, remember the exact anchor, auto-trim at bounds,
  1-point curve, labels), images / gifs / videos (drop or paste; videos get a
  hover play/stop/seek/A-B-loop pill), groups with drill-in selection.
- **Boards are folders**: `board.json` + copied `assets/` + `undo.jsonl` —
  self-contained, portable, autosaved, with undo history that survives
  restarts.

## Build (NixOS / WSL2)

```sh
nix develop --command make -C editor    # → build/teidraw.exe (Win64 PE)
./build/teidraw.exe myboard             # WSLInterop runs it on the Windows host
```

## Keys

`V` select · `H` hand · `T` text · `A` arrow · `Del` delete ·
`Ctrl+Z/Y` undo/redo · `Ctrl+C/X/V` clipboard · `Ctrl+D` duplicate ·
`Ctrl+G` group · `Ctrl+Shift+G` ungroup · `[` `]` z-order ·
`Ctrl+A` select all · `Ctrl+Shift+D` theme · `Esc` pop/deselect ·
`Shift+1` zoom to fit · `Shift+2` zoom to selection · `Shift+0` 100%

## License

MIT. Bundled fonts (Shantell Sans, Inter, JetBrains Mono, Lora) are SIL OFL —
see `assets/fonts/OFL-ShantellSans.txt`.
