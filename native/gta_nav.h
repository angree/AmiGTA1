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

#define GTA_NAV_BYTES ((long)GTA_MAP_DIM * GTA_MAP_DIM * GTA_MAP_LAYERS)

typedef struct {
    unsigned char *b;           /* NULL until gta_nav_build() succeeds */
} gta_nav;

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

#define gta_nav_dirs(v)    ((v) & 0x0f)
#define gta_nav_ground(v)  (((v) >> 4) & 0x07)
#define gta_nav_sloped(v)  (((v) & 0x80) != 0)

/* The four direction bits, in the map's own order. */
#define GTA_NAV_N  0x01
#define GTA_NAV_S  0x02
#define GTA_NAV_W  0x04
#define GTA_NAV_E  0x08

#endif /* GTA_NAV_H */
