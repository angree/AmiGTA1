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

# THE SCREEN IS A SETTING NOW, not a build. Since v0.0.4 the size is chosen at
# run time from gta.prefs, and where MorphOS differs is in what AUTO means -
# 640x480 really rasterised, decided in gta_prefs_screen_size() and explained
# there. All four sizes stay selectable with `gtaprefs SCREEN=...`.
#
# These two only seed g_screen_w/h for the moment before the settings are read.
# They match the AUTO default so that moment is not a different size, and
# GTA_SCALE2X is deliberately NOT defined: the doubling exists because a 68020
# cannot rasterise four times the pixels at a playable rate, which is a
# statement about that CPU and not about this one.
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
compile gta_score.c
compile gta_weapon.c
compile gta_pickup.c
compile gta_script.c
compile gta_script_run.c
compile gta_front.c
compile gta_font.c
compile gta_text.c
compile gta_route.c
compile gta_traffic.c
compile gta_map.c
compile gta_prefs.c
compile gta_sfx.c
compile gta_iff.c
compile gta_audio.c

# amiga_adpcm.c is named for the Amiga and is not OF it: its header says
# "plain C, stdio only - NO Amiga <proto/*> headers", and it decodes the sound
# bank's ADPCM. It builds here unchanged.
compile amiga_adpcm.c

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
compile amiga_watchdog.c

# AUDIO IS AHI HERE, NOT PAULA.
#
# amiga_audio.c is NOT built. It does not drive an audio API, it drives the
# chipset: ADCMD_ALLOCATE over four hardware channels, sample data in Chip RAM
# "because Paula DMA reads nothing else", and a period register counted in the
# PAL colour clock. None of that is on a PowerPC machine. MorphOS does carry an
# audio.device for compatibility, so linking it would have produced something
# that might have made a noise and might have sat silent - and either way the
# wrong thing to ship on a system whose sound API is AHI.
#
# native/morphos_audio.c implements the same amiga_audio.h contract over AHI's
# low-level API, so gta_audio.c - the portable mixer the game actually talks
# to - is unchanged. Same arrangement as morphos_gfx.c against amiga_gfx.c.
#
# -O2 like everything else. build.sh pins amiga_audio.c to -O0 because bebbo's
# GCC 6.5 miscompiles read-after-call at -O1; that is a defect of that
# compiler, and this is a different file on a different one.
compile morphos_audio.c

# THE BINARY IS `AmiGTA-morphos`: upstream's name, plus which machine.
#
# v0.0.3 shipped gta-aga, gta-rtg240 and gta-rtg480, and this build was named
# gta-morphos to sit beside them. v0.0.4 collapsed those three into one
# `AmiGTA` - the screen became a setting instead of a binary - so that reason
# is gone and the name follows: same program, so the same name, with the one
# thing that still differs on the end.
#
# No .exe. Nothing on MorphOS carries an executable extension - a file is
# executable because of its protection bits - so a name ending in .exe would be
# the one file in the drawer that looked like it came off a PC.
echo "--- linking ---"
echo "  CC  gta_main.c"
$GCC $CFLAGS $SCREEN -c "$NATIVE/gta_main.c" -o "$OBJ/gta_main.o"
$GCC $COMMON -o "$OUT/AmiGTA-morphos" "$OBJ/gta_main.o" $OBJS -lm
echo "  LD  build/morphos/AmiGTA-morphos"

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

# THE MUSIC EXTRACTOR. Ships with the Amiga release (package.sh copies it), so
# it ships here: it is how the player turns their own copy's IFF audio into
# something the game reads, and it is portable C over gta_iff.c.
echo "--- music extractor ---"
$GCC $CFLAGS -o "$OUT/gtaiff" "$TOOLS/gtaiff.c" "$NATIVE/gta_iff.c" -lm
echo "  LD  build/morphos/gtaiff"

ls -la "$OUT/AmiGTA-morphos" "$OUT/gtabake" "$OUT/gtaprefs" "$OUT/gtaiff"
echo "--- NOT stripped, on purpose (see the header of this script) ---"
