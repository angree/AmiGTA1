#!/bin/sh
# Uncovered-pixel count at specific camera positions.
#
#   wsl -e sh /mnt/i/GITHUB/Amiga_GTA/tools/bin/holeat.sh 54:144 114:204 ...
#
# holesweep.sh walks a grid; this one answers "is THERE a hole at this exact
# spot", which is what a bug report from a person driving the camera turns into.
# Positions are bx:by. A frame is left in out/holeat_<bx>_<by>.bmp so the same
# run can be looked at as well as counted.
set -e

ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i

ROOT=/mnt/i/GITHUB/Amiga_GTA
cd "$ROOT"

MAP=${MAP:-/mnt/c/temp/amiga_gta/work/GTADATA/nyc.cmp}
TIL=${TIL:-build/data/style001.til}
W=${W:-320}
H=${H:-200}

mkdir -p out

for pos in "$@"; do
    bx=$(echo "$pos" | cut -d: -f1)
    by=$(echo "$pos" | cut -d: -f2)
    out="out/holeat_${bx}_${by}.bmp"
    ./build/host/gtadump view "$MAP" "$TIL" "$bx" "$by" "$W" "$H" \
        "$out" 200 >/dev/null
    python3 - "$bx" "$by" "$out" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[3])
d = im.tobytes()
n = d.count(bytes([200]))
w = im.size[0]
where = ""
if n:
    xs = [i % w for i, v in enumerate(d) if v == 200]
    ys = [i // w for i, v in enumerate(d) if v == 200]
    where = "  bbox x %d..%d y %d..%d" % (min(xs), max(xs), min(ys), max(ys))
print("block %s,%s  uncovered %d%s" % (sys.argv[1], sys.argv[2], n, where))
PY
done
