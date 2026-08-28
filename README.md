# AmiGTA

A native AmigaOS 68k port of **Grand Theft Auto (1997)**, targeting *classic*
hardware — 68020 and up, AGA or RTG. Not PiStorm, not Vampire, not Emu68.

**This is a reimplementation of the engine, not a wrapper and not an emulator.**
It contains no game code and no game data. You supply your own copy of GTA and
the port converts it on your own machine.

Current release: **v0.0.1** — you can walk around Liberty City, steal a car and
drive it. See [what is and is not in it](#what-v001-actually-is).

---

## Download

Grab the archive from [Releases](../../releases). It has three binaries, a data
converter, a startup script and a README. It has **no game data** — step-by-step
setup instructions are in the README inside the archive.

| build | screen | backend |
|---|---|---|
| `gta-aga` | 320x200 | AGA. The reference build; every speed figure is measured with this one. |
| `gta-rtg240` | 320x240 | CyberGraphX / Picasso96. More of the city on screen. |
| `gta-rtg480` | 640x480 | RTG. Renders 320x240 and doubles it — a 68020 cannot rasterise 640x480 at a playable rate, so this way the picture is sharp rather than slow. |

You need a 68020 or better (no FPU is required and none is used), about 8 MB of
fast RAM, and your own copy of GTA (1997) for the PC. The DOS 8-bit release is
what it was built against; the 2002 Windows re-release carries the same two data
files, so that works too.

## What v0.0.1 actually is

**Works:** the city renders in 2.5D with correct projection and no gaps in the
geometry; you walk, run and turn; pedestrians walk around with varied clothing;
traffic drives itself along real routes, gives way, queues and takes corners as
an arc; you get in and out of cars, dragging the driver out if there is one; the
driven car has the original's own physics — mass, moment of inertia, contact
point, and cornering that rotates the car when you clip it rather than sliding
it bodily; cars collide with each other and with walls.

**Not there yet:** no missions, no weapons, no police, no wanted level; sound is
not wired up; tyre marks, blood and oil are not drawn; only Liberty City.

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

## Building

Toolchain: [bebbo amiga-gcc](https://github.com/bebbo/amiga-gcc) 6.5.0b, plus
`vasm`. The build runs under WSL or any Linux.

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
