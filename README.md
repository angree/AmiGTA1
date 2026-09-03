# AmiGTA

A native AmigaOS 68k port of **Grand Theft Auto (1997)**, targeting *classic*
hardware — 68020 and up, AGA or RTG. Not PiStorm, not Vampire, not Emu68.

**This is a reimplementation of the engine, not a wrapper and not an emulator.**
It contains no game code and no game data. You supply your own copy of GTA and
the port converts it on your own machine.

Current release: **v0.0.4** — you can walk around Liberty City, steal a car,
drive it, and use any of five weapons on what you find. Cars dent, burn and
explode. See [what is and is not in it](#what-v004-actually-is).

---

## Download

Grab the archive from [Releases](../../releases). It has the game, a settings
editor, a data converter, a startup script and a README. It has **no game
data** — step-by-step setup instructions are in the README inside the archive.

**One binary, since v0.0.4.** v0.0.3 shipped three — `gta-aga`, `gta-rtg240`
and `gta-rtg480` — which were the same program built at three screen sizes, so
the player had to pick their display by picking an icon. The screen is a
setting now:

| screen | what it is |
|---|---|
| 320x200 | AGA. The reference; every speed figure is measured at this size. |
| 320x240 | CyberGraphX / Picasso96. More of the city on screen. |
| 640x480 | RTG, **really rasterised at 640x480** — every tile, car and person at the art's own size, nothing stretched, twice as much city on screen. About four times the drawing. |
| 640x480 doubled | RTG. Renders 320x240 and doubles every pixel: the picture of 320x240 on a 640x480 screen, for almost nothing. What a plain 020 wants. |

Measured on the RTG calibration machine (68020 core, throttle standing in for a
faster CPU, JIT off): **38.4 fps doubled, 14.7 fps native**. That is the whole
trade, and it is why both are offered instead of one being called "640x480".

### Settings

`gtaprefs` is a small Intuition window that picks the sound path (Paula or AHI),
the display path (AGA, RTG or a window on Workbench) and the screen size, and
tells you which of those your machine actually has. It writes `gta.prefs` next
to the game.

It needs no mouse — `A`, `G`, `R`, `S` and `Esc` drive it — and it has a command
line for machines with no working pointer at all:

```
gtaprefs SHOW              print the settings and what was detected
gtaprefs GFX=WB            set it and save, no window
gtaprefs SCREEN=640x480    the size the game opens at
gtaprefs AUDIO=AHI
```

**On MorphOS, set Graphics to Window** (`gtaprefs GFX=WB`). The port otherwise
opens a screen of its own, cannot get an 8-bit mode, falls back to the planar +
c2p path that needs the real Amiga chipset, and draws the city as colour noise.
Sound does not play yet on any machine; the setting is recorded for when it
does.

You need a 68020 or better (no FPU is required and none is used), about 8 MB of
fast RAM, and your own copy of GTA (1997) for the PC. The DOS 8-bit release is
what it was built against; the 2002 Windows re-release carries the same two data
files, so that works too.

## What v0.0.4 actually is

**Works:** the city renders in 2.5D with correct projection and no gaps in the
geometry; you walk, run and turn; pedestrians keep to the pavements, turn
corners, cross roads at a run and are knocked down or killed by a car; traffic
drives itself along real routes, gives way, queues and takes corners as an arc;
getting in and out of a car is animated door-by-door, you vault the car when you
approach it from the far side, drag the driver out if there is one, and ride a
bike or a convertible in view; the driven car has the original's own physics —
mass, moment of inertia, contact point, and cornering that rotates the car when
you clip it rather than sliding it bodily; cars collide with each other and with
walls, and neither of them can be pushed through the other or thrown out of a
contact; all five weapons work — fists, pistol, machine gun, rocket launcher
and flamethrower — and the street reacts to a shot; cars take damage from
crashes and gunfire, wear the dents, and at a hundred points burn and explode,
taking whatever is parked next to them with them; the score counts, and it and
the current weapon are on screen.

**Not there yet:** no missions, no police, no wanted level; the five weapons are
handed to you at the start because the crates that hold them are not built yet;
sound is not wired up; tyre marks, blood and oil are not drawn; only Liberty
City.

**Speed:** 60.3 fps at 320x200 on the project's calibration machine — a 68020
core with the throttle set to stand in for a faster CPU, JIT off. That number is
a measurement on one specific configuration, not a promise about your Amiga.

## How it was made

The engine is written from scratch as native 68020 code — fixed point
throughout, sin/cos lookup tables, built around c2p — rather than translated
from anything. The original's *behaviour* is the specification: how fast a car
accelerates, how far it slides, how the AI picks a route and when it brakes, how
much a collision pushes and how much it spins. Those numbers are the game's own
rather than values tuned by hand until the result looked plausible, which is why
the comments in `native/gta_traffic.c` and `native/gta_vehphys.c` read the way
they do.

The data layer follows the official DMA **CityScape Data Structure** spec and
the MIT-licensed [Carnage3D](https://github.com/codenamecpp/carnage3d) project.

The platform layer — AGA screen handling, c2p, RTG, Paula audio, the soft-float
fixes for the m68k GCC — is carried over from the same author's
[openttd_amiga_68k](https://github.com/angree) and OpenXcom ports.

There is no floating point anywhere in the game code. On this target it is not
an optimisation but a hazard: Kickstart 3.1's `mathieeesingbas.library` has
broken single-precision multiply and divide entries on FPU-less machines, and
68040/68060 FPUs are partly trap-emulated anyway.

## Building

Toolchain: [bebbo amiga-gcc](https://github.com/bebbo/amiga-gcc) 6.5.0b, plus
`vasm`. The build runs under WSL or any Linux.

```sh
tools/bin/build.sh          # the Amiga game, the settings editor, the converter
tools/bin/build_host.sh     # the renderer as a host binary, for fast iteration
tools/bin/package.sh        # the release archives
```

**You must supply CyberGraphX developer headers yourself.** They are
copyright phase5 digital products and not redistributable, so they are not in
this repository. Put `cybergraphx/`, `clib/`, `inline/`, `libraries/` and
`proto/` into `native/cgx-include/` and the RTG build will find them.

Some flags are not negotiable, and each one cost real time to find:

* `-O1`, not `-O2` — `-O2` breaks C++ exception unwinding on this toolchain.
* `-mcpu=68020 -msoft-float` — `-m68040` silently selects the 68881 multilib.
* never `-lpthread` or `-lc` — they pull newlib in beside libnix.
* never strip — `m68k-amigaos-strip` produces a Hunk executable that halts the
  machine: black screen, no Guru, no output.
* never call `sprintf` — it produces nonsense on this libc. `snprintf`,
  `fprintf` and the `v*` forms are all correct.

The shell scripts and the WinUAE configs carry the author's own paths
(`I:\GITHUB\Amiga_GTA`, `C:\temp\amiga_gta`). `build.sh` and `build_host.sh`
work out the repository root from their own location; the rest are development
harness and you will want to edit them.

## Testing

The renderer has no Amiga headers in it, so the same code builds and runs on the
host in a second instead of a two-minute emulator round trip:

```sh
build/host/gtadump view   <nyc.cmp> build/data/style001.til 64 64 320 200 out/v.bmp
build/host/gtadump column <nyc.cmp> 66 62      # every field of one map column
tools/bin/holecheck.sh                          # must print 0
```

`holecheck.sh` is the renderer's regression test. A black tile and an undrawn
pixel look identical, and GTA's roofs are full of dark tiles, so gaps in the
geometry are invisible to the eye — it clears the frame to a colour Liberty City
never uses and counts what survives.

The game is driven from **inside** the emulated machine, from a script in
`tools/scripts/`, never by synthesising host input.

## Licence

MIT — see [LICENSE](LICENSE).

Two files are not mine and keep their own headers: the chunky-to-planar kernels
`native/c2p1x1_6_c5_bm_040.s` and `native/c2p_rect.s`, both © **Mikael Kalms**.
The wrappers around them are MIT like everything else. What is included but not
mine, and what is deliberately absent, is set out in
[THIRD-PARTY.md](THIRD-PARTY.md).

## Credits

* **Mikael Kalms** — the c2p kernels, the fast part of every frame.
* **DMA Design** — Grand Theft Auto, and the CityScape Data Structure
  specification they published.
* **[Carnage3D](https://github.com/codenamecpp/carnage3d)** (MIT, © 2019
  jericho) — the reading of the data formats.

Grand Theft Auto is © DMA Design / BMG Interactive / Take-Two Interactive.
This project is not affiliated with any of them, and it ships nothing of theirs.
