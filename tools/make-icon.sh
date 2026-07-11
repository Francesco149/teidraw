#!/usr/bin/env bash
# Generate the teidraw icon: "TEI" (Shantell Sans) in the upper half, a
# 3-squares-high checkerboard filling the lower half. Outputs:
#   assets/icon-256.png  (embedded into the binary; SDL window icon, README)
#   assets/icon.ico      (multi-size, linked into the PE via editor/teidraw.rc)
# Run inside `nix develop` (imagemagick) from the repo root.
set -euo pipefail
cd "$(dirname "$0")/.."

BG='#101011'      # dark-theme canvasBg
INK='#f0f0f0'     # dark-theme textMain
FONT=assets/fonts/ShantellSans.ttf
S=80              # checker square at working scale; lower half = 3 rows of S
W=$((6 * S))      # 6 columns wide
H=$((6 * S))      # square icon; upper half = 3S for the wordmark

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# 2x2 checker tile -> 6x3 board for the lower half
magick -size $((2 * S))x$((2 * S)) xc:"$BG" -fill "$INK" \
  -draw "rectangle 0,0 $((S - 1)),$((S - 1))" \
  -draw "rectangle $S,$S $((2 * S - 1)),$((2 * S - 1))" "$tmp/tile.png"
magick -size ${W}x$((3 * S)) tile:"$tmp/tile.png" "$tmp/board.png"

# wordmark centered in the upper half
magick -size ${W}x${H} xc:"$BG" \
  \( +size -background none -fill "$INK" -font "$FONT" -pointsize 112 label:TEI \) \
  -gravity north -geometry +4+80 -composite \
  "$tmp/board.png" -gravity south -geometry +0+0 -composite \
  "$tmp/icon.png"

magick "$tmp/icon.png" -resize 256x256 assets/icon-256.png
magick "$tmp/icon.png" -define icon:auto-resize=256,128,64,48,32,16 assets/icon.ico
echo "wrote assets/icon-256.png assets/icon.ico"
