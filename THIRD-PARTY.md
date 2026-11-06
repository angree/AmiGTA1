# Third-party code, and what is deliberately not here

The project is MIT — see [LICENSE](LICENSE). Three things fall outside that,
and each is listed here rather than buried in the licence text so that GitHub
reads the licence as what it is.

## Included, but not mine

Two files are the chunky-to-planar conversion kernels — the fast part of every
frame. They are included **verbatim, with their original headers**, as scene
code has been shared and reused on the Amiga for twenty-five years. The headers
are the attribution; rewriting or stripping them is the one thing that would
turn a normal reuse into a problem.

    native/c2p1x1_6_c5_bm_040.s     (c) Mikael Kalms <mikael@kalms.org>, 2000
    native/c2p_rect.s               (c) Mikael Kalms

The wrappers around them in `native/c2p_glue.s` are mine and are MIT like
everything else. If you need a tree that is MIT and nothing else, delete those
two files and write your own c2p — do not relabel his.

The two kernels are linked into the 68k `AmiGTA` binary, so the README inside
that archive credits them too. The native MorphOS binary does not contain them
(`native/mos_stubs.c` stands in, and is never called).

## Other code and contributors

* **Carnage3D** — the reading of the data formats follows it; the field orders
  and names are credited in the headers concerned. Its notice is below. It is
  reference material in `ref/`, which is not published and never built.
* **stefkos** — the first native MorphOS/PowerPC build (pull request #2):
  `native/morphos_gfx.c`, `native/morphos_audio.c`, `makefile.morphos`,
  `morphos/`, contributed as MIT. They are not used by the v0.7.0 build.

Linked into the binaries by the toolchains, under their own terms: libgcc and
libnix (m68k-amigaos), and the MorphOS SDK's startup and libraries (PowerPC).

### Carnage3D's licence

    MIT License

    Copyright (c) 2019 jericho

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

## Not included: CyberGraphX developer headers

`native/cgx-include/` is © phase5 digital products, "all rights reserved", and
is **not redistributable**. It is needed to build the RTG backend, so you must
supply it yourself: put `cybergraphx/`, `clib/`, `inline/`, `libraries/` and
`proto/` into `native/cgx-include/` and the build will find them.

## Not included: the game

**No file of Grand Theft Auto (1997) is in this repository or in any release
built from it** — no art, no maps, no sounds and no code, and no conversion of
them. Not a converted tile set, not a screenshot, not a data file. The engine is
written from scratch; how the original behaves, including its numbers and
tables of constants, was established by studying the game.

Grand Theft Auto is © DMA Design / BMG Interactive / Take-Two Interactive.
This project is not affiliated with any of them.

The port reads **your** copy of the game, on **your** machine: two files go
into a `GTADATA` drawer and the bundled `gtabake` converts the art there. That
is the same arrangement OpenXcom and OpenTTD use, and it is the reason the
release archive is 280 KB rather than 30 MB.
