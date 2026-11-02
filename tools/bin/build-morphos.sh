#!/bin/bash
# THE NATIVE MORPHOS (PowerPC) BUILD - AmiGTA_MorphOS, one extra program beside
# the 68k AmiGTA (which itself runs on AmigaOS, AROS and MorphOS's 68k
# emulation). Following I:\GITHUB\!instrukcje_kompilacji_68K_AROS_i_MorphOS\
# 45-MorphOS-natywny-PPC-2D.md and AmiSC's build-morphos.sh. Run it from the
# Ubuntu-24 WSL distro (glibc >= 2.36), which holds morphos-cross (GCC 15.2,
# MorphOS SDK 3.20), as root:
#
#   wsl -d Ubuntu-24 -u root -- bash /mnt/c/temp/amiga_gta_build/py/bmos.sh
#
# Same C sources as tools/bin/build.sh, with these differences:
#  * no 68k assembly - c2p_glue.s gives way to native/mos_stubs.c (MorphOS has
#    no AGA; the RTG deep path is always taken), amiga_span_blit.s is unused;
#  * no fp_single.c / fp_conv.c (the 68k soft-float fixes) and no
#    libnix_fixes.c, amiga_trap.c (68k only, and the trap is never armed);
#  * -fsigned-char: PowerPC's plain char is unsigned, the game was written
#    and measured on signed char (68k, x86);
#  * -fno-strict-aliasing -fwrapv: the code only ever met -O1 on 68k and the
#    host; -O2 here must not exploit punning or signed overflow;
#  * __MORPHOS__ (the compiler's) puts "AmiGTA PPC" on the title bar.
set -e
R=/mnt/i/GITHUB/Amiga_GTA
N=$R/native
B=${BUILD:-$HOME/gtamos}
OUT=${DEPLOY_DIR:-/mnt/c/temp/amiga_gta/mos-build}
export PATH=$HOME/.morphos-cross/current/bin:$PATH
ls /mnt/i/GITHUB >/dev/null

rm -rf "$B"; mkdir -p "$B/obj"
OPT="-O2 -fno-strict-aliasing -fwrapv -fsigned-char -noixemul"
CF="$OPT -I$N -w"

SRC="gta_style.c gta_tiles.c gta_render.c gta_hud.c gta_trig.c gta_player.c
gta_car.c gta_nav.c gta_vehphys.c gta_peds.c gta_score.c gta_weapon.c
gta_pickup.c gta_script.c gta_script_run.c gta_front.c gta_font.c gta_text.c
gta_route.c gta_traffic.c gta_train.c gta_service.c gta_map.c gta_prefs.c
gta_sfx.c gta_iff.c gta_audio.c amiga_adpcm.c amiga_audio.c amiga_gfx.c
amiga_startup.c amiga_uclock.c amiga_watchdog.c amiga_wblaunch.c mos_stubs.c
gta_main.c"
OBJS=""
for f in $SRC; do
	[ -f "$N/$f" ] || continue
	o="$B/obj/${f%.c}.o"
	echo "  CC  $f"
	ppc-morphos-gcc $CF -c "$N/$f" -o "$o"
	OBJS="$OBJS $o"
done
echo "  LD  AmiGTA_MorphOS"
ppc-morphos-gcc $OPT $OBJS -o "$B/AmiGTA_MorphOS" -lm
file "$B/AmiGTA_MorphOS" | grep -q PowerPC || { echo "*** not a PowerPC executable"; file "$B/AmiGTA_MorphOS"; exit 1; }
strings "$B/AmiGTA_MorphOS" | grep -q "AmiGTA PPC" || { echo "*** no 'AmiGTA PPC' title in it"; exit 1; }
mkdir -p "$OUT"
cp "$B/AmiGTA_MorphOS" "$OUT/"
ls -l "$OUT/AmiGTA_MorphOS"
echo MOS_BUILD_OK
