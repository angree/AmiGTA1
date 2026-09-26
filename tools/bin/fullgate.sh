#!/bin/sh
# THE TRAFFIC GATE for the one model the game has (218): the four drive
# sites x seeds 1-3, 12000 ticks each, the fleet at its default 20.
# Run inside WSL:  wsl sh /mnt/i/GITHUB/Amiga_GTA/tools/bin/fullgate.sh
ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i
sh /mnt/i/GITHUB/Amiga_GTA/tools/bin/cbuild.sh host 2>&1 | grep -i " error\|HOST_BUILD"
B=/mnt/c/temp/amiga_gta_build
D=/mnt/c/temp/amiga_gta/work/GTADATA
cd $B || exit 1
mkdir -p out
for seed in ${SEEDS:-1 2 3}; do
  for s in "64 64" "61 52" "108 228" "106 114"; do
    set -- $s
    L=out/full${seed}_$1_$2.log
    timeout 900 ./build/host/gtadump drive $D/nyc.cmp $D/style001.til $1 $2 out/d 12000 50 $seed > $L 2>&1
    echo "seed $seed $1,$2: $(grep 'flow -' $L | cut -c15-120) | $(grep 'PASSED\|FAILED' $L | cut -c8-20) | $(grep 'after 12000' $L | cut -c30-100)"
  done
done
