#!/bin/sh
# Deploy the test machine's shared folder.
#
#   tools/bin/deploy.sh [--data]
#
# The Amiga side reads everything from Work: (C:\temp\amiga_gta\work), which is
# the ONLY writable volume on the test machine - the boot hardfile is mounted
# read-only. Swapping a build is therefore a host-side file copy; the HDF image
# is never touched.
#
# --data also copies the game data, which is slow-ish (~54 MB) and only needs
# doing when the data changes, so it is off by default.
set -e

ROOT=/i/GITHUB/Amiga_GTA
WORK=/c/temp/amiga_gta/work
DATA="$ROOT/dos/Grand_Theft_Auto/gtadata"

mkdir -p "$WORK"

# The startup script is version-controlled in the repo; the copy in Work: is
# disposable. Edit winuae/work-template/run, never the deployed one.
cp "$ROOT/winuae/work-template/run" "$WORK/run"
echo "deployed: run"

# The scripted camera path the game replays before going interactive. Same rule
# as `run`: the template in the repo is the original, the copy in Work: is
# disposable. Delete Work:autoinput.txt to get a purely interactive session.
cp "$ROOT/winuae/work-template/autoinput.txt" "$WORK/autoinput.txt"
echo "deployed: autoinput.txt"

# The scripted WALK, replayed after the camera tour. Same format as the host
# harness so one script runs in both places; see the file's own header.
cp "$ROOT/winuae/work-template/autowalk.txt" "$WORK/autowalk.txt"
echo "deployed: autowalk.txt"

for b in gta-aga gta-rtg240 gta-rtg480 gta-rtg; do
    if [ -f "$ROOT/build/$b" ]; then
        cp "$ROOT/build/$b" "$WORK/$b"
        echo "deployed: $b"
    fi
done

# WORKBENCH ICONS.
#
# The game is normally started by Work:run from User-Startup, but it is an
# Amiga port and it should be startable the way an Amiga program is: by
# double-clicking it. That needs an .info next to the binary.
#
# THE ICON CARRIES THE STACK SIZE, and that is the reason this is not
# decoration. A tool launched from Workbench gets the stack ITS ICON asks for
# and not the shell's default; the usual 4096 is far too little here and
# running out of stack on a 68020 is a silent corruption of whatever sits below
# it, not an error message. tools/bin/mkicon.py has the number and the reason.
#
# `AmiGTA` is a copy of the AGA binary under the name the game actually goes
# by, so Workbench shows "AmiGTA" under the icon rather than "gta-aga". It is
# re-copied on every deploy, so it cannot drift from the binary it came from -
# which is the usual objection to keeping a second copy.
# Which python actually RUNS, rather than which one is on the PATH. On this
# host `python3` under Git Bash is Microsoft's Store stub: it exists, it is
# executable, and it prints an advert instead of running the script. So the
# candidates are tried by executing one, which is the only test that means
# anything.
PY=""
for c in python3 python py; do
    if command -v "$c" >/dev/null 2>&1 && "$c" -c "pass" >/dev/null 2>&1; then
        PY="$c"
        break
    fi
done

if [ -n "$PY" ]; then
    if [ -f "$ROOT/build/gta-aga" ]; then
        cp "$ROOT/build/gta-aga" "$WORK/AmiGTA"
        echo "deployed: AmiGTA (copy of gta-aga, for Workbench)"
    fi
    for n in AmiGTA gta-aga gta-rtg240 gta-rtg480 gta-rtg; do
        if [ -f "$WORK/$n" ]; then
            "$PY" "$ROOT/tools/bin/mkicon.py" "$WORK/$n.info" 1000000 >/dev/null
            echo "deployed: $n.info (stack 1000000)"
        fi
    done
else
    echo "WARNING: no working python - Workbench icons not generated"
fi

# The baked tile set. It is small (1 MB) and it changes whenever gtabake or the
# tile format changes, so unlike the 54 MB of raw game data it is deployed every
# time. Baking here rather than on the Amiga is the whole point: see
# native/gta_tiles.h.
TIL="$ROOT/build/data/style001.til"
GRY="$WORK/GTADATA/style001.gry"
if [ ! -f "$TIL" ] || [ "$GRY" -nt "$TIL" ]; then
    if [ -x "$ROOT/build/host/gtabake" ] && [ -f "$GRY" ]; then
        mkdir -p "$ROOT/build/data"
        "$ROOT/build/host/gtabake" "$GRY" "$TIL" >/dev/null
        echo "baked:    build/data/style001.til"
    fi
fi
if [ -f "$TIL" ]; then
    mkdir -p "$WORK/GTADATA"
    cp "$TIL" "$WORK/GTADATA/style001.til"
    echo "deployed: GTADATA/style001.til"
fi

if [ "$1" = "--data" ]; then
    if [ ! -d "$DATA" ]; then
        echo "no game data at $DATA" >&2
        exit 1
    fi
    mkdir -p "$WORK/GTADATA"
    # -u so re-running is cheap; the data almost never changes.
    cp -ru "$DATA/." "$WORK/GTADATA/"
    echo "deployed: GTADATA ($(du -sh "$WORK/GTADATA" | cut -f1))"
fi

echo "--- Work: now holds ---"
ls "$WORK"
