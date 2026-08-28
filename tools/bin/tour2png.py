#!/usr/bin/env python3
"""tour2png.py <dir-with-tourNN.raw> <out.png> [cols] [scale]

Assemble every Work:tourNN.raw the scripted camera tour left behind into one
contact sheet.

The tour dumps a frame at the end of each of its legs, so one run leaves a
dozen views from a dozen districts. Looking at them one at a time is slow and,
worse, biased: the corner-square gap survived for as long as it did because the
same two or three views kept being re-rendered. One sheet makes a repeated
artefact obvious at a glance.

    wsl python3 tools/bin/tour2png.py /mnt/c/temp/amiga_gta/work out/tour.png 4 1
"""
import glob
import os
import sys

from PIL import Image, ImageDraw

W, H = 320, 200


def load(path):
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) != 768 + W * H:
        return None
    img = Image.frombytes("P", (W, H), blob[768:])
    img.putpalette(blob[:768])
    return img.convert("RGB")


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    cols = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    scale = int(sys.argv[4]) if len(sys.argv) > 4 else 1

    paths = sorted(glob.glob(os.path.join(src, "tour*.raw")))
    frames = [(os.path.basename(p), load(p)) for p in paths]
    frames = [(n, im) for n, im in frames if im is not None]
    if not frames:
        print("no tour*.raw in %s" % src, file=sys.stderr)
        return 1

    rows = (len(frames) + cols - 1) // cols
    pad = 2
    sheet = Image.new("RGB", (cols * (W + pad) + pad, rows * (H + pad) + pad),
                      (255, 0, 255))
    draw = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(frames):
        x = pad + (i % cols) * (W + pad)
        y = pad + (i // cols) * (H + pad)
        sheet.paste(im, (x, y))
        draw.text((x + 3, y + 3), name.replace(".raw", ""), fill=(255, 255, 0))

    if scale > 1:
        sheet = sheet.resize((sheet.width * scale, sheet.height * scale),
                             Image.NEAREST)
    sheet.save(dst)
    print("%d frames -> %s (%dx%d)" % (len(frames), dst, sheet.width,
                                       sheet.height))
    return 0


if __name__ == "__main__":
    sys.exit(main())
