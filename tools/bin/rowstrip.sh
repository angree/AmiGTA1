#!/bin/sh
# rowstrip.sh <y> <x0> <x1> [z] [map] - one line per block along a row: for each
# x, the z=2 and z=3 lines of `gtadump column` (or only layer z if given).
cd "$(dirname "$0")/../.." || exit 1
Y=$1; X0=$2; X1=$3; Z=${4:-}
MAP=${5:-/mnt/c/temp/amiga_gta/work/GTADATA/nyc.cmp}
x=$X0
while [ "$x" -le "$X1" ]; do
    if [ -n "$Z" ]; then
        build/host/gtadump column "$MAP" "$x" "$Y" 2>&1 | grep "z=$Z " | sed "s/^/($x,$Y) /"
    else
        build/host/gtadump column "$MAP" "$x" "$Y" 2>&1 | grep "z=[234] " | sed "s/^/($x,$Y) /"
    fi
    x=$((x+1))
done
