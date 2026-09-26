#!/bin/sh
# buildretry.sh - the Amiga build, proven rather than assumed.
#
# /mnt/i drops out of WSL not only before a command but in the MIDDLE of one;
# build.sh then prints only "can't cd", nothing is rebuilt, and the next test
# runs the OLD binary - which reads exactly like "the change did nothing".
# So: remount, build, and count the build as done only when the LD line is in
# the log and build/AmiGTA exists; up to three tries. Prints the binary's size
# and time so the caller can check it against what gets deployed.
n=0
while [ $n -lt 3 ]; do
  n=$((n+1))
  ls /mnt/i/GITHUB >/dev/null 2>&1 || sudo -n mount -t drvfs I: /mnt/i
  cd /mnt/i/GITHUB/Amiga_GTA || { echo "cd failed, try $n"; sleep 3; continue; }
  sh tools/bin/build.sh > /tmp/build_amiga.log 2>&1
  if grep -q "LD  build/AmiGTA" /tmp/build_amiga.log && [ -f build/AmiGTA ]; then
    grep -i " error\|duplicate section" /tmp/build_amiga.log | head
    ls -la --time-style=+%H:%M:%S build/AmiGTA
    echo "AMIGA_BUILD_OK try $n"
    exit 0
  fi
  echo "try $n failed:"; tail -5 /tmp/build_amiga.log
  sleep 5
done
echo AMIGA_BUILD_FAILED
exit 1
