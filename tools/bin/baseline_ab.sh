#!/bin/sh
# baseline_ab.sh <backup.tar.gz> [ticks] - run the traffic regression against
# the code inside a backup, then put the working copy back.
#
# WHY: one seed cannot tell a two-point change in flow from the spread between
# seeds - measured 91/95/92/91 on one build - so "did this change cost flow"
# has to be asked of both builds over the same four seeds. There is no version
# control in this repo, so the A side comes out of a backup tar.
#
# WHAT IT SWAPS: the traffic engine AND tools/gtadump.c, because the tool calls
# whatever API the engine had at the time; swapping only the engine leaves the
# tool calling functions that do not exist yet and the build fails.
#
# THE RESTORE IS A TRAP, not a line at the end. The first version of this
# script was `set -e` with the restore at the bottom: the baseline build
# failed, the script stopped, and the working copy stayed in /tmp while
# native/gta_traffic.c did not exist at all. Nothing that swaps a source file
# out may be written any other way.
BK=$1
T=${2:-6000}
cd "$(dirname "$0")/../.." || exit 1
[ -f "$BK" ] || { echo "usage: baseline_ab.sh <backup.tar.gz> [ticks]"; exit 1; }

TMP=$(mktemp -d) || exit 1
FILES="native/gta_traffic.c native/gta_traffic.h tools/gtadump.c"

restore() {
    for f in $FILES; do
        [ -f "$TMP/mine/$f" ] && cp "$TMP/mine/$f" "$f"
    done
    echo "=== working copy restored"
    sh tools/bin/build_host.sh >/dev/null 2>&1
    rm -rf "$TMP"
}
trap 'restore' EXIT INT TERM

mkdir -p "$TMP/mine/native" "$TMP/mine/tools"
for f in $FILES; do cp "$f" "$TMP/mine/$f" || exit 1; done

tar xzf "$BK" -C "$TMP" \
    Amiga_GTA/native/gta_traffic.c Amiga_GTA/native/gta_traffic.h \
    Amiga_GTA/tools/gtadump.c || exit 1
for f in $FILES; do cp "$TMP/Amiga_GTA/$f" "$f" || exit 1; done

echo "=== BASELINE ($BK)"
if sh tools/bin/build_host.sh >"$TMP/build.log" 2>&1; then
    sh tools/bin/driveseeds.sh "$T"
else
    echo "the baseline does not build - last lines:"
    tail -5 "$TMP/build.log"
fi
