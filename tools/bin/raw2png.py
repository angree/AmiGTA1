#!/usr/bin/env python3
"""raw2png.py <frame.raw> <out.png> [width] [height] [scale]

Turn a framebuffer dump written by the Amiga (gta_main.c, dump_frame) into a
PNG. The format is deliberately trivial - 768 bytes of RGB palette followed by
width*height palette indices, no header - because this script is the only thing
that reads it.

It exists because PrintWindow screenshots of the WinUAE window come back black
while the game is blitting: the emulated display lives on a DirectDraw surface
rather than in the window's GDI device context. A dump straight out of the
chunky buffer is better evidence anyway - it is exactly the bytes the
chunky-to-planar routine is about to consume, so it separates "the renderer is
wrong" from "the screenshot is wrong".

    wsl python3 tools/bin/raw2png.py /mnt/c/temp/amiga_gta/work/frame.raw \\
        out/amiga_frame.png 320 200 2
"""
import sys

from PIL import Image


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    w = int(sys.argv[3]) if len(sys.argv) > 3 else 320
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 200
    scale = int(sys.argv[5]) if len(sys.argv) > 5 else 1

    with open(src, "rb") as f:
        blob = f.read()

    want = 768 + w * h
    if len(blob) != want:
        print("%s is %d bytes, expected %d for %dx%d"
              % (src, len(blob), want, w, h), file=sys.stderr)
        return 1

    img = Image.frombytes("P", (w, h), blob[768:])
    img.putpalette(blob[:768])
    img = img.convert("RGB")
    if scale > 1:
        img = img.resize((w * scale, h * scale), Image.NEAREST)
    img.save(dst)
    print("%s -> %s (%dx%d)" % (src, dst, img.width, img.height))
    return 0


if __name__ == "__main__":
    sys.exit(main())
