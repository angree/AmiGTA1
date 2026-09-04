#!/bin/sh
# rampview.sh <bx> <by> <name> - one 2.5D frame of the real renderer, camera on
# block (bx,by), written to out/<name>.bmp and doubled to out/<name>.png.
cd "$(dirname "$0")/../.." || exit 1
MAP=${MAP:-/mnt/c/temp/amiga_gta/work/GTADATA/nyc.cmp}
TIL=${TIL:-build/data/style001.til}
mkdir -p out
build/host/gtadump view "$MAP" "$TIL" "$1" "$2" 320 200 "out/$3.bmp" || exit 1
python3 - "$3" <<'EOF'
import sys
from PIL import Image
n = sys.argv[1]
im = Image.open("out/%s.bmp" % n)
im.resize((im.width * 2, im.height * 2)).save("out/%s.png" % n)
print("out/%s.png" % n)
EOF
