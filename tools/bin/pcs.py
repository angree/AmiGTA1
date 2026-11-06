"""PC samples -> functions, statics included: each object file's own symbol table
is placed through its globals, whose final addresses the executable's table has."""
import struct, subprocess, collections, sys, glob, bisect, os
NM = "/opt/amiga/bin/m68k-amigaos-nm"
exe = "/mnt/c/temp/amiga_gta_build/build/AmiGTA"
objs = glob.glob("/mnt/c/temp/amiga_gta_build/build/amiga/obj/*.o")
final = {}
for l in subprocess.run([NM, exe], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in "tT":
        final[p[2]] = int(p[0], 16)
syms = []
for o in objs:
    rows = []
    for l in subprocess.run([NM, o], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) == 3 and p[1] in "tT":
            rows.append((int(p[0], 16), p[2], p[1]))
    base = None
    for a, s, t in rows:
        if t == "T" and s in final:
            base = final[s] - a
            break
    if base is None:
        continue
    for a, s, t in rows:
        syms.append((a + base, os.path.basename(o)[:-2] + ":" + s.lstrip("_")))
syms.sort()
addr = [a for a, _ in syms]
d = open("/mnt/c/temp/amiga_gta/work/pcprof.bin", "rb").read()
m, a, n = struct.unpack(">III", d[:12])
base = a - final["_amiga_watchdog_start"]
top = int(sys.argv[1]) if len(sys.argv) > 1 else 40
fn = collections.Counter(); N = 0; PH = ["none", "input", "sim", "traffic", "peds", "weapons", "player", "render", "present", "cap"]
byph = collections.defaultdict(collections.Counter)


def name(x):
    i = bisect.bisect_right(addr, x - base) - 1
    return syms[i][1] if i >= 0 and 0 <= x - base < 400000 else None


for i in range(n):
    pc, ra, ph = struct.unpack(">III", d[12 + i * 12:24 + i * 12])
    if pc == 0:
        s = "(waiting)"
    else:
        N += 1
        s = name(pc)
        if s is None:
            s = "ROM <- " + (name(ra) or "?")
    fn[s] += 1
    byph[PH[ph] if ph < 10 else "?"][s] += 1
print("objects", len(objs), "symbols", len(syms), "samples", n)
for k, v in fn.most_common(top):
    print("%6.1f%% %5d %s" % (100.0 * v / n, v, k))
for ph in ("render", "present", "cap", "player", "traffic", "sim"):
    t = sum(byph[ph].values())
    print("--", ph, "%.1f%%" % (100.0 * t / n), ", ".join("%s %.1f" % (k, 100.0 * v / n) for k, v in byph[ph].most_common(9)))
