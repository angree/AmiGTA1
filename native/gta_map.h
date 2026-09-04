/* GTA 1 ".CMP" city map reader.
 *
 * Portable C89, same rules as gta_style.h: stdio only, no floats, compiles
 * unchanged on the host and on m68k-amigaos.
 *
 * THE MAP IS NOT DECOMPRESSED IN MEMORY, ON PURPOSE.
 * The city is 256x256x6 blocks. Expanding that to an array of block records
 * costs 256*256*6*8 = 3.1 MB, which is not affordable next to 1.5 MB of style
 * tiles on a 32 MB machine that also has to hold a chunky buffer, bitplanes and
 * the game itself. The file's own column compression is already compact -
 * nyc.cmp is 468 KB in total - so we keep it and pay a few instructions per
 * lookup instead. gta_map_block() is the accessor; treat it as cheap but not
 * free, and hoist it out of inner loops.
 *
 * Format knowledge: the official DMA Design "CityScape Data Structure"
 * (cds.doc, v3.10, 1995-03-21) and Carnage3D's GameMapManager.cpp
 * (ref/carnage3d/, MIT, (c) 2019 jericho). Implementation ours, MIT.
 */
#ifndef GTA_MAP_H
#define GTA_MAP_H

#include <stdio.h>

#define GTA_MAP_DIM     256
#define GTA_MAP_LAYERS  6

#define GTA_CMP_VERSION 331

/* Face order as stored in the file. */
typedef enum {
    GTA_FACE_W = 0,
    GTA_FACE_E,
    GTA_FACE_N,
    GTA_FACE_S,
    GTA_FACE_LID,
    GTA_FACE_COUNT
} gta_face;

/* One map block, 8 bytes, exactly as stored. The bitfields inside type_map and
 * type_map_ext are decoded by the accessors below rather than expanded into
 * separate members - at 393216 potential blocks, every byte matters. */
typedef struct {
    unsigned short type_map;
    unsigned char  type_map_ext;
    unsigned char  faces[GTA_FACE_COUNT];
} gta_block;

typedef struct {
    unsigned long version;
    int  style_number;
    int  sample_number;

    unsigned long *base;        /* GTA_MAP_DIM * GTA_MAP_DIM byte offsets */
    unsigned short *columns;
    unsigned long   column_words;
    gta_block      *blocks;
    unsigned long   block_count;
} gta_map;

int  gta_map_load(const char *path, gta_map *m);
void gta_map_free(gta_map *m);

/* Fetch the block at (x, y, z). Returns 0 and leaves *out zeroed for air or
 * out-of-range coordinates, 1 when a block was written. */
int  gta_map_block(const gta_map *m, int x, int y, int z, gta_block *out);

/* Height of the column at (x, y): the number of layers that hold a block,
 * counting up from the ground. 0 means the column is entirely air. */
int  gta_map_column_height(const gta_map *m, int x, int y);

/* WHICH WAY A RAMP GOES UP - 0 N, 64 E, 128 S, 192 W - or -1 if this block is
 * not a ramp at all.
 *
 * `type_map` bits 8..13 are the slope: 0 flat, 1..8 a 26-degree ramp, 9..40 a
 * 7-degree one in eight steps, 41..44 a 45-degree one, four directions each.
 * Carnage3D groups them N/S/W/E; that the letter is the direction of ASCENT
 * comes out of the map itself, and getting it backwards sends anything using
 * it down where it should go up. `gtadump slopes` shows the run at
 * (11..18, 44) rising eastward with the value climbing 33 to 40 - the "E"
 * group in ascending order - and the run at x=25..32 doing the same westward.
 *
 * THIS LIVES HERE AND NOT IN gta_player.c because the walker is no longer the
 * only thing that climbs: a car has to find the same ramps, and two copies of
 * a table this easy to get backwards is one copy too many. */
int gta_map_slope_up_dir(const gta_map *m, int bx, int by, int z);

/* Is this ramp block at its HIGHEST step - the one whose surface reaches the
 * top of its own layer?
 *
 * A ramp climbs over several blocks: the 7-degree group takes eight of them,
 * and only the last stands a full layer high. That distinction is what lets
 * anything stand at layer z over the top of a ramp whose block is at z-1,
 * while still refusing the layer above the MIDDLE of the ramp, where the
 * surface is nowhere near that high. */
int gta_map_slope_is_top(const gta_map *m, int bx, int by, int z);

/* The compass point a movement is mostly along: 0 N, 64 E, 128 S, 192 W.
 * Beside the two above because it is what their answers are compared with. */
int gta_map_step_dir(long dx, long dy);

/* --- block attribute accessors (see cds.doc for the meaning of each) --- */
#define gta_block_slope(b)        (((b)->type_map >> 8) & 0x3f)
#define gta_block_lid_rotation(b) (((b)->type_map >> 14) & 0x03)
#define gta_block_ground_type(b)  (((b)->type_map >> 4) & 0x07)
#define gta_block_is_flat(b)      (((b)->type_map & 0x80) != 0)
#define gta_block_remap(b)        (((b)->type_map_ext >> 3) & 0x03)
#define gta_block_flip_tb(b)      (((b)->type_map_ext & 0x20) != 0)
#define gta_block_flip_lr(b)      (((b)->type_map_ext & 0x40) != 0)
#define gta_block_is_railway(b)   (((b)->type_map_ext & 0x80) != 0)

/* ROAD DIRECTIONS - the four lowest bits of type_map, one per compass point.
 *
 * These are what traffic drives along, and they were sitting in a field this
 * reader already parsed and simply did not decode. Bit set means "traffic may
 * leave this block in that direction"; a one-way street has one bit, a
 * two-way road has the opposing pair, a junction has three or four.
 *
 * THE SAME SHIFT AS THE GROUND TYPE APPLIES. The CityScape spec says the road,
 * water, field, pavement, DIRECTION, railway and traffic-light bits are stored
 * in the block ABOVE the one holding the graphic - which is exactly what this
 * port found on its own for the ground type (gta_player.h: the type is on the
 * block the player OCCUPIES, not the one underneath). So a direction bit is
 * read from the same block a car occupies, and no extra correction is needed.
 *
 * Names are ours and follow gta_trig.h's compass, not the file's: "up" is the
 * bit the spec calls up, which is -y, which is NORTH. */
#define gta_block_dir_north(b)  (((b)->type_map & 0x01) != 0)
#define gta_block_dir_south(b)  (((b)->type_map & 0x02) != 0)
#define gta_block_dir_west(b)   (((b)->type_map & 0x04) != 0)
#define gta_block_dir_east(b)   (((b)->type_map & 0x08) != 0)
#define gta_block_dirs(b)       ((b)->type_map & 0x0F)

/* type_map_ext bits 0..2. 1 is a traffic light; 4..7 are train stations and
 * turns. Named from Carnage3D's eTrafficHint (MIT). */
#define gta_block_traffic_hint(b) ((b)->type_map_ext & 0x07)

void gta_map_describe(const gta_map *m, FILE *out);

#endif /* GTA_MAP_H */
