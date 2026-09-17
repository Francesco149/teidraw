#!/usr/bin/env python3
"""Generate a synthetic teidraw board for profiling / perf regression runs.

Deterministic (seeded): same args -> byte-identical board. Images are written
by hand (pure stdlib zlib PNG) as procedural "manga pages" — panel frames,
screentone dots and text-line bars give real high-frequency content, so
minification quality and decode cost behave like the real boards. Videos come
from system ffmpeg (skipped with a warning when absent).

    tools/mkboard.py DIR --images 40 --texts 60 --videos 12 --strokes 20

Board layout: a vertical column of image/video "pages" (like the manga boards),
texts clustered beside them, strokes scattered. Board.json + assets/ only —
open it with `teidraw DIR`.
"""
import argparse, json, math, os, random, shutil, struct, subprocess, sys, zlib

# ── minimal PNG writer (RGBA8) ─────────────────────────────────────────────
def write_png(path, w, h, rows):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)

def manga_page(w, h, rng):
    """Procedural manga-ish page: white paper, black panel frames, screentone
    dots, text-line bars, a few diagonals — high frequency at every scale."""
    px = bytearray()
    white = (250, 248, 244, 255)
    ink = (18, 18, 22, 255)
    panel = 26
    # panels: 2x3 grid of frames
    pw, ph = (w - panel * 4) // 2, (h - panel * 6) // 3
    frames = []
    for gy in range(3):
        for gx in range(2):
            x0 = panel + gx * (pw + 2 * panel)
            y0 = panel + gy * (ph + 2 * panel)
            frames.append((x0, y0, x0 + pw, y0 + ph))
    frame_i = 0
    for y in range(h):
        for x in range(w):
            c = white
            for (x0, y0, x1, y1) in frames:
                if x0 <= x < x1 and y0 <= y < y1:
                    frame_i = frames.index((x0, y0, x1, y1))
                    # frame border
                    if x < x0 + 3 or x >= x1 - 3 or y < y0 + 3 or y >= y1 - 3:
                        c = ink
                    elif frame_i == 0:
                        # screentone: 2px dot grid
                        if (x % 6) < 2 and (y % 6) < 2:
                            c = (150, 150, 160, 255)
                    elif frame_i == 1:
                        # text lines: bars with random word gaps
                        ly = (y - y0) % 22
                        if 4 <= ly < 14:
                            wx = (x - x0) // 7
                            r = (x * 2654435761 + y * 40503 + wx * 97) & 0xFFFF
                            if r % 11 != 0:
                                c = ink
                    elif frame_i == 2:
                        # dense hatch lines
                        if (x + y) % 7 < 2:
                            c = ink
                    elif frame_i == 3:
                        # diagonal strokes
                        if abs(((x * 2 + y * 3) % 61) - 30) < 2:
                            c = ink
                    elif frame_i == 4:
                        # radial burst (speed lines)
                        dx, dy = x - (x0 + x1) // 2, y - (y0 + y1) // 2
                        a = int(math.atan2(dy, dx) * 40)
                        r2 = (a * 2654435761) & 0xFF
                        if r2 < 60 and (x + y) % 3:
                            c = ink
                    else:
                        if (x % 4) < 1 and (y % 5) < 1:
                            c = (90, 90, 100, 255)
                    break
            px += bytes(c)
    return [px[y * w * 4:(y + 1) * w * 4] for y in range(h)]

# ── board assembly ─────────────────────────────────────────────────────────
def stroke_pts(rng, x0, y0, x1, y1, n=14):
    pts = []
    for i in range(n):
        t = i / (n - 1)
        cx = x0 + (x1 - x0) * t + math.sin(t * 6.28) * 8 * rng.uniform(0.4, 1.0)
        cy = y0 + (y1 - y0) * t + math.cos(t * 6.28) * 8 * rng.uniform(0.4, 1.0)
        pts += [round(cx, 2), round(cy, 2), round(rng.uniform(0.25, 1.0), 2)]
    return pts

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--images", type=int, default=40)
    ap.add_argument("--texts", type=int, default=60)
    ap.add_argument("--videos", type=int, default=0)
    ap.add_argument("--strokes", type=int, default=0)
    ap.add_argument("--arrows", type=int, default=0)
    ap.add_argument("--groups", type=int, default=0)
    ap.add_argument("--img-size", default="1114x1600", help="WxH (manga pages are 1114x1600)")
    ap.add_argument("--cols", type=int, default=1, help="grid columns for media (1 = one vertical column)")
    ap.add_argument("--cam-x", type=float, default=0.0)
    ap.add_argument("--cam-y", type=float, default=0.0)
    ap.add_argument("--cam-z", type=float, default=0.55, help="saved camera zoom (fit a stress grid with ~0.35)")
    ap.add_argument("--play", action="store_true", help="videos start playing")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    iw, ih = (int(v) for v in args.img_size.lower().split("x"))
    d = args.dir
    assets = os.path.join(d, "assets")
    os.makedirs(assets, exist_ok=True)

    shapes, nid = [], 1
    def new_id():
        nonlocal nid
        i = nid
        nid += 1
        return i

    SCALE = 0.7   # world units per source px, like the real boards
    ww, wh = iw * SCALE, ih * SCALE
    cols = max(1, args.cols)
    col_gap = ww * 1.06
    row_gap = wh * 1.08
    per_col = cols
    video_assets = []

    # ── images ──
    for i in range(args.images):
        col, row = i % cols, i // cols
        name = f"assets/page-{i:03d}.png"
        path = os.path.join(d, name)
        if not os.path.exists(path):
            write_png(path, iw, ih, manga_page(iw, ih, rng))
        shapes.append({
            "id": new_id(), "type": 2, "asset": name,
            "x": round(col * col_gap, 3), "y": round(row * row_gap, 3),
            "w": round(ww, 3), "h": round(wh, 3),
        })

    # ── videos ──
    ffmpeg = shutil.which("ffmpeg")
    for i in range(args.videos):
        col, row = i % cols, i // cols
        name = f"assets/clip-{i:03d}.mp4"
        path = os.path.join(d, name)
        if not os.path.exists(path):
            if not ffmpeg:
                print("mkboard: ffmpeg missing, skipping videos", file=sys.stderr)
                break
            subprocess.run([
                ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                "-f", "lavfi", "-i", f"testsrc2=size=1280x720:rate=30:duration=6",
                "-f", "lavfi", "-i", "sine=frequency=440:duration=6",
                "-c:v", "libx264", "-preset", "veryfast", "-crf", "23",
                "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", path,
            ], check=True)
        vw = ww * 0.8
        sh = {"id": new_id(), "type": 2, "asset": name,
              "x": round(col * col_gap + ww * 0.1, 3), "y": round(row * row_gap, 3),
              "w": round(vw, 3), "h": round(vw * 9 / 16, 3)}
        if args.play:
            sh["play"] = True
        shapes.append(sh)

    # ── texts ──
    words = ("the quick brown fox jumps over a lazy dog while the canvas keeps "
             "rendering every glyph at zoom").split()
    for i in range(args.texts):
        col, row = i // max(1, args.texts // 4 + 1), i % max(1, args.texts // 4 + 1)
        txt = " ".join(rng.choice(words) for _ in range(rng.randint(4, 26)))
        if rng.random() < 0.25:
            txt += "\n" + " ".join(rng.choice(words) for _ in range(rng.randint(3, 12)))
        sh = {"id": new_id(), "type": 0, "text": txt,
              "family": rng.randint(0, 3), "tsize": rng.randint(0, 3),
              "scale": 1.0, "x": round(cols * col_gap + 60 + col * 420 + rng.uniform(-60, 60), 2),
              "y": round(row * 260 + rng.uniform(-80, 80), 2)}
        if rng.random() < 0.4:
            sh["wrap"] = round(rng.uniform(140, 320), 1)
        if rng.random() < 0.2:
            sh["algn"] = rng.randint(1, 2)
        shapes.append(sh)

    # ── strokes ──
    for i in range(args.strokes):
        x = rng.uniform(-ww, ww * 2.2); y = rng.uniform(0, max(1.0, args.images) * row_gap)
        s = {"id": new_id(), "type": 4, "x": round(x, 2), "y": round(y, 2)}
        s["w"] = round(ww * 0.35, 2); s["h"] = round(wh * 0.08, 2)
        s["tsize"] = rng.randint(0, 3); s["scale"] = 1.0
        lo = [round(rng.uniform(10, ww * 0.3), 2) for _ in range(2)]
        s["pts"] = stroke_pts(rng, lo[0], lo[1], lo[0] + s["w"], lo[1] + s["h"])
        shapes.append(s)

    # ── arrows ──
    imgs = [s for s in shapes if s["type"] == 2 and "page" in s.get("asset", "")]
    for i in range(args.arrows):
        if not imgs:
            break
        a = imgs[i % len(imgs)]; b = imgs[(i + 3) % len(imgs)]
        shapes.append({
            "id": new_id(), "type": 1,
            "a": {"x": round(a["x"] + a["w"], 2), "y": round(a["y"] + a["h"] / 2, 2),
                  "bind": a["id"], "ax": 1, "ay": 0.5},
            "b": {"x": round(b["x"], 2), "y": round(b["y"] + b["h"] / 2, 2),
                  "bind": b["id"], "ax": 0, "ay": 0.5},
            "bend": round(rng.uniform(-0.2, 0.2), 3),
            "label": f"link {i}" if i % 3 == 0 else "",
        })

    # ── groups ──
    for g in range(args.groups):
        members = shapes[g * 4:g * 4 + 3]
        if not members:
            continue
        gid = new_id()
        for m in members:
            m["parent"] = gid
        shapes.append({"id": gid, "type": 3})

    board = {"v": 2, "nextId": nid,
             "cam": {"x": args.cam_x, "y": args.cam_y, "z": args.cam_z}, "shapes": shapes}
    with open(os.path.join(d, "board.json"), "w") as f:
        json.dump(board, f)
    print(f"mkboard: {d}: {args.images} images, {args.videos} videos, "
          f"{args.texts} texts, {args.strokes} strokes, {args.arrows} arrows, "
          f"{args.groups} groups, {len(shapes)} shapes total")

if __name__ == "__main__":
    main()
