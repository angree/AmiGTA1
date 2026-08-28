#!/usr/bin/env python3
"""facingsheet.py <prefix> <out.png> [label ...]

Put the frames a walk script produced side by side, magnified around the
centre of the screen - which is where the camera keeps the player - with a
cross through the middle of each so "which way does he stick out" is a
question about the picture and not about the eye.

    wsl python3 tools/bin/facingsheet.py out/facing/f out/facing/compass.png \
        "leg0 S" "leg1 W" "leg2 N" "leg3 E" "leg4 S"
"""
import sys

from PIL import Image, ImageDraw

HALF = 26
SCALE = 12


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    prefix, dst = sys.argv[1], sys.argv[2]
    labels = sys.argv[3:]

    cells = []
    for n, label in enumerate(labels):
        img = Image.open(f"{prefix}{n:02d}.bmp").convert("RGB")
        w, h = img.size
        cx, cy = w // 2, h // 2
        crop = img.crop((cx - HALF, cy - HALF, cx + HALF, cy + HALF))
        cells.append((crop.resize((HALF * 2 * SCALE,) * 2, Image.NEAREST), label))

    pad, lab = 8, 16
    cw = HALF * 2 * SCALE
    out = Image.new("RGB", (len(cells) * (cw + pad) + pad, cw + lab + 2 * pad),
                    (32, 32, 32))
    draw = ImageDraw.Draw(out)
    for n, (cell, label) in enumerate(cells):
        x = pad + n * (cw + pad)
        out.paste(cell, (x, pad + lab))
        draw.text((x + 2, pad + 2), label, fill=(255, 255, 0))
        mid, midy = x + cw // 2, pad + lab + cw // 2
        draw.line((mid, pad + lab, mid, pad + lab + cw), fill=(255, 0, 255))
        draw.line((x, midy, x + cw, midy), fill=(255, 0, 255))
    out.save(dst)
    print(f"{prefix}*.bmp -> {dst} ({out.width}x{out.height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
