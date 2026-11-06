#!/usr/bin/env python3
"""Turn Work:pcprof.bin (`pcprof 1` in opts.txt) into a table of where the time goes.

    python3 tools/bin/pcprof.py <pcprof.bin> <AmiGTA, not stripped> [top]

Reads the symbols with m68k-amigaos-nm and lines them up through
amiga_watchdog_start, whose run-time address the file's header carries."""
import struct, subprocess, sys, bisect, collections

path, exe = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
data = open(path, "rb").read()
magic, anchor, n = struct.unpack(">III", data[:12])
assert magic == 0x50435046, "not a pcprof file"
nm = subprocess.run(["/opt/amiga/bin/m68k-amigaos-nm", "-n", exe],
                    capture_output=True, text=True).stdout
syms = []
for line in nm.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in "tT":
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addr = [a for a, _ in syms]
base = anchor - dict((s, a) for a, s in syms)["_amiga_watchdog_start"]
end = addr[-1] + base + 0x4000


def name(a):
    if a < base or a >= end:
        return None
    i = bisect.bisect_right(addr, a - base) - 1
    return syms[i][1].lstrip("_") if i >= 0 else None


PH = ["none", "input", "sim", "traffic", "peds", "weapons", "player",
      "render", "present", "cap"]
fn = collections.Counter()
phase = collections.Counter()
out = collections.Counter()
waiting = 0
for i in range(n):
    pc, ra, ph = struct.unpack(">III", data[12 + i * 12:24 + i * 12])
    label = PH[ph] if ph < len(PH) else "?"
    phase[label] += 1
    if pc == 0:
        waiting += 1
        fn["(waiting in the OS, " + label + ")"] += 1
        continue
    s = name(pc)
    if s is None:
        out[pc & 0xfffff000] += 1
        s = "ROM/lib <- " + (name(ra) or "?")
    fn[s] += 1

print("%d samples, %d waiting" % (n, waiting))
print("-- by phase")
for k, v in phase.most_common():
    print("%6.1f%%  %s" % (100.0 * v / n, k))
print("-- by function")
for k, v in fn.most_common(top):
    print("%6.1f%%  %5d  %s" % (100.0 * v / n, v, k))
