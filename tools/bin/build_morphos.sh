#!/bin/sh
# Cross-build the MorphOS/PowerPC binary.
#
#   tools/bin/build_morphos.sh
#
# The MorphOS counterpart of build.sh, and deliberately the same shape: same
# ROOT discovery, same compile helper, same one-object-set-per-variant
# arrangement, same refusal to strip. Read that script first; this one differs
# only where the target does.
#
# Toolchain: ppc-morphos-gcc-9 with the MorphOS SDK at /gg. Nothing else - no
# vasm, no NDK, and NO VENDORED CYBERGRAPHX HEADERS. cybergraphics.library's
# includes are part of the MorphOS SDK (/gg/os-include/cybergraphx/), so the
# "you must supply your own CGX headers" rule that governs the 68k RTG builds
# does not apply here and native/cgx-include/ is never referenced.
#
# THE FLAGS, and how they differ from the Amiga's:
#
#   -O2                        NOT -O1. The 68k build is pinned to -O1 because
#                              bebbo's GCC 6.5 breaks C++ exception unwinding
#                              above it. That is a defect of that compiler, not
#                              a property of this code, and GCC 9.5 for MorphOS
#                              does not have it.
#   no -mcpu, no -msoft-float  PowerPC has an FPU. The engine still does not
#                              use one - it is fixed point from top to bottom,
#                              which is why its numbers come out identical to
#                              the Amiga's - but there is no soft-float
#                              multilib to steer around here.
#   -noixemul                  as on the Amiga.
#   never strip                the same rule and very nearly the same reason:
#                              an unstripped binary is what turns a PC in a
#                              crash log into a function name.
set -e

# THE REPOSITORY ROOT. This script lives in <root>/tools/bin, so it works out
# its own tree exactly as build.sh does.
ROOT=$(cd "$(dirname "$0")/../.." 2>/dev/null && pwd)
NATIVE="$ROOT/native"
TOOLS="$ROOT/tools"
OBJ="$ROOT/build/morphos/obj"
OUT="$ROOT/build/morphos"

GCC=ppc-morphos-gcc-9

command -v "$GCC" >/dev/null 2>&1 || {
    echo "build_morphos: $GCC not found - install the MorphOS cross toolchain"
    exit 1
}

mkdir -p "$OBJ" "$OUT"

# THE SCREEN. 640x480, RENDERED - not 320x240 doubled.
#
# The Amiga's own gta-rtg480 renders 320x240 and scales it up, because "a 68020
# cannot rasterise 640x480 at a playable rate, so this way the picture is sharp
# rather than slow". That trade does not exist here: 640x480 is four times the
# pixels and the slowest machine MorphOS runs on is orders of magnitude past a
# 68020. So the picture is drawn at full resolution and GTA_SCALE2X is NOT
# defined. 200 lines was an AGA screen's shape and there is no AGA here either.
SCREEN="-DGTA_SCREEN_W=640 -DGTA_SCREEN_H=480"

OPT="-O2"
COMMON="$OPT -noixemul"
INCS="-I$NATIVE"
CFLAGS="$COMMON $INCS -Wall"

OBJS=""

compile() {
    src="$1"
    out="$OBJ/$(basename "$src" .c).o"
    echo "  CC  $(basename "$src")"
    $GCC $CFLAGS -c "$NATIVE/$src" -o "$out"
    OBJS="$OBJS $out"
}

echo "--- compiling ---"
# The engine, from the SAME SOURCES the Amiga build uses, unmodified. It was
# already portable: fixed point with no floating point anywhere, and GTA's
# little-endian data files read a byte at a time rather than by casting a
# struct over them, because the same code has to build for the big-endian 68k
# and for the host test harness. PowerPC is big-endian too and got that free.
#
# KEEP THIS LIST IN STEP WITH build.sh. Writing it out rather than globbing is
# what makes a new engine file a link error here instead of a mystery: v0.0.3
# added gta_prefs.c, gta_sfx.c and gta_weapon.c, and the missing symbols named
# all three the first time this script met it. A wildcard would have picked
# them up silently - along with amiga_trap.c and the c2p, which is the failure
# this list exists to prevent.
compile gta_tiles.c
compile gta_render.c
compile gta_hud.c
compile gta_trig.c
compile gta_player.c
compile gta_car.c
compile gta_nav.c
compile gta_vehphys.c
compile gta_peds.c
compile gta_weapon.c
compile gta_route.c
compile gta_traffic.c
compile gta_map.c
compile gta_prefs.c
compile gta_sfx.c

# THE PLATFORM LAYER, and it is the only part of the port that changes.
#
# native/amiga_gfx.c is NOT built. It carries four display backends and three
# of them are 68k to the bone: contiguous Chip RAM bitplanes, Kalms'
# chunky-to-planar in 68020 assembler, EHB's hardware half-brights,
# WritePixelArray8. native/morphos_gfx.c implements the same amiga_gfx.h
# contract using the RTG path, which is the one that survives the move - on
# MorphOS every screen is an RTG screen, so the chunky buffer the renderer
# writes into is already the display format and the c2p disappears entirely.
#
# Nor are these, and none of them is "not needed yet":
#   amiga_trap.c    a 68k supervisor-mode exception handler in Motorola
#                   assembler, installed into Task->tc_TrapCode. There is no
#                   such frame layout on PowerPC and MorphOS reports crashes
#                   itself.
#   fp_single.c     __mulsf3/__divsf3 replacements for Kickstart 3.1's broken
#   fp_conv.c       mathieeesingbas.library on FPU-less 68k machines.
#   libnix_fixes.c  a fix for libnix's wmemcpy. Not libnix here.
#   amiga_startup.c the AGA-or-RTG startup requester; this build has one
#                   display path, and nothing calls it.
#   the four .s     chunky-to-planar. There are no bitplanes to convert to.
compile morphos_gfx.c
compile amiga_uclock.c

# ONE BINARY, NAMED LIKE ITS SIBLINGS.
#
# The Amiga ships gta-aga, gta-rtg240 and gta-rtg480 - "gta-" plus whatever
# makes that build different. This build's difference is the machine, so it is
# gta-morphos, and a drawer holding all four says which is which without a
# README. No .exe: nothing on MorphOS carries an executable extension.
#
# Only gta_main.c reads GTA_SCREEN_W/H, so everything above is compiled once
# and none of it has to know what size the screen is.
echo "--- variant ---"
echo "  CC  gta_main.c (gta-morphos)"
$GCC $CFLAGS $SCREEN -c "$NATIVE/gta_main.c" -o "$OBJ/gta_main.o"
$GCC $COMMON -o "$OUT/gta-morphos" "$OBJ/gta_main.o" $OBJS -lm
echo "  LD  build/morphos/gta-morphos"

# THE TILE BAKER, FOR MORPHOS.
#
# The player supplies their own GTA data and converts it themselves - we ship
# no derived art - so the baker has to be a MorphOS binary too. tools/gtabake.c
# is portable C89 and needs only the style, tile and sound readers, not the
# engine. Source list mirrors build.sh's.
echo "--- tile baker ---"
$GCC $CFLAGS -o "$OUT/gtabake" \
    "$TOOLS/gtabake.c" \
    "$NATIVE/gta_style.c" "$NATIVE/gta_tiles.c" \
    "$NATIVE/gta_car.c" "$NATIVE/gta_trig.c" "$NATIVE/gta_sfx.c" -lm
echo "  LD  build/morphos/gtabake"

# THE SETTINGS EDITOR.
#
# Carried over because it writes the file the game reads, and because its
# command line (`gtaprefs SHOW`) is what works on a machine whose display is
# the thing being configured.
#
# ITS GRAPHICS SETTING MEANS LESS HERE THAN ON THE AMIGA: it picks between AGA,
# RTG and a Workbench window, and MorphOS has only the RTG path - gta_main.c
# reads the setting, says so, and opens RTG regardless. Audio is recorded and
# unused on both targets, since no version of this port plays sound yet.
echo "--- settings editor ---"
$GCC $CFLAGS -o "$OUT/gtaprefs" "$TOOLS/gtaprefs.c" "$NATIVE/gta_prefs.c" -lm
echo "  LD  build/morphos/gtaprefs"

ls -la "$OUT/gta-morphos" "$OUT/gtabake" "$OUT/gtaprefs"
echo "--- NOT stripped, on purpose (see the header of this script) ---"
