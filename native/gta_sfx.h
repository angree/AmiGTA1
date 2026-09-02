/* The sound-effect bank: GTA's own .SDT/.RAW pair, baked for Paula.
 *
 * Portable C89, same rules as gta_tiles.h and gta_style.h - stdio only, no
 * floats, compiles unchanged on the host and on m68k-amigaos. There is nothing
 * Amiga-specific in here: this is the DATA layer. Whatever plays the samples -
 * audio.device, AHI, or SDL on the host - takes a pointer and a length from
 * this and does its own thing with them.
 *
 * WHAT THE ORIGINAL SHIPS
 * -----------------------
 * Two files per level in GTADATA/audio, and the format is as simple as it
 * gets - verified against the real data rather than assumed:
 *
 *     level001.sdt   1572 bytes = 131 records of 12 bytes, LITTLE-endian
 *                    { u32 offset, u32 length, u32 sample_rate }
 *     level001.raw   1044377 bytes of 8-bit UNSIGNED PCM, concatenated
 *
 * The last record's offset + length is exactly the size of the .raw, on every
 * level file, which is what makes the reading of the format a fact and not a
 * guess. Liberty City is level001; level000 has only 16 entries and is the
 * front end.
 *
 * The sample rates that actually occur, across those 131 sounds:
 *
 *     5500 Hz x44   22050 x33   11025 x22   8000 x12   4000 x6
 *     ...and one each of 17000, 15000, 14800, 19000, 13600, 14000, 16000,
 *     14900, 14686, 6000, 15200, 5000
 *
 * EVERY ONE OF THOSE IS INSIDE PAULA'S RANGE. The DMA floor is period 124,
 * about 28.6 kHz on PAL, and the fastest sound here is 22050 (period 161). So
 * the chipset can play this bank at its own rates with NO resampling and no
 * mixer - which is the entire reason the audio design in PLAN.md is four
 * hardware channels rather than a software mixer.
 *
 * WHY IT IS BAKED, LIKE THE TILES
 * -------------------------------
 * Same argument as gta_tiles.h: do the work once, on whichever machine is
 * converting, so the machine that reads it every time the game starts does
 * nothing but read.
 *
 *   - UNSIGNED to SIGNED. Paula wants signed 8-bit; the .raw is unsigned.
 *     That is a subtract over a megabyte, and on a 68020 a megabyte of
 *     byte-at-a-time work at startup is a visible pause for no reason.
 *   - PERIOD instead of RATE. The period is what the hardware register takes,
 *     it needs a divide to compute, and it never changes.
 *   - BIG-ENDIAN index, so the Amiga byte-swaps nothing. The host swaps
 *     instead; it runs once, in a build tool.
 *   - EVEN LENGTHS. Paula's length register counts WORDS, so an odd byte count
 *     is not expressible. Rounding is done here rather than at every play.
 *
 * The result is the same size as the input, because it is the same samples.
 * That is fine: the win is that nothing is computed at load time.
 *
 * WHERE THE SAMPLES LIVE IS NOT THIS FILE'S PROBLEM, and it is the real
 * constraint on an 8 MB machine: Paula can only DMA out of Chip RAM, and a
 * megabyte of Chip is most of what an A1200 has. This loader puts the bank
 * wherever malloc puts it. Deciding which sounds get a Chip copy, and when,
 * belongs to the player side.
 *
 * Licence: MIT (ours).
 */
#ifndef GTA_SFX_H
#define GTA_SFX_H

#include <stdio.h>

/* 'GSFX' - the baked bank's magic, big-endian like the rest of the header. */
#define GTA_SFX_MAGIC   0x47534658UL
#define GTA_SFX_VERSION 1

/* The paranoid ceiling. level00N.sdt has 131 entries and vocalcom.sdt has 71;
 * this is here so a corrupt header cannot make the loader allocate nonsense. */
#define GTA_SFX_MAX 1024

typedef struct {
    unsigned long offset;   /* byte offset into the bank's sample data */
    unsigned long length;   /* bytes, EVEN - Paula counts words */
    unsigned short period;  /* Paula period, PAL. >= 124 */
    unsigned short rate;    /* the original rate in Hz, for a non-Paula player */
} gta_sfx_entry;

typedef struct {
    int count;
    gta_sfx_entry *entry;   /* count of them */
    signed char *data;      /* the samples, 8-bit SIGNED, one block */
    unsigned long bytes;    /* how many of them */
} gta_sfx;

/* Load a baked bank. 0 on success, non-zero on failure; the struct is zeroed
 * either way, so gta_sfx_free() is always safe afterwards. */
int  gta_sfx_load(const char *path, gta_sfx *sfx);
void gta_sfx_free(gta_sfx *sfx);

/* One sound: pointer into the bank and its length, or NULL/0 for a bad index
 * or a zero-length entry (the original has those - they are holes in the
 * table, not sounds). Never returns a pointer without a usable length. */
const signed char *gta_sfx_sample(const gta_sfx *sfx, int n, unsigned long *len,
                                  int *period);

void gta_sfx_describe(const gta_sfx *sfx, FILE *out);

/* ---- the converter half, which runs in gtabake and not in the game -------
 *
 * It is here rather than in tools/ for the same reason gta_style.c is: gtabake
 * is cross-compiled for the Amiga and handed to the player, so the code that
 * reads the player's own files has to be portable, not host-only.
 *
 * `sdt` and `raw` are the two input paths, `out` the baked bank. Returns 0 on
 * success. Prints what it found to `log` if that is not NULL. */
int gta_sfx_bake(const char *sdt, const char *raw, const char *out, FILE *log);

#endif /* GTA_SFX_H */
