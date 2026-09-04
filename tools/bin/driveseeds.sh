#!/bin/sh
# driveseeds.sh [ticks] - the traffic regression over four seeds, one line each.
#
# One seed is a sample, not a measurement: any behavioural change shifts the
# spawn sequence, so two runs of different builds at the same seed are
# different fleets. The spread across seeds is what says whether a couple of
# points of flow is a signal.
cd "$(dirname "$0")/../.." || exit 1
MAP=${MAP:-/mnt/c/temp/amiga_gta/work/GTADATA/nyc.cmp}
TIL=${TIL:-build/data/style001.til}
T=${1:-6000}
for s in 12345 777 4242 99; do
    printf 'seed %-6s ' "$s"
    build/host/gtadump drive "$MAP" "$TIL" 64 64 out/drive.bmp "$T" 50 "$s" 2>&1 |
        grep -a "flow -\|after $T ticks\|impossible -" |
        sed 's/drive: after .* - //; s/drive: //' | tr '\n' '|' |
        sed 's/|$//'
    echo
done
