AmiGTA for MorphOS
==================

A native PowerPC port of the AmiGTA engine - a reimplementation of Grand
Theft Auto (1997) for classic Amiga hardware, built here for MorphOS.

THIS ARCHIVE CONTAINS NO GAME DATA AND NO GAME CODE. You supply your own
copy of GTA and convert it on your own machine with the bundled gtabake.


WHAT IS IN HERE
---------------

    gta-morphos  the game. 640x480. Named the way the Amiga builds are -
                 gta-aga, gta-rtg240, gta-rtg480 - so the four can share a
                 drawer and still be told apart.
    gtabake      the data converter. You run this once, on your own copy
                 of GTA, before the game will start.
    gtaprefs     the settings editor. It records a sound setting that
                 nothing plays yet, and a graphics setting that this
                 target has only one legal value for; `gtaprefs SHOW`
                 printing what your machine actually has is the useful
                 part.
    run          a one-line startup script that redirects the log.
    GTADATA/     empty. Your two converted files go in here.


SETTING IT UP
-------------

1. Unpack this archive wherever you like. There is nothing to assign:
   the game finds its data through PROGDIR:, the drawer the executable
   itself was loaded from, and that is set for a shell start and for a
   double-click from Ambient alike.

2. Copy two files out of your own PC copy of Grand Theft Auto into the
   GTADATA drawer here. They are in the game's own GTADATA directory:

       style001.gry     about 2.7 MB  - the Liberty City art
       nyc.cmp          about 460 KB  - the Liberty City map

   COPY THEM IN BINARY MODE. A "text" or "ASCII" mode transfer corrupts
   them silently.

   The DOS 8-bit release is what the engine was written against. The 2002
   Windows re-release carries the same two files, so that works too.

3. Convert the art. From a shell, in this drawer:

       gtabake GTADATA/style001.gry GTADATA/style001.til

   That produces style001.til, which is what the game reads. You only
   ever do this once, and you can delete style001.gry afterwards - only
   style001.til and nyc.cmp are used.

   gtabake can also convert GTA's sound bank (`gtabake -sfx ...`), and
   the game loads it and reports what is in it, but nothing plays yet on
   any version of this port. There is no point converting it until
   something does.

4. Start it:

       execute run

   or double-click the icon.


PLAYING
-------

  arrows          walk and turn; hold shift to walk instead of run
  TAB             free the camera from the player
  RETURN          get in or out of a car
  SPACE           handbrake in a car; dump a frame on foot
  - and =         zoom out and in (keypad - and + work too)
  F1 / F2         full resolution / half resolution
  F3              show or hide the screen title bar
  F4              width 640 / 512
  F5              2.5D / 2.5D-light / flat
  F6              frame cap on or off
  F7 / F8         camera height
  ESC             quit

The title bar is on by default because this is an Amiga-family port and
the screen's depth gadget is how you get back to Ambient.


WHAT THIS VERSION IS
--------------------

This is v0.0.3 of the engine, built for MorphOS. Everything the Amiga
version does, this does - it is the same engine source, and only the
display and timing layer differs.

Works: the city renders in 2.5D with correct projection and no gaps in
the geometry; you walk, run and turn; pedestrians keep to the pavements,
turn corners, cross roads at a run and are knocked down or killed by a
car; traffic drives itself along real routes, gives way, queues and
takes corners as an arc; getting in and out of a car is animated
door-by-door, you vault the car when you approach it from the far side,
drag the driver out if there is one, and ride a bike or a convertible in
view; the driven car has the original's own physics; cars collide with
each other and with walls; fists and a pistol work, and the street
reacts to a shot.

Not there yet: no missions, no police, no wanted level; only fists and
the pistol, with no pickups; sound is not wired up; tyre marks, blood
and oil are not drawn; only Liberty City.


IF SOMETHING GOES WRONG
-----------------------

Start it with `execute run` and read gta.log beside the executable.

The engine reports everything it does there: which screen mode it asked
for and which one it got, how deep that screen turned out to be and
therefore which blit route it is using, the map and tile summaries, the
frame timings, and the reason for every failure. Started by
double-clicking there is no console for any of that to go to, which is
why the script redirects it to a file.

Two lines worth knowing how to read:

    morphos: screen 640x480x8, ... blit: direct LUT8 bitmap access
        The best case. An 8-bit screen means the buffer the renderer
        draws into IS the display format, so a frame is a memory copy
        and nothing is converted.

    morphos: blit: WriteLUTPixelArray (truecolour screen, CTABFMT_XRGB8)
        Your graphics driver offers no 8-bit mode, which is normal on
        Radeon hardware. The picture is identical; each frame costs a
        conversion, which on any machine that runs MorphOS is not
        something you will see.

    gta: FAILED to load PROGDIR:GTADATA/style001.til
        Step 3 above has not been done, or gtabake wrote it somewhere
        else.


LICENCE
-------

MIT. Grand Theft Auto is (c) DMA Design / BMG Interactive / Take-Two
Interactive. This project is not affiliated with any of them and it
ships nothing of theirs.
