/* The baked tile set: what the Amiga actually loads instead of a .GRY.
 *
 * Portable C89, same rules as gta_style.h and gta_map.h - stdio only, no
 * floats, compiles unchanged on the host and on m68k-amigaos.
 *
 * WHY BAKE AT ALL
 * ---------------
 * GTA's blocks are 64x64. At 320x200 that is 5x3 tiles on screen, which was
 * measured and rejected (the notes); the renderer draws 32x32. Downscaling
 * 380 blocks at startup would cost time and would still leave the 2.7 MB .GRY
 * in RAM. Baking moves that work to the host, and the result is SMALLER than
 * the file it replaces even though it holds four rotations of every lid and
 * two orientations of every side:
 *
 *     side  195 x 1024 x 2 (stored + transposed)   =  399 360
 *     lid   154 x 1024 x 4 (four rotations)        =  630 784
 *     aux    31 x 1024                             =   31 744
 *     header + palette                             =      800
 *                                                    ---------
 *                                                    1 062 688   vs 2 708 088
 *
 * WHY FOUR ROTATIONS AND A TRANSPOSE
 * ----------------------------------
 * Baking the variants means the runtime blit is always a straight walk of
 * consecutive source bytes, whatever the map says about the block:
 *
 *   - lid rotation is a per-block field in the map (type_map bits 14..15) and
 *     applies ONLY to the lid - confirmed against Carnage3D's
 *     GameMapHelpers.cpp, which rotates the lid's texture coordinates and
 *     nothing else. Four pre-rotated copies turn it into an index.
 *   - the two side flips apply only to side faces and only along the wall, so
 *     they cost nothing at draw time: the renderer walks its along-wall LUT
 *     backwards. No baked variants needed for them.
 *   - the TRANSPOSED side copy exists because west and east walls extrude
 *     HORIZONTALLY on screen (see the Phase 4 design note in PLAN.md). Their
 *     inner loop wants a fixed texture column, which is a stride-32 read in
 *     the stored tile and a contiguous one in the transpose.
 *
 * The file is written big-endian by tools/gtabake.c so that the Amiga - the
 * machine that reads it every time the game starts - does no byte swapping.
 * The host loader swaps instead; it runs once, in a build tool.
 *
 * Licence: MIT (ours).
 */
#ifndef GTA_TILES_H
#define GTA_TILES_H

#include <stdio.h>

#include "gta_car.h"

#define GTA_TILE_DIM   32
#define GTA_TILE_AREA  (GTA_TILE_DIM * GTA_TILE_DIM)

/* 'GTAT' */
#define GTA_TIL_MAGIC    0x47544154UL
/* VERSION 4 (2026-08-24): the transposed side tiles are gone.
 *
 * They existed for `blit_wall_h`, which walked destination ROWS and therefore
 * wanted a fixed texture COLUMN - a stride-32 read in a normally-stored tile.
 * The trapezoid rasteriser scans that face by COLUMN instead (which is what
 * the original does, and why), so the normal tile is the contiguous one and
 * `gta_tiles_side_t()` lost its last caller.
 *
 * Dropping the section saves 195 KB of the .til and the same again in RAM, and
 * - the part that matters on a 68030 - it takes a third off the tile set, so
 * more of what IS read stays in a 256-byte data cache.
 *
 * The version bump is the whole safety net: a .til built before this is
 * refused with a clear message instead of being read one section out of
 * step. */
/* VERSION 5 (2026-08-27): the palette remap tables.
 *
 * 256 tables of 256 bytes, straight out of the .GRY's 64 KB block. Without
 * them every pedestrian in the city wore the player's own colours and every
 * car of one model was identical - the remap field existed on the traffic's
 * cars and had nowhere to be applied. See gta_style.h for which ranges belong
 * to cars and which to people. */
#define GTA_TIL_VERSION  5UL
#define GTA_TIL_HDR      32
#define GTA_TIL_PALETTE  768
#define GTA_TIL_DATA_OFF (GTA_TIL_HDR + GTA_TIL_PALETTE)

#define GTA_LID_ROTATIONS 4

/* SPRITES (format version 2)
 * -------------------------
 * Everything drawn on top of the city - the player, pedestrians, cars,
 * explosions - is a sprite, and until version 2 the baked file held none of
 * them, so nothing could be drawn on the city at all.
 *
 * Sprites are NOT downscaled 2:1 the way blocks are, and that is deliberate:
 *
 *   - a pedestrian is 14x18 source pixels. Halving it gives 7x9, at which
 *     point the eight walk frames are no longer distinguishable from each
 *     other. Blocks survive halving because they are flat texture; a person is
 *     a silhouette.
 *   - the zoom range is 16..64 screen pixels per block and a block is 64
 *     source pixels, so at maximum zoom the world is drawn at exactly 1:1 with
 *     the original art. A full-resolution sprite is then pixel-exact and is
 *     never magnified - only ever minified, which is what the scaler is good
 *     at.
 *   - it costs 914 641 bytes for all 1009 sprites of style001, against
 *     ~460 KB halved. On a 32 MB machine that is not the constraint.
 *
 * A sprite therefore lives in SOURCE scale (64 px to a block) while a tile
 * lives in BAKED scale (32 px to a block). The renderer scales a tile by
 * step/32 and a sprite by step/64; same reciprocal table, different numerator.
 *
 * The section is laid out after aux:
 *
 *     u32 sprite_bytes                total pixels below, no padding
 *     u32 counts[21]                  the .GRY sprite_numbers table, so the
 *                                     Amiga can still ask "the 4th ped"
 *     entry[n_sprites]:  u16 w, u16 h, u32 offset
 *     pixels[sprite_bytes]            each sprite tightly packed, w bytes/row
 *
 * Palette index 0 is transparent, exactly as in the .GRY. Unlike a block, a
 * sprite is an arbitrary w x h rectangle, so it needs the index above; that is
 * the only structural difference from the block sections. */
#define GTA_TIL_SPRITE_TYPES 21
#define GTA_TIL_SPRHDR   (4 + GTA_TIL_SPRITE_TYPES * 4)
#define GTA_TIL_SPRENTRY 8

/* CARS (format version 3)
 * ----------------------
 * The 38 vehicle definitions from the .GRY's car table, which the Amiga
 * otherwise could not reach: the guest loads this file and never opens a .GRY.
 *
 * The section is after the sprites:
 *
 *     u32 n_cars
 *     record[n_cars]                  GTA_TIL_CARREC bytes each, big-endian
 *
 * FIXED-SIZE RECORDS, unlike the .GRY's. In the style file a record carries
 * its doors inline and is therefore 174 bytes plus 8 per door, so the table
 * can only be WALKED. Here every record reserves all four door slots and the
 * table can be INDEXED - `cars[n]` with no walk, which is what the runtime
 * wants and what costs 38 x 138 = 5244 bytes to have. The doors that do not
 * exist are zeroed and `n_doors` still says how many are real.
 *
 * `sprite_index` is stored RESOLVED - the absolute index into the sprite
 * table, not the per-category one - because resolving it needs the category
 * bases and those are a .GRY concept. The Amiga should not have to know that
 * a bus counts its sprites separately from a car. */
#define GTA_TIL_CARREC 138

/* REMAPS (format version 5), after the cars:
 *
 *     u32 n_remaps                 256, or 0 if the .GRY had none
 *     bytes[n_remaps * 256]        table n at offset n*256
 */
#define GTA_TIL_REMAP_STRIDE 256

typedef struct {
    unsigned short w, h;
    unsigned long  off;         /* byte offset into gta_tiles.sprite_pixels */
} gta_tile_sprite;

typedef struct {
    int dim;                    /* GTA_TILE_DIM; checked, not trusted */
    int n_side, n_lid, n_aux;
    unsigned char *remaps;      /* n_remaps * 256, or NULL */
    int n_remaps;

    unsigned char palette[GTA_TIL_PALETTE];

    unsigned char *data;        /* ONE allocation; the four pointers below
                                 * point into it and are never freed */
    /* IS THIS SIDE TILE FULLY OPAQUE? One byte per side tile, worked out at
     * load time - no file-format change, and the scan is 195 x 1024 bytes,
     * which is lost in the noise beside reading the file.
     *
     * It exists to take a branch out of the wall blitter's inner loop. A wall
     * is drawn masked because index 0 has to show through fences, railings and
     * fire escapes - but most side tiles are the flat side of a building and
     * have no transparent pixel at all, and for those the test is a branch per
     * pixel that can never fire. Walls are the whole of the per-pixel work in
     * a 2.5D frame once the lids are memcpy'd, so it is worth knowing.
     *
     * The saving grows with the camera: at the shipped height a frame draws
     * about a hundred walls, three times what it drew at sixteen grid levels. */
    unsigned char *side_opaque; /* n_side flags, 1 = no index-0 pixel */

    unsigned char *side;        /* n_side tiles, as stored          */
    unsigned char *lid;         /* n_lid * 4 tiles, rotation-major  */
    unsigned char *aux;         /* n_aux tiles                      */

    /* Sprites: a separate allocation, because unlike the blocks they are not
     * a fixed-size grid and the index has to be decoded rather than pointed
     * at (the file is big-endian; `unsigned long` is 4 bytes here and 8 on
     * the host). */
    int n_sprites;
    gta_tile_sprite *sprites;
    unsigned char   *sprite_pixels;
    unsigned long    sprite_bytes;
    int sprite_numbers[GTA_TIL_SPRITE_TYPES];

    /* The car table (format version 3). See the note below. */
    gta_car_info *cars;
    int n_cars;
} gta_tiles;

/* First index of a sprite category and how many it holds; the categories are
 * concatenated in the order of gta_sprite_type (gta_style.h). Kept here as
 * well as in the .GRY reader so the Amiga never needs gta_style.c. */
int gta_tiles_sprite_base(const gta_tiles *t, int type);
int gta_tiles_sprite_count(const gta_tiles *t, int type);

/* Returns 0 on success and prints the reason to stderr on failure. On failure
 * the struct is left safe to pass to gta_tiles_free(). */
int  gta_tiles_load(const char *path, gta_tiles *t);
void gta_tiles_free(gta_tiles *t);

/* Tile index 0 means "no face" everywhere in the map, so these are only ever
 * called with a non-zero index. They are macros because they sit in the
 * renderer's per-block path. */
#define gta_tiles_side(t, i)      ((t)->side   + (long)(i) * GTA_TILE_AREA)
#define gta_tiles_side_opaque(t, i) \
    ((t)->side_opaque ? (t)->side_opaque[i] : 0)
#define gta_tiles_lid(t, i, rot)  ((t)->lid + \
        (((long)(i) * GTA_LID_ROTATIONS + (rot)) * GTA_TILE_AREA))
#define gta_tiles_aux(t, i)       ((t)->aux    + (long)(i) * GTA_TILE_AREA)

void gta_tiles_describe(const gta_tiles *t, FILE *out);

#endif /* GTA_TILES_H */
