#!/usr/bin/env python3
"""bmp2png.py <in.bmp> <out.png> [scale]

The host tools write 8-bit BMPs because that keeps the palette visible and
needs no library. Looking at one still means turning it into something a
viewer will open, and optionally magnifying it: at 320x200 a one-pixel seam
between two tiles is invisible until it is blown up.

    wsl python3 tools/bin/bmp2png.py out/view.bmp out/view.png 3
"""
import sys

from PIL import Image


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1

    img = Image.open(src).convert("RGB")
    if scale > 1:
        img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
    img.save(dst)
    print("%s -> %s (%dx%d)" % (src, dst, img.width, img.height))
    return 0


if __name__ == "__main__":
    sys.exit(main())
