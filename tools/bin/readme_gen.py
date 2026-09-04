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

...and, OPTIONAL but worth it, five more from the same directory:

    mission.ini      the level script - the weapon crates come from it;
                     without it you start with every weapon instead
    english.fxt      the texts - the pager messages (or french/german/
                     italian.fxt renamed to english.fxt)
    pager1.fon       the pager font        \
    score1.fon       the score digits       > the original's letters
    big1.fon         the BUSTED / WASTED font/

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
    (and the optional five, copied as they are - nothing converts them)

style001.gry is not read by the game and can be deleted once the tiles are
made.

No game data is shipped with AmiGTA and none is derived from it - you supply
your own, exactly as with OpenXcom or OpenTTD.

You can delete this note.
"""

TEXT = """AmiGTA v0.2.0
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

  AmiGTA        The game. ONE binary for every machine.

                v0.0.3 shipped three - gta-aga, gta-rtg240 and gta-rtg480 -
                which were the same program built at three screen sizes.
                The screen is a setting now, so there is nothing to choose
                by icon: start AmiGTA, and if it does not look right run
                gtaprefs. See SETTINGS below.

  gtaprefs      Settings. Sound, display path and screen size, chosen here
                rather than by editing files by hand or by picking one of
                three programs. Double-click it, or run it from a shell.

  gtabake       The converter. Runs ON THE AMIGA. It reads the game's own
                style001.gry and writes the tile set the engine loads, and
                with -sfx it converts the sound bank too - see WHAT IS NOT
                IN v0.2.0 for why you do not need that yet.

  run           Startup script. Sets the stack and starts the game.

All three have their own Workbench icon.

===========================================================================
WHAT YOU NEED
===========================================================================

  * 68020 or better. No FPU is required and none is used.
  * AGA, or a CyberGraphX / Picasso96 8-bit screen, or - slower, and
    where neither of those works - a window on the Workbench.
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

Five more from the same directory are OPTIONAL and copied as they are:
mission.ini (the level script: the weapon crates - without it you start
with every weapon), english.fxt (the texts: the pager), and the fonts
pager1.fon, score1.fon and big1.fon (the original's letters for the
pager, the score and the BUSTED / WASTED cards; without them the port's
own small letters draw those). Nothing converts them.

STEP 1 - unpack this archive anywhere you like - DH1:Games/AmiGTA,
    Work:AmiGTA, a CF card, it does not matter. You get the game, the two
    tools, run, and an empty GTADATA drawer with a note in it.

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

        AmiGTA/AmiGTA
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

Or double-click the AmiGTA icon in Workbench - it carries the same 1 MB
stack the script sets.

Up to v0.0.1 every path was hard-coded to Work: and this section told you to
assign Work: to the game drawer. That was a bug with a workaround printed
next to it. Since v0.0.2 the game finds its own data through PROGDIR:, the
drawer the program was started from.

The run script sets a 1 MB stack before starting the game, and that is not
optional: libnix gives a CLI process 4 KB by default and the map and sprite
loaders go well past it. Starting a binary by hand needs the same:

        stack 1000000
        AmiGTA

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
                    People nearby run from the shot.
    X / Z           next / previous weapon. You start with all five:

                      Fists         a punch; the man goes down and
                                    gets up again.
                      Pistol        one round every fifth of a second,
                                    four blocks of range.
                      Machine gun   the same round, five times as fast.
                      Rocket        explodes on the first thing it
                                    meets and takes out everything
                                    within a block.
                      Flamethrower  a jet about a block long; whoever
                                    it touches burns, runs, and dies.

                    You start with three times what the original's
                    crates hold: 60 pistol rounds, 60 for the machine
                    gun, 15 rockets and 30 tanks of fuel.

                    The five are handed to you at the start only
                    because the crates that hold them are not built
                    yet - in the original you begin with your fists.

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
It opens a small window on Workbench with three choices:

  Sound       Auto, Off, Paula or AHI.
              Paula is the Amiga chipset, played straight through
              audio.device. AHI is the sound API that sound cards,
              MorphOS and OS4 all speak, and it costs some CPU because it
              mixes in software. Auto picks Paula where a real chipset
              exists and AHI where one does not.

              NOTHING PLAYS YET. v0.2.0 has no sound at all; the setting
              is read and reported in gta.log and nothing more. It is here
              because the choice has to be settled before the sound layer
              is written, not after.

  Graphics    Auto, AGA, RTG or Window.
              Auto opens an AGA screen. Window runs the game inside a
              window on Workbench: it is the slowest of the three and it
              works on machines where the other two do not - MorphOS in
              particular.

  Screen      Auto, 320x200, 320x240, 640x480 or 640x480 doubled.
              This is what v0.0.3 needed three separate programs for.

              320x200 is the reference and what every speed figure in the
              project's notes was measured at. 320x240 shows more of the
              city and wants an RTG screen. Auto means 320x200, or
              320x240 if you chose RTG above.

              THE TWO 640x480 MODES ARE DIFFERENT PICTURES:

              640x480 is really drawn at 640x480. Every tile, car and
              person goes down at the art's own size - one stored pixel
              to one screen pixel, nothing stretched - so twice as much
              of Liberty City is on screen and all of it is sharp. It
              costs about four times the drawing, so it is for a fast
              accelerator with a graphics card, not a plain 020.

              640x480 doubled renders 320x240 and doubles every pixel on
              the way to the screen. It is the same picture as 320x240
              with fatter pixels - no more detail - and it fills a
              640x480 screen for almost nothing. This is the one a slow
              machine wants, and up to v0.0.3 it was the only one there
              was.

The window also tells you what it found on YOUR machine - AGA, RTG and
AHI, each yes or no.

It needs no mouse. A cycles the sound, G the graphics, R the screen size,
S saves, Esc cancels. And there is a command line for a machine with no
pointer at all:

    gtaprefs SHOW              print the settings and what was detected
    gtaprefs GFX=WB            set it and save, without opening a window
    gtaprefs AUDIO=AHI GFX=RTG SCREEN=640x480
    gtaprefs ?                 the full list

Settings are written to gta.prefs beside the game, which is plain text you
can edit by hand. backend.txt still works and still wins if you have one;
gtaprefs keeps it in step with what it saves, so the two cannot disagree.

===========================================================================
NEW SINCE v0.1.0
===========================================================================

  * THE POLICE. A wanted level from the original's own heat rules (it
    never decays; the level is the heat against Liberty City's
    thresholds), shown as cop heads at the top. Patrol cars on the
    streets; at a level, cars are sent by its quota - they chase, cut
    you off, ram, turn round after you; at level 3 roadblocks go up
    across the exits of your district. A cop gets out beside your
    stopped car or when you are on foot, and ARRESTS you - or shoots,
    when you are armed or the level is 3 or more. BUSTED: the
    multiplier halved, weapons gone, released outside a police station.
    Take a police car and its cop comes after you on foot.
  * YOU CAN DIE. Health 100, four lives, armour from its crate, fire
    burns. WASTED: a life gone, the hospital.
  * THE CRATES. The level script's 150 crates - pistols, machine guns,
    armour, bribes, multipliers, jail-free cards, lives - opened by a
    shot, by walking into them or driving over them. You start with
    your fists, as the original does. (Needs mission.ini.)
  * TRAFFIC LIGHTS run and are drawn on the corners; traffic waits at
    the red, and pedestrians wait at the kerb and cross on the green.
  * THE ORIGINAL'S LETTERS: the pager line along the bottom with the
    level's opening message, the score digits, the BUSTED and WASTED
    cards. (Needs english.fxt and the three fonts.)
  * A rocket leaves fires on the wall it hits; a bike's rider is
    knocked off, not pulled out.

===========================================================================
WHAT IS NOT IN v0.2.0
===========================================================================

  * No missions. The phones do not ring and there is no guide arrow;
    the script's texts and fonts are in, the interpreter is next.
  * The police cars have no siren, and the cop has no firing pose.
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

  Nothing opens with Graphics set to RTG
      There is no CyberGraphX or Picasso96 screen available. Run gtaprefs
      and set Graphics to AGA, or to Window if AGA is no good either. If
      you also chose one of the 640x480 sizes, set Screen back to
      320x200: those want a graphics card.

  640x480 runs, but it crawls
      That is the native one - it really draws 640x480, which is four
      times the work. Pick "640x480 doubled" instead: same screen size,
      the picture of 320x240, and almost no extra cost.

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
