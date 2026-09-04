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
#   AmiGTA-v0.2.0.zip    the game, the settings editor, the tile converter,
#                        icons, run
#   AmiGTA-v0.2.0.lha    the same, in the format an Amiga unpacks natively
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
VER=v0.2.0
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
for b in AmiGTA gtaprefs; do
    [ -f "$ROOT/build/$b" ] || { echo "package: missing build/$b - run build.sh"; exit 1; }
    cp "$ROOT/build/$b" "$STAGE/$b"
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
if [ -f "$ROOT/build/gtabake" ]; then
    cp "$ROOT/build/gtabake" "$STAGE/gtabake"
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
cd "$DIST"
zip -r -q "AmiGTA-$VER.zip" "AmiGTA-$VER"
# lha(1) here takes the archive name FIRST and has no -q: passing one makes it
# the archive name and leaves a file literally called "-q" behind, which is
# exactly what the first run of this script did.
lha a "AmiGTA-$VER.lha" "AmiGTA-$VER" >/dev/null 2>&1 || echo "package: lha failed"

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
        for f in style001.til nyc.cmp; do
            [ -f "$WORK/GTADATA/$f" ] || {
                echo "package: --with-data needs $WORK/GTADATA/$f"; exit 1; }
            cp "$WORK/GTADATA/$f" "$STAGE/GTADATA/$f"
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
unzip -l "AmiGTA-$VER.zip" | head -12
