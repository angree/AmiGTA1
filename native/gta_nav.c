/* The navigation grid. Read gta_nav.h first - it says where this comes from
 * and why 384 KB is affordable when 3.1 MB is not.
 *
 * Licence: MIT (ours).
 */
#include <stdio.h>
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

    if (!gta_map_block(m, bx, by, bz, &b)) {
        /* NOT IN THE FILE. An empty block inside a surface is still that
         * surface - see gta_map_ground_type(), and the invisible wall on
         * the pavement at (25,17) that it exists to answer. It gets the
         * ground type and no direction bits: a car may stand on it and be
         * pushed across it, and no route is planned through it. */
        return gta_map_ground_type(m, bx, by, bz) == 3
               ? (unsigned char)0x30 : (unsigned char)0;
    }

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

    /* A FIELD WITH DIRECTION ARROWS IS A DIRT ROAD (223). San Andreas'
     * countryside is ground type 4 with arrows painted on it - (53,172) is
     * 0x44, a westbound track - and everything that decides where a car
     * may drive asks "is it ROAD?". A SENDTO car parked on one sat there for
     * ever, re-asking for a route every tick. The arrows are the original's
     * own statement that cars drive here, so the grid calls it road. */
    if ((b.type_map & 0x70) == 0x40 && (b.type_map & 0x0f))
        return (unsigned char)(((b.type_map & 0x7f) & ~0x70) | 0x20 |
                               (gta_block_slope(&b) ? 0x80 : 0));

    return (unsigned char)((b.type_map & 0x7f) |
                           (gta_block_slope(&b) ? 0x80 : 0));
}

/* THE SOLID FLAT-FACE TILES, per style - the original's the original's table,
 * filled in the original's routine (the research notes 1.4). */
static const unsigned char solid1[] = { 46, 51, 55, 56, 108, 114, 168, 187,
                                        190, 194, 0 };
static const unsigned char solid2[] = { 6, 48, 51, 108, 111, 112, 126, 142,
                                        160, 193, 0 };
static const unsigned char solid3[] = { 69, 70, 100, 101, 102, 106, 124, 125,
                                        155, 156, 159, 160, 161, 162, 163,
                                        164, 169, 170, 184, 185, 186, 197, 0 };

const gta_nav *gta_nav_walls;

static int tile_solid(int style, int tile)
{
    const unsigned char *l = style == 2 ? solid2 : style == 3 ? solid3 : solid1;
    if (!tile) return 0;
    for (; *l; l++)
        if (*l == tile) return 1;
    return 0;
}

static int wall_bits(const gta_map *m, int style, int bx, int by, int bz)
{
    gta_block b;
    int w = 0;
    if (!gta_map_block(m, bx, by, bz, &b) || !(b.type_map & 0x80))
        return 0;
    if (tile_solid(style, b.faces[GTA_FACE_W])) w |= GTA_WALL_W;
    if (tile_solid(style, b.faces[GTA_FACE_N])) w |= GTA_WALL_N;
    return w;
}

int gta_nav_wall(const gta_nav *nav, int bx, int by, int bz)
{
    long i;
    if (!nav || !nav->walls) return 0;
    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM) return 0;
    if (bz < 0 || bz >= GTA_MAP_LAYERS) return 0;
    i = ((long)bz << 16) | ((long)by << 8) | bx;
    return (nav->walls[i >> 2] >> ((i & 3) * 2)) & 3;
}

void gta_nav_set_wall(gta_nav *nav, int bx, int by, int bz, int bits)
{
    long i;
    int sh;
    if (!nav || !nav->walls) return;
    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM) return;
    if (bz < 0 || bz >= GTA_MAP_LAYERS) return;
    i = ((long)bz << 16) | ((long)by << 8) | bx;
    sh = (int)(i & 3) * 2;
    nav->walls[i >> 2] = (unsigned char)((nav->walls[i >> 2] & ~(3 << sh)) |
                                         ((bits & 3) << sh));
}

/* THE FLAT FACES A SHOT GOES THROUGH, per style - the original's table holds 1 for
 * every other tile (the research notes 1.4). The post tile is in each
 * list: a shot passes the post that stops a car. */
static const unsigned char shot1[] = { 108, 109, 110, 168, 190, 194, 97, 0 };
static const unsigned char shot2[] = { 20, 58, 108, 112, 126, 160, 188, 193,
                                       21, 0 };
static const unsigned char shot3[] = { 101, 102, 140, 171, 197, 0 };

static int tile_in(const unsigned char *l, int tile)
{
    for (; *l; l++)
        if (*l == tile) return 1;
    return 0;
}

/* The wall bits a SHOT sees, read off the map: any face on the W or N edge
 * of a FLAT block whose tile is not in the list. A door's virtual tile is in
 * no list, so a door stops shots open or shut, as in the original (3.3). */
static int shot_bits(const gta_nav *nav, int bx, int by, int bz)
{
    gta_block b;
    const unsigned char *l;
    int w = 0;
    if (!nav->map || !gta_map_block(nav->map, bx, by, bz, &b) ||
        !(b.type_map & 0x80))
        return 0;
    l = nav->style == 2 ? shot2 : nav->style == 3 ? shot3 : shot1;
    if (b.faces[GTA_FACE_W] && !tile_in(l, b.faces[GTA_FACE_W])) w |= GTA_WALL_W;
    if (b.faces[GTA_FACE_N] && !tile_in(l, b.faces[GTA_FACE_N])) w |= GTA_WALL_N;
    return w;
}

/* A wall line x = X (W edge of column X/32) or y = Y crossed by the segment
 * inside its 32-px span - both ends of the span count, as in the original's routine.
 * Whole pixels, so the one multiply stays inside 32 bits. */
static int seg_cross(const gta_nav *nav, int bz, long x0, long y0,
                     long x1, long y1,
                     int (*bits)(const gta_nav *, int, int, int))
{
    int ax = (int)(x0 >> 16), ay = (int)(y0 >> 16);
    int qx = (int)(x1 >> 16), qy = (int)(y1 >> 16);
    int lo, hi, c;
    if (ax == qx && ay == qy)
        return 0;
    /* W edges: vertical lines at x = c*32 */
    lo = (ax < qx ? ax : qx) >> 5;
    hi = (ax < qx ? qx : ax) >> 5;
    for (c = lo; c <= hi + 1; c++) {
        int X = c * 32, y, r;
        if ((ax < X) == (qx < X)) continue;
        y = ay + (int)((long)(qy - ay) * (X - ax) / (qx - ax));
        r = y >> 5;
        if (bits(nav, c, r, bz) & GTA_WALL_W) return 1;
        if ((y & 31) == 0 && (bits(nav, c, r - 1, bz) & GTA_WALL_W))
            return 1;
    }
    /* N edges: horizontal lines at y = c*32 */
    lo = (ay < qy ? ay : qy) >> 5;
    hi = (ay < qy ? qy : ay) >> 5;
    for (c = lo; c <= hi + 1; c++) {
        int Y = c * 32, x, r;
        if ((ay < Y) == (qy < Y)) continue;
        x = ax + (int)((long)(qx - ax) * (Y - ay) / (qy - ay));
        r = x >> 5;
        if (bits(nav, r, c, bz) & GTA_WALL_N) return 1;
        if ((x & 31) == 0 && (bits(nav, r - 1, c, bz) & GTA_WALL_N))
            return 1;
    }
    return 0;
}

int gta_nav_wall_cross(const gta_nav *nav, int bz, long x0, long y0,
                       long x1, long y1)
{
    if (!nav || !nav->walls)
        return 0;
    return seg_cross(nav, bz, x0, y0, x1, y1, gta_nav_wall);
}

/* The bullet's test, the original's routine with no posts. Read straight off the map
 * rather than kept in a grid: a dozen shots a tick, two blocks each. */
int gta_nav_shot_cross(const gta_nav *nav, int bz, long x0, long y0,
                       long x1, long y1)
{
    if (!nav || bz < 0 || bz >= GTA_MAP_LAYERS)
        return 0;
    return seg_cross(nav, bz, x0, y0, x1, y1, shot_bits);
}

/* The style's post tiles - the original's table[style], and 180 as well in San
 * Andreas (the original's routine). A face whose tile is in the solid list is a wall,
 * not a post, even where the numbers meet. */
static int tile_post(int style, int tile)
{
    if (style == 2) return tile == 21 || tile == 180;
    if (style == 3) return tile == 140;
    return tile == 97;
}

static int post_bits(const gta_map *m, int style, int bx, int by, int bz)
{
    gta_block b;
    int w = 0, t;
    if (!gta_map_block(m, bx, by, bz, &b) || !(b.type_map & 0x80))
        return 0;
    t = b.faces[GTA_FACE_W];
    if (t && !tile_solid(style, t) && tile_post(style, t)) w |= GTA_WALL_W;
    t = b.faces[GTA_FACE_N];
    if (t && !tile_solid(style, t) && tile_post(style, t)) w |= GTA_WALL_N;
    return w;
}

static int post_at(const gta_nav *nav, int bx, int by, int bz)
{
    unsigned long key = ((unsigned long)bz << 16) | ((unsigned long)by << 8) | bx;
    int lo = 0, hi = nav->n_posts - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        unsigned long k = nav->posts[mid] >> 2;
        if (k == key) return (int)(nav->posts[mid] & 3);
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* The point (px,py), whole px, inside the car's box grown by the post's own
 * half size - the original's exact shape test, near enough for a square
 * 4 px across. */
static int post_in_box(long px, long py, long cx, long cy, long fx, long fy,
                       long rx, long ry, int hl, int hw)
{
    long dx = px - cx, dy = py - cy, a, b;
    a = (dx * fx + dy * fy) >> 14;
    b = (dx * rx + dy * ry) >> 14;
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    return a <= hl + 2 && b <= hw + 2;
}

int gta_nav_post_hit(const gta_nav *nav, int bz, long cx, long cy,
                     long fx, long fy, long rx, long ry, int hl, int hw)
{
    int r, x0, x1, y0, y1, x, y;
    long px = cx >> 16, py = cy >> 16;
    if (!nav || !nav->n_posts)
        return 0;
    r = (hl > hw ? hl : hw) + 4;
    x0 = (int)((px - r) >> 5); x1 = (int)((px + r) >> 5) + 1;
    y0 = (int)((py - r) >> 5); y1 = (int)((py + r) >> 5) + 1;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            int w = post_at(nav, x, y, bz);
            if (!w) continue;
            if ((w & GTA_WALL_W) &&
                post_in_box((long)x * 32, (long)y * 32 + 16, px, py,
                            fx, fy, rx, ry, hl, hw))
                return 1;
            if ((w & GTA_WALL_N) &&
                post_in_box((long)x * 32 + 16, (long)y * 32, px, py,
                            fx, fy, rx, ry, hl, hw))
                return 1;
        }
    return 0;
}

int gta_nav_build(gta_nav *nav, const gta_map *m)
{
    int x, y, z;

    nav->walls = NULL;          /* before anything can fail: free() reads it */
    nav->posts = NULL;
    nav->n_posts = 0;
    nav->b = (unsigned char *)malloc((size_t)GTA_NAV_BYTES);
    if (!nav->b)
        return 1;
    memset(nav->b, 0, (size_t)GTA_NAV_BYTES);   /* the two spare layers, see .h */
    nav->map = m;

    /* The walk order is z innermost because that is how the map stores a
     * column: one column lookup then six blocks out of it. Doing it the other
     * way round would re-walk the same column six times over. */
    for (y = 0; y < GTA_MAP_DIM; y++)
        for (x = 0; x < GTA_MAP_DIM; x++)
            for (z = 0; z < GTA_MAP_LAYERS; z++)
                nav->b[((long)z << 16) | ((long)y << 8) | x] =
                    nav_byte(m, x, y, z);

    /* THE THIN WALLS - a quarter of the grid's size, two bits a cell. A
     * machine that cannot spare 96 KB plays without them, as before. */
    nav->style = m->style_number;
    nav->walls = (unsigned char *)malloc((size_t)(GTA_NAV_BYTES / 4));
    if (nav->walls) {
        memset(nav->walls, 0, (size_t)(GTA_NAV_BYTES / 4));
        for (z = 0; z < GTA_MAP_LAYERS; z++)
            for (y = 0; y < GTA_MAP_DIM; y++)
                for (x = 0; x < GTA_MAP_DIM; x++) {
                    int w = wall_bits(m, nav->style, x, y, z);
                    if (w) gta_nav_set_wall(nav, x, y, z, w);
                }
    }
    /* THE POSTS: counted, then listed in key order, which is the walk order
     * z, y, x - so the list comes out sorted with no sort. */
    {
        int n = 0;
        for (z = 0; z < GTA_MAP_LAYERS; z++)
            for (y = 0; y < GTA_MAP_DIM; y++)
                for (x = 0; x < GTA_MAP_DIM; x++)
                    if (post_bits(m, nav->style, x, y, z)) n++;
        if (n)
            nav->posts = (unsigned long *)malloc((size_t)n * sizeof(unsigned long));
        if (nav->posts) {
            for (z = 0; z < GTA_MAP_LAYERS; z++)
                for (y = 0; y < GTA_MAP_DIM; y++)
                    for (x = 0; x < GTA_MAP_DIM; x++) {
                        int w = post_bits(m, nav->style, x, y, z);
                        if (w)
                            nav->posts[nav->n_posts++] =
                                ((((unsigned long)z << 16) |
                                  ((unsigned long)y << 8) | x) << 2) | w;
                    }
        }
        printf("gta: nav - %d posts on the map\n", nav->n_posts);
        for (x = 0; x < nav->n_posts && x < 4; x++)
            printf("gta: nav - post %d at (%lu,%lu) layer %lu edge %s\n", x,
                   (nav->posts[x] >> 2) & 255, (nav->posts[x] >> 10) & 255,
                   nav->posts[x] >> 18,
                   (nav->posts[x] & GTA_WALL_W) ? "W" : "N");
    }
    gta_nav_walls = nav;
    return 0;
}

void gta_nav_free(gta_nav *nav)
{
    free(nav->b);
    nav->b = NULL;
    free(nav->walls);
    nav->walls = NULL;
    free(nav->posts);
    nav->posts = NULL;
    nav->n_posts = 0;
    if (gta_nav_walls == nav) gta_nav_walls = NULL;
    nav->map = NULL;
}

void gta_nav_update(gta_nav *nav, const gta_map *m, int bx, int by, int bz)
{
    if (!nav->b) return;
    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM) return;
    if (bz < 0 || bz >= GTA_MAP_LAYERS) return;
    nav->b[((long)bz << 16) | ((long)by << 8) | bx] = nav_byte(m, bx, by, bz);
    gta_nav_set_wall(nav, bx, by, bz, wall_bits(m, nav->style, bx, by, bz));
}

unsigned char gta_nav_at(const gta_nav *nav, int bx, int by, int bz)
{
    if (!nav->b) return 0;
    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM) return 0;
    if (bz < 0 || bz >= GTA_MAP_LAYERS) return 0;
    return nav->b[((long)bz << 16) | ((long)by << 8) | bx];
}
