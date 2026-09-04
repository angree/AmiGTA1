#!/bin/sh
# live2png.sh <prefix> - every Work:liveNN.raw filmed by autodrive.txt, as
# out/<prefix>NN.png at 2x. Runs under WSL.
cd "$(dirname "$0")/../.." || exit 1
P=${1:-live}
mkdir -p out
for f in /mnt/c/temp/amiga_gta/work/live[0-9][0-9].raw; do
    [ -f "$f" ] || continue
    n=$(basename "$f" .raw); n=${n#live}
    python3 tools/bin/raw2png.py "$f" "out/$P$n.png" 320 200 2 >/dev/null && echo "out/$P$n.png"
done
