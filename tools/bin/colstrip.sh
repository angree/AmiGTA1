#!/bin/sh
# colstrip.sh <x> <y0> <y1> [map] - dump every map column of one x from y0 to y1.
# Exists because a $var inside `wsl sh -c '...'` from Git Bash expands empty.
cd "$(dirname "$0")/../.." || exit 1
X=$1; Y0=$2; Y1=$3
MAP=${4:-/mnt/c/temp/amiga_gta/work/GTADATA/nyc.cmp}
y=$Y0
while [ "$y" -le "$Y1" ]; do
    echo "== ($X,$y)"
    build/host/gtadump column "$MAP" "$X" "$y" 2>&1
    y=$((y+1))
done
