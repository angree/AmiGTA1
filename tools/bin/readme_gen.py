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

TEXT = """AmiGTA v0.0.3
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

  gtaprefs      Settings. Pick the sound and display path here rather
                than by editing files by hand. Double-click it, or run it
                from a shell. New in v0.0.3 - see SETTINGS below.

  gtabake       The converter. Runs ON THE AMIGA. It reads the game's own
                style001.gry and writes the tile set the engine loads, and
                with -sfx it converts the sound bank too - see WHAT IS NOT
                IN v0.0.3 for why you do not need that yet.

  run           Startup script. Its last line chooses which build runs.

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

STEP 1 - unpack this archive anywhere you like - DH1:Games/AmiGTA,
    Work:AmiGTA, a CF card, it does not matter. You get the three
    binaries, gtabake, run, and an empty GTADATA drawer with a note in it.

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
        AmiGTA/gtaprefs
        AmiGTA/gtabake
        AmiGTA/run
        AmiGTA/GTADATA/style001.til     (made in step 3)
        AmiGTA/GTADATA/nyc.cmp          (copied in step 2)

    ...and that whole drawer can sit anywhere on any volume.

===========================================================================
RUNNING IT
===========================================================================

THERE IS NOTHING TO ASSIGN. Put the game wherever you like, go to that
drawer and start it:

        cd DH1:Games/AmiGTA
        execute run

Or double-click one of the three icons in Workbench - they carry the same
1 MB stack the script sets.

Up to v0.0.1 every path was hard-coded to Work: and this section told you to
assign Work: to the game drawer. That was a bug with a workaround printed
next to it. Since v0.0.2 the game finds its own data through PROGDIR:, the
drawer the program was started from.

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
                    they are dragged out. From the wrong side of the car
                    he jumps over it first, as in the original.
    Space           jump: running at a low car he goes over it, at a
                    bus he slides under it.
    Ctrl            fire. Held down it keeps firing, as in the original.
                    With fists it is a punch; the man you hit goes down
                    and gets up again. With the pistol a bullet carries
                    four blocks and stops at the first person, car or
                    wall it meets. People nearby run from the shot.
    X / Z           next / previous weapon.

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
    SPACE           on foot with no car ahead: write the framebuffer to
                    frame.raw, beside the game
    ESC             quit

===========================================================================
SETTINGS
===========================================================================

Run gtaprefs - double-click it, or type its name in the game's drawer.
It opens a small window on Workbench with two choices:

  Sound       Auto, Off, Paula or AHI.
              Paula is the Amiga chipset, played straight through
              audio.device. AHI is the sound API that sound cards,
              MorphOS and OS4 all speak, and it costs some CPU because it
              mixes in software. Auto picks Paula where a real chipset
              exists and AHI where one does not.

              NOTHING PLAYS YET. v0.0.3 has no sound at all; the setting
              is read and reported in gta.log and nothing more. It is here
              because the choice has to be settled before the sound layer
              is written, not after.

  Graphics    Auto, AGA, RTG or Window.
              Auto uses whatever the build you start was made for. Window
              runs the game inside a window on Workbench: it is the
              slowest of the three and it works on machines where the
              other two do not - MorphOS in particular.

The window also tells you what it found on YOUR machine - AGA, RTG and
AHI, each yes or no.

It needs no mouse. A cycles the sound, G the graphics, S saves, Esc
cancels. And there is a command line for a machine with no pointer at all:

    gtaprefs SHOW              print the settings and what was detected
    gtaprefs GFX=WB            set it and save, without opening a window
    gtaprefs AUDIO=AHI GFX=RTG
    gtaprefs ?                 the full list

Settings are written to gta.prefs beside the game, which is plain text you
can edit by hand. backend.txt still works and still wins if you have one;
gtaprefs keeps it in step with what it saves, so the two cannot disagree.

===========================================================================
WHAT IS NOT IN v0.0.3
===========================================================================

  * Fists and the pistol only - no machine gun, rocket launcher or
    flamethrower, and no crates to pick them up from.
  * No missions, no police, no wanted level.
  * Tyre marks, blood and oil are not drawn.
  * Only Liberty City. The startup path is fixed to nyc.cmp.
  * Sound is not wired up yet. The DATA side is done - gtabake can
    already convert GTA's own sound bank, and the game loads it and
    reports what is in it - but nothing plays. There is no point
    converting it until something does:

        gtabake -sfx GTADATA/audio/level001 GTADATA/level001.snd

    (level001 is Liberty City. You would need audio/level001.sdt and
    audio/level001.raw from your PC copy, about 1 MB.)

===========================================================================
IF IT DOES NOT START
===========================================================================

  Nothing happens, or it exits at once
      Look at gta.log in the game's drawer - the game writes what it
      was doing there.
      The usual cause is a missing or text-mode-corrupted data file.

  "Cannot open PROGDIR:GTADATA/..."
      The two data files are not in the GTADATA drawer next to the
      program, or style001.til was never made - see STEP 3. The game looks
      beside its own executable and nowhere else, so this is never an
      assign problem.

  gtabake says the style file is not one
      style001.gry was copied in text mode and is corrupted. Copy it again
      in binary mode.

  Not enough memory during conversion
      gtabake holds the whole style file and the output at once. Close
      other programs; it needs roughly 6 MB free.

  The RTG builds open nothing
      There is no CyberGraphX or Picasso96 screen available. Run gtaprefs
      and set Graphics to AGA, or to Window if AGA is no good either.

  The city is drawn in the wrong colours, or as noise
      The game asked for an 8-bit screen, did not get one, and fell back
      to a path that needs the real Amiga chipset. Run gtaprefs and set
      Graphics to Window. This is what MorphOS does.
      With no pointer:  gtaprefs GFX=WB

  A black screen with the machine still alive
      Try F3 to toggle the title bar, which reopens the screen.
"""

data = (DATANOTE if MODE == "datanote" else TEXT).replace("\n", "\r\n")
io.open(OUT, "wb").write(data.encode("ascii"))
print("wrote %s (%d bytes, CRLF, ascii)" % (OUT, len(data)))
