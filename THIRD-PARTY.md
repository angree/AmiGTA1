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

## Not included: CyberGraphX developer headers

`native/cgx-include/` is © phase5 digital products, "all rights reserved", and
is **not redistributable**. It is needed to build the RTG backend, so you must
supply it yourself: put `cybergraphx/`, `clib/`, `inline/`, `libraries/` and
`proto/` into `native/cgx-include/` and the build will find them.

## Not included: the game

**No part of Grand Theft Auto (1997) is in this repository or in any release
built from it** — no art, no maps, no sounds, no code, and nothing derived from
any of them. Not a converted tile set, not a screenshot, not a data file.

Grand Theft Auto is © DMA Design / BMG Interactive / Take-Two Interactive.
This project is not affiliated with any of them.

The port reads **your** copy of the game, on **your** machine: two files go
into a `GTADATA` drawer and the bundled `gtabake` converts the art there. That
is the same arrangement OpenXcom and OpenTTD use, and it is the reason the
release archive is 280 KB rather than 30 MB.
