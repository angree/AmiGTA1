#!/usr/bin/env python3
"""Write the release README: CRLF, plain ASCII, wrapped at 76.

CRLF because the archive is read on both machines and it is the one encoding
both accept - Windows wants it, and AmigaDOS `type`, `More`, `MuchMore`, ED and
CygnusEd all cope with the extra CR.

The instructions describe converting the player's OWN data with the bundled
gtabake. This archive contains no game data and no file derived from any."""
import io, os, sys

MODE = "readme"
args = sys.argv[1:]
if args and args[0] == "--datanote":
    MODE = "datanote"
    args = args[1:]
OUT = args[0] if args else "/tmp/README.txt"


DATANOTE = """PUT YOUR GTA DATA FILES IN THIS DRAWER
=======================================

AmiGTA needs exactly TWO files from your own copy of Grand Theft Auto (1997)
for the PC. Both are in the game's GTADATA directory on the PC:

    style001.gry     about 2.7 MB  - the Liberty City art
    nyc.cmp          about 460 KB  - the Liberty City map

COPY THEM IN BINARY MODE. If your transfer tool has a "text" or "ASCII"
mode, turn it off - a text-mode copy corrupts these files silently, and the
game then reports that the map is not a map.

Then, from a shell in the drawer ABOVE this one:

    stack 1000000
    gtabake GTADATA/style001.gry GTADATA/style001.til

That converts the art into the tile set the engine loads, once. It takes a
minute or two on an 020. Afterwards this drawer needs only:

    style001.til     made by gtabake
    nyc.cmp          copied from the PC

style001.gry is not read by the game and can be deleted once the tiles are
made.

No game data is shipped with AmiGTA and none is derived from it - you supply
your own, exactly as with OpenXcom or OpenTTD.

You can delete this note.
"""

TEXT = """AmiGTA v0.0.1
A native AmigaOS 68k port of Grand Theft Auto (1997)

===========================================================================
IMPORTANT: THIS ARCHIVE CONTAINS NO GAME DATA
===========================================================================

You need your own copy of Grand Theft Auto (1997) for the PC. Nothing from
the game is included here and nothing derived from it either - not the art,
not the maps, not the sounds. What is included is the engine and a
converter, and you point the converter at the files you already own.

This is the same arrangement OpenXcom and OpenTTD use.

===========================================================================
WHAT IS IN THIS ARCHIVE
===========================================================================

  gta-aga       320x200, AGA.
                The reference build. Every speed figure in the project's
                notes was measured with this one.

  gta-rtg240    320x240, RTG (CyberGraphX or Picasso96).
                The same picture with more of the city on screen.

  gta-rtg480    640x480, RTG.
                Renders 320x240 and doubles it when it puts it on screen.
                A 68020 cannot rasterise 640x480 at a playable rate; this
                way the picture is sharp and chunky rather than slow.

  gtabake       The tile converter. Runs ON THE AMIGA. It reads the game's
                own style001.gry and writes the tile set the engine loads.

  run           Startup script. Its last line chooses which build runs.

All three binaries also read Work:backend.txt if it exists - one word,
"rtg", "aga" or "wb" - which overrides the built-in default.

===========================================================================
WHAT YOU NEED
===========================================================================

  * 68020 or better. No FPU is required and none is used.
  * AGA for gta-aga; a CyberGraphX or Picasso96 8-bit screen for the two
    RTG builds.
  * About 8 MB of fast RAM to play. The one-off conversion below wants
    about 6 MB free while it runs.
  * Your own copy of GTA (1997) for the PC.

WHICH PC VERSION: this port was built against the DOS, 8-bit release - the
one whose art files are style001.gry and whose executable is gta8.exe. The
2002 Windows re-release carries the same two files, so it works too.

===========================================================================
SETTING IT UP - THE WHOLE JOB IS TWO FILES AND ONE COMMAND
===========================================================================

From your PC installation of GTA you need exactly TWO files. Both are in
the game's GTADATA directory:

    style001.gry     about 2.7 MB  - the Liberty City art
    nyc.cmp          about 460 KB  - the Liberty City map

STEP 1 - unpack this archive somewhere on the Amiga, for example to
    Work:AmiGTA . You get the three binaries, gtabake, run, and an empty
    GTADATA drawer with a note in it.

STEP 2 - copy style001.gry and nyc.cmp from the PC into the GTADATA
    drawer that is already there, IN BINARY MODE.

    Network share, FTP client, CF card or a shared folder in an emulator
    are all fine. If your transfer tool has a "text" or "ASCII" mode, turn
    it OFF. A text-mode copy corrupts these files silently and the game
    then reports that the map is not a map.

STEP 3 - convert the art. From a shell, in the drawer you unpacked to:

        stack 1000000
        gtabake GTADATA/style001.gry GTADATA/style001.til

    It prints what it found and takes a minute or two on an 020. When it
    is done you have GTADATA/style001.til, about 1.8 MB, and you never
    need to run it again.

    You can keep or delete style001.gry afterwards - the game does not
    read it. Only style001.til and nyc.cmp are used.

STEP 4 - you should now have:

        AmiGTA/gta-aga
        AmiGTA/gta-rtg240
        AmiGTA/gta-rtg480
        AmiGTA/gtabake
        AmiGTA/run
        AmiGTA/GTADATA/style001.til     (made in step 4)
        AmiGTA/GTADATA/nyc.cmp          (copied in step 3)

===========================================================================
RUNNING IT
===========================================================================

The game looks for its data as Work:GTADATA, so the simplest thing is to
assign Work: to the drawer you unpacked into:

        assign Work: Work:AmiGTA
        execute run

Or, if the drawer already IS your Work: partition root, just:

        execute run

The run script sets a 1 MB stack before starting the game, and that is not
optional: libnix gives a CLI process 4 KB by default and the map and sprite
loaders go well past it. Starting a binary by hand needs the same:

        stack 1000000
        gta-aga

To change which build the script starts, edit its last line.

===========================================================================
CONTROLS
===========================================================================

  On foot
    Arrow keys      walk forward/back, turn left/right
    Shift           WALK. He runs by default - GTA 1 has no walk key at
                    all, so running is the normal state, not a modifier.
    Enter           get into the nearest car. If somebody is driving it,
                    they are dragged out.

  In a car
    Up / Down       throttle / reverse
    Left / Right    steer
    Space           handbrake
    Enter           get out. The car stays where you left it.

  Any time
    TAB             free camera (fly around without the player)
    F1 / F2         full or half render resolution
    F3              Workbench title bar on/off
    F4              render width
    F5              2.5D full / 2.5D light / flat top-down
    F6              60 fps cap on/off
    F7 / F8         camera height
    - / =           zoom out / in
    SPACE           write the framebuffer to Work:frame.raw
    ESC             quit

===========================================================================
WHAT IS NOT IN v0.0.1
===========================================================================

  * No missions, no weapons, no police, no wanted level.
  * Tyre marks, blood and oil are not drawn.
  * Only Liberty City. The startup path is fixed to nyc.cmp.
  * Sound is not wired up yet.

===========================================================================
IF IT DOES NOT START
===========================================================================

  Nothing happens, or it exits at once
      Look at Work:gta.log - the game writes what it was doing there.
      The usual cause is a missing or text-mode-corrupted data file.

  "Cannot open Work:GTADATA/..."
      The game looks for Work:GTADATA specifically. Either assign Work: to
      the drawer you unpacked into, or move the files to your real Work:.

  gtabake says the style file is not one
      style001.gry was copied in text mode and is corrupted. Copy it again
      in binary mode.

  Not enough memory during conversion
      gtabake holds the whole style file and the output at once. Close
      other programs; it needs roughly 6 MB free.

  The RTG builds open nothing
      There is no CyberGraphX or Picasso96 screen available. Use gta-aga,
      or put the word "aga" into Work:backend.txt.

  A black screen with the machine still alive
      Try F3 to toggle the title bar, which reopens the screen.
"""

data = (DATANOTE if MODE == "datanote" else TEXT).replace("\n", "\r\n")
io.open(OUT, "wb").write(data.encode("ascii"))
print("wrote %s (%d bytes, CRLF, ascii)" % (OUT, len(data)))
