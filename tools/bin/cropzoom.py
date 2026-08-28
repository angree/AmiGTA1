#!/usr/bin/env python3
"""cropzoom.py <in.bmp> <out.png> [cx cy half scale]

Crop a square out of one of the host tools' 8-bit BMPs and magnify it, so a
sprite that is nine pixels tall on a 320x200 frame can actually be looked at.

Defaults to the middle of the frame, which is where the camera puts the player.

    wsl python3 tools/bin/cropzoom.py out/walk/w00.bmp out/walk/c00.png
    wsl python3 tools/bin/cropzoom.py out/walk/w00.bmp out/c.png 160 100 24 10
"""
import sys

from PIL import Image


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    img = Image.open(src).convert("RGB")
    w, h = img.size
    cx = int(sys.argv[3]) if len(sys.argv) > 3 else w // 2
    cy = int(sys.argv[4]) if len(sys.argv) > 4 else h // 2
    half = int(sys.argv[5]) if len(sys.argv) > 5 else 32
    scale = int(sys.argv[6]) if len(sys.argv) > 6 else 8

    box = (max(0, cx - half), max(0, cy - half),
           min(w, cx + half), min(h, cy + half))
    crop = img.crop(box)
    crop = crop.resize((crop.width * scale, crop.height * scale), Image.NEAREST)
    crop.save(dst)
    print(f"{src} {box} -> {dst} ({crop.width}x{crop.height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
