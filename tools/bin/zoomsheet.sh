#!/bin/sh
# Render one camera position at every zoom notch and put them on one sheet.
#
#   wsl -e sh /mnt/i/GITHUB/Amiga_GTA/tools/bin/zoomsheet.sh [bx] [by]
#
# The zoom notches live in native/gta_render.c. 32 pixels per block is the
# default and the only one where a street tile is a memcpy; the point of this
# sheet is to see what the others actually look like before deciding the range
# is right.
set -e

ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i

ROOT=/mnt/i/GITHUB/Amiga_GTA
cd "$ROOT"

MAP=${MAP:-/mnt/c/temp/amiga_gta/work/GTADATA/nyc.cmp}
TIL=${TIL:-build/data/style001.til}
BX=${1:-64}
BY=${2:-64}

mkdir -p out

for z in 16 20 24 28 32 40 48 64; do
    ./build/host/gtadump view "$MAP" "$TIL" "$BX" "$BY" 320 200 \
        "out/zoom$z.bmp" -1 "$z" >/dev/null
done

python3 - <<'PY'
from PIL import Image, ImageDraw

zs = [16, 20, 24, 28, 32, 40, 48, 64]
cols, W, H, pad = 4, 320, 200, 2
rows = (len(zs) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (W + pad) + pad, rows * (H + pad) + pad),
                  (255, 0, 255))
draw = ImageDraw.Draw(sheet)
for i, z in enumerate(zs):
    im = Image.open("out/zoom%d.bmp" % z).convert("RGB")
    x = pad + (i % cols) * (W + pad)
    y = pad + (i // cols) * (H + pad)
    sheet.paste(im, (x, y))
    draw.text((x + 3, y + 3), "%d px/block" % z, fill=(255, 255, 0))
sheet.save("out/zooms.png")
print("wrote out/zooms.png (%dx%d)" % sheet.size)
PY
