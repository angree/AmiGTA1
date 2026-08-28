#!/usr/bin/env python3
"""facingcheck.py <peds.bmp> <out.png>

Which way does the pedestrian ART point before any rotation is applied?

`gta_player_draw_angle` adds GTA_SPRITE_ART_SOUTH to the player's angle before
handing it to the rotozoom, so the constant encodes the art's own resting
direction. Getting it wrong by half a turn makes every ped and every car in the
game moonwalk, and it cannot be seen on a 9-pixel sprite in a moving frame.

So this magnifies the four frames that can answer it on their own, straight out
of the tilesprites contact sheet (24 columns, one cell per frame):

  89  shoot_pistol_while_standing  - a pistol is held in FRONT of the body
  50  punching_while_standing      - so is a fist
  42  lies_on_floor                - a body seen full length: head one end
  98  standing_still               - the resting frame the game actually uses
  97  sitting_in_car               - carries the answer ACROSS to vehicles:
                                     a driver faces his car's front, and this
                                     frame is drawn unrotated, so whichever way
                                     the seated man faces is the way an
                                     unrotated CAR faces. Car art cannot be
                                     read directly - a top-down saloon is very
                                     nearly symmetric - so it is read through
                                     the man sitting in it.

Read them as pictures, not as numbers: whichever way the gun and the fist
stick out is the direction the art faces on screen with no rotation.

    wsl python3 tools/bin/facingcheck.py out/facing/peds.bmp out/facing/gun.png
"""
import sys

from PIL import Image, ImageDraw

COLS = 24
CELL = 42
SCALE = 14
FRAMES = [
    (89, "89 pistol"),
    (50, "50 punch"),
    (42, "42 lying"),
    (98, "98 stand"),
    (97, "97 in car"),
]


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    sheet = Image.open(src).convert("RGB")

    pad = 8
    label = 16
    cw = CELL * SCALE
    out = Image.new("RGB", (len(FRAMES) * (cw + pad) + pad,
                            cw + label + 2 * pad), (32, 32, 32))
    draw = ImageDraw.Draw(out)

    for n, (idx, name) in enumerate(FRAMES):
        cx = (idx % COLS) * CELL
        cy = (idx // COLS) * CELL
        cell = sheet.crop((cx, cy, cx + CELL, cy + CELL))
        cell = cell.resize((cw, cw), Image.NEAREST)
        x = pad + n * (cw + pad)
        out.paste(cell, (x, pad + label))
        draw.text((x + 2, pad + 2), name, fill=(255, 255, 0))
        # A cross through the centre of the cell, so "which side does it stick
        # out of" is a question about the picture and not about the eye.
        mid = x + cw // 2
        draw.line((mid, pad + label, mid, pad + label + cw), fill=(255, 0, 255))
        midy = pad + label + cw // 2
        draw.line((x, midy, x + cw, midy), fill=(255, 0, 255))

    draw.text((pad, out.height - 14), "top of image = up", fill=(160, 160, 160))
    out.save(dst)
    print(f"{src} -> {dst} ({out.width}x{out.height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
