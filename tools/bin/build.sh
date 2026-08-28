#!/bin/sh
# Cross-build the Amiga binary.
#
#   wsl sh -c 'sh /mnt/i/GITHUB/Amiga_GTA/tools/bin/build.sh'
#
# Toolchain: bebbo amiga-gcc 6.5.0b at /opt/amiga/bin, inside WSL.
#
# THE FLAGS ARE NOT STYLE. Every one of them cost real time in the sibling
# ports; see CLAUDE.md for the full list of toolchain defects.
#
#   -mcpu=68020 -msoft-float   NOT -m68040: that silently selects the 68881
#                              multilib even with -msoft-float, and the result
#                              needs an FPU that the target machine lacks.
#   -O1                        NOT -O2: it breaks C++ exception unwinding. We
#                              have no C++ yet, but the flag stays consistent
#                              with the ports this platform layer came from.
#   -noixemul                  libnix, not ixemul.
#   never -lpthread, never -lc they drag newlib in alongside libnix.
#   never strip                m68k-amigaos-strip produces a Hunk executable
#                              that halts the machine: black screen, no Guru,
#                              no output at all.
#
# fp_single.o must be linked AHEAD of -lm. It provides __mulsf3 / __divsf3,
# because Kickstart 3.1's mathieeesingbas.library has broken multiply and divide
# entries on machines without an FPU - they point into the function table
# itself, giving a Line-F exception and Guru 8000000B.
set -e

# /mnt/i drops out of WSL regularly.
ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i

# THE REPOSITORY ROOT. This script lives in <root>/tools/bin, so it can
# find its own tree; the fallback is the author's own path, so the
# established way of calling this by absolute path still works.
ROOT=$(cd "$(dirname "$0")/../.." 2>/dev/null && pwd)
[ -d "$ROOT/native" ] || ROOT=/mnt/i/GITHUB/Amiga_GTA
NATIVE="$ROOT/native"
OBJ="$ROOT/build/amiga/obj"
OUT="$ROOT/build"
NDK=/opt/amiga/m68k-amigaos/ndk-include

export PATH=/opt/amiga/bin:$PATH

mkdir -p "$OBJ" "$OUT"

CPU="-mcpu=68020 -msoft-float"
OPT="-O1 -fomit-frame-pointer"
COMMON="$CPU $OPT -noixemul"
INCS="-I$NATIVE -I$NATIVE/cgx-include"
CFLAGS="$COMMON $INCS -Wall"

OBJS=""

compile() {
    src="$1"
    out="$OBJ/$(basename "$src" .c).o"
    echo "  CC  $(basename "$src")"
    m68k-amigaos-gcc $CFLAGS -c "$NATIVE/$src" -o "$out"
    OBJS="$OBJS $out"
}

assemble() {
    src="$1"
    out="$OBJ/$(basename "$src" .s).o"
    echo "  AS  $(basename "$src")"
    # Kalms' routines are Motorola syntax, which vasm assembles directly. The
    # NDK assembler includes are not on vasm's default search path, hence -I.
    vasmm68k_mot -Fhunk -m68020 -no-opt -I"$NDK" -o "$out" "$NATIVE/$src"
    OBJS="$OBJS $out"
}

echo "--- compiling ---"
# Our own code first: plain C89, stdio only, no Amiga headers. If the cross
# compiler is going to complain about anything of ours, it complains here.
compile gta_tiles.c
compile gta_render.c
compile gta_hud.c
compile gta_trig.c
compile gta_player.c
compile gta_car.c
compile gta_nav.c
compile gta_vehphys.c
compile gta_peds.c
compile gta_route.c
compile gta_traffic.c
compile gta_map.c

# The platform layer, carried over from openttd_amiga_68k via Amiga_OpenXCOM.
compile amiga_gfx.c
compile amiga_startup.c
compile amiga_uclock.c
compile amiga_trap.c
compile libnix_fixes.c
compile fp_conv.c
compile fp_single.c

echo "--- assembling ---"
assemble c2p_glue.s
assemble amiga_span_blit.s

# THREE BINARIES, ONE SET OF OBJECTS.
#
# Only gta_main.c reads GTA_SCREEN_W/H, GTA_SCALE2X and GTA_DEFAULT_BACKEND, so
# everything else above is compiled once and shared between the three. If one
# variant misbehaves it cannot be because a different file was built for it.
#
#   gta-aga      320x200  AGA   the reference; every timing in the notes
#   gta-rtg240   320x240  RTG   the same picture, more of the city on screen
#   gta-rtg480   640x480  RTG   320x240 doubled at present time - see
#                               scale2x_rows() for why it is not rendered
#
# Work:backend.txt still overrides the backend at runtime in all three.

echo "--- variants ---"
variant() {
    name="$1"; shift
    echo "  CC  gta_main.c ($name)"
    m68k-amigaos-gcc $CFLAGS "$@" -c "$NATIVE/gta_main.c" \
        -o "$OBJ/gta_main_$name.o"
    m68k-amigaos-gcc $COMMON -o "$OUT/$name" "$OBJ/gta_main_$name.o" $OBJS -lm
    echo "  LD  build/$name"
}

variant gta-aga
variant gta-rtg240 -DGTA_SCREEN_W=320 -DGTA_SCREEN_H=240 \
        -DGTA_DEFAULT_BACKEND=AMIGAGFX_BACKEND_RTG
variant gta-rtg480 -DGTA_SCREEN_W=640 -DGTA_SCREEN_H=480 -DGTA_SCALE2X \
        -DGTA_DEFAULT_BACKEND=AMIGAGFX_BACKEND_RTG

# THE TILE BAKER, FOR THE AMIGA.
#
# The player supplies their own GTA data and converts it themselves - we ship
# no derived art. tools/gtabake.c is portable C89 with no host dependencies, so
# the same source that bakes tiles on the PC bakes them on the Amiga; it needs
# only the style and tile readers, not the whole engine.
echo "--- tile baker ---"
m68k-amigaos-gcc $CFLAGS -o "$OUT/gtabake" \
    "$ROOT/tools/gtabake.c" \
    "$NATIVE/gta_style.c" "$NATIVE/gta_tiles.c" \
    "$NATIVE/gta_car.c" "$NATIVE/gta_trig.c" -lm
echo "  LD  build/gtabake"

ls -la "$OUT/gta-aga" "$OUT/gta-rtg240" "$OUT/gta-rtg480" "$OUT/gtabake"
echo "--- NOT stripped, on purpose (see the header of this script) ---"
