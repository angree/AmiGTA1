#!/bin/sh
# wbuild.sh - the Amiga build WITHOUT WSL, from Git Bash on Windows.
#
#   sh I:/GITHUB/Amiga_GTA/tools/bin/wbuild.sh
#
# Same job as cbuild.sh (mirror the sources to C:\temp\amiga_gta_build and run
# build.sh there), but with the Windows build of bebbo amiga-gcc 6.5.0b in
# C:\amiga-gcc instead of /opt/amiga inside WSL. Written on 2026-11-26 when the
# WSL service hung in STOP_PENDING and could not be restarted without admin.
SRC=/i/GITHUB/Amiga_GTA
DST=/c/temp/amiga_gta_build
mkdir -p "$DST/tools/bin" "$DST/native" || exit 1
cp -r "$SRC/native/." "$DST/native/" || { echo WBUILD_COPY_FAILED; exit 1; }
cp "$SRC"/tools/*.c "$SRC"/tools/*.h "$DST/tools/" 2>/dev/null
cp "$SRC"/tools/bin/build.sh "$DST/tools/bin/" || { echo WBUILD_COPY_FAILED; exit 1; }
cd "$DST" || exit 1

AMIGA_PREFIX=/c/amiga-gcc AMIGA_NDK=C:/amiga-gcc/m68k-amigaos/ndk-include \
    sh tools/bin/build.sh > "$DST/build_amiga.log" 2>&1
if grep -q "LD  build/AmiGTA" "$DST/build_amiga.log" && [ -f build/AmiGTA ]; then
  grep -i " error\|duplicate section\|warning" "$DST/build_amiga.log" | head -20
  ls -la build/AmiGTA
  echo AMIGA_BUILD_OK
  exit 0
fi
tail -25 "$DST/build_amiga.log"
echo AMIGA_BUILD_FAILED
exit 1
