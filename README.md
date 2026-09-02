# AmiGTA

A native AmigaOS 68k port of **Grand Theft Auto (1997)**, targeting *classic*
hardware — 68020 and up, AGA or RTG. Not PiStorm, not Vampire, not Emu68.

**This is a reimplementation of the engine, not a wrapper and not an emulator.**
It contains no game code and no game data. You supply your own copy of GTA and
the port converts it on your own machine.

Current release: **v0.0.3** — you can walk around Liberty City, steal a car,
drive it, and throw a punch or fire a pistol. See [what is and is not in it](#what-v003-actually-is).

---

## Download

Grab the archive from [Releases](../../releases). It has three binaries, a
settings editor, a data converter, a startup script and a README. It has **no
game data** — step-by-step setup instructions are in the README inside the
archive.

| build | screen | backend |
|---|---|---|
| `gta-aga` | 320x200 | AGA. The reference build; every speed figure is measured with this one. |
| `gta-rtg240` | 320x240 | CyberGraphX / Picasso96. More of the city on screen. |
| `gta-rtg480` | 640x480 | RTG. Renders 320x240 and doubles it — a 68020 cannot rasterise 640x480 at a playable rate, so this way the picture is sharp rather than slow. |

### Settings

`gtaprefs` is a small Intuition window that picks the sound path (Paula or AHI)
and the display path (AGA, RTG or a window on Workbench), and tells you which of
those your machine actually has. It writes `gta.prefs` next to the game.

It needs no mouse — `A`, `G`, `S` and `Esc` drive it — and it has a command line
for machines with no working pointer at all:

```
gtaprefs SHOW              print the settings and what was detected
gtaprefs GFX=WB            set it and save, no window
gtaprefs AUDIO=AHI
```

**On MorphOS, use the native build instead** — see [MorphOS](#morphos). If you
do run the 68k binaries there under emulation, set Graphics to Window
(`gtaprefs GFX=WB`): they otherwise open a screen of their own, cannot get an
8-bit mode, fall back to the planar + c2p path that needs the real Amiga
chipset, and draw the city as colour noise. That fallback is exactly what the
native build removes.

Sound does not play yet on any machine; the setting is recorded for when it
does.

You need a 68020 or better (no FPU is required and none is used), about 8 MB of
fast RAM, and your own copy of GTA (1997) for the PC. The DOS 8-bit release is
what it was built against; the 2002 Windows re-release carries the same two data
files, so that works too.

## What v0.0.3 actually is

**Works:** the city renders in 2.5D with correct projection and no gaps in the
geometry; you walk, run and turn; pedestrians keep to the pavements, turn
corners, cross roads at a run and are knocked down or killed by a car; traffic
drives itself along real routes, gives way, queues and takes corners as an arc;
getting in and out of a car is animated door-by-door, you vault the car when you
approach it from the far side, drag the driver out if there is one, and ride a
bike or a convertible in view; the driven car has the original's own physics —
mass, moment of inertia, contact point, and cornering that rotates the car when
you clip it rather than sliding it bodily; cars collide with each other and with
walls; fists and a pistol work, and the street reacts to a shot.

**Not there yet:** no missions, no police, no wanted level; only fists and the
pistol, with no pickups; sound is not wired up; tyre marks, blood and oil are
not drawn; only Liberty City.

**Speed:** 59.8 fps for `gta-aga` on the project's calibration machine — a
68020 core with the throttle set to stand in for a faster CPU, JIT off. That
number is a measurement on one specific configuration, not a promise about your
Amiga.

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

## MorphOS

There is a native PowerPC build for **MorphOS**. It is not the 68k binary under
emulation — that does not work, and the reason is worth stating: the RTG build
asks CyberGraphX for an 8-bit screen and *falls back to AGA* when there is none,
and modern MorphOS hardware (a Radeon) frequently offers no 8-bit chunky mode at
all. The fallback then allocates bitplanes and runs Kalms' 68020
chunky-to-planar against a chipset that is not there.

```sh
tools/bin/build_morphos.sh                      # gta-morphos, gtabake, gtaprefs
make -f makefile.morphos release ARCHIVEDIR=    # the shippable drawer, archived
```

`build_morphos.sh` sits beside `build.sh` and has the same shape — same root
discovery, same compile helper, same written-out file list, same refusal to
strip. The binary is `gta-morphos`, named the way `gta-aga` and `gta-rtg240`
are, so the four can share a drawer and still be told apart.

`makefile.morphos` adds only what a shell script is bad at: `beta` and
`release` stage a clean `AmiGTA` drawer — binary, `gtabake`, `gtaprefs`, `run`,
a MorphOS README and an empty `GTADATA` with a note saying which two of the
player's own files go in it — and archive it. They write into `ram:` by
default; pass `ARCHIVEDIR=` when building on the Linux cross box, where `ram:`
is not a path. The `.lha` is skipped there with a note (Linux `lha` is usually
Lhasa, which only extracts); the `.zip` is always written.

Toolchain: `ppc-morphos-gcc-9` with the MorphOS SDK at `/gg`. Nothing else — no
`vasm`, and **no vendored CyberGraphX headers**: `cybergraphx/` is part of the
MorphOS SDK, so the "you must supply your own" note below does not apply here.

The whole engine — every line of the rendering, physics, traffic and data code —
is compiled from the same sources the Amiga build uses, unmodified. It was
already portable: fixed point throughout with no floating point anywhere, and
GTA's little-endian data files read a byte at a time rather than by casting a
struct over them, because the same code has to build for the big-endian 68k and
for the host test harness. PowerPC is big-endian too and got that for free.

What changes is the platform layer, and only that:

| | |
|---|---|
| `native/morphos_gfx.c` | new. The RTG path, natively — screen, palette, blit, input. Replaces `native/amiga_gfx.c` wholesale. |
| `native/amiga_uclock.c` | `TimerBase` is `struct Library *` here, and there is a `timer.device` sleep for the frame cap. |
| `native/gta_main.c` | 640x480 and RTG by default; the frame cap sleeps instead of spinning. |

Everything MorphOS-specific is under `#ifdef __MORPHOS__`. `amiga_gfx.c`,
`amiga_startup.c`, `amiga_trap.c`, `fp_single.c`, `fp_conv.c`, `libnix_fixes.c`
and the four assembler files are not built at all — they are 68k Chip RAM, 68k
exception frames, workarounds for Kickstart 3.1's broken soft-float and for
libnix's `wmemcpy`, and chunky-to-planar. None of them mean anything on
PowerPC.

The 68k build is unaffected, and that is checked rather than asserted: with
`__MORPHOS__` undefined, `gta_main.c`, `gta_peds.c` and `gta_traffic.c`
preprocess byte-for-byte identically to the commit this branched from.

Three differences you will actually notice:

* **640x480, rendered.** The Amiga's own `gta-rtg480` renders 320x240 and
  doubles it, because a 68020 cannot rasterise four times the pixels at a
  playable rate. That constraint is the CPU's, not the renderer's, and it does
  not survive the move — so here the picture is drawn at full resolution and
  `GTA_SCALE2X` is never defined. `SCREEN_W`/`SCREEN_H` at the top of
  `makefile.morphos` are the only place it is decided.
* **The screen is whatever depth the machine has.** 8-bit is asked for first,
  because the chunky buffer *is* the display format there and a blit becomes a
  memcpy; 32/16/24/15 are tried after, and on those the blit is
  `WriteLUTPixelArray`. The granted size is read back and the picture centred
  in it, so a display that hands back something other than what was asked for
  still works.
* **The frame cap sleeps.** A 68020 that finishes a frame early has no spare
  capacity worth donating, so the Amiga build busy-waits. A G4 finishes in a
  fraction of the budget and would spend the rest pinning a core at 100%, which
  makes a game that is running perfectly look like one that has locked the
  machine.

Start it with `morphos/run`, or redirect stdout yourself — the engine reports
the screen mode it got, the blit method, and the reason for every failure there,
and from Ambient there is no console for it to go to.

## Building

Toolchain: [bebbo amiga-gcc](https://github.com/bebbo/amiga-gcc) 6.5.0b, plus
`vasm`. The build runs under WSL or any Linux. (For MorphOS see
[above](#morphos).)

```sh
tools/bin/build.sh          # the three Amiga binaries and the tile converter
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
