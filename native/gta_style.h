/* GTA 1 style file reader - the 8-bit ".GRY" variant.
 *
 * Portable C89. stdio only, no floats, no dynamic C++ machinery: this file
 * compiles unchanged on the host and on m68k-amigaos.
 *
 * WHY THIS EXISTS SEPARATELY FROM THE Carnage3D REFERENCE
 * ------------------------------------------------------
 * Carnage3D (ref/carnage3d/, MIT, (c) 2019 jericho) reads only the 24-bit
 * ".G24" styles: it hardcodes version 336 and builds its filenames as
 * "STYLE%03d.G24".  Our port targets the 8-bit ".GRY" styles (version 325)
 * because they are already palettised for a 256-colour AGA screen.  The block
 * storage layout below is the same in both and was taken from Carnage3D's
 * StyleData.cpp; the header layout is not, and was derived from the files.
 *
 * Licence: MIT (ours).  Attribution for the layout knowledge: Carnage3D, MIT.
 */
#ifndef GTA_STYLE_H
#define GTA_STYLE_H

#include <stdio.h>

#include "gta_car.h"

/* A block (map tile) is 64x64 palette indices. */
#define GTA_BLOCK_DIM   64
#define GTA_BLOCK_AREA  (GTA_BLOCK_DIM * GTA_BLOCK_DIM)

/* Blocks are stored in 256x256 pages holding a 4x4 grid of blocks. */
#define GTA_PAGE_DIM    256
#define GTA_PAGE_SIZE   (GTA_PAGE_DIM * GTA_PAGE_DIM)
#define GTA_BLOCKS_PER_PAGE 16

#define GTA_STYLE_VERSION_GRY 325

typedef enum {
    GTA_BLOCK_SIDE = 0,     /* vertical faces of the world */
    GTA_BLOCK_LID,          /* horizontal faces - roads, roofs */
    GTA_BLOCK_AUX,          /* everything else */
    GTA_BLOCK_TYPE_COUNT
} gta_block_type;

/* The 13 dwords at the start of a .GRY.  The twelve sizes sum exactly to the
 * file size minus this header, which is how the layout was confirmed:
 * for style001.gry, 2708036 + 52 == 2708088.
 *
 * Fields 4..6 are where .GRY departs from .G24: the 24-bit format spends six
 * fields on CLUT pages there, the 8-bit format needs far less.  Field 4 is
 * 768 == 256*3, a VGA palette, and is treated as such.  The two after it are
 * carried through so section offsets stay correct even though we do not yet
 * interpret them. */
typedef struct {
    unsigned long version;
    unsigned long side_size;
    unsigned long lid_size;
    unsigned long aux_size;
    unsigned long anim_size;
    unsigned long palette_size;     /* 768 for .GRY */
    unsigned long unknown_a;        /* 65536 = 256 remap tables of 256 bytes */
    unsigned long unknown_b;        /* 1024 */
    unsigned long object_info_size;
    unsigned long car_size;
    unsigned long sprite_info_size;
    unsigned long sprite_graphics_size;
    unsigned long sprite_numbers_size;
} gta_style_header;

typedef struct {
    gta_style_header hdr;

    int side_blocks;
    int lid_blocks;
    int aux_blocks;
    int total_blocks;               /* rounded up to a multiple of 4 */

    unsigned char *blocks;          /* raw paged block data */
    unsigned long  blocks_len;

    unsigned char palette[768];     /* RGB triplets, already scaled to 0..255 */
    int palette_was_6bit;           /* the source file stored 0..63 */

    unsigned char *sprite_graphics; /* raw, paged the same way as blocks */
    unsigned long  sprite_graphics_len;

    struct gta_sprite *sprites;
    int sprite_count;

    int sprite_numbers[21];         /* GTA_SPR_TYPE_COUNT; see below */

    struct gta_car_info *cars;      /* the car table; see below */
    int car_count;

    /* THE PALETTE REMAP TABLES - what the header called `unknown_a`.
     *
     * 256 tables of 256 bytes: table `n` maps every palette index to another
     * one, and drawing a sprite through it recolours it. This is how one
     * pedestrian sheet dresses a whole city and how one car sprite comes in
     * a dozen colours.
     *
     * Read out of style001.gry: 94 of the 256 differ from the identity, in two
     * runs. **1..43 are the vehicles'** - the car table's own `remap8` bytes
     * index exactly this range (0, 3, 4, 15..19, 32..34, 38 appear there) -
     * and **128..187 are the pedestrians'**, which matches Carnage3D's
     * MAX_PED_REMAPS of 64 reserved at the top of the table. The rest are the
     * identity and recolour nothing.
     *
     * The .G24 format spends six header fields on CLUT pages for the same job;
     * the 8-bit .GRY needs one 64 KB block, and it is the same 64 KB the
     * loader used to skip past with an fseek. */
    unsigned char *remaps;          /* GTA_REMAP_COUNT * 256, or NULL */
    int remap_count;
} gta_style;

#define GTA_REMAP_COUNT   256
#define GTA_REMAP_CAR_LO    1
#define GTA_REMAP_CAR_HI   43
#define GTA_REMAP_PED_LO  128
#define GTA_REMAP_PED_HI  187

/* One sprite: 10 bytes in the file, plus 6 per delta.
 *
 *   u8  w, u8 h, u8 delta_count, u8 pad
 *   u16 size          bytes per frame; equals w*h, which is what let us find
 *                     this record layout in the first place
 *   u8  page_x, u8 page_y
 *   u16 page
 *
 * The 24-bit .G24 record is 12 bytes because it carries a CLUT index as well.
 * The 8-bit .GRY has one global palette and drops that field - which is why
 * reusing the .G24 layout desynchronises after 38 sprites.
 *
 * Sprites live in the sprite_graphics section, paged 256x256 exactly like the
 * block data - but unlike blocks a sprite is an arbitrary w x h rectangle at
 * (page_x, page_y) inside page `page`, not a fixed 64x64 cell.
 *
 * Deltas are the small overlays GTA uses for car damage and opening doors. We
 * record how many there are but do not apply them yet; the count is needed
 * regardless, because the records are variable-length and cannot be indexed
 * without walking them. */
struct gta_sprite {
    unsigned char  w, h;
    unsigned char  delta_count;
    unsigned short size;            /* bytes per frame, should be w*h */
    unsigned char  page_x, page_y;
    unsigned short page;
};

/* The sprite_numbers section: 21 little-endian u16 counts, one per category,
 * in this exact order. The sprite array is not tagged - it is simply the 21
 * categories laid end to end - so this table is the ONLY thing that says which
 * of the 1009 sprites is a pedestrian and which is a bus. Order taken from
 * Carnage3D's eSpriteType (GameDefs.h, MIT); the counts come from the file.
 *
 * 21 * 2 == 42, and style001.gry's sprite_numbers_size is exactly 42, which is
 * the check that the order has the right LENGTH. Nothing in the file confirms
 * the order itself; a wrong one shows up immediately as a car where a person
 * should be. */
typedef enum {
    GTA_SPR_ARROW = 0, GTA_SPR_DIGIT, GTA_SPR_BOAT, GTA_SPR_BOX,
    GTA_SPR_BUS, GTA_SPR_CAR, GTA_SPR_OBJECT, GTA_SPR_PED,
    GTA_SPR_SPEEDO, GTA_SPR_TANK, GTA_SPR_TRAFFIC_LIGHT, GTA_SPR_TRAIN,
    GTA_SPR_TRDOOR, GTA_SPR_BIKE, GTA_SPR_TRAM, GTA_SPR_WRECKED_CAR,
    GTA_SPR_WBUS, GTA_SPR_EX, GTA_SPR_TUMCAR, GTA_SPR_TUMTRUCK,
    GTA_SPR_FERRY,
    GTA_SPR_TYPE_COUNT
} gta_sprite_type;

extern const char *const gta_sprite_type_name[GTA_SPR_TYPE_COUNT];

/* Sprite category for a vtype, or GTA_SPR_TYPE_COUNT if it is not a known
 * class. Juggernaut front and back are both drawn from the car sprites. */
gta_sprite_type gta_vehicle_sprite_type(int vtype);
const char     *gta_vehicle_class_name(int vtype);

/* Returns 0 on success, non-zero on failure, and prints the reason to stderr.
 * On failure the struct is left safe to pass to gta_style_free(). */
int  gta_style_load(const char *path, gta_style *st);
void gta_style_free(gta_style *st);

/* Number of blocks of one type. */
int  gta_style_block_count(const gta_style *st, gta_block_type type);

/* Copy one 64x64 block into dst, which must hold GTA_BLOCK_AREA bytes.
 * dst_stride is the row stride in dst (use GTA_BLOCK_DIM for a tight copy).
 * Returns 0 on success. */
int  gta_style_get_block(const gta_style *st, gta_block_type type, int index,
                         unsigned char *dst, int dst_stride);

/* Copy sprite `index` into dst at the given row stride. Palette index 0 is
 * GTA's transparent colour and is skipped rather than written, so dst keeps
 * whatever background it already held. Returns 0 on success. */
int  gta_style_get_sprite(const gta_style *st, int index,
                          unsigned char *dst, int dst_stride);

/* First sprite index of a category, and how many it has. The sprite array is
 * the 21 categories concatenated in enum order, so the base is just the sum of
 * the counts before it. Returns 0 for an unknown type. */
int  gta_style_sprite_base(const gta_style *st, gta_sprite_type type);
int  gta_style_sprite_count(const gta_style *st, gta_sprite_type type);

/* Human-readable dump of the header and derived counts. */
void gta_style_describe(const gta_style *st, FILE *out);

#endif /* GTA_STYLE_H */
