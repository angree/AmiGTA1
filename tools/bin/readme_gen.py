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

SOUND, if you want it - two more things from your own copy:

    audio/level001.sdt  \  the effects. Convert them once, on the Amiga:
    audio/level001.raw  /  gtabake -sfx GTADATA/audio/level001 \\
                                         GTADATA/level001.snd

    Music/Track1.wav ...   the soundtrack from the 2002 Windows release.
                           Put the WAVs in a Music drawer INSIDE this one
                           and the game converts them itself the first
                           time it starts, showing a progress bar.

No game data is shipped with AmiGTA and none is derived from it - you supply
your own, exactly as with OpenXcom or OpenTTD.

You can delete this note.
"""

TEXT = """AmiGTA v0.7.1
A native Amiga port of Grand Theft Auto (1997) - AmigaOS 3.x and AROS on
68k, and MorphOS (PowerPC)

===========================================================================
WHICH ARCHIVE
===========================================================================

  AmiGTA-v0.7.1-68k      AmigaOS 3.1 and later, and AROS 68k. AmiGTA is a
                         68020 program; the same file also runs under
                         MorphOS's 68k emulation.

  AmiGTA-v0.7.1-MorphOS  MorphOS on PowerPC. AmiGTA is a native PPC build
                         (its title bar says "AmiGTA PPC"); gtaprefs,
                         gtabake and gtaiff are the 68k tools, which
                         MorphOS runs as they are.

Everything below is the same for both.

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
                with -sfx it converts the sound effects - see SOUND below.

  gtaiff        The music extractor, also ON THE AMIGA. It turns one WAV
                of the soundtrack into an IFF 8SVX the game streams. You
                do not normally need it: the game does the same thing by
                itself the first time it starts, with a progress bar. It
                is here for converting one track at a time, and for
                looking at what came out (gtaiff -info file.8svx).

  run           Startup script. Sets the stack and starts the game.

The game, the settings editor and the converter have their own Workbench
icons.

===========================================================================
WHAT YOU NEED
===========================================================================

  * 68020 or better. No FPU is required and none is used.
    Or AROS 68k, or MorphOS (either archive; the PPC one is faster).
  * AGA, or a CyberGraphX / Picasso96 8-bit screen, or - slower, and
    where neither of those works - a window on the Workbench.
    On MorphOS the game opens a 640x480 screen of its own by itself.
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

    ...and, if you want the title screen, one more:

        gtabake -front GTADATA GTADATA/front.fnt

    That one reads the front end's own pictures - f_logo0.raw to
    f_logo7.raw and f_lower0.raw - and squashes them to the Amiga's
    screen with a palette of their own. It is OPTIONAL: without
    front.fnt the game starts straight into the city, exactly as
    v0.3.0 did.

    It prints what it found and takes a minute or two on an 020. When it
    is done you have GTADATA/style001.til, about 1.8 MB, and you never
    need to run it again.

    You can keep or delete style001.gry afterwards - the game does not
    read it. Only style001.til and nyc.cmp are used.

    SAN ANDREAS AND VICE CITY are the same job with their own files:
    copy sanb.cmp and style002.gry (San Andreas), miami.cmp and
    style003.gry (Vice City), then

        gtabake GTADATA/style002.gry GTADATA/style002.til
        gtabake GTADATA/style003.gry GTADATA/style003.til

    and, for their sound, -sfx with audio/level002 and audio/level003
    the way SOUND shows for level001. Without them, play Liberty City
    only - do not pick a level whose files you have not converted.

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
    TAB             shout, as in the original.

  In a car
    Up / Down       throttle / reverse
    Left / Right    steer
    Space           handbrake
    Enter           get out. The car stays where you left it.
    TAB             the horn, for as long as you hold it. In a police car
                    the first press puts the siren on; hold TAB after that
                    for the yelp.

  Any time
    HELP            free camera (fly around without the player). This was
                    TAB before v0.7.1, which gives TAB back to the horn.
    F1 / F2         full or half render resolution
    F3              Workbench title bar on/off
    F4              render width
    F5              2.5D full / 2.5D light / flat top-down
    F6              60 fps cap on/off
    F7 / F8         camera height
    - / =           zoom out / in
    SPACE           on foot with no car ahead: write the framebuffer to
                    frame.raw, beside the game
    P               pause, and P again to play on
    ESC             pause and ask QUIT GAME? - Y or Return quits,
                    N or ESC plays on

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

              Both work since v0.7.0. On MorphOS Auto always means AHI.

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
              320x240 if you chose RTG above - and on MorphOS a full
              640x480 screen: drawn at 640x480 by the PPC build, doubled
              from 320x240 by the 68k one.

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
NEW IN v0.7.1
===========================================================================

  TYRE MARKS AND WHEEL SMOKE. A sliding car leaves marks on road, pavement
  and grass and smokes on the road, by the original's rules and limits.

  PAUSE on P: the world holds, the sound stops, as in the original.

  WATER. Walk off a quay or land a jump short in the river and you drown.

  BLOOD. A body leaves a pool and a splat that stay as long as it lies there,
  a body shot along the ground leaves a trail, and a car sliding through blood
  leaves red marks. An explosion ends in a black cloud, a fire smokes on the
  road. Explosions and fire run at the original's pace: they were twice too
  fast.

  YOUR CAREER IS SAVED, in the original's own file Work:player_a.dat (made
  on the first start if it is not there). A level opens when you pass the one
  before it. At the end of a level - passed, failed, out of lives or quit -
  the end screen shows your criminal record, the Public Enemies table and your
  best scores; RETURN plays on, ESC goes back to the title. Out of lives is
  GAME OVER now, not a free restart. Your lives are shown beside the score,
  as in the original.

  PLAYER on the title screen: left/right picks one of the eight players,
  RETURN types a new name, R starts that player again from the first level.
  The original's cheat names work as they did.

  OPTIONS on the title: sound and music volume, the text speed and whether
  the music plays only in cars or all the time, kept in player_a.dat like
  the original, and the number of cars.

  A wall blown up by a job leaves rubble, and a car coming down from a jump
  throws sparks from its four corners.

  GANG GUNMEN. People a job turns against you now shoot: some stand their
  ground and fire in pairs, others run at you. Mostly they miss by a little
  until they are close; at a car they aim straight.

  THE HORN on TAB, the original's key: a horn in a car, the siren in a
  police car, a shout on foot. The free camera moved to HELP.

  The name of the car you get into is shown at the top, as in the original.

  The renderer copies its rows of pixels itself instead of calling the
  library's memcpy and memset, which went through the ROM.

  This README now has the licence and credits section that v0.7.0's lacked.

===========================================================================
NEW IN v0.7.0
===========================================================================

  ALL SIX LEVELS. Liberty City, San Andreas and Vice City, two levels
  each, load and play; the first job of every one of them has been
  played through from the phone to the pay-out. (Each level needs its
  own style and map files from your copy - see SETTING IT UP.)

  AROS AND MORPHOS. The 68k AmiGTA runs on AROS 68k and under MorphOS's
  68k emulation, and there is a native MorphOS PowerPC build in its own
  archive. Sound goes through AHI on MorphOS; the frame cap sleeps in
  timer.device instead of spinning the CPU.

  THE POLICE TAKE YOU THE ORIGINAL'S WAY. In a car the cop gets out,
  walks to your door, waits for the car to stop and pulls you out - no
  more BUSTED the moment he touches you. On foot you are knocked down
  first and then arrested.

  TRAINS. The El runs on its tracks with its stations, round curved
  corners; you can board it and ride it, and a train hit hard enough is
  wrecked - and the script's train jobs work.

  THE AMBULANCE AND THE FIRE ENGINE. Run someone over and an ambulance
  comes for the body and its medic brings him back; set something alight
  and a fire engine comes and puts it out.

  TRAFFIC that sorts itself out: two cars nose to nose at a junction give
  way to each other, and out of sight a jammed car hops ahead, swaps with
  the one blocking it or a junction stuck for long enough is cleared, as
  the original does. More traffic at a higher wanted level.

  WALLS ARE SOLID AND DOORS OPEN. Shots stop at walls and fences, cars at
  posts; garage doors open for the right car only. The bomb shop works.

  And: shots from beside a car hit the car, explosions have no seam, the
  title menu's letters no longer run into each other.

===========================================================================
NEW SINCE v0.2.0
===========================================================================

  * A BRIDGE IS A BRIDGE. Traffic climbs the ramps onto the flyovers and
    comes down the far side instead of treating the deck as a junction
    and driving off it onto the street below; a car you shove up a ramp
    goes up it rather than disappearing under the deck.
  * THE MAN YOU DRAG OUT OF A CAR gets up and runs away again instead of
    dying under your wheels as you drive off.
  * THE WANTED LEVEL is the original's arithmetic. One pedestrian run
    over is a third of a head, not two heads; hitting or shooting a
    police car is one head at once. Nothing decays, as in the original.
  * WEIGHT DECIDES DAMAGE. A tanker shrugs off what wrecks a saloon: the
    car table's mass is what a crash is divided by, and a heavy body
    takes two points a bullet where a car takes five - thirty-nine
    shots to destroy a tanker against sixteen for a saloon.
  * AND THE PAVEMENT beside the police station no longer has an
    invisible wall in it.

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
NEW IN v0.5.0
===========================================================================

  IT MAKES A NOISE. Gunshots, explosions, doors, pickups and the police
  come out of the game's own sound bank through the Amiga's four Paula
  channels - the effects on two of them, music on the other two, with a
  small Chip RAM cache so a sound is copied down once and played from
  then on. See SOUND below for the one conversion it needs.

  A CAR RADIO. Get in and a station plays; get out and it stops; get in
  again and it is the next station. A POLICE CAR carries the police band
  instead, the way the original does - an ordinary saloon never does.

  AND THE FIRST TIME YOU START IT, IT EXTRACTS THE MUSIC ITSELF. Put the
  soundtrack WAVs where SOUND says and the first launch converts them,
  with a progress bar, before the title screen appears - once, on your
  own machine, out of your own files. Every launch after that finds them
  already converted. ESC abandons it and the next start picks up where
  you left off.

===========================================================================
NEW IN v0.4.0
===========================================================================

  THE MISSIONS RUN. The level's own script is read and obeyed. The game
  starts you where the script says - which is also the trigger that
  begins the level - the South Park telephones ring, and walking up to
  one on foot gives you a job: a brief, an arrow, a car to steal and a
  garage to deliver it to. Stealing pays, delivering pays more, and the
  game rings you back on another phone for the next one. Liberty City's
  first two missions play from the phone to the pay-out; the second has
  a passenger to collect on the way.

  THE HUD IS THE GAME'S OWN. The wanted level's cop heads flash across
  the top, the weapon in hand and its ammunition sit at the left with
  the armour and the get-out-of-jail-free card under them, and the three
  displays the original uses are all there: the brief box along the
  bottom with its icon, the pager scrolling its line through the little
  device at the top left, and MISSION COMPLETE across the middle. The
  score has the multiplier under it.

  A TITLE SCREEN, with the logo animating and START GAME / AMIGA
  OPTIONS / QUIT. It needs one extra conversion - see SETTING IT UP -
  and the game runs without it if you skip that.

  AMIGA OPTIONS: CARS, the most traffic the city will have, 6 to 50 in
  steps of 4 (Left/Right or Return to change it; 18 to begin with). The
  traffic is most of the CPU on a slow Amiga, so this is the setting
  that buys frame rate there. Parked cars and wrecks do not count.

  A CAR IS SOLID. On foot you cannot walk through one any more - run at
  it and press SPACE to jump over a low car or slide under a tall one.

  A CAR CAN JUMP. A ramp with a hole after it is a jump, and the arc is
  the original's.

  THE SPRAY SHOPS WORK. Drive in, the car changes colour, and the
  police lose interest in it.

===========================================================================
SOUND
===========================================================================

Both halves are optional and both come out of files you already own.

THE EFFECTS. One conversion, once, on the Amiga:

    gtabake -sfx GTADATA/audio/level001 GTADATA/level001.snd

level001 is Liberty City; it needs audio/level001.sdt and
audio/level001.raw from your PC copy, about 1 MB. Without level001.snd
the game is simply quiet and nothing else changes.

THE MUSIC. Copy the soundtrack WAVs from the 2002 Windows release into

    GTADATA/Music/Track1.wav ... Track10.wav

and start the game. It converts them itself, with a progress bar, into
signed 8-bit mono at 22050 Hz - which is what the Amiga's sound hardware
reads directly. This takes a while the first time (they are large files)
and never happens again.

THE TRACKS ARE NOT ALL RADIO STATIONS, and the game sorts them out:

    Track1  -> title.8svx    the title screen
    Track2  -> radio1.8svx   \
    Track3  -> radio2.8svx    |
    Track4  -> radio3.8svx    > the car radio, next station each time
    Track5  -> radio4.8svx    |  you get in
    Track6  -> radio5.8svx    |
    Track7  -> radio6.8svx   /
    Track9  -> police.8svx   the police band - IN A POLICE CAR ONLY

Track8 is not in the release and Track10 is not used. Convert fewer if
you want less disk: any file that is not there is simply skipped.

You can convert them yourself instead, one at a time:

    gtaiff GTADATA/Music/Track2.wav GTADATA/radio1.8svx
    gtaiff -scan GTADATA/                 (what a first run would do)

WHERE THE DISK GOES. The converted tracks are about four times smaller
than the WAVs they came from, but they are still roughly 1.3 MB a
minute. You do not have to convert all of them.

===========================================================================
WHAT IS NOT IN v0.7.1
===========================================================================

  * Not every job has been played through - the first of each level
    has. What the game reaches is in its own log.
  * Only your own car leaves tyre marks.
  * Trams stand still - as they do in the original, DOS and Windows.
  * Speed: a plain 68020 is not enough for the full city; an 030/040
    or faster with fast RAM is what it wants.

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
      Graphics to Window.
      With no pointer:  gtaprefs GFX=WB
      (MorphOS needs none of this since v0.7.0: it opens a 32-bit RTG
      screen by itself.)

  A black screen with the machine still alive
      Try F3 to toggle the title bar, which reopens the screen.

===========================================================================
LICENCE AND CREDITS
===========================================================================

AmiGTA is MIT licensed, (c) 2026 angree, except for the following.

The 68k AmiGTA contains the chunky-to-planar routines c2p1x1_6_c5_bm_040 and
c2p_rect, (c) Mikael Kalms, used under their own terms. Their source, with
his headers, is in the public repository.

The reading of the game's data formats follows DMA Design's CityScape Data
Structure specification and Carnage3D, whose licence requires this notice:

    Copyright (c) 2019 jericho

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the
    "Software"), to deal in the Software without restriction, including
    without limitation the rights to use, copy, modify, merge, publish,
    distribute, sublicense, and/or sell copies of the Software, and to
    permit persons to whom the Software is furnished to do so, subject to
    the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
    OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
    IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
    CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
    TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
    SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Thanks to stefkos for the first native MorphOS build.

Grand Theft Auto is (c) DMA Design / BMG Interactive / Take-Two Interactive.
This project is not affiliated with any of them and ships none of their
files. You supply your own copy of the game.
"""

data = (DATANOTE if MODE == "datanote" else TEXT).replace("\n", "\r\n")
io.open(OUT, "wb").write(data.encode("ascii"))
print("wrote %s (%d bytes, CRLF, ascii)" % (OUT, len(data)))
