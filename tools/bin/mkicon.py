#!/usr/bin/env python3
"""mkicon.py <out.info> [stack]

Write an AmigaOS Workbench icon (.info) for the game.

WHY THIS IS GENERATED RATHER THAN CHECKED IN
--------------------------------------------
An .info is a binary with pointers in it, and the one field that matters most
here - the STACK SIZE - is a long buried 74 bytes in. Generating it means the
stack is a number in a script with a comment next to it rather than a byte
nobody can find, and it means the icon can be regenerated when the image or the
tool types change.

THE STACK IS THE POINT. A tool launched from Workbench gets the stack its icon
asks for, and NOT the shell's default - so an icon with the usual 4096 gives
this game a quarter of the stack it gets from `Work:run`, and the failure is a
silent trashing of whatever sits below the stack rather than an error. The
renderer alone puts a 64-entry sprite request array and several hundred bytes
of locals on it, and gta_view is a large struct. 1000000 is what
`Work:run` sets and what the README tells the player to set by hand, and this
number MUST match those: the same binary getting a fifth of the stack because
it was double-clicked rather than started from the script is a bug that only
shows up as corruption somewhere else. It was 200000 here until 2026-08-28 and
the mismatch went unnoticed because the scripted path was the one being
tested. Stack costs nothing until it is used, and running out of it on a
68020 does not produce a diagnosable crash.

FORMAT, from the AmigaOS includes (workbench/workbench.h, intuition/intuition.h)
- everything big-endian:

    DiskObject       78 bytes   magic E310, version 1, a Gadget, type, pointers
      Gadget         44 bytes   inside it; GadgetRender must be non-NULL
    Image            20 bytes   follows the DiskObject when GadgetRender is set
    image data                  planar, plane-major, rows padded to a WORD

Only the fields Workbench actually reads are filled; the pointer fields are
booleans as far as the file format is concerned (non-zero means "data for this
follows"), which is why they are written as 1 rather than as addresses.

    wsl python3 tools/bin/mkicon.py winuae/work-template/AmiGTA.info 1000000
    wsl python3 tools/bin/mkicon.py --verify <file.info> <out.png>
"""
import struct

# THE ONE STACK SIZE. `Work:run` sets this and the README tells the player to
# set it by hand; an icon that asks for less gives a double-clicked game a
# smaller stack than a scripted one, and overrunning it on a 68020 is a silent
# overwrite rather than an error. Change it here and nowhere else.
GTA_STACK = 1000000
import sys

# Workbench's four standard pens. The icon is drawn in these and nothing else,
# so it looks right on any Workbench rather than only on one with our palette:
#   0 grey (background)   1 black   2 white   3 blue
GREY, BLACK, WHITE, BLUE = 0, 1, 2, 3

W, H = 46, 40
DEPTH = 2


def draw() -> list:
    """A top-down car on a stretch of road - the game in one picture, and the
    only thing that reads at 46x40 in four colours. Drawn with primitives
    rather than as pixel art because the shapes are all rectangles anyway."""
    px = [[BLACK] * W for _ in range(H)]

    def rect(x0, y0, x1, y1, c):
        for y in range(max(0, y0), min(H, y1 + 1)):
            for x in range(max(0, x0), min(W, x1 + 1)):
                px[y][x] = c

    # The road surface is the black background. Lane markings down both sides,
    # dashed, in grey - white would compete with the car.
    for y in range(2, H - 2, 6):
        rect(4, y, 4, y + 2, GREY)
        rect(W - 5, y, W - 5, y + 2, GREY)

    # Car body, pointing up the icon. Narrower at the nose, which is what makes
    # it read as facing somewhere rather than as a box.
    rect(15, 7, 30, 33, WHITE)
    rect(16, 5, 29, 6, WHITE)
    rect(17, 4, 28, 4, WHITE)

    # Wheels, outside the body on both sides. GREY and not black: the road is
    # black too, and the first version drew them black on black - invisible,
    # and the car read as a plain white slab.
    rect(13, 10, 14, 14, GREY)
    rect(31, 10, 32, 14, GREY)
    rect(13, 25, 14, 29, GREY)
    rect(31, 25, 32, 29, GREY)

    # Windscreen and rear window. Blue is the only colour left and it is what
    # every Amiga icon uses for glass.
    rect(17, 9, 28, 15, BLUE)
    rect(17, 24, 28, 30, BLUE)
    # Roof between them.
    rect(17, 16, 28, 23, WHITE)
    # A dark centre line down the roof, so the two windows do not merge into
    # one shape at small sizes.
    rect(22, 16, 23, 23, GREY)

    # Headlights.
    rect(17, 5, 19, 6, GREY)
    rect(26, 5, 28, 6, GREY)

    # A white frame, so the icon has an edge against any Workbench backdrop.
    rect(0, 0, W - 1, 0, WHITE)
    rect(0, H - 1, W - 1, H - 1, WHITE)
    rect(0, 0, 0, H - 1, WHITE)
    rect(W - 1, 0, W - 1, H - 1, WHITE)
    return px


def planar(px: list) -> bytes:
    """Planar, plane-major, each row padded out to a whole number of WORDs -
    which is what the Amiga's blitter wants and what Image.ImageData means."""
    words = (W + 15) // 16
    out = bytearray()
    for plane in range(DEPTH):
        for y in range(H):
            bits = 0
            for x in range(W):
                bits = (bits << 1) | ((px[y][x] >> plane) & 1)
            bits <<= (words * 16 - W)
            out += bits.to_bytes(words * 2, "big")
    return bytes(out)


def verify(path: str, png: str) -> int:
    """Read the .info back and render it, because writing a binary proves
    nothing about whether it is the binary the format describes.

    This project has twice shipped a reader that compiled, ran, and was wrong
    about the layout it was reading (the .GRY header, the 10-byte sprite
    record). An icon is the same kind of risk with less feedback: Workbench
    either draws it or quietly substitutes a default, and neither outcome
    reaches a log. So the fields are checked by offset and the image is drawn.
    """
    raw = open(path, "rb").read()
    magic, ver = struct.unpack(">HH", raw[0:4])
    # Gadget starts at 4; inside it NextGadget is 4 bytes and LeftEdge/TopEdge
    # 2 each, so Width and Height are at 12 and 14. Reading them at 10 gave
    # "0x46" - LeftEdge and Width - and failed a file that was correct.
    gw, gh = struct.unpack(">hh", raw[12:16])
    dtype = raw[48]
    stack = struct.unpack(">i", raw[74:78])[0]
    iw, ih, idepth = struct.unpack(">hhh", raw[82:88])

    print(f"{path}")
    print(f"  magic      ${magic:04X}   (want $E310)")
    print(f"  version    {ver}")
    print(f"  gadget     {gw}x{gh}")
    print(f"  type       {dtype}      (3 = WBTOOL)")
    print(f"  STACK      {stack}")
    print(f"  image      {iw}x{ih} depth {idepth}")

    ok = (magic == 0xE310 and dtype == 3 and stack > 0 and
          gw == iw and gh == ih and idepth == DEPTH)
    if not ok:
        print("  *** the fields do not describe a usable tool icon ***")
        return 1

    words = (iw + 15) // 16
    data = raw[98:]
    need = words * 2 * ih * idepth
    if len(data) < need:
        print(f"  *** short by {need - len(data)} bytes of image data ***")
        return 1

    from PIL import Image
    rgb = [(170, 170, 170), (0, 0, 0), (255, 255, 255), (102, 136, 187)]
    im = Image.new("RGB", (iw, ih))
    for y in range(ih):
        for x in range(iw):
            v = 0
            for pl in range(idepth):
                off = (pl * ih + y) * words * 2 + (x >> 3)
                v |= ((data[off] >> (7 - (x & 7))) & 1) << pl
            im.putpixel((x, y), rgb[v])
    im.resize((iw * 6, ih * 6), Image.NEAREST).save(png)
    print(f"  rendered   {png}")
    return 0


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "--verify":
        return verify(sys.argv[2], sys.argv[3])
    out = sys.argv[1]
    stack = int(sys.argv[2]) if len(sys.argv) > 2 else GTA_STACK

    px = draw()
    data = planar(px)

    # struct Gadget, 44 bytes. Width/Height must match the Image or Workbench
    # draws the icon clipped. GADGIMAGE (0x0004) says GadgetRender is an Image
    # rather than a Border; GadgetRender itself is a "there is data" flag here.
    gadget = struct.pack(
        ">IhhhhHHHIIIiIHI",
        0,          # NextGadget
        0, 0,       # LeftEdge, TopEdge
        W, H,       # Width, Height
        0x0004,     # Flags: GADGIMAGE
        0x0001,     # Activation: RELVERIFY
        0x0001,     # GadgetType: BOOLGADGET
        1,          # GadgetRender - non-zero: an Image follows
        0,          # SelectRender - none, so no second image
        0,          # GadgetText
        0,          # MutualExclude
        0,          # SpecialInfo
        0,          # GadgetID
        0,          # UserData
    )
    assert len(gadget) == 44, len(gadget)

    disk = struct.pack(">HH", 0xE310, 1) + gadget + struct.pack(
        ">BBIIiiIIi",
        3,          # do_Type: WBTOOL - an executable, launched directly
        0,          # pad
        0,          # do_DefaultTool - a tool needs none
        0,          # do_ToolTypes  - none
        -0x80000000,  # do_CurrentX: NO_ICON_POSITION, let Workbench place it
        -0x80000000,  # do_CurrentY
        0,          # do_DrawerData - not a drawer
        0,          # do_ToolWindow
        stack,      # do_StackSize  <-- the field this script exists for
    )
    assert len(disk) == 78, len(disk)

    image = struct.pack(
        ">hhhhhIBBI",
        0, 0,       # LeftEdge, TopEdge
        W, H,       # Width, Height
        DEPTH,      # Depth
        1,          # ImageData - non-zero: the bitmap follows
        (1 << DEPTH) - 1,   # PlanePick: both planes come from the data
        0,          # PlaneOnOff
        0,          # NextImage
    )
    assert len(image) == 20, len(image)

    with open(out, "wb") as f:
        f.write(disk)
        f.write(image)
        f.write(data)

    print(f"{out}: {W}x{H}x{DEPTH}, stack {stack}, "
          f"{78 + 20 + len(data)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
