#!/bin/sh
# pedsrun.sh - the pedestrian regression: host build, then `gtadump peds` at
# three places x two seeds x standing/walking camera, 9000 ticks each. One line
# a run: stuck reports (gta_ped.blk_x) and pair-ticks within four pixels.
# Full logs in out/peds/. PROGRESS.md 184.
ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i
cd /mnt/i/GITHUB/Amiga_GTA || exit 1
sh tools/bin/build_host.sh release 2>&1 | grep -i " error\|gta_peds.c.*warning\|gtadump.c.*warning: unused"
echo HOST_BUILD_DONE
MAP=/mnt/c/temp/amiga_gta/work/GTADATA/nyc.cmp
TIL=build/data/style001.til
mkdir -p out/peds
for place in "62 60" "105 119" "64 64"; do
  for seed in 777 12345; do
    for w in 0 1; do
      set -- $place
      tag="$1_$2_s${seed}_w$w"
      if [ $w = 1 ]; then
        GTA_PEDS_WALK=1 build/host/gtadump peds $MAP $TIL $1 $2 9000 $seed > out/peds/$tag.log 2>&1
      else
        build/host/gtadump peds $MAP $TIL $1 $2 9000 $seed > out/peds/$tag.log 2>&1
      fi
      echo "$tag: $(grep -a '^peds: after' out/peds/$tag.log | sed 's/peds: after 9000 ticks - //')"
    done
  done
done
echo PEDSRUN_DONE
