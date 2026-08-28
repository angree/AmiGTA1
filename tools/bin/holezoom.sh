#!/bin/sh
# holecheck.sh across the zoom range.
#
#   wsl -e sh /mnt/i/GITHUB/Amiga_GTA/tools/bin/holezoom.sh
#
# The corner fill and the slope interpolation are both expressed in grid-level
# positions, so they ought to be zoom-independent - but "ought to be" is not a
# proof, and the rounding to whole pixels happens after the zoom is applied.
# Zoom is continuous, so this samples awkward sizes too - primes and values
# just off the ones the tiles were baked at. Every one must print 0.
set -e

ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i

cd /mnt/i/GITHUB/Amiga_GTA

for z in 16 17 19 22 25 28 31 32 34 37 40 45 50 55 60 63 64; do
    printf 'zoom %2d px/block : ' "$z"
    ZOOM=$z sh tools/bin/holecheck.sh 2>/dev/null | tail -1
done
