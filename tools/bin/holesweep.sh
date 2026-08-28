#!/bin/sh
# holecheck.sh over the WHOLE city instead of eight hand-picked spots.
#
#   wsl -e sh /mnt/i/GITHUB/Amiga_GTA/tools/bin/holesweep.sh [step]
#
# Eight positions is enough for a regression test and nowhere near enough to
# FIND a bug: the corner-square gap survived two people looking straight at it,
# and the artefact reported after it was fixed did not appear in any of the
# eight. This walks the map on a grid, renders a frame at each point with the
# buffer cleared to a colour Liberty City does not use, and reports every
# position that leaves anything uncovered - worst first.
#
# The map edges genuinely run out of city, so positions within `step` of the
# border are skipped rather than reported as failures.
set -e

ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i

ROOT=/mnt/i/GITHUB/Amiga_GTA
cd "$ROOT"

MAP=${MAP:-/mnt/c/temp/amiga_gta/work/GTADATA/nyc.cmp}
TIL=${TIL:-build/data/style001.til}
STEP=${1:-16}

mkdir -p out
: > out/holesweep.txt

y=32
while [ "$y" -le 224 ]; do
    x=32
    while [ "$x" -le 224 ]; do
        ./build/host/gtadump view "$MAP" "$TIL" "$x" "$y" 320 200 \
            out/holesweep.bmp 200 >/dev/null
        python3 - "$x" "$y" >> out/holesweep.txt <<'PY'
import sys
from PIL import Image
n = Image.open("out/holesweep.bmp").tobytes().count(bytes([200]))
if n:
    print("%8d  %s %s" % (n, sys.argv[1], sys.argv[2]))
PY
        x=$((x + STEP))
    done
    y=$((y + STEP))
done

echo "--- positions leaving uncovered pixels (count, bx, by) ---"
sort -rn out/holesweep.txt | head -20
echo "--- total positions with holes:" "$(grep -c . out/holesweep.txt || true)" "---"
