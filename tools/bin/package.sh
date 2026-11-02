#!/bin/sh
# Build the release archives.
#
#   tools/bin/package.sh
#
# Produces, on the SSD at C:/temp/amiga_gta/dist/ - see DIST below for why
# it is not on I:. (Forward slashes on purpose: a sed through the Bash tool
# eats doubled backslashes, which is how this line first came out as
# "C:<tab>empmiga_gtadist".)
#
#   AmiGTA-v0.7.0.zip    the game, the settings editor, the tile converter,
#                        icons, run
#   AmiGTA-v0.7.0.lha    the same, in the format an Amiga unpacks natively
#
# NO GAME DATA IS SHIPPED, and nothing derived from any. The player converts
# their own style001.gry with the bundled gtabake - see LICENSING.md line 88.
#
# WHY THE ARCHIVES ARE BUILT FROM WSL AND NOT FROM POWERSHELL: path separators.
# A zip made by a Windows tool can carry backslashes in its member names, and an
# Amiga unpacker then creates one file called "GTADATA\style001.til" instead of
# a directory with a file in it. zip(1) and lha(1) here write forward slashes,
# which is what every unpacker on every platform expects.
#
# The staging directory is built fresh each time and the runtime's working
# files - frame dumps, logs, the backend override, PNG captures - are left out
# deliberately: they are debris from testing, not part of a release.
set -e

ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i

ROOT=/mnt/i/GITHUB/Amiga_GTA
WORK=/mnt/c/temp/amiga_gta/work
# THE ARCHIVES ARE BUILT ON THE SSD, NOT ON THE NETWORK DRIVE.
#
# I: is a network share. Building a staging tree there and deleting it again
# left the directory in a state where WSL saw `d?????????` and Windows answered
# "Access is denied" to Test-Path - unrecoverable without touching it from
# somewhere else. The runtime already lives on C: for the same class of reason
# (see CLAUDE.md); the archives now do too.
#
#   Windows: C:\temp\amiga_gta\dist\
DIST=/mnt/c/temp/amiga_gta/dist
# 0.0.4: weapons, car damage, the collision rewrite - and ONE GAME BINARY
# instead of three. gta-aga / gta-rtg240 / gta-rtg480 were the same program
# built with different screen sizes; the size is a setting in gtaprefs now, so
# the archive carries `AmiGTA` and nothing else has to be chosen by icon.
VER=v0.7.0
STAGE="$DIST/AmiGTA-$VER"

# drvfs (the I: mount) sometimes reports EEXIST from `mkdir -p` on a directory
# that is already there, which `set -e` then turns into a failed build. Clear
# the CONTENTS and tolerate the directory itself.
rm -rf "$DIST"/* 2>/dev/null || true
mkdir -p "$STAGE" 2>/dev/null || true
[ -d "$STAGE" ] || { echo "package: cannot create $STAGE"; exit 1; }

# --- the binaries and what they need to start -------------------------------
# gtaprefs ships WITH the game and not as an extra download, because the case
# it exists for is a player whose machine draws nothing usable. Telling them to
# go and fetch a second archive at that point would be the same fault as
# telling them to create backend.txt in a text editor, which is what it
# replaces.
# THE BINARIES COME FROM WHERE THE BUILD PUTS THEM. Since the build moved to
# C: (tools/bin/cbuild.sh) $ROOT/build holds September's files; packaging from
# there would ship a stale game. GTA_BUILD overrides, the C: build is the
# default when it exists.
BIN=${GTA_BUILD:-/mnt/c/temp/amiga_gta_build/build}
[ -f "$BIN/AmiGTA" ] || BIN=$ROOT/build
MOSBIN=${MOS_BUILD:-/mnt/c/temp/amiga_gta/mos-build}/AmiGTA_MorphOS
echo "package: binaries from $BIN"
for b in AmiGTA gtaprefs; do
    [ -f "$BIN/$b" ] || { echo "package: missing $BIN/$b - run build.sh"; exit 1; }
    cp "$BIN/$b" "$STAGE/$b"
    [ -f "$WORK/$b.info" ] && cp "$WORK/$b.info" "$STAGE/$b.info"
done
cp "$ROOT/winuae/work-template/run" "$STAGE/run"

# THE README IS GENERATED, WITH CRLF LINE ENDINGS.
#
# LF-only is unreadable in Notepad on the PC - one enormous line - and this
# archive is handed to people on both machines. CRLF is the one encoding both
# accept: Windows wants it, and AmigaDOS `type`, `More`, `MuchMore`, ED and
# CygnusEd all cope with the extra CR. Plain ASCII, hard-wrapped at 76 columns
# so it reads in an 80-column shell.
#
# The text lives in tools/bin/readme_gen.py so it can be written once and kept
# out of the middle of a shell script.
python3 "$ROOT/tools/bin/readme_gen.py" "$STAGE/README.txt"

# THE TILE BAKER GOES IN. THE TILES DO NOT.
#
# An earlier version of this script shipped style001.til, and a second archive
# with the whole of GTADATA in it. Both redistribute the player's game - which
# LICENSING.md line 88 forbids in as many words: "Nothing from pc/, dos/ or the
# ISO is ours or redistributable. The port must require the player to supply
# their own GTA data files."
#
# So the archive carries the CONVERTER instead. gtabake cross-compiles for the
# Amiga from the same portable C89 that bakes tiles on the PC, and it produces a
# byte-identical file - checked with cmp against build/data/style001.til. The
# player converts their own art on their own machine and nothing derived from
# the game ever leaves this one.
if [ -f "$BIN/gtabake" ]; then
    cp "$BIN/gtabake" "$STAGE/gtabake"
    # Its own icon, like the other two - deploy.sh generated it. Without this
    # the converter was the one program in the drawer with no icon at all, so
    # a Workbench-only player could not start the one step the archive
    # actually requires of them.
    if [ -f "$WORK/gtabake.info" ]; then
        cp "$WORK/gtabake.info" "$STAGE/gtabake.info"
    fi
else
    echo "package: no build/gtabake - run build.sh"
    exit 1
fi

# THE MUSIC EXTRACTOR, for the same reason and with the same rule: no audio
# derived from the game leaves this machine either. The game converts the
# player's own soundtrack on its first run, with a progress bar; this is that
# converter as a Shell program, for one track at a time or for looking at what
# came out (-info). Not fatal if it is missing - a player without it still
# gets the first-run extraction.
if [ -f "$BIN/gtaiff" ]; then
    cp "$BIN/gtaiff" "$STAGE/gtaiff"
    echo "package: gtaiff"
fi

# THE GTADATA DRAWER SHIPS, EMPTY.
#
# It costs nothing, removes a step, and removes a chance to mistype the name.
# An empty directory is not reliable in an archive - some unpackers skip
# zero-length directory entries - so it carries a note, which also tells
# whoever opens the drawer exactly which two files belong in it.
mkdir -p "$STAGE/GTADATA"
python3 "$ROOT/tools/bin/readme_gen.py" --datanote \
    "$STAGE/GTADATA/_PUT_YOUR_GTA_FILES_HERE.txt"

# --- archive ----------------------------------------------------------------
# TWO ARCHIVES SINCE v0.7.0, one per CPU:
#   AmiGTA-$VER-68k      AmigaOS 3.1+ and AROS 68k (and MorphOS's emulation)
#   AmiGTA-$VER-MorphOS  the same drawer with AmiGTA swapped for the native
#                        PPC build; the 68k tools stay, MorphOS runs them.
# Both unpack to a drawer of the same name, AmiGTA-$VER.
# lha(1) here takes the archive name FIRST and has no -q: passing one makes it
# the archive name and leaves a file literally called "-q" behind, which is
# exactly what the first run of this script did.
cd "$DIST"
zip -r -q "AmiGTA-$VER-68k.zip" "AmiGTA-$VER"
lha a "AmiGTA-$VER-68k.lha" "AmiGTA-$VER" >/dev/null 2>&1 || echo "package: lha failed"
if [ -f "$MOSBIN" ]; then
    file "$MOSBIN" | grep -q PowerPC || { echo "package: $MOSBIN is not PowerPC"; exit 1; }
    cp "$STAGE/AmiGTA" "$DIST/AmiGTA.68k"
    cp "$MOSBIN" "$STAGE/AmiGTA"
    zip -r -q "AmiGTA-$VER-MorphOS.zip" "AmiGTA-$VER"
    lha a "AmiGTA-$VER-MorphOS.lha" "AmiGTA-$VER" >/dev/null 2>&1 || echo "package: lha (MorphOS) failed"
    mv "$DIST/AmiGTA.68k" "$STAGE/AmiGTA"     # --with-data below stays 68k
else
    echo "package: no $MOSBIN - run build-morphos.sh; MorphOS archive NOT built"
fi

# --- and, ONLY ON REQUEST, the same thing with the data in it ---------------
#
# `--with-data` adds AmiGTA-$VER-full.zip / .lha, which contain the player's
# GTADATA - the converted tiles and the map.
#
# THESE TWO ARE NOT RELEASE ARTEFACTS AND MUST NEVER BE PUBLISHED. They
# redistribute Grand Theft Auto, which LICENSING.md line 88 forbids and which
# the developer himself stopped an earlier version of this script from doing.
# They exist for one purpose: handing a ready-to-run drawer to somebody who
# already owns the game, without walking them through gtabake first. Whoever
# receives one needs their own copy of GTA for it to be theirs to run.
#
# publish.sh refuses any file over 1 MB and any *.til or *.cmp, so these
# cannot reach the public repository by accident.
if [ "$1" = "--with-data" ]; then
    if [ -d "$WORK/GTADATA" ]; then
        # EXACTLY THE TWO FILES THE GAME READS, and not the other 179.
        # Copying the drawer whole made a 30 MB archive out of a 2.3 MB need -
        # every other city, every sound bank, the lot. Shipping somebody
        # else's game at all is the thing to keep small.
        # (2026-09-30: the game reads more than those two by now - the level
        # script, the texts, the fonts, the title screen, the effects, the
        # voices and the converted radio. The first group is required; the
        # music is optional and simply copied when it is there.)
        for f in style001.til nyc.cmp mission.ini english.fxt front.fnt \
                 level001.snd vocalcom.snd big1.fon f_mtext.fon \
                 missmul1.fon pager1.fon score1.fon; do
            [ -f "$WORK/GTADATA/$f" ] || {
                echo "package: --with-data needs $WORK/GTADATA/$f"; exit 1; }
            cp "$WORK/GTADATA/$f" "$STAGE/GTADATA/$f"
        done
        for f in title.mus title.8svx police.mus radio1.mus radio2.mus \
                 radio3.mus sanb.cmp miami.cmp style002.til style003.til \
                 level002.snd level003.snd; do
            [ -f "$WORK/GTADATA/$f" ] && cp "$WORK/GTADATA/$f" "$STAGE/GTADATA/$f"
        done
        # The note about supplying your own files is wrong once they are here.
        rm -f "$STAGE/GTADATA/_PUT_YOUR_GTA_FILES_HERE.txt"
        zip -r -q "AmiGTA-$VER-full.zip" "AmiGTA-$VER"
        lha a "AmiGTA-$VER-full.lha" "AmiGTA-$VER" >/dev/null 2>&1 \
            || echo "package: lha (full) failed"
        echo "package: ALSO built -full.zip / -full.lha WITH GAME DATA."
        echo "package: those two are local only - do not publish them."
    else
        echo "package: --with-data asked for, but $WORK/GTADATA is not there"
        exit 1
    fi
fi

cd "$DIST"
rm -rf "AmiGTA-$VER"
ls -la "$DIST"

echo "--- member names (must use / and never \\) ---"
unzip -l "AmiGTA-$VER-68k.zip" | head -12
[ -f "AmiGTA-$VER-MorphOS.zip" ] && unzip -l "AmiGTA-$VER-MorphOS.zip" | head -12
