#!/bin/sh
# cbuild.sh - build on C:, not on the network drive.
#
#   wsl sh /mnt/i/GITHUB/Amiga_GTA/tools/bin/cbuild.sh          Amiga build
#   wsl sh /mnt/i/GITHUB/Amiga_GTA/tools/bin/cbuild.sh host     host tools
#
# I: is a network drive, and a build there reads every source and writes
# every object file across it - on a busy day that is minutes of I/O for a
# build that takes seconds. So the sources (native/ and the tools' own .c and
# .sh - about 3 MB, no game data) are mirrored to C:\temp\amiga_gta_build and
# the build runs there; build.sh and build_host.sh find their tree from their
# own path, so they need no change. Everything is copied every time: cp -u
# trusts mtimes, and the network drive's clock is not this machine's - it
# once skipped a header edited a minute earlier and the build failed on it.
#
# The binaries stay on C: - deploy.sh takes them from there when GTA_BUILD is
# set (GTA_BUILD=/c/temp/amiga_gta_build/build). Nothing large goes on C:
# either: the objects and the four binaries are a few MB, and build/data is
# NOT mirrored (host tools that need it read it from I: by path).
#
# Up to three tries, same as buildretry.sh, because /mnt/i can drop out in the
# middle of the copy.
SRC=/mnt/i/GITHUB/Amiga_GTA
DST=/mnt/c/temp/amiga_gta_build
n=0
while [ $n -lt 3 ]; do
  n=$((n+1))
  ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i
  mkdir -p "$DST/tools/bin" "$DST/native" &&
  cp -r "$SRC/native/." "$DST/native/" &&
  cp "$SRC"/tools/*.c "$SRC"/tools/*.h "$DST/tools/" 2>/dev/null
  cp "$SRC"/tools/bin/build.sh "$SRC"/tools/bin/build_host.sh "$DST/tools/bin/" &&
  break
  echo "copy failed, try $n"; sleep 3
done
[ -f "$DST/tools/bin/build.sh" ] || { echo CBUILD_COPY_FAILED; exit 1; }
cd "$DST" || exit 1

if [ "$1" = "host" ]; then
  sh tools/bin/build_host.sh release && echo HOST_BUILD_OK
  exit $?
fi

sh tools/bin/build.sh > "$DST/build_amiga.log" 2>&1
if grep -q "LD  build/AmiGTA" "$DST/build_amiga.log" && [ -f build/AmiGTA ]; then
  grep -i " error\|duplicate section\|warning" "$DST/build_amiga.log" | head
  ls -la --time-style=+%H:%M:%S build/AmiGTA
  echo AMIGA_BUILD_OK
  exit 0
fi
tail -15 "$DST/build_amiga.log"
echo AMIGA_BUILD_FAILED
exit 1
