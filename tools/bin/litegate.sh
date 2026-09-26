#!/bin/sh
# THE SIMPLE-TRAFFIC GATE (217): the four drive sites x seeds 1-3 with
# GTA_LITE=1, then a wreck parked in the level-1 lane at (106,119).
# Run inside WSL:  wsl sh /mnt/i/GITHUB/Amiga_GTA/tools/bin/litegate.sh
# Builds the host tools on C: first (tools/bin/cbuild.sh host).
ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i
sh /mnt/i/GITHUB/Amiga_GTA/tools/bin/cbuild.sh host 2>&1 | grep -i " error\|HOST_BUILD"
B=/mnt/c/temp/amiga_gta_build
D=/mnt/c/temp/amiga_gta/work/GTADATA
cd $B || exit 1
mkdir -p out
for seed in ${SEEDS:-1 2 3}; do
  for s in "64 64" "61 52" "108 228" "106 114"; do
    set -- $s
    L=out/lites${seed}_$1_$2.log
    GTA_LITE=1 timeout 900 ./build/host/gtadump drive $D/nyc.cmp $D/style001.til $1 $2 out/d 12000 50 $seed > $L 2>&1
    echo "seed $seed $1,$2: $(grep 'flow -' $L | cut -c15-120) | $(grep -c 'lite stuck' $L) stuck | $(grep 'PASSED\|FAILED' $L | cut -c1-20)"
  done
done
for seed in ${SEEDS:-1 2 3}; do
  L=out/wreck$seed.log
  GTA_WRECK="106 119 0 2" GTA_LITE=1 timeout 900 ./build/host/gtadump drive $D/nyc.cmp $D/style001.til 106 119 out/d 12000 50 $seed > $L 2>&1
  echo "wreck seed $seed: $(grep 'flow -' $L | cut -c15-120) | $(grep -c 'lite stuck' $L) stuck | $(grep 'OVERTAKES' $L | cut -c80-200)"
done
