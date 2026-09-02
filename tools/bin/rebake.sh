#!/bin/sh
# Re-bake the tile set on the host (WSL) and prove it: the object table, the
# bake summary, and holecheck. Run as
#
#     wsl sh /mnt/i/GITHUB/Amiga_GTA/tools/bin/rebake.sh
#
# Paths live in here rather than on the command line because the Git Bash
# that launches wsl rewrites /mnt/... arguments (LEFTOFF.md, the harness
# hazards). /mnt/i drops out of WSL now and then - remount first.
ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i

ROOT=/mnt/i/GITHUB/Amiga_GTA
GRY=/mnt/c/temp/amiga_gta/work/GTADATA/style001.gry
TIL=$ROOT/build/data/style001.til

cd "$ROOT" || exit 1
mkdir -p build/data out

if [ ! -f "$GRY" ]; then
    echo "rebake: no $GRY"; exit 1
fi

echo "--- objinfo ---"
build/host/gtadump objinfo "$GRY" > out/objinfo.txt || exit 1
head -3 out/objinfo.txt
grep -E "^0x(0d|4a|1f|4b|54|55|2e|18|3f|40|4d|4e|4f|50|51) " out/objinfo.txt

echo "--- bake ---"
build/host/gtabake "$GRY" "$TIL" || exit 1

echo "--- holecheck (must print 0) ---"
sh tools/bin/holecheck.sh 2>&1 | tail -3
