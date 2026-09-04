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
compile gta_score.c
compile gta_weapon.c
compile gta_pickup.c
compile gta_font.c
compile gta_text.c
compile gta_route.c
compile gta_traffic.c
compile gta_map.c
# The settings file, shared with the external editor below. No Amiga
# headers in it, so it also builds on the host and for the PowerPC tree.
compile gta_prefs.c
# The sound bank reader. Nothing plays yet - see native/gta_sfx.h.
compile gta_sfx.c

# The platform layer, carried over from openttd_amiga_68k via Amiga_OpenXCOM.
compile amiga_gfx.c
compile amiga_startup.c
compile amiga_uclock.c
compile amiga_trap.c
compile amiga_watchdog.c
compile libnix_fixes.c
compile fp_conv.c
compile fp_single.c

echo "--- assembling ---"
assemble c2p_glue.s
assemble amiga_span_blit.s

# ONE BINARY. THERE USED TO BE THREE, AND THAT WAS THE BUG.
#
# gta-aga, gta-rtg240 and gta-rtg480 were the same program built three times
# with different -D flags on gta_main.c: the screen width, the height, and
# whether the picture is doubled on the way out. Nothing else differed. So the
# archive carried three copies of the game, the player had to know which icon
# was for their machine, and picking wrong looked like a broken port.
#
# gtaprefs already chooses the display path, so since v0.0.4 it chooses the
# size too and the game reads both out of gta.prefs at start-up. The rig can
# still switch size without a GUI: `screen 200|240|480` in opts.txt.
#
# Work:backend.txt still overrides the backend at runtime.

echo "--- the game ---"
echo "  CC  gta_main.c"
m68k-amigaos-gcc $CFLAGS -c "$NATIVE/gta_main.c" -o "$OBJ/gta_main.o"
m68k-amigaos-gcc $COMMON -o "$OUT/AmiGTA" "$OBJ/gta_main.o" $OBJS -lm
echo "  LD  build/AmiGTA"

# THE SETTINGS EDITOR.
#
# A separate program on purpose: it edits the settings that decide whether the
# game can open a display at all, so it cannot live behind a menu drawn by that
# display. See the header of tools/gtaprefs.c.
#
# It is the only thing in this repository that links gadtools.library. That is
# also why it is built here rather than folded into the game: an Intuition GUI
# has no business inside a 68020 game loop's binary.
echo "--- settings editor ---"
m68k-amigaos-gcc $CFLAGS -o "$OUT/gtaprefs"     "$ROOT/tools/gtaprefs.c" "$NATIVE/gta_prefs.c"
echo "  LD  build/gtaprefs"

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
    "$NATIVE/gta_car.c" "$NATIVE/gta_trig.c" "$NATIVE/gta_sfx.c" -lm
echo "  LD  build/gtabake"

# The three old names are removed rather than left lying in build/, because a
# stale binary from before the merge would still deploy, still run, and still
# be reported as "the new build" - which is exactly the class of mistake that
# cost an evening when a stale gta-aga was tested against a fresh source.
rm -f "$OUT/gta-aga" "$OUT/gta-rtg240" "$OUT/gta-rtg480" "$OUT/gta-rtg"

ls -la "$OUT/AmiGTA" "$OUT/gtabake" "$OUT/gtaprefs"
echo "--- NOT stripped, on purpose (see the header of this script) ---"
