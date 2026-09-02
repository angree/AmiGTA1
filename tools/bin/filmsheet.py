#!/usr/bin/env python3
"""filmsheet.py <out.png> <live00.raw> [live01.raw ...] [--crop W H] [--scale N]
                [--every K] [--w 320] [--h 200]

A contact sheet from the filmstrip the `film <n>` autodrive order writes
(gta_main.c: live00.raw, live01.raw, ... one a tick). Each frame is cropped
around the centre of the screen - the camera keeps the player there - scaled
up, and labelled with its tick number, so a sequence that happens over forty
ticks can be looked at side by side instead of one frame at a time.

The raw format is the one raw2png.py reads: 768 bytes of RGB palette followed
by width*height palette indices, no header.

    python tools/bin/filmsheet.py out/exit_sheet2.png C:/temp/amiga_gta/work/live*.raw \\
        --crop 96 80 --scale 3 --every 2
"""
import glob
import os
import sys

from PIL import Image, ImageDraw


def load_raw(path, w, h):
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) != 768 + w * h:
        raise SystemExit("%s is %d bytes, expected %d for %dx%d"
                         % (path, len(blob), 768 + w * h, w, h))
    pal = list(blob[:768])
    img = Image.frombytes("P", (w, h), blob[768:])
    img.putpalette(pal)
    return img.convert("RGB")


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 2
    crop_w, crop_h, scale, every, w, h = 96, 80, 3, 1, 320, 200
    files = []
    out = None
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--crop":
            crop_w, crop_h = int(args[i + 1]), int(args[i + 2]); i += 3
        elif a == "--scale":
            scale = int(args[i + 1]); i += 2
        elif a == "--every":
            every = int(args[i + 1]); i += 2
        elif a == "--w":
            w = int(args[i + 1]); i += 2
        elif a == "--h":
            h = int(args[i + 1]); i += 2
        elif out is None:
            out = a; i += 1
        else:
            # Windows shells do not expand globs; do it here.
            g = sorted(glob.glob(a))
            files.extend(g if g else [a]); i += 1
    if out is None or not files:
        print(__doc__)
        return 2
    files = files[::every]

    cols = min(len(files), 10)
    rows = (len(files) + cols - 1) // cols
    cw, ch = crop_w * scale, crop_h * scale + 12
    sheet = Image.new("RGB", (cols * cw, rows * ch), (24, 24, 32))
    draw = ImageDraw.Draw(sheet)
    for n, path in enumerate(files):
        img = load_raw(path, w, h)
        cx, cy = w // 2, h // 2
        box = (cx - crop_w // 2, cy - crop_h // 2,
               cx + crop_w // 2, cy + crop_h // 2)
        tile = img.crop(box).resize((crop_w * scale, crop_h * scale),
                                    Image.NEAREST)
        x, y = (n % cols) * cw, (n // cols) * ch
        sheet.paste(tile, (x, y + 12))
        # The label is the frame's own number, not its position in the
        # sheet, so `--every 2` still reads as ticks.
        base = os.path.basename(path)
        digits = "".join(c for c in base if c.isdigit())
        draw.text((x + 2, y), "t%s" % (digits or n), fill=(255, 255, 0))
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    sheet.save(out)
    print("%s: %d frames, %dx%d" % (out, len(files), sheet.width, sheet.height))
    return 0


if __name__ == "__main__":
    sys.exit(main())
