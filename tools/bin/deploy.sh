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
#
# THE TEST RIG NEEDS ONE EXTRA LINE THAT PLAYERS MUST NOT GET. The HDF's
# User-Startup runs `Work:run` without changing directory first, so the script
# has to CD to the shared folder itself. A player unpacks the game wherever
# they like and cds there before `execute run`, and a hard-coded `CD Work:`
# would send them somewhere else entirely - which is the whole fault being
# fixed in v0.0.2. So the line is added HERE, on deploy, and the shipped
# script stays clean.
sed 's|^Stack 1000000$|Stack 1000000\nCD Work:|' \
    "$ROOT/winuae/work-template/run" > "$WORK/run"
echo "deployed: run (with the test rig's CD Work:)"

# THE STARTUP SELF-TEST RUNS HERE AND NOWHERE ELSE.
#
# It closes and reopens the screen twice right after the first frame, to prove
# the F3 path rebinds g_chunky, g_pitch and the renderer's target - a real
# regression check, and the only way to reach that path without a keypress.
#
# But it is a screen teardown before the player has touched anything, and a
# system where reopening a display is not routine dies there: MorphOS was
# reported as "draws one frame and crashes", which is exactly this. So the
# game defaults it OFF and the rig turns it on, keeping the cover where it was
# designed to run without shipping it to anybody.
#
# Append rather than overwrite: opts.txt is also where an A/B under
# investigation lives, and this must not wipe one.
if [ ! -f "$WORK/opts.txt" ] || ! grep -q '^selftest ' "$WORK/opts.txt"; then
    echo "selftest 1" >> "$WORK/opts.txt"
    echo "deployed: opts.txt selftest 1 (the rig keeps the F3 regression check)"
fi

# The scripted camera path the game replays before going interactive. Same rule
# as `run`: the template in the repo is the original, the copy in Work: is
# disposable. Delete Work:autoinput.txt to get a purely interactive session.
cp "$ROOT/winuae/work-template/autoinput.txt" "$WORK/autoinput.txt"
echo "deployed: autoinput.txt"

# The scripted WALK, replayed after the camera tour. Same format as the host
# harness so one script runs in both places; see the file's own header.
cp "$ROOT/winuae/work-template/autowalk.txt" "$WORK/autowalk.txt"
echo "deployed: autowalk.txt"

# ONE GAME BINARY since v0.0.4 - the screen size is a setting, not a build.
# Any leftover gta-aga / gta-rtg* in the shared folder is deleted rather than
# left alone: a stale one still runs, and a stale binary that still runs is
# the most expensive kind of debris this rig can leave behind.
for b in gta-aga gta-rtg240 gta-rtg480 gta-rtg; do
    rm -f "$WORK/$b" "$WORK/$b.info"
done
for b in AmiGTA gtaprefs gtabake; do
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
# Since v0.0.4 the game IS `AmiGTA` - one binary, the name it goes by on the
# Workbench, with the screen size chosen in gtaprefs instead of by picking one
# of three icons.
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
    if [ -f "$WORK/AmiGTA" ]; then
        "$PY" "$ROOT/tools/bin/mkicon.py" "$WORK/AmiGTA.info" 1000000 \
            --kind game >/dev/null
        echo "deployed: AmiGTA.info (stack 1000000)"
    fi
    # The two tools are not the game and do not need the game's stack.
    # gtabake recurses through the sprite reader, gtaprefs opens one window;
    # 100000 covers both with room to spare. Giving them 1 MB would suggest
    # the number means something here, and the one place it does mean
    # something is the game - see the header of mkicon.py.
    #
    # AND THEY GET THEIR OWN PICTURES. Every program in the drawer used to
    # carry the same car icon, so the settings editor was identifiable only by
    # the name under it - which is no use at all on the machine this program
    # exists for, where the display is already wrong.
    for n in gtaprefs gtabake; do
        if [ -f "$WORK/$n" ]; then
            # if/else and not `[ ... ] && k=bake`: under `set -e` a test that
            # comes out false at the end of a line ends the script.
            if [ "$n" = gtabake ]; then k=bake; else k=prefs; fi
            "$PY" "$ROOT/tools/bin/mkicon.py" "$WORK/$n.info" 100000 \
                --kind "$k" >/dev/null
            echo "deployed: $n.info (stack 100000, $k icon)"
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

# THE SOUND BANK, baked the same way and for the same reason.
#
# Baked from the DEVELOPER'S OWN copy under dos/ - which is read, never
# written, and never leaves this machine. level001 is Liberty City; level000 is
# the front end and has 16 sounds. Nothing plays it yet; deploying it is what
# proves a megabyte of samples survives the trip and loads on the Amiga.
SND="$ROOT/build/data/level001.snd"
SFXSRC="$ROOT/dos/Grand_Theft_Auto/gtadata/audio/level001"
if [ ! -f "$SND" ] || [ "$SFXSRC.raw" -nt "$SND" ]; then
    if [ -x "$ROOT/build/host/gtabake" ] && [ -f "$SFXSRC.sdt" ]; then
        mkdir -p "$ROOT/build/data"
        "$ROOT/build/host/gtabake" -sfx "$SFXSRC" "$SND" >/dev/null
        echo "baked:    build/data/level001.snd"
    fi
fi
if [ -f "$SND" ]; then
    mkdir -p "$WORK/GTADATA"
    cp "$SND" "$WORK/GTADATA/level001.snd"
    echo "deployed: GTADATA/level001.snd"
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
