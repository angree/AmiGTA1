#!/bin/sh
# Is the renderer leaving holes anywhere in the city?
#
#   wsl sh /mnt/i/GITHUB/Amiga_GTA/tools/bin/holecheck.sh
#
# Renders a spread of camera positions with the frame cleared to palette index
# 200 - a colour nothing in Liberty City uses - and counts how many pixels are
# still that colour afterwards. Any non-zero count is a gap in the geometry.
#
# WHY THIS EXISTS. Black holes in the picture are invisible as bugs: a black
# tile and an undrawn pixel look identical, and GTA's artwork is full of dark
# roofs. Clearing to something loud turns a question of taste into a number.
# It is how the corner-square gap was found and how the fix was proved: the
# same eight positions went from 1446 uncovered pixels to 0.
#
# A count above zero is not automatically a bug at the EDGE of the map, where
# the city genuinely runs out - so the positions below are all inland.
set -e

ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i

ROOT=/mnt/i/GITHUB/Amiga_GTA
cd "$ROOT"

MAP=${MAP:-/mnt/c/temp/amiga_gta/work/GTADATA/nyc.cmp}
TIL=${TIL:-build/data/style001.til}
CLEAR=200
ZOOM=${ZOOM:-32}
W=${W:-320}
H=${H:-200}

mkdir -p out

total=0
for pos in 40:40 56:58 64:64 90:70 120:120 160:90 200:200 30:180; do
    bx=$(echo "$pos" | cut -d: -f1)
    by=$(echo "$pos" | cut -d: -f2)
    ./build/host/gtadump view "$MAP" "$TIL" "$bx" "$by" "$W" "$H" \
        out/holecheck.bmp "$CLEAR" "$ZOOM" >/dev/null
    n=$(python3 - "$bx" "$by" <<'PY'
import sys
from PIL import Image
d = Image.open("out/holecheck.bmp").tobytes()
n = d.count(bytes([200]))
print("block %s,%s  uncovered %d of %d" % (sys.argv[1], sys.argv[2], n, len(d)),
      file=sys.stderr)
print(n)
PY
)
    total=$((total + n))
done

echo "--- total uncovered pixels: $total ---"
[ "$total" -eq 0 ] || exit 1
