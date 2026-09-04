/* The navigation grid. Read gta_nav.h first - it says where this comes from
 * and why 384 KB is affordable when 3.1 MB is not.
 *
 * Licence: MIT (ours).
 */
#include <stdlib.h>
#include <string.h>

#include "gta_nav.h"

/* The block's low byte, plus the original's slope bit.
 *
 * `type_map & 0x7f` is directions, ground type and the flat flag; the original
 * throws the flat flag away and puts "this block is a ramp" in bit 7 instead,
 * which is the more useful question for something deciding where to drive. */
static unsigned char nav_byte(const gta_map *m, int bx, int by, int bz)
{
    gta_block b;

    if (!gta_map_block(m, bx, by, bz, &b))
        return 0;

    /* AND A RAILWAY BLOCK IS NOTHING AT ALL, which is our deviation from the
     * original's grid and it is a deliberate one.
     *
     * The elevated railway's own blocks carry ground type ROAD - correct, a
     * train runs on them - so "is it road?" says yes, and cars were parked on
     * the tracks and driven along a viaduct one block wide. That was found by
     * the placement test and dealt with by an `is_railway()` beside every
     * ground check; but `is_railway()` walks a map COLUMN, and once the ramp
     * rule started asking about neighbouring blocks on three layers it was
     * costing a fifth of the traffic tick on the 68020 - 1080 us a tick became
     * 1318.
     *
     * A grid that already exists to answer "may a car drive here" is the right
     * place to answer it. The blocks are zeroed rather than merely stripped of
     * their ground type so that nothing routes across them either. Trains,
     * when they exist, will read the map and not this. */
    if (gta_block_is_railway(&b))
        return 0;

    return (unsigned char)((b.type_map & 0x7f) |
                           (gta_block_slope(&b) ? 0x80 : 0));
}

int gta_nav_build(gta_nav *nav, const gta_map *m)
{
    int x, y, z;

    nav->b = (unsigned char *)malloc((size_t)GTA_NAV_BYTES);
    if (!nav->b)
        return 1;
    nav->map = m;

    /* The walk order is z innermost because that is how the map stores a
     * column: one column lookup then six blocks out of it. Doing it the other
     * way round would re-walk the same column six times over. */
    for (y = 0; y < GTA_MAP_DIM; y++)
        for (x = 0; x < GTA_MAP_DIM; x++)
            for (z = 0; z < GTA_MAP_LAYERS; z++)
                nav->b[((long)z << 16) | ((long)y << 8) | x] =
                    nav_byte(m, x, y, z);
    return 0;
}

void gta_nav_free(gta_nav *nav)
{
    free(nav->b);
    nav->b = NULL;
    nav->map = NULL;
}

void gta_nav_update(gta_nav *nav, const gta_map *m, int bx, int by, int bz)
{
    if (!nav->b) return;
    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM) return;
    if (bz < 0 || bz >= GTA_MAP_LAYERS) return;
    nav->b[((long)bz << 16) | ((long)by << 8) | bx] = nav_byte(m, bx, by, bz);
}

unsigned char gta_nav_at(const gta_nav *nav, int bx, int by, int bz)
{
    if (!nav->b) return 0;
    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM) return 0;
    if (bz < 0 || bz >= GTA_MAP_LAYERS) return 0;
    return nav->b[((long)bz << 16) | ((long)by << 8) | bx];
}
