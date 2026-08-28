#!/bin/sh
# Build every host-side tool.
#
#   tools/bin/build_host.sh          release + AddressSanitizer builds
#   tools/bin/build_host.sh release  release only (faster)
#
# Run it from WSL, or let it re-enter WSL itself:
#   wsl sh /mnt/i/GITHUB/Amiga_GTA/tools/bin/build_host.sh
#
# TWO BINARIES, ALWAYS. The ASan build is not optional politeness: the very
# first bug in native/gta_map.c was a heap overrun that could only ever happen
# on the host, because `unsigned long` is 4 bytes on m68k-amigaos and 8 here.
# ASan found it in seconds; on the Amiga it would have been silent corruption.
# Cheaper still, the second lesson from that morning: after fixing it only the
# ASan binary was rebuilt, so the release build kept crashing on stale code.
# Building both, every time, removes that whole class of confusion.
#
# -std=c89 -pedantic on purpose: everything under native/ has to compile with
# bebbo's m68k-amigaos GCC 6.5, which is far stricter about C89 aggregates than
# the host compiler. Catching that here costs nothing; catching it after a
# 20-minute cross-build costs an afternoon.
set -e

# /mnt/i drops out of WSL regularly.
ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i

# THE REPOSITORY ROOT. This script lives in <root>/tools/bin, so it can
# find its own tree; the fallback is the author's own path, so the
# established way of calling this by absolute path still works.
ROOT=$(cd "$(dirname "$0")/../.." 2>/dev/null && pwd)
[ -d "$ROOT/native" ] || ROOT=/mnt/i/GITHUB/Amiga_GTA
cd "$ROOT"
mkdir -p build/host out

WARN="-Wall -Wextra -std=c89 -pedantic -Wno-overlength-strings"
SRC_COMMON="native/gta_style.c native/gta_map.c native/gta_tiles.c native/gta_render.c native/gta_hud.c native/gta_trig.c native/gta_player.c native/gta_car.c native/gta_nav.c native/gta_route.c native/gta_traffic.c native/gta_vehphys.c native/gta_peds.c"

echo "--- gtadump (release) ---"
gcc -O2 $WARN -o build/host/gtadump tools/gtadump.c $SRC_COMMON

echo "--- vehruler (release) ---"
gcc -O2 $WARN -o build/host/vehruler tools/vehruler.c $SRC_COMMON

echo "--- gtabake (release) ---"
gcc -O2 $WARN -o build/host/gtabake tools/gtabake.c $SRC_COMMON

if [ "$1" != "release" ]; then
    echo "--- gtadump (asan) ---"
    gcc -g -O0 -fsanitize=address -std=c89 \
        -o build/host/gtadump_asan tools/gtadump.c $SRC_COMMON
    echo "--- gtabake (asan) ---"
    gcc -g -O0 -fsanitize=address -std=c89 \
        -o build/host/gtabake_asan tools/gtabake.c $SRC_COMMON
fi

echo "--- built ---"
ls -la build/host/
