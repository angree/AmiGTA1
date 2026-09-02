#!/usr/bin/env python3
"""Read the sprite DELTA records out of style001.gry and draw one.

The port's own reader walks past the 6-byte delta records without keeping
them (native/gta_style.c, read_sprite_info). This probe answers, on the host
in a second, the three questions that decide whether it is worth keeping them:

  1. How many deltas are there, and which sprites have them?
  2. Is the record really {u16 size, s32 offset} as Carnage3D reads it?
  3. Does decoding one over its base sprite actually open a car door?

Question 3 is the only one that matters, and it can only be answered by
LOOKING at the result - a decoder can be syntactically perfect and wrong,
which is how the .GRY header and the sprite record were each got wrong once.
"""
import io, struct, sys, os

GRY = "dos/Grand_Theft_Auto/gtadata/style001.gry"
OUT = "out"

f = io.open(GRY, "rb")
hdr = struct.unpack("<13I", f.read(13 * 4))
(version, side, lid, aux, anim, pal, unk_a, unk_b,
 objinfo, carsz, sprinfo, sprgfx, sprnums) = hdr
print("version %d  sprite_info %d  sprite_graphics %d" % (version, sprinfo, sprgfx))

# sections in file order after the header
off = 13 * 4
def skip(n):
    global off
    off += n

skip(side + lid + aux + anim)
pal_off = off; skip(pal)
skip(unk_a + unk_b + objinfo + carsz)
sprinfo_off = off; skip(sprinfo)
sprgfx_off  = off; skip(sprgfx)
sprnums_off = off

# --- the palette (768 bytes, may be 6-bit) --------------------------------
f.seek(pal_off)
palette = bytearray(f.read(768))
if max(palette) <= 63:
    palette = bytearray((v * 255) // 63 for v in palette)

# --- walk sprite_info, KEEPING the deltas ---------------------------------
f.seek(sprinfo_off)
blob = f.read(sprinfo)
sprites = []
p = 0
while p + 10 <= len(blob):
    w, h, ndelta = blob[p], blob[p+1], blob[p+2]
    size, = struct.unpack_from("<H", blob, p+4)
    px, py = blob[p+6], blob[p+7]
    page, = struct.unpack_from("<H", blob, p+8)
    p += 10
    deltas = []
    for _ in range(ndelta):
        dsize, doff = struct.unpack_from("<Hi", blob, p)
        deltas.append((dsize, doff))
        p += 6
    sprites.append(dict(w=w, h=h, size=size, px=px, py=py, page=page,
                        deltas=deltas))
print("walked %d sprite records, consumed %d of %d bytes"
      % (len(sprites), p, len(blob)))

bad = [i for i, s in enumerate(sprites) if s["size"] != s["w"] * s["h"]]
print("records failing size == w*h:", len(bad))

tot = sum(len(s["deltas"]) for s in sprites)
withd = [i for i, s in enumerate(sprites) if s["deltas"]]
print("sprites with deltas: %d   total delta records: %d" % (len(withd), tot))

# category bases, from the port's own order
f.seek(sprnums_off)
nums = struct.unpack("<21H", f.read(42))
names = ["arrow","digit","boat","box","bus","car","object","ped","speedo",
         "tank","traffic_light","train","trdoor","bike","tram","wrecked_car",
         "wbus","ex","tumcar","tumtruck","ferry"]
base = 0
bases = {}
for i, n in enumerate(nums):
    bases[names[i]] = (base, n)
    base += n
print("car sprites: base %d count %d" % bases["car"])

cbase, ccount = bases["car"]
for i in range(cbase, cbase + min(ccount, 6)):
    s = sprites[i]
    print("  car sprite %3d  %2dx%-2d  %2d deltas  sizes %s"
          % (i, s["w"], s["h"], len(s["deltas"]),
             [d[0] for d in s["deltas"]][:8]))

# --- sprite graphics ------------------------------------------------------
f.seek(sprgfx_off)
gfx = f.read(sprgfx)
PAGE = 256
PAGESZ = PAGE * PAGE

def sprite_pixels(s):
    """The base sprite, w*h palette indices, out of its 256x256 page."""
    out = bytearray(s["w"] * s["h"])
    pg = s["page"] * PAGESZ
    for y in range(s["h"]):
        src = pg + (s["py"] + y) * PAGE + s["px"]
        out[y*s["w"]:(y+1)*s["w"]] = gfx[src:src + s["w"]]
    return out

def apply_delta(s, pix, dsize, doff):
    """Carnage3D's ApplySpriteDelta, on the SPRITE's own w*h buffer.

    The run offsets accumulate in units of a 256-wide page row, so a run that
    steps to the next line adds 256 - hence the divmod by 256 and not by the
    sprite width. They are relative to the sprite's top-left, not the page's:
    that is the one thing this probe is really testing."""
    d = gfx[doff:doff + dsize]
    pos = 0
    dst = 0
    runs = 0
    while pos + 3 <= len(d):
        off = d[pos] | (d[pos+1] << 8)
        ln = d[pos+2]
        pos += 3
        if ln == 0 or pos + ln > len(d):
            break
        dst += off
        y, x = divmod(dst, PAGE)
        for k in range(ln):
            if 0 <= x + k < s["w"] and 0 <= y < s["h"]:
                pix[y * s["w"] + x + k] = d[pos + k]
        dst += ln
        pos += ln
        runs += 1
    return runs

def write_png(path, cols, rows, cell_w, cell_h, images, scale=6):
    """One PNG, images laid out in a grid, nearest-scaled."""
    from PIL import Image
    W = cols * (cell_w * scale + 4) + 4
    H = rows * (cell_h * scale + 4) + 4
    sheet = Image.new("RGB", (W, H), (25, 25, 30))
    for n, (w, h, pix) in enumerate(images):
        im = Image.new("RGB", (w, h))
        im.putdata([(palette[p*3], palette[p*3+1], palette[p*3+2])
                    for p in pix])
        im = im.resize((w*scale, h*scale), Image.NEAREST)
        r, c = divmod(n, cols)
        sheet.paste(im, (4 + c*(cell_w*scale+4), 4 + r*(cell_h*scale+4)))
    sheet.save(path)
    print("wrote", path, sheet.size)

# The first car sprite that actually has deltas.
target = None
for i in range(cbase, cbase + ccount):
    if len(sprites[i]["deltas"]) >= 10:
        target = i
        break
if target is None:
    print("NO car sprite has 10+ deltas - the door-delta assumption is wrong")
    sys.exit(1)

s = sprites[target]
print("drawing car sprite %d: %dx%d, %d deltas, page %d at (%d,%d)"
      % (target, s["w"], s["h"], len(s["deltas"]), s["page"], s["px"], s["py"]))

imgs = [(s["w"], s["h"], sprite_pixels(s))]           # base first
for k, (dsize, doff) in enumerate(s["deltas"][:15]):
    pix = sprite_pixels(s)
    runs = apply_delta(s, pix, dsize, doff)
    print("  delta %2d  size %5d off %8d  -> %d runs" % (k, dsize, doff, runs))
    imgs.append((s["w"], s["h"], pix))

os.makedirs(OUT, exist_ok=True)
write_png(os.path.join(OUT, "delta_car_%d.png" % target),
          4, (len(imgs)+3)//4, s["w"], s["h"], imgs)
