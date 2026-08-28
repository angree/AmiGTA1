#!/bin/sh
# Build the public source tree, from an ALLOW-LIST.
#
# The working tree holds the game itself, a 346 MB ISO, the research material
# and the tools that produced it. None of that is ours to hand out, so this
# script never says "copy everything except" - it names the files that may go,
# copies only those, and then refuses to finish if anything it did not expect
# turns up in the result.
#
#   sh tools/bin/publish.sh            # -> C:\temp\amiga_gta\github\AmiGTA1
#   sh tools/bin/publish.sh <dir>
#
# It does NOT commit or push. Look at the tree first; the git commands are
# printed at the end.
#
# WHY THE SSD AND NOT THE REPOSITORY DRIVE: the working tree is on a network
# share. Git on it is slow enough to look hung, and a staging directory built
# there once came out as `d?????????` in WSL and unreachable from Windows.
set -e

ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i

ROOT=$(cd "$(dirname "$0")/../.." 2>/dev/null && pwd)
[ -d "$ROOT/native" ] || ROOT=/mnt/i/GITHUB/Amiga_GTA

DEST=${1:-/mnt/c/temp/amiga_gta/github/AmiGTA1}

echo "source: $ROOT"
echo "dest:   $DEST"

# Keep .git if the destination is already a clone - this is an update, not a
# fresh export.
if [ -d "$DEST" ]; then
    find "$DEST" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
else
    mkdir -p "$DEST"
fi

copy() {                                # copy <relative path>
    d=$(dirname "$1")
    mkdir -p "$DEST/$d"
    cp -p "$ROOT/$1" "$DEST/$1"
}

# --- the port itself -------------------------------------------------------
# Everything in native/ EXCEPT cgx-include, which is (c) phase5 and not
# redistributable. The build looks for it and the README says so.
for f in "$ROOT"/native/*.c "$ROOT"/native/*.h "$ROOT"/native/*.s; do
    copy "native/$(basename "$f")"
done

# --- tools -----------------------------------------------------------------
# Not the Ghidra drivers or the Watcom compiler spec: those are the
# reverse-engineering toolchain, not part of the port.
for f in "$ROOT"/tools/bin/*.sh "$ROOT"/tools/bin/*.py; do
    b=$(basename "$f")
    case "$b" in
        ghidra_*.sh|install_watcom_cspec.py) continue ;;
    esac
    copy "tools/bin/$b"
done
for f in "$ROOT"/tools/*.c; do copy "tools/$(basename "$f")"; done
for f in "$ROOT"/tools/scripts/*.txt; do copy "tools/scripts/$(basename "$f")"; done

# --- emulator harness ------------------------------------------------------
for f in "$ROOT"/winuae/*.uae; do copy "winuae/$(basename "$f")"; done
for f in "$ROOT"/winuae/harness/*.ps1; do copy "winuae/harness/$(basename "$f")"; done
for f in "$ROOT"/winuae/work-template/*; do
    copy "winuae/work-template/$(basename "$f")"
done

# --- front matter ----------------------------------------------------------
copy README.md
copy LICENSE
copy .gitignore

# --- and now refuse to be wrong --------------------------------------------
echo "--- checks ---"

bad=0

# 1. nothing from the game, and nothing derived from it
if find "$DEST" -type f \( -iname '*.gry' -o -iname '*.cmp' -o -iname '*.til' \
        -o -iname '*.iso' -o -iname '*.raw' -o -iname '*.sdt' -o -iname '*.mp4' \
        -o -iname '*.exe' \) | grep -q .; then
    echo "FAIL: game data or a binary reached the export tree:"
    find "$DEST" -type f \( -iname '*.gry' -o -iname '*.cmp' -o -iname '*.til' \
        -o -iname '*.iso' -o -iname '*.raw' -o -iname '*.exe' \)
    bad=1
fi

# 2. no reverse-engineering provenance in the comments.
#
# This script and .gitignore name the excluded things deliberately - they are
# the exclusion - so they are the two files the check must not read.
scan() { grep -rliE "$1" "$DEST" --exclude=publish.sh --exclude=.gitignore \
             --exclude-dir=.git 2>/dev/null; }
hits=$(scan '\b(FUN|DAT|_DAT|LAB)_[0-9a-fA-F]{4,}' || true)
if [ -n "$hits" ]; then
    echo "FAIL: decompiler symbols left in:"; echo "$hits"; bad=1
fi
hits=$(scan 'ghidra|decompil|disassembl|re/(notes|decomp|flat)|the DOS (binary|build)' || true)
if [ -n "$hits" ]; then
    echo "FAIL: reverse-engineering references left in:"; echo "$hits"; bad=1
fi

# 3. the phase5 headers must not be here
if [ -d "$DEST/native/cgx-include" ]; then
    echo "FAIL: cgx-include was copied - it is (c) phase5, not redistributable"
    bad=1
fi

# 4. nothing large: this tree is text
big=$(find "$DEST" -type f -size +1M -not -path '*/.git/*' | head)
if [ -n "$big" ]; then
    echo "FAIL: file over 1 MB in a source-only tree:"
    echo "$big"
    bad=1
fi

[ "$bad" = 0 ] || { echo "PUBLISH REFUSED"; exit 1; }

n=$(find "$DEST" -type f -not -path '*/.git/*' | wc -l)
kb=$(du -sk "$DEST" --exclude=.git 2>/dev/null | cut -f1)
echo "OK: $n files, $kb KB"
echo
echo "next:"
echo "  cd $DEST"
echo "  git add -A && git commit && git push"
