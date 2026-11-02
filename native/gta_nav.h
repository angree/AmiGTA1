/* The navigation grid - one byte per map block, and the thing every driving
 * decision reads.
 *
 * THIS IS THE ORIGINAL'S OWN STRUCTURE, not an invention of this port.
 * The original walks the whole map once at load and writes exactly this byte
 * per block, then keeps it in step whenever a block changes. Every AI query
 * in the game reads one indexed byte instead of walking a column - the
 * traffic code, the route finder and the traffic lights all use it and none
 * of them touch the map itself.
 *
 *     nav[z << 16 | y << 8 | x] = (type_map & 0x7f) | (slope ? 0x80 : 0)
 *
 *     bits 0..3   road directions   1 = N, 2 = S, 4 = W, 8 = E
 *     bits 4..6   ground type       2 = road, 3 = pavement
 *     bit  7      this block has a slope
 *
 * WHY THIS IS AFFORDABLE WHEN A FULL BLOCK ARRAY IS NOT. gta_map.h explains
 * that the map is deliberately left compressed: expanding it to 8-byte records
 * costs 3.1 MB, which does not fit next to the tiles on a 32 MB machine. This
 * is a TWELFTH of that - 256 x 256 x 6 = 384 KB - and it buys the same thing
 * for everything that only needs to know "may I drive here and which way".
 *
 * Portable C89, no floats, no Amiga headers: the same rules as the renderer.
 *
 * Licence: MIT (ours).
 */
#ifndef GTA_NAV_H
#define GTA_NAV_H

#include "gta_map.h"

#define GTA_NAV_LAYERS_ALLOC GTA_MAP_LAYERS
#define GTA_NAV_BYTES ((long)GTA_MAP_DIM * GTA_MAP_DIM * GTA_NAV_LAYERS_ALLOC)

typedef struct {
    unsigned char *b;           /* NULL until gta_nav_build() succeeds */
    /* THE MAP THE GRID WAS BUILT FROM, kept so that the few questions the
     * byte cannot answer do not need a second parameter threaded through
     * every caller.
     *
     * There is exactly one such question and it is ramps: the grid says
     * "this cell is air", but the cell above the TOP of a ramp is where a
     * car or a walker legitimately stands once it has climbed. That fact
     * lives in the block's slope field, not in the byte. Read-only, and
     * never followed when it is NULL. */
    const gta_map *map;
    /* THE THIN WALLS (229, the research notes). The original's walls
     * are of two kinds: a BUILDING block (ground type 5, which the byte
     * above already says) and a SOLID FLAT FACE - a fence, a garage door, a
     * railing - on the W or N edge of a block with the FLAT bit, whose tile
     * is in the style's short list. Two bits a cell: 1 = the W edge is a
     * wall, 2 = the N edge. 96 KB; a flat face's E/S are its own W/N seen
     * from behind, so two bits are the whole of it. */
    unsigned char *walls;
    int style;
    /* THE POSTS (WALLS_DOORS.md 2.3): a flat face showing the style's post
     * tile stops a CAR in an 8x8 px square in the middle of that edge, and
     * nothing else. Few of them, so a sorted list of (z,y,x,edge) keys
     * rather than two more bits in every cell. */
    unsigned long *posts;
    int n_posts;
} gta_nav;

#define GTA_WALL_W 1
#define GTA_WALL_N 2

/* The two wall bits of a cell, 0 off the map. */
int  gta_nav_wall(const gta_nav *nav, int bx, int by, int bz);
/* Set them - for a DOOR, whose face is solid only while it is shut. */
void gta_nav_set_wall(gta_nav *nav, int bx, int by, int bz, int bits);
/* Does the segment (x0,y0)-(x1,y1), 16.16 world px, cross a wall on layer
 * bz? The original's test: an edge crossing a wall line inside its span. */
int  gta_nav_wall_cross(const gta_nav *nav, int bz, long x0, long y0,
                        long x1, long y1);
/* Does a car's box - centre (cx,cy) 16.16 px, unit heading (fx,fy) and
 * right (rx,ry) in Q14, half length hl and half width hw in px - touch a
 * post on layer bz? */
int  gta_nav_post_hit(const gta_nav *nav, int bz, long cx, long cy,
                      long fx, long fy, long rx, long ry, int hl, int hw);
/* The same for a SHOT: every flat face stops it bar the style's short list
 * of see-through tiles (railings, the post), and a door stops it open or
 * shut. */
int  gta_nav_shot_cross(const gta_nav *nav, int bz, long x0, long y0,
                        long x1, long y1);
/* The grid the game is playing on, for code that is handed only the map
 * (the player's walk). Set by gta_nav_build(). */
extern const gta_nav *gta_nav_walls;

/* Build the grid from a loaded map. Returns 0 on success, non-zero if the
 * 384 KB could not be allocated - which is not fatal to the caller, but every
 * accessor below then answers "nothing here", so traffic simply does not run. */
int  gta_nav_build(gta_nav *nav, const gta_map *m);
void gta_nav_free(gta_nav *nav);

/* Refresh one block, for when the map changes under us (the original does this
 * from set_block; nothing in this port changes the map yet, and it is here so
 * that when something does, it is one call rather than a stale grid). */
void gta_nav_update(gta_nav *nav, const gta_map *m, int bx, int by, int bz);

/* The byte for a block, or 0 - which reads as "not drivable, no ground" - for
 * anything off the map or before the grid is built. One test at the top rather
 * than a bounds check in every caller. */
unsigned char gta_nav_at(const gta_nav *nav, int bx, int by, int bz);

/* The same lookup as a macro, for the traffic tick's hot paths - on a 68020
 * the call and prologue cost as much as the lookup (measured 552 calls per
 * tick from the drive loop). Semantically identical to gta_nav_at: the
 * unsigned compares are the two-sided bounds tests, 0 off the map or before
 * the grid exists. Arguments are evaluated more than once - pass plain
 * variables, never expressions with side effects. */
#define gta_nav_at_m(nav, bx, by, bz)     (((nav)->b != 0 &&       (unsigned)(bx) < (unsigned)GTA_MAP_DIM &&       (unsigned)(by) < (unsigned)GTA_MAP_DIM &&       (unsigned)(bz) < (unsigned)GTA_MAP_LAYERS)      ? (nav)->b[((long)(bz) << 16) | ((long)(by) << 8) | (long)(bx)]      : (unsigned char)0)

/* THE CHEAPER READ, for the traffic tick (PERF.md 7 step 4, PROGRESS 183).
 * Two tests instead of four: (x | y) has a bit above the low eight exactly
 * when either is negative or 256 or more, so one AND is the whole x/y
 * bounds test; z keeps its own. The b != 0 test is dropped - the caller
 * must know the grid exists, and every traffic caller tests g_nav once.
 * Same answer as gta_nav_at_m for every input. (A version that masked x
 * and y to the grid and wrapped to the far edge was tried first; it
 * CHANGED the traffic on all four seeds - the far edge is not all sea -
 * and a merged x/y test inside is_junction() changed it too, which is not
 * understood and is why is_junction keeps its two compares.) */
#define gta_nav_at_f(nav, bx, by, bz) \
    ((((bx) | (by)) & ~255) == 0 && (unsigned)(bz) < (unsigned)GTA_MAP_LAYERS \
     ? (nav)->b[((long)(bz) << 16) | ((long)(by) << 8) | (long)(bx)] \
     : (unsigned char)0)

#define gta_nav_dirs(v)    ((v) & 0x0f)
#define gta_nav_ground(v)  (((v) >> 4) & 0x07)
#define gta_nav_sloped(v)  (((v) & 0x80) != 0)

/* The four direction bits, in the map's own order. */
#define GTA_NAV_N  0x01
#define GTA_NAV_S  0x02
#define GTA_NAV_W  0x04
#define GTA_NAV_E  0x08

#endif /* GTA_NAV_H */
