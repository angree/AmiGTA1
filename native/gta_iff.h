/* THE MUSIC EXTRACTOR - a PCM WAV in, an 8-bit IFF 8SVX out.
 *
 * WHY THIS RUNS ON THE AMIGA AND NOT ON THE PC
 * --------------------------------------------
 * Everything else this port converts - the tiles, the sprites, the sound
 * bank, the title screen - is baked by a host tool AND by `gtabake` on the
 * Amiga, because the rule of this project is that the player supplies their
 * own GTA data and converts it themselves; we ship no derived art. The music
 * was the one thing that broke the rule: `tools/gtamusic.c` is host-only, so
 * a player with an Amiga and the 2002 CD had no way to get the soundtrack in.
 *
 * This is that missing half. It is the same portable C89 the rest of the
 * data layer is - stdio only, no Amiga headers, no floats, nothing over
 * 32 bits - so it builds into the game, into a standalone Amiga tool, and
 * into a host tool that can be tested in a second instead of a two-minute
 * emulator round trip.
 *
 * WHAT IT WRITES, AND WHY THAT FORMAT
 * -----------------------------------
 * 8SVX: signed 8-bit mono, 22050 Hz, one BODY chunk. That is not a
 * compromise, it is what Paula reads. The chip's DMA fetches signed 8-bit
 * samples and nothing else, so an 8SVX BODY can be handed to the hardware
 * with no decoding at all - `fread` straight into the Chip buffer. The
 * IMA-ADPCM path (`.mus`, tools/gtamusic.c) costs a table lookup and three
 * shifts per sample and buys half the disk; this costs nothing per sample
 * and is the format every other Amiga program in the world can also read.
 *
 * 22050 Hz is the source's own rate - the 2002 release's Track*.wav are
 * 22050 Hz 16-bit stereo - so for those files there is no resampling to do
 * at all and the fast path below is exact. Paula's PAL DMA floor is period
 * 124 (about 28.6 kHz); 22050 is period 160, comfortably inside it.
 *
 * THE ARITHMETIC IS ALL INTEGER, and on purpose: see CLAUDE.md, defect 5.
 * A float multiply on this machine reaches the ROM and Kickstart 3.1's
 * mathieeesingbas.library has broken multiply and divide entries on an
 * FPU-less machine. The resampler is a 16.16 fixed-point accumulator.
 *
 * IT IS STEPPED, NOT BLOCKING. `gta_iff_step()` does one buffer and returns,
 * so the caller can draw a progress bar, service Intuition and let the player
 * press ESC - which is what the first-run extraction screen does. A single
 * call that converted 270 MB would look exactly like a hung machine.
 *
 * Licence: MIT (ours).
 */
#ifndef GTA_IFF_H
#define GTA_IFF_H

#include <stdio.h>

/* What the extractor targets. Paula's period for it is 3546895/22050 = 160. */
#define GTA_IFF_RATE     22050

/* Source frames per step. 4096 frames is a 16 KB read of 16-bit stereo -
 * big enough that the disk is not the bottleneck, small enough that a step
 * is over in well under a frame and the bar keeps moving. */
#define GTA_IFF_FRAMES   4096
#define GTA_IFF_RAWBYTES (GTA_IFF_FRAMES * 4)
#define GTA_IFF_OUTBUF   4096

#define GTA_IFF_PATHLEN  160

/* One conversion in progress. Roughly 30 KB - malloc it, do not put it on
 * an Amiga stack. */
typedef struct {
    FILE *in, *out;

    /* the source */
    int  channels, bits, rate;
    unsigned long data_left;            /* source bytes not yet read */
    unsigned long data_total;

    /* the destination */
    unsigned long out_total;            /* samples the header promises */
    unsigned long out_done;
    unsigned long body_pad;             /* 1 when BODY needs an even byte */
    int  out_rate;

    /* The 16.16 resampler. `phase` is an offset into the CURRENT window and
     * never an absolute position: absolute overflows 32 bits after 65 536
     * source frames, which is three seconds of music. */
    unsigned long step;
    unsigned long phase;

    unsigned char raw[GTA_IFF_RAWBYTES];
    short         mono[GTA_IFF_FRAMES];
    int           n_mono;
    short         last;                 /* for padding a short tail */

    signed char   obuf[GTA_IFF_OUTBUF];
    int           n_obuf;

    int  done, err;
    char dst[GTA_IFF_PATHLEN];
} gta_iff;

/* Open `src` (a PCM WAV: 8 or 16 bit, mono or stereo, any rate) and start
 * writing `dst` as an 8SVX at `out_rate` Hz (0 means GTA_IFF_RATE).
 * Returns 0 on success. On failure nothing is left behind. */
int  gta_iff_open(gta_iff *j, const char *src, const char *dst, int out_rate);

/* One buffer's worth. Returns 1 while there is more to do, 0 when the file
 * is finished and closed, -1 on an error (the partial output is removed). */
int  gta_iff_step(gta_iff *j);

/* Abandon a conversion: closes both files and DELETES the partial output, so
 * an interrupted extraction never leaves a truncated track that would look
 * like a finished one on the next run. Safe on a finished job. */
void gta_iff_abort(gta_iff *j);

/* 0..1000 for a progress bar. */
int  gta_iff_permille(const gta_iff *j);

/* The whole thing in one call, for the host tool and the standalone
 * converter. Returns 0 on success. */
int  gta_iff_convert(const char *src, const char *dst, int out_rate);

/* ---- WHAT NEEDS EXTRACTING ----------------------------------------------
 *
 * The player drops the 2002 release's soundtrack next to the game data:
 *
 *     GTADATA/Music/Track1.wav ... Track10.wav      (or flat in GTADATA/)
 *
 * and the first run converts them BY ROLE - `title.8svx`, `radio1..6.8svx`
 * and `police.8svx` - not by their position in the drawer. Which track is
 * which was measured; the table and its numbers are at the bottom of
 * gta_iff.c. It matters: the first version numbered them in file order and
 * put the POLICE BAND on the car radio.
 *
 * A track whose output already exists is skipped, so the extraction happens
 * ONCE - which is the whole point of the progress bar being on the first
 * load and never again.
 */
typedef struct {
    char src[GTA_IFF_PATHLEN];
    char dst[GTA_IFF_PATHLEN];
    unsigned long bytes;                /* of the source, for the bar */
} gta_iff_item;

#define GTA_IFF_MAX_ITEMS 10

/* Fill `items` with the conversions still to do under `dir` (which must end
 * in a separator, e.g. "GTADATA/"). Returns how many. */
int  gta_iff_scan(const char *dir, gta_iff_item *items, int max);

#endif /* GTA_IFF_H */
