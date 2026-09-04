/* The 2.5D city renderer. Read gta_render.h first, and Phase 4 of PLAN.md for
 * the reasoning; this file is the obedient half.
 *
 * Everything is 16.16 fixed point in a signed 32-bit int, there is no division
 * outside gta_render_init(), and the pixel loops step a source index with a
 * single add. That is not micro-optimisation ahead of a measurement - it is the
 * shape the projection has anyway, because a grid level projects to a REGULAR
 * grid and regular grids are walked by adding.
 *
 * Licence: MIT (ours).
 */
#include <stdlib.h>
#include <string.h>

#include "gta_render.h"
#include "gta_traffic.h"

#define TILE  GTA_TILE_DIM      /* 32 */
#define FP    16

/* Zoom is CONTINUOUS: any whole number of screen pixels per block between
 * GTA_ZOOM_MIN and GTA_ZOOM_MAX. It started as eight fixed notches and that was
 * wrong - GTA's camera slides smoothly, pulling back as the car speeds up, and
 * a staircase of eight steps does not look like that.
 *
 * The pre-scaled lid cache survives this because it is keyed on the level and
 * thrown away whenever the zoom changes: holding a zoom key rebuilds it every
 * frame, which costs the scaling of the couple of dozen tiles actually on
 * screen. Zooming is a little slower than sitting still, and settles the
 * moment the key is released.
 *
 * 32 is the default, and it is the size the art was drawn for: GTA's blocks
 * are 64x64 and the original ran at 640x480, which is 10 blocks across - the
 * same field of view as 32-pixel blocks on our 320x200. */
#define ZOOM_DEFAULT GTA_TILE_DIM

/* Tile pitch of grid level g, 16.16.
 *
 * (zoom * (CAM_H - GREF)) << 16 is 31 457 280 at the default zoom and 58 982
 * 400 at the widest notch, so this stays a plain 32-bit unsigned divide - no
 * 64-bit helper, which matters because m68k-amigaos would call __udivdi3 for
 * one. */
static long level_step(int zoom_px, int g, int flat, int cam_h)
{
    /* Flat 2D is one line, because that is all the projection is: every level
     * at the same pitch is a camera infinitely far away. gta_render.h lists
     * the three things that then fall out of draw_block on their own. It is a
     * runtime flag and not a build option because a player should be able to
     * try both without a rebuild - and because the benchmark has to measure
     * both in one run to be comparable. */
    if (flat)
        return (long)zoom_px << FP;
    /* cam_h is in QUARTER grid levels - see GTA_CAM_H. The original's camera
     * lands between 6.25 and 8.25 levels depending on zoom mode, and whole
     * levels cannot express that. */
    return (((long)zoom_px * (cam_h - 4 * GTA_GREF)) << FP) / (cam_h - 4 * g);
}

/* AND THE SAME PITCH DOWN THE SCREEN, WHICH IS FIVE SIXTHS OF IT.
 *
 * Taken from the original's own behaviour rather than guessed - checked twice,
 * independently. The projection is
 *
 *   *x = (((wx - camx) / z + 0x7f) * zoom >> 16) + centre_x;
 *   *y = ((zoom * ((wy - camy) / z + 0x7f) >> 16) * 5) / 6 + centre_y;
 *
 * and the routine that fills the lattice of screen coordinates for one grid
 * level walks it with `x += step * 0x40` against
 * `y += (step * 0x140) / 6` - 64 across against 53.33 down.
 *
 * See gta_view.stepy for why it is there at all and what it costs the rest of
 * the renderer.
 *
 * The multiply is safe in 32 bits: the largest step this renderer can produce
 * is the widest zoom at the lowest camera, 64 * 17 = 1088 pixels, which is
 * 71.3 million in 16.16 - times five is 357 million, an order of magnitude
 * inside a signed long. */
static long level_step_y(int zoom_px, int g, int flat, int cam_h)
{
    return (level_step(zoom_px, g, flat, cam_h) * 5) / 6;
}

/* Defined with the quad blitters, wanted here as well. */
static long tex_inc(const gta_view *v, int n);

void gta_render_set_zoom(gta_view *v, int px)
{
    if (px < GTA_ZOOM_MIN) px = GTA_ZOOM_MIN;
    if (px > GTA_ZOOM_MAX) px = GTA_ZOOM_MAX;
    v->zoom_px = px;
}

int gta_render_zoom(gta_view *v, int delta)
{
    gta_render_set_zoom(v, v->zoom_px + delta);
    return v->zoom_px;
}

/* THE CAMERA HEIGHT, ADJUSTABLE AT RUNTIME - see GTA_CAM_H for what it does and
 * for the fact that the shipped constant came out of the 1995 prototype rather
 * than out of GTA.
 *
 * It is a knob rather than a constant because two confident guesses at the right
 * value were both wrong (the notes, "THE PROJECTION IS WRONG"), and the person
 * who can actually judge it has the original running beside the port. Finding it
 * by eye in one sitting beats another afternoon of my arithmetic.
 *
 * THE LOWER CLAMP IS NOT TASTE. level_step() divides by (cam_h - g) and g runs
 * to GTA_GRID_LEVELS-1, so cam_h must stay strictly above that or the renderer
 * takes a divide-by-zero and the machine goes down. */
void gta_render_set_cam_h(gta_view *v, int h)
{
    if (h < GTA_CAM_H_MIN) h = GTA_CAM_H_MIN;
    if (h > GTA_CAM_H_MAX) h = GTA_CAM_H_MAX;
    v->cam_h = h;
}

int gta_render_cam_h(gta_view *v, int delta)
{
    gta_render_set_cam_h(v, v->cam_h + delta);
    return v->cam_h;
}

void gta_render_init(gta_view *v, const gta_map *map, const gta_tiles *tiles)
{
    v->ov_tr = 0;
    v->ov_on = 0;

    int i;

    memset(v, 0, sizeof(*v));
    v->map = map;
    v->tiles = tiles;

    /* The only divisions in the renderer. recip[n] converts "walk n
     * destination pixels" into "step this much of a 32-pixel tile each time". */
    v->recip[0] = 0;
    for (i = 1; i <= GTA_RECIP_MAX; i++)
        v->recip[i] = ((long)TILE << FP) / i;

    v->zoom_px = ZOOM_DEFAULT;
    v->cam_h   = GTA_CAM_H;

    /* The pre-scaled lid cache. One pointer per (level, lid, rotation); the
     * pixels come out of a single arena with a bump pointer, so there is no
     * allocator in the frame and nothing to free but these two blocks. */
    v->lc_slots = GTA_GRID_LEVELS * tiles->n_lid * GTA_LID_ROTATIONS;
    v->lc_slot = (unsigned char **)calloc((size_t)v->lc_slots,
                                          sizeof(unsigned char *));
    v->lc_arena = (unsigned char *)malloc(GTA_LIDCACHE_BYTES);
    if (!v->lc_slot || !v->lc_arena) {
        /* Not fatal - every blit has a scaling path that does not need this. */
        free(v->lc_slot);
        free(v->lc_arena);
        v->lc_slot = NULL;
        v->lc_arena = NULL;
    }
    v->lc_zoom = -1;

    gta_render_look_at_block(v, GTA_MAP_DIM / 2, GTA_MAP_DIM / 2);
}

void gta_render_free(gta_view *v)
{
    free(v->lc_slot);
    free(v->lc_arena);
    free(v->spr_scratch);
    v->lc_slot = NULL;
    v->lc_arena = NULL;
    v->spr_scratch = NULL;
    v->spr_scratch_cap = 0;
    v->lc_used = 0;
}

/* Rebuild the per-level tile sizes and throw the cache away. Called when the
 * zoom changes, which is the only thing that alters them. */
static void lidcache_reset(gta_view *v)
{
    int g;

    for (g = 0; g < GTA_GRID_LEVELS; g++) {
        long sx = level_step(v->zoom_px, g, v->flat_2d, v->cam_h);
        long sy = level_step_y(v->zoom_px, g, v->flat_2d, v->cam_h);
        v->lc_w[g] = (int)((sx + 0xFFFFL) >> FP);     /* ceil, in pixels */
        v->lc_h[g] = (int)((sy + 0xFFFFL) >> FP);
    }
    /* And the row map for the full-width path - see gta_view.lc_vrow. */
    for (g = 0; g < GTA_GRID_LEVELS; g++) {
        int hh = v->lc_h[g], y;
        v->lc_vrow_ok[g] = 0;
        if (hh > 0 && hh <= GTA_VROW_MAX) {
            long inc = tex_inc(v, hh), acc = 0;
            for (y = 0; y < hh; y++) {
                int sy = (int)(acc >> FP);
                if (sy > TILE - 1) sy = TILE - 1;
                v->lc_vrow[g][y] = (unsigned char)sy;
                acc += inc;
            }
            v->lc_vrow_ok[g] = 1;
        }
    }

    if (v->lc_slot)
        memset(v->lc_slot, 0, (size_t)v->lc_slots * sizeof(unsigned char *));
    v->lc_used = 0;
    v->lc_fills = 0;
    v->lc_full = 0;
    v->lc_zoom = v->zoom_px;
    v->lc_cam_h = v->cam_h;
    v->lc_flat = v->flat_2d;
}

/* The lid tile for (tex, rot) already scaled to this level's size, or NULL if
 * it is not cacheable and the caller must scale per pixel. */
static const unsigned char *lid_scaled(gta_view *v, int tex, int rot, int lg)
{
    const unsigned char *src = gta_tiles_lid(v->tiles, tex, rot);
    int w = v->lc_w[lg];
    int h = v->lc_h[lg];
    int slot, x, y;
    unsigned char *dst;
    long incx, incy, need;

    /* A level whose two steps are both a whole tile needs no scaling at all.
     * With the 5/6 squash in place that no longer happens - the street is 32
     * across and 27 down - so the street now costs one cache entry it used not
     * to. It keeps its memcpy blit, which is what actually mattered. */
    if (w == TILE && h == TILE)
        return src;
    if (!v->lc_arena || w <= 0 || h <= 0)
        return NULL;

    slot = (lg * v->tiles->n_lid + tex) * GTA_LID_ROTATIONS + rot;
    if (v->lc_slot[slot])
        return v->lc_slot[slot];

    need = (long)w * h;
    if (v->lc_used + (unsigned long)need > GTA_LIDCACHE_BYTES) {
        v->lc_full++;
        return NULL;
    }

    dst = v->lc_arena + v->lc_used;
    v->lc_used += (unsigned long)need;
    v->lc_slot[slot] = dst;
    v->lc_fills++;

    incx = tex_inc(v, w);
    incy = tex_inc(v, h);
    for (y = 0; y < h; y++) {
        int sy = (int)(((long)y * incy) >> FP);
        const unsigned char *srow;
        long ax = 0;

        if (sy > TILE - 1) sy = TILE - 1;
        srow = src + (long)sy * TILE;
        for (x = 0; x < w; x++) {
            int sx = (int)(ax >> FP);
            if (sx > TILE - 1) sx = TILE - 1;
            *dst++ = srow[sx];
            ax += incx;
        }
    }
    return v->lc_slot[slot];
}

void gta_render_target(gta_view *v, unsigned char *dst, int w, int h, int pitch)
{
    v->dst = dst;
    v->dst_w = w;
    v->dst_h = h;
    v->dst_pitch = pitch;
}

void gta_render_look_at_block(gta_view *v, int bx, int by)
{
    v->cam_x = ((long)bx * TILE + TILE / 2) << FP;
    v->cam_y = ((long)by * TILE + TILE / 2) << FP;
}

void gta_render_move(gta_view *v, int dx_px, int dy_px)
{
    long lo = 0;
    long hi = ((long)GTA_MAP_DIM * TILE) << FP;

    v->cam_x += (long)dx_px << FP;
    v->cam_y += (long)dy_px << FP;
    if (v->cam_x < lo) v->cam_x = lo;
    if (v->cam_y < lo) v->cam_y = lo;
    if (v->cam_x >= hi) v->cam_x = hi - 1;
    if (v->cam_y >= hi) v->cam_y = hi - 1;
}

/* ------------------------------------------------------------------ blits */

/* A lid: an axis-aligned w x h rectangle of a 32x32 tile.
 *
 * `masked` decides whether palette index 0 is transparent, and it is not a
 * style question - it is what the map means:
 *
 *   a normal lid is the ground or a roof. It covers what is under it, so it
 *   is drawn opaque and gets the memcpy fast path below, which is where most
 *   of a frame's pixels are written.
 *
 *   a FLAT block's lid is a decal laid on the surface below it - the map holds
 *   a whole layer of them above the roofs. In nyc.cmp, column (56,58) has its
 *   roof on layer 4 (lid 22, grey concrete) and a flat block on layer 5 whose
 *   lid 144 is 2269 transparent pixels out of 4096. Drawn opaque, that decal
 *   paints a black square over the roof - which is exactly the black holes the
 *   first render of this city had, and the same holes the flat top-down
 *   viewer in gtadump has always had.
 */
static void blit_lid(gta_view *v, const unsigned char *tile,
                     int x0, int y0, int w, int h, int masked)
{
    int cx0, cx1, cy0, cy1, x, y;
    long inc_x, inc_y;

    if (v->debug_no_blits) return;
    if (w <= 0 || h <= 0) return;
    if (x0 >= v->dst_w || y0 >= v->dst_h) return;
    if (x0 + w <= 0 || y0 + h <= 0) return;

    cx0 = x0 < 0 ? 0 : x0;
    cy0 = y0 < 0 ? 0 : y0;
    cx1 = x0 + w; if (cx1 > v->dst_w) cx1 = v->dst_w;
    cy1 = y0 + h; if (cy1 > v->dst_h) cy1 = v->dst_h;

    /* Fast path: the reference grid level, whole and on screen. This is the
     * street, and it is most of the frame. */
    if (!masked && w == TILE && h == TILE && cx0 == x0 && cy0 == y0 &&
        cx1 == x0 + TILE && cy1 == y0 + TILE) {
        unsigned char *d = v->dst + (long)y0 * v->dst_pitch + x0;
        for (y = 0; y < TILE; y++) {
            memcpy(d, tile, TILE);
            d += v->dst_pitch;
            tile += TILE;
        }
        return;
    }

    inc_x = tex_inc(v, w);
    inc_y = tex_inc(v, h);

    for (y = cy0; y < cy1; y++) {
        long ay = (long)(y - y0) * inc_y;
        int sy = (int)(ay >> FP);
        const unsigned char *srow;
        unsigned char *d;
        long ax;

        if (sy > TILE - 1) sy = TILE - 1;
        srow = tile + (long)sy * TILE;
        d = v->dst + (long)y * v->dst_pitch + cx0;
        ax = (long)(cx0 - x0) * inc_x;
        if (masked) {
            for (x = cx0; x < cx1; x++) {
                int sx = (int)(ax >> FP);
                unsigned char c;
                if (sx > TILE - 1) sx = TILE - 1;
                c = srow[sx];
                if (c) *d = c;
                d++;
                ax += inc_x;
            }
        } else {
            for (x = cx0; x < cx1; x++) {
                int sx = (int)(ax >> FP);
                if (sx > TILE - 1) sx = TILE - 1;
                *d++ = srow[sx];
                ax += inc_x;
            }
        }
    }
}

/* An opaque copy of an already-correctly-sized tile: no scaling, no test, one
 * memcpy per row. This is what the pre-scaled cache exists to make possible,
 * and it is where the frame's pixels want to end up - the scaling loop above
 * costs about six operations a pixel and this costs one.
 *
 * `src_pitch` is the source tile's row stride, which is its width. */
static void blit_copy(gta_view *v, const unsigned char *src, int src_pitch,
                      int x0, int y0, int w, int h)
{
    int cx0, cx1, cy0, cy1, y, run;
    const unsigned char *s;
    unsigned char *d;

    if (v->debug_no_blits) return;
    if (w <= 0 || h <= 0) return;
    if (x0 >= v->dst_w || y0 >= v->dst_h) return;
    if (x0 + w <= 0 || y0 + h <= 0) return;

    cx0 = x0 < 0 ? 0 : x0;
    cy0 = y0 < 0 ? 0 : y0;
    cx1 = x0 + w; if (cx1 > v->dst_w) cx1 = v->dst_w;
    cy1 = y0 + h; if (cy1 > v->dst_h) cy1 = v->dst_h;
    run = cx1 - cx0;
    if (run <= 0) return;

    s = src + (long)(cy0 - y0) * src_pitch + (cx0 - x0);
    d = v->dst + (long)cy0 * v->dst_pitch + cx0;
    for (y = cy0; y < cy1; y++) {
        memcpy(d, s, (size_t)run);
        s += src_pitch;
        d += v->dst_pitch;
    }
}

/* --------------------------------------------------- the two quad blitters */

/* THE WALLS ARE TRAPEZOIDS, NOT RECTANGLES, AND THAT IS THE WHOLE POINT.
 *
 * A wall stands between two grid levels, and the two levels have different
 * pitches. Its four projected corners are therefore
 *
 *      (fa, ya)          (fa, ya + say)        <- on grid z,   pitch say
 *      (fb, yb)          (fb, yb + sby)        <- on grid z+1, pitch sby
 *
 * which is a trapezoid whose two parallel sides are VERTICAL, of different
 * lengths, at different heights. Drawing it as one axis-aligned rectangle of
 * the upper level's extent - which is what this renderer did until now - is
 * exactly the fault the developer described from the game, in these words:
 *
 *   "each successive row of windows is bigger, but each individual row has no
 *    perspective - top and bottom are spaced linearly, instead of a line
 *    running from the bottom of the building that also draws the sides of the
 *    windows."
 *
 * Of course: a rectangle per block can only change size BETWEEN blocks. It
 * cannot converge WITHIN one, so every row of windows came out flat and the
 * building read as a stack of sprites rather than a solid.
 *
 * THE SHAPE IS THE ORIGINAL'S OWN. The original has exactly two rasterisers
 * behind its block drawer, and the six coordinates each is handed say which:
 *
 *   x0 x1 | y0 y1 y2 y3   two x, four y  -> vertical parallels
 *   x0 x1 x2 x3 | y0 y1   four x, two y  -> horizontal parallels
 *
 * The first walks COLUMNS, stepping the destination by its pitch and
 * interpolating the top and bottom edges as it goes; the second
 * walks rows through a scanline table. Both take the texture step for the
 * inner span out of a reciprocal table indexed by the span length, so the
 * texture always fills the span exactly: affine per span, one add per pixel,
 * no divide anywhere in a loop. That is what is reproduced here.
 *
 * The two loops below are that pair. Neither is a general quad rasteriser and
 * neither needs to be: this projection only ever produces axis-parallel
 * trapezoids, because a grid level projects to a regular grid.
 */

/* (TILE << FP) / n at any n, without the table's ceiling.
 *
 * The table covers every span a quad on a 320x200 screen can need. The divide
 * is the escape hatch for the oversized ones a low camera makes - a wall on a
 * block five out spans |dbx| * (step[z+1] - step[z]), which runs into the
 * thousands - and taking a divide there is far cheaper than the wrong picture
 * the old clamp drew, which was the texture running out part way across and
 * the last row smeared over the rest. */
static long tex_inc(const gta_view *v, int n)
{
    if (n <= 0)             return 0;
    if (n <= GTA_RECIP_MAX) return v->recip[n];
    return ((long)TILE << FP) / n;
}

/* The same thing with the table already in a register. Every span in both
 * blitters below needs one, and a function call per span was showing up: at
 * -O1 gcc 6.5 does not inline across the `const gta_view *` indirection. */
#define TEX_INC(rec, n) \
    ((n) <= GTA_RECIP_MAX ? (rec)[n] : (((long)TILE << FP) / (n)))

/* WHY THESE TWO LOOPS ARE WRITTEN THE WAY THEY ARE - three things that each
 * cost real frames on the 68040 and none of which is visible in the C.
 *
 * 1. NOTHING IN THE INNER LOOP MAY TOUCH `v`. The first version stepped the
 *    destination with `d += v->dst_pitch`, and that is a memory load per
 *    PIXEL: `*d = c` writes through an unqualified pointer, so the compiler
 *    must assume it can alias `v->dst_pitch` and reloads it every iteration.
 *    The rectangle blitter this replaced never showed it because it walked the
 *    destination with `d++`. Everything the loops need is copied into a local
 *    first.
 *
 * 2. THE MIRROR IS AN XOR, NOT A BRANCH. The along-wall index runs 0..TILE-1
 *    (times the stride), and TILE-1 is all ones, so `cmax - ci` is exactly
 *    `ci ^ cmax` - and that holds for the strided case too, because there the
 *    index is a multiple of TILE and cmax is (TILE-1)*TILE, whose low bits are
 *    all clear. So a per-pixel conditional becomes one XOR with a value that
 *    is 0 when not mirrored.
 *
 * 3. THE CLAMP STAYS, and it is not paranoia. The accumulator starts at half a
 *    texel and steps TILE/(span+1), so the last sample reaches index TILE once
 *    the span passes 63 pixels - which at a low camera is most walls. It is a
 *    compare and a rarely-taken branch, which the 040 predicts away; removing
 *    it would need the span end special-cased and is not worth the second code
 *    path. */

/* A FULL-WIDTH LID, SCALED ONLY VERTICALLY, STRAIGHT OUT OF THE TILE SET.
 *
 * This exists because of the 5/6 squash, and it is worth its own routine.
 *
 * Before the squash a level whose step was a whole tile needed no scaling at
 * all: `lid_scaled()` returned the source tile itself and `blit_copy()`
 * memcpy'd 32 rows of 32 bytes out of the TILE SET - contiguous, hot, and
 * already in cache because every other tile blit that frame reads from the
 * same place. In FLAT 2D that was every level, so flat 2D used the lid cache
 * for nothing at all.
 *
 * With the squash the same tile is 32 wide and 27 tall, so `w == TILE &&
 * h == TILE` is never true, and every one of those lids started going through
 * the pre-scaled cache instead - reading from a 1.5 MB arena scattered across
 * memory. On the 68020 test machine, which has no data cache to spoil, that
 * cost nothing and the benchmark said flat 2D had got FASTER. On a real 030
 * with a 256-byte data cache it is a different machine entirely, and that is
 * where it was reported: flat 2D from 10+ fps to 6.5.
 *
 * So: when only the HEIGHT needs scaling, do not build a cache entry. Walk the
 * destination rows, pick the source row with the same reciprocal step the
 * cache would have used, and memcpy the full width from the original tile. One
 * index per ROW instead of one per pixel, no arena, and the source stays the
 * hot tile set.
 *
 * It is exact: the row chosen here is the row `lid_scaled()` would have
 * written into the cache entry, so the picture is identical - which
 * holecheck.sh and a pixel-for-pixel diff against the previous build both
 * confirm. */
static void blit_vscale(gta_view *v, const unsigned char *tile,
                        int x0, int y0, int w, int h, int lg)
{
    int cx0, cx1, cy0, cy1, y, run, pitch, sx;
    unsigned char *dst;

    if (v->debug_no_blits) return;
    if (w <= 0 || h <= 0) return;
    if (x0 >= v->dst_w || y0 >= v->dst_h) return;
    if (x0 + w <= 0 || y0 + h <= 0) return;

    cx0 = x0 < 0 ? 0 : x0;
    cy0 = y0 < 0 ? 0 : y0;
    cx1 = x0 + w; if (cx1 > v->dst_w) cx1 = v->dst_w;
    cy1 = y0 + h; if (cy1 > v->dst_h) cy1 = v->dst_h;
    run = cx1 - cx0;
    if (run <= 0) return;

    pitch = v->dst_pitch;
    dst   = v->dst + (long)cy0 * pitch + cx0;
    sx    = cx0 - x0;

    /* THE ROW MAP MAKES THIS A PURE COPY LOOP. One byte load per row instead
     * of a multiply, a shift and a clamp - see gta_view.lc_vrow. */
    if (v->lc_vrow_ok[lg]) {
        const unsigned char *vr = v->lc_vrow[lg] + (cy0 - y0);
        for (y = cy0; y < cy1; y++) {
            memcpy(dst, tile + ((long)*vr++ << 5) + sx, (size_t)run);
            dst += pitch;
        }
    } else {
        long inc = tex_inc(v, h);
        long acc = (long)(cy0 - y0) * inc;
        for (y = cy0; y < cy1; y++) {
            int sy = (int)(acc >> FP);
            if (sy > TILE - 1) sy = TILE - 1;
            memcpy(dst, tile + (long)sy * TILE + sx, (size_t)run);
            dst += pitch;
            acc += inc;
        }
    }
}

/* SPANS ARE INCLUSIVE OF BOTH ENDS, and that is load-bearing rather than
 * sloppy. Two faces of the same cube share the projection of its vertical
 * corner edge; one rasterises that diagonal by columns and the other by rows,
 * and two scan directions over one diagonal do not produce the same pixels.
 * Half-open spans leave a string of single-pixel holes down every building
 * corner; inclusive spans overlap by one pixel instead, and an overlap of wall
 * texture with wall texture is invisible. The original does the same - its
 * inner loops are `do { } while (0 < n--)`, i.e. n+1 pixels - off accumulators
 * biased by +0.5, exactly as these are. */

/* A trapezoid with VERTICAL parallel edges: the left edge is the segment
 * x = xl, y from yl0 to yl1; the right edge is x = xr, y from yr0 to yr1.
 * xr must be greater than xl.
 *
 * The outer loop is the COLUMN, so the texture's row index moves with x and
 * its column index moves with y down each span. `rstride` and `cstride` are
 * the two source strides: (TILE, 1) reads the tile the normal way round -
 * which is what a wall wants, because a wall tile is stored [height][along]
 * and this face's height axis IS the screen x axis - and (1, TILE) reads it
 * transposed, which is what a lid tilted along x wants.
 *
 * That also retires the transposed side tiles the old blit_wall_h needed:
 * scanning by column makes the NORMAL tile the contiguous one, which is
 * exactly why the original scans this face by column too. */
static void blit_quad_v(gta_view *v, const unsigned char *tex,
                        int rstride, int cstride,
                        int xl, int yl0, int yl1,
                        int xr, int yr0, int yr1,
                        int flip_v, int flip_u, int masked)
{
    int n, x, xs, xe, cmask, cmax, fx;
    int pitch, dh;
    unsigned char *dst;
    const long *rec;
    long d0, d1, a0, a1, av, iv;

    if (v->debug_no_blits) return;

    n = xr - xl;
    if (n <= 0) return;                  /* flat 2D collapses walls to this */
    if (xr < 0 || xl >= v->dst_w) return;
    if (v->dst_h <= 0) return;

    pitch = v->dst_pitch;
    dh    = v->dst_h;
    dst   = v->dst;
    rec   = v->recip;

    iv = TEX_INC(rec, n);
    d0 = (((long)(yr0 - yl0)) << FP) / n;
    d1 = (((long)(yr1 - yl1)) << FP) / n;
    a0 = ((long)yl0 << FP) + 0x8000L;
    a1 = ((long)yl1 << FP) + 0x8000L;
    av = 0x8000L;

    xs = xl;
    xe = xr;
    if (xs < 0) {                        /* clip by stepping, never by testing */
        long k = -(long)xs;
        a0 += d0 * k;
        a1 += d1 * k;
        av += iv * k;
        xs = 0;
    }
    if (xe > v->dst_w - 1) xe = v->dst_w - 1;

    cmask = ~(cstride - 1);
    cmax  = (TILE - 1) * cstride;
    fx    = flip_u ? cmax : 0;

    for (x = xs; x <= xe; x++) {
        int ytop = (int)(a0 >> FP);
        int ybot = (int)(a1 >> FP);
        int row  = (int)(av >> FP);

        if (row > TILE - 1) row = TILE - 1;
        if (row < 0) row = 0;
        if (flip_v) row = TILE - 1 - row;

        if (ybot >= ytop && ybot >= 0 && ytop < dh) {
            const unsigned char *srow = tex + (long)row * rstride;
            int cy0 = ytop < 0 ? 0 : ytop;
            int cy1 = ybot > dh - 1 ? dh - 1 : ybot;
            long iu = TEX_INC(rec, ybot - ytop + 1) * cstride;
            long au = 0x8000L * cstride + iu * (long)(cy0 - ytop);
            unsigned char *d = dst + (long)cy0 * pitch + x;
            int y;

            if (masked) {
                for (y = cy0; y <= cy1; y++) {
                    int ci = (int)(au >> FP) & cmask;
                    unsigned char c;
                    if (ci > cmax) ci = cmax;
                    c = srow[ci ^ fx];
                    if (c) *d = c;
                    d += pitch;
                    au += iu;
                }
            } else {
                for (y = cy0; y <= cy1; y++) {
                    int ci = (int)(au >> FP) & cmask;
                    if (ci > cmax) ci = cmax;
                    *d = srow[ci ^ fx];
                    d += pitch;
                    au += iu;
                }
            }
        }
        a0 += d0;
        a1 += d1;
        av += iv;
    }
}

/* A trapezoid with HORIZONTAL parallel edges: the top edge is y = yt, x from
 * xt0 to xt1; the bottom edge is y = yb, x from xb0 to xb1. yb must be
 * greater than yt.
 *
 * The mirror image of the routine above, and the cheaper of the two: the outer
 * loop is the ROW, so the destination run is contiguous and the source run is
 * contiguous with it. This is the north/south wall and the ramp that tilts
 * along y. */
static void blit_quad_h(gta_view *v, const unsigned char *tex,
                        int rstride, int cstride,
                        int yt, int xt0, int xt1,
                        int yb, int xb0, int xb1,
                        int flip_v, int flip_u, int masked)
{
    int n, y, ys, ye, cmask, cmax, fx;
    int pitch, dw;
    unsigned char *dst;
    const long *rec;
    long d0, d1, a0, a1, av, iv;

    if (v->debug_no_blits) return;

    n = yb - yt;
    if (n <= 0) return;
    if (yb < 0 || yt >= v->dst_h) return;
    if (v->dst_w <= 0) return;

    pitch = v->dst_pitch;
    dw    = v->dst_w;
    dst   = v->dst;
    rec   = v->recip;

    iv = TEX_INC(rec, n);
    d0 = (((long)(xb0 - xt0)) << FP) / n;
    d1 = (((long)(xb1 - xt1)) << FP) / n;
    a0 = ((long)xt0 << FP) + 0x8000L;
    a1 = ((long)xt1 << FP) + 0x8000L;
    av = 0x8000L;

    ys = yt;
    ye = yb;
    if (ys < 0) {
        long k = -(long)ys;
        a0 += d0 * k;
        a1 += d1 * k;
        av += iv * k;
        ys = 0;
    }
    if (ye > v->dst_h - 1) ye = v->dst_h - 1;

    cmask = ~(cstride - 1);
    cmax  = (TILE - 1) * cstride;
    fx    = flip_u ? cmax : 0;

    for (y = ys; y <= ye; y++) {
        int xlo = (int)(a0 >> FP);
        int xhi = (int)(a1 >> FP);
        int row = (int)(av >> FP);

        if (row > TILE - 1) row = TILE - 1;
        if (row < 0) row = 0;
        if (flip_v) row = TILE - 1 - row;

        if (xhi >= xlo && xhi >= 0 && xlo < dw) {
            const unsigned char *srow = tex + (long)row * rstride;
            int cx0 = xlo < 0 ? 0 : xlo;
            int cx1 = xhi > dw - 1 ? dw - 1 : xhi;
            long iu = TEX_INC(rec, xhi - xlo + 1) * cstride;
            long au = 0x8000L * cstride + iu * (long)(cx0 - xlo);
            unsigned char *d = dst + (long)y * pitch + cx0;
            int x;

            if (masked) {
                for (x = cx0; x <= cx1; x++) {
                    int ci = (int)(au >> FP) & cmask;
                    unsigned char c;
                    if (ci > cmax) ci = cmax;
                    c = srow[ci ^ fx];
                    if (c) *d = c;
                    d++;
                    au += iu;
                }
            } else {
                for (x = cx0; x <= cx1; x++) {
                    int ci = (int)(au >> FP) & cmask;
                    if (ci > cmax) ci = cmax;
                    *d++ = srow[ci ^ fx];
                    au += iu;
                }
            }
        }
        a0 += d0;
        a1 += d1;
        av += iv;
    }
}

/* -------------------------------------------------------------- the walk */

/* Interpolate between the two grid levels a block spans. `e` is a height in
 * EIGHTHS of the block: 0 is the bottom, 8 the top.
 *
 * This works because a grid level's projection is a uniform scale about the
 * screen centre, so it is linear in the level - the position of a point at a
 * fractional height is just the lerp of its position at the two whole levels.
 * That is what makes slopes almost free here: no new projection, no division,
 * one multiply and a shift. */
#define LERP8(a, b, e)  ((a) + ((((b) - (a)) * (long)(e)) >> 3))

/* Decode a slope type into the lid height at the low-coordinate edge and at the
 * high-coordinate edge, in eighths, plus the axis it tilts along.
 *
 * The 44 slope types are four directions x three pitches: 26 degrees in two
 * steps, 7 degrees in eight, and a single 45-degree block. The name of each
 * group is the side the surface rises TOWARD. Taken from Carnage3D's
 * GameMapHelpers.cpp, where the corner numbering is 0=SW 1=SE 4=NW 5=NE on the
 * top face - read off its face definitions, since it never says so.
 *
 * A block with no slope decodes to (8, 8), which makes the lid code below one
 * path rather than two: the interpolation collapses to the top grid level and
 * the street keeps its memcpy fast path. */
static void slope_heights(int slope, int *axis, int *e0, int *e1)
{
    *axis = 0; *e0 = 8; *e1 = 8;

    if (slope <= 0 || slope > 44)
        return;
    if (slope <= 2)       { *axis = 2; *e0 = (slope - 1 + 1) * 4; *e1 = (slope - 1) * 4; }
    else if (slope <= 4)  { *axis = 2; *e0 = (slope - 3) * 4;     *e1 = (slope - 3 + 1) * 4; }
    else if (slope <= 6)  { *axis = 1; *e0 = (slope - 5 + 1) * 4; *e1 = (slope - 5) * 4; }
    else if (slope <= 8)  { *axis = 1; *e0 = (slope - 7) * 4;     *e1 = (slope - 7 + 1) * 4; }
    else if (slope <= 16) { *axis = 2; *e0 = slope - 9 + 1;       *e1 = slope - 9; }
    else if (slope <= 24) { *axis = 2; *e0 = slope - 17;          *e1 = slope - 17 + 1; }
    else if (slope <= 32) { *axis = 1; *e0 = slope - 25 + 1;      *e1 = slope - 25; }
    else if (slope <= 40) { *axis = 1; *e0 = slope - 33;          *e1 = slope - 33 + 1; }
    else if (slope == 41) { *axis = 2; *e0 = 8; *e1 = 0; }
    else if (slope == 42) { *axis = 2; *e0 = 0; *e1 = 8; }
    else if (slope == 43) { *axis = 1; *e0 = 8; *e1 = 0; }
    else                  { *axis = 1; *e0 = 0; *e1 = 8; }
}

static void draw_block(gta_view *v, const gta_block *b,
                       long xa, long xb, long ya, long yb,
                       long sax, long sbx, long say, long sby,
                       int dbx, int dby, int z)
{
    const gta_tiles *t = v->tiles;
    int tex, flat, west, north;

    flat = gta_block_is_flat(b) ? 1 : 0;

    /* WHAT "FLAT" MEANS, because getting it wrong costs whole squares of city.
     *
     * A flat block is zero thickness HORIZONTALLY but full height vertically:
     * a plate standing in the block, not a cube and not a decal lying down.
     * GTA uses them for fences, fire escapes, shop fronts and railings - 11041
     * of them in nyc.cmp. So its east face coincides with its west face and its
     * south face with its north face, which is exactly what Carnage3D's
     * GameMapHelpers.cpp does ("should draw at W position" / "at N position").
     *
     * The first version of this renderer skipped flat blocks' side faces
     * altogether and drew only the lid. That is what left squares missing from
     * the edges of buildings - not a hole, because the ground behind stayed
     * visible, which is why holecheck.sh could not see it either. */
    west  = (dbx >= 0);
    north = (dby >= 0);

    /* --- the side facing the middle of the screen, east/west ---------------
     * At most one of the two can ever be seen, because the camera looks
     * straight down: a column to the right of centre shows its west face, one
     * to the left shows its east face. That is fact 2 of the design note doing
     * half the culling before any arithmetic.
     *
     * This face extrudes HORIZONTALLY on screen, so its trapezoid is the one
     * with vertical parallel edges: the base edge stands on grid z at screen
     * x = fa and is say tall, the top edge stands on grid z+1 at fb and is sby
     * tall. Those are four different numbers and that is the perspective. */
    {
        int far_plane;

        tex = b->faces[west ? GTA_FACE_W : GTA_FACE_E];
        if (!tex && flat)
            tex = b->faces[west ? GTA_FACE_E : GTA_FACE_W];

        /* Which of the block's two x planes the face sits on. A cube's east
         * face is at bx+1; a flat block's is at bx, with its west face. */
        far_plane = (!west && !flat);

        if (tex && tex < t->n_side) {
            long fa = xa + (far_plane ? sax : 0);   /* screen x on grid z   */
            long fb = xb + (far_plane ? sbx : 0);   /* screen x on grid z+1 */
            int xl, yl0, yl1, xr, yr0, yr1, flip_v, flip_a;

            /* Texture row 0 is the TOP of the wall, so whichever edge stands
             * on grid z+1 gets row 0 and the flip follows the direction the
             * block splays in - which is the sign of dbx. */
            if (fb >= fa) {
                xl  = (int)(fa >> FP);
                yl0 = (int)(ya >> FP);
                yl1 = (int)((ya + say) >> FP);
                xr  = (int)(fb >> FP);
                yr0 = (int)(yb >> FP);
                yr1 = (int)((yb + sby) >> FP);
                flip_v = 1;
            } else {
                xl  = (int)(fb >> FP);
                yl0 = (int)(yb >> FP);
                yl1 = (int)((yb + sby) >> FP);
                xr  = (int)(fa >> FP);
                yr0 = (int)(ya >> FP);
                yr1 = (int)((ya + say) >> FP);
                flip_v = 0;
            }

            /* THE SENSE OF `flat` HERE WAS INVERTED, and it is the mirrored
             * signs the developer photographed.
             *
             * A FLAT block is a billboard: one plane, and the map puts the
             * SAME tile on both of its faces - `gtadump findtile` on the two
             * lettered "KS" tiles shows every user of them is flat and carries
             * the tile on W and E (or N and S) alike, with the flip bit
             * choosing which way it reads. So both faces must be drawn the
             * SAME way round, or the sign reads backwards from one side.
             *
             * A CUBE is the opposite case: its west and east faces are two
             * different surfaces seen from opposite directions, so their
             * texture runs in opposite screen directions and the far one needs
             * the extra flip. */
            flip_a = west ? (gta_block_flip_lr(b) ? 1 : 0)
                          : ((gta_block_flip_lr(b) ? 1 : 0) != !flat);
            blit_quad_v(v, gta_tiles_side(t, tex), TILE, 1,
                        xl, yl0, yl1, xr, yr0, yr1, flip_v, flip_a,
                        !gta_tiles_side_opaque(t, tex));
            v->walls_drawn++;
        }
    }

    /* --- and the north/south one, the trapezoid the other way up ----------- */
    {
        int far_plane;

        tex = b->faces[north ? GTA_FACE_N : GTA_FACE_S];
        if (!tex && flat)
            tex = b->faces[north ? GTA_FACE_S : GTA_FACE_N];

        far_plane = (!north && !flat);

        if (tex && tex < t->n_side) {
            long ga = ya + (far_plane ? say : 0);
            long gb = yb + (far_plane ? sby : 0);
            int yt, xt0, xt1, ybt, xb0, xb1, flip_v, flip_a;

            if (gb >= ga) {
                yt  = (int)(ga >> FP);
                xt0 = (int)(xa >> FP);
                xt1 = (int)((xa + sax) >> FP);
                ybt = (int)(gb >> FP);
                xb0 = (int)(xb >> FP);
                xb1 = (int)((xb + sbx) >> FP);
                flip_v = 1;
            } else {
                yt  = (int)(gb >> FP);
                xt0 = (int)(xb >> FP);
                xt1 = (int)((xb + sbx) >> FP);
                ybt = (int)(ga >> FP);
                xb0 = (int)(xa >> FP);
                xb1 = (int)((xa + sax) >> FP);
                flip_v = 0;
            }

            flip_a = north ? (gta_block_flip_tb(b) ? 1 : 0)
                           : ((gta_block_flip_tb(b) ? 1 : 0) != flat);
            blit_quad_h(v, gta_tiles_side(t, tex), TILE, 1,
                        yt, xt0, xt1, ybt, xb0, xb1, flip_v, flip_a,
                        !gta_tiles_side_opaque(t, tex));
            v->walls_drawn++;
        }
    }

    /* THERE IS NO CORNER SQUARE ANY MORE, AND THAT IS THE POINT OF ALL THIS.
     *
     * While the two faces were rectangles they met at a convex corner leaving
     * a rectangular hole - the projection of the cube's vertical edge - and
     * this renderer used to fill it by stretching the east/west wall's
     * along-extent all the way down to the base grid.
     *
     * That patch scaled with (step[z+1] - step[z]), which is a few pixels at
     * sixteen grid levels and a few HUNDRED at the original's six and a bit.
     * It is what the developer reported as "roofs in the wrong place, roofs
     * vanishing, only at c8 and only close in": a slab of wall texture the
     * height of the splay, painted over whatever was beside it.
     *
     * Two true trapezoids need no patch. They share the projected corner edge
     * exactly - the segment from (xa,ya) to (xb,yb) - and lie on opposite
     * sides of it, so together with the lid they cover the whole silhouette
     * and overlap only on the one pixel the inclusive spans give them. */

    /* --- the lid ----------------------------------------------------------
     * A cube's lid is the top of the cube, grid z+1. A flat block's lid is a
     * marking on the surface it stands on, so palette index 0 stays
     * transparent - `nyc.cmp` carries a whole layer of these above the roofs,
     * and drawing them opaque paints black squares over the roof they belong
     * to.
     *
     * A FLAT lid needs no quad: a horizontal square at one grid level projects
     * to an axis-aligned RECTANGLE, because a grid level projects to a regular
     * grid. It is a rectangle rather than a square only because of the 5/6
     * squash on y. So the pre-scaled cache and its memcpy survive intact, and
     * they are most of the frame's pixels. Only a RAMP, whose two edges sit on
     * different heights, actually needs the quad blitters. */
    tex = b->faces[GTA_FACE_LID];
    if (tex && tex < t->n_lid) {
        int axis, e0, e1;
        int rot = gta_block_lid_rotation(b);

        if (flat) {
            /* A flat block's lid sits on TOP of its own cube, exactly like any
             * other lid - it is only the four SIDES that collapse onto one
             * plane, because the block has no horizontal thickness.
             *
             * Putting it on the bottom instead was wrong and the elevated
             * railway is what proved it: in nyc.cmp the track is a flat block
             * on layer 2 (column (112,49), lid 140), sitting over the road,
             * which is the lid of layer 1. On the bottom of its own cube the
             * track lands on grid 2 - the road surface - so the railway ran
             * along the street instead of above it. */
            axis = 0; e0 = 8; e1 = 8;
        } else {
            slope_heights(gta_block_slope(b), &axis, &e0, &e1);

            /* THE FAST PATH, and the one that matters: a plain flat lid on a
             * whole grid level. Its size on screen is the same wherever the
             * camera is (lc_w/lc_h, the ceilings of the level's two steps), so
             * it can be scaled once and copied for ever after. */
            if (axis == 0) {
                /* ONLY THE HEIGHT NEEDS SCALING: skip the cache entirely and
                 * copy rows out of the tile set. See blit_vscale() - this is
                 * every level in flat 2D and the reference level in 2.5D, and
                 * it is where the 030 lost its frames. */
                if (v->lc_w[z + 1] == TILE) {
                    blit_vscale(v, gta_tiles_lid(t, tex, rot),
                                (int)(xb >> FP), (int)(yb >> FP),
                                TILE, v->lc_h[z + 1], z + 1);
                    v->lids_drawn++;
                    return;
                }
                {
                    const unsigned char *pre = lid_scaled(v, tex, rot, z + 1);
                    if (pre) {
                        blit_copy(v, pre, v->lc_w[z + 1],
                                  (int)(xb >> FP), (int)(yb >> FP),
                                  v->lc_w[z + 1], v->lc_h[z + 1]);
                        v->lids_drawn++;
                        return;
                    }
                }
            }
        }

        if (axis == 0) {
            /* Flat, but either masked (a flat block's decal) or a cache miss.
             * Same rectangle, scaled per pixel. */
            int x0 = (int)(xb >> FP), y0 = (int)(yb >> FP);
            int x1 = (int)((xb + sbx) >> FP), y1 = (int)((yb + sby) >> FP);
            blit_lid(v, gta_tiles_lid(t, tex, rot),
                     x0, y0, x1 - x0, y1 - y0, flat);
        } else if (axis == 2) {
            /* A RAMP TILTING ALONG Y. Its low-y edge sits at height e0 and its
             * high-y edge at e1, so the two edges project at different scales
             * but each stays HORIZONTAL - the horizontal-parallel trapezoid.
             *
             * This used to be drawn as the bounding box of that trapezoid,
             * which over-covered by a couple of pixels along the tilted axis.
             * It is now the trapezoid itself, which is both correct and, on a
             * bridge deck, visibly straighter. */
            blit_quad_h(v, gta_tiles_lid(t, tex, rot), TILE, 1,
                        (int)(LERP8(ya, yb, e0) >> FP),
                        (int)(LERP8(xa, xb, e0) >> FP),
                        (int)(LERP8(xa + sax, xb + sbx, e0) >> FP),
                        (int)(LERP8(ya + say, yb + sby, e1) >> FP),
                        (int)(LERP8(xa, xb, e1) >> FP),
                        (int)(LERP8(xa + sax, xb + sbx, e1) >> FP),
                        0, 0, 0);
        } else {
            /* A RAMP TILTING ALONG X: vertical parallel edges, so the outer
             * loop is the column and the lid tile arrives with its two axes
             * the wrong way round. Swapping the strides reads it transposed,
             * which costs nothing but the AND already in that loop. */
            blit_quad_v(v, gta_tiles_lid(t, tex, rot), 1, TILE,
                        (int)(LERP8(xa, xb, e0) >> FP),
                        (int)(LERP8(ya, yb, e0) >> FP),
                        (int)(LERP8(ya + say, yb + sby, e0) >> FP),
                        (int)(LERP8(xa + sax, xb + sbx, e1) >> FP),
                        (int)(LERP8(ya, yb, e1) >> FP),
                        (int)(LERP8(ya + say, yb + sby, e1) >> FP),
                        0, 0, 0);
        }
        v->lids_drawn++;
    }
}


/* One block of one column, at one layer. Everything a block needs from the
 * projection is the position of its two grid levels and their tile pitches -
 * four multiplies - so this is cheap enough to call once per layer instead of
 * once per column. */
/* ------------------------------------------------------- resolution modes */

/* Blow a reduced-resolution frame up into the display buffer. See the note in
 * gta_render.h for what this does and does not save.
 *
 * The horizontal pass builds a 32-bit word from two source pixels and stores
 * it once, instead of storing two bytes. That halves the store count, and the
 * store count is the whole cost here - there is no arithmetic to speak of.
 * `unsigned int` and not `unsigned long`, because long is 4 bytes on m68k and
 * 8 on the host and this code has to write the same 4 bytes on both; that
 * exact confusion has already cost this project an afternoon in gta_map.c.
 *
 * The vertical pass is a plain memcpy of the row just written, which is why a
 * half-height mode is much the cheaper of the two to expand. */
void gta_render_expand(const unsigned char *src, int sw, int sh, int spitch,
                       unsigned char *dst, int dpitch, int sx, int sy)
{
    int y;

    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;

    for (y = 0; y < sh; y++) {
        const unsigned char *s = src + (long)y * spitch;
        unsigned char *d = dst + (long)(y * sy) * dpitch;

        if (sx == 1) {
            memcpy(d, s, (size_t)sw);
        } else if (((long)(d - (unsigned char *)0) & 3L) != 0) {
            /* A destination row that is not longword aligned. The 68020 will
             * do a misaligned longword store, slowly; a 68000 would take an
             * address error. Neither is worth risking for a case that only
             * arises if the platform layer hands back an odd chunky pointer or
             * pitch, so it falls back to bytes and stays correct. */
            int x;
            for (x = 0; x < sw; x++) {
                d[x * 2]     = s[x];
                d[x * 2 + 1] = s[x];
            }
        } else {
            /* Two source pixels -> one aligned longword. sw is even in every
             * mode the game offers (160), and the display buffer is aligned,
             * so the tail below never actually runs - it is there so that an
             * odd width degrades instead of losing a column. */
            unsigned int *d32 = (unsigned int *)(void *)d;
            int x;
            for (x = 0; x + 1 < sw; x += 2) {
                unsigned int a = s[x], b = s[x + 1];
                *d32++ = (a << 24) | (a << 16) | (b << 8) | b;
            }
            if (x < sw) {
                unsigned char *t = (unsigned char *)(void *)d32;
                t[0] = t[1] = s[x];
            }
        }

        if (sy == 2)
            memcpy(d + dpitch, d, (size_t)(sw * sx));
    }
}

/* ---------------------------------------------------------------- sprites */

int gta_render_add_sprite(gta_view *v, long wx, long wy, int layer, int grid,
                          int index, int angle)
{
    return gta_render_add_sprite_r(v, wx, wy, layer, grid, index, angle, 0);
}

/* ON A RAMP, THE SURFACE UNDER A SPRITE IS ITS OWN LAYER'S LID.
 *
 * A sprite on layer L is drawn at the START of pass L - after the lid of
 * layer L-1, which is the surface it stands on, and before anything at its
 * own height or above (see the layer loop). That is right on every flat
 * surface and wrong on every slope: a ramp block carries the road TYPE and
 * the sloping LID in the same block, so the surface the car is driving up is
 * painted in pass L, after the car, and the car vanishes for the length of
 * the ramp. Reported four times as "wjezdza sie pod most": the car drove into
 * the truss bridge at (32..25,44..47), disappeared under the ramp's own road
 * tiles, and came out on the deck a layer up.
 *
 * So a sprite standing on a slope goes in one pass later. The block under the
 * centre is not enough: a car is a block and a half long, and at the top of
 * the far ramp its nose is on the slope while its middle is still on the
 * deck - the layer follows the nose (gta_veh_layer), so for one block the car
 * would be drawn under the deck. Both ends of the sprite are tested. */
static int slope_under_sprite(const gta_view *v, long wx, long wy, int layer,
                              int index, int angle)
{
    const gta_tiles *t = v->tiles;
    long hl, dx, dy;
    int a;

    if (!v->map || layer < 0 || layer >= GTA_MAP_LAYERS)
        return 0;
    if (gta_map_slope_up_dir(v->map, (int)(wx >> 21), (int)(wy >> 21),
                             layer) >= 0)
        return 1;
    if (!t || !t->sprites || index < 0 || index >= t->n_sprites)
        return 0;
    hl = (long)t->sprites[index].h / 2;
    if (hl < TILE / 2)
        return 0;                       /* a man: his one block is enough */
    /* The art faces south, so the heading is the angle less that; the sine
     * is Q14 and world pixels times four is 16.16. Sign does not matter -
     * both ends are looked at. */
    a = (angle - 128) & 255;
    dx = gta_sin(a) * hl * 4;
    dy = gta_cos(a) * hl * 4;
    if (gta_map_slope_up_dir(v->map, (int)((wx + dx) >> 21),
                             (int)((wy - dy) >> 21), layer) >= 0)
        return 1;
    if (gta_map_slope_up_dir(v->map, (int)((wx - dx) >> 21),
                             (int)((wy + dy) >> 21), layer) >= 0)
        return 1;
    return 0;
}

int gta_render_add_sprite_r(gta_view *v, long wx, long wy, int layer, int grid,
                            int index, int angle, int remap)
{
    gta_sprite_req *sp;
    if (v->n_sprites >= GTA_MAX_SPRITES)
        return 0;
    if (slope_under_sprite(v, wx, wy, layer, index, angle))
        layer++;
    sp = &v->sprites[v->n_sprites++];
    sp->remap = remap;
    sp->delta = -1;
    sp->delta_mask = 0;
    sp->wx = wx;
    sp->wy = wy;
    sp->layer = layer;
    sp->grid  = grid;
    sp->index = index;
    sp->angle = angle & 255;
    return 1;
}

int gta_render_add_sprite_d(gta_view *v, long wx, long wy, int layer, int grid,
                            int index, int angle, int remap, int delta)
{
    if (!gta_render_add_sprite_r(v, wx, wy, layer, grid, index, angle, remap))
        return 0;
    v->sprites[v->n_sprites - 1].delta = delta;
    return 1;
}

int gta_render_add_sprite_dm(gta_view *v, long wx, long wy, int layer, int grid,
                             int index, int angle, int remap, int delta,
                             unsigned long delta_mask)
{
    if (!gta_render_add_sprite_r(v, wx, wy, layer, grid, index, angle, remap))
        return 0;
    v->sprites[v->n_sprites - 1].delta = delta;
    v->sprites[v->n_sprites - 1].delta_mask = delta_mask;
    return 1;
}

/* Rotate and scale one sprite into the frame.
 *
 * DESTINATION-DRIVEN, and that is the whole design. Walking the SOURCE and
 * scattering its pixels leaves holes wherever the sprite is magnified and
 * writes the same destination pixel repeatedly wherever it is minified; both
 * are wrong and the first is the class of bug this renderer has already paid
 * for twice. Walking the destination and reading back through the inverse
 * rotation writes every pixel of the sprite's footprint exactly once, with two
 * adds per pixel and no division at all inside the loop.
 *
 * SCALE. A sprite lives at SOURCE scale, 64 pixels to a block (gta_tiles.h),
 * while a tile is baked at 32. So one sprite pixel is step[grid]/64 screen
 * pixels, and the inverse - source pixels per screen pixel - is what the loop
 * steps by. The single division here is per sprite, not per pixel.
 *
 * The (1<<30) / x << 2 form avoids a 64-bit numerator: m68k-amigaos would call
 * __udivdi3 for one, and the two bits it gives up are worth about a thousandth
 * of a source pixel. */
/* (16.16 blocks) x (16.16 screen pixels per block) -> 16.16 screen pixels,
 * without ever leaving a signed 32-bit long.
 *
 * The whole-block part is multiplied at full precision - the caller bounds it
 * to +-64 blocks, and 64 times the largest step in this renderer is about half
 * a billion. The fractional part drops eight bits from each side first, so its
 * product is at most 255 * 32768. Nothing here can reach 2^31, which the code
 * this replaced could and did.
 *
 * The cost is an eighth of a pixel on the fraction, the same trade the camera
 * origin already makes one screen further up. */
static long blocks_to_px(long db, long step)
{
    long bi = db >> FP;                 /* whole blocks, sign-extending */
    long bf = db - (bi << FP);          /* 0..65535, never negative */
    return bi * step + ((bf >> 8) * (step >> 8));
}

void gta_render_sprite(gta_view *v, const gta_sprite_req *sp)
{
    const gta_tiles *t = v->tiles;
    const gta_tile_sprite *rec;
    const unsigned char *src;
    const unsigned char *rmp;
    long sstepx, sstepy, iscx, iscy, du_dx, du_dy, dv_dx, dv_dy;
    long u_row, v_row;
    int cs, sn, acs, asn;
    int sw, sh, aw, ah, dw, dh;
    int cx, cy, x0, y0, x1, y1, x, y;
    int cbx, cby;
    long dxb, dyb;

    if (v->debug_no_blits) return;
    if (!t || !t->sprites || sp->index < 0 || sp->index >= t->n_sprites)
        return;
    if (sp->grid < 0 || sp->grid >= GTA_GRID_LEVELS)
        return;

    rec = &t->sprites[sp->index];
    sw = (int)rec->w;
    sh = (int)rec->h;
    if (sw <= 0 || sh <= 0) return;
    src = t->sprite_pixels + rec->off;

    /* A DELTA MEANS THE SPRITE IS ASSEMBLED FIRST - an open door, a damage
     * panel. The base art in the tile set is shared by every car of the model
     * and must never be edited in place, so the overlay goes over a copy.
     *
     * The buffer grows on demand and is never shrunk: one sprite in a frame
     * has ever needed it (the car being entered), and a game where no door
     * opens never allocates it at all. If the allocation fails the sprite is
     * drawn plain, which is a shut door rather than a missing car. */
    if ((sp->delta >= 0 || sp->delta_mask) && t->delta_data) {
        unsigned long need = (unsigned long)sw * sh;
        if (v->spr_scratch_cap < need) {
            unsigned char *nb = (unsigned char *)malloc((size_t)need);
            if (nb) {
                free(v->spr_scratch);
                v->spr_scratch = nb;
                v->spr_scratch_cap = need;
            }
        }
        if (v->spr_scratch && v->spr_scratch_cap >= need) {
            unsigned long m = sp->delta_mask;
            if (sp->delta >= 0 && sp->delta < 32)
                m |= 1UL << sp->delta;
            gta_tiles_delta_apply_mask(t, sp->index, m, v->spr_scratch);
            src = v->spr_scratch;
        }
    }
    /* The remap table for this sprite, resolved once per sprite rather than
     * once per pixel. Table 0 is the identity in every style file, so "no
     * remap" and "remap 0" mean the same thing and neither costs a lookup. */
    rmp = (sp->remap > 0 && t->remaps && sp->remap < t->n_remaps)
        ? t->remaps + (long)sp->remap * GTA_TIL_REMAP_STRIDE : 0;

    /* Where the sprite's centre lands. ox[] is the screen x of the CAMERA
     * BLOCK's left edge at this level, so what is added is how many blocks away
     * the sprite is, times the level's step.
     *
     * THIS USED TO OVERFLOW, AND ONLY ON THE AMIGA, AND ONLY SIDEWAYS.
     *
     * It was written as `((dxb >> 8) * step) >> 8`, copied from the camera
     * fraction in gta_render_frame with a comment saying so. But that line is
     * safe only because ITS input is a fraction of one block: `fx >> 8` is at
     * most 255, which its own comment states. `dxb >> 8` is 256 per BLOCK, so
     * at four blocks it is 1024, and 1024 * a step of about 2.2 million is 2.3
     * billion - past the end of a signed 32-bit long. The pattern was reused
     * without its precondition.
     *
     * On the host `long` is 64 bits and nothing happened, which is why every
     * host picture in this project was right. On the Amiga the product wrapped
     * negative and the sprite was culled as off-screen.
     *
     * And it showed up HORIZONTALLY ONLY, which is what made it look like a
     * clipping bug rather than an arithmetic one: the screen is 320 wide and
     * 200 tall, so at zoom 32 a sprite can be five blocks away sideways but
     * only three vertically. Cars vanished at the left and right edges and
     * never at the top or bottom - reported exactly that way.
     *
     * The split below multiplies the whole blocks at full precision and the
     * fraction at reduced precision, so neither term can overflow: whole
     * blocks are bounded by the cull above, and (bf >> 8) * (step >> 8) is at
     * most 255 * 32768. */
    cbx = (int)(v->cam_x >> (FP + 5));
    cby = (int)(v->cam_y >> (FP + 5));
    dxb = (sp->wx >> 5) - ((long)cbx << FP);      /* 16.16 blocks */
    dyb = (sp->wy >> 5) - ((long)cby << FP);

    /* Nothing this far away can be on a 320x200 screen at any zoom this
     * renderer allows, and culling here is what bounds the multiply below. */
    if (dxb >  (64L << FP) || dxb < -(64L << FP) ||
        dyb >  (64L << FP) || dyb < -(64L << FP))
        return;

    cx = (int)((v->ox[sp->grid] + blocks_to_px(dxb, v->step[sp->grid])) >> FP);
    cy = (int)((v->oy[sp->grid] + blocks_to_px(dyb, v->stepy[sp->grid])) >> FP);

    /* TWO SCALES, NOT ONE. The 5/6 squash is a property of the screen, so it
     * applies to everything drawn on it - a pedestrian included. Scaling a
     * sprite isotropically in a squashed world makes him 20% too tall, which
     * on a 12-pixel man is a pixel and a half and reads as him floating. */
    sstepx = v->step[sp->grid]  >> 6;             /* 16.16 screen px per src px */
    sstepy = v->stepy[sp->grid] >> 6;
    if (sstepx <= 0 || sstepy <= 0) return;
    iscx = ((1L << 30) / sstepx) << 2;            /* 16.16 src px per screen px */
    iscy = ((1L << 30) / sstepy) << 2;

    cs = gta_cos(sp->angle);
    sn = gta_sin(sp->angle);
    acs = cs < 0 ? -cs : cs;
    asn = sn < 0 ? -sn : sn;

    /* The rotated bounding box, in source pixels, then in screen pixels. The
     * +2 is slop for the rounding at both ends; drawing two blank columns costs
     * nothing and clipping the sprite's own edge would be visible. */
    aw = (acs * sw + asn * sh) >> 14;
    ah = (asn * sw + acs * sh) >> 14;
    dw = (int)(((long)aw * sstepx) >> FP) + 2;
    dh = (int)(((long)ah * sstepy) >> FP) + 2;
    if (dw <= 0 || dh <= 0) return;

    x0 = cx - dw / 2;
    y0 = cy - dh / 2;
    x1 = x0 + dw;
    y1 = y0 + dh;
    if (x1 <= 0 || y1 <= 0 || x0 >= v->dst_w || y0 >= v->dst_h)
        return;

    /* Inverse rotation, in source pixels per screen pixel. 14 = 6 + 8, so the
     * shift pair below is a Q14 multiply that never leaves 32 bits: isc is at
     * most about 300 000 and the cosine at most 16 384. */
    du_dx = ((long)cs  * (iscx >> 6)) >> 8;
    du_dy = ((long)sn  * (iscy >> 6)) >> 8;
    dv_dx = ((long)-sn * (iscx >> 6)) >> 8;
    dv_dy = ((long)cs  * (iscy >> 6)) >> 8;

    /* Source coordinate at the top-left of the destination box, expressed from
     * the sprite's centre outward so that rotation is about the centre - which
     * is what GTA means by a sprite's position. */
    {
        long ox = (long)(x0 - cx) << FP;
        long oy = (long)(y0 - cy) << FP;
        u_row = ((long)sw << (FP - 1)) + (((ox >> 8) * (du_dx >> 8))
                                       + ((oy >> 8) * (du_dy >> 8)));
        v_row = ((long)sh << (FP - 1)) + (((ox >> 8) * (dv_dx >> 8))
                                       + ((oy >> 8) * (dv_dy >> 8)));
    }

    /* Clip by stepping the start point instead of testing inside the loop. */
    if (x0 < 0) { u_row += du_dx * (long)(-x0); v_row += dv_dx * (long)(-x0); x0 = 0; }
    if (y0 < 0) { u_row += du_dy * (long)(-y0); v_row += dv_dy * (long)(-y0); y0 = 0; }
    if (x1 > v->dst_w) x1 = v->dst_w;
    if (y1 > v->dst_h) y1 = v->dst_h;

    for (y = y0; y < y1; y++) {
        long u = u_row, uv = v_row;
        unsigned char *d = v->dst + (long)y * v->dst_pitch + x0;
        for (x = x0; x < x1; x++) {
            int su = (int)(u >> FP);
            int sv = (int)(uv >> FP);
            /* The bounding box is the rotated rectangle's bounding box, so its
             * corners map outside the sprite. Unsigned compare does both ends
             * of the range in one test. */
            if ((unsigned)su < (unsigned)sw && (unsigned)sv < (unsigned)sh) {
                unsigned char px = src[(long)sv * sw + su];
                /* Index 0 is transparent; everything else goes through the
                 * remap table if this sprite has one. */
                if (px) *d = rmp ? rmp[px] : px;
            }
            u  += du_dx;
            uv += dv_dx;
            d++;
        }
        u_row += du_dy;
        v_row += dv_dy;
    }
    v->sprites_drawn++;
}

static void draw_cell(gta_view *v, int cbx, int cby, int dbx, int dby, int z,
                      int cache, int stride, int R)
{
    int bx = cbx + dbx, by = cby + dby;
    int h, top = 0;
    gta_block b;

    /* The layer-0 pass sees every column first, so it is the one that fills
     * the height cache; the five passes above it only read. `cache` is off
     * when the target is too wide for the table, and then this behaves exactly
     * as it did before. */
    if (z == 0 || !cache) {
        if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM)
            h = 0;
        else
            h = gta_map_column_height(v->map, bx, by);
        if (h > GTA_MAP_LAYERS)
            h = GTA_MAP_LAYERS;
        if (cache)
            v->col_h[(dby + R) * stride + (dbx + R)] = (unsigned char)h;

        /* FLAT 2D: find the topmost OPAQUE lid, so the layers underneath can
         * be skipped entirely. In flat 2D every lid of a column projects onto
         * the same rectangle, and a non-flat lid is drawn opaque and covers
         * all of it - so nothing below it can ever be seen. At block (90,70)
         * that turned 273 lids over ~88 on-screen cells into roughly one each.
         *
         * "Opaque" is exactly "has a lid face and is not a flat block": a flat
         * block's lid is a decal drawn masked (see draw_block), so it does NOT
         * hide what is under it and cannot be the floor of the search.
         *
         * Never do this in 2.5D. There a lower lid is genuinely visible,
         * displaced inward from the one above, and skipping it would punch a
         * hole in every building in the city. */
        top = 0;
        if (v->flat_2d && cache && h > 0) {
            int zz;
            gta_block probe;
            for (zz = h - 1; zz > 0; zz--) {
                if (!gta_map_block(v->map, bx, by, zz, &probe))
                    continue;
                if (probe.faces[GTA_FACE_LID] && !gta_block_is_flat(&probe)) {
                    top = zz;
                    break;
                }
            }
            v->col_top[(dby + R) * stride + (dbx + R)] = (unsigned char)top;
        }
    } else {
        h = v->col_h[(dby + R) * stride + (dbx + R)];
        top = v->flat_2d ? v->col_top[(dby + R) * stride + (dbx + R)] : 0;
    }

    if (z >= h || z < top)
        return;
    if (!gta_map_block(v->map, bx, by, z, &b))
        return;

    v->columns_visited++;
    draw_block(v, &b,
               v->ox[z]     + (long)dbx * v->step[z],
               v->ox[z + 1] + (long)dbx * v->step[z + 1],
               v->oy[z]     + (long)dby * v->stepy[z],
               v->oy[z + 1] + (long)dby * v->stepy[z + 1],
               v->step[z],  v->step[z + 1],
               v->stepy[z], v->stepy[z + 1], dbx, dby, z);
}

/* ------------------------------------------------ the reservation overlay */

void gta_render_set_overlay(gta_view *v, const void *tr, int on)
{
    v->ov_tr = tr;
    v->ov_on = on;
}

static unsigned char ov_pal[21];
static int ov_pal_done = 0;

/* THE TWENTY MOST MUTUALLY DISTINCT COLOURS THE PALETTE HAS. A preset
 * hue list failed twice: first the nearest-match folded three hues onto
 * one index, then the index-dedupe still picked entries that LOOK the
 * same (yellow, gold and olive are three indices and one colour to the
 * eye - "od razu byly 2 auta zolte"). Farthest-point sampling instead:
 * start from black, and twenty times take the palette entry whose
 * nearest already-chosen colour is farthest away. What that yields is,
 * by construction, the most spread-out set the palette can offer. Very
 * dark entries are skipped for the car slots so every colour reads
 * against the road. */
static void ov_palette(const gta_tiles *t)
{
    const unsigned char *pal = t->palette;
    int k, i;

    /* slot 0: true black, for the free-square grid */
    {
        long bd = 0x7fffffffL;
        int best = 0;
        for (i = 0; i < 256; i++) {
            long r = pal[i * 3], g = pal[i * 3 + 1], b = pal[i * 3 + 2];
            long d = r * r + g * g + b * b;
            if (d < bd) {
                bd = d;
                best = i;
            }
        }
        ov_pal[0] = (unsigned char)best;
    }
    for (k = 1; k < 21; k++) {
        long bestscore = -1;
        int best = -1;
        for (i = 0; i < 256; i++) {
            long r = pal[i * 3], g = pal[i * 3 + 1], b = pal[i * 3 + 2];
            long near = 0x7fffffffL;
            int q, dup = 0;
            if (r + g + b < 140)
                continue;               /* too dark to read on tarmac */
            for (q = 0; q < k; q++) {
                long dr = r - pal[ov_pal[q] * 3];
                long dg = g - pal[ov_pal[q] * 3 + 1];
                long db = b - pal[ov_pal[q] * 3 + 2];
                long d = dr * dr + dg * dg + db * db;
                if (ov_pal[q] == (unsigned char)i)
                    dup = 1;
                if (d < near)
                    near = d;
            }
            if (dup)
                continue;
            if (near > bestscore) {
                bestscore = near;
                best = i;
            }
        }
        ov_pal[k] = (unsigned char)(best < 0 ? 255 : best);
    }
    ov_pal_done = 1;
}

/* One square's checkerboard: every second pixel, phase alternating per
 * row, the other pixels keeping whatever the ground pass drew. */
static void ov_square(gta_view *v, int gl, int cbx, int cby,
                      int bx, int by, unsigned char col)
{
    long sx0 = v->ox[gl] + (long)(bx - cbx) * v->step[gl];
    long sy0 = v->oy[gl] + (long)(by - cby) * v->stepy[gl];
    int x0 = (int)(sx0 >> 16), y0 = (int)(sy0 >> 16);
    int x1 = (int)((sx0 + v->step[gl]) >> 16);
    int y1 = (int)((sy0 + v->stepy[gl]) >> 16);
    int x, y;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > v->dst_w) x1 = v->dst_w;
    if (y1 > v->dst_h) y1 = v->dst_h;
    for (y = y0; y < y1; y++) {
        unsigned char *row = v->dst + (long)y * v->dst_pitch;
        for (x = x0 + ((x0 ^ y) & 1); x < x1; x += 2)
            row[x] = col;
    }
}

/* Everything the developer asked to see, for the ground that pass `z`
 * stands on (block layer z-1, lid plane z): the black grid on free
 * junction squares, every live booking in its owner's colour, and each
 * committed car's start square. Runs before the pass's sprites, so the
 * cars are drawn on top. */
static unsigned char ov_car_col(const gta_traffic *tr, unsigned long serial)
{
    int i;

    for (i = 0; i < tr->n; i++)
        if (!tr->cars[i].done && tr->cars[i].serial == serial)
            return (unsigned char)(1 + tr->cars[i].ov_col);
    return (unsigned char)(1 + (int)(serial % 20UL));
}

static void ov_pass(gta_view *v, int z, int cbx, int cby)
{
    const gta_traffic *tr = (const gta_traffic *)v->ov_tr;
    /* the squares of layer z, projected at grid z: the plane of the road
     * the cars stand on, not one level above it ("obniz go") */
    int lay = z;
    int rx, ry, dbx, dby, i;

    if (!tr || !v->ov_on || lay < 0)
        return;
    if (!ov_pal_done)
        ov_palette(v->tiles);
    if ((v->step[z] >> 16) <= 0 || (v->stepy[z] >> 16) <= 0)
        return;
    rx = (int)(v->dst_w / (v->step[z] >> 16)) / 2 + 2;
    ry = (int)(v->dst_h / (v->stepy[z] >> 16)) / 2 + 2;

    for (dby = -ry; dby <= ry; dby++)
        for (dbx = -rx; dbx <= rx; dbx++) {
            int bx = cbx + dbx, by = cby + dby;
            if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM)
                continue;
            if (gta_traffic_is_junction(v->map, bx, by, lay))
                ov_square(v, z, cbx, cby, bx, by, ov_pal[0]);
        }
    for (i = 0; i < GTA_CLAIM_MAX; i++) {
        if (tr->claim_ttl[i] <= 0 || (int)tr->claim_z[i] != lay)
            continue;
        ov_square(v, z, cbx, cby, tr->claim_x[i], tr->claim_y[i],
                  ov_pal[ov_car_col(tr, tr->claim_car[i])]);
    }
    for (i = 0; i < tr->n; i++) {
        const gta_car *c = &tr->cars[i];
        if (c->done || !c->crossing || c->layer != lay)
            continue;
        ov_square(v, z, cbx, cby, c->book_ax, c->book_ay,
                  ov_pal[1 + c->ov_col]);
    }
}

void gta_render_frame(gta_view *v)
{
    int cbx, cby, g, ring, i, z, R, rx, ry, cache, stride;
    long fx, fy, cx16, cy16;

    if (!v->dst || !v->map || !v->tiles)
        return;

    v->lids_drawn = v->walls_drawn = v->columns_visited = 0;
    v->sprites_drawn = 0;

    /* Split the camera into "which block" and "how far into it". The fraction
     * is in 16.16 BLOCK units, which is what the per-level offset wants. */
    cbx = (int)(v->cam_x >> (FP + 5));
    cby = (int)(v->cam_y >> (FP + 5));
    fx  = (v->cam_x >> 5) & 0xFFFF;
    fy  = (v->cam_y >> 5) & 0xFFFF;

    cx16 = (long)(v->dst_w / 2) << FP;
    cy16 = (long)(v->dst_h / 2) << FP;

    for (g = 0; g < GTA_GRID_LEVELS; g++) {
        long s  = level_step(v->zoom_px, g, v->flat_2d, v->cam_h);
        long sy = level_step_y(v->zoom_px, g, v->flat_2d, v->cam_h);
        v->step[g]  = s;
        v->stepy[g] = sy;
        /* (fx >> 8) is at most 255 and s at most about 3.1M, so this stays
         * inside 32 bits without a 64-bit multiply. The cost is an eighth of a
         * pixel of camera precision. */
        v->ox[g] = cx16 - (((fx >> 8) * s)  >> 8);
        v->oy[g] = cy16 - (((fy >> 8) * sy) >> 8);
    }

    /* The cache is keyed on the tile SIZES, and the projection changes those
     * as surely as the zoom does - flat 2D makes every level the same size.
     * Forgetting this half of the key would serve a level's tile at another
     * level's size, which is a wrong picture rather than a slow one. */
    if (v->lc_zoom != v->zoom_px || v->lc_flat != v->flat_2d ||
        v->lc_cam_h != v->cam_h)
        lidcache_reset(v);

    /* CLEAR THE TARGET RECTANGLE, NOT THE BUFFER.
     *
     * This used to be one memset of dst_pitch * dst_h, which is right only
     * while the target starts at the beginning of a row and fills it. A narrow
     * render mode does neither: it hands over dst = buffer + 27 with a pitch
     * of 320, so the old clear ran 27 bytes off the end of the allocation
     * every frame. At half resolution those 27 bytes landed in the statics
     * behind the 160x100 low buffer and zeroed the mode variables themselves,
     * which is why F4 looked dead at half resolution - the mode was being set
     * and then wiped by the very next clear.
     *
     * The full-width case is still a single memset, so nothing pays for this. */
    if (v->dst_pitch == v->dst_w) {
        memset(v->dst, v->clear_index, (size_t)v->dst_pitch * v->dst_h);
    } else {
        int cy;
        for (cy = 0; cy < v->dst_h; cy++)
            memset(v->dst + (long)cy * v->dst_pitch, v->clear_index,
                   (size_t)v->dst_w);
    }

    /* How far out to look. Grid 0 has the smallest step, so it is the level
     * that can still put a block on screen from the greatest distance; every
     * higher level pushes blocks further out, never closer. */
    rx = (int)(((long)v->dst_w << (FP - 1)) / v->step[0]) + 2;
    /* stepy, not step: the squash makes a block 20% shorter on screen, so 20%
     * more rings of city are reachable down the frame than across it. Sizing
     * ry from the x pitch would cut a row off the top and bottom of every
     * frame - and it would do it silently, because the missing row is
     * off-screen geometry that simply never gets drawn. */
    ry = (int)(((long)v->dst_h << (FP - 1)) / v->stepy[0]) + 2;
    R = rx > ry ? rx : ry;

    /* DRAW ORDER: LAYER FIRST, THEN THE SHRINKING CIRCLE INSIDE IT.
     *
     * The camera looks straight down, so the thing nearest it wins, and
     * "nearest" means highest. Layer therefore beats distance, and the layer
     * loop has to be the OUTER one.
     *
     * It used to be the inner one - a whole column drawn bottom to top, columns
     * ordered outermost first - and that is wrong whenever two columns hold
     * geometry at different heights. The bridges over the water showed it
     * plainly: the deck is the lid of layer 2 and the river is the lid of layer
     * 0, and a river tile one ring further out was drawn AFTER the deck tile
     * beside it, so water appeared in patches on top of the bridge. It looked
     * exactly like holes in the deck, which is why it was hunted as one; it was
     * proved to be the water by repainting tile 41 in the tile set and watching
     * every patch change colour with it.
     *
     * Within a layer the shrinking circle still matters, but only for walls
     * against lids: two lids on one grid level can never overlap, because a
     * level projects to a regular grid. Outermost ring first, inward last. */
    cache = (R <= GTA_MAX_RINGS);
    stride = 2 * R + 1;


    /* THE RING IS RECTANGULAR, NOT SQUARE.
     *
     * R is max(rx, ry) because it sizes the cache's square index, but the
     * VISIBLE region is rx by ry, and on a 320x200 frame those are 7 and 5. A
     * square walk visits 15x15 cells where 15x11 are reachable: the top and
     * bottom bands are entirely off-screen and every cell in them was still
     * looked up in the map, decoded, and handed to draw_block to be clipped
     * away. Measured before this clip went in: 320x200 and 320x100 both
     * visited 809 columns - halving the HEIGHT changed the traversal by
     * nothing at all, which is precisely the mode a slow machine wants.
     *
     * A cell outside the rectangle is off-screen at EVERY level, not just at
     * grid 0: higher levels have a larger step and push a block further from
     * the centre, never closer. That is the same fact rx and ry are derived
     * from, used once more.
     *
     * The order of the cells that ARE drawn is untouched - outermost ring
     * first, all four edges, inward - so the draw order the bridges depend on
     * is exactly what it was. */
    for (z = 0; z < GTA_MAP_LAYERS; z++) {
        /* SPRITES GO IN BEFORE THEIR OWN LAYER, NOT AFTER IT.
         *
         * This used to be at the bottom of the loop, which reads as the
         * obvious choice - "draw layer z, then what stands on it" - and it is
         * off by one level. A block's LID is drawn at grid z+1, the top of its
         * own cube (see draw_block); so the lid belonging to layer z is above
         * the head of anything standing ON layer z. Drawing the sprite after
         * layer z therefore painted it over its own ceiling: canopies,
         * walkways and the pipes that cross the roads all ended up UNDER the
         * player, who appeared to be walking on top of them.
         *
         * What the sprite must be after is the surface it stands on, and that
         * is the lid of layer z-1, drawn on the previous pass. So the correct
         * point is here, at the START of layer z. Anything at or above his own
         * height - his ceiling, the bridge over the street, every layer above -
         * is then drawn after him and covers him, which is what the layer
         * field was always for.
         *
         * A block found with `gtadump overhead` pins it down: (233,4) is a
         * ROAD block on layer 2 that also carries lid 131, so a man standing
         * there has something directly over his head, and
         * `tools/scripts/walk_overhead.txt` parks him under it. Leg 1 of that
         * script walks him back out into the open, which is the control - the
         * case this change must not break. */
        ov_pass(v, z, cbx, cby);

        for (i = 0; i < v->n_sprites; i++) {
            if (v->sprites[i].layer == z)
                gta_render_sprite(v, &v->sprites[i]);
        }

        for (ring = R; ring > 0; ring--) {
            int lo = -ring, hi = ring;
            if (lo < -rx) lo = -rx;
            if (hi >  rx) hi =  rx;

            if (ring <= ry) {
                for (i = lo; i <= hi; i++) {
                    draw_cell(v, cbx, cby, i, -ring, z, cache, stride, R);
                    draw_cell(v, cbx, cby, i,  ring, z, cache, stride, R);
                }
            }
            if (ring <= rx) {
                int loy = -ring + 1, hiy = ring - 1;
                if (loy < -ry) loy = -ry;
                if (hiy >  ry) hiy =  ry;
                for (i = loy; i <= hiy; i++) {
                    draw_cell(v, cbx, cby, -ring, i, z, cache, stride, R);
                    draw_cell(v, cbx, cby,  ring, i, z, cache, stride, R);
                }
            }
        }
        draw_cell(v, cbx, cby, 0, 0, z, cache, stride, R);
    }

    /* A sprite whose layer is above the map's own is still drawn - last, on
     * top of everything - rather than silently dropped. Getting no picture at
     * all from a bad layer number is the kind of failure that reads as "the
     * sprite code does not work". */
    for (i = 0; i < v->n_sprites; i++) {
        if (v->sprites[i].layer >= GTA_MAP_LAYERS || v->sprites[i].layer < 0)
            gta_render_sprite(v, &v->sprites[i]);
    }

    v->n_sprites = 0;
}
